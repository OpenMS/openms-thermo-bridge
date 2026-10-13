#!/usr/bin/env python3
"""Compare a thermo_bridge_bench --dump against a v0.3.1 baseline dump (local parity check).

usage: compare_parity.py baseline.jsonl new.jsonl

Baseline lines come from the v0.3.1 API: file_metadata_json(true, true),
scan_metadata_json, spectrum_data(centroid / profile) digests and
spectrum_auxiliary_array(0..3) digests. Peaks must match bit-exactly
(new output is m/z-sorted, so it is compared with the baseline's sorted digest).
Typed trailers are compared with the vendor-formatted baseline strings within
the formatting precision. Since 1.0 a scan without noise data (no centroid stream)
reports an empty noise triplet, where v0.3.1 returned the segmented masses as aux1.
"""
import json
import math
import sys
from collections import Counter
from itertools import zip_longest

HEADER_KEYS = ["scan_number", "controller_type", "controller_number", "native_id", "retention_time", "ms_level",
               "ms_order_name", "centroid", "polarity", "filter", "analyzer", "ionization", "low_mass", "high_mass",
               "low_wavelength", "high_wavelength", "tic", "base_peak_mass", "base_peak_intensity"]
REACTION_KEYS = ["index", "precursor_mass", "isolation_width", "isolation_offset", "activation", "collision_energy",
                 "collision_energy_valid"]
TRUE_WORDS = {"yes", "on", "true", "1"}
FALSE_WORDS = {"no", "off", "false", "0", ""}


def same_number(a, b):
    if a is None or b is None:
        return a is None and b is None or (a is None and isinstance(b, float) and math.isnan(b))
    return float(a) == float(b)


def typed_matches(kind, value, text):
    t = text.strip()
    if kind == 3:
        return value == text
    if kind == 2:
        return (t.lower() in TRUE_WORDS) == value if t.lower() in TRUE_WORDS | FALSE_WORDS else False
    if kind == 1:
        try:
            return int(float(t)) == value
        except ValueError:
            return False
    try:
        expected = float(t)
    except ValueError:
        return value is None
    if value is None:
        return math.isnan(expected)
    decimals = len(t.split(".")[1]) if "." in t and "e" not in t.lower() else 0
    tolerance = 0.5 * 10 ** -decimals + 1e-6 * abs(expected) + 1e-12
    return abs(value - expected) <= tolerance


def main(baseline_path, new_path):
    problems = Counter()
    examples = {}

    def fail(kind, detail):
        problems[kind] += 1
        examples.setdefault(kind, detail)

    with open(baseline_path) as bf, open(new_path) as nf:
        base_file = json.loads(bf.readline())
        new_file = json.loads(nf.readline())
        layout = new_file["layout"]

        base_meta = dict(base_file["meta"])
        new_meta = dict(new_file["meta"])
        if new_file["sha1"] != base_meta["file"]["sha1"]:
            fail("file_sha1", (new_file["sha1"], base_meta["file"]["sha1"]))
        base_meta["file"] = dict(base_meta["file"], sha1=None, path=None)
        new_meta["file"] = dict(new_meta["file"], path=None)
        if new_meta != base_meta:
            fail("file_metadata_json", [k for k in base_meta if base_meta.get(k) != new_meta.get(k)])
        if new_file["tic"] != base_file["tic"]:
            fail("tic", (new_file["tic"], base_file["tic"]))

        scans = 0
        for base_line, new_line in zip_longest(bf, nf):
            if base_line is None or new_line is None:
                fail("scan_count", "new dump has extra scans" if base_line is None else "new dump is missing scans")
                break
            b = json.loads(base_line)
            n = json.loads(new_line)
            s = b["scan"]
            scans += 1
            if n["scan"] != s:
                fail("scan_order", (s, n["scan"]))
                continue
            meta = b["meta"]
            if n["legacy_meta"] != meta:
                fail("scan_metadata_json", s)
            for key in ("c", "p"):
                if not n[key]["sorted"]:
                    fail(key + "_unsorted", s)
                if (n[key]["n"], n[key]["h"]) != (b[key]["n"], b[key]["hs"]):
                    fail(key + "_peaks", s)
            if (n["aux_c"]["n"], n["aux_c"]["h"]) != (b["c"]["n"], b["c"]["hs"]):
                fail("noise_path_peaks", s)
            expected_aux = dict(b)
            if b["aux2"]["n"] == 0 and b["aux3"]["n"] == 0:
                expected_aux["aux1"] = b["aux2"]
            for key in ("aux0", "aux1", "aux2", "aux3"):
                if n[key] != expected_aux[key]:
                    fail(key, (s, n[key], b[key]))

            h = n["header"]
            for key in HEADER_KEYS:
                bv, nv = meta.get(key), h.get(key)
                ok = same_number(nv, bv) if isinstance(bv, (int, float)) and not isinstance(bv, bool) else nv == bv
                if not ok:
                    fail("header." + key, (s, nv, bv))
            if len(h["reactions"]) != len(meta["reactions"]):
                fail("header.reactions.count", (s, len(h["reactions"]), len(meta["reactions"])))
            else:
                for nr, br in zip(h["reactions"], meta["reactions"]):
                    for key in REACTION_KEYS:
                        bv, nv = br.get(key), nr.get(key)
                        ok = same_number(nv, bv) if isinstance(bv, (int, float)) and not isinstance(bv, bool) else nv == bv
                        if not ok:
                            fail("reaction." + key, (s, nv, bv))

            trailer = meta.get("trailer", [])
            if [t["label"] for t in trailer] != [l["label"] for l in layout][: len(trailer)]:
                fail("trailer_labels", s)
            if n["trailer_text"] != [t["value"] for t in trailer]:
                fail("trailer_text", s)
            for i, t in enumerate(trailer):
                if i >= len(n["trailers"]):
                    fail("trailer_missing", s)
                    break
                if not typed_matches(layout[i]["kind"], n["trailers"][i], t["value"]):
                    fail("trailer." + t["label"], (s, n["trailers"][i], t["value"]))

        run = base_meta["run"]
        declared = run["last_scan"] - run["first_scan"] + 1 if run.get("scan_count") else 0
        if scans != declared:
            fail("scan_count", f"{scans} scans compared, {declared} declared")

    print(f"{new_path}: {scans} scans compared")
    if not problems:
        print("  PARITY OK: peaks, aux arrays, headers, reactions, trailers, legacy JSON, sha1, TIC")
        return 0
    for kind, count in sorted(problems.items()):
        print(f"  MISMATCH {kind}: {count} (e.g. {examples[kind]})")
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
