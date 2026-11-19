"""The browser demo against the plugin: the colour organ, and the Organ Off.

    python3 demo/tools/crosscheck.py --fitest build/fitest [--keep DIR] [--only TEXT]

Needs a GPU, Google Chrome, numpy, the Mac's clang, a built fitest (from the
same source/ as the page's .wasm), and stoatworks-backend's
`release/cdpshot.py` (CDPSHOT=path overrides ~/Projects/infrastructure/
stoatworks-backend/release/cdpshot.py). Not run by tools/verify.sh. FILAMENT_
NO_DEVICES=1 is set for every fitest run; nothing here opens an audio device.
(woofer's crosscheck, with filament's driven clock: AGENTS.md, "The browser
demo".)

The page is served from here on a free port and loaded in headless Chrome on
the real GPU (ANGLE on Metal) at ?size=960x540&clip=bars, paused, and driven
one frame at a time from a fresh instance (`window.__filamentDemo.hooks`:
`fresh()`, then the clock set to n / 60 and one frame drawn, read back inside
`afterRender`), as `fitest --pipe --fps 60` drives the plugin. Four checks:

  1. ORGAN OFF IS v0.1.0, bit for bit. The page before the organ came to it
     (demo/ at the parent of the commit that added demo/organ.js, or --before
     REF) and this page, driven the same, the Organ Off: every byte of the
     canvas at frames 1, 5 and 30, for the defaults and the cases v0.1.0's
     measurement used. And neither organ.js nor the WebAssembly was fetched.

  2. THE ROWS: the page's WebAssembly against the same C++ built natively.
     Every organ frame's arguments (start, now, hertz, the half-cycle number,
     the row count, the five host values) are recorded from the page with the
     rows its Organ::Advance returned, and replayed through
     demo/tools/organ_rows.cpp, compiled here with clang beside source/Organ.cpp,
     earshot and demo/wasm/glue.cpp itself. Every float compared, by its bits.

  3. THE HARNESS'S TRACE: `fitest --out --trace` (the real plugin, fed the
     same track by the same feeder) prints, per frame, each channel's firing
     angle for a white cell and its control, averaged over the half-cycles that
     ended in the frame. The same numbers are computed from the page's rows, by
     the same arithmetic, and compared as fitest prints them.

  4. THE PICTURE: `fitest --pipe --fps 60 --size 960x540` fed the page's own
     input frame (the clip is still: checked), with the same settings by the
     plugin's names, against the page's canvas at the same frames. The organ's
     defaults case sets only Organ=1, so the plugin's own constructor defaults
     meet the page's. A negative control -- Bass Gain one slider step (0.001)
     off -- must differ from the page by more than the defaults case does.

Also: the page's ports of Organ.cpp's laws (the readouts and the defaults)
against the compiled ones in the page's own module (fo_law, fo_law_inverse),
over a sweep of the host's floats; and the page's console.

Exit 1 when the Organ Off is not v0.1.0's bit for bit, a picture is past the
mark (max 8 codes, 1% of values off -- the v0.1.0 cross-check's 1 code on the
wall is the expectation, the organ's lamps add a few), the trace differs, the
negative control passes, or the page logs an error or a WebGL warning. The
rows' agreement is reported, not judged: they need not be bit-identical
(WebAssembly has no fused multiply-add and its libm is musl's).
"""
import argparse
import base64
import functools
import http.server
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

import numpy as np

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))

CDPSHOT = os.environ.get("CDPSHOT", os.path.expanduser("~/Projects/infrastructure/stoatworks-backend/release/cdpshot.py"))
sys.path.insert(0, os.path.dirname(CDPSHOT))
import cdpshot  # noqa: E402

# The real GPU: SwiftShader rounds differently and is slow; --disable-gpu has no WebGL.
cdpshot.GL_FLAGS = ["--use-gl=angle", "--use-angle=metal", "--ignore-gpu-blocklist"]

W, H = 960, 540
FPS = 60
KEEP = (1, 5, 30)

