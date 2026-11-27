# BYTE_Electronics

Everything concerning the electronics (PCBs, electrical schemes, BOMs, firmware,
documentation) of **BYTE**, the quadruped robot dog of the AiTeam.

---

## Repository map

Each top-level folder is one board or one system-level scheme. Every folder is
self-contained: it carries its own libraries, documentation and outputs.

| Folder | What it is | Tool | Where to start |
|---|---|---|---|
| [`Good_Boy_Limiter/`](Good_Boy_Limiter) | **GBL** — inrush limiter / main power switch between the battery and the PDBs (LM5069 pre-charge + relay bypass, ESP32-S3) | KiCad | [`GBL_Documentation/GBL_Documentation.pdf`](Good_Boy_Limiter/GBL_Documentation/GBL_Documentation.pdf) |
| [`BYTE_SPIne/`](BYTE_SPIne) | **SPIne** — communication backbone between the Jetson Orin Nano and the 12 joint actuators (USB/SPI → CAN, STM32F072) | Altium | [`Documentation/BYTE_SPIne_Documentation.pdf`](BYTE_SPIne/Documentation/BYTE_SPIne_Documentation.pdf) |
| [`BYTE_Rib/`](BYTE_Rib) | **Rib** — breakout of the Jetson signals (I2C, UART, I2S, GPIO) from the SPIne P3 interface, with ESD protection | Altium | [`Documentation/BYTE_Rib_Documentation.pdf`](BYTE_Rib/Documentation/BYTE_Rib_Documentation.pdf) |
| [`PDBueno/`](PDBueno) | **PDBueno** — power distribution board (2×6S batteries, 24 V / 48 V, buck converter) | KiCad (originally EasyEDA) | [`PDBueno.pdf`](PDBueno/PDBueno.pdf) |
| [`BYTE_Power_Scheme/`](BYTE_Power_Scheme) | System-level power scheme of the whole robot: battery → GBL → PDBs → Jetson, 12 V buck, SPIne | KiCad | [`BYTE_Power_Scheme.pdf`](BYTE_Power_Scheme/BYTE_Power_Scheme.pdf) |
| [`BYTE_TestLeg_Power_Scheme/`](BYTE_TestLeg_Power_Scheme) | Power scheme of the single-leg test bench | KiCad | [`BYTE-Test Leg_Power_Scheme.pdf`](BYTE_TestLeg_Power_Scheme/BYTE-Test%20Leg_Power_Scheme.pdf) |

> **Quick look without CAD tools:** every folder has a PDF export of the
> schematic. Open that first.

### How the boards fit together

```
Battery ──► GBL (pre-charge + relay) ──► PDBueno (×N) ──► motor drivers (12 actuators)
                                              │
                                              └─► 12 V buck ──► Jetson Orin Nano
                                                                   │
                                     SPIne (USB/SPI → 4× CAN) ◄────┤
                                     Rib (I2C/UART/I2S/GPIO)  ◄────┘ via SPIne P3
```

The full wiring (cable gauges, connectors, fuses) is in
[`BYTE_Power_Scheme`](BYTE_Power_Scheme), one sheet per board
(`gbl`, `pdb`, `jetson`, `bytespine`, `buck_12V`, `bbm`, `bbp`).

---

## Inside each folder

### Good_Boy_Limiter (KiCad)

```
Good_Boy_Limiter/
├── Good_Boy_Limiter.kicad_pro/.kicad_sch/.kicad_pcb   main project
├── inrush_controller.kicad_sch, 3V3_selector.kicad_sch  sub-sheets
├── Libraries/LCSC_Library/        project-local symbols, footprints, 3D (easyeda2kicad)
├── AIT-Logo.pretty/               logo footprint
├── GBL_Documentation/             LaTeX report (main.tex → GBL_Documentation.pdf), datasheets, images
├── SPICE/                         LTspice simulation of the LM5069 pre-charge
└── Firmware/
    ├── GBL_Firmware/              production firmware (ESP-IDF) — see its README
    └── GBL_Firmware_emergency/    TEMPORARY firmware for GBL v1 on the robot — see its README
```

