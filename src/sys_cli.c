/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sys_cli.c
 * @brief Wired service console (UART REPL) built on ESP-IDF esp_console.
 *
 *        All commands and subcommands live in one table (s_commands[]).
 *        A single dispatcher routes "<cmd> <sub> args..." to the handler,
 *        and the same table generates every piece of help output:
 *
 *          help                    summary of all commands
 *          help <cmd>              command + all its subcommands
 *          help <cmd> <sub>        one subcommand in detail
 *          <cmd> -h | <cmd> <sub> -h   same as above
 *          tree [cmd]              full command tree
 *
 *        Adding a command = write a handler + add a row to the table.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <ctype.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "nvs.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "cJSON.h"

#include "config.h"
#include "sys_cli.h"
#include "sys_nvs.h"
#include "sys_wifi.h"
#include "sensor_registry.h"
#include "hal_i2c.h"

static const char *TAG = "SYS_CLI";

#define CLI_PROMPT            "node> "
#define CLI_MAX_CMDLINE_LEN   400     /* fits a 255-char webhook URL + command */
#define CLI_TASK_STACK_BYTES  6144    /* cJSON + sensor drivers + printf       */
#define CLI_VALUE_BUF_LEN     (SYS_WEBHOOK_URL_MAX_LEN + 1)
#define CLI_HELP_COL          30      /* column where descriptions start       */

/** Handler return codes. */
#define CLI_OK      0
#define CLI_EUSAGE  2   /* bad arguments → dispatcher prints the usage line */

/* ── Console state ───────────────────────────────────────────────────────── */

static sys_config_t      s_cfg;          /* working copy edited by commands  */
static bool              s_dirty;        /* RAM differs from NVS             */
static SemaphoreHandle_t s_exit_sem;     /* given by `exit` → unblocks run() */

/* ══════════════════════════════════════════════════════════════════════════
 * Command table types
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Handler signature.  argv[0] is the (sub)command name itself, so for
 * "config set ssid foo" a subcommand handler sees {"set","ssid","foo"}.
 */
typedef int (*cli_fn_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *args;      /* usage after the name, e.g. "<key> <value>" */
    const char *desc;      /* one line, shown in lists and the tree      */
    const char *details;   /* optional extra text for 'help <cmd> <sub>' */
    cli_fn_t    fn;
} cli_sub_t;

typedef struct {
    const char      *name;
    const char      *args;     /* only for commands without subcommands */
    const char      *desc;
    const cli_sub_t *subs;     /* NULL = plain command handled by fn    */
    size_t           nsubs;
    cli_fn_t         fn;
    void           (*extra_help)(void);  /* optional, appended to 'help <cmd>' */
} cli_cmd_t;


