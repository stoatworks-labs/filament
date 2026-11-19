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

### earshot — Stoatworks earshot

<https://github.com/stoatworks-labs/earshot>  
Licence: MIT  
Copyright: Stoatworks Labs

external/earshot/ is earshot, the fleet's shared audio front end, as a snapshot: one commit taken with git archive and left unmodified, named in external/earshot/SNAPSHOT. It is built into the plugin by add_subdirectory and supplies the capture device, chosen by name with a Silent state, and the audio analysis the plugin reads every frame. It brings miniaudio with it, credited below.

### The colour organ's device menu, status lamp and lazy start — Stoatworks needle and woofer, and spasis before them

<https://github.com/stoatworks-labs/needle>  
Licence: MIT  
Copyright: Stoatworks Labs

The Audio Input option, whose values are earshot device keys so a composition remembers the device by name, with None, System default, the spare slots and Rescan Inputs; the status lamp's states (amber no input, green delivering, red silent, blinking red missing); and the rule that no earshot engine is built and no device listed until the first frame drawn with the organ On (source/Organ.{h,cpp}, from needle's VFD input, which is woofer's source/Audio.cpp, spasis's design before it). Adapted.

### The browser demo's WebAssembly build — Stoatworks flyback, woofer and mirrorball

<https://github.com/stoatworks-labs/woofer>  
Licence: MIT  
Copyright: Stoatworks Labs

demo/tools/build-wasm.sh, which compiles the colour organ's own C++ and the earshot snapshot to WebAssembly for the browser demo, and demo/wasm/inputs.sha256, which pins what went in, are flyback's (woofer's and mirrorball's before it), cut down to the organ's files. Adapted.

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

### miniaudio, inside earshot

<https://github.com/mackron/miniaudio>  
Licence: Public Domain (Unlicense) or MIT-0, at your choice  
Copyright: David Reid

Arrives inside the vendored earshot snapshot, at external/earshot/third_party/miniaudio/miniaudio.h, unmodified. Not vendored separately.

earshot's capture backend: it lists the capture devices and runs the audio callback, on CoreAudio and WASAPI. The plugin never calls miniaudio itself; it arrives only with earshot.

## Data this project ships

Measurements made by other people and carried inside the product. Each is credited the way its publisher asks, and where a licence requires its attribution to travel with the data, that text is reproduced here.

### CIE 1931 colour-matching functions, 2° observer — CIE, in the copy the Colour & Vision Research Laboratory (UCL) publishes

<https://doi.org/10.25039/CIE.DS.xvudnb9b>  
Licence: Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)  
Copyright: International Commission on Illumination (CIE)