> ⚠️ **GBL v1 on the robot runs the emergency firmware and must NOT be powered
> at 48 V (12S/13S)** until GBL 2.0 is installed. Details in the GBL
> documentation (section "Required Changes for GBL 2.0") and in
> [`GBL_Firmware_emergency/README.md`](Good_Boy_Limiter/Firmware/GBL_Firmware_emergency/README.md).

### BYTE_SPIne (Altium)

```
BYTE_SPIne/
├── BYTE_SPIne.PrjPcb              Altium project (open this)
├── *.SchDoc / BYTE_SPIne.PcbDoc   schematic sheets (top, CAN_channel, USB_Communication) and PCB
├── Libraries/                     project-local SchLib/PcbLib/IntLib, 3D models, logos
├── Stackup/                       JLCPCB 4-layer stackup
├── SI_Models/                     IBIS models used for signal integrity
├── Documentation/                 LaTeX reports (design + SI simulation), schematic PDF, Draftsman drawings (stackup, assembly, 3D view), datasheets, SI data/figures
├── Firmware/                      candleLight (gs_usb) patch for the STM32
├── Validation/                    DRC/ERC reports
├── Manufacturing/                 current outputs: Gerber, NC drill, BOM, pick & place
└── Releases/v1.0-ordered/         frozen files actually sent to the fab
```

Status: **USB mode validated, SPI mode not yet tested** (see the documentation).

### BYTE_Rib (Altium)

```
BYTE_Rib/
├── BYTE_Rib.PrjPcb                Altium project (open this)
├── BYTE_Rib.SchDoc / .PcbDoc      schematic and PCB; BYTE_Rib.pdf is the schematic export
├── Libraries/                     project-local SchLib/PcbLib (copy of the SPIne ones), connector IntLibs, logos
├── Documentation/                 short LaTeX report (BYTE_Rib_Documentation.tex → .pdf)
├── Manufacturing/                 current outputs: Gerber, NC drill, BOM, pick & place
└── Releases/v1.0-ordered/         frozen files actually sent to the fab
```

Status: **v1.0 ordered, not yet tested**.

### PDBueno (KiCad)

`PDBueno.kicad_pro` is the working project, with its local libraries
(`Power_compo_Byte.kicad_sym`, `Footprint_PB_Byte.pretty`).
`EasyEDA_PDBueno/` is the original EasyEDA source and `Imported PDBueno/` the
Altium import; both are kept for reference only.

---

## Conventions

- **One folder per board, everything inside it.** Libraries are project-local
  and referenced with `${KIPRJMOD}` (KiCad) or relative paths (Altium); never
  use global library tables or absolute paths.
- **Always export a PDF** of the schematic next to the project when you change it.
- **Released = frozen.** Files sent to the fab go in `Releases/<version>/` and
  are not modified afterwards; `Manufacturing/` holds the latest outputs.
- **Not tracked by git** (see `.gitignore`): KiCad backups (`*-backups`),
  `*.kicad_prl`, `fp-info-cache`, Altium `History/`, `__Previews/`,
  `Project Logs*/`, `Project Outputs*/`, LaTeX build files.
  The root `.gitignore` also ignores `*.csv` and `*.xml`, except BOM and pick &
  place files (names containing `BOM`, `Bill of Materials` or `Pick Place`).
- **Commit messages:** English, [Conventional Commits](https://www.conventionalcommits.org/)
  with the board as scope, e.g. `feat(gbl): ...`, `docs(spine): ...`,
  `fix(pdbueno): ...`.

### Tools

| Tool | Used for |
|---|---|
| KiCad 9 | GBL, PDBueno, power schemes |
| Altium Designer | SPIne, Rib |
| ESP-IDF v5.5 | GBL firmware |
| LTspice | GBL pre-charge simulation |
| LaTeX (latexmk) | board reports — build outside the repo (`latexmk -pdf -outdir=<tmp>`) and copy only the PDF back |

### Adding an LCSC part (easyeda2kicad)

```
easyeda2kicad --full --lcsc_id [LCSC CODE] --output Good_Boy_Limiter/Libraries/LCSC_Library/easyeda2kicad
```

For another KiCad board, point `--output` to that board's own `Libraries/` folder.
