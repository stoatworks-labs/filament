#!/usr/bin/env python3
"""The light of a tungsten filament through a theatre gel, at every temperature.

Writes source/GelTable.h for the colour organ: for each gel (and Clear), for T
from 250 K to 3700 K in 10 K steps -- the Planck table's own grid -- the light
of a black body at T after the gel, as linear sRGB, divided by the luminance
of the UNFILTERED black body at T:

    row = ( r_gel / Y, g_gel / Y, b_gel / Y, Y_gel / Y )

so that the shader's light through a gel is  row.rgb * 2^( log2 Y )  with
log2 Y read from PlanckTable.h, exactly as the bare filament is chroma * 2^logY.
The fourth column is the gel's luminous transmittance for that filament.

    python3 tools/gel_table.py            # rewrite source/GelTable.h
    python3 tools/gel_table.py --check    # exit 1 if the header differs
    python3 tools/gel_table.py --print    # each gel at 3200 K against LEE's figures

--------------------------------------------------------------------- sources

**The gels.** LEE Filters' own spectral transmission, percent, at 5 nm from
400 to 700 nm, read on 2026-11-16 from the graph data on each filter's page
(https://leefilters.com/colour/106-primary-red/ and the same pattern for the
others) and committed unedited as tools/data/lee_gels.csv; its SHA-256 is
checked below. LEE's graphs stop at 400 and 700 nm. Outside that the
transmittance is HELD at the end value: an assumption, and the --print
comparison below says what it costs.

**The colour-matching functions, Planck's law and the sRGB matrix** are
tools/planck_table.py's, imported from it, so a gel row and a Planck row are
the same integral with and without the gel.

--------------------------------------------------------------------- method

The gel's 5 nm samples are interpolated linearly onto the CMF table's 1 nm
grid, then

    X_gel = sum B(l, T) tau(l) x-bar(l)   over 360..830 nm at 1 nm

and likewise Y and Z, through the IEC 61966-2-1 matrix. Clear is tau = 1, so
its rows are the Planck table's chromaticity rows and a luminous transmittance
of exactly 1.

--------------------------------------------------------------------- the check

LEE prints each filter's tungsten colorimetry (3200 K: Y %, x, y) beside the
graph. --print integrates the same spectrum against a 3200 K black body and
the 1931 observer and prints both. LEE does not say which observer, step or
range it used, so this is a sanity line, not a pass/fail check: the
chromaticities agree to about 0.01 and the Y to about 10 %.
"""
import argparse
import hashlib
import math
import pathlib
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import planck_table  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "source" / "GelTable.h"
GELS = ROOT / "tools" / "data" / "lee_gels.csv"
GELS_SHA256 = "da460f4466fe9094b929b9b0dba50624f18c4e87a06e5722be6961762f6095af"

# Menu order (append-only: an option stores its element's index). The number
# is LEE's, the column header in the CSV.
ORDER = [
    ("106", "106 Primary Red"),
    ("139", "139 Primary Green"),
    ("119", "119 Dark Blue"),
    ("132", "132 Medium Blue"),
    ("120", "120 Deep Blue"),
    ("101", "101 Yellow"),
    ("105", "105 Orange"),
    ("020", "020 Medium Amber"),
    (None, "Clear"),
]

# LEE's printed tungsten (3200 K) figures, Y %, x, y, from the same pages.
LEE_TUNGSTEN = {
    "106": (12.8, 0.693, 0.305),
    "139": (10.1, 0.212, 0.717),
    "119": (2.3, 0.137, 0.109),
    "132": (7.0, 0.148, 0.195),
    "120": (1.1, 0.147, 0.068),
    "101": (80.7, 0.508, 0.473),
    "105": (50.0, 0.590, 0.406),
    "020": (56.8, 0.558, 0.424),
}


def load_gels():
    data = GELS.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != GELS_SHA256:
        raise SystemExit("%s is not the committed LEE file (sha256 %s)" % (GELS, digest))
    lines = [l for l in data.decode().splitlines() if l.strip() and not l.startswith("#")]
    head = lines[0].split(",")
    if head[0] != "nm":
        raise SystemExit("unexpected header in %s" % GELS)
    nm = []
    cols = {h: [] for h in head[1:]}
    for line in lines[1:]:
        parts = line.split(",")
        nm.append(int(parts[0]))
        for h, v in zip(head[1:], parts[1:]):
            cols[h].append(float(v) / 100.0)
    if nm[0] != 400 or nm[-1] != 700 or len(nm) != 61:
        raise SystemExit("unexpected gel table shape")
    return nm, cols


def tau_at(nm_grid, values, nm):
    """Linear between LEE's 5 nm points; held at the ends (the assumption)."""
    if nm <= nm_grid[0]:
        return values[0]
    if nm >= nm_grid[-1]:
        return values[-1]
    i = (nm - nm_grid[0]) // 5
    t = (nm - nm_grid[i]) / 5.0
    return values[i] + (values[i + 1] - values[i]) * t


