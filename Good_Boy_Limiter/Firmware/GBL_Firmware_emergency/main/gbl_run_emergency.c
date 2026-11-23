/*
 * GBL - EMERGENCY application (TEMPORARY, NOT production)
 *
 * Copy of the production state machine (../GBL_Firmware/main/gbl_run.c) with
 * ONE difference: the pre-charge is emergency_precharge() below instead of
 * gbl_power_precharge().
 *
 * Why: on the current board revision, with the motors connected, the GDS68
 * drivers draw more power than Rpre1 can deliver (~2.9 W at 24 V). The charge
 * stalls at a few volts and the strict pre-charge always aborts.
 * What it does: charges as far as the board can, then closes the relay anyway.
 * Risk: the remaining voltage step (e.g. 24 V - 6 V) charges the capacitors
 * through the relay contacts, limited only by wiring and capacitor ESR:
 * contact wear and possible welding. A welded relay can no longer be opened
 * by the GBL: keep the hardware emergency stop and the battery connector at hand.
 *
 * The line is controlled by the main power switch (J2 pins 9-10).
 * This is NOT the emergency stop, which must act in hardware on the relay
 * coil without going through the ESP32.
 *
 *   WAIT_SWITCH_OFF --switch OFF--> OFF --switch ON--> pre-charge --ok--> ON
 *          ^                          ^                    |              |
 *          +----------- fault --------|--------------------+  switch OFF  |
 *          +---- undervoltage --------|------------------------------+    v
 *                                     |                              JETSON_OFF
 *                                     |                                   | Jetson halted
 *                                     |                                   v (or no reply)
 *                                     +--- bus < 5 V (or switch ON) -- STOPPING
 *
 * - If the switch is already ON at boot, nothing starts: an OFF -> ON cycle is
 *   required, so a reset or a mains return never re-energises the line alone.
 * - After a fault (short, ambiguous charge, voltage out of range, undervoltage) the line stays off
 *   until the switch is turned back OFF: no automatic retry on a fault.
 * - Before the line is opened, the Jetson (powered downstream) is asked to
 *   halt over the J2 UART, so its file system is never cut while writing:
 *     GBL -> "GBL:SHUTDOWN", Jetson -> "JETSON:ACK" and runs poweroff,
 *     GBL waits JETSON_HALT_MS, then opens the line.
 *   No ACK within JETSON_ACK_MS (Jetson off, booting, or not connected): the
 *   line is opened right away. Once requested, the shutdown always completes,
 *   even if the switch goes back ON: the Jetson is already halting and needs a
 *   power cycle to restart.
 * - Shutdown is controlled (see gbl_power_shutdown) and the OFF state is
 *   reached only once the bus is discharged: only then is unplugging the PDBs
 *   safe.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"
#include "gbl_log.h"

#define LOOP_PERIOD_MS 20
#define V_BUS_SAFE 5.0f	   // below this the bus is discharged: safe to unplug the PDBs
#define V_MIN_ON (3.0f * CONFIG_GBL_BATT_CELLS) // in ON, 3.0 V/cell under load: pack empty or line lost (6S 18 V, 12S 36 V, 13S 39 V)
#define UNDERVOLT_MS 100   // minimum undervoltage duration before acting

/* Jetson shutdown handshake (protocol strings shared with tools/jetson/gbl_link.sh) */
#define JETSON_CMD_SHUTDOWN "GBL:SHUTDOWN"
#define JETSON_REPLY_ACK "JETSON:ACK"
#define JETSON_ACK_MS 2000	 // no reply within this time: Jetson not running
#define JETSON_HALT_MS 20000 // Orin Nano poweroff takes ~10-15 s: margin included

