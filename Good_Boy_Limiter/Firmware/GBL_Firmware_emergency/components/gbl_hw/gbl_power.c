/*
 * GBL - Power path control
 *
 * Power architecture (see GBL schematic, "In-rush controller" sheet):
 *
 *   +48VIN --Rsns1--[Q1 STB15810]--Rpre1 50R--+--> +48VOUT (PDB output caps)
 *                    ^ gate driven by LM5069   |
 *   +48VIN -----------[external relay contacts]-+   (low-impedance bypass)
 *
 * - Q1 is driven by the LM5069. The ESP32 only enables/disables it through the
 *   LM5069 UVLO pin, which Q2 (AO3422) pulls to GND when GPIO2 = 1.
 * - The relay (coil driven low-side by Q4) must only close once pre-charge is
 *   complete, so the inrush current never flows through its contacts.
 * - Rpre1 (EWWR0010J50R0T9: 10 W, ~25 J single pulse at 50 Ω) is the
 *   thermally critical part: every pre-charge dissipates ½·C·V² in it
 *   (≈ 9.4 J with 8.1 mF at 48 V), and with a shorted output it sees V²/50
 *   (≈ 46 W at 48 V), which the LM5069 does not limit because the current
 *   stays below its thresholds. Q1 is therefore protected by a watchdog, a
 *   cooldown and PDB voltage monitoring.
 */

#include "gbl_power.h"
#include "gbl_vsens.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Pins */
#define PIN_Q1_DISABLE GPIO_NUM_2 // 1 = Q2 on -> UVLO to GND -> Q1 off (inverted logic)
#define PIN_RELAY GPIO_NUM_38	  // 1 = Q4 on -> relay coil energised

/* Rpre1 protection and timings */
#define PRECHARGE_TIMEOUT_MS 3000				 // ≈ 7τ with 8.1 mF (4 PDBs × 6×330 µF + Cout2)
#define Q1_MAX_ON_MS (PRECHARGE_TIMEOUT_MS + 500) // watchdog: hard limit, independent of the caller
#define Q1_COOLDOWN_MS 10000					 // minimum pause between two pre-charges
#define RELAY_SETTLE_MS 100						 // contact closing/opening and bounce
#define SHUTDOWN_OVERLAP_MS 200					 // Q1 holds the bus while the relay opens

/* Battery pack, from menuconfig "GBL firmware -> Battery pack (LiPo)".
 * REMEMBER: switching between 6S (24 V) and 12S/13S (48 V) also requires the
 * UVLO resistor between +48VIN and UVLO (TP9-TP2), in parallel with R31 143k:
 *   - 6S     : 100k SOLDERED -> UVLO on ~17.2 V, off ~16.0 V
 *   - 12S/13S: REMOVED       -> UVLO on ~38.3 V, off ~35.3 V (R31/R30 only) */
#define CELLS CONFIG_GBL_BATT_CELLS
#define CELL_V_EMPTY 3.2f // at rest (pre-charge draws no load): below this the pack is nearly empty
#define CELL_V_FULL 4.2f  // LiPo charge limit
#define CELL_V_MARGIN 0.15f // divider + ADC tolerance above the full voltage

/* Pre-charge checks, derived from τ = 50 Ω · C (τ ≈ 0.41 s with 8.1 mF).
 * With much larger capacitance (> ~20 mF) SHORT_CHECK_MS must be increased. */
#define SAMPLE_PERIOD_MS 20
#define SHORT_CHECK_MS 300	 // at 300 ms a healthy PDB is already at ~50% of Vin
#define V_MIN_AT_CHECK 5.0f	 // below this at SHORT_CHECK_MS: short or faulty sensor
#define V_MAX_MISMATCH (0.15f * CELLS) // max spread between PDBs, above: ambiguous charge (6S 0.9 V, 12S 1.8 V, 13S 2.0 V)
#define V_MIN_CHARGED (CELL_V_EMPTY * CELLS) // pack empty or wrong config (6S 19.2 V, 12S 38.4 V, 13S 41.6 V)
#define V_MAX_CHARGED ((CELL_V_FULL + CELL_V_MARGIN) * CELLS) // above: wrong pack/config (6S 26.1 V, 12S 52.2 V, 13S 56.6 V)
#define SETTLE_WINDOW_MS 200 // window used to detect the end of the charge
#define SETTLE_DV 0.1f		 // 0.1 V / 200 ms = 0.5 V/s -> residual gap ≈ τ·0.5 ≈ 0.2 V