/* ══════════════════════════════════════════════════════════════════════════
 * Config field table
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum { F_STR, F_U8, F_U16, F_I16, F_U32, F_BOOL } field_type_t;

typedef struct {
    const char  *key;
    field_type_t type;
    size_t       offset;
    size_t       size;
    bool         secret;
    int64_t      min;
    int64_t      max;
    const char  *desc;
} cfg_field_t;

#define MEMBER_SIZE(m) sizeof(((sys_config_t *)0)->m)
#define STR_FIELD(k, m, sec, d) \
    { k, F_STR, offsetof(sys_config_t, m), MEMBER_SIZE(m), sec, 0, 0, d }
#define NUM_FIELD(k, t, m, lo, hi, d) \
    { k, t, offsetof(sys_config_t, m), MEMBER_SIZE(m), false, lo, hi, d }

static const cfg_field_t s_fields[] = {
    STR_FIELD("ssid",        wifi_ssid,           false, "Wi-Fi network name"),
    STR_FIELD("pass",        wifi_pass,           true,  "Wi-Fi password"),
    STR_FIELD("tg_token",    tg_token,            true,  "Telegram bot token"),
    STR_FIELD("tg_chat",     tg_chat_id,          false, "Telegram chat ID"),
    STR_FIELD("discord_url", discord_webhook_url, true,  "Discord webhook URL"),
    STR_FIELD("custom_url",  custom_webhook_url,  false, "Custom webhook URL"),
    STR_FIELD("mqtt_uri",    mqtt_broker_uri,     false, "MQTT broker host/URI (empty = off)"),
    NUM_FIELD("mqtt_port",   F_U16, mqtt_port,    1, 65535, "MQTT port"),
    STR_FIELD("mqtt_user",   mqtt_username,       false, "MQTT username"),
    STR_FIELD("mqtt_pass",   mqtt_password,       true,  "MQTT password"),
    STR_FIELD("mqtt_prefix", mqtt_topic_prefix,   false, "MQTT topic prefix"),
    NUM_FIELD("v_dry",       F_I16, v_dry_mv,     0, 3300, "Soil ADC dry point, mV"),
    NUM_FIELD("v_wet",       F_I16, v_wet_mv,     0, 3300, "Soil ADC wet point, mV"),
    NUM_FIELD("soil_th",     F_U8,  soil_alert_threshold_pct, 0, 100, "Soil alert threshold, %"),
    NUM_FIELD("sensors",     F_U32, active_sensors, 0, 0xFFFFFFFFLL, "Active sensor bitmask"),
    NUM_FIELD("configured",  F_BOOL, is_configured, 0, 1, "1 = normal cycle, 0 = portal on boot"),
};
#define FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

static const cfg_field_t *field_find(const char *key)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (strcasecmp(s_fields[i].key, key) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

static int64_t field_get_num(const cfg_field_t *f)
{
    const uint8_t *p = (const uint8_t *)&s_cfg + f->offset;
    switch (f->type) {
    case F_U8:   return *(const uint8_t  *)p;
    case F_U16:  return *(const uint16_t *)p;
    case F_I16:  return *(const int16_t  *)p;
    case F_U32:  return *(const uint32_t *)p;
    case F_BOOL: return *(const bool     *)p ? 1 : 0;
    default:     return 0;
    }
}

static void field_set_num(const cfg_field_t *f, int64_t v)
{
    uint8_t *p = (uint8_t *)&s_cfg + f->offset;
    switch (f->type) {
    case F_U8:   *(uint8_t  *)p = (uint8_t)v;  break;
    case F_U16:  *(uint16_t *)p = (uint16_t)v; break;
    case F_I16:  *(int16_t  *)p = (int16_t)v;  break;
    case F_U32:  *(uint32_t *)p = (uint32_t)v; break;
    case F_BOOL: *(bool     *)p = (v != 0);    break;
    default: break;
    }
}

static void field_print(const cfg_field_t *f, bool reveal)
{
    const char *p = (const char *)&s_cfg + f->offset;

    printf("  %-12s = ", f->key);
    if (f->type == F_STR) {
        if (p[0] == '\0') {
            printf("(empty)");
        } else if (f->secret && !reveal) {
            printf("******** (set)");
        } else {
            printf("\"%s\"", p);
        }
    } else if (f->type == F_U32) {
        printf("0x%08" PRIX32, (uint32_t)field_get_num(f));
    } else {
        printf("%" PRId64, field_get_num(f));
    }
    printf("\n");
}

/* Joins argv[from..argc-1] with single spaces into buf. */
static bool join_args(int argc, char **argv, int from, char *buf, size_t len)
{
    buf[0] = '\0';
    size_t used = 0;
    for (int i = from; i < argc; i++) {
        int n = snprintf(buf + used, len - used, "%s%s", (i > from) ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= len - used) {
            return false;
        }
        used += (size_t)n;
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

static const char *wakeup_cause_str(esp_sleep_wakeup_cause_t c)
{
    switch (c) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none (not from sleep)";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:      return "UART";
    default:                         return "other";
    }
}

/* Case-insensitive compare that ignores '-', '_' and spaces,
 * so "mhz19", "MH-Z19" and "mh_z19" all match. */
static bool name_matches(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) {
            return *a == *b;
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
}

static const env_sensor_t *sensor_find(const char *name)
{
    for (size_t i = 0; i < g_sensor_registry_count; i++) {
        if (name_matches(g_sensor_registry[i].name, name)) {
            return &g_sensor_registry[i];
        }
    }
    return NULL;
}

static void print_sensor_names(void)
{
    printf("Sensors:");
    for (size_t i = 0; i < g_sensor_registry_count; i++) {
        printf(" %s", g_sensor_registry[i].name);
    }
    printf("\n");
}

static bool is_help_flag(const char *s)
{
    return strcmp(s, "-h") == 0 || strcmp(s, "--help") == 0 || strcmp(s, "help") == 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Banner (plain ASCII - renders identically on every serial terminal)
 * ══════════════════════════════════════════════════════════════════════════ */

#define A_RST  "\x1b[0m"
#define A_DIM  "\x1b[2m"
#define A_BOLD "\x1b[1m"
#define A_CYAN "\x1b[36m"
#define A_GRN  "\x1b[32m"
#define A_YEL  "\x1b[33m"
#define A_ORNG "\x1b[1;38;5;208m"

#define BANNER_RULE A_DIM "  ------------------------------------------------------------" A_RST "\n"

static void banner_kv(const char *key, const char *vcol, const char *value)
{
    printf("  " A_DIM "%-10s" A_RST "%s%s" A_RST "\n", key, vcol, value);
}

static void print_banner(void)
{
    /* Logo by d7main, cyan -> blue gradient */
    static const char *const logo[] = {
        "    /////////    ////     ///  ///       //",
        "   ///          /// //   ///   ///     //",
        "  ////////     ///  //  ///    ///   //",
        " ///          ///   // ///     /// //",
        "///////////  ///    /////      ///",
    };
    static const char *const grad[] = {
        "\x1b[1;38;5;51m", "\x1b[1;38;5;45m", "\x1b[1;38;5;39m",
        "\x1b[1;38;5;33m", "\x1b[1;38;5;27m",
    };

    int active = 0;
    char bar[96] = "";
    size_t n = 0;
    for (size_t i = 0; i < g_sensor_registry_count && n + 24 < sizeof(bar); i++) {
        const bool on = (s_cfg.active_sensors & g_sensor_registry[i].sensor_bit) != 0;
        if (on) active++;
        n += snprintf(bar + n, sizeof(bar) - n, "%s%c" A_RST, on ? A_GRN : A_DIM, on ? '#' : '-');
    }

    char sens[160];
    snprintf(sens, sizeof(sens), "%d/%u active  " A_DIM "[" A_RST "%s" A_DIM "]" A_RST,
             active, (unsigned)g_sensor_registry_count, bar);

    char state[48];
    snprintf(state, sizeof(state), "%s%s",
             s_cfg.is_configured ? "configured" : "NOT configured",
             s_dirty ? " (unsaved changes)" : "");

    printf("\n");
    for (size_t i = 0; i < sizeof(logo) / sizeof(logo[0]); i++) {
        printf("  %s%s" A_RST "\n", grad[i], logo[i]);
    }
    printf("\n  " A_BOLD "ESP32 Environmental Node" A_RST "  " A_CYAN "v" FW_VERSION A_RST
           "   " A_DIM "by" A_RST " d" A_ORNG "7" A_RST "main\n");
    printf("  " A_DIM "Low-power multi-sensor firmware with a built-in service console" A_RST "\n\n");

    printf(BANNER_RULE);
    banner_kv("author",   A_BOLD, "d7main");
    banner_kv("source",   A_CYAN, "github.com/d7main/ESP32_EnvironmentalNode");
    banner_kv("chip",     A_CYAN, CHIP_NAME);
    banner_kv("firmware", A_CYAN, "v" FW_VERSION);
    banner_kv("state",    s_cfg.is_configured ? A_GRN : A_YEL, state);
    banner_kv("sensors",  "", sens);
    printf(BANNER_RULE);

    printf("  " A_CYAN "help" A_RST "      list all commands\n");
    printf("  " A_CYAN "config" A_RST "    view and edit settings\n");
    printf("  " A_CYAN "wifi" A_RST "      start Wi-Fi portal manually\n");
    printf("  " A_CYAN "exit" A_RST "      leave console and continue boot\n\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Command: info
 * ══════════════════════════════════════════════════════════════════════════ */

static int cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const esp_app_desc_t *app = esp_app_get_description();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    int64_t up_s = esp_timer_get_time() / 1000000;

    printf("Device     : %s (rev %d.%d, %d core%s)\n", CHIP_NAME,
           chip.revision / 100, chip.revision % 100,
           chip.cores, chip.cores > 1 ? "s" : "");
    printf("MAC (STA)  : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("Flash      : %" PRIu32 " KB\n", flash_size / 1024);
    printf("Firmware   : %s %s (built %s %s)\n",
           app->project_name, app->version, app->date, app->time);
    printf("ESP-IDF    : %s\n", esp_get_idf_version());
    printf("Heap       : free %" PRIu32 " B, min-ever %" PRIu32 " B\n",
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    printf("Uptime     : %" PRId64 " s\n", up_s);
    printf("Reset      : %s\n", reset_reason_str(esp_reset_reason()));
    printf("Wake cause : %s\n", wakeup_cause_str(esp_sleep_get_wakeup_cause()));
    printf("Sleep      : %d s between cycles\n", DEFAULT_SLEEP_SEC);
    printf("Config     : %s%s\n", s_cfg.is_configured ? "configured" : "NOT configured",
           s_dirty ? " (unsaved changes)" : "");
    return CLI_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Command: config
 * ══════════════════════════════════════════════════════════════════════════ */

static void config_print_keys(void)
{
    printf("\nKeys:\n");
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const cfg_field_t *f = &s_fields[i];
        if (f->type == F_STR) {
            printf("  %-12s %s (max %u chars)%s\n", f->key, f->desc,
                   (unsigned)(f->size - 1), f->secret ? " [secret]" : "");
        } else {
            printf("  %-12s %s [%" PRId64 "..%" PRId64 "]\n", f->key, f->desc, f->min, f->max);
        }
    }
}

static int config_show(int argc, char **argv)
{
    bool reveal = (argc >= 2 && strcmp(argv[1], "--reveal") == 0);
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        field_print(&s_fields[i], reveal);
    }
    if (s_dirty) {
        printf("(unsaved changes - run 'config save')\n");
    }
    return CLI_OK;
}

static int config_set(int argc, char **argv)
{
    if (argc < 3) {
        return CLI_EUSAGE;
    }
    const cfg_field_t *f = field_find(argv[1]);
    if (!f) {
        printf("Unknown key '%s'. Run 'help config' for the list.\n", argv[1]);
        return CLI_OK;
    }

    static char value[CLI_VALUE_BUF_LEN];
    if (!join_args(argc, argv, 2, value, sizeof(value))) {
        printf("Value too long.\n");
        return CLI_OK;
    }

    if (f->type == F_STR) {
        size_t len = strlen(value);
        if (len >= f->size) {
            printf("Value too long: %u chars, max %u.\n", (unsigned)len, (unsigned)(f->size - 1));
            return CLI_OK;
        }
        memcpy((char *)&s_cfg + f->offset, value, len + 1);
    } else {
        char *end = NULL;
        long long v = strtoll(value, &end, 0);   /* base 0: accepts 0x.. */
        if (end == value || *end != '\0') {
            printf("'%s' is not a number.\n", value);
            return CLI_OK;
        }
        if (v < f->min || v > f->max) {
            printf("Out of range: allowed %" PRId64 "..%" PRId64 ".\n", f->min, f->max);
            return CLI_OK;
        }
        field_set_num(f, v);
    }

    s_dirty = true;
    field_print(f, false);
    return CLI_OK;
}

static int config_clear(int argc, char **argv)
{
    if (argc != 2) {
        return CLI_EUSAGE;
    }
    const cfg_field_t *f = field_find(argv[1]);
    if (!f) {
        printf("Unknown key '%s'. Run 'help config' for the list.\n", argv[1]);
        return CLI_OK;
    }
    if (f->type != F_STR) {
        printf("'%s' is numeric - use 'config set %s <value>'.\n", f->key, f->key);
        return CLI_OK;
    }
    ((char *)&s_cfg + f->offset)[0] = '\0';
    s_dirty = true;
    field_print(f, false);
    return CLI_OK;
}

static int config_save(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!sys_nvs_save_config(&s_cfg)) {
        printf("Save FAILED - see log above. Stored config unchanged.\n");
        return CLI_OK;
    }
    s_dirty = false;
    printf("Saved to NVS.\n");
    if (!s_cfg.is_configured && s_cfg.wifi_ssid[0]) {
        printf("Hint: 'configured' is 0 - the device will open the portal on boot.\n"
               "      Run 'config set configured 1' and save again to start the normal cycle.\n");
    }
    return CLI_OK;
}

static int config_reload(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!sys_nvs_load_config(&s_cfg)) {
        printf("Load failed - working copy reset to empty config.\n");
    } else {
        printf("Reloaded from NVS.\n");
    }
    s_dirty = false;
    return CLI_OK;
}

static int config_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("This erases ALL stored settings and reboots.\n");
        return CLI_EUSAGE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("NVS erase failed: %s\n", esp_err_to_name(err));
        return CLI_OK;
    }

    printf("Config erased. Rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return CLI_OK;
}

static const cli_sub_t s_config_subs[] = {
    { "show",   "[--reveal]",      "Print working config (secrets masked)",
      "  --reveal  also print passwords and tokens in clear text\n", config_show },
    { "set",    "<key> <value>",   "Change a field in RAM",
      "  Values may contain spaces; quotes are optional.\n"
      "  Numbers accept decimal or hex (0x..).\n"
      "  Example: config set ssid My Home WiFi\n", config_set },
    { "clear",  "<key>",           "Empty a string field", NULL, config_clear },
    { "save",   "",                "Write working config to NVS", NULL, config_save },
    { "reload", "",                "Discard RAM changes, reload from NVS", NULL, config_reload },
    { "reset",  "yes",             "Erase stored config and reboot", NULL, config_reset },
};

/* ══════════════════════════════════════════════════════════════════════════
 * Command: sensor
 * ══════════════════════════════════════════════════════════════════════════ */

static void sensor_read_one(const env_sensor_t *s)
{
    printf("[%s] ", s->name);
    fflush(stdout);

    esp_err_t err = s->init();
    if (err != ESP_OK) {
        printf("init FAILED: %s\n", esp_err_to_name(err));
        s->deinit();   /* contract: safe even after a failed init */
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        printf("out of memory\n");
        s->deinit();
        return;
    }

    err = s->read_data(root);
    if (err != ESP_OK) {
        printf("read FAILED: %s\n", esp_err_to_name(err));
    } else {
        char *json = cJSON_PrintUnformatted(root);
        printf("%s\n", json ? json : "(serialisation failed)");
        free(json);
    }

    cJSON_Delete(root);
    s->deinit();
}

static int sensor_list(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("  %-10s %-10s %s\n", "NAME", "BIT", "STATE");
    for (size_t i = 0; i < g_sensor_registry_count; i++) {
        const env_sensor_t *s = &g_sensor_registry[i];
        printf("  %-10s 0x%08" PRIX32 " %s\n", s->name, s->sensor_bit,
               (s_cfg.active_sensors & s->sensor_bit) ? "ON" : "off");
    }
    return CLI_OK;
}

static int sensor_set_enabled(int argc, char **argv)
{
    if (argc != 2) {
        return CLI_EUSAGE;
    }
    const bool on = (strcmp(argv[0], "enable") == 0);

    if (strcasecmp(argv[1], "all") == 0) {
        for (size_t i = 0; i < g_sensor_registry_count; i++) {
            if (on) s_cfg.active_sensors |=  g_sensor_registry[i].sensor_bit;
            else    s_cfg.active_sensors &= ~g_sensor_registry[i].sensor_bit;
        }
    } else {
        const env_sensor_t *s = sensor_find(argv[1]);
        if (!s) {
            printf("Unknown sensor '%s'. ", argv[1]);
            print_sensor_names();
            return CLI_OK;
        }
        if (on) s_cfg.active_sensors |=  s->sensor_bit;
        else    s_cfg.active_sensors &= ~s->sensor_bit;
    }
    s_dirty = true;
    printf("active_sensors = 0x%08" PRIX32 " (run 'config save' to persist)\n",
           s_cfg.active_sensors);
    return CLI_OK;
}

static int sensor_read(int argc, char **argv)
{
    /* Moisture adapter needs calibration bounds from the working copy. */
    sensor_registry_set_config(&s_cfg);

    if (argc >= 2 && strcasecmp(argv[1], "all") != 0) {
        const env_sensor_t *s = sensor_find(argv[1]);
        if (!s) {
            printf("Unknown sensor '%s'. ", argv[1]);
            print_sensor_names();
            return CLI_OK;
        }
        sensor_read_one(s);   /* explicit name: read even if disabled */
        return CLI_OK;
    }

    const bool read_all = (argc >= 2);   /* "all" = every registered sensor */
    int count = 0;
    for (size_t i = 0; i < g_sensor_registry_count; i++) {
        const env_sensor_t *s = &g_sensor_registry[i];
        if (read_all || (s_cfg.active_sensors & s->sensor_bit)) {
            sensor_read_one(s);
            count++;
        }
    }
    if (count == 0) {
        printf("No sensors enabled. Use 'sensor read <name>' or 'sensor read all'.\n");
    }
    return CLI_OK;
}

static const cli_sub_t s_sensor_subs[] = {
    { "list",    "",            "Show all sensors and their ON/off state", NULL, sensor_list },
    { "enable",  "<name|all>",  "Turn a sensor on (RAM, then 'config save')", NULL, sensor_set_enabled },
    { "disable", "<name|all>",  "Turn a sensor off (RAM, then 'config save')", NULL, sensor_set_enabled },
    { "read",    "[name|all]",  "Init, read and deinit; print JSON",
      "  (no arg)  read every ENABLED sensor\n"
      "  <name>    read one sensor, even if disabled\n"
      "  all       read every registered sensor\n", sensor_read },
};

/* ══════════════════════════════════════════════════════════════════════════
 * Command: i2c
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *i2c_hint(uint8_t addr)
{
    switch (addr) {
    case 0x23: case 0x5C: return "BH1750";
    case 0x44: case 0x45: return "SHT3x";
    case 0x76: case 0x77: return "BME280/BMP280";
    default:              return NULL;
    }
}

static int i2c_scan(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* If a sensor already owns the bus, hal_i2c_init() fails (driver
     * already installed) - we then just use the existing driver. */
    bool installed_here = hal_i2c_init();

    printf("Scanning I2C (SDA=GPIO%d, SCL=GPIO%d)...\n", I2C_SDA_PIN, I2C_SCL_PIN);
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(HAL_I2C_PORT_NUM, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);

        if (r == ESP_ERR_INVALID_STATE) {
            printf("I2C driver not available - scan aborted.\n");
            break;
        }
        if (r == ESP_OK) {
            const char *hint = i2c_hint(addr);
            printf("  0x%02X%s%s\n", addr, hint ? "  <- " : "", hint ? hint : "");
            found++;
        }
    }
    printf("%d device(s) found.\n", found);

    if (installed_here) {
        hal_i2c_deinit();
    }
    return CLI_OK;
}

