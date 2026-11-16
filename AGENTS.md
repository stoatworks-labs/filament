# AGENTS.md — Filament

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you tell
anybody this works.

---

## What the plugin is

The picture on a wall of incandescent bulbs, as an FFGL 2.1 effect (`FI01`, shown as
`SW Filament`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` and (in CI) a Windows `.dll`. MIT, at
`github.com/stoatworks-labs/filament`. Released v0.1.0 on 2026-09-25.

Built 2026-09-25 in one session (tranche five) from `specs/SPEC-filament.md` and the
fleet's templates: wetplate for the plugin shape, the harness, the generated table with
a `--check`, verify and the negative controls; toner and wetplate for `--pipe` with
SIGPIPE ignored; tinsel for `PassBuffer`, the sweep and CI; photofinish for the
resize-mid-run trap; afterglow and needle for the idea of state that decays by its own
physics rather than by a tuned constant.

**The colour organ** (parameters 19–30, Off by default) was added on 2026-11-16 on branch
`claude/organ` from `91bef09`, after v0.1.0, and is unreleased. It has its own section
below. Its spec, with a source for every physical claim, is
`~/Projects/resolume/specs/SPEC-filament-organ.md`. `SPEC-filament.md` is unchanged.

---

## The one idea

**A bulb is a tungsten wire heated by current, not an LED with a slow fade.** Its light
is its temperature, and its temperature follows

    C( T ) dT/dt = V( t )^2 / R( T )  -  e s A ( T^4 - T0^4 )  -  k ( T - T0 )

Point a matrix of them at the clip, each bulb's dimmer set by its cell, and:

| the term | what comes out |
| --- | --- |
| the light is Planck's law at T through the CIE observer | **warm fades**: a dimmed bulb goes red as well as dark (2816 K at full, 1975 K at 45%) |
| radiation as T^4 | **a fast rise and a slow fall**: a 60 W bulb reaches 90% of its rise in 65 ms and takes 176 ms to fall to 1500 K, and the fall slows as it goes (tau grows 2.5x from 2400 K to 1200 K) |
| R( T ) from tungsten's resistivity table, 15x from cold to hot | **inrush**: a cold bulb draws 15x its rated power and heats 2.1x faster than a constant-R bulb would |
| V( t ) is the mains, through a sine or a phase-cut dimmer | **mains ripple** at exactly twice the mains: 137 K peak to peak on a 25 W filament at 50 Hz under a triac, 19 K on a 500 W one |
| C from the wire's mass: a wire sized for P watts at V volts | **big lamps are slow, small lamps quick**: C / P goes as ( P / V )^(2/3), and a 120 V lamp is slower than a 230 V one of the same wattage |

### The pipeline

1. **Drive** (one texel per bulb): the mean of an 8 x 8 texelFetch grid over the bulb's
   cell of the source, as code values; luma for a white wall, rgb for a gelled one. The
   code value is the fader.
2. **Thermal** (one texel per bulb, ping-ponged): the ODE over the frame's seconds in RK4
   substeps, the mains waveform through the dimmer law evaluated at each stage's time.
   Two render targets: the new temperatures, and the light averaged over the substeps.
3. **Bloom**: that light blurred at the grid's raster, x then y.
4. **Output**: the bulb of each pixel's cell (a coil in clear glass, a lit globe in
   frosted, a flat square in Pixel Mode), halos from its eight neighbours, the bloom,
   glass tint, exposure `1 - exp( -g x 2^stops x L )` per channel, sRGB, Mix.

### The lamp, precisely (`Lamp.h`)

Every lamp is designed to dissipate `Wattage` at the mains voltage (230 V at 50 Hz,
120 V at 60 Hz) with its wire at **2800 K** in **25 C**, **15%** of that power leaving by
conduction and the rest by radiation. That design point fixes both loss terms outright,
so the right-hand side divided by P depends only on T, the drive and the ambient. The
lamp's size enters only through C( T ) / P, and C comes from the wire: a straight wire of
diameter d and length L, radiating from **half** its surface (a coiled coil shades
itself) at emissivity **0.30**, with resistance V^2 / P hot, is fixed by two equations;
its mass times tungsten's c_p( T ) (NIST-JANAF) is C( T ). A 60 W 230 V lamp comes out at
33.6 um x 0.92 m, 15.8 mg — the right order for a real one (30–45 um, about a metre
uncoiled).

The three bold constants that are not a design point — 0.30, one half, 15% — are
**assumptions**, not measurements. They set the time scale (through the mass) and nothing
else. The spec wrote C as a constant; C( T ) here follows c_p, which rises from 0.13 to
0.20 J/(g K) across the range.

### Time and the substeps

The frame's seconds and the mains phase at its start are reduced in double on the CPU
(`start x hertz - floor( start x hertz )`); the shader sees `Phase0`, `StepSeconds` and
`StepCycles` and nothing absolute. The substep count is `ceil( dt / hMax )` with
`hMax = min( half-cycle / 20, 1.5 / stiffness )`, the stiffness being the largest
|d(dT/dt)/dT| over every temperature at the peak of the cycle (a cold filament's falling
resistance is the stiff part). Capped at 4096; dt capped at 0.25 s; a dt of 0 runs no
substeps and shows the instant's light.

**The cost** (`fitest --bench`, the largest grid, 160 x 90 = 14 400 bulbs, best of three
on a shared M4 Max GPU):

| | 10 W (147 substeps) | 40 W, the default (59) | 1000 W (34) | 10 W, RGB gels (3 filaments) |
| --- | --- | --- | --- | --- |
| 1280x720 | 0.61 ms | 0.30 ms | 0.22 ms | 1.54 ms |
| 1920x1080 | 0.70 ms | 0.38 ms | 0.30 ms | 1.63 ms |
| 3840x2160 | 0.84 ms | 0.54 ms | 0.46 ms | 1.78 ms |

So the sub-stepping costs about **3.4 us per substep per 14 400 bulbs** (0.24 ns per
bulb-substep: four derivative evaluations and a colour lookup), and it is the stiffness
rule, not the mains, that sets the count for anything under ~100 W. At the default 48 x 27
it is 0.25 / 0.26 / 0.39 ms at 720p / 1080p / 4K. State held is the grid's buffers only:
1.1 MB at 160 x 90, whatever the output raster.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Lamp.{h,cpp}` | The ODE in double: the resistivity table, c_p, the wire, the drive waveform, the steady state, the stiffness and the substep rule, the `Perturb` bits. |
| `source/PlanckTable.h` | GENERATED by `tools/planck_table.py`: 346 rows, 250–3700 K by 10 K, ( r/Y, g/Y, b/Y, log2 Y ). |
| `source/Model.h` | Presentation: the exposure gain, the wall, the glass sheen, the gels, the bloom width; the `Probe` hooks. Judged by eye. |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses; the option lists. |
| `source/Shaders.{h,cpp}` | `kFilament` (the GLSL library: resistivity, c_p, sinPi, the waveform, the derivative, the colour) and the six pass bodies. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed, wetplate's Swap. |
| `source/Filament.{h,cpp}` | The plugin: parameters, the clock, the lamp cache, the buffers, the passes. |
| `tools/planck_table.py` | The CMFs, Planck, the sRGB matrix; writes the table; `--check` for verify. |
| `tools/data/ciexyz31_1.csv` | CVRL's CIE 1931 2° CMFs at 1 nm, unedited. |
| `tools/fitest/` | The harness: renders, probes, the double reference, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py`, `tools/verify.sh` | No dead control; all of it. |
| `source/Organ.{h,cpp}` | The colour organ's CPU half: the device menu, the earshot Engine, the FIFO onto the host's clock, the DC block, the LR2 crossover, ETI 428's detectors, the control rows. **The only file that includes earshot.** |
| `source/GelTable.h` | GENERATED by `tools/gel_table.py`: nine gels on the Planck grid, ( r/Y, g/Y, b/Y, Y_gel/Y ). |
| `tools/gel_table.py`, `tools/data/lee_gels.csv` | LEE's %T every 5 nm, 400–700 nm, unedited and SHA-256 checked; `--check`, `--print`. |
| `tools/fitest/Organ.inc` | The organ's checks, the scripted backend, the double reference of the triac and lamp. |
| `tools/mutate.sh` | 20 one-character mutants of the organ, each against the check that must catch it. |
| `external/earshot/` | earshot 8124191, vendored by `git archive`, unpatched (`SNAPSHOT`). Never edit it. |

---

## The colour organ

**One idea.** It models a 1970s three-channel light box. A passive three-way crossover
feeds three triacs under phase-angle control, and each lamp sits behind a coloured gel and
a frosted panel. Filament's own thermal model (Lamp.h, unchanged) does the rest. The bass
pumping on kicks, a cold lamp lagging its first hit and a treble that cannot follow fast
hats are not coded anywhere. They come out of the detector's time constants meeting the
filament's.

### The chain

On the CPU, per sample, in double, at earshot's 32 kHz tap (`Organ.cpp`):

1. **Input.** Left and right averaged to mono, then a one-pole DC block at 10 Hz. That
   corner is an assumption: earshot's tap passes DC, and DC would light the bass lamp.
2. **Crossover.** LR2 RBJ biquads with Q 0.5, prewarped. Bass = LP(f_low),
   mid = HP(f_low)·LP(f_high), treble = HP(f_high).
3. **Detector** (ETI 428). The diode is ideal on the negative swing. With Rc the 10k and
   Rd the 100k, the node charges toward r·Rd/(Rc+Rd) with τ = (Rc‖Rd)·C and releases with
   τ = Rd·C. The parts are red's R16, C12 and R19, green's R15, C11 and R18, and blue's
   R14, C10 and R17. C is 1.0 µF for bass and 0.47 µF for mid and treble, so the releases
   are 100 ms and 47 ms. Both steps are exact for a sample held over its period.
4. **Rows.** Each mains half-cycle is cut into 16 intervals. A row holds the integral of
   gain × detector at the 17 interval ends. Over each 1/16 the slope is the mean control
   over the previous 1/16, so a firing is never revised by audio that arrives in a later
   frame. Half-cycles are numbered `floor( 2 × hertz × t )` in double from the host clock's
   zero.

On the GPU, per bulb, per filament (`OrganThermal` in `Shaders.cpp`):

5. **Trigger.** The channel fires where b × (the row's integral) reaches 1/2, with b the
   cell's luma. Inside the 1/16 where it crosses, the integral is a straight line (the
   slope is constant over each 1/16), so interpolating it is exact. So a steady control
   of 1 fires at 90°, 2 at 45°, and below 1/2 it never fires. The integral so far rides
   between frames in a ramp buffer tagged with its half-cycle, mod 2^22.
6. **Lamp.** Lamp.h's ODE in RK4 substeps. Each substep's drive is the exact mean of
   (v/V_rms)² over its conducting part, `d − cosPi( lo + hi ) × sinPi( d ) / π`, in the
   product form so that short pieces don't cancel.
7. **Light.** `bulbLight`: the gel table's row for each filament at its T and its gel,
   averaged over the substeps like the wall's light. The bulb is drawn Frosted, and Glass's
   tint still applies.

**Organ off touches nothing.** `organOn` is the Organ option AND `ensureOrgan()`, and
`ensureOrgan` (the organ's shaders and textures) is only reached when the option is On.
The plugin's constructor does build the `Organ` object, but that object only holds a menu
of None, System default and fourteen `(none)`, with no enumeration, plus empty buffers.
The earshot Engine and the device list wait for `Activate()` on the first organ frame.
verify.sh proves both halves:

- `--coreaudio` runs in its own process on the real backend. It reads 0 engines, 0 lists,
  0 selects, no audio image loaded and no audio thread started.
- The organ-off renders, `--pipe` bytes and `--list` 0–18 match a build of `91bef09`.

**The device menu** follows woofer's and needle's shape. It is valued by
`earshot::deviceKey` (FNV-1a folded to 23 bits), so a composition finds its device by
name. A key with no device met in this process asks earshot for a name that cannot match,
which gives Missing and a blinking red lamp, never the default device. Rescan lists again.
Organ Off closes the device. `FILAMENT_NO_DEVICES=1` never lists and never hands earshot a
name: earshot's own lookup starts CoreAudio even for a name it then fails to match.

**The status lamp** is a disc in the output's top-right corner, drawn after Mix:

| Lamp | Meaning |
| --- | --- |
| amber | no input, or the device is still opening |
| green | delivering |
| steady red | open and bit-exactly silent |
| blinking red | missing or refused |

### Traps (organ)

#### ☠️ The organ-switch mutant would have opened a real microphone

`--coreaudio` is the only check on the real backend. Mutant 9 inverts the organ switch
(`== 1` → `!= 1`), so the organ runs while Off. Run against `--coreaudio`, it would have
built an Engine and listed the Mac's devices. With a saved key it could have opened one.
It is mapped to `--organdevices` on the scripted backend, which catches it on (a) and (b).
A mutant is only as safe as the backend of the check it runs against.

#### ☠️ A constant read by both the plugin and the harness moves with its mutant

The detector's 100k and capacitors live in `Organ.h`. A harness that took its expected τ
from there would follow mutant 5 (treble's C10 0.47 → 0.57 µF) and pass. `--detector`
holds ETI's printed values itself (`etiR19`, `etiC`), so the plugin and the harness agree
only when the plugin is right.

#### ☠️ Two copies of the light sum meant `--organcolour` covered one of them

The substep loop and the `Substeps == 0` branch each summed the three gels' light.
`--organcolour` reads the light through a dt = 0 frame, so only the second was checked.
Both now call `bulbLight`. Mutant 19 changes the one copy left and is caught.

#### ☠️ Mutant 15 survived the step check

`r1 > row` → `r1 < row` ends a substep's first conducting part at the wrong place. Its
error is cubic in how far the substep overshoots the zero crossing. In `--organstep` (60 W
at 2400 fps) that stayed under the 0.76 K tolerance. `--firing` at 150° catches it:
+0.75 K against 0.38, with an RMS² of 0.16999 against 0.16981 at a tolerance of 0.00009.
A late firing leaves a short conduction, where the overshoot is a large share of it.

#### Smaller ones

- **earshot's FFT has no bass.** Its Linear law has no bin from 30 to 150 Hz (an earshot
  trap), which is one reason the organ reads the waveform tap.
- **The scripted backend runs at 4× real time.** The FIFO drops whatever is more than
  120 ms above its 30 ms reserve, so the backlog never grows. Every check that measures
  the music feeds it in through earshot's `feed()` instead.
- **Saturated gels are outside sRGB.** 106, 139 and 119 have negative linear components.
  They are kept through the bloom and clipped at the encode, as planck_table.py does for
  low T.
- **verify.sh's `warnings` step counts the vendored trees apart.** The FFGL SDK has one
  warning (`FFGLUtilities.cpp:144`); earshot has none.
- **`--organframes`' rate-agreement line is weak.** Its tolerance, 3.8 K, is mostly the
  accumulated float bound. The claim rests on its bit-identical rows and its per-rate
  float-bound lines.

## Traps

In the order they bit, or would have.

### ☠️ wetplate's dt clamp would have blinded the harness

wetplate clamps a frame to at least 1/240 s. The rise, fall and ripple checks clock the
harness at 2400 fps so that a 100 Hz ripple is 24 samples a period and a 65 ms rise is
156; a 1/240 floor would have integrated ten times the real time per frame and every
trajectory would have been wrong while looking plausible. Here dt is clamped to
[0, 0.25 s] only. Anyone copying the clock from wetplate into something with its own
dynamics: check the floor.

### ☠️ The ripple's spectral-purity bound was six orders loose

The first bound on the power off the 2 x mains harmonics came from the GPU's distance to
the FINE reference, which includes the plugin's discretisation error: bounds of 0.4 and
6 on a measured 1e-7. But the plugin's substep schedule repeats exactly every mains cycle
(48 frames at 2400 fps and 50 Hz), so its discretisation error is periodic too and lands
ON the harmonics. The bound is now taken against the double reference on the plugin's
own schedule: 7e-11 to 7e-6.

### ☠️ A triac gate and a rational frame rate meet exactly

At 2400 fps, 50 Hz and two substeps a frame, the RK4 stage times fall on multiples of
1/192 of a half-cycle, and a triac at level 0.5 gates at exactly 0.5 of it. Float and
double can then disagree which side of the gate a stage is on, and one stage's v^2 jumps
from 0 to 2. The ripple check drives the triac at 0.537, which no stage lands on.

### ☠️ In a stateful plugin no two frames are alike, so wetplate's pipe step test cannot apply

wetplate proved an option steps by showing frames 0–3 of a still identical. Here the
filaments warm every frame. The test compares each scripted frame with the same frame of a
run that held one value throughout: frames 0–3 must be the Clear run's, 4–5 the Amber
run's, and a ramp would match neither. The same for Pixel Mode, and a Glow ramp must equal
a constant 0.5 run on frame 2.

### ☠️ The first wall was a grey fog

The wall at 0.006 linear through the exposure gain and a 1.2 bloom read as sRGB 0.2 grey
with an orange haze over every demo clip. The wall is 0.0016 now, the bloom 0.6 x, and an
unlit bulb shows as a faint disc of glass sheen (0.005) so a dark wall still reads as a
wall of bulbs.

### Instant or shutter

A bulb sampled at the frame's instant strobes: at 60 fps a 100 Hz ripple aliases to a
20 Hz flicker at its full depth. A camera's shutter and an eye both integrate. The thermal
pass averages the light over the frame's substeps and the picture shows that; the probes
show the instant. On a flat grey at the defaults the frame-to-frame variation is ±0.3%
(1 code value in 8 bits); a 10 W bulb on a triac at 50 Hz keeps a ±1% 20 Hz beat, which is
what filming a small tungsten lamp at 60 fps looks like. At 60 Hz mains and 60 fps there is
none.

### The spec's "R roughly doubles by 10x from cold"

Forsythe and Worthing's table gives 15.0x from 300 K to 2800 K; that is what the model
uses, and the inrush is 15.1x at 298 K.

### Output alpha is 1 at Mix 1, on purpose

Resolume's demo clips are DXV with alpha (Trinity is transparent over 90% of the frame,
with rgb 0 there). The wall paints the whole frame, so alpha is `mix( src.a, 1, Mix )`:
255 everywhere at Mix 1, the clip's own at Mix 0, measured through `--pipe` on Trinity.
The drive reads rgb only; transparent pixels are black and their bulbs are off.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put back
before the output pass); every `ffglex::Scoped*` clears to 0 on exit, so every `Ensure()`
happens before the frame's passes bind anything; `FFGLFBO::Release()` leaks the colour
texture (`PassBuffer::Destroy()` deletes it first); the thermal pass attaches a second
colour target to the SDK's FBO and puts it back to one target after; `SetParamInfo` clamps
a STANDARD default into 0..1; the core is an **OBJECT** library; `SetTextParameter` must
return `FF_SUCCESS` for the About block; the harness drives a synthetic clock; an option's
range reads back 0..1; Resolume's clock overflows a float; a reallocated buffer is a cleared
buffer (here the temperatures do not live at the output raster at all, and a grid change
regrids them); `packed` and the rest of GLSL 4.10's reserved words are not identifiers
(verify greps); no `M_PI`, no `far`, no `near`; GLSL's `sin` has no accuracy requirement,
so the waveform uses its own Taylor `sinPi`, the same on every driver; a hardware linear
filter on a float texture may interpolate with 8-bit weights, so the colour table is read
with two `texelFetch`es and interpolated by hand.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at 320x180
and 1280x720 and on Apple's software renderer at 320x180 in `verify.sh`. The numbers are
identical at both rasters, because every check reads one bulb's raw float state through
the `State` or `Instant` probe, and the bulbs live at the grid's raster, not the output's:
nothing here is a coverage or a filtered read. The drive pass reads with `texelFetch`, and
the harness reads the drive level the plugin actually used back from the probe (a sum of 64
equal floats need not be exactly 64 of them), so the reference runs on the same level.

Two float bounds recur. **Per step**: the GPU's RK4 step may differ from the same step in
double by one ULP of T (the final add, generously) plus `kDerivativeRel` = 1e-5 of the step,
twice the ~5e-6 that ~20 float operations (1.2e-6), the Shomate sum's cancellation above
1900 K (3e-6) and the float-rounded constants account for; summed over every substep taken.
**The discretisation**: the double reference on the plugin's schedule against one 16x finer,
measured on every frame.

| check | what it measures | tolerance and where it comes from | raster / renderer dependence |
| --- | --- | --- | --- |
| `--steady` mean | mean T over 25 mains cycles vs the power balance at the RMS, three levels, 200 W | **3 pp^2 / ( 4 T )**: the second-order rectification bias of a ripple through a T^4 and a 1/rho( T ) (|g''/g'| <= 3/T each), pp measured; **plus 2 ( ulp( T ) + 1e-5 x step ) tau / h**, the float error a contracting system settles to | none; per-bulb probe |
| `--steady` rated | the mean at rated voltage vs 2800 K | the same; the balance is 2800.000000 K by construction | none |
| `--colour` xy | the Instant probe's rgb, through the inverse IEC matrix, vs this harness's Planckian locus at the probed T | **2 x h^2/8 |xy''|**, h = the table's 10 K, xy'' from the locus's own second difference, doubled for curvature changing across the bracket; **+ 1e-6** for the float read through the matrix | none: texelFetch and hand interpolation, no filter |
| `--colour` luminance | Y vs the locus's Y( T ) / Y( 2856 K ) | the same rule on log2 Y (linear interpolation of a log), + 1e-6 relative | none |
| `--colour` McCamy | McCamy's CCT of the render vs McCamy's CCT of the true locus | the xy tolerance through McCamy's gradient (0.03–0.04 K); McCamy's own error at T (−20 to +1 K) is printed, not hidden | none |
| `--colour` redward | T, x and CCT over five levels | strict monotonicity | none |
| `--rise` / `--fall` schedule | GPU vs double on the plugin's schedule, every frame | the per-step float sum (0.52 K and 0.87 K by the end); measured 0.0015 and 0.0022 K | none; the software renderer passes unchanged |
| `--rise` / `--fall` ODE | GPU vs the 16x finer reference | measured discretisation (0.004 K, 0.17 K) + the float sum | none |
| `--rise` t90 | the time to 90% of the rise, interpolated between frames | **one frame** (1/2400 s) of interpolation + the trajectory tolerance over the slope at the crossing (0.43 ms) | none |
| `--rise` inrush | constant-R t90 minus R( T ) t90 | must exceed 4 x the t90 tolerance; it is 70 ms against 1.7 ms | none |
| `--fall` shape | tau( 1200 K ) / tau( 2400 K ) from central differences of the GPU's frames | above the geometric mean of the two models' own predictions (T^4 2.53, linear 0.84: 1.45) | none |
| `--ripple` frequency | the strongest DFT line of T over exactly 0.5 s | within half a 2 Hz bin of 2 x mains; the window holds whole cycles, so a periodic state has no leakage | none |
| `--ripple` purity | power off the 2f harmonics | **4 x** the double reference's own remainder on the plugin's schedule (the settling left) **+ 8 ( e / pp )^2**, e the worst |GPU − double| in the window: by Parseval the most an additive error can carry | none |
| `--ripple` size | pp( 25 W ) > pp( 500 W ), and pp > half the ODE's | inequalities; the ODE's pp is printed beside the GPU's (137.2 vs 136.6 K) | none |
| `--resize` | every bulb after an output resize (1.5x and back) and after a 16x9 → 24x13 regrid mid-fade vs an untouched run | **one ULP of 3000 K per substep taken** (0.45 K), for a software renderer that is not bit-repeatable; measured 0 exactly | the state does not live at the output raster; the regrid is nearest, exact on a flat field |
| `--crossover` (organ) | each band's −6.02 dB point, 26 bisections in log f. The gain at each step is a lock-in over whole periods on a sine fed through earshot's `feed()` and tap, against the tap's DC-blocked input. At the defaults: 220.0000 Hz (off 1.0e-8) and 1760.0458 Hz (off 2.6e-5) | **2 × the lock-in's leak** \|sin Nω\| / (N sin ω), over the measured gain slope, plus the bisection's bracket (1.0–1.1e-4). For mid's corners, add **2 (f1/f2)²**, the most the other section can move the point (from the LR2 magnitude) | no GL: the CPU's double |
| `--detector` release (organ) | τ from a least-squares fit of ln v over 150 ms, starting 40 ms after the band last swung past 1% of the node: 100.0000 and 47.0000 ms, off ≤ 9.5e-14 | **2 r / (Tw \|slope\|)**, r the fit's worst residual, + N × 2^-52 for the sums (1.1e-12). Also requires the diode stayed off: \|band\| / node < 1, worst 2.9e-3 | no GL |
| `--detector` gain law (organ) | every row point at +20, +6.02 and −12 dB against 10^(dB/20) × 0 dB's: worst 1.1e-7 | **two float roundings**, 2 × 2^-24 × 1.01 = 1.2e-7 (the rows are floats) | no GL |
| `--firing` mean (organ) | mean T over 25 cycles of a 200 W lamp at 30–150°, against the T whose power balance gives the closed form's RMS. Off −0.08 to −0.19 K | **bias + disc + float** (0.21–0.39 K). Bias: the double reference's own mean against its balance (a rippling T's mean is not the T at the mean power), measured. Disc: the scheme against the fine ODE, measured. Float: the accumulated per-step bound | none: State probe. Software: the same to 0.0003 K |
| `--firing` RMS (organ) | RMS² read back out of the mean T against 1 − α/π + sin 2α/(2π), e.g. 0.16978 against 0.16981 at 150° | the mean's tolerance through the balance's slope (0.00009–0.00026) | none |
| `--organstep` | a 60 W lamp from cold, 2400 fps. GPU against double on the plugin's scheme: 0.0027 K. GPU against the fine ODE (400 RK4 steps a half-cycle, aligned to every zero crossing and firing): 0.19 K. t90 96.88 ms on both | scheme: the accumulated float bound (0.567 K). ODE: measured disc + float (0.756 K). t90: **one frame** + the trajectory tolerance over the slope (0.44 ms) | none. Software: 0.0030 K |
| `--organframes` | the synthetic track at 30, 60 and 144 fps. 200 half-cycles' rows bit-identical. Each rate's GPU within its float bound of the double scheme (0.0011–0.0017 K). The rates agree to 0.018 K | rows: **exact**. Scheme: the float bound (1.8–1.9 K). Agreement: each pair's disc + float (**3.8 K: weak**, dominated by the float bound) | none |
| `--organclock` | the music with the clock at 503316.25 s, across the tag's 2^22 wrap, against a run from 0. 151 rows bit-identical, T within 4.9e-4 K | rows: **exact**. T: both runs' float bounds (2.8 K) | none |
| `--organresize` | all 432 filaments after a resize (1.5× and back), all 936 after a 16x9 → 24x13 regrid mid-music, ramps included: 0 K | one ULP of 3000 K per substep (0.45 K), as `--resize` | none: state at the grid's raster |
| `--organcolour` | a dt = 0 frame's light through the shutter probe, against the gel table at the probed T, all 9 gels, 3 filaments up to 550 K apart: 4.0e-7 relative | a float bound that counts exp2 at **GLSL 4.10's stated 3 + 2\|x\| ULP** (§4.7.1), the reads and the sum (6.2e-6) | none. Software: 4.5e-7 against 7.0e-6 |
| `--organdevices` | states from earshot, the lamp's colour read at its centre pixel | ±2 code values of the declared colour. The lamp is a disc ≥ 3 px in radius, read at its centre, never at an edge | the one organ check that reads the output raster; passes at both rasters and on software |
| `--coreaudio` | counters, dyld images and audio threads in a process of its own, on the real backend | **exact zeros**, and a positive control that must add threads | no GL dependence |

Deliberately NOT relied on: GLSL `sin` (the waveform is `sinPi`, a folded Taylor series
to u^11, error < 6e-8), a hardware-filtered read of any float texture, exact cancellation
anywhere (T0^4 − T0^4 is not assumed zero; a cold filament's derivative is whatever the
float arithmetic says, and the float bound covers it), or `pow`.

What might still differ on another rasteriser: nothing in a check reads a rasterised
edge; the bulb shapes, halos, bloom and anti-aliasing are only swept, not measured. The
organ's status lamp is read at its centre pixel only. The organ's frosted bulb and its
gels' mixes on the wall are judged by eye.

The organ's per-step float bound is the wall's with the derivative term doubled: one ULP
of T plus **2 × `kDerivativeRel`** of the step. The second 1e-5 is for the substep's
conduction energy, about 15 float operations at half an ULP plus the firing point's
interpolation. The energy is in product form, `d − cos(π(lo + hi)) sin(πd)/π`, so its
error scales with the piece and not with the half-cycle.

### The negative controls

`fitest --negative` runs nineteen: six for the wall and thirteen for the organ.
`--perturb BITS` runs any check verbosely against one. Each perturbs the *plugin's* model —
a `Perturb` bit the shipped plugin carries at zero — never the harness's expectation. The
two device controls are harness switches on the plugin's own class
(`Organ::negListAtConstruction`, `negMenuByIndex`).

| perturbation | what fails, at 320x180, 1280x720 and on the software renderer |
| --- | --- |
| R held at its hot value (no inrush) | `--rise`: 3 of 4 — the trajectory, and t90 135 ms against 65 |
| radiation as a linear term matched at 2800 K | `--fall`: 3 of 3 — the trajectory and the shape (tau ratio ~0.84, below 1.45) |
| DC at the same RMS, no mains waveform | `--ripple`: 14 of 14 — no line at 2 x mains, no ripple |
| the conduction term dropped | `--steady`: 4 of 4 — the filament settles hot of the balance |
| the colour table read 5% hot | `--colour`: 15 of 16 — every xy, luminance and CCT (the order is still redward) |
| a resize or regrid restarts the wall cold | `--resize`: 2 of 3 |
| organ: crossover corners not prewarped (64) | `--crossover`: 4 of 6 |
| organ: Butterworth Q 0.707 for LR2 (128) | `--crossover`: 6 of 6 |
| organ: detector released through 10k ‖ 100k (8192) | `--detector`: 3 of 4 |
| organ: gains read as decibels of power (16384) | `--detector`: 1 of 4 (the gain law) |
| organ: conduction energy linear in angle (256) | `--firing`: 16 of 20 |
| organ: ramp never reset at the zero crossing (2048) | `--firing`: 20 of 20; `--organstep`: 3 of 3 |
| organ: control's slope from the frame's audio (512) | `--organframes`: 5 of 5 |
| organ: half-cycle grid from a float clock (1024) | `--organclock`: 2 of 4 |
| organ: a resize or regrid restarts it cold | `--organresize`: 2 of 3 |
| organ: each filament behind the next channel's gel (4096) | `--organcolour`: 1 of 2 |
| organ: device menu valued by index | `--organdevices`: 4 of 10 |
| organ: devices listed with the organ Off | `--coreaudio`: 2 of 3 |

### The mutation

One character of the shipped GLSL, on a committed tree: in `kFilament`'s `sinPi`,
`-1.0 / 6.0` → `-1.0 / 5.0` (the cubic term of the mains waveform's sine, 20% wrong).
Caught by `--steady` (4 of 4), `--rise` (3 of 4: |GPU − double| 104 K against a float bound
of 0.52 K, t90 76.4 ms against 65.2), `--fall` (2 of 3) and `--ripple` (4 of 14, every
"runs the stated schedule" line). `--colour` and `--resize` passed, which is right: neither
compares against the waveform. Reverted with `git checkout source/Shaders.cpp`, rebuilt,
`--rise` 4 of 4 again.

### The organ's mutants (`tools/mutate.sh`)

Twenty one-character mutants of the shipped C++ and GLSL. Each is built in a temporary copy
of the tree with every filament_core and fitest object deleted first (make's one-second
mtime trap). Each runs only the check named against it, at 320x180, under a 600 s perl
alarm. A timeout would count as caught; none timed out. Final run 2026-11-16 on `27fb229`:
**20 of 20 caught**, each in 0–28 s.

| # | mutant | caught by | what failed |
| --- | --- | --- | --- |
| 1 | low-pass b1 with the high-pass's sign | `--crossover` | bass −6.02 dB at 120 Hz for 60 |
| 2 | biquad a2 with α added | `--crossover` | treble corner 5131.7 Hz for 6000 |
| 3 | LR2's Q 0.6 | `--crossover` | bass corner 70.6 Hz for 60 |
| 4 | detector release growing (`-dt` → `+dt`) | `--detector` | τ −100 ms |
| 5 | treble's C10 0.57 µF in `Organ.h` | `--detector` | τ 57 ms against ETI's 47 |
| 6 | gain law 10^(dB/21) | `--detector` | 1.0e-1 relative against 1.2e-7 |
| 7 | devices never listed on the first frame on | `--organdevices` | (b), (d) |
| 8 | organ Off leaves the device open | `--organdevices` | (h) |
| 9 | the organ runs with its switch Off | `--organdevices` | (a), (b). Mapped here, not to `--coreaudio`: see the trap |
| 10 | rows numbered from the wrong half-cycle | `--organframes` | 7 rows differ; 487 K |
| 11 | conduction energy's sine term added | `--firing` | 30°: −190 K |
| 12 | `cosPi` a tenth of a half-cycle off | `--firing` | 30°: −19 K |
| 13 | firing point on a grid of 15 | `--firing` | 30°: −3.6 K |
| 14 | ramp carried across the wrong frames | `--organframes` | 194 K at 30 fps |
| 15 | a substep's first part ending at the wrong place | `--firing` | 150°: +0.75 K against 0.38. **Survived `--organstep`**, see the trap |
| 16 | the organ's RK4 weights over 5 | `--organstep` | 117 K |
| 17 | the ramp's tag wrapped at 2^22 + 1 | `--organclock` | 78 K |
| 18 | gel row interpolation `−` → `+` | `--organcolour` | 2.3 relative |
| 19 | mid filament behind the treble gel | `--organcolour` | 28 relative |
| 20 | status lamp drawn off its corner | `--organdevices` | (c), (d): the corner pixel unlit |

Not mutated: the DC block, the detector's attack, the FIFO and its re-anchoring, and the
gel-table generator (held by `--check` instead). No check measures the first three
either (see What is not verified). The run's output is in the session's scratchpad, not
the repo. Rerun it with `tools/mutate.sh`. Since that run only comments and the
harness's printed resistor names have changed.

---

## The browser demo

`demo/` is the page at **filament-demo.stoatworks-labs.com** (2026-09-25), on the
fleet's kit (`stoatworks-backend/resolume-demo`, vendored by its `sync.sh`).

**The colour organ is not on the page.** Its two programs (`OrganThermal`, `StatusLamp`)
are not spliced, the page has no audio input, and nothing under `demo/` changed with it.
`check_shaders.py` still holds the shared pieces to the C++.

**What is the plugin's.** Every GLSL string of `Shaders.cpp` — the version line, the
vertex body, `kFilament` and the six pass bodies — is spliced into `demo/plugin.js` by
`demo/tools/sync_shaders.py`, tabs and comments included, and assembled as `assemble`
does. So the ODE runs on the GPU as it does here: RK4 substeps, the resistivity table,
c_p, `sinPi`, the waveform through the dimmer law and the Planck lookup, over two
ping-ponged RGBA32F state buffers, the thermal pass writing its two render targets
(WebGL2 needs `EXT_color_buffer_float` for that, and the page refuses to start without
it or without a complete two-target framebuffer, rather than fake it). The same script
copies Lamp.h's constants and tables, Model.h's, the control ranges, option lists and
glass tints, and the 346 rows of `PlanckTable.h`. `demo/tools/check_shaders.py` holds
all of it to the C++ character for character and `tools/verify.sh` runs it; a shader
change here means re-running the sync script, never an edit of the page.

**What is a hand port, checked by nobody but a reader:** Controls.cpp's laws, Lamp.cpp's
`Resistivity`, `HeatCapacity`, `MainsVolts`, `Make` (the wire), `Derivative` and
`Stiffness`, and from `ProcessOpenGL` the clock (dt clamped to [0, 0.25 s], the nominal
first frame), the mains phase reduced in double, the substep rule, the lamp cache, the
fill, the regrid and the pass order. Change one of those here and change the page by hand.
The page says so in its banner and disclosure, and says the three constants (0.30, one
half, 15%) are assumptions.

**What differs, each said on the page:** the clock is the kit's (no unit vote; a paused
page renders dt = 0 frames, the instant's light, as for a stopped host clock; Restart is a
backward clock, which the dt clamp reads as 0 s, so the wall keeps its temperatures);
Columns and Rows are dropdowns (no integer control in the kit); no About block; `Perturb`
and `Probe` are 0; the kit caps one frame at 0.1 s.

**Measured once (2026-09-25).** The page driven frame by frame at n / 60 from a fresh
instance (`window.__filamentDemo.hooks`: `fresh()`, and `afterRender` to read the canvas
inside the frame) on the Colour bars clip at 960x540, against `fitest --pipe --fps 60` on
the same 31 input frames read back from the page: at frames 1, 5 and 30, and for 1000 W,
10 W on a triac, RGB gels behind frosted glass at 64x32, and Pixel Mode at 60 Hz +1.6
stops, every pixel within 1/255. Through ANGLE on Metal, 1 to 3 channel values of
1.56 million differ, by 1; through SwiftShader, up to 11 235, by 1. The Wattage control
moves the picture (frame 5, 40 W against 1000 W: mean |difference| 41 levels).

Deploy: `cf-run npx wrangler deploy` from the repo root, or push to main
(`.github/workflows/deploy.yml`). The host is a Worker **route** over a proxied
`AAAA 100::` record made through the API on 2026-09-25, not a custom domain: the zone
is at Cloudflare's limit of 100. Delete that record and the page goes dark while deploys
stay green. Verify by content:
`curl -s 'https://filament-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Decisions taken without asking

- **The resistivity table** is Forsythe and Worthing (1925) as widely reproduced; the copy
  used is The Physics Factbook's (citing Zerda, TCU 2001), not the 1925 paper. Below 300 K
  it extrapolates the 300–400 K segment (Ambient Temp goes to 0 C).
- **c_p( T ) from NIST-JANAF's Shomate fit**, so C varies with T; the spec's C was constant.
- **Emissivity is a constant 0.30**, the coil radiates from half its surface, 15% of the
  rated power is conducted. Assumptions (round figures, not read from a table), used only
  to size the wire and so the heat capacity. Tungsten's total emissivity rises with T;
  modelling that would make the fall slower still at low T.
- **The filament is a black body** in colour. Real tungsten's spectral emissivity falls
  slowly across the visible, which makes it slightly bluer than a black body at the same T.
- **Mains sets the voltage**: 50 Hz is a 230 V lamp, 60 Hz a 120 V one — the two worlds,
  and it makes Mains move the filament's size as well as its ripple.
- **Every lamp at rated T reads the same brightness.** A 1000 W bulb is not 25x a 40 W one
  in the picture: the wall is exposed for its lamps, and Wattage is dynamics, not a gain.
- **The dimmer laws.** Linear: V_rms = level (a sine-wave dimmer). Square Law: V_rms^2 =
  level, so electrical power roughly follows the fader. Triac: leading-edge phase cut at
  firing angle ( 1 − level ) pi. Every law has the mains waveform; only the triac chops it.
- **The fader is the code value**, the Rec. 709 luma of the source's (encoded) rgb, averaged
  over the cell. Tungsten on a Linear dimmer is so contrasty that mid-greys go dark (light
  goes as ~V^3.4); the default is Square Law, which theatre dimmer curves exist to be.
- **Defaults**, chosen on six of Resolume's demo clips (Trinity, Beat 001, IntoTheGlow_02,
  NeonRoom2_32, Metalive 01, Cyberspace_09) through `--pipe`: 48 x 27, Gap 0.3, clear glass,
  no gels, 40 W, Square Law, 50 Hz, 25 C, Glow 0.5, Bloom 0.35, Exposure 0, Mix 1.