static const char *TAG = "power";

static esp_timer_handle_t q1_guard_timer;
static volatile bool q1_on = false;
// Initialised in the past so the first switch-on after boot does not wait for the cooldown.
static volatile int64_t q1_off_since_us = -(int64_t)Q1_COOLDOWN_MS * 1000;

/* ------------------------------------------------------------------------- */
/* Q1 and relay                                                              */
/* ------------------------------------------------------------------------- */

/* Turns Q1 off and records the time for the cooldown. Called both from the
 * calling task and from the esp_timer callback. */
static void q1_force_off(void)
{
	gpio_set_level(PIN_Q1_DISABLE, 1);
	if (q1_on)
		q1_off_since_us = esp_timer_get_time();
	q1_on = false;
}

/* Watchdog independent of the application code: it runs in the esp_timer
 * task, so it turns Q1 off even if the caller is stuck. */
static void q1_guard_cb(void *arg)
{
	q1_force_off();
	ESP_LOGW(TAG, "Q1 on for more than %d ms -> switched off to protect Rpre1", Q1_MAX_ON_MS);
}

/* Turns Q1 on, arming the watchdog first (never after: if the code hung
 * between the two steps, Q1 would stay on unprotected). */
static void q1_force_on(void)
{
	esp_timer_stop(q1_guard_timer); // ESP_ERR_INVALID_STATE if not armed: safe to ignore
	esp_timer_start_once(q1_guard_timer, (uint64_t)Q1_MAX_ON_MS * 1000);
	q1_on = true;
	gpio_set_level(PIN_Q1_DISABLE, 0);
}

/* ------------------------------------------------------------------------- */
/* Public API (documented in gbl_power.h)                                    */
/* ------------------------------------------------------------------------- */

int gbl_power_cooldown_left_ms(void)
{
	if (q1_on)
		return 0;
	int64_t left = Q1_COOLDOWN_MS - (esp_timer_get_time() - q1_off_since_us) / 1000;
	return left > 0 ? (int)left : 0;
}

bool gbl_power_q1_set(bool on)
{
	if (!on)
	{
		esp_timer_stop(q1_guard_timer);
		q1_force_off();
		ESP_LOGI(TAG, "Q1 OFF");
		return true;
	}

	int left = gbl_power_cooldown_left_ms();
	if (left > 0)
	{
		ESP_LOGW(TAG, "Rpre1 cooling down, retry in %d ms", left);
		return false;
	}
	q1_force_on();
	ESP_LOGI(TAG, "Q1 ON");
	return true;
}

void gbl_power_relay_set(bool closed)
{
	gpio_set_level(PIN_RELAY, closed);
	ESP_LOGI(TAG, "Relay %s", closed ? "CLOSED" : "OPEN");
}

void gbl_power_off(void)
{
	gbl_power_relay_set(false);
	gbl_power_q1_set(false);
}

/* ------------------------------------------------------------------------- */
/* Sequences                                                                 */
/* ------------------------------------------------------------------------- */

/* Opens everything and logs why, with the voltages that triggered the abort. */
static gbl_precharge_result_t precharge_abort(const char *reason, const gbl_vsens_t *r)
{
	gbl_power_off();
	ESP_LOGE(TAG, "Pre-charge aborted: %s", reason);
	gbl_vsens_log("abort", r);
	return GBL_PRECHARGE_FAULT;
}

/* Charges the PDBs through Q1 + Rpre1 and closes the relay only once the
 * charge is complete.
 *
 * Every SAMPLE_PERIOD_MS the connected PDBs are read, and the sequence aborts
 * (Q1 off, relay open) if:
 *   - a PDB is still below V_MIN_AT_CHECK after SHORT_CHECK_MS  -> short
 *   - the PDBs differ by more than V_MAX_MISMATCH               -> ambiguous charge
 *   - the charge settles below V_MIN_CHARGED                    -> insufficient voltage
 *   - the charge settles above V_MAX_CHARGED                    -> wrong battery config
 *   - the charge does not settle within PRECHARGE_TIMEOUT_MS    -> timeout
 * The charge is complete when, within SETTLE_WINDOW_MS, no PDB rises by more
 * than SETTLE_DV. Vin is not measured: the end of charge is detected from the
 * slope. */
