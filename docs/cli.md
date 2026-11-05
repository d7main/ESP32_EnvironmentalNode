# Service Console (CLI)

The service console is a small wired command line for bench work: inspect the chip, edit the stored configuration, test sensors and start the Wi-Fi portal manually. It runs over the same USB/UART port you use for flashing, so no Wi-Fi is needed.

---

## Opening the console

The console is offered **only on a cold boot or RESET**, never on a deep-sleep timer wake-up.

1. Open a serial monitor at **115200 baud** (for example `pio device monitor`).
2. Reset the board. The firmware prints:

    ```text
    >>> Press ENTER within N s to open the service console...
    ```

3. Press **ENTER** inside that window (`CLI_ENTRY_WINDOW_MS`, 10 s by default). The banner appears and you get a prompt.

If you do nothing, the node continues its normal measure → alert → deep-sleep cycle.

!!! tip "Colors look like garbage?"
    The banner uses ANSI colors. Use a monitor that passes escape codes through. In PlatformIO set `monitor_filters = direct` (already the project default). The `colorize` and `printable` filters replace the ESC character with `␛`.

!!! note "Disabling the console"
    Set `CLI_ENABLED` to `0` in `include/config.h` to compile the console out of `app_main`.

---

## Conventions

- Command and key names are **case-insensitive**.
- Numbers accept decimal or hex (`0x..`).
- String values may contain spaces. Quotes are optional.
- `-h`, `--help` or `help` after any command prints its help.
- **Changes live in RAM until you run `config save`.** The banner shows `(unsaved changes)` while edits are pending. `exit` and `wifi portal` refuse to run with unsaved changes unless you add `-f`.

---

## Command reference

| Command | Purpose |
|---|---|
| [`info`](#info) | Chip, firmware, heap, uptime and reset info |
| [`config`](#config) | View and edit the stored configuration |
| [`sensor`](#sensor) | List, enable/disable and read sensors |
| [`i2c`](#i2c) | I²C bus diagnostics |
| [`wifi`](#wifi) | Manually turn on Wi-Fi (configuration portal) |
| [`banner`](#banner) | Reprint the logo and status |
| [`reboot`](#reboot) | Restart the chip |
| [`exit`](#exit) | Leave the console and continue the normal cycle |
| [`help`](#help) | Help for all commands or one command |
| [`tree`](#tree) | Show the command tree |

### info

Prints device model and revision, MAC (STA), flash size, firmware name/version/build time, ESP-IDF version, free and minimum-ever heap, uptime, last reset reason, wake-up cause, sleep interval and configuration state.

### config

Edits the **working copy** of the configuration. Nothing is written to flash until `config save`.

| Subcommand | Description |
|---|---|
| `config` | With no subcommand: banner plus the list of keys |
| `config show [--reveal]` | Print the working config. Secrets are masked unless `--reveal` is given |
| `config set <key> <value>` | Change a field in RAM. Example: `config set ssid My Home WiFi` |
| `config clear <key>` | Empty a string field |
| `config save` | Write the working config to NVS |
| `config reload` | Discard RAM changes and reload from NVS |
| `config reset yes` | Erase the stored config and reboot |

**Keys**

| Key | Type | Range / notes |
|---|---|---|
| `ssid` | string | Wi-Fi network name |
| `pass` | string, secret | Wi-Fi password |
| `tg_token` | string, secret | Telegram bot token |
| `tg_chat` | string | Telegram chat ID |
| `discord_url` | string, secret | Discord webhook URL |
| `custom_url` | string | Custom webhook URL |
| `mqtt_uri` | string | MQTT broker host/URI. Empty = MQTT off |
| `mqtt_port` | u16 | 1 – 65535 |
| `mqtt_user` | string | MQTT username |
| `mqtt_pass` | string, secret | MQTT password |
| `mqtt_prefix` | string | MQTT topic prefix |
| `v_dry` | i16 | Soil ADC dry point, 0 – 3300 mV |
| `v_wet` | i16 | Soil ADC wet point, 0 – 3300 mV |
| `soil_th` | u8 | Soil alert threshold, 0 – 100 % |
| `sensors` | u32 | Active sensor bitmask. Prefer the `sensor` command |
| `configured` | bool | `1` = normal cycle, `0` = portal on next boot |

Maximum string lengths are shown by `config` with no arguments.

### sensor

| Subcommand | Description |
|---|---|
| `sensor list` | Show every sensor, its bit and ON/off state |
| `sensor enable <name\|all>` | Turn a sensor on (RAM, then `config save`) |
| `sensor disable <name\|all>` | Turn a sensor off (RAM, then `config save`) |
| `sensor read` | Init, read and deinit every **enabled** sensor, printing JSON |
| `sensor read <name>` | Read one sensor, even if it is disabled |
| `sensor read all` | Read every registered sensor |

Registered sensors: `BME280`, `SHT3x`, `BH1750`, `MH-Z19`, `PMS5003`, `Moisture`, `DS18B20`.

```text
> sensor read BME280
[BME280] {"temperature":22.4,"humidity":41.2,"pressure":1008.3}
```

An error such as `init FAILED: ESP_ERR_NOT_FOUND` usually means wiring or a wrong I²C address. Use [`i2c scan`](#i2c) to check.

### i2c

| Subcommand | Description |
|---|---|
| `i2c scan` | Probe addresses `0x08`–`0x77` and list responders |

Known addresses are annotated with a hint: `0x23`/`0x5C` BH1750, `0x44`/`0x45` SHT3x, `0x76`/`0x77` BME280/BMP280.

### wifi

| Subcommand | Description |
|---|---|
| `wifi portal [-f]` | Start the access point and web configuration portal by hand |

This is the same portal as the 3-second BOOT-button hold, see [Configuration](configuration.md). The command prints the AP SSID, password and URL, then **blocks**: the portal runs until you save settings in the browser (the chip then reboots) or press RESET. Use `-f` to discard unsaved console changes first.

### banner

Reprints the logo with author, source, chip, firmware, configuration state and a sensor bar (`#` = enabled, `-` = disabled).

### reboot

Restarts the chip. Unsaved changes are discarded.

### exit

Leaves the console and continues the normal measure → alert → deep-sleep cycle. Refuses when there are unsaved changes. Use `exit -f` to drop them.

### help

`help` lists all commands. `help <command>` or `help <command> <sub>` shows usage and details.

### tree

`tree` prints every command with its subcommands. `tree <command>` limits the output to one command.

---

## Typical workflows

**First-time setup over serial**

```text
> config set ssid MyNetwork
> config set pass hunter2
> sensor enable BME280
> sensor enable BH1750
> config set configured 1
> config save
> exit
```

**Checking a new sensor**

```text
> i2c scan
> sensor read SHT3x
```

**Changing the broker**

```text
> config set mqtt_uri broker.local
> config set mqtt_port 1883
> config show
> config save
```

**Starting over**

```text
> config reset yes
```

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `␛[...` or `ÔÉŤ[...` characters in the output | The monitor does not pass ANSI codes. Use `monitor_filters = direct` |
| No prompt after reset | The window passed. Reset again and press ENTER sooner. Deep-sleep wake-ups never offer the console |
| `exit` says "unsaved changes" | Run `config save`, `config reload`, or use `exit -f` |
| `sensor read` prints `init FAILED` | Check wiring and power, then run `i2c scan` |
| `i2c scan` finds nothing | Check SDA/SCL pins in `include/config.h` and the pull-up resistors |
