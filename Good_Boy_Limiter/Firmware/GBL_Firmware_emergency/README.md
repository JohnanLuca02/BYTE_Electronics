# GBL EMERGENCY firmware (temporary)

> **Not the production firmware.** Use only on the current board revision,
> only while the robot runs with the motors connected, and only until the
> pre-charge stage is redesigned. Then delete this folder and flash
> `../GBL_Firmware`.

## Why it exists

With the 12 GDS68 drivers connected, the drivers draw more power during the
pre-charge than Rpre1 (50 Ω) can deliver (~2.9 W at 24 V). The charge stalls
at a few volts and the production firmware correctly refuses to close the relay.

## What it does differently

Only the pre-charge (`emergency_precharge()` in `main/gbl_run_emergency.c`).
Everything else (switch handling, Jetson shutdown handshake, logging, Q1
watchdog and cooldown, controlled shutdown) is the production code: the
drivers in `components/` are a frozen copy of the production ones (nothing is
shared: changes in `../GBL_Firmware` do not reach this project).

| | Production | Emergency |
|---|---|---|
| Charge stalls or times out | abort | **relay closes anyway** |
| Timeout | 3 s | 1.5 s (a stall dissipates ~6.5 W in Rpre1) |
| Short circuit (bus below threshold after 300 ms) | abort (< 5 V) | abort (< 3 V) |
| PDB mismatch, voltage above battery max | abort | abort |
| Bus not above 19.2 V within 1 s of closing the relay | - | abort (relay not closed, empty battery, short) |

Battery fixed to **6S** (100k UVLO resistor fitted).

## Risk

The remaining voltage step (e.g. 24 V - 6 V) charges the capacitors through
the relay contacts, limited only by wiring and capacitor ESR: contact wear and
possible welding. A welded relay can no longer be opened by the GBL, so keep
the hardware emergency stop and the battery connector at hand.

## Build and flash

```
cd Good_Boy_Limiter/Firmware/GBL_Firmware_emergency
idf.py build
idf.py -p COM3 flash monitor
```

The boot log shows `*** GBL EMERGENCY firmware (TEMPORARY) ... ***` and each
pre-charge logs `EMERGENCY pre-charge started` and the voltage at which the
relay closed. To go back, flash `../GBL_Firmware`. The stored log in flash is
kept across both firmwares (same partition table).

## Welded relay detection

At every switch OFF, while the Jetson is still running, the relay opens for
50 ms (Q1 in parallel) and closes again. With the motors and the Jetson
connected the bus drops by several volts; if it does not move, the contacts
are welded:

1. the GBL logs `RELAY WELDED?` and sends `GBL:RELAY_WELDED` to the Jetson
   (instead of `GBL:SHUTDOWN`);
2. the Jetson runs `GBL_MOTORS_OFF_CMD` (motors torque-off, to be set in
   `gbl-link.service`) and halts;
3. after the normal shutdown, if the bus is still at battery level after 3 s,
   the GBL logs `RELAY WELDED: ...` every 10 s.

Then: robot in a safe position, motors off, **disconnect the battery**
(only the driver logic, ~1 A, is flowing).

On the bench without load the bus hardly drops when the relay opens: expect
false `RELAY WELDED?` messages there. They only change the message sent to the
Jetson; the shutdown is the same.
