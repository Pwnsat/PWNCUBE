# PWNCUBE hardware — KiCad schematics and PCB design files

Design files for the three physical boards that make up a PWNCUBE (see
[Hardware Anatomy](https://github.com/Pwnsat/PWNCUBE/wiki/Hardware-Anatomy)
in the wiki for how they connect and what runs on each).

| Folder | Board | Contains |
|---|---|---|
| `PWNSAT/` | Main / flight-computer board | RV1106 SoC, radios, USB selector, power supply |
| `Sensor_Board/` | Sensor board | BME280 + ICM-42670, sensor connectors |
| `Battery_Board/` | Power/battery board | Battery holder, USB, battery connector |

Each board folder is a self-contained KiCad project: `.kicad_pro` (project),
`.kicad_sch` (schematic sheets), `.kicad_pcb` (PCB layout), `.pretty/`
(custom footprint library), and `3D/` (3D models for parts used in that
board's layout, `.step`/`.stp`/`.igs`).

## Opening a project

Requires [KiCad](https://www.kicad.org/) (developed against KiCad 7/8).
Open the `.kicad_pro` file in each board's folder — schematic and PCB editors
are reachable from there.

## Generating manufacturing files (gerbers)

`electroniccats_pcb.kibot.yaml` and `electroniccats_sch.kibot.yaml` are
[KiBot](https://github.com/INTI-CMNB/KiBot) configs for JLCPCB-compatible
output (gerbers + drill files, schematic PDF). With KiBot installed:

```bash
kibot -c electroniccats_pcb.kibot.yaml -e <board>/<board>.kicad_sch -b <board>/<board>.kicad_pcb
```