# Organ Off: name, settings by the page's ids, frames to drive.
OFF_CASES = [
    ("defaults", {}, 31),
    ("1000 W", {"wattage": None}, 31),  # filled in from the page's own law
    ("10 W on a triac, RGB gels, frosted, 64 x 32", {"wattage": None, "dimmer": 2, "gels": 1, "glass": 1, "columns": 60, "rows": 30}, 31),
    ("Pixel Mode at 60 Hz, +1.6 stops", {"pixelMode": 1, "mains": 1, "exposure": 0.7}, 31),
]

# The organ: name, page settings, the Audio, frames, the frames compared.
ORGAN_CASES = [
    ("organ defaults, the test track", {"organ": 1}, "track", 121, (1, 5, 30, 60, 120)),
    ("16 x 9, 150 W, Status Lamp Off", {"organ": 1, "columns": 12, "rows": 7, "wattage": None, "statusLamp": 0}, "track", 91, (5, 30, 90)),
    ("gels 105/020/132, +60 dB, 100 Hz / 4 kHz, 60 Hz mains", {"organ": 1, "bassGel": 6, "midGel": 7, "trebleGel": 3,
     "bassGain": 1.0, "midGain": 1.0, "trebleGain": 1.0, "crossoverLow": None, "crossoverHigh": None, "mains": 1}, "track", 61, (5, 30, 60)),
    ("silence: the lamp goes steady red", {"organ": 1}, "silence", 200, (30, 199)),
]

PRESERVE = """
(() => {
  const real = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (kind, attrs) {
    if (kind === 'webgl2') attrs = Object.assign({}, attrs || {}, { preserveDrawingBuffer: true });
    return real.call(this, kind, attrs);
  };
})();
"""

# One driven run: from a fresh instance, frames 0..count-1 at n / 60, the
# canvas read inside the frame at `keep`, the input at the first and the last,
# and (with the organ) every organ frame's arguments and rows.
DRIVE = r"""(async (spec) => {
  const d = window.__filamentDemo;
  const raf = () => new Promise((r) => requestAnimationFrame(() => r()));
  const b64 = (bytes) => { let s = ''; for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000)); return btoa(s); };
  const readCanvas = (gl) => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight, px = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    return { w, h, b64: b64(px) };
  };
  const readInput = (gl, input) => {
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, input.texture, 0);
    const px = new Uint8Array(input.width * input.height * 4);
    gl.readPixels(0, 0, input.width, input.height, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fbo);
    return { w: input.width, h: input.height, b64: b64(px) };
  };
  d.demo.state.playing = false;
  await raf(); await raf();
  if (d.audio) d.audio.id = spec.audio;
  d.demo.params.apply(spec.params);
  if (spec.params.organ === 1) {
    // Build the organ first (a fetch), then start again from a fresh instance.
    const t0 = performance.now();
    while (d.telemetry.organ !== 'ready') {
      if (d.telemetry.organ === 'failed') throw new Error(`the organ failed: ${d.telemetry.organError}`);
      if (performance.now() - t0 > 20000) throw new Error('the organ never loaded');
      d.demo.redraw(); await raf();
    }
  }
  d.hooks.fresh();
  const rec = [];
  const organ = d.hooks.organ ? d.hooks.organ() : null;
  if (organ) {
    const cpu = organ.cpu;
    if (!cpu.__real) cpu.__real = cpu.frame;
    cpu.frame = function (a) {
      const n = cpu.__real.call(cpu, a);
      const rows = cpu.rows();
      rec.push({ start: a.start, now: a.now, hertz: a.hertz, firstHalf: a.firstHalf, halves: a.halves, values: a.values,
                 count: n, gotFirstHalf: cpu.firstHalf(), lamp: cpu.lamp(),
                 rows: b64(new Uint8Array(rows.buffer, rows.byteOffset, rows.byteLength)) });
      return n;
    };
  }
  const out = { canvases: {}, inputs: {}, dts: [], stats: null };
  for (let n = 0; n < spec.count; n += 1) {
    d.demo.state.time = n / 60;
    await new Promise((resolve) => {
      d.hooks.afterRender = (gl, input) => {
        d.hooks.afterRender = null;
        if (spec.keep.includes(n)) out.canvases[n] = readCanvas(gl);
        if (n === 0 || n === spec.count - 1) out.inputs[n] = readInput(gl, input);
        resolve();
      };
      d.demo.redraw();
    });
    out.dts.push(d.telemetry.dt);
  }
  if (organ) {
    organ.cpu.frame = organ.cpu.__real;
    out.stats = organ.cpu.stats();
    out.lamp = organ.cpu.lamp();
  }
  out.rec = rec;
  out.fetched = performance.getEntriesByType('resource').map((e) => e.name).filter((n) => /organ/.test(n));
  return out;
})(%s)"""

