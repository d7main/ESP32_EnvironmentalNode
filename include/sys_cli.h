/**
 * This file is part of the ESP32_EnvironmentalNode project.
 *
 * @file  sys_cli.h
 * @brief Wired service console (UART REPL) for configuration and diagnostics.
 *
 *        Entry flow
 *        ──────────
 *        On a cold boot / RESET (never on a deep-sleep timer wake-up, so the
 *        battery budget of the normal cycle is untouched) app_main calls
 *        sys_cli_wait_for_key().  If the user presses ENTER in the serial
 *        terminal within the window, sys_cli_run() opens an interactive
 *        REPL and blocks until the `exit` command is issued.
 *
 *        The console works on its OWN copy of the configuration loaded from
 *        NVS.  Changes are persisted only by `config save`; the caller should
 *        reload the config from NVS after sys_cli_run() returns.
 *
 * @author d7main
 * contact info: demianzaiats@gmail.com
 * @license MIT
 */

#ifndef SYS_CLI_H
#define SYS_CLI_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Print an invitation and wait for ENTER on the console UART.
 *
 * @param timeout_ms  How long to wait for the key press.
 * @return true if ENTER was pressed within the window.
 */
bool sys_cli_wait_for_key(uint32_t timeout_ms);

/**
 * @brief Start the interactive service console.
 *
 *        Blocks the calling task until the user types `exit`.
 *        (`reboot` and `config reset` never return — they restart the chip.)
 */
void sys_cli_run(void);

#endif /* SYS_CLI_H */
