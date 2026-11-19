"""The demo's shaders must be the plugin's shaders, character for character,
and its WebAssembly the plugin's C++ as it is now.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the eight GLSL strings of `source/Shaders.cpp` -- the
version line, the vertex body, the `kFilament` library (the resistivity table's
rule, c_p, `sinPi`, the mains waveform through the dimmer law, the RK4
derivative, the colour lookup) and the six pass bodies -- and the numbers the
lamp and the picture are made of. That is two copies of the same text, and two
copies drift quietly: a demo that renders a *plausible* wall of bulbs looks
exactly like one that renders the right one. The page's whole claim is that the
thermal ODE running in your browser is the plugin's, so the claim needs something
enforcing it. `fitest` drives the real plugin class and has never heard of this
page.

------------------------------------------------------------------- what it does

1. Pulls each `R"( ... )"` body out of Shaders.cpp and each matching backtick
   literal out of plugin.js, and compares them exactly -- no whitespace
   normalisation, no comment stripping. A comment updated on one side only is
   drift worth catching; the comments carry the reasoning (why `sinPi` and not
   `sin`, why two texelFetches and not a filtered read).

   The one transformation is a decode, not a normalisation. A template literal
   cannot hold a raw backtick, backslash or `${`, so `sync_shaders.py` escapes
   those three (kFilament quotes `cycles` in backticks); this undoes exactly
   those three and REJECTS any other backslash on the JS side, which could only
   be somebody hiding a difference in the decoder.

2. Regenerates the rest of the generated block -- Lamp.h's constants and tables,
   Model.h's, the control ranges and option lists, the glass tints and the
   346-row Planck table -- from the C++ and compares it with plugin.js, line for
   line.

3. The colour organ's pieces in `demo/organ.js`: `kOrgan`, `kOrganThermalBody`
   and `kStatusLampBody`, and the rest of its generated block (the 9 x 346 gel
   table from source/GelTable.h), the same two ways.

4. The organ's music: `noiseAt`, `trackAt`, `Wav`, `kFeedLead` and `Audio` in
   demo/wasm/glue.cpp must be tools/fitest/Organ.inc's, character for character,
   so the page feeds the organ exactly what the harness feeds it.

5. No device: no served script or page may name getUserMedia,
   enumerateDevices or mediaDevices. The page never asks for a microphone.

6. The WebAssembly: every file demo/wasm/inputs.sha256 says went into
   demo/filament-organ.wasm (build-wasm.sh writes it from the compiler's own
   dependency lists) must still hash the same, both outputs included, and no
   earshot source may have appeared since. A stale .wasm cannot be seen any
   other way without emscripten: rerun demo/tools/build-wasm.sh.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORT (demo/tools/crosscheck.py compares the page with
fitest once, by hand: AGENTS.md has what it found). `watts`, `ambientKelvin`, `makeLamp`,
`stiffness`, the substep rule, the clock and the mains phase, the lamp cache and
the pass order in plugin.js are a hand translation of Controls.cpp, Lamp.cpp and
Filament::ProcessOpenGL, and only a reader can tell whether they still agree.
Change one of those and change the page by hand to match.

If this fails because a shader changed in the plugin: `python3 demo/tools/sync_shaders.py`,
never an edit of plugin.js by hand.
"""
import hashlib
import os
import re
import sys

# No __pycache__ beside the page's sources.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_shaders  # noqa: E402

REPO = sync_shaders.REPO

# tools/fitest/Organ.inc's music and feeder, which demo/wasm/glue.cpp copies.
HARNESS_FUNCTIONS = ["noiseAt", "trackAt"]
HARNESS_STRUCTS = ["Wav", "Audio"]
HARNESS_LINES = ["constexpr double kFeedLead = 0.010;"]

DEVICE_APIS = ("getUserMedia", "enumerateDevices", "mediaDevices")

MANIFEST = os.path.join("demo", "wasm", "inputs.sha256")
OUTPUTS = ("demo/filament-organ.js", "demo/filament-organ.wasm")
# earshot's miniaudio backend is never compiled (EARSHOT_CAPTURE=OFF).
NOT_COMPILED = ("Miniaudio.cpp", "MiniaudioImpl.cpp", "MiniaudioConfig.h")


