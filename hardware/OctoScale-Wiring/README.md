# OctoScale wiring schematic

KiCad 10 project for the wiring diagram in the [Assembly Guide](../../docs/Assembly-Guide.md).
It documents how the modules are wired together; there is no PCB, because the
build is point-to-point wiring between ready-made boards.

## Files

| File | Purpose |
| --- | --- |
| `OctoScale-Wiring.kicad_pro` | project file — open this in KiCad |
| `OctoScale-Wiring.kicad_sch` | the schematic |
| `OctoScale-Modules.kicad_sym` | module symbols (see below) |

## Why a local symbol library

KiCad's stock libraries carry the bare *chips*: `Analog_ADC:HX711` has sixteen
pins named `BASE`, `VFB`, `XI`, `XO` and `RATE`. The board in the BOM is a
breakout with eight labelled holes — `E+ E- A- A+` and `VCC GND DT SCK`. Someone
holding that board cannot match it against the chip symbol without a datasheet.

So every module here is drawn with the pin names printed on its own silkscreen.
Stock symbols are used where they already match what the builder sees:
`Device:Buzzer`, `Device:C_Polarized` and the `power` symbols.

## Checks

The schematic passes ERC with no errors and no warnings:

```bash
kicad-cli sch erc --severity-all --output erc.rpt OctoScale-Wiring.kicad_sch
```

ERC only proves the drawing is self-consistent. That the nets match the firmware
is a separate question, so the netlist is checked against the pin constants in
`src/main.cpp`:

```bash
kicad-cli sch export netlist --output net.net OctoScale-Wiring.kicad_sch
```

All nineteen signal nets, plus the `+5V`, `+3V3` and `GND` rails, were verified
against `src/main.cpp` this way. When a pin assignment changes in the firmware,
update `generate_schematic.py` and re-run that comparison.

## Exports

Images used by the wiki live in `assets/wiring/`:

```bash
kicad-cli sch export pdf --output ../../assets/wiring/OctoScale-Wiring.pdf OctoScale-Wiring.kicad_sch
kicad-cli sch export svg --no-background-color --output ../../assets/wiring OctoScale-Wiring.kicad_sch
```

The PNG in that folder is rendered from the PDF; the wiki embeds the PNG because
GitHub does not render SVG reliably inside `<img>`.
