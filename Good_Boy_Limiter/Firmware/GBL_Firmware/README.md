# GBL Firmware

Firmware for the **Good Boy Limiter** (ESP32-S3). It powers the PDB line
without inrush current: the PDB capacitors are pre-charged through a resistor,
then a relay bypasses it. It also handles the main power switch and a
controlled shutdown that does not arc the relay contacts.

Toolchain: **ESP-IDF v5.5.4**.

---

## Quick start

From an ESP-IDF terminal, inside this folder:

```
idf.py build
idf.py -p COM3 flash monitor
```

Exit the monitor with `Ctrl+]`. In VS Code (ESP-IDF extension), open **this
folder** (`GBL_Firmware`), not `Firmware/`, otherwise CMake finds no project.

Before the first power-up, check that menuconfig matches the battery and the
UVLO resistor on the board (see [Battery pack](#battery-pack-and-uvlo-resistor)).

---

## How it works

### Power path

```
+48VIN --Rsns1--[Q1]--Rpre1 50R--+--> +48VOUT (PDB capacitors, ~8 mF with 4 PDBs)
                 ^ LM5069         |
+48VIN ----------[relay contacts]-+
```

| Element | Driven by | Role |
|---|---|---|
| Q1 (STB15810) | LM5069 gate; the ESP32 enables/disables it via UVLO through Q2 (**GPIO2 = 1 -> Q1 off**) | pre-charge path through Rpre1 |
| Relay | Q4 low side (**GPIO38 = 1 -> closed**) | low-impedance path while running |
| Vsens1..4 | ADC on GPIO4, GPIO6, GPIO15, GPIO17 (100k / 4.7k divider) | voltage of each PDB |
| Power switch | GPIO1 (J2 pin 9) to GND (J2 pin 10), internal pull-up | ON / OFF request |

### Pre-charge (`gbl_power_precharge`)

1. Relay open, Q1 on: the PDBs charge through Rpre1 (τ = 50 Ω · C ≈ 0.4 s).
2. Every 20 ms all enabled PDBs are read. The sequence **aborts** (everything off) if:
   - a PDB is still below 5 V after 300 ms: short or faulty sensor;
   - the PDBs differ by more than the allowed spread: a PDB is not charging (ambiguous charge);
   - the charge settles below the battery minimum or above its maximum;
   - the charge does not settle within 3 s.
3. The charge is complete when no PDB rises by more than 0.1 V in 200 ms.
4. Relay closed, then after 100 ms Q1 off.

### Controlled shutdown (`gbl_power_shutdown`)

Opening a DC relay under load draws an arc. So: Q1 on in parallel with the
relay, relay open (the current moves to Q1 + Rpre1, almost no voltage across
the contacts), and after 200 ms Q1 off. The MOSFET does the actual interruption.

### Rpre1 protection

Rpre1 is the thermally critical part (10 W, ~25 J single pulse). Independently
of the application code:

- **Watchdog:** Q1 is switched off after 3.5 s whatever happens (an `esp_timer`
  armed *before* Q1 turns on).
- **Cooldown:** 10 s between two Q1 switch-ons.
- The pre-charge aborts on a short, so Rpre1 never sees V²/50 for long.

### Production application (`gbl_run.c`)

```
WAIT_SWITCH_OFF --switch OFF--> OFF --switch ON--> pre-charge --ok--> ON
       ^                         ^                     |               |
       +---------- fault --------|---------------------+  switch OFF   |
       +---- undervoltage in ON -|-------------------------------------+
                                 |                                     v
                                 +--- bus < 5 V (or switch ON) --- STOPPING
```

- Switch already ON at boot: nothing starts until it is cycled OFF -> ON
  (a reset never re-energises the line on its own).
- After a fault the line stays off until the switch is turned OFF: no automatic retry.
- In STOPPING the line is off; OFF is reached once the bus is below 5 V,
  i.e. when unplugging the PDBs is safe.
- The power switch is **not** the emergency stop, which must cut the relay
  coil in hardware.

### Debug application (`gbl_debug.c`)

Manual control from the monitor. The Rpre1 protections stay active.

| Key | Action |
|---|---|
| `S` | Full pre-charge (Q1 + checks + relay) |
| `X` | Controlled shutdown |
| `s` / `0` | Immediate power-off |
| `P` / `p` | Q1 on / off (watchdog 3.5 s) |
| `R` / `r` | Relay on / off (**no protection**: closing on empty PDBs = full inrush) |
| `V` | PDB voltages and switch state |
| `?` | Help |

Switch changes are logged automatically, which is handy to check the wiring.

---

## Code structure

```
GBL_Firmware/
  CMakeLists.txt              ESP-IDF project
  sdkconfig.defaults          shared settings (tracked by git)
  main/                       application layer
    main.c                    app_main(): init drivers, start the selected application
    gbl_run.c / .h            production state machine
    gbl_debug.c / .h          debug console
    Kconfig.projbuild         "GBL firmware" menu in menuconfig
  components/gbl_hw/          hardware drivers (no application logic)
    gbl_power.c               Q1, relay, pre-charge and shutdown sequences, Rpre1 protection
    gbl_vsens.c               PDB voltage sensing (ADC, calibration)
    gbl_switch.c              power switch input (debounced)
    include/*.h               public API of each driver
```

Layering rules:

- `components/gbl_hw` only knows the hardware. Every safety-critical rule
  (watchdog, cooldown, pre-charge checks) lives there, so no application can
  bypass it.
- `main/` decides **when** to call the drivers. Both applications are always
  compiled; `main.c` starts the one selected in menuconfig.
- Headers expose only types and functions. Pins, timings and thresholds are
  `#define`s at the top of each `.c`.
- Code, comments and logs in English. Logs go through `ESP_LOG*` (tags
  `power`, `vsens`, `run`, `debug`).

---

## Configuration (menuconfig)

```
idf.py menuconfig
```

or in VS Code: `Ctrl+Shift+P` -> **ESP-IDF: SDK Configuration Editor (menuconfig)**.

Open **GBL firmware**. Arrow keys to move, Enter to select, `?` for help,
then `Q` and `Y` to save. Rebuild and flash after every change.

| Option | Values | Default |
|---|---|---|
| Application | Production (switch + state machine) / Test-debug (console) | Production |
| Battery pack (LiPo) | 6S / 12S / 13S | 6S |

`sdkconfig` is personal and not tracked by git. The shared settings are in
**`sdkconfig.defaults`**: when `sdkconfig` does not exist (fresh clone, or
after deleting it) ESP-IDF generates it from that file. Local menuconfig
changes stay local; to change a default for everyone, edit
`sdkconfig.defaults`. If a build behaves strangely after a pull, delete
`sdkconfig` and rebuild.

### Battery pack and UVLO resistor

> **Change the hardware BEFORE flashing a different battery configuration.**

The LM5069 UVLO threshold is set by R31 143k / R30 10k (Q1 enabled above
~38.3 V), which is too high for a 6S pack. An extra resistor between +48VIN
and UVLO (TP9-TP2), in parallel with R31, lowers it.

| Pack | Voltage range | UVLO resistor (TP9-TP2) | Q1 enabled / disabled |
|---|---|---|---|
| 6S  | 18.0 - 25.2 V | **100k soldered** | ~17.2 V / ~16.0 V |
| 12S | 36.0 - 50.4 V | **removed** | ~38.3 V / ~35.3 V |
| 13S | 39.0 - 54.6 V | **removed** | ~38.3 V / ~35.3 V |

- 6S without the resistor: Q1 never turns on and every pre-charge fails.
- 12S/13S with the resistor: the undervoltage protection is lost.
- 12S nearly empty (< ~38.3 V): the pre-charge does not start. This is intended.
- 13S: a full pack (54.6 V) is close to the OVLO threshold (~55 V, R32 210k / R29 10k).
  Raise R32 to ~220k (OVLO ~57.5 V) before using 13S.

The firmware derives its thresholds from the cell count:

| Threshold | Per cell | 6S | 12S | 13S |
|---|---|---|---|---|
| Minimum voltage to accept a pre-charge (pack at rest) | 3.2 V | 19.2 V | 38.4 V | 41.6 V |
| Maximum voltage (wrong pack or wrong config) | 4.35 V | 26.1 V | 52.2 V | 56.6 V |
| Minimum voltage while ON (under load, for 100 ms) | 3.0 V | 18 V | 36 V | 39 V |
| Maximum spread between PDBs | 0.15 V | 0.9 V | 1.8 V | 2.0 V |

The boot log shows the active configuration, e.g.
`Configured for a 6S LiPo, 19.2-26.1 V (UVLO resistor 100k must be SOLDERED)`.

---

## Modifying the code

| To change | Where |
|---|---|
| Connected PDBs | `ch_enabled[]` in `components/gbl_hw/gbl_vsens.c`. A disabled channel is still read but ignored by the checks, so a PDB on it is **not protected**. |
| Voltage calibration | `ch_gain[]` in `gbl_vsens.c`. With the debug firmware, close the line (`S`), press `V`, measure the bus with a multimeter: `gain = V_multimeter / V_read` per channel. |
| Pre-charge timings and checks | `#define`s at the top of `gbl_power.c`. They assume ~8 mF of PDB capacitance: with much more (> ~20 mF) raise `SHORT_CHECK_MS` and `PRECHARGE_TIMEOUT_MS`, and check the Rpre1 energy (½·C·V² per pre-charge, max ~25 J). |
| Per-cell voltage limits | `CELL_V_*` in `gbl_power.c`, `V_MIN_ON` in `gbl_run.c`. |
| Production behaviour | state handlers `on_*()` in `gbl_run.c`. |
| Pins | `PIN_*` at the top of `gbl_power.c`, `gbl_switch.c`; ADC channels in `gbl_vsens.c`. |
| New menuconfig option | `main/Kconfig.projbuild` (read it in C as `CONFIG_<NAME>`, include `sdkconfig.h`); add it to `sdkconfig.defaults` if the default must be shared. |
| New driver | new `.c` in `components/gbl_hw/`, header in `include/`, add it to `SRCS` in `components/gbl_hw/CMakeLists.txt`. |

After any change to `gbl_power.c` or `gbl_vsens.c`, re-test on the bench with
the debug firmware (`S`, `X`, `V`) before using the production firmware.

---

## Hardware notes

- **Rshort1 must not be fitted (DNP).** With TIMER shorted to GND the LM5069
  never ends its insertion time and Q1 never turns on.
- No pull resistor on the Q2 gate: until the ESP32 boots, Q1 can enable itself
  ~0.7 s after VIN appears. Next revision: 100k pull-up from GPIO2 to 3V3.
- Optional 1k in series on the switch wire (GPIO1), as protection if the
  wiring touches a higher voltage.