gbl_precharge_result_t gbl_power_precharge(void)
{
	gbl_power_relay_set(false);
	if (!gbl_power_q1_set(true))
		return GBL_PRECHARGE_REFUSED;
	ESP_LOGI(TAG, "Pre-charge started (timeout %d ms)", PRECHARGE_TIMEOUT_MS);

	gbl_vsens_t r, window_start;
	gbl_vsens_read(&r);
	gbl_vsens_log("start", &r);
	window_start = r;

	int64_t t0 = esp_timer_get_time();
	int window_start_ms = 0;

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
		int ms = (esp_timer_get_time() - t0) / 1000;

		// The watchdog already switched Q1 off: the PDBs are no longer fed, never close the relay.
		if (!q1_on)
		{
			gbl_power_relay_set(false);
			return GBL_PRECHARGE_FAULT;
		}

		gbl_vsens_read(&r);
		if (!r.valid)
			return precharge_abort("no PDB enabled", &r);

		if (ms >= SHORT_CHECK_MS && r.min < V_MIN_AT_CHECK)
			return precharge_abort("voltage not rising, possible short or faulty sensor", &r);
		if (ms >= SHORT_CHECK_MS && r.max - r.min > V_MAX_MISMATCH)
			return precharge_abort("ambiguous charge, PDB voltages too far apart", &r);

		if (ms - window_start_ms >= SETTLE_WINDOW_MS)
		{
			bool settled = true;
			for (int ch = 0; ch < GBL_VSENS_CHANNELS; ch++)
				if (gbl_vsens_enabled(ch) && r.volt[ch] - window_start.volt[ch] > SETTLE_DV)
					settled = false;
			window_start = r;
			window_start_ms = ms;

			if (settled && ms >= SHORT_CHECK_MS)
			{
				if (r.min < V_MIN_CHARGED)
					return precharge_abort("charge settled but voltage too low", &r);
				if (r.max > V_MAX_CHARGED)
					return precharge_abort("voltage too high for the configured battery, check menuconfig and the UVLO resistor", &r);
				ESP_LOGI(TAG, "Pre-charge completed in %d ms", ms);
				gbl_vsens_log("charged", &r);
				break;
			}
		}

		if (ms >= PRECHARGE_TIMEOUT_MS)
			return precharge_abort("timeout, charge does not settle", &r);
	}

	gbl_power_relay_set(true);
	// Q1 off once the contacts are closed: if the relay dropped out, the load
	// must not be able to flow through Rpre1 (up to ~11 W at 48 V).
	vTaskDelay(pdMS_TO_TICKS(RELAY_SETTLE_MS));
	gbl_power_q1_set(false);
	return GBL_PRECHARGE_OK;
}

/* Controlled shutdown.
 *
 * Q1 is switched on in parallel with the relay before opening it. When the
 * contacts separate, the PDB output capacitance holds the bus close to Vin and
 * the load current moves to Q1 + Rpre1: almost no voltage appears across the
 * contacts, so no DC arc forms. The actual interruption then happens in Q1,
 * which has no contacts to wear. The Rpre1 cooldown is bypassed: opening the
 * line always takes priority, and the energy involved (at most V²/50 for
 * SHUTDOWN_OVERLAP_MS, ~2.5 J at 6S, ~10 J at 12S) is within the Rpre1
 * single-pulse rating (~25 J). */
void gbl_power_shutdown(void)
{
	ESP_LOGI(TAG, "Controlled shutdown started");
	q1_force_on();
	ESP_LOGI(TAG, "Q1 ON (parallel path)");
	vTaskDelay(pdMS_TO_TICKS(RELAY_SETTLE_MS)); // let the LM5069 turn the gate back on before opening
	gbl_power_relay_set(false);
	vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_OVERLAP_MS));
	gbl_power_q1_set(false);
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

void gbl_power_init(void)
{
	// Safe levels written BEFORE enabling the outputs: no glitch on Q1 or the relay.
	gpio_set_level(PIN_Q1_DISABLE, 1);
	gpio_set_level(PIN_RELAY, 0);

	gpio_config_t cfg = {
		.pin_bit_mask = (1ULL << PIN_Q1_DISABLE) | (1ULL << PIN_RELAY),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&cfg));

	const esp_timer_create_args_t guard_args = {.callback = q1_guard_cb, .name = "q1_guard"};
	ESP_ERROR_CHECK(esp_timer_create(&guard_args, &q1_guard_timer));

	ESP_LOGI(TAG, "Configured for a %dS LiPo, %.1f-%.1f V (UVLO resistor %s)", CELLS, V_MIN_CHARGED,
			 V_MAX_CHARGED, CELLS <= 6 ? "100k must be SOLDERED" : "must be REMOVED");
}