def filtered_xyz(kelvin, cmf, tau):
    X = Y = Z = 0.0
    for nm, xb, yb, zb in cmf:
        b = planck_table.planck(nm, kelvin) * tau(nm)
        X += b * xb
        Y += b * yb
        Z += b * zb
    return X, Y, Z


def gel_rows(cmf, nm_grid, cols):
    tables = []
    for key, _ in ORDER:
        if key is None:
            tau = lambda nm: 1.0  # noqa: E731
        else:
            values = cols[key]
            tau = lambda nm, v=values: tau_at(nm_grid, v, nm)  # noqa: E731
        rows = []
        for T in range(planck_table.T_FIRST, planck_table.T_LAST + 1, planck_table.T_STEP):
            Y0 = planck_table.xyz(float(T), cmf)[1]
            X, Y, Z = filtered_xyz(float(T), cmf, tau)
            r, g, b = planck_table.rgb(X, Y, Z)
            rows.append((T, r / Y0, g / Y0, b / Y0, Y / Y0))
        tables.append(rows)
    return tables


def render(tables):
    n = len(tables[0])
    lines = [
        "#pragma once",
        "",
        "// GENERATED by tools/gel_table.py -- do not edit. Run the script.",
        "//",
        "// A black body at T through a LEE Filters gel (LEE's measured spectral",
        "// transmission, tools/data/lee_gels.csv), as linear sRGB over the UNFILTERED",
        "// luminance at T: rows of ( r / Y, g / Y, b / Y, Y_gel / Y ) on the Planck",
        "// table's grid, so light through a gel is row.rgb * 2^log2Y with log2Y from",
        "// PlanckTable.h. Sources and method are in the script.",
        "",
        "namespace filament::gels",
        "{",
        "",
        "constexpr int kGelCount   = %d;" % len(tables),
        "constexpr int kRowCount   = %d;" % n,
        "constexpr int kFirstKelvin = %d;" % planck_table.T_FIRST,
        "constexpr int kStepKelvin  = %d;" % planck_table.T_STEP,
        "",
        "/// The menu, in element order (append-only).",
        "constexpr const char* kNames[ kGelCount ] = {",
    ]
    for _, name in ORDER:
        lines.append('\t"%s",' % name)
    lines += ["};", "", "constexpr float kTable[ kGelCount ][ kRowCount ][ 4 ] = {"]
    for (key, name), rows in zip(ORDER, tables):
        lines.append("\t{ // %s" % name)
        for T, cr, cg, cb, ty in rows:
            lines.append("\t\t{ %s, %s, %s, %s }, // %d K" % (planck_table.float_literal(cr), planck_table.float_literal(cg),
                                                          planck_table.float_literal(cb), planck_table.float_literal(ty), T))
        lines.append("\t},")
    lines += ["};", "", "} // namespace filament::gels", ""]
    return "\n".join(lines)


def compare(cmf, nm_grid, cols):
    print("each gel against a 3200 K black body, CIE 1931 2-degree, 360-830 nm (LEE's 400-700 nm held at the ends)")
    print("            computed: Y %     x      y      LEE prints: Y %     x      y     share of Y outside 400-700 nm")
    for key, name in ORDER:
        if key is None:
            continue
        values = cols[key]
        tau = lambda nm, v=values: tau_at(nm_grid, v, nm)  # noqa: E731
        X0, Y0, Z0 = planck_table.xyz(3200.0, cmf)
        X, Y, Z = filtered_xyz(3200.0, cmf, tau)
        inside = filtered_xyz(3200.0, [r for r in cmf if 400 <= r[0] <= 700], tau)[1]
        Yp, xp, yp = LEE_TUNGSTEN[key]
        print("%-18s  %7.2f  %.4f  %.4f        %7.1f  %.3f  %.3f     %.2f %%" % (
            name, 100.0 * Y / Y0, X / (X + Y + Z), Y / (X + Y + Z), Yp, xp, yp, 100.0 * (Y - inside) / Y))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="compare with the committed header, do not write")
    ap.add_argument("--print", action="store_true", help="each gel at 3200 K against LEE's printed figures")
    args = ap.parse_args()

    cmf = planck_table.load_cmf()
    nm_grid, cols = load_gels()
    if args.print:
        compare(cmf, nm_grid, cols)
        return 0
    text = render(gel_rows(cmf, nm_grid, cols))
    if args.check:
        if not HEADER.exists() or HEADER.read_text() != text:
            print("%s does not match what the script computes -- run tools/gel_table.py" % HEADER.relative_to(ROOT))
            return 1
        print("%s recomputes from LEE's spectra and the CIE 1931 CMFs (both sha256 checked), %d gels" % (HEADER.relative_to(ROOT), len(ORDER)))
        return 0
    HEADER.write_text(text)
    print("wrote %s" % HEADER.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
