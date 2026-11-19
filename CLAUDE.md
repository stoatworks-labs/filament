# filament

The picture on a wall of incandescent bulbs — tungsten filaments with their own heat
balance — as an FFGL **effect** for Resolume Arena/Avenue. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the lamp model, the substep rule, the colour table or
the probes.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/fitest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one)
- Set anything by name: `--set "Wattage=0.5" --set "Dimmer=2" --set "Columns=64"`
  (0..1 for sliders, the real integer for Columns and Rows, the element index for
  options, 0/1 for Pixel Mode)
- List parameters, kinds, defaults and ranges: `./build/fitest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/fitest --dump-shaders DIR`
- Recompute the Planck table: `python3 tools/planck_table.py` (`--print` to see rows,
  `--check` to compare with the committed `source/PlanckTable.h`)
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between
  cues; an option, a boolean and an integer STEP (they hold the last cue at or before
  the frame); an event fires on its cue frame only. A cue naming no parameter is refused
  with exit 2, a partial frame at the end of stdin ends the stream cleanly, a failed
  render or a closed stdout exits 1 (SIGPIPE is ignored, so never 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/fitest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg …`
- **The colour organ** (`--set Organ=1`): the harness feeds earshot through `feed()`
  before every frame and never opens a device. Default music is a synthetic 120 BPM track;
  `--wav PATH` plays a file (looped), `--silent` zeros. `--trace PATH` (with `--out`) writes
  each frame's firing angles and controls, `--trace-state` the centre bulb's three
  filament temperatures instead.
- Recompute the gel table: `python3 tools/gel_table.py` (`--check`, `--print` for LEE's
  printed 3200 K figures beside ours)

## Verify
- Everything: `tools/verify.sh` (fresh universal build + the table recompute + glslc +
  the reserved-word grep + every check at 320x180 AND 1280x720 AND on the software
  renderer + --pipe + the sweep + the bundle, ~4 min on this Mac)
- Constant RMS settles at the power balance, rated at 2800 K: `./build/fitest --steady`
- The light sits on the Planckian locus; dimming is redward: `./build/fitest --colour`
- A cold start follows R( T ), faster than constant R: `./build/fitest --rise`
- Switch-off follows T^4 + conduction, not an exponential: `./build/fitest --fall`
- Triac ripple at exactly 2 x mains, bigger on a small lamp: `./build/fitest --ripple`
- An output resize and a grid change keep the temperatures: `./build/fitest --resize`
- The checks can fail: `./build/fitest --negative`; one perturbation verbosely:
  `./build/fitest --perturb BITS --rise` (bits in `Lamp.h`)
- Every check takes `--size WxH`; CI runs them at 320x180
- CI's renderer, on this Mac: `FITEST_RENDERER=software ./build/fitest --ripple --size 320x180`
  (Apple's software renderer, not repeatable at the last bit; verify.sh runs every check on it)
- The organ, rendered (each takes `--size`; verify runs them with the rest):
  `--firing` (lamp RMS against firing angle, the closed form), `--organstep` (a step from
  cold, the lamp's ODE), `--organframes` (30/60/144 fps), `--organclock` (5.8 days in),
  `--organresize`, `--organcolour` (each filament's gel), `--organdevices` (the menu and
  lamp, scripted backend)
- The organ without GL: `--crossover` (each corner's −6.02 dB point), `--detector`
  (ETI 428's release times, the gain law)
- **Organ off never reaches CoreAudio**: `./build/fitest --coreaudio`, REAL backend, alone
  in its process (verify runs it so)
- Mutants of the organ, one character each, each under a 600 s alarm:
  `tools/mutate.sh` (all 20, ~4 min) or `tools/mutate.sh 4 15` (by index)
- No name over 16 characters, none duplicated: `./build/fitest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost, substeps and the state held: `./build/fitest --bench`
  (`--set Columns=160 --set Rows=90` for the largest grid, `--set Wattage=0` for the most substeps)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Filament.bundle`

## Notes
- **The bulb is a wire, not a fade.** `Lamp.h` states the ODE and sizes the wire from
  the wattage; `kFilament` in `Shaders.cpp` runs it. The CPU-side `Lamp.cpp` is the same
  model in double, and the harness's reference. A wrong filament is a fix in both.
- **The colour table is generated.** `source/PlanckTable.h` is written by
  `tools/planck_table.py` from `tools/data/ciexyz31_1.csv` (CVRL's copy of the CIE 1931
  observer, SHA-256 checked); edit the script, never the header. `verify.sh` refuses a
  header that does not recompute.
- **Time is reduced in double on the CPU.** The frame's seconds and the mains phase at
  its start (cycles, fractional) are the only times the shaders see. Resolume's clock is
  ~5e8 ms, where a float resolves tens of milliseconds and a 50 Hz waveform needs 0.1.
- **The substep is the stiffer of two rules**: 20 per mains half-cycle, and 1.5 / the
  lamp's stiffest |d(dT/dt)/dT| (a cold filament at the peak of the cycle). For small
  lamps the second wins: 10 W needs 147 substeps a frame at 60 fps, 1000 W needs 34.
- **The temperatures live at the grid's raster**, so an output resize never touches
  them; a Columns/Rows change regrids them. `--resize` checks both.
- **The light is shutter-averaged**: the thermal pass writes the mean of the substeps'
  light alongside the temperatures, as a camera's open shutter would. The probes read the
  instant.
- **Output alpha is 1 at Mix 1** (`mix( src.a, 1, Mix )`): the wall paints the whole
  frame. Resolume's demo clips carry alpha.
- **`Perturb` bits and `Probe` are test hooks**, always 0 in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses.
  `FF_TYPE_INTEGER` is exempt: Columns and Rows hold real values. Options map by index.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `filament_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include it by hand.
- macOS build must be universal. Verify with `lipo`, never the build log.
- GLSL 4.10 reserved words are not identifiers: `patch sample input output filter
  common active half layout flat packed` and the rest of s3.6; `verify.sh` greps the
  dumped shaders for them. MSVC has no `M_PI` (use `lamp::kPi`) and `far`/`near` are
  macros there.
- FFGL id is `FI01`, display name `SW Filament`.

### The colour organ (parameters 19–30, after v0.1.0)
- **Append parameters, never renumber.** v0.1.0 declared 0–18; a saved composition stores
  nothing at 19+, so it loads with the organ Off. verify.sh compares `--list` 0–18 and the
  organ-off picture and `--pipe` bytes against `ORGAN_OFF_REF` (default 91bef09), built
  from `git archive`.
- **Only `source/Organ.cpp` includes earshot** (`external/earshot/`, an UNPATCHED snapshot:
  see its `SNAPSHOT`; never edit it). The Engine, the device list and the organ's GL are
  built on the first frame drawn with the organ On, never in the constructor:
  `listDevices` and `selectDevice` both start CoreAudio.
- `FILAMENT_NO_DEVICES=1` refuses every real device (the menu shows None and System
  default only). The harness installs a scripted backend for everything but `--coreaudio`.
- The Audio Input menu is valued by **device key** (earshot::deviceKey, a hash of the
  name), not by index, so a saved device is found by name after the list changes.
- The organ's `Perturb` bits are in `Organ.h` (64 and up), above Lamp.h's.
- `source/GelTable.h` is GENERATED by `tools/gel_table.py` from LEE's spectra
  (`tools/data/lee_gels.csv`, SHA-256 checked); edit the script, never the header.
- With the organ On, Dimmer and the wall's Gels are inert (the triacs and the organ's gels
  replace them) and Glass's shape is forced Frosted (its tint still applies).

