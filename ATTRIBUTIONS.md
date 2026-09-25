# Attributions

Filament is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

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

PassBuffer is tinsel's, with wetplate's Swap; tools/sweep.py and the fleet's trap list are tinsel's too.

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

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The resistivity of tungsten — Forsythe and Worthing, 1925, via a web reproduction

<https://hypertextbook.com/facts/2004/DeannaStewart.shtml>

W. E. Forsythe and A. G. Worthing, "The Properties of Tungsten and the Characteristics of Tungsten Lamps", The Astrophysical Journal 61, 146 (1925), doi:10.1086/142880. The 34 resistivities from 300 K to 3600 K in source/Lamp.h were copied from a web reproduction of that table, The Physics Factbook's page on the resistivity of tungsten (which cites T. W. Zerda, Texas Christian University, 2001); they were not checked against the 1925 paper itself. They give the fifteenfold cold-to-hot ratio the inrush comes from.

### The heat capacity of tungsten — NIST-JANAF (Chase, 1998)

<https://webbook.nist.gov/cgi/cbook.cgi?ID=C7440337&Mask=2>

M. W. Chase, Jr., NIST-JANAF Thermochemical Tables, Fourth Edition, J. Phys. Chem. Ref. Data, Monograph 9 (1998), as the Shomate fit for solid tungsten on the NIST Chemistry WebBook (fetched 2026-09-25), two ranges, 298-1900 K and 1900-3680 K. c_p rises from 0.13 to 0.20 J/(g K) over a filament's range and the filament's heat capacity follows it.

### The CIE 1931 2-degree standard observer — CIE, via the Colour & Vision Research Laboratory (UCL)

<http://www.cvrl.org/database/data/cmfs/ciexyz31_1.csv>

The colour-matching functions at 1 nm from 360 to 830 nm (CIE 015:2018; CIE 018:2019, doi:10.25039/CIE.DS.xvudnb9b), committed unedited as tools/data/ciexyz31_1.csv with its SHA-256 checked before use. The Planck table in source/PlanckTable.h is computed from it by tools/planck_table.py, and the harness integrates its own locus from the same file.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Incandescent scoreboards, marquees and bulb walls

Stadium scoreboards of the 1960s to 80s, theatre marquees and the tungsten bulb walls of stage and television design: pictures made of lamps whose light is a hot wire's. No code, assets or binaries from any of them were used or examined.

## Standards and published specifications

What the implementation is measured against.

- **Planck's law** — with the 2019 SI exact values of h, c and k: the light of each filament at its temperature, treated as a black body.
- **IEC 61966-2-1:1999** — the sRGB primaries and white (the XYZ-to-linear-sRGB matrix) and the transfer function.
- **CIE 15:2004 / 015:2018** — Illuminant A as a Planckian radiator at 2856 K, the colour table's luminance reference.
- **McCamy, Color Research & Application 17(2), 1992** — the cubic the harness recovers a correlated colour temperature with, as an external check on the colour.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
