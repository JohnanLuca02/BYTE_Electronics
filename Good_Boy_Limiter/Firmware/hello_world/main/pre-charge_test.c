#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

// Pin dallo schema elettrico della GBL Board
#define PIN_PRECHARGE_CTRL GPIO_NUM_2 // GPIO2 -> Gate di Q2 (AO3422) su pin UVLO di LM5069
#define PIN_RELAY_CTRL GPIO_NUM_38	  // GPIO38 -> Gate di Q4 (AO3400A) per il Relè
#define PIN_USER_BTN GPIO_NUM_1		  // GPIO1 -> Pulsante utente (attivo LOW)

// Helper per il MOSFET di potenza Q3 (Logica INVERTITA tramite Q2 su UVLO)
// enable = true  -> GPIO2 = 0 (Q2 OFF -> UVLO libero -> Q3 ON)
// enable = false -> GPIO2 = 1 (Q2 ON  -> UVLO a GND  -> Q3 OFF)
static void set_power_mosfet(bool enable)
{
	if (enable)
	{
		gpio_set_level(PIN_PRECHARGE_CTRL, 0);
		printf("[POWER MOSFET Q3] -> CHIUSO / ATTIVO (GPIO2 = LOW)\n");
	}
	else
	{
		gpio_set_level(PIN_PRECHARGE_CTRL, 1);
		printf("[POWER MOSFET Q3] -> APERTO / DISATTIVO (GPIO2 = HIGH)\n");
	}
}

// Helper per il Relè Q4 (Logica ATTIVA ALTA)
// close_relay = true  -> GPIO38 = 1 (Q4 ON  -> Relè CHIUSO)
// close_relay = false -> GPIO38 = 0 (Q4 OFF -> Relè APERTO)
static void set_relay(bool close_relay)
{
	if (close_relay)
	{
		gpio_set_level(PIN_RELAY_CTRL, 1);
		printf("[RELAY Q4]        -> CHIUSO / ATTIVO (GPIO38 = HIGH)\n");
	}
	else
	{
		gpio_set_level(PIN_RELAY_CTRL, 0);
		printf("[RELAY Q4]        -> APERTO / DISATTIVO (GPIO38 = LOW)\n");
	}
}

// Pre-carica: Q1 ON (carica Cout tramite Rpre1), poi relè. Il relè non va mai chiuso con Cout scarico.
// Il tempo di attesa serve alla carica di Cout attraverso Rpre1 (tau = 50ohm * C_carico).
#define PRECHARGE_MS 2000 // ponytail: attesa fissa, sostituire con lettura ADC di Vout ≈ Vin
static void precharge_sequence(void)
{
	set_relay(false);
	set_power_mosfet(true);
	printf("[SEQUENZA] Pre-carica tramite Q1 + Rpre1 (%d ms)...\n", PRECHARGE_MS);
	vTaskDelay(pdMS_TO_TICKS(PRECHARGE_MS));
	set_relay(true);
	// Relè chiuso: Q1 off, così se il relè si apre il carico non passa da Rpre1 (fino a ~11W a 48V)
	vTaskDelay(pdMS_TO_TICKS(100)); // tempo di chiusura + rimbalzo contatti
	set_power_mosfet(false);
}

void app_main(void)
{
	// 1. Imposta i livelli logici sicuri PRIMA di abilitare gli output
	gpio_set_level(PIN_PRECHARGE_CTRL, 1); // HIGH = Tira UVLO a GND -> Q3 SPENTO
	gpio_set_level(PIN_RELAY_CTRL, 0);	   // LOW  = Q4 SPENTO -> Relè APERTO

	// 2. Configura GPIO2 e GPIO38 come Output
	gpio_config_t out_conf = {
		.pin_bit_mask = (1ULL << PIN_PRECHARGE_CTRL) | (1ULL << PIN_RELAY_CTRL),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE};
	gpio_config(&out_conf);

	// 3. Configura GPIO1 (Pulsante) come Input
	gpio_config_t btn_conf = {
		.pin_bit_mask = (1ULL << PIN_USER_BTN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE};
	gpio_config(&btn_conf);

	printf("\n=== GBL Board Test (ESP-IDF) ===\n");
	printf("Comandi da terminale: 'P'=Q3 ON, 'p'=Q3 OFF, 'R'=Relay ON, 'r'=Relay OFF, 'S'=Sequenza, '0'=STOP\n");
	printf("Oppure premi il pulsante su GPIO1 per fare toggle di Relay + MOSFET!\n\n");

	bool toggle_state = false;
	int last_btn = 1;

	while (1)
	{
		// Lettura comandi da console seriale / USB
		int c = getchar();
		if (c != EOF)
		{
			switch ((char)c)
			{
			case 'P':
				set_power_mosfet(true);
				break;
			case 'p':
				set_power_mosfet(false);
				break;
			case 'R':
				set_relay(true);
				break;
			case 'r':
				set_relay(false);
				break;
			case '0':
				set_relay(false);
				set_power_mosfet(false);
				break;
			case 'S':
			case 's':
				precharge_sequence();
				break;
			default:
				break;
			}
		}

		// Controllo anche tramite pulsante fisico su GPIO1
		int btn = gpio_get_level(PIN_USER_BTN);
		if (btn == 0 && last_btn == 1)
		{
			toggle_state = !toggle_state;
			printf("\n[PULSANTE GPIO1] Stato -> %s\n", toggle_state ? "ATTIVO" : "SPENTO");
			if (toggle_state)
				precharge_sequence();
			else
			{
				set_relay(false);
				set_power_mosfet(false);
			}
			vTaskDelay(pdMS_TO_TICKS(250)); // Debounce
		}
		last_btn = btn;

		vTaskDelay(pdMS_TO_TICKS(20));
	}
}