## Not done yet
- **In Resolume on macOS only registered and compared** (Arena 7.29.0, 2026-11-19: every
  param as declared, organ Off as v0.1.0, organ On with no input dark). Everything numeric
  is measured offline against the real plugin class in a headless CGL context, plus an
  `oxbow` load. On Windows v0.1.0 passed the fleet's Arena gate (Arena 7.27.1, llvmpipe),
  9/9 (see AGENTS.md).
- No OpenFX port, no factory presets.
- **The colour organ (v0.2.0)** has never heard a live audio device (every check feeds
  earshot through `feed()` or a scripted backend). It is compiled into the Windows DLL and
  has never run there.
  `specs/SPEC-filament-organ.md` (outside the repo) is its spec, with every source.
- `StoatworksAbout.h`, `ATTRIBUTIONS.md` and `.github/ISSUE_TEMPLATE/` are GENERATED by the
  backend's sync scripts; the user guide is `docs/USER-GUIDE.md` (PDF generated).

## Browser demo

`demo/` is the page at **filament-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` (a Worker route over a proxied `AAAA 100::` DNS record, not a
custom domain) with `cf-run npx wrangler deploy` or by any push to main — no build
step; what is committed is what is served. `demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh filament` and
is not a place to edit.
- **A shader or table change in the plugin: `python3 demo/tools/sync_shaders.py`**
  (splices Shaders.cpp's strings, Lamp.h's, Model.h's and Controls' constants and the
  Planck table into `demo/plugin.js`), then `python3 demo/tools/check_shaders.py`
  (in verify.sh). Never hand-edit the generated block.
- The page's CPU half (Controls.cpp's laws, Lamp.cpp's `Make`/`Stiffness`, the clock
  and substep rule of `ProcessOpenGL`) is a **hand port**; change it by hand with
  the C++. Only a reader checks it.
- Verify a deploy **by content**:
  `curl -s 'https://filament-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/filament/filament.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\filament\logs\filament.YYYY-MM-DD.log   (Windows)

Each lamp change logs the wire the model sized (diameter, length, mass) and its stiffness.
