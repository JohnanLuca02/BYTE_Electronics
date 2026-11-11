/*
 * GBL - Main power switch input
 *
 * Optional hardware: 1k in series on GPIO1 near the J2 header, to limit the
 * current into the pin if the switch wiring touches a higher voltage.
 */

#include "gbl_switch.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#define PIN_SWITCH GPIO_NUM_1 // J2 pin 9, active low
#define DEBOUNCE_US (50 * 1000)

static bool stable_on;
static bool last_raw;
static int64_t last_change_us;

void gbl_switch_init(void)
{
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << PIN_SWITCH,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE, // broken wire = high level = OFF
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&cfg));

	// Start from the current level so the first reading is already valid.
	last_raw = stable_on = gbl_switch_raw_on();
	last_change_us = esp_timer_get_time();
}

bool gbl_switch_raw_on(void)
{
	return gpio_get_level(PIN_SWITCH) == 0;
}

bool gbl_switch_is_on(void)
{
	bool raw = gbl_switch_raw_on();
	int64_t now = esp_timer_get_time();

	if (raw != last_raw)
	{
		last_raw = raw;
		last_change_us = now;
	}
	else if (raw != stable_on && now - last_change_us >= DEBOUNCE_US)
	{
		stable_on = raw;
	}
	return stable_on;
}