# The page's settings in the plugin's terms, by name: an option's element
# index, Columns and Rows the integer, a standard parameter its float.
HOST = r"""(() => {
  const d = window.__filamentDemo;
  const out = [];
  for (const p of d.demo.params.spec) {
    let v = d.demo.params.get(p.id);
    if (p.id === 'columns') v = 4 + v;
    else if (p.id === 'rows') v = 2 + v;
    out.push([p.name, p.type === 'standard' ? Math.fround(v) : v]);
  }
  return out;
})()"""

LAWS = r"""(() => {
  const d = window.__filamentDemo;
  const m = d.hooks.organ().module;
  const L = d.laws;
  const pairs = [[L.crossoverLowHertz, L.crossoverLowParam, 0], [L.crossoverHighHertz, L.crossoverHighParam, 1], [L.gainDb, L.gainParam, 2]];
  const out = { forward: [], inverse: [], defaults: [] };
  for (const [law, inverse, which] of pairs) {
    let worst = 0, same = 0, n = 0;
    for (let i = 0; i <= 1000; i += 1) {
      const v = Math.fround(i / 1000);
      const js = law(v), cpp = m._fo_law(which, v);
      worst = Math.max(worst, Math.abs(js - cpp) / Math.max(Math.abs(cpp), 1e-300));
      same += js === cpp; n += 1;
    }
    out.forward.push({ which, worst, same, n });
    let isame = 0, ni = 0;
    for (let i = 0; i <= 1000; i += 1) {
      const x = law(Math.fround(i / 1000));
      isame += inverse(x) === m._fo_law_inverse(which, x); ni += 1;
    }
    out.inverse.push({ which, same: isame, n: ni });
  }
  for (const [id, which, real] of [['crossoverLow', 0, 220], ['crossoverHigh', 1, 1760], ['bassGain', 2, 48], ['midGain', 2, 51], ['trebleGain', 2, 46]]) {
    out.defaults.push({ id, page: d.demo.params.defaults[id], cpp: m._fo_law_inverse(which, real) });
  }
  return out;
})()"""


class Page(cdpshot.Chrome):
    """cdpshot's Chrome, keeping the events it would discard."""

    def __init__(self):
        self.events = []
        super().__init__()
        self.call("Runtime.enable")
        self.call("Log.enable")
        self.call("Page.addScriptToEvaluateOnNewDocument", {"source": PRESERVE})

    def call(self, method, params=None):
        self.msg_id += 1
        want = self.msg_id
        self.ws.send(json.dumps({"id": want, "method": method, "params": params or {}}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == want:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg)

    def run(self, expression):
        r = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in r:
            raise RuntimeError(f"the page threw: {json.dumps(r['exceptionDetails'])[:600]}")
        return r.get("result", {}).get("value")

    def load(self, url):
        self.call("Page.navigate", {"url": url})
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                if self.run("Boolean(window.__filamentDemo && window.__filamentDemo.telemetry.ticked)"):
                    return
            except RuntimeError:
                pass
            time.sleep(0.25)
        raise RuntimeError(f"{url} drew no frames")

    def problems(self):
        out = []
        for e in self.events:
            m, p = e["method"], e.get("params", {})
            if m == "Runtime.exceptionThrown":
                out.append(json.dumps(p.get("exceptionDetails", {}))[:300])
            elif m == "Runtime.consoleAPICalled" and p.get("type") in ("error", "warning", "assert"):
                out.append(" ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", []))[:300])
            elif m == "Log.entryAdded" and p.get("entry", {}).get("level") in ("error", "warning"):
                out.append(f"{p['entry'].get('source')}: {p['entry'].get('text')}"[:300])
        return out


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=directory))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}/"


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args], capture_output=True, check=True).stdout


