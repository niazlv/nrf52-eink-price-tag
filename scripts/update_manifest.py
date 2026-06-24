#!/usr/bin/env python3
"""Update web/firmware/manifest.json with one build variant's entry.

Maintains a `variants` array (keyed by integer layout id) so a single
publish location can serve several OTA images built from one source tree, and
mirrors the layout-1 (legacy) entry to the top-level fields so the existing web
DFU page keeps working unchanged. All inputs arrive via environment variables
set by the Makefile.
"""
import json
import os

mf = os.environ["MF"]
layout = int(os.environ["LAYOUT"])

entry = {
    "layout": layout,
    "name": os.environ["VNAME"],
    "file": os.environ["VFILE"],
    "version": os.environ["VER"],
    "size": int(os.environ["VSIZE"]),
    "sha256": os.environ["VSHA"],
    "date": os.environ["VDATE"],
    "key": os.environ["VKEY"],        # informational; real lock is the MCUboot key
    "ota_max": int(os.environ["VOTA"]),
    "min_version": "1.0.0",
}

try:
    with open(mf) as f:
        m = json.load(f)
except (FileNotFoundError, ValueError):
    m = {}

# Replace any existing entry for this layout, then keep the list sorted.
variants = [v for v in m.get("variants", []) if v.get("layout") != layout]
variants.append(entry)
variants.sort(key=lambda v: v["layout"])
m["variants"] = variants

# Back-compat top-level fields mirror the legacy (layout 1) image so the
# current web DFU page (which reads version/file/sha256) keeps working.
legacy = next((v for v in variants if v["layout"] == 1), entry)
m["version"] = legacy["version"]
m["file"] = legacy["file"]
m["size"] = legacy["size"]
m["sha256"] = legacy["sha256"]
m["date"] = legacy["date"]
m["notes"] = m.get("notes", "")
m["min_version"] = legacy.get("min_version", "1.0.0")

with open(mf, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")

print(">>> manifest: layout {} ({}) -> {} v{} {}B key={}".format(
    layout, entry["name"], entry["file"], entry["version"],
    entry["size"], entry["key"]))