/* Welded relay detection (the partial pre-charge stresses the relay contacts).
 * Test at switch OFF, while the Jetson is still running: the relay opens for
 * WELD_TEST_OPEN_MS with Q1 in parallel. With the drivers + Jetson load
 * (~1-1.5 A) the bus drops by several volts; a bus that does not move means
 * the contacts did not separate. The relay is then closed again and the
 * normal shutdown follows. With no load (bench) the bus barely moves either:
 * expect false alarms there. */
#define JETSON_CMD_WELDED "GBL:RELAY_WELDED" // Jetson: motors off, then poweroff
#define WELD_TEST_OPEN_MS 50
#define WELD_TEST_Q1_ON_MS 100	   // LM5069 gate turn-on before opening the relay
#define WELD_TEST_MIN_DROP 1.0f	   // smaller drop while open: contacts welded
#define WELD_CONFIRM_MS 3000	   // after the shutdown the bus must have dropped within this time
#define WELD_REMIND_MS 10000	   // repeat the error while the bus stays up

/* Emergency pre-charge. Same per-cell limits as gbl_power.c; the Q1 watchdog
 * (3.5 s) and the 10 s Rpre1 cooldown of the driver stay active. */
#define EP_SAMPLE_MS 20
#define EP_SHORT_CHECK_MS 300						  // from here on a real short is detected
#define EP_V_SHORT 3.0f								  // below this at EP_SHORT_CHECK_MS: short (1 Ω reads ~0.5 V)
#define EP_V_MAX_MISMATCH (0.15f * CONFIG_GBL_BATT_CELLS) // max spread between PDBs (6S 0.9 V)
#define EP_V_MAX ((4.2f + 0.15f) * CONFIG_GBL_BATT_CELLS) // above: wrong battery (6S 26.1 V)
#define EP_V_MIN_CLOSED (3.2f * CONFIG_GBL_BATT_CELLS)	  // with the relay closed the bus must be here (6S 19.2 V)
#define EP_SETTLE_WINDOW_MS 200
#define EP_SETTLE_DV 0.1f		// < 0.1 V rise in 200 ms: charge stalled or complete
#define EP_TIMEOUT_MS 1500		// a stall at ~6 V dissipates ~6.5 W in Rpre1: do not linger
#define EP_RELAY_SETTLE_MS 100
#define EP_CLOSE_TIMEOUT_MS 1000 // relay closed: bus must reach EP_V_MIN_CLOSED within this time

static const char *TAG = "run_EMERGENCY";

typedef enum
{
	ST_WAIT_SWITCH_OFF, // locked: waits for the switch to go OFF (boot with switch ON or fault)
	ST_OFF,				// line off, ready to switch on
	ST_ON,				// relay closed, line powered
	ST_JETSON_OFF,		// line still powered, waiting for the Jetson to halt
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
	case ST_JETSON_OFF:
		return "JETSON_OFF";
	case ST_STOPPING:
		return "STOPPING";
	}
	return "?";
}

static state_t state;
static int undervolt_ms;
static bool cooldown_notified;
static bool jetson_acked;
static int jetson_wait_ms; // time left in ST_JETSON_OFF
static int stopping_ms;	   // time spent in ST_STOPPING

static void set_state(state_t next)
{
	if (next != state)
		ESP_LOGI(TAG, "State %s -> %s", state_name(state), state_name(next));
	state = next;
}

static gbl_precharge_result_t ep_abort(const char *reason, const gbl_vsens_t *r)
{
	gbl_power_off();
	ESP_LOGE(TAG, "Pre-charge aborted: %s", reason);
	gbl_vsens_log("abort", r);
	return GBL_PRECHARGE_FAULT;
}

/* Charges through Q1 + Rpre1 until the voltage stops rising (or
 * EP_TIMEOUT_MS), then closes the relay WHATEVER the voltage reached.
 * Still aborts on: a real short (below EP_V_SHORT after EP_SHORT_CHECK_MS),
 * PDB mismatch, voltage above the battery maximum, and a bus still below
 * EP_V_MIN_CLOSED once the relay is closed (relay not closed, empty battery,
 * short). Built only on the public gbl_power / gbl_vsens API. */
