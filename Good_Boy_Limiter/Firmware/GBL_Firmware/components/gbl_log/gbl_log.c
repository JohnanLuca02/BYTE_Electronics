/*
 * GBL - Persistent and remote logging
 *
 * ESP_LOG callers never touch the flash or the UART: the vprintf hook only
 * copies the formatted line into a ring buffer (non-blocking, thread-safe) and
 * a low-priority task writes it to the file and the UART. Logging therefore
 * never stalls the pre-charge loop or the Q1 watchdog callback.
 *
 * Storage: SPIFFS on the "log" partition (partitions.csv). The current file
 * rotates to LOG_FILE_OLD when it exceeds LOG_FILE_MAX, so the last
 * ~2 × LOG_FILE_MAX bytes are always kept. Each line is flushed and synced:
 * the last line before a power loss is on flash. Wear is negligible because
 * only events are logged (a few lines per power cycle), not periodic samples.
 */

#include "gbl_log.h"

#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"

/* J2 UART (UART0 is free: the console runs on USB Serial/JTAG) */
#define LOG_UART UART_NUM_0
#define PIN_LOG_TX GPIO_NUM_43 // J2 pin 1 (U0TXD)
#define PIN_LOG_RX GPIO_NUM_44 // J2 pin 3 (U0RXD)
#define LOG_UART_BAUD 115200
#define CMD_DUMP "L"		   // line received on the UART: dump the stored log
#define RX_LINE_MAX 32		   // received lines are short commands/replies
#define RX_QUEUE_LEN 4		   // received lines waiting for gbl_log_wait_line()

/* Storage */
#define LOG_MOUNT "/log"
#define LOG_FILE LOG_MOUNT "/gbl.log"
#define LOG_FILE_OLD LOG_MOUNT "/gbl.1.log"
#define LOG_FILE_MAX (128 * 1024) // per file; the 512 KB partition keeps SPIFFS headroom

/* Buffering */
#define LOG_LINE_MAX 256		  // longer lines are truncated in the file/UART copy only
#define RING_SIZE (8 * 1024) // ~ 100 lines: covers boot, before the file system is mounted
#define TASK_STACK 4096
#define TASK_PRIO 2			  // below the application: logging must never delay it
#define POLL_MS 100			  // UART command polling period while idle

static const char *TAG = "log";

static RingbufHandle_t ring;
static QueueHandle_t rx_lines; // char[RX_LINE_MAX] items
static vprintf_like_t console_vprintf;

/* ESP_LOG hook: queue a copy of the line, then print it on the console as before. */
static int log_vprintf(const char *fmt, va_list args)
{
	char line[LOG_LINE_MAX];
	va_list copy;
	va_copy(copy, args);
	int n = vsnprintf(line, sizeof line, fmt, copy);
	va_end(copy);

	if (n > 0)
		xRingbufferSend(ring, line, n < LOG_LINE_MAX ? n : LOG_LINE_MAX - 1, 0); // full: line dropped, never block
	return console_vprintf(fmt, args);
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
	switch (r)
	{
	case ESP_RST_POWERON:
		return "power-on";
	case ESP_RST_SW:
		return "software";
	case ESP_RST_PANIC:
		return "panic (crash)";
	case ESP_RST_INT_WDT:
	case ESP_RST_TASK_WDT:
	case ESP_RST_WDT:
		return "watchdog";
	case ESP_RST_BROWNOUT:
		return "brownout (3V3 dip)";
	case ESP_RST_EXT:
		return "reset pin";
	default:
		return "other";
	}
}

static long file_size(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 ? st.st_size : 0;
}

/* Moves the current file to LOG_FILE_OLD (dropping the previous one) and reopens it. */
static FILE *rotate(FILE *f)
{
	if (f)
		fclose(f);
	unlink(LOG_FILE_OLD);
	rename(LOG_FILE, LOG_FILE_OLD);
	return fopen(LOG_FILE, "a");
}

static void dump_file(const char *path, FILE *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char buf[256];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		fwrite(buf, 1, n, out);
	fclose(f);
}

static void uart_write_str(const char *s)
{
	uart_write_bytes(LOG_UART, s, strlen(s));
}

/* Same framing as gbl_log_dump(): the Jetson script keys on these markers. */
static void dump_to_uart(void)
{
	char buf[256];
	const char *files[] = {LOG_FILE_OLD, LOG_FILE};
	uart_write_str("=== stored log (older first) ===\n");
	for (int i = 0; i < 2; i++)
	{
		FILE *f = fopen(files[i], "r");
		if (!f)
			continue;
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			uart_write_bytes(LOG_UART, buf, n);
		fclose(f);
	}
	uart_write_str("=== end of stored log ===\n");
}