static const cli_sub_t s_i2c_subs[] = {
    { "scan", "", "Probe addresses 0x08..0x77 and list responders", NULL, i2c_scan },
};

static int wifi_portal(int argc, char **argv)
{
    const bool force = (argc >= 2 && strcmp(argv[1], "-f") == 0);
    if (s_dirty && !force) {
        printf("You have unsaved changes. Run 'config save', or 'wifi portal -f' to discard them.\n");
        return CLI_OK;
    }
    printf("Starting Wi-Fi access point + configuration portal...\n");
    printf("  SSID     : %s\n", AP_WIFI_SSID);
    printf("  Password : %s\n", AP_WIFI_PASS);
    printf("  Open     : http://192.168.4.1/\n");
    printf("The portal runs until you save settings (the chip then reboots) or press RESET.\n");
    fflush(stdout);

    /* Blocks for good, exactly like the button-triggered portal in main.c. */
    sys_wifi_start_ap_and_server();
    return CLI_OK;
}

static const cli_sub_t s_wifi_subs[] = {
    { "portal", "[-f]", "Turn on Wi-Fi AP + web config portal manually",
      "Starts the same access point and web page as the 3-second BOOT-button hold.\n"
      "Blocks until settings are saved in the browser (chip reboots) or you reset it.\n"
      "-f discards unsaved console changes.", wifi_portal },
};