static gbl_precharge_result_t emergency_precharge(void)
{
	gbl_power_relay_set(false);
	if (!gbl_power_q1_set(true))
		return GBL_PRECHARGE_REFUSED;
	ESP_LOGW(TAG, "EMERGENCY pre-charge started: the relay closes even if the charge stalls");

	gbl_vsens_t r, window_start;
	gbl_vsens_read(&r);
	window_start = r;
	int64_t t0 = esp_timer_get_time();
	int window_start_ms = 0;

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(EP_SAMPLE_MS));
		int ms = (esp_timer_get_time() - t0) / 1000;

		gbl_vsens_read(&r);
		if (!r.valid)
			return ep_abort("no PDB enabled", &r);
		if (r.max > EP_V_MAX)
			return ep_abort("voltage too high for the battery", &r);
		if (ms >= EP_SHORT_CHECK_MS && r.min < EP_V_SHORT)
			return ep_abort("voltage not rising: short circuit", &r);
		if (ms >= EP_SHORT_CHECK_MS && r.max - r.min > EP_V_MAX_MISMATCH)
			return ep_abort("ambiguous charge, PDB voltages too far apart", &r);

		bool stalled = false;
		if (ms - window_start_ms >= EP_SETTLE_WINDOW_MS)
		{
			stalled = true;
			for (int ch = 0; ch < GBL_VSENS_CHANNELS; ch++)
				if (gbl_vsens_enabled(ch) && r.volt[ch] - window_start.volt[ch] > EP_SETTLE_DV)
					stalled = false;
			window_start = r;
			window_start_ms = ms;
		}

		if ((stalled && ms >= EP_SHORT_CHECK_MS) || ms >= EP_TIMEOUT_MS)
		{
			ESP_LOGW(TAG, "Charge stopped at %.1f V after %d ms: closing the relay", r.min, ms);
			gbl_vsens_log("precharged", &r);
			break;
		}
	}

	// With the relay closed the bus rises within EP_CLOSE_TIMEOUT_MS
	// (instantly on the battery, slower on a current-limited bench supply).
	gbl_power_relay_set(true);
	int closed_ms = 0;
	do
	{
		vTaskDelay(pdMS_TO_TICKS(EP_SAMPLE_MS));
		closed_ms += EP_SAMPLE_MS;
		gbl_vsens_read(&r);
	} while (r.min < EP_V_MIN_CLOSED && closed_ms < EP_CLOSE_TIMEOUT_MS);
	if (r.min < EP_V_MIN_CLOSED)
		return ep_abort("bus still low with the relay closed: relay not closed, battery empty or short", &r);
	ESP_LOGI(TAG, "Bus above %.1f V %d ms after closing the relay", EP_V_MIN_CLOSED, closed_ms);
	vTaskDelay(pdMS_TO_TICKS(EP_RELAY_SETTLE_MS)); // contact bounce before Q1 goes off
	gbl_vsens_log("closed", &r);
	gbl_power_q1_set(false);
	return GBL_PRECHARGE_OK;
}

/* Opens the relay for WELD_TEST_OPEN_MS (Q1 in parallel limits the contact
 * arc), measures how much the bus drops, then closes it again. Returns true
 * if the bus did not drop: the contacts did not separate. Blocking, ~250 ms. */
