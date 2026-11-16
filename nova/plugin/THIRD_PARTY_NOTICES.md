# Nova Modular: third-party notices

Nova Modular is licensed under the GNU General Public License v3.0 or later
(see `LICENSE.md`). It incorporates the following material.

## 1. BrickRack

https://github.com/andrewfrangella/brickrack (commit 49b726c, v0.1.0),
Copyright (C) 2026 BrickRack contributors, GPL-3.0-or-later.

`src/br/core/` and `src/br/modules/` are BrickRack's module framework and five
of its modules, unmodified: the wrappers that drive the firmware DSP the way
the hardware does, the digital models of the analog modules, and the panel
layouts. `src/br/ui/canvas.h` replaces BrickRack's software rasteriser with the
same interface on NanoVG. BrickRack's own notices are reproduced in
`THIRD_PARTY_NOTICES.brickrack.txt`.

## 2. Eurorack module firmware by Emilie Gillet

https://github.com/pichenettes/eurorack (commit f4cfa76) and
https://github.com/pichenettes/stmlib (commit e3bd7c9), in `eurorack/`.

Only the files the five modules compile are included: the DSP code and the
lookup tables of Plaits, Beads and Peaks, the tables of Edges, and stmlib.

- STM32 firmware (Plaits, Beads, Peaks) and stmlib: MIT License, reproduced
  below.
- AVR firmware (Edges), whose tables are included and whose algorithms are
  reproduced in BrickRack's wrapper: GNU GPL v3.0 or later, Copyright Emilie
  Gillet.
- Hardware designs, the reference for the model of Blades: CC-BY-SA-3.0,
  Emilie Gillet.

One change was made to these files: in
`eurorack/beads/dsp/wavetable_granular_player.h`, a debugging `printf` that the
`TEST` configuration enables in the audio path was removed. (BrickRack does the
same with a copy made at build time.)

"Mutable Instruments" is a registered trademark. Nova Modular is not
affiliated with or endorsed by Mutable Instruments.

```
Copyright 2012 Emilie Gillet.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## 3. Barlow Condensed

`res/fonts/BarlowCondensed-SemiBold.ttf`, Copyright 2017 The Barlow Project
Authors, SIL Open Font License 1.1 (`res/fonts/OFL.txt`).

## 4. VCV Rack

The plugin is built against the VCV Rack 2 API (https://vcvrack.com),
GPL-3.0-or-later, and uses its component library for the jacks.