/* ══════════════════════════════════════════════════════════════════════════
 * Commands: reboot, exit
 * ══════════════════════════════════════════════════════════════════════════ */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s_dirty) {
        printf("Unsaved changes discarded.\n");
    }
    printf("Rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return CLI_OK;
}

static int cmd_banner(int argc, char **argv)
{
    (void)argc; (void)argv;
    print_banner();
    return CLI_OK;
}

static int cmd_exit(int argc, char **argv)
{
    const bool force = (argc >= 2 && strcmp(argv[1], "-f") == 0);
    if (s_dirty && !force) {
        printf("You have unsaved changes. Run 'config save', or 'exit -f' to discard them.\n");
        return CLI_OK;
    }
    printf("Leaving service console, continuing normal boot...\n");
    fflush(stdout);

    /* Hand control back to app_main, then park the REPL task inside this
     * handler so it never prints another prompt over the normal-cycle logs.
     * The chip either deep-sleeps or reboots afterwards, which reclaims it. */
    xSemaphoreGive(s_exit_sem);
    vTaskSuspend(NULL);
    return CLI_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Command table
 * ══════════════════════════════════════════════════════════════════════════ */

static int cmd_help(int argc, char **argv);
static int cmd_tree(int argc, char **argv);

#define SUBS(arr) arr, (sizeof(arr) / sizeof(arr[0]))

static const cli_cmd_t s_commands[] = {
    { "info",   "",                   "Show chip, firmware, heap and reset info",
      NULL, 0, cmd_info, NULL },
    { "config", NULL,                 "View/edit stored configuration",
      SUBS(s_config_subs), NULL, config_print_keys },
    { "sensor", NULL,                 "List, enable/disable and read sensors",
      SUBS(s_sensor_subs), NULL, print_sensor_names },
    { "i2c",    NULL,                 "I2C bus diagnostics",
      SUBS(s_i2c_subs), NULL, NULL },
    { "wifi",   NULL,                 "Manually turn on Wi-Fi (config portal)",
      SUBS(s_wifi_subs), NULL, NULL },
    { "banner", "",                   "Show the ESP32 EnvironmentalNode logo and status",
      NULL, 0, cmd_banner, NULL },
    { "reboot", "",                   "Restart the chip",
      NULL, 0, cmd_reboot, NULL },
    { "exit",   "[-f]",               "Leave console, continue normal cycle (-f drops unsaved)",
      NULL, 0, cmd_exit, NULL },
    { "help",   "[command [sub]]",    "Show help for all or one command",
      NULL, 0, cmd_help, NULL },
    { "tree",   "[command]",          "Show the command tree",
      NULL, 0, cmd_tree, NULL },
};
static const size_t s_command_count = sizeof(s_commands) / sizeof(s_commands[0]);

/* ══════════════════════════════════════════════════════════════════════════
 * Help & tree
 * ══════════════════════════════════════════════════════════════════════════ */

static const cli_cmd_t *cmd_find(const char *name)
{
    for (size_t i = 0; i < s_command_count; i++) {
        if (strcmp(s_commands[i].name, name) == 0) {
            return &s_commands[i];
        }
    }
    return NULL;
}

static const cli_sub_t *sub_find(const cli_cmd_t *c, const char *name)
{
    for (size_t i = 0; i < c->nsubs; i++) {
        if (strcmp(c->subs[i].name, name) == 0) {
            return &c->subs[i];
        }
    }
    return NULL;
}

/* Prints "<prefix><left>" padded to CLI_HELP_COL, then the description. */
static void print_row(const char *prefix, const char *left, const char *desc)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s", prefix, left);
    printf("%-*s %s\n", CLI_HELP_COL, buf, desc);
}

