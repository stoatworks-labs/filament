# Attributions

Filament is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

PROVISIONAL, hand-written in the shape the backend's `scripts/sync-attributions.py`
generates (graticule's recipe). Filament is not yet registered in the website's
lists, so the sync cannot produce this file; once it is, the generated copy replaces
this one and the entries below move to the master lists in `stoatworks-backend`.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Plugin shape, harness, --pipe contract and verify — Stoatworks wetplate, toner

<https://github.com/stoatworks-labs/wetplate>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the clock-unit voting, the About block, the Diag logger), the harness shape, the --pipe contract with SIGPIPE ignored and cues that step for options and booleans, the software-renderer pass, the verify script, the generated-table-with-a-check pattern and the negative-control pattern are wetplate's and toner's, which had them from rebate and pitch; the host clock-unit voting is readout's by way of all of them.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, with wetplate's Swap.

### The resize-mid-run guard — Stoatworks photofinish

<https://github.com/stoatworks-labs/photofinish>  
Licence: MIT  
Copyright: Stoatworks Labs

The trap that a reallocated buffer is a cleared buffer, and the check that guards it, are photofinish's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the fleet.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately. Resolves OpenGL entry points on Windows.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample. Part of the upstream SDK tree rather than something this plugin calls.

## Data this project carries

Published measurements the model is computed from. Each is committed with its source and checked where it can be.

### The CIE 1931 2° standard observer — CIE, via the Colour & Vision Research Laboratory

The colour-matching functions x̄, ȳ, z̄ at 1 nm from 360 to 830 nm, as published in CIE 015:2018 *Colorimetry, 4th Edition* and the CIE dataset CIE 018:2019 (DOI 10.25039/CIE.DS.xvudnb9b). Taken from the CVRL database at UCL (<http://www.cvrl.org/database/data/cmfs/ciexyz31_1.csv>, fetched 2026-09-25) and committed unedited as `tools/data/ciexyz31_1.csv`, SHA-256 `ccbe601d7abf2227b46007ab29742f1f6bcb8621a6413283fb5ade7626a662c5`, which `tools/planck_table.py` checks before it uses a byte of it. The Planck table in `source/PlanckTable.h` is computed from it, and `fitest --colour` integrates its own locus from the same file.

### The resistivity of tungsten — Forsythe and Worthing, 1925

W. E. Forsythe and A. G. Worthing, "The Properties of Tungsten and the Characteristics of Tungsten Lamps", *The Astrophysical Journal* 61, 146 (1925), doi:10.1086/142880. The 34 values of resistivity from 300 K to 3600 K by 100 K in `source/Lamp.h` are that table as it is widely reproduced (for example in the CRC Handbook's tungsten tables); the transcription used was taken from The Physics Factbook's page on the resistivity of tungsten (<https://hypertextbook.com/facts/2004/DeannaStewart.shtml>), which cites T. W. Zerda (Texas Christian University, 2001). It was **not** checked against the 1925 paper itself. It gives the cold-to-hot ratio of 15.0 (300 K to 2800 K) that the inrush comes from.

### The heat capacity of tungsten — NIST-JANAF

M. W. Chase, Jr., *NIST-JANAF Thermochemical Tables, Fourth Edition*, J. Phys. Chem. Ref. Data, Monograph 9 (1998), as the Shomate fit for solid tungsten on the NIST Chemistry WebBook (<https://webbook.nist.gov/cgi/cbook.cgi?ID=C7440337&Mask=2>, fetched 2026-09-25): two ranges, 298–1900 K and 1900–3680 K. `c_p` rises from 0.13 to 0.20 J/(g·K) over a filament's range, and the filament's heat capacity follows it.

## Standards and published specifications

What the implementation is measured against.

- **Planck's law**, with the 2019 SI exact values of h, c and k.
- **IEC 61966-2-1:1999** — the sRGB primaries and white (the XYZ-to-linear-sRGB matrix) and the transfer function.
- **CIE 15:2004 / 015:2018** — Illuminant A as a Planckian radiator at 2856 K: the table's luminance reference.
- **C. S. McCamy, "Correlated color temperature as an explicit function of chromaticity coordinates", Color Research & Application 17(2), 142–144 (1992)** — the cubic `fitest --colour` recovers a CCT with, as an external check on the colour.
- **NIST-JANAF / Chase (1998)** — above.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Incandescent scoreboards, marquees and chaser walls

Stadium scoreboards of the 1960s–80s, theatre marquees and the tungsten "bulb walls" of stage and TV design: pictures made of lamps whose light is a hot wire's. The thing they share, and the thing this plugin is built from, is that every bulb's colour and lag come from its filament's temperature.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
