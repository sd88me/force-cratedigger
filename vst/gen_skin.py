#!/usr/bin/env python3
"""layout.conf + params.json -> build/skin/<vendor> - VST - <name>/ (via shadow_skin.py, from mpc-vst-plugins)."""
import json
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)
import shadow_skin  # noqa: E402

spec = json.load(open(os.path.join(ROOT, "params.json")))
art = os.environ.get("SHADOW_ART", os.path.join(ROOT, "build", "shadow_art"))
print("skin:", shadow_skin.write_skin(os.path.join(ROOT, "build", "skin"), spec["vendor"], spec["name"],
                                      os.path.join(ROOT, "layout.conf"), spec["params"], art))