static void format_usage(char *buf, size_t len, const char *a, const char *b, const char *c)
{
    snprintf(buf, len, "%s%s%s%s%s", a,
             (b && *b) ? " " : "", b ? b : "",
             (c && *c) ? " " : "", c ? c : "");
}

static void print_sub_help(const cli_cmd_t *c, const cli_sub_t *s)
{
    char usage[96];
    format_usage(usage, sizeof(usage), c->name, s->name, s->args);
    printf("Usage: %s\n  %s\n", usage, s->desc);
    if (s->details) {
        printf("%s", s->details);
    }
}

static void print_cmd_help(const cli_cmd_t *c)
{
    char usage[96];
    printf("%s - %s\n\n", c->name, c->desc);

    if (c->nsubs == 0) {
        format_usage(usage, sizeof(usage), c->name, c->args, NULL);
        printf("Usage: %s\n", usage);
    } else {
        printf("Usage:\n");
        for (size_t i = 0; i < c->nsubs; i++) {
            format_usage(usage, sizeof(usage), c->name, c->subs[i].name, c->subs[i].args);
            print_row("  ", usage, c->subs[i].desc);
        }
    }
    if (c->extra_help) {
        c->extra_help();
    }
    if (c->nsubs > 0) {
        printf("\nRun 'help %s <subcommand>' or '%s <subcommand> -h' for details.\n",
               c->name, c->name);
    }
}

