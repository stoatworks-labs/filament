# Filament user guide

Filament is **a wall of incandescent bulbs, for [Resolume](https://resolume.com) Arena and
Avenue**, as an FFGL effect. It does not draw glowing dots with a slow fade. Every bulb of the
wall is a tungsten wire heated by current, and its light is its temperature: its dimmer is set by
its cell of the clip, the mains drives it through that dimmer, and the wire's own heat balance
decides how hot it gets and how fast. Dimmed bulbs going red as well as dark, flashes that come up
fast and die away slowly, the surge of a cold bulb, the lag of a big lamp and the ripple of a
small one are all what the filament does, not what somebody drew.

![Resolume's demo clip Metalive through the wall: a tumbling sphere of tiles drawn in clear bulbs, the hot ones yellow-white with glowing coils, the dim ones orange, the unlit ones faint discs of glass](hero.png)

*Resolume's bundled demo clip Metalive 01 through the plugin at its defaults, rendered by the
offline harness rather than captured from Resolume: a 48 × 27 wall of clear 40 W bulbs on 50 Hz
square-law dimmers. The bright tiles are filaments near 2800 K, nearly white; the dim ones are
cooler and so redder; the unlit bulbs are faint discs of glass.*

> **Before you rely on this:** released at **v0.1.0**, and honestly early. The filaments are
> measured rather than asserted, by a harness that drives the real plugin class and reads each
> bulb's temperature back through the plugin's own probes, at two rasters and on a software
> renderer: at a steady voltage a filament settles where power in balances power out (at rated
> voltage 2799.95 K against 2800); its light sits on the Planckian locus to 1e-6 in xy and moves
> redward with every step down; a cold start follows the heat-balance equation to 0.004 K and
> reaches 90% in 65 ms, where a lamp without tungsten's fifteenfold inrush would take 135; a
> switched-off filament cools by T⁴ radiation plus conduction, its time constant 2.5 times longer
> at 1200 K than at 2400 K; a triac-dimmed filament ripples at exactly twice the mains, 7.4 times
> more on a 25 W lamp than a 500 W one; and a resize mid-fade keeps every temperature exactly. Six
> deliberately broken models are each shown to fail their check. All 14 controls are shown to
> change the picture. **The checks verify the stated model, not a real lamp**: three of its
> constants are assumptions (see Known limits), and no real bulb has been measured against it.
> It has **never been loaded into Resolume on macOS** — the one host it has run in there is the
> fleet's own test host, `oxbow`, for 120 frames.
> On Windows it has: a build of this source loads, registers and renders in Resolume Arena 7.27.1 on software rendering (win-lab, Mesa llvmpipe, no GPU), with every control matching what the plugin declares and all 14 moving the picture, in the fleet's Arena gate (9 of 9 checks). The gate's picture is a still, so Wattage, Mains and Ambient Temp, which act mostly on how the wall moves, read weakly there (about 4.6 to 5 levels against a noise floor of 1.1, where the other controls read 10 to 55); software rendering says nothing about a GPU or about speed.
> Try it on a spare layer before you put it in a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Every download carries one effect, **SW Filament**. Drop it into Resolume's effects folder and
restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. The effect then appears in the effects
browser as **SW Filament**.

The macOS download is a universal build (Apple silicon and Intel), as a `.dmg` or a `.zip`. It
is Developer ID-signed and notarised by the release pipeline after publication, so the bundle simply
loads; if macOS refuses a download, it predates the signing — download it again. The Windows download is an x64 installer or a `.zip`. It is not code-signed, so
the installer trips SmartScreen once: **More info** → **Run anyway**.

---

## A bulb is a wire, not a fade

An incandescent bulb is a tungsten wire heated by the current through it. Its temperature follows
one equation, and each bulb of the wall solves it for itself, on the GPU, every frame:

    C(T) dT/dt  =  V(t)² / R(T)  −  ε σ A (T⁴ − T₀⁴)  −  k (T − T₀)

power in, through a resistance that rises with temperature; power radiated, as the fourth power of
temperature; power conducted away to the lead-in wires and supports; all against the wire's heat
capacity. Each term makes something you can see:

| the term | what comes out |
| --- | --- |
| the light is Planck's law at T through the CIE 1931 observer | **warm fades**: a dimmed bulb goes red as well as dark. A 60 W bulb is 2816 K at full and 1975 K at 45%, and its colour follows |
| radiation goes as T⁴ | **a fast rise and a slow fall**: a 60 W bulb reaches 90% of its rise in 65 ms and takes 176 ms to cool to 1500 K, and the cooling slows as it goes |
| tungsten's resistance is 15 times lower cold than hot | **inrush**: a cold bulb draws fifteen times its rated power and heats twice as fast as a constant-resistance bulb would |
| the voltage is the mains, through a dimmer | **mains ripple** at exactly twice the mains frequency, large on a small filament, almost nothing on a big one |
| the heat capacity is the wire's mass | **big lamps are slow, small lamps quick**: each lamp's wire is sized from its wattage and voltage, so a 1000 W filament weighs over a thousand times what a 10 W one does |

Every lamp is designed the same way: it dissipates its rated wattage at the mains voltage with the
wire at **2800 K** in a 25 °C room, and 15% of that power leaves by conduction. So every lamp at
full reads the same colour and the same brightness — Wattage changes how the wall **moves**, not
how bright it is.

---

## Start here

Put SW Filament on a layer or a clip with **some brightness and some movement** — the bundled
Metalive, IntoTheGlow_02 or Beat 001 loops. Out of the box you get a 48 × 27 wall of clear 40 W
bulbs on 50 Hz square-law dimmers, with a modest halo and bloom.

Then:

1. **Mix → 0 and back.** Compare the clip with the wall. Where the clip is bright the bulbs are
   near white; where it is mid-grey they are orange; where it is dark they are off, and you see
   the glass.
2. **Wattage → 1000 W.** Watch a flash or a fast move. The heavy wires barely register a quick
   flash and glow on after it, and movement smears into a warm trail. **→ 10 W** and the wall
   follows the clip almost frame for frame.
3. **Dimmer → Linear.** A plain sine-wave dimmer: light from tungsten goes as roughly the 3.4th
   power of the voltage, so the mid-greys go dark and red and only the highlights stay. **→
   Triac** chops each half-cycle instead, and sits between. Back to **Square Law**, which is what
   theatre dimmer curves are for.
4. **Gels → RGB.** Three filaments per cell behind red, green and blue gels, drawn as one bulb:
   the wall now carries the clip's colour. Blue is dim, because tungsten makes little blue light.
5. **Glass → Frosted**, then **Amber**. A frosted globe glows all over instead of showing its
   coil; coloured glass tints the light.
6. **Columns → 160, Rows → 90.** A finer wall. Changing the grid keeps every filament's
   temperature. **Pixel Mode** on draws each bulb as a flat square of its cell.
7. **Exposure** up or down: the wall's camera exposure, in stops.

**Thin, dim features vanish.** Each bulb sees the average of its whole cell of the clip, so a thin
line on black averages to almost nothing, and red has a luma of only 0.21. Lift a dark clip with a
brightness or levels effect ahead of the plugin, or use a finer grid.

Every slider is declared to the host as 0 to 1. The value each position stands for is given with
each control below.

---

## The Wall group

**Columns** (4 to 160, default 48) and **Rows** (2 to 90, default 27). The number of bulbs across
and down. Each bulb's dimmer is the average of its cell of the clip. The bulbs are square cells of
the grid stretched to the frame, so 48 × 27 is square bulbs on a 16:9 frame. A change regrids the
filaments: each new bulb takes the temperature of the old bulb nearest it, so a change mid-fade
carries on from where the wall was.

**Gap** (0 to 0.8 of the cell, default 0.24, slider 0.3). The dark space between bulbs. 0 packs the
bulbs edge to edge; 0.8 leaves small points of light.

**Pixel Mode** (off). Each bulb is drawn as a flat square filling its cell (less the gap) instead
of a bulb with a coil in a glass. The filaments, the colour and the timing are unchanged. It reads
best on a coarse wall: at 160 × 90 the squares are a few pixels across and look much like the
bulbs.

**Glass** (Clear). What the bulb's envelope is:

| | |
| --- | --- |
| Clear | the coil shows as a bright bar in the middle of a lit disc |
| Frosted | the whole globe glows evenly, a little dimmer |
| Amber, Red, Green, Blue | coloured glass: the filament's light times the glass's transmittance |

The coloured glasses' transmittances are chosen by eye, as the look of each glass, not measured
from a filter.

**Gels** (Off, RGB). RGB puts three co-located filaments in every cell, behind red, green and blue
gels, each driven by its own channel of the clip, and draws them as one bulb. That is how a bulb
wall shows colour. Tungsten at 2800 K has much more red than blue in it, so the blue gel is dim;
that is left in rather than trimmed. RGB costs three filaments per cell.

---

## The Bulb group

**Wattage** (10 to 1000 W, logarithmic, default 40 W, slider 0.301). The lamp every bulb is. It
sets the wire's size and so its heat capacity, and so every time constant of the wall. From the
model at 50 Hz / 230 V, rounded:

| | 10 W | 40 W | 100 W | 1000 W |
| --- | --- | --- | --- | --- |
| wire | 10 µm × 0.51 m | 26 µm × 0.81 m | 47 µm × 1.10 m | 219 µm × 2.36 m |
| 90% of the rise from cold | 20 ms | 49 ms | 91 ms | 420 ms |
| full to 1500 K when switched off | 53 ms | 134 ms | 247 ms | 1.15 s |
| triac ripple at a half setting | 242 K | 97 K | 53 K | 11 K |

(The rise and fall figures are the model's at a steady voltage, not a measured lamp; the harness
measures the 60 W case directly: 65 ms and 176 ms.) Wattage is dynamics, not brightness: every lamp
at full is 2800 K and reads the same.

**Dimmer** (Linear, Square Law, Triac; default Square Law). How the clip's value becomes a
voltage. All three keep the mains waveform; only the triac chops it.

| | |
| --- | --- |
| Linear | a sine-wave dimmer: RMS voltage = the level. Tungsten's light rises as about the 3.4th power of the voltage, so this is very contrasty — mid-greys go dark and red |
| Square Law | RMS voltage squared = the level, so electrical power roughly follows the fader. The default, and what theatre dimmer curves exist for |
| Triac | a leading-edge phase-cut dimmer, firing at (1 − level) of each half-cycle: the real electronics of most wall dimmers. The ripple is largest here |

**Mains** (50 Hz, 60 Hz). The supply: 50 Hz is a 230 V lamp, 60 Hz a 120 V one. It sets the
ripple's frequency (100 or 120 Hz) and, because a 120 V lamp of the same wattage has a thicker,
shorter wire, it makes the lamp slower: a 40 W 120 V lamp takes 76 ms to 90% where a 230 V one
takes 49.

**Ambient Temp** (0 to 200 °C, default 25 °C, slider 0.125). The room the wire loses heat to. A
hot ambient means a slightly hotter unlit filament and slightly slower cooling at the bottom; it
barely matters above a dull red.

**Why you rarely see the ripple.** The picture each frame shows is the light averaged over the
frame, as a camera's shutter would collect it. A 100 Hz ripple averages out over a 1/60 s or
1/30 s frame almost entirely: at the defaults the frame-to-frame variation on a flat grey is ±0.3%,
about one code value. A 10 W bulb on a triac at 50 Hz keeps a faint ±1% beat at 60 fps, which is
what filming a small tungsten lamp at 60 fps looks like. At 60 Hz mains and 60 fps there is none.

---

## The Look group

**Glow** (0 to 1, default 0.5). The halo each bulb throws from its glass and reflector onto its
neighbours' cells.

**Bloom** (0 to 1, default 0.35). A wider blur of the wall's light, as a lens flares around a
bright lamp.

**Exposure** (−4 to +4 stops, default 0, slider 0.5). The wall's camera exposure: the light goes
through `1 − exp(−g × 2^stops × L)` per channel, so bright bulbs roll off softly into white
rather than clipping. Raise it to see dim filaments; lower it to keep the hot ones from going
white.

**Mix** (0 to 1, default 1). The wall over the clip. At 1 the output is opaque: the wall paints
the whole frame, whatever the clip's alpha. At 0 it is the clip, with the clip's own alpha.

---

## How it works

Four stages a frame, with every filament's temperature kept on the GPU between frames:

1. **Drive.** One texel per bulb: the mean of an 8 × 8 grid of samples over its cell of the clip,
   as code values — Rec. 709 luma for a white wall, each channel for RGB gels. The code value is
   the fader.
2. **Thermal.** One texel per bulb, ping-ponged: the heat-balance equation over the frame's seconds
   in fourth-order Runge–Kutta substeps, the mains waveform through the dimmer law evaluated at
   each stage's time. The substep is the smaller of a twentieth of a mains half-cycle and 1.5 over
   the lamp's stiffness (a cold filament's falling resistance is the stiff part), so a 10 W wall
   takes about 150 substeps a frame and a 1000 W one about 34. It writes the new temperatures and
   the light averaged over the substeps.
3. **Bloom.** That light blurred at the grid's raster, across and then down.
4. **Output.** For each pixel, the bulb of its cell (a coil in clear glass, a lit globe in frosted,
   a flat square in Pixel Mode), halos from its eight neighbours, the bloom, the glass tint, the
   exposure curve, sRGB, Mix.

The light at each temperature comes from a table of Planck's law integrated against the CIE 1931
colour-matching functions from 250 K to 3700 K, generated by a script from the CIE data and
checked byte for byte in the build. The resistance comes from Forsythe and Worthing's 1925 table of
tungsten's resistivity; the heat capacity from NIST-JANAF's fit for tungsten.

Time is kept by the host's clock. The frame's seconds and the mains phase at its start are worked
out in double precision on the CPU, because Resolume's clock overflows a float; the shaders see only
the frame's step and the phase. A frame longer than a quarter of a second is clamped to a quarter
of a second; a paused clock runs no substeps and shows the wall as it is.

---

## Performance

Measured by the offline harness on an M4 Max, best of three, `glFinish` both sides, on a GPU shared
with other work:

| | default (48 × 27, 40 W) | largest grid (160 × 90, 40 W) | largest, 10 W on RGB gels (worst) |
| --- | --- | --- | --- |
| 1280 × 720 | 0.25 ms (1.5%) | 0.30 ms | 1.54 ms |
| 1920 × 1080 | 0.26 ms (1.5%) | 0.38 ms | 1.63 ms |
| 3840 × 2160 | 0.39 ms (2.3%) | 0.54 ms | 1.78 ms (10.7%) |

(percentages of a 60 fps frame). Small lamps cost the most, because they need the most substeps;
RGB gels triple the filaments. The state held is the grid's buffers only, 1.1 MB at 160 × 90,
whatever the output resolution. Nothing was timed inside Resolume, and nothing was timed on
Windows.

---

## If it looks wrong

**The wall is dark except for a few bulbs.** The clip is dark, or Dimmer is Linear. Use Square
Law, raise Exposure, or lift the clip with a levels effect ahead of the plugin.

**Everything is orange, nothing is white.** The clip's highlights are not reaching full: a bulb is
only near white at full drive. Raise Exposure a stop, or brighten the clip.

**A thin line or a small shape is gone.** Each bulb averages its whole cell. Raise Columns and
Rows.

**The wall smears and lags.** Wattage is high: a big lamp is slow. Lower it; 10 W follows the clip
almost frame for frame.

**The blue is missing.** That is tungsten: a 2800 K filament has little blue in it, so behind a
blue gel it is dim. Blue glass is dim for the same reason.

**The bulbs flicker faintly.** A small lamp on a triac, at a frame rate that does not divide the
ripple: the light is averaged over each frame, and what is left is a slow beat. Raise Wattage, or
use Square Law.

**The clip's transparency is gone.** The wall is opaque above Mix 0. Lower Mix to see the clip's
own alpha again.

**SW Filament is not in the effects browser.** Check the folder under Installing, and that
Resolume was restarted.

**The effect does nothing at all.** A shader that will not compile looks exactly like that, and
the real message is in the log:

```
macOS    ~/Library/Logs/filament/filament.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\filament\logs\filament.YYYY-MM-DD.log
```

It records the GL vendor, renderer and version at load, which pass failed if one did, the host's
clock and the unit the plugin decided it is in, and on every lamp change the wire the model sized
(diameter, length, mass) and its stiffness.

---

## Known limits

- **Three of the model's constants are assumptions, not measurements.** The wire's total
  emissivity is taken as a constant **0.30**; the coiled coil is taken to radiate from **half** its
  surface (it shades itself); and **15%** of the rated power is taken to leave by conduction. They
  are round figures, not read from a table. Together they size the wire, and so its heat capacity
  and every time constant of the wall. They give a 60 W 230 V filament of 34 µm by 0.92 m and
  16 mg, the right order for a real one, but **no real lamp has been measured against the model**.
  Real tungsten's emissivity rises with temperature; modelling that would make the fall slower
  still at low temperatures.
- **The resistivity table is a web reproduction.** The 34 values from 300 K to 3600 K are
  Forsythe and Worthing, "The Properties of Tungsten and the Characteristics of Tungsten Lamps",
  *The Astrophysical Journal* 61, 146 (1925), copied from a web reproduction of that table (The
  Physics Factbook's page on the resistivity of tungsten, which cites T. W. Zerda, TCU, 2001), not
  checked against the 1925 paper itself. Below 300 K the table is extrapolated.
- **The filament's colour is a black body's.** Real tungsten's spectral emissivity falls slowly
  across the visible, which makes a real filament slightly bluer than a black body at the same
  temperature.
- **Every bulb is identical.** A real wall's lamps differ a little; here they do not.
- **The picture is judged by eye**: the bulb shapes, the coil, the halo, the reflector, the bloom,
  the glass sheen, the glass tints, the gels and the exposure curve. The harness proves each
  control moves the picture; nothing measures how it looks.
- **The fader is the code value**, not linear light, averaged over the cell.
- **Never loaded into Resolume on macOS.** Everything numeric was compiled, rendered and measured
  offline against the real plugin class in a headless CGL context, plus an `oxbow` load.
- **Never seen on camera footage**, only on Resolume's bundled CG loops.
- **Checked at 320 × 180 and 1280 × 720** in the harness, and only timed at 4K.
- **Only ever run on an Apple M4 Max**, although the macOS build contains an Intel slice. On
  Windows, see the note at the top of this guide.
- **No presets**, no preheat, no lamp-to-lamp variation, and no OpenFX version.
- **There is a browser demo** at [filament-demo.stoatworks-labs.com](https://filament-demo.stoatworks-labs.com/).
  It is a port to a web page, not the plugin: the shaders run in WebGL2, and the time bookkeeping
  and the lamp sizing are rewritten in JavaScript. The page lists what it does not reproduce.

---

## About

The last group, **About**, carries the plugin's name, version, licence and maker, and buttons
that open this user guide ([stoatworks-labs.com/software/filament/guide/](https://stoatworks-labs.com/software/filament/guide/)),
the project page, the source on GitHub and the support page in your browser.

## Reporting something

[github.com/stoatworks-labs/filament/issues](https://github.com/stoatworks-labs/filament/issues).
A screenshot, the Wall and Bulb settings, and the composition's resolution and frame rate are
usually enough. If the effect did nothing, attach the log.
