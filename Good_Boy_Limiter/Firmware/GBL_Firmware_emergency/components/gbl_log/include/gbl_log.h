/*
 * GBL - Persistent and remote logging
 *
 * Every ESP_LOG message, besides the USB console, is:
 *   - appended to a log file in flash (black box: survives resets and power loss);
 *   - streamed live on the J2 UART (pin 1 TX, pin 3 RX, 115200 8N1, 3.3 V),
 *     meant for the robot computer (Jetson Orin Nano).
 * Sending the line "L" on the J2 UART dumps the stored log on the same UART.
 *
 * The same UART also carries short command lines between the GBL and the
 * Jetson (e.g. the shutdown handshake in gbl_run.c), see gbl_log_send_line()
 * and gbl_log_wait_line().
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

/* Installs the ESP_LOG hook and starts the logging task. Call it first in
 * app_main(): it does no flash or UART I/O itself (the task does), so it does
 * not delay the rest of the start-up, and messages logged before the file
 * system is mounted are buffered, not lost. */
void gbl_log_init(void);

/* Writes the stored log (older file first) to `out`, e.g. stdout. */
void gbl_log_dump(FILE *out);

/* Sends one command line (max 31 characters, newline added) on the J2 UART.
 * Lines received before this call are discarded, so a later
 * gbl_log_wait_line() only sees replies to this command. */
void gbl_log_send_line(const char *line);

/* Waits up to `timeout_ms` for the line `expected` on the J2 UART. Other
 * received lines are discarded. Returns false on timeout. */
bool gbl_log_wait_line(const char *expected, int timeout_ms);
