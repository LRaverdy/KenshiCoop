"""Runs every in-game experiment of tools/coop_test.py one after the other (each launches its own
host and client, then closes them) and prints the summary of each.

    python tools/run_all.py [experiment ...]      default: the whole list below

Logs go to test_out/all_<experiment>.log. Kenshi must not be running (every experiment kills
the game instances it finds).
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ["suite", "trade", "doors", "factions", "diplomacy", "prison", "ranged", "build", "placevalid", "stuck", "farnpc", "beds", "tpdown", "research"]

which = sys.argv[1:] or DEFAULT
os.makedirs(os.path.join(ROOT, "test_out"), exist_ok=True)
summary = []
for exp in which:
    log = os.path.join(ROOT, "test_out", f"all_{exp}.log")
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as f:
        code = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "coop_test.py"), exp], cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    text = open(log, encoding="utf-8", errors="replace").read()
    m = re.findall(r"BILAN : (\d+)/(\d+)", text)
    fails = re.findall(r"ECHEC : (.*)", text)
    crash = "died (crash?)" in text or "Traceback" in text
    line = f"{exp:10s} {m[-1][0] + '/' + m[-1][1] if m else '-':>7s}  {int(time.time() - t0):4d}s  exit={code}" + ("  CRASH/ERROR" if crash else "")
    summary.append(line)
    print(line, flush=True)
    for fl in fails:
        print("      ECHEC :", fl[:200], flush=True)
print("\n".join(["=" * 60] + summary))
