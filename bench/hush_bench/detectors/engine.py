"""The real C++ engine as a bench detector.

Runs engine/build/hush_run over every call in one batch and reports the spans
the engine ACTUALLY muted. The engine enforces its own look-ahead buffer, so
what the bench scores is exactly what a listener on the call would hear.
"""
import json
import os
import subprocess
import tempfile
from pathlib import Path

from . import Detector, Mute

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_BIN = ROOT / "engine" / "build" / ("hush_run.exe" if os.name == "nt" else "hush_run")


def binary():
    b = Path(os.environ.get("HUSH_RUN", DEFAULT_BIN))
    if not b.exists():
        raise SystemExit(f"hush_run not found at {b}. Build it: cmake -S engine -B engine/build && cmake --build engine/build")
    return b


def run_tool(mode, paths, extra=()):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(str(p) for p in paths))
        lst = f.name
    try:
        out = subprocess.run([str(binary()), mode, *extra, "--list", lst], capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(lst)
    return {Path(r["file"]).resolve(): r for r in map(json.loads, out.splitlines())}


class EngineDetector(Detector):
    name = "engine"
    batch = True  # the scorer hands over every file at once

    def __init__(self, block=160):
        self.block = int(block)

    def params(self):
        return {"block": self.block, "detector": "rules (C++)"}

    def mutes_batch(self, paths, lookahead_ms):
        res = run_tool("spans", paths, ["--lookahead-ms", str(lookahead_ms), "--block", str(self.block)])
        # The engine already applied its buffer: the span IS the decision.
        return {p: [Mute(a, b, a) for a, b in res[Path(p).resolve()]["spans"]] for p in paths}