static void uart_setup(void)
{
	const uart_config_t cfg = {
		.baud_rate = LOG_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(LOG_UART, 1024, 4096, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(LOG_UART, &cfg));
	ESP_ERROR_CHECK(uart_set_pin(LOG_UART, PIN_LOG_TX, PIN_LOG_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* Mounts the file system (formatting it on first use) and opens the log file.
 * Returns NULL on failure: the UART stream keeps working without the file. */
static FILE *storage_open(void)
{
	const esp_vfs_spiffs_conf_t conf = {
		.base_path = LOG_MOUNT,
		.partition_label = "log",
		.max_files = 4,
		.format_if_mount_failed = true,
	};
	if (esp_vfs_spiffs_register(&conf) != ESP_OK)
		return NULL;
	return file_size(LOG_FILE) > LOG_FILE_MAX ? rotate(NULL) : fopen(LOG_FILE, "a");
}

static void poll_uart_rx(void);

static void log_task(void *arg)
{
	uart_setup();
	FILE *f = storage_open();
	if (!f)
		ESP_LOGE(TAG, "Log file unavailable, logging to UART only");

	// Session marker: separates boots in the file and records why the previous one ended.
	ESP_LOGI(TAG, "=== boot, reset reason: %s ===", reset_reason_name(esp_reset_reason()));

	while (1)
	{
		size_t len;
		char *line = xRingbufferReceive(ring, &len, pdMS_TO_TICKS(POLL_MS));
		if (line)
		{
			uart_write_bytes(LOG_UART, line, len);
			if (f)
			{
				fwrite(line, 1, len, f);
				fflush(f);
				fsync(fileno(f));
				if (ftell(f) > LOG_FILE_MAX)
					f = rotate(f);
			}
			vRingbufferReturnItem(ring, line);
		}

		poll_uart_rx();
	}
}

/* Assembles received characters into lines. "L" is handled here (stored log
 * dump); any other line is queued for gbl_log_wait_line(). */
static void poll_uart_rx(void)
{
	static char rx[RX_LINE_MAX];
	static size_t n;
	uint8_t c;

	while (uart_read_bytes(LOG_UART, &c, 1, 0) == 1)
	{
		if (c != '\n' && c != '\r')
		{
			if (n < RX_LINE_MAX - 1)
				rx[n++] = c;
			continue;
		}
		if (n == 0)
			continue; // empty line or second half of "\r\n"
		rx[n] = '\0';
		n = 0;
		if (strcmp(rx, CMD_DUMP) == 0)
			dump_to_uart();
		else
			xQueueSend(rx_lines, rx, 0); // queue full: oldest unread replies are kept
	}
}

void gbl_log_init(void)
{
	ring = xRingbufferCreate(RING_SIZE, RINGBUF_TYPE_NOSPLIT);
	rx_lines = xQueueCreate(RX_QUEUE_LEN, RX_LINE_MAX);
	if (!ring || !rx_lines)
		return; // out of memory at boot: keep the plain console log
	console_vprintf = esp_log_set_vprintf(log_vprintf);
	xTaskCreate(log_task, "gbl_log", TASK_STACK, NULL, TASK_PRIO, NULL);
}

void gbl_log_dump(FILE *out)
{
	fprintf(out, "\n=== stored log (older first) ===\n");
	dump_file(LOG_FILE_OLD, out);
	dump_file(LOG_FILE, out);
	fprintf(out, "=== end of stored log ===\n");
}

void gbl_log_send_line(const char *line)
{
	char buf[RX_LINE_MAX + 2];
	int n = snprintf(buf, sizeof buf, "%s\n", line);
	if (rx_lines)
		xQueueReset(rx_lines); // stale replies must not answer this command
	if (n > 0 && n < (int)sizeof buf && uart_is_driver_installed(LOG_UART))
		uart_write_bytes(LOG_UART, buf, n); // one call: never split by a log line
}

bool gbl_log_wait_line(const char *expected, int timeout_ms)
{
	if (!rx_lines)
		return false;
	TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
	char line[RX_LINE_MAX];
	do
	{
		TickType_t now = xTaskGetTickCount();
		TickType_t wait = deadline > now ? deadline - now : 0;
		if (xQueueReceive(rx_lines, line, wait) == pdTRUE && strcmp(line, expected) == 0)
			return true;
	} while (xTaskGetTickCount() < deadline);
	return false;
}