- **Pixel Mode** is read as "each bulb a flat square pixel of its cell", on the same grid.
  The other reading — one filament per output pixel — is an open question below.
- **Gels** are three co-located filaments per cell behind red, green and blue gels, drawn as
  one bulb. The gels' transmittances, like the glass tints, are chosen by eye, not measured
  from filters; a blue gel over tungsten is dim, because tungsten has little blue, and that
  is left in.
- **The light is shutter-averaged** over the frame (see the trap).
- **Output alpha** is `mix( src.a, 1, Mix )`.
- **No randomness.** Every bulb of a real wall differs a little; here they are identical.
- **About and attributions are generated** since registration (2026-09-25):
  `StoatworksAbout.h` by `sync-about.py`, `ATTRIBUTIONS.md` by `sync-attributions.py`, the
  issue forms by `sync-issue-templates.py`. The guide button made it 19 parameters.
- **Test hooks live in the shipped plugin** (`Perturb`, `Probe`, `LastFrameForTest`),
  always inert.

### The colour organ's decisions (2026-11-16)

The spec gives the reasons for each.

- **earshot's waveform tap, not Resolume's FFT.** A crossover and an RC detector need a
  waveform. A corner can only be measured on a signal path. earshot's FFT has no bass bin.
- **Linkwitz–Riley 2nd order for all three bands**, not ETI's RC filters. The IDEAS entry
  asks for a passive three-way, and LR2 has a defined corner. The bands are ETI's: up to
  "the A an octave below middle C", read as 220 Hz, then three octaves to 1760 Hz.