static void print_tree_cmd(const cli_cmd_t *c, const char *indent, bool last, bool root)
{
    char left[96];

    if (root) {
        format_usage(left, sizeof(left), c->name, c->args, NULL);
        print_row("", left, c->desc);
    } else {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "%s%s", indent, last ? "`-- " : "|-- ");
        format_usage(left, sizeof(left), c->name, c->nsubs ? NULL : c->args, NULL);
        print_row(prefix, left, c->desc);
    }

    char child_indent[16];
    snprintf(child_indent, sizeof(child_indent), "%s",
             root ? "" : (last ? "    " : "|   "));
    char full_indent[24];
    snprintf(full_indent, sizeof(full_indent), "%s%s", indent, child_indent);

    for (size_t i = 0; i < c->nsubs; i++) {
        const cli_sub_t *s = &c->subs[i];
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "%s%s", full_indent,
                 (i == c->nsubs - 1) ? "`-- " : "|-- ");
        format_usage(left, sizeof(left), s->name, s->args, NULL);
        print_row(prefix, left, s->desc);
    }
}

static int cmd_help(int argc, char **argv)
{
    if (argc == 1) {
        printf("Commands:\n");
        for (size_t i = 0; i < s_command_count; i++) {
            const cli_cmd_t *c = &s_commands[i];
            char left[64];
            format_usage(left, sizeof(left), c->name,
                         c->nsubs ? "<subcommand>" : c->args, NULL);
            print_row("  ", left, c->desc);
        }
        printf("\nRun 'help <command>' for details, 'tree' for the full command tree.\n"
               "Tab completes command names, Up/Down recalls history.\n");
        return CLI_OK;
    }

    const cli_cmd_t *c = cmd_find(argv[1]);
    if (!c) {
        printf("Unknown command '%s'. Run 'help' for the list.\n", argv[1]);
        return CLI_OK;
    }
    if (argc >= 3 && c->nsubs) {
        const cli_sub_t *s = sub_find(c, argv[2]);
        if (!s) {
            printf("'%s' has no subcommand '%s'.\n\n", c->name, argv[2]);
            print_cmd_help(c);
            return CLI_OK;
        }
        print_sub_help(c, s);
        return CLI_OK;
    }
    print_cmd_help(c);
    return CLI_OK;
}

