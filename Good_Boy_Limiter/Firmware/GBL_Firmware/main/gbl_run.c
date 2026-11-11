/*
 * GBL - Production application
 *
 * The line is controlled by the main power switch (J2 pins 9-10).
 * This is NOT the emergency stop, which must act in hardware on the relay
 * coil without going through the ESP32.
 *
 *   WAIT_SWITCH_OFF --switch OFF--> OFF --switch ON--> pre-charge --ok--> ON
 *          ^                          ^                    |              |
 *          +----------- fault --------|--------------------+  switch OFF  |
 *          +---- undervoltage in ON --|-----------------------------------+
 *                                     |                                   v
 *                                     +--- bus < 5 V (or switch ON) -- STOPPING
 *
 * - If the switch is already ON at boot, nothing starts: an OFF -> ON cycle is
 *   required, so a reset or a mains return never re-energises the line alone.
 * - After a fault (short, ambiguous charge, voltage out of range, undervoltage) the line stays off
 *   until the switch is turned back OFF: no automatic retry on a fault.
 * - Shutdown is controlled (see gbl_power_shutdown) and the OFF state is
 *   reached only once the bus is discharged: only then is unplugging the PDBs
 *   safe.
 */

#include "gbl_run.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"

#define LOOP_PERIOD_MS 20
#define V_BUS_SAFE 5.0f	   // below this the bus is discharged: safe to unplug the PDBs
#define V_MIN_ON (3.0f * CONFIG_GBL_BATT_CELLS) // in ON, 3.0 V/cell under load: pack empty or line lost (6S 18 V, 12S 36 V, 13S 39 V)
#define UNDERVOLT_MS 100   // minimum undervoltage duration before acting

static const char *TAG = "run";

typedef enum
{
	ST_WAIT_SWITCH_OFF, // locked: waits for the switch to go OFF (boot with switch ON or fault)
	ST_OFF,				// line off, ready to switch on
	ST_ON,				// relay closed, line powered
	ST_STOPPING,		// relay open, waiting for the bus to discharge
} state_t;

static const char *state_name(state_t s)
{
	switch (s)
	{
	case ST_WAIT_SWITCH_OFF:
		return "WAIT_SWITCH_OFF";
	case ST_OFF:
		return "OFF";
	case ST_ON:
		return "ON";
	case ST_STOPPING:
		return "STOPPING";
	}
	return "?";
}

static state_t state;
static int undervolt_ms;
static bool cooldown_notified;

static void set_state(state_t next)
{
	if (next != state)
		ESP_LOGI(TAG, "State %s -> %s", state_name(state), state_name(next));
	state = next;
}

/* One handler per state. Each runs once per loop iteration with the debounced
 * switch level (and the bus voltages where needed) and decides the next state. */

static void on_wait_switch_off(bool sw_on)
{
	if (!sw_on)
		set_state(ST_OFF);
}

static void on_off(bool sw_on)
{
	if (!sw_on)
	{
		cooldown_notified = false;
		return;
	}

	// Switch ON: pre-charge starts as soon as Rpre1 is out of its cooldown.
	int cooldown = gbl_power_cooldown_left_ms();
	if (cooldown > 0)
	{
		if (!cooldown_notified)
			ESP_LOGI(TAG, "Waiting for Rpre1 cooldown (%d ms)", cooldown);
		cooldown_notified = true;
		return;
	}
	cooldown_notified = false;

	switch (gbl_power_precharge())
	{
	case GBL_PRECHARGE_OK:
		undervolt_ms = 0;
		set_state(ST_ON);
		break;
	case GBL_PRECHARGE_FAULT:
		ESP_LOGE(TAG, "Pre-charge failed: turn the switch OFF to retry");
		set_state(ST_WAIT_SWITCH_OFF);
		break;
	case GBL_PRECHARGE_REFUSED:
		break; // not expected: the cooldown was checked above
	}
}

static void on_on(bool sw_on, const gbl_vsens_t *bus)
{
	if (!sw_on)
	{
		gbl_power_shutdown();
		set_state(ST_STOPPING);
		return;
	}

	// Line lost (relay dropped out, source missing, overload): open everything.
	bool low = !bus->valid || bus->min < V_MIN_ON;
	undervolt_ms = low ? undervolt_ms + LOOP_PERIOD_MS : 0;
	if (undervolt_ms >= UNDERVOLT_MS)
	{
		gbl_power_off();
		ESP_LOGE(TAG, "Undervoltage while ON: line opened, turn the switch OFF");
		gbl_vsens_log("fault", bus);
		set_state(ST_WAIT_SWITCH_OFF);
	}
}

static void on_stopping(bool sw_on, const gbl_vsens_t *bus)
{
	if (sw_on)
	{
		// Switch-on requested before full discharge: pre-charge starts from
		// the residual voltage, no inrush risk.
		set_state(ST_OFF);
		return;
	}
	if (bus->valid && bus->max < V_BUS_SAFE)
	{
		ESP_LOGI(TAG, "Bus discharged (%.1f V): safe to unplug the PDBs", bus->max);
		set_state(ST_OFF);
	}
}

void gbl_run_start(void)
{
	ESP_LOGI(TAG, "GBL production firmware");

	state = ST_OFF;
	if (gbl_switch_is_on())
	{
		ESP_LOGW(TAG, "Switch already ON at boot: turn it OFF and then ON to power the line");
		state = ST_WAIT_SWITCH_OFF;
	}
	ESP_LOGI(TAG, "State %s", state_name(state));

	while (1)
	{
		bool sw_on = gbl_switch_is_on();
		gbl_vsens_t bus;
		gbl_vsens_read(&bus);

		switch (state)
		{
		case ST_WAIT_SWITCH_OFF:
			on_wait_switch_off(sw_on);
			break;
		case ST_OFF:
			on_off(sw_on);
			break;
		case ST_ON:
			on_on(sw_on, &bus);
			break;
		case ST_STOPPING:
			on_stopping(sw_on, &bus);
			break;
		}

		vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
	}
}