static bool relay_seems_welded(void)
{
	gbl_vsens_t before, open;
	gbl_vsens_read(&before);

	// May be refused during the Rpre1 cooldown: the test then runs without the
	// parallel path (small arc at ~1 A, acceptable once).
	if (gbl_power_q1_set(true))
		vTaskDelay(pdMS_TO_TICKS(WELD_TEST_Q1_ON_MS));
	gbl_power_relay_set(false);
	vTaskDelay(pdMS_TO_TICKS(WELD_TEST_OPEN_MS));
	gbl_vsens_read(&open);
	gbl_power_relay_set(true);
	vTaskDelay(pdMS_TO_TICKS(EP_RELAY_SETTLE_MS));
	gbl_power_q1_set(false);

	float drop = before.min - open.min;
	ESP_LOGI(TAG, "Relay test: %.1f V closed, %.1f V after %d ms open (drop %.2f V)",
			 before.min, open.min, WELD_TEST_OPEN_MS, drop);
	return drop < WELD_TEST_MIN_DROP;
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

	switch (emergency_precharge())
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

/* Line lost while powered (relay dropped out, source missing, overload):
 * after UNDERVOLT_MS opens everything and locks until the switch goes OFF.
 * Returns true if it acted. */
static bool check_undervoltage(const gbl_vsens_t *bus)
{
	bool low = !bus->valid || bus->min < V_MIN_ON;
	undervolt_ms = low ? undervolt_ms + LOOP_PERIOD_MS : 0;
	if (undervolt_ms < UNDERVOLT_MS)
		return false;

	gbl_power_off();
	ESP_LOGE(TAG, "Undervoltage while the line is on: line opened, turn the switch OFF");
	gbl_vsens_log("fault", bus);
	set_state(ST_WAIT_SWITCH_OFF);
	return true;
}

static void on_on(bool sw_on, const gbl_vsens_t *bus)
{
	if (check_undervoltage(bus))
		return;
	if (sw_on)
		return;

	// Switch OFF: check the relay while the Jetson can still be told, then ask
	// it to halt before cutting its power.
	if (relay_seems_welded())
	{
		ESP_LOGE(TAG, "RELAY WELDED? Telling the Jetson to stop the motors and halt");
		gbl_log_send_line(JETSON_CMD_WELDED);
	}
	else
	{
		ESP_LOGI(TAG, "Switch OFF: asking the Jetson to halt");
		gbl_log_send_line(JETSON_CMD_SHUTDOWN);
	}
	jetson_acked = false;
	jetson_wait_ms = JETSON_ACK_MS;
	set_state(ST_JETSON_OFF);
}

static void on_jetson_off(const gbl_vsens_t *bus)
{
	if (check_undervoltage(bus))
		return;

	if (!jetson_acked && gbl_log_wait_line(JETSON_REPLY_ACK, 0))
	{
		jetson_acked = true;
		jetson_wait_ms = JETSON_HALT_MS;
		ESP_LOGI(TAG, "Jetson halting, line opens in %d s", JETSON_HALT_MS / 1000);
	}

	jetson_wait_ms -= LOOP_PERIOD_MS;
	if (jetson_wait_ms > 0)
		return;

	if (!jetson_acked)
		ESP_LOGW(TAG, "No reply from the Jetson: opening the line anyway");
	gbl_power_shutdown();
	stopping_ms = 0;
	set_state(ST_STOPPING);
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
		return;
	}

	// Line opened but the bus is still at battery level: contacts welded.
	stopping_ms += LOOP_PERIOD_MS;
	if (stopping_ms >= WELD_CONFIRM_MS && bus->min >= V_MIN_ON &&
		(stopping_ms - WELD_CONFIRM_MS) % WELD_REMIND_MS < LOOP_PERIOD_MS)
	{
		ESP_LOGE(TAG, "RELAY WELDED: bus still at %.1f V with the relay open. "
					  "Motors off, then disconnect the battery",
				 bus->min);
		gbl_log_send_line(JETSON_CMD_WELDED);
	}
}

void gbl_run_emergency_start(void)
{
	ESP_LOGW(TAG, "*** GBL EMERGENCY firmware (TEMPORARY): relay closes on a partial pre-charge ***");

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
		case ST_JETSON_OFF:
			on_jetson_off(&bus); // switch ignored: the shutdown always completes
			break;
		case ST_STOPPING:
			on_stopping(sw_on, &bus);
			break;
		}

		vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
	}
}