def from_js(source, name):
    match = re.search(r"^const " + name + r" = `(.*?)`;$", source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?![`\\$])", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, backslash or dollar, at line {upto.count(chr(10)) + 1}"
    decoded = re.sub(r"\\([`\\$])", r"\1", body)
    return decoded, None


def compare_pieces(js, where, pieces, shaders_cpp):
    problems = 0
    for name, symbol in pieces:
        cpp_text = sync_shaders.cpp_shader(shaders_cpp, symbol)
        js_text, complaint = from_js(js, name)
        if complaint is not None:
            print(f"FAIL  {name} in {where} has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in {where}")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        a_lines, b_lines = cpp_text.splitlines(), js_text.splitlines()
        for i in range(max(len(a_lines), len(b_lines))):
            a = a_lines[i] if i < len(a_lines) else "<missing>"
            b = b_lines[i] if i < len(b_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break
    return problems


def compare_block(js, where, want_text, what):
    region = sync_shaders.region(js)
    if region is None:
        print(f"FAIL  {where} has no generated block")
        return 1
    have = js[region[0]: region[1]].splitlines()
    want_lines = want_text.splitlines()
    if have == want_lines:
        print(f"ok    the generated block of {where} matches source/ ({what})")
        return 0
    for i in range(max(len(have), len(want_lines))):
        a = want_lines[i] if i < len(want_lines) else "<missing>"
        b = have[i] if i < len(have) else "<missing>"
        if a != b:
            print(f"FAIL  the generated block of {where} differs from source/ at its line {i + 1}")
            print(f"          source: {a[:120]}")
            print(f"          js    : {b[:120]}")
            break
    return 1


#---------------------------------------------------------------------------
# The organ's music, and no device.
#---------------------------------------------------------------------------

def function_text(source, name):
    """A top-level C++ function: from the line that declares `name(` at the
    start of a line (its return type first) to the first `}` at the start of
    a line after it."""
    match = re.search(r"^[A-Za-z_][\w:<>&* ]*\b" + name + r"\(.*?^\}$", source, re.S | re.M)
    return None if match is None else match.group(0)


def struct_text(source, name):
    """A top-level struct: from `struct Name` at the start of a line to the
    first `};` at the start of a line after it."""
    match = re.search(r"^struct " + name + r"$.*?^\};$", source, re.S | re.M)
    return None if match is None else match.group(0)


def check_harness(harness, glue):
    problems = 0
    pieces = [(n, function_text) for n in HARNESS_FUNCTIONS] + [(n, struct_text) for n in HARNESS_STRUCTS]
    for name, extract in pieces:
        theirs, ours = extract(harness, name), extract(glue, name)
        if theirs is None:
            print(f"FAIL  {name} not found in tools/fitest/Organ.inc")
            problems += 1
        elif ours is None:
            print(f"FAIL  {name} not found in demo/wasm/glue.cpp")
            problems += 1
        elif ours == theirs:
            print(f"ok    {name:<18} matches tools/fitest/Organ.inc's ({len(ours)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} in demo/wasm/glue.cpp has drifted from tools/fitest/Organ.inc's")
            a_lines, b_lines = theirs.splitlines(), ours.splitlines()
            for i in range(max(len(a_lines), len(b_lines))):
                a = a_lines[i] if i < len(a_lines) else "<missing>"
                b = b_lines[i] if i < len(b_lines) else "<missing>"
                if a != b:
                    print(f"        first difference at line {i + 1}")
                    print(f"          fitest: {a}")
                    print(f"          glue  : {b}")
                    break
    for line in HARNESS_LINES:
        missing = [where for where, text in (("tools/fitest/Organ.inc", harness), ("demo/wasm/glue.cpp", glue))
                   if line not in (l.strip() for l in text.splitlines())]
        if missing:
            print(f"FAIL  `{line}` is not in {' or '.join(missing)}")
            problems += 1
        else:
            print(f"ok    {line}")
    return problems


def check_no_devices():
    problems = 0
    demo = os.path.join(REPO, "demo")
    served = []
    for root, dirs, files in os.walk(demo):
        rel = os.path.relpath(root, demo)
        if rel.split(os.sep)[0] in ("tools", "wasm"):
            continue  # not served (.assetsignore), and this file names them
        for name in files:
            if name.endswith((".js", ".html")):
                served.append(os.path.join(root, name))
    for path in sorted(served):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for api in DEVICE_APIS:
            if api in text:
                print(f"FAIL  {os.path.relpath(path, REPO)} names {api}: the page must never ask for or list an audio device")
                problems += 1
    if problems == 0:
        print(f"ok    NO DEVICES         none of the {len(served)} served scripts and pages names {', '.join(DEVICE_APIS)}")
    return problems


#---------------------------------------------------------------------------
# The WebAssembly.
#---------------------------------------------------------------------------

def sha256(path):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, path), "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_wasm():
    try:
        with open(os.path.join(REPO, MANIFEST)) as handle:
            lines = handle.read().splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1
    problems = 0
    recorded = set()
    for line in lines:
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        recorded.add(path)
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            print(f"FAIL  {path} has changed since demo/filament-organ.wasm was built -- rerun demo/tools/build-wasm.sh")
            problems += 1
    # An earshot file added since the build is an input the build never saw.
    for tree in (os.path.join("external", "earshot", "src"), os.path.join("external", "earshot", "include", "earshot")):
        for name in sorted(os.listdir(os.path.join(REPO, tree))):
            rel = f"{tree}/{name}".replace(os.sep, "/")
            if name.endswith((".h", ".cpp")) and name not in NOT_COMPILED and rel not in recorded:
                print(f"FAIL  {rel} is newer than the .wasm's manifest -- rerun demo/tools/build-wasm.sh")
                problems += 1
    for output in OUTPUTS:
        if output not in recorded:
            print(f"FAIL  {MANIFEST} does not record {output}")
            problems += 1
    if problems == 0:
        print(f"ok    WASM               built from these sources ({len(recorded)} files hashed, both outputs among them)")
    return problems


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    with open(sync_shaders.ORGAN_JS) as handle:
        organ_js = handle.read()
    shaders_cpp = sync_shaders.read("source/Shaders.cpp")

    problems = 0

    # The version line assemble() prepends.
    want = sync_shaders.cpp_version(shaders_cpp)
    got = re.search(r'^const K_VERSION = "(.*?)";$', js, re.M)
    if got is None or got.group(1) != want:
        print("FAIL  K_VERSION is not Shaders.cpp's kVersion")
        problems += 1
    else:
        print(f"ok    {'K_VERSION':<18} matches kVersion")

    problems += compare_pieces(js, "demo/plugin.js", sync_shaders.SHADERS, shaders_cpp)
    want = sync_shaders.block()
    rows = sum(1 for line in want.splitlines() if re.search(r"// \d+ K$", line))
    problems += compare_block(js, "demo/plugin.js", want, f"Lamp.h, Model.h, Controls, Organ.h, {rows} Planck rows")

    problems += compare_pieces(organ_js, "demo/organ.js", sync_shaders.ORGAN_SHADERS, shaders_cpp)
    want = sync_shaders.organ_block()
    rows = sum(1 for line in want.splitlines() if re.search(r"// \d+ K$", line))
    problems += compare_block(organ_js, "demo/organ.js", want, f"GelTable.h, {rows} gel rows")

    problems += check_harness(sync_shaders.read("tools/fitest/Organ.inc"), sync_shaders.read("demo/wasm/glue.cpp"))
    problems += check_no_devices()
    problems += check_wasm()

    print()
    if problems:
        print(f"{problems} problem(s) -- run python3 demo/tools/sync_shaders.py for the GLSL and tables and"
              " demo/tools/build-wasm.sh for the .wasm; never edit plugin.js's or organ.js's generated blocks by hand")
        return 1
    print(f"all {len(sync_shaders.SHADERS) + len(sync_shaders.ORGAN_SHADERS) + 1} shader pieces and every copied table are"
          f" identical to the plugin's, the glue's music is fitest's, no served file names a device API, and the .wasm"
          " is built from the current sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
