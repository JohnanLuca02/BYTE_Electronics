/*
 * GBL - PDB voltage sensing
 *
 * Vsens1..4 = GPIO4, GPIO6 (ADC1), GPIO15, GPIO17 (ADC2).
 * Each input has a 100k / 4.7k divider with a 100 nF filter and BAT54S clamps
 * (see "Voltage Sensing Modules" sheet): 48 V -> 2.15 V at the pin, full scale
 * ≈ 69 V with 12 dB attenuation. ADC2 is usable because Wi-Fi is not running.
 */

#include "gbl_vsens.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"

#define DIVIDER_RATIO ((100.0f + 4.7f) / 4.7f)
#define OVERSAMPLE 16				 // samples averaged per reading
#define FALLBACK_FULL_SCALE_MV 3100 // linear conversion used only without eFuse calibration

static const char *TAG = "vsens";

static const adc_unit_t ch_unit[GBL_VSENS_CHANNELS] = {ADC_UNIT_1, ADC_UNIT_1, ADC_UNIT_2, ADC_UNIT_2};
static const adc_channel_t ch_adc[GBL_VSENS_CHANNELS] = {ADC_CHANNEL_3, ADC_CHANNEL_5, ADC_CHANNEL_4, ADC_CHANNEL_6};

/* Per-channel gain correction: V_multimeter / V_read, measured with the debug
 * firmware ('V' command). Covers divider tolerance and residual ADC error. */
static const float ch_gain[GBL_VSENS_CHANNELS] = {1.0f, 1.0f, 1.0f, 1.0f};

/* PDBs connected to the pre-charge line. Disabled channels read ~0 V and
 * would otherwise be mistaken for a short. */
static const bool ch_enabled[GBL_VSENS_CHANNELS] = {true, true, true, true}; // PDB1, PDB2, PDB3, PDB4

static adc_oneshot_unit_handle_t unit_handle[2];
static adc_cali_handle_t cali_handle[GBL_VSENS_CHANNELS];

void gbl_vsens_init(void)
{
	for (int u = 0; u < 2; u++)
	{
		adc_oneshot_unit_init_cfg_t cfg = {.unit_id = u == 0 ? ADC_UNIT_1 : ADC_UNIT_2};
		ESP_ERROR_CHECK(adc_oneshot_new_unit(&cfg, &unit_handle[u]));
	}

	adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
	for (int ch = 0; ch < GBL_VSENS_CHANNELS; ch++)
	{
		ESP_ERROR_CHECK(adc_oneshot_config_channel(unit_handle[ch_unit[ch]], ch_adc[ch], &chan_cfg));

		adc_cali_curve_fitting_config_t cali_cfg = {
			.unit_id = ch_unit[ch],
			.chan = ch_adc[ch],
			.atten = ADC_ATTEN_DB_12,
			.bitwidth = ADC_BITWIDTH_DEFAULT,
		};
		if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle[ch]) != ESP_OK)
		{
			cali_handle[ch] = NULL;
			ESP_LOGW(TAG, "PDB%d: no eFuse calibration, using linear conversion", ch + 1);
		}
	}
}

/* Voltage of one channel in volts: averaged raw reading -> eFuse-calibrated
 * millivolts at the pin -> divider ratio -> per-channel gain. Returns -1 on read failure: a negative
 * value always fails the pre-charge checks, so a faulty input can never allow
 * the relay to close. */
static float read_channel(int ch)
{
	int sum = 0;
	for (int k = 0; k < OVERSAMPLE; k++)
	{
		int raw;
		if (adc_oneshot_read(unit_handle[ch_unit[ch]], ch_adc[ch], &raw) != ESP_OK)
			return -1.0f;
		sum += raw;
	}

	int raw = sum / OVERSAMPLE;
	int mv;
	if (!cali_handle[ch] || adc_cali_raw_to_voltage(cali_handle[ch], raw, &mv) != ESP_OK)
		mv = raw * FALLBACK_FULL_SCALE_MV / 4095;
	return mv / 1000.0f * DIVIDER_RATIO * ch_gain[ch];
}

void gbl_vsens_read(gbl_vsens_t *out)
{
	out->valid = false;
	for (int ch = 0; ch < GBL_VSENS_CHANNELS; ch++)
	{
		float v = read_channel(ch);
		out->volt[ch] = v;
		if (!ch_enabled[ch])
			continue;
		if (!out->valid || v < out->min)
			out->min = v;
		if (!out->valid || v > out->max)
			out->max = v;
		out->valid = true;
	}
}

bool gbl_vsens_enabled(int ch)
{
	return ch >= 0 && ch < GBL_VSENS_CHANNELS && ch_enabled[ch];
}

void gbl_vsens_log(const char *tag, const gbl_vsens_t *r)
{
	ESP_LOGI(TAG, "%s: PDB1=%.2fV PDB2=%.2fV PDB3=%.2fV PDB4=%.2fV", tag,
			 r->volt[0], r->volt[1], r->volt[2], r->volt[3]);
}