The colour-matching functions at 1 nm from 360 to 830 nm (CIE 015:2018; CIE 018:2019), as CVRL publishes them, "CIE 1931 2-deg, XYZ CMFs" (http://www.cvrl.org/database/data/cmfs/ciexyz31_1.csv): committed unedited as tools/data/ciexyz31_1.csv (sha256 ccbe601d7abf2227b46007ab29742f1f6bcb8621a6413283fb5ade7626a662c5, checked before use), with a note beside it (tools/data/README.md). The Planck table in source/PlanckTable.h is computed from it by tools/planck_table.py, and so is the colour organ's gel table, source/GelTable.h (tools/gel_table.py); both are compiled into the plugin, and the harness integrates its own locus from the same file. The data and the tables baked from it are CC BY-SA 4.0, not MIT; the code is MIT.

### LEE Filters' spectral transmission curves — LEE Filters

<https://leefilters.com/colour/>  
Copyright: Panavision (LEE Filters)

The percentage of light each of eight gels passes, every 5 nm from 400 to 700 nm: 106 Primary Red, 139 Primary Green, 119 Dark Blue, 132 Medium Blue, 120 Deep Blue, 101 Yellow, 105 Orange and 020 Medium Amber. They were read on 2026-11-16 from the graph data on each filter's page (https://leefilters.com/colour/106-primary-red/, and the same pattern for the others) and committed unedited as tools/data/lee_gels.csv (sha256 da460f4466fe9094b929b9b0dba50624f18c4e87a06e5722be6961762f6095af, checked before use), with a note beside it (tools/data/README.md). tools/gel_table.py integrates them against a black body and the CIE observer into the colour organ's gel table, source/GelTable.h, which is compiled into the plugin, and the gel menu uses LEE's numbers and names. LEE's pages state no licence for the data (the site's footer says only "Copyright Panavision"), so it is credited here as LEE's published data.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The resistivity of tungsten — Forsythe and Worthing, 1925, via a web reproduction

<https://hypertextbook.com/facts/2004/DeannaStewart.shtml>

W. E. Forsythe and A. G. Worthing, "The Properties of Tungsten and the Characteristics of Tungsten Lamps", The Astrophysical Journal 61, 146 (1925), doi:10.1086/142880. The 34 resistivities from 300 K to 3600 K in source/Lamp.h were copied from a web reproduction of that table, The Physics Factbook's page on the resistivity of tungsten (which cites T. W. Zerda, Texas Christian University, 2001); they were not checked against the 1925 paper itself. They give the fifteenfold cold-to-hot ratio the inrush comes from.

### The heat capacity of tungsten — NIST-JANAF (Chase, 1998)

<https://webbook.nist.gov/cgi/cbook.cgi?ID=C7440337&Mask=2>

M. W. Chase, Jr., NIST-JANAF Thermochemical Tables, Fourth Edition, J. Phys. Chem. Ref. Data, Monograph 9 (1998), as the Shomate fit for solid tungsten on the NIST Chemistry WebBook (fetched 2026-09-25), two ranges, 298-1900 K and 1900-3680 K. c_p rises from 0.13 to 0.20 J/(g K) over a filament's range and the filament's heat capacity follows it.

### Colour Organ, Project 428 — Electronics Today International (Australia), November 1974

<https://worldradiohistory.com/AUSTRALIA/ETI-Australia/70s/ETI-1974-11.pdf>

Pages 77-81. The colour organ is modelled on this circuit: its band split (red up to the A an octave below middle C, green over the next three octaves, then blue, which give the 220 Hz and 1760 Hz defaults), the half-wave diode detector with each channel's printed parts (10k into the node, 100k discharging it, 1.0 uF on the red channel and 0.47 uF on the others, so releases of 100 ms and 47 ms), and the trigger: a constant-current ramp, reset at every mains zero crossing, whose slope the detector sets and which fires each lamp's triac. No text or drawing of it is reproduced; source/Organ.{h,cpp} models it from the schematic and the parts list, and the harness holds those part values itself.

### Psychedelia I — D. Lancaster, Popular Electronics, September 1969

<https://worldradiohistory.com/Archive-Poptronics/60s/69/Pop-1969-09.pdf>

Pages 27-44: the same trigger earlier, a control voltage setting a ramp's rate, the ramp reset at every line zero crossing and the triac turning off as the line passes through zero; and coloured lamps behind a diffuser. Read to confirm ETI 428's trigger is the period's; no code.

### Audio EQ Cookbook — W3C Note, 8 June 2021, ed. R. Toy, after R. Bristow-Johnson

<https://www.w3.org/TR/audio-eq-cookbook/>

The low-pass and high-pass biquad coefficients and the bilinear prewarping the colour organ's crossover is built from (source/Organ.cpp), as Linkwitz-Riley second-order sections of Q 0.5.

### Linkwitz-Riley crossovers — S. Linkwitz, "Active Filters" (linkwitzlab.com), and D. Bohn, RaneNote 160 (2005)

<https://www.linkwitzlab.com/filters.htm>

The second-order Linkwitz-Riley alignment (Q 0.5, each output 6 dB down at the corner, the outputs summing flat), which the crossover uses and which the harness's --crossover check holds its -6.02 dB corners to.

### The RMS of a phase-cut sine — Monolithic Power Systems, MPScholar, "Single-Phase AC/AC Converters"; H. F. Ifech, University of Anbar, Power Electronics ch. 5

<https://www.monolithicpower.com/en/learning/mpscholar/power-electronics/ac-ac-converters/single-phase-ac-ac-converters>

The closed form 1 - a/pi + sin 2a / 2pi for a lamp's RMS squared at firing angle a, measured from the zero crossing, which the harness's --firing check compares the organ's lamps against. No code; the shader integrates the cut sine itself.

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