def before_ref(given):
    if given:
        return given
    added = git("log", "--diff-filter=A", "--format=%H", "--", "demo/organ.js").decode().split()
    return f"{added[-1]}^" if added else "HEAD"


def extract_demo(ref, into):
    data = git("archive", "--format=tar", ref, "demo")
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(into, filter="data")
    return os.path.join(into, "demo")


def image(entry):
    """A readPixels buffer (rows bottom first) as rows top first."""
    a = np.frombuffer(base64.b64decode(entry["b64"]), np.uint8).reshape(entry["h"], entry["w"], 4)
    return a[::-1].copy()


def fitest_env():
    env = dict(os.environ)
    env["FILAMENT_NO_DEVICES"] = "1"
    return env


def fitest_pipe(binary, frame, count, sets, audio, keep):
    args = [binary, "--pipe", "--fps", str(FPS), "--size", f"{W}x{H}"] + (["--silent"] if audio == "silence" else [])
    for name, value in sets:
        args += ["--set", f"{name}={value:.9g}" if isinstance(value, float) else f"{name}={value}"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=fitest_env())
    raw = frame.tobytes()

    def feed():
        try:
            for _ in range(count):
                proc.stdin.write(raw)
            proc.stdin.close()
        except BrokenPipeError:
            pass

    writer = threading.Thread(target=feed, daemon=True)
    writer.start()
    size = len(raw)
    frames, got = {}, 0
    while True:
        chunk = proc.stdout.read(size)
        if not chunk:
            break
        if len(chunk) < size:
            chunk += proc.stdout.read(size - len(chunk))
        if len(chunk) == size:
            if got in keep:
                frames[got] = np.frombuffer(chunk, np.uint8).reshape(H, W, 4)
            got += 1
    writer.join()
    err = proc.stderr.read().decode()
    if proc.wait() != 0 or got != count:
        raise RuntimeError(f"fitest {' '.join(args[1:])[:300]}: {got} of {count} frames; {err[-400:]}")
    return frames


def compare(a, b):
    diff = np.abs(a.astype(np.int16) - b.astype(np.int16))
    rgb = diff[..., :3]
    return int(rgb.max()), int((rgb > 0).sum()), rgb.size, float(rgb.mean()), int(diff[..., 3].max())


#---------------------------------------------------------------------------
# The rows: the native build, and fitest's trace arithmetic.
#---------------------------------------------------------------------------

def build_native(into):
    stub = os.path.join(into, "stub", "emscripten")
    os.makedirs(stub, exist_ok=True)
    with open(os.path.join(stub, "emscripten.h"), "w") as handle:
        handle.write("#define EMSCRIPTEN_KEEPALIVE\n")
    es = os.path.join(REPO, "external", "earshot")
    names = "Capture Engine Fft HostFft Onsets Partials Pitch Stereo Tap Tempo ThirdOctave NoCapture".split()
    sources = [os.path.join(REPO, "source", f"{n}.cpp") for n in ("Organ", "Diag")]
    sources += [os.path.join(es, "src", f"{n}.cpp") for n in names]
    sources += [os.path.join(REPO, "demo", "wasm", "glue.cpp"), os.path.join(HERE, "organ_rows.cpp")]
    binary = os.path.join(into, "organ_rows")
    cmd = ["/usr/bin/clang++", "-std=c++17", "-O3", "-DNDEBUG", "-arch", "arm64",
           "-I" + os.path.join(REPO, "source"), "-I" + os.path.join(es, "include"), "-I" + os.path.join(es, "src"),
           "-I" + os.path.join(into, "stub"), *sources, "-o", binary]
    subprocess.run(cmd, check=True, capture_output=True)
    return binary


def replay(binary, source, rec):
    lines = [f"source {source}"]
    for r in rec:
        v = r["values"]
        lines.append(f"{r['start']:.17g} {r['now']:.17g} {r['hertz']:.17g} {r['firstHalf']:.17g} {r['halves']}"
                     f" {v['low']:.9g} {v['high']:.9g} {v['bass']:.9g} {v['mid']:.9g} {v['treble']:.9g}")
    out = subprocess.run([binary], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True, env=fitest_env()).stdout
    frames, it = [], iter(out.splitlines())
    for head in it:
        _, rows, _, first, _, lamp = head.split()
        data = [np.array([int(x, 16) for x in next(it).split()], np.uint32) for _ in range(int(rows))]
        frames.append({"rows": np.concatenate(data) if data else np.zeros(0, np.uint32), "firstHalf": float(first), "lamp": int(lamp)})
    return frames