static int cmd_tree(int argc, char **argv)
{
    if (argc >= 2) {
        const cli_cmd_t *c = cmd_find(argv[1]);
        if (!c) {
            printf("Unknown command '%s'. Run 'help' for the list.\n", argv[1]);
            return CLI_OK;
        }
        print_tree_cmd(c, "", true, true);
        return CLI_OK;
    }

    printf("node\n");
    for (size_t i = 0; i < s_command_count; i++) {
        print_tree_cmd(&s_commands[i], "", i == s_command_count - 1, false);
    }
    return CLI_OK;
}

/**
 * Single entry point registered for every top-level command.
 * Always returns 0 — handlers print their own errors, so the REPL does not
 * add its generic "Command returned non-zero error code" line.
 */
static int cli_dispatch(int argc, char **argv)
{
    const cli_cmd_t *c = cmd_find(argv[0]);
    if (!c) {
        return 0;
    }

    /* "<cmd> -h" — but not for 'help' itself, where "help help" is valid. */
    if (argc >= 2 && c->fn != cmd_help && is_help_flag(argv[1])) {
        print_cmd_help(c);
        return 0;
    }

    if (c->nsubs == 0) {
        c->fn(argc, argv);
        return 0;
    }

    if (argc < 2) {
        if (strcmp(c->name, "config") == 0) {
            print_banner();   /* bare 'config' = logo + status + settings help */
        }
        print_cmd_help(c);
        return 0;
    }

    const cli_sub_t *s = sub_find(c, argv[1]);
    if (!s) {
        printf("%s: unknown subcommand '%s'\n\n", c->name, argv[1]);
        print_cmd_help(c);
        return 0;
    }

    if (argc >= 3 && is_help_flag(argv[2])) {
        print_sub_help(c, s);
        return 0;
    }

    if (s->fn(argc - 1, argv + 1) == CLI_EUSAGE) {
        char usage[96];
        format_usage(usage, sizeof(usage), c->name, s->name, s->args);
        printf("Usage: %s\n", usage);
    }
    return 0;
}