- **The detector's attack through 10k ‖ 100k only.** The filter's output resistance is
  ignored, and the diode is ideal. Attack is 9.1 ms (bass) and 4.3 ms (mid, treble).
- **A 10 Hz DC block**, assumed. No period design states a corner.
- **The trigger is linear in the detector voltage** with no offset (ETI's pre-bias).
- **The slope over each 1/16 half-cycle is the previous 1/16's mean.** That is 0.625 ms of
  lag at 50 Hz, so no frame revises a firing and every frame rate sees the same integral.
- **No latching or holding current, and no minimum firing angle.** A 40 W lamp at 230 V is
  below the BT136's worst-case 15 mA hold only in the last ~2% of the half-cycle.
- **The mapping: three co-located filaments per bulb, with the cell's luma as each bulb's
  sensitivity.** b multiplies the control current, so a dim cell fires later and runs
  redder, not just dimmer. Rejected: three banks (the picture becomes stripes), and tinting
  v0.1.0's lamps (no triac would be driven by the music).
- **Frosted forced while the organ is On**, because the brief says "a frosted panel".
  Glass's tint is kept.
- **The wall's Dimmer and Gels are inert while the organ is On.** The triacs and the
  organ's gels replace them. Measured: 0 pixels differ between their ends.
- **Saturated gels keep their negative sRGB components** until the final encode.
- **Audio Input defaults to None.** A device opens only when chosen, never with the organ
  Off.
- **Gain is in dB of voltage, −12 to +60 dB**, widened so that the defaults (48, 51 and
  46 dB) sit inside it.
- **Defaults set on Resolume's six Beat clips**, the demo media that fill the frame. The
  median clip's 90th-percentile effective control is 2, a 45° firing. The Bass and Synth
  clips are line art and stay mostly dark at these gains.
- **The parameters are appended after About** (19–30). Appending is the only order that
  keeps v0.1.0 compositions intact.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

Every number is `tools/verify.sh` on this machine against a fresh universal Release build,
at 320x180 and 1280x720 and on the software renderer.

- **Steady.** 200 W: at rated voltage 2799.945 K against 2800 (tolerance 0.28 K); at 0.7
  and 0.4 of the voltage 2404.28 and 1862.32 K against a balance of 2404.32 and 1862.40.
- **Colour.** Five temperatures from 1975 to 2816 K: xy on the locus to 1e-6 (tolerance
  1.5–3e-6), luminance to 8e-5 relative, McCamy's CCT the true locus's to 0.04 K; redward
  at every step down.
- **Rise.** 60 W from cold: the GPU on the stated scheme to 0.0015 K, on the ODE to
  0.004 K; t90 65.22 ms, the ODE 65.22; a constant-R lamp 135.29 ms (2.1x slower).
- **Fall.** 60 W off from rated: on the ODE to 0.17 K; 1500 K in 176 ms; tau 2.53x longer at
  1200 K than at 2400 K, as the T^4 model says (a linear cooler: 0.84x).
- **Ripple.** Strongest line exactly 100 Hz at 50 Hz mains and 120 Hz at 60 Hz, for 25 W and
  500 W; 137.2 K vs 18.5 K pp at 50 Hz (7.4x), 71.6 vs 10.1 K at 60 Hz (7.1x); at most 6e-7
  of the power off the harmonics.
- **Resize.** 144 bulbs after an output resize, 312 after a regrid: identical to the
  untouched run's 2168.16 K, difference exactly 0.
- **Negative controls.** All six fail their check. **Mutation** caught (above).
- **No dead controls**, all 14, the four About buttons skipped.
- **Every shader compiles** through `glslc` as the plugin assembles it; no reserved word.
- **The table recomputes** byte for byte from the SHA-checked CMFs.
- **`--pipe`**: two frames for two and a half, exit 2 on an unknown cue, **exit 1 on a closed
  stdout (`| head -c 1`)**, exit 1 on a failed render, an option and a boolean step, a
  slider ramps.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.filament`, ad-hoc signs, and `oxbow` reports `SW Filament` / `FI01` /
  `effect` and renders 120 frames through `plugMain`.
- **Alpha**: 255 everywhere at Mix 1 on Trinity (a clip transparent over 90% of the frame).

### The colour organ, verified by measurement (2026-11-16, same machine)

`tools/verify.sh` passed in full on `27fb229`. The numbers are in the rasteriser table
above. Each was the same at 320x180 and 1280x720 and within its bound on the software
renderer:

- `--crossover`, `--detector`, `--firing`, `--organstep`, `--organframes`, `--organclock`,
  `--organresize`, `--organcolour`, `--organdevices`;
- `--coreaudio`, in its own process;
- `--negative` with all 19 controls.

**The three asked-for claims:**

1. **Crossover corners at the stated frequencies:** 220.0000 Hz and 1760.0458 Hz at the
   defaults, and 59.9993 / 6000.0001 Hz at 60 / 6000. The tolerance is 1.0–1.1e-4
   relative.
2. **Lamp RMS against firing angle follows 1 − α/π + sin 2α/(2π)** at 30–150°, measured
   from the lamp's temperature. Worst 0.00013 off at 90°, tolerance 0.00026.
3. **The step response matches the lamp's ODE:** 0.19 K against 0.76 K, and t90 96.88 ms
   on both.

**Organ off is v0.1.0.**

- verify.sh's `organ off against 91bef09` step builds 91bef09 from `git archive`, Release
  arm64. Against it, every pixel is identical across 13 renders. Six settings cases span
  four rasters (96x54, 320x180, 640x360, 1280x720) and cover every v0.1.0 control off its
  default somewhere. Each is rendered with the organ controls moved while Off and not
  moved. One more is on the software renderer.
- 30 moving frames through `--pipe` give the same 276480 bytes.
- `--list` 0–18 are the same.
- Its negative control: Organ On differs.
- `--coreaudio`: 0 engines, 0 lists and 0 selects with every control at both ends and the
  middle, Audio Input set to two device keys and Rescan pressed. No audio image was added
  (14 at launch) and no audio thread was started. Its positive control, `listDevices()`,
  starts 2 audio threads.

**The rest:**

- **Sweep.** `sweep.py` turns the organ on for its nine sweepable controls, and all 24
  swept parameters move the picture. With the organ Off, those nine are dead, which is
  the sweep's negative control.
- **Mutants.** 20 of 20 caught.
- **`--pipe` with the organ on:** two frames out for two and a half in, and `| head -c 1`
  exits 1.
- **Render cost** (ms/frame, 48 × 27, the synthetic track fed before each frame, the
  feed's time excluded):

  | Raster | Organ on | Organ off |
  | --- | --- | --- |
  | 1280x720 | 0.722 | 0.245 |
  | 1920x1080 | 0.725 | 0.255 |
  | 3840x2160 | 0.749 | 0.386 |

  State held: 0.14 MB on, 0.10 MB off.
- **Every demo WAV** (`/Applications/Resolume Arena/media/AV/`, 18 clips) through the
  organ at the defaults, 7.5 s, on a flat wall at each clip's mean luma:
  - Beat clips: all three lamps swing. Bass p90 − p10 is 1007–2301 K. The treble is steady
    only on Beat 002 (2341–2817 K).
  - Bass and Synth clips (mean luma 0.002–0.008): dark at their mean luma. At the luma of
    their lit cells, Bass 002–006 light bass and mid, and treble stays dark. Synth 002,
    005 and 006 stay dark.
  - The spec has the table.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** Everything numeric was compiled, rendered and
  measured offline against the real plugin class in a headless CGL context, plus an `oxbow` load.
- **Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, 2026-09-25): the DLL
  release.yml built from this source loads from Extra Effects, registers as `SW Filament` /
  `FI01` / effect, all 20 host controls match the declaration
  (`plugin-bench/arena/expect/filament.json`), it renders, Arena's log stays clean, and all 14
  valued controls move the picture: 9 of the gate's 9 checks, one run. Wattage (4.58), Mains
  (4.99) and Ambient Temp (4.62) read weak against a floor of 1.07 (the rest 10 to 55): on a
  still carrier they act only through the settling and the ripple. Software rendering says
  nothing about a GPU or about speed. MSVC compiled it first time (CI run 36147540787).
- **The checks verify the stated model, not tungsten.** They show the GPU integrates Lamp.h's
  ODE, with Forsythe and Worthing's R( T ), JANAF's c_p and the CIE observer, to stated
  bounds. Whether the model's three assumed constants give a real 40 W lamp's time
  constant has not been measured against a lamp. Its orders of magnitude are right.
- **The picture is judged by eye**: the bulb shapes, the coil, the halo, the reflector, the
  bloom, the glass sheen, the glass tints, the gels, the exposure curve. The sweep proves
  each control moves it; nothing measures how.
- **Seen on Resolume's bundled demo clips** (CG loops) through `--pipe`; never on camera
  footage. Thin, dim features vanish: Cyberspace's red lines average to almost nothing over
  a 27-pixel cell, and red has a luma of 0.21.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
- **Not verified at 4K**, only benchmarked there.
- **No OpenFX port, factory presets or seed.** There is a user guide (`docs/USER-GUIDE.md`,
  the only copy anyone edits; the PDF and the site page are generated by the website's
  `build_guides.py`).
- **Filming the release video** (`stoatworks-backend/video/projects/filament/`) found no
  defect. It found that the mains ripple cannot be filmed at 30 fps (the shutter average
  removes it), that a 1000 W wall barely registers a flash, and that Pixel Mode only reads
  at a coarse grid; the guide says all three.

### The colour organ: not verified

- ☠️ **Never loaded into Resolume** on either platform, and never in the Arena gate.
  `oxbow` loads the bundle and renders 120 frames with the organ Off; no host has run it
  On.
- **Never heard a live device.** Every organ check feeds earshot through `feed()` or a
  scripted backend. A real microphone or loopback through CoreAudio, its 30 ms FIFO
  against a real device clock, and its drop and re-anchor paths are untested. So is the
  microphone permission prompt.
- **MSVC has never compiled earshot or this branch.** Windows is untried.
- **The detector's attack** is not measured by any check, only its release. Nor are the
  rectifier's polarity (the negative swing), the 1/16 slope lag beyond frame-rate
  agreement, or the 10 Hz DC block.
- **The organ's light averaged over substeps** is the wall's division by the substep count
  and is not separately checked. `--organcolour` reads a dt = 0 frame.
- **The model, not a light box.** No colour organ, triac or lamp was measured. The 10 Hz
  DC block, the ideal diode, the linear trigger and the omission of latching and holding
  current are assumptions. The gel table's Y at 3200 K is within 0.82 points of LEE's
  printed figures (020: 55.98 against 56.8). Relatively, the worst is 119 Dark Blue
  (2.54 against 2.3, 10%). LEE does not say how it computed its figures.
- **The picture is judged by eye**: the frosted bulb with three gels, the mixes and the
  status lamp's look. The demo pass traced the centre bulb on flat walls and looked at
  five contact sheets. Not every clip was watched through its own video.
- **`ATTRIBUTIONS.md` does not list earshot, miniaudio, LEE's data or ETI 428 yet.** It is
  generated from the backend's master lists, which this branch cannot edit.

---

## Open questions

- **Pixel Mode**: a flat square per bulb (as built), or one filament per output pixel (the
  whole clip behaving like incandescent emitters at full resolution; affordable — a 1080p
  state is 2M filaments x 59 substeps)?
- **The fader**: code value (as built) or linear light? Linear light would make mid-greys
  much darker again.
- **Preheat.** Theatre dimmers hold idle filaments just below glowing to kill the inrush lag;
  a `Preheat` control would be one line in the drive.
- **Emissivity( T )** and tungsten's spectral emissivity: both are published and both would
  move the fall and the colour a little.
- **Lamp-to-lamp variation**: a seeded spread of wattage or resistance would make a wall look
  less perfect.
- **Alpha as light**: an option for the unlit wall to be transparent, so bulbs composite over
  another layer.
- **Gel balance**: leave the blue gel as dim as tungsten makes it, or trim the gels so white
  in reads white out?

### The colour organ's open questions

- **Publishing earshot.** `external/earshot/` is a snapshot of a local repo that has never
  been pushed (`SNAPSHOT`). Merging this branch into this public repo publishes earshot's
  source, its AGENTS.md and its tests.
- **No input, no light.** The organ is dark until an Audio Input is chosen, and the default
  is None. earshot's host-FFT fallback gives band levels, not a waveform. It could drive the
  detectors directly, but then the crossover claims would be about a path that isn't
  running.
- **A Background (preheat) control.** Most period designs had one, and R-E Oct 1969 advises
  biasing lamps just below glow to cut the lag. It would be one constant added to each
  ramp's slope. Today a channel under 1/2 of control is fully dark.
- **The organ's parameters sit after About** in the panel. That is the only safe order for
  v0.1.0 compositions.
- **Forced Frosted.** Should the organ honour Glass's shape too?
- **Dim blue.** 119 passes about a fifth of 106's luminance on tungsten. ETI's answer was
  an extra blue lamp. The options are a second treble filament, a 132 Medium Blue default,
  or leaving it.
- **Dark clips.** The luma mapping leaves line art (Resolume's Bass and Synth clips) almost
  dark. The options are a floor on b, an option to ignore the clip (b = 1), or "raise the
  gain".
- **The gain range** was widened to 60 dB to fit defaults of 46–51 dB. Keep it, or move the
  0 dB point with a fixed make-up gain?
- **The treble settles only on dense hats** (one Beat clip of six). That is the model's
  answer at 40 W. Should the organ default to a bigger lamp?
- **Dimmer and Gels inert with the organ On.** Grey them out in the guide only, as now, or
  have the organ use Dimmer as a master?

---

## Siblings

- **wetplate** — the plugin shape, the generated table with `--check`, the harness, the
  negative controls, verify.
- **toner, wetplate** — the `--pipe` contract with SIGPIPE ignored.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, and the fleet's trap list.
- **photofinish** — the resize-mid-run trap.
- **afterglow, needle** — state across frames, and a physical element's dynamics solved
  from its constants.
- **oxbow** — `oxbow probe` and `oxbow selftest` load this bundle as a host.