def first_crossing(row, c, theta):
    """tools/fitest/Organ.inc's firstCrossing, in the same double arithmetic."""
    a = float(row[c])
    if a >= theta:
        return 0.0
    for j in range(16):
        b = float(row[(j + 1) * 4 + c])
        if b >= theta:
            span = b - a
            t = min(max((theta - a) / span, 0.0), 1.0) if span > 0.0 else 0.0
            return (j + t) / 16
        a = b
    return 2.0


def trace_line(n, rows, first_half, seconds, hz):
    fire, control, ended = [0.0, 0.0, 0.0], [0.0, 0.0, 0.0], 0
    end = n / FPS
    for r in range(len(rows) // 68):
        close = (first_half + r + 1) / (2.0 * hz)
        if close <= end - seconds or close > end:
            continue
        row = rows[r * 68:(r + 1) * 68]
        for c in range(3):
            fire[c] += 180.0 * min(first_crossing(row, c, 0.5), 1.0)
            control[c] += float(row[16 * 4 + c])
        ended += 1
    fire = [x / ended if ended else 180.0 for x in fire]
    control = [x / ended if ended else 0.0 for x in control]
    return f"{n},{n / FPS:.6f},{fire[0]:.2f},{fire[1]:.2f},{fire[2]:.2f},{control[0]:.5f},{control[1]:.5f},{control[2]:.5f}"


def fitest_trace(binary, sets, audio, count, into):
    path = os.path.join(into, "trace.csv")
    args = [binary, "--out", os.path.join(into, "trace.png"), "--size", "64x36", "--frames", str(count), "--fps", str(FPS),
            "--trace", path] + (["--silent"] if audio == "silence" else [])
    for name, value in sets:
        args += ["--set", f"{name}={value:.9g}" if isinstance(value, float) else f"{name}={value}"]
    subprocess.run(args, check=True, capture_output=True, env=fitest_env())
    with open(path) as handle:
        return [",".join(line.strip().split(",")[:8]) for line in handle.readlines()[1:]]


#---------------------------------------------------------------------------

def drive(page, params, audio, count, keep):
    spec = {"params": params, "audio": audio, "count": count, "keep": list(keep)}
    return page.run(DRIVE % json.dumps(spec))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fitest", required=True)
    ap.add_argument("--before", help="the git ref whose demo/ is v0.1.0's page (default: the parent of the commit that added demo/organ.js)")
    ap.add_argument("--keep", help="write each organ case's two pictures and their difference (x40) here as PNG")
    ap.add_argument("--only", help="run only the cases whose name contains this")
    args = ap.parse_args()
    fitest = os.path.abspath(args.fitest)

    work = tempfile.mkdtemp(prefix="filament-crosscheck-")
    ref = before_ref(args.before)
    old_demo = extract_demo(ref, os.path.join(work, "before"))
    new_server, new_base = serve(DEMO)
    old_server, old_base = serve(old_demo)
    url = f"?size={W}x{H}&clip=bars"
    page = Page()
    failures = 0
    try:
        # Values from the page's own laws for the cases that name a wattage or a frequency.
        page.load(new_base + url)
        laws = {
            "1000": page.run("Math.fround(Math.log10(1000 / 10) / 2)"),  # Controls.cpp's WattsParam( 1000 )
            "10": 0.0,                                                   # WattsParam( 10 )
            "150": page.run("Math.fround(Math.log10(150 / 10) / 2)"),    # WattsParam( 150 )
            "low100": page.run("window.__filamentDemo.laws.crossoverLowParam(100)"),
            "high4000": page.run("window.__filamentDemo.laws.crossoverHighParam(4000)"),
        }

        def fill(name, settings):
            s = dict(settings)
            if "wattage" in s and s["wattage"] is None:
                s["wattage"] = laws["1000"] if "1000 W" in name else laws["150"] if "150 W" in name else laws["10"]
            if s.get("crossoverLow", 0) is None:
                s["crossoverLow"] = laws["low100"]
            if s.get("crossoverHigh", 0) is None:
                s["crossoverHigh"] = laws["high4000"]
            return s

        #--- 1. Organ Off is v0.1.0's --------------------------------------
        print(f"1. Organ Off against the page before the organ (demo/ at {ref})")
        for name, settings, count in OFF_CASES:
            if args.only and args.only not in name:
                continue
            settings = fill(name, settings)
            page.load(old_base + url)
            old = drive(page, settings, "track", count, KEEP)
            page.load(new_base + url)
            new = drive(page, settings, "track", count, KEEP)
            worst = []
            for n in KEEP:
                a, b = image(old["canvases"][str(n)]), image(new["canvases"][str(n)])
                worst.append(int((a != b).sum()))
            ok = all(x == 0 for x in worst) and not new["fetched"]
            failures += not ok
            print(f"   {'ok  ' if ok else 'FAIL'}  {name:<48} bytes differing at frames {', '.join(map(str, KEEP))}: {worst};"
                  f" organ files fetched: {new['fetched'] or 'none'}")

        #--- 2 to 4. The organ ---------------------------------------------
        native = build_native(work)
        print("\n2-4. The organ: the page's WebAssembly against a native build of the same C++, fitest's trace and fitest --pipe")
        defaults_mean = None
        for name, settings, audio, count, keep in ORGAN_CASES:
            if args.only and args.only not in name:
                continue
            settings = fill(name, settings)
            page.load(new_base + url)
            got = drive(page, settings, audio, count, keep)
            host = page.run(HOST)
            # The plugin's own defaults on the defaults case: only Organ=1.
            sets = [("Organ", 1)] if name.startswith("organ defaults") else host
            first, last = image(got["inputs"]["0"]), image(got["inputs"][str(count - 1)])
            if not np.array_equal(first, last):
                raise RuntimeError("the clip moved between the first frame and the last: feed every frame")

            # 2. the rows, bit for bit
            source = {"track": 1, "silence": 2}[audio]
            theirs = replay(native, source, got["rec"])
            if len(theirs) != len(got["rec"]):
                raise RuntimeError(f"{len(got['rec'])} organ frames on the page, {len(theirs)} replayed")
            floats = same = 0
            worst_abs = 0.0
            worst_ulp = 0
            lamps_differ = halves_differ = 0
            for mine, other in zip(got["rec"], theirs):
                bits = np.frombuffer(base64.b64decode(mine["rows"]), np.uint32)
                if bits.size != other["rows"].size:
                    raise RuntimeError("row counts differ")
                floats += bits.size
                same += int((bits == other["rows"]).sum())
                a = bits.view(np.float32).astype(np.float64)
                b = other["rows"].view(np.float32).astype(np.float64)
                worst_abs = max(worst_abs, float(np.abs(a - b).max()) if bits.size else 0.0)
                worst_ulp = max(worst_ulp, int(np.abs(bits.view(np.int32).astype(np.int64) - other["rows"].view(np.int32).astype(np.int64)).max()) if bits.size else 0)
                lamps_differ += mine["lamp"] != other["lamp"]
                halves_differ += mine["gotFirstHalf"] != other["firstHalf"]
            print(f"   {name}")
            print(f"     rows   {len(got['rec'])} organ frames, {floats} floats: {same} bit-identical to the native build"
                  f" ({100.0 * same / max(floats, 1):.4f}%), worst {worst_abs:.3g} ({worst_ulp} ulp);"
                  f" lamp states differ on {lamps_differ} frames, first half-cycles on {halves_differ}")

            # 3. fitest's trace
            hz = 60.0 if settings.get("mains") == 1 else 50.0
            page_trace = []
            for n, mine in enumerate(got["rec"]):
                rows = np.frombuffer(base64.b64decode(mine["rows"]), np.float32)
                page_trace.append(trace_line(n, rows, mine["gotFirstHalf"], got["dts"][n], hz))
            fit_trace = fitest_trace(fitest, sets, audio, count, work)
            mismatch = [n for n, (x, y) in enumerate(zip(page_trace, fit_trace)) if x != y]
            ok_trace = not mismatch and len(fit_trace) == len(page_trace)
            failures += not ok_trace
            fired = sum(1 for line in fit_trace if any(float(x) < 180.0 for x in line.split(",")[2:5]))
            print(f"     trace  {'ok  ' if ok_trace else 'FAIL'}  {len(page_trace)} frames against fitest --trace, {len(mismatch)} differ as printed"
                  f" ({fired} frames with a triac fired); first: {mismatch[:3]}")
            if mismatch:
                n = mismatch[0]
                print(f"              page  : {page_trace[n]}\n              fitest: {fit_trace[n]}")

            # 4. the picture
            frames = fitest_pipe(fitest, first, count, sets, audio, set(keep))
            for n in keep:
                a, b = image(got["canvases"][str(n)]), frames[n]
                mx, off, size, mean, amax = compare(a, b)
                ok = mx <= 8 and off <= 0.01 * size and amax <= 1
                failures += not ok
                print(f"     frame {n:>3} {'ok  ' if ok else 'FAIL'}  max {mx} codes, {off} of {size} values off (mean {mean:.2e}), alpha max {amax}")
                if args.keep:
                    from PIL import Image
                    os.makedirs(args.keep, exist_ok=True)
                    stem = os.path.join(args.keep, "".join(c if c.isalnum() else "-" for c in name) + f"-{n}")
                    Image.fromarray(a[..., :3]).save(f"{stem}-page.png")
                    Image.fromarray(np.ascontiguousarray(b[..., :3])).save(f"{stem}-fitest.png")
                    Image.fromarray(np.clip(np.abs(a.astype(np.int16) - b.astype(np.int16))[..., :3] * 40, 0, 255).astype(np.uint8)).save(f"{stem}-diff-x40.png")
                if name.startswith("organ defaults") and n == keep[-1]:
                    defaults_mean = mean
            s = got["stats"]
            print(f"     page   earshot {s['state']}, the lamp {['amber', 'green', 'steady red', 'blinking red'][got['lamp']]},"
                  f" {s['fed']:.3f} s fed, FIFO {s['underruns']:.0f} short / {s['dropped']:.0f} dropped")

            if name.startswith("organ defaults"):
                slip = [(k, float(np.float32(v + 0.001)) if k == "Bass Gain" else v) for k, v in host]
                slipped = fitest_pipe(fitest, first, count, slip, audio, {keep[-1]})[keep[-1]]
                _, off, _, mean, _ = compare(image(got["canvases"][str(keep[-1])]), slipped)
                caught = mean > defaults_mean
                failures += not caught
                print(f"     {'ok  ' if caught else 'FAIL'}  negative: Bass Gain one step (0.001) off: mean {mean:.2e} against the defaults' {defaults_mean:.2e}, {off} values off")

        #--- The laws -------------------------------------------------------
        laws_out = page.run(LAWS)
        print("\nThe page's ports of Organ.cpp's laws against the compiled ones (fo_law, fo_law_inverse), 1001 host floats each")
        names = ["Crossover Low", "Crossover High", "Gain"]
        for f_, i_ in zip(laws_out["forward"], laws_out["inverse"]):
            print(f"   {names[f_['which']]:<15} readout: {f_['same']} of {f_['n']} identical, worst relative {f_['worst']:.2e};"
                  f" inverse: {i_['same']} of {i_['n']} identical")
        for d in laws_out["defaults"]:
            ok = d["page"] == d["cpp"]
            failures += not ok
            print(f"   {'ok  ' if ok else 'FAIL'}  default {d['id']:<14} page {d['page']!r} against Organ.cpp's {d['cpp']!r}")

        problems = page.problems()
        for p in problems:
            print(f"FAIL  the page logged: {p}")
        failures += len(problems)
        renderer = page.run("(() => { const gl = document.createElement('canvas').getContext('webgl2'); const x = gl.getExtension('WEBGL_debug_renderer_info'); return x ? gl.getParameter(x.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER); })()")
        print(f"\nthe page rendered on: {renderer}")
    finally:
        page.close()
        new_server.shutdown()
        old_server.shutdown()
    print(f"{'everything within the mark' if failures == 0 else f'{failures} failure(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    os.environ.setdefault("TMPDIR", tempfile.gettempdir())
    sys.exit(main())