static void register_commands(void)
{
    for (size_t i = 0; i < s_command_count; i++) {
        const esp_console_cmd_t cmd = {
            .command = s_commands[i].name,
            .help    = s_commands[i].desc,
            .hint    = NULL,
            .func    = cli_dispatch,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM || \
    CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif

bool sys_cli_wait_for_key(uint32_t timeout_ms)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&usj_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial/JTAG driver busy - service console unavailable.");
        return false;
    }
#else
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "UART driver busy - service console unavailable.");
        return false;
    }
    uart_flush_input(port);
#endif

    bool hit = false;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int64_t next_hint = 0;
    while (esp_timer_get_time() < deadline) {
        /* Repeat the invitation every second: with USB Serial/JTAG the host
         * re-enumerates the port after a reset, so the terminal often attaches
         * only a few seconds after boot and would miss a single message. */
        if (esp_timer_get_time() >= next_hint) {
            printf("\n>>> Press ENTER within %" PRId64 " s to open the service console...\n",
                   (deadline - esp_timer_get_time()) / 1000000 + 1);
            fflush(stdout);
            next_hint = esp_timer_get_time() + 1000000;
        }
        uint8_t ch;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(50));
#else
        int n = uart_read_bytes(port, &ch, 1, pdMS_TO_TICKS(50));
#endif
        if (n == 1 && (ch == '\r' || ch == '\n')) {
            hit = true;
            break;
        }
    }

    /* The REPL installs its own driver instance, so release ours. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_uninstall();
#else
    uart_driver_delete(port);
#endif
    return hit;
}

void sys_cli_run(void)
{
    if (!sys_nvs_load_config(&s_cfg)) {
        ESP_LOGW(TAG, "No stored config - starting from empty defaults.");
    }
    s_dirty = false;

    s_exit_sem = xSemaphoreCreateBinary();
    if (!s_exit_sem) {
        ESP_LOGE(TAG, "Out of memory - console not started.");
        return;
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt             = CLI_PROMPT;
    repl_cfg.max_cmdline_length = CLI_MAX_CMDLINE_LEN;
    repl_cfg.task_stack_size    = CLI_TASK_STACK_BYTES;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_exit_sem);
        return;
    }

    /* Our own 'help' (table-driven) replaces esp_console's built-in one. */
    register_commands();

    print_banner();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
        return;
    }

    xSemaphoreTake(s_exit_sem, portMAX_DELAY);
}

#else  /* console is not on a UART (USB-CDC / USB-Serial-JTAG / none) */

bool sys_cli_wait_for_key(uint32_t timeout_ms)
{
    (void)timeout_ms;
    ESP_LOGW(TAG, "Service console requires a UART console - disabled.");
    return false;
}

void sys_cli_run(void) {}

#endif
