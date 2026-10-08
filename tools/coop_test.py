"""KenshiCoop in-game test harness.

Drives real Kenshi instances through the plugin's debug channel (KenshiCoop.ini: [debug] commands=1)
and compares what the host and the client see. Usage examples:

    python tools/coop_test.py run --save kctest_base          # full scenario, prints a desync report
    python tools/coop_test.py cmd <pid> status                 # one command to one instance
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import math
import os
import subprocess
import sys
import time

KENSHI = r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
EXE = os.path.join(KENSHI, "kenshi_x64.exe")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_out")

_ids = {}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def running_pids():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq kenshi_x64.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower().startswith("kenshi_x64"):
            pids.append(int(parts[1]))
    return pids


def kill_all():
    for pid in running_pids():
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 20
    while running_pids() and time.time() < deadline:
        time.sleep(0.5)


user32 = ctypes.windll.user32
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def windows_of(pid):
    res = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            res.append((hwnd, r.right, r.bottom))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return res


def dismiss_launcher(pid, timeout=25):
    """Kenshi shows its settings launcher (a small dialog) after an unclean exit: press OK."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, w, h in windows_of(pid):
            if 300 < w < 700 and 300 < h < 700:   # the launcher dialog, not the game window
                user32.PostMessageW(hwnd, 0x0111, 1, 0)   # WM_COMMAND IDOK
                log("dismissed the Kenshi launcher")
                return True
            if w >= 800:
                return False   # the game window is up: no launcher this time
        time.sleep(0.5)
    return False


def launch():
    before = set(running_pids())
    subprocess.Popen([EXE], cwd=KENSHI)
    deadline = time.time() + 30
    while time.time() < deadline:
        new = set(running_pids()) - before
        if new:
            pid = new.pop()
            dismiss_launcher(pid)
            return pid
        time.sleep(0.3)
    raise RuntimeError("Kenshi did not start")


def alive(pid):
    return pid in running_pids()


def cmd(pid, command, timeout=20.0):
    """Send one command, wait for its answer line. Returns (ok, text)."""
    n = _ids.get(pid, 0) + 1
    _ids[pid] = n
    cid = f"c{n}_{int(time.time() * 1000) % 100000}"
    path = os.path.join(KENSHI, f"kcp_cmd_{pid}.txt")
    out = os.path.join(KENSHI, f"kcp_out_{pid}.txt")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(f"{cid} {command}\n")
    deadline = time.time() + timeout
    while os.path.exists(path) and time.time() < deadline:
        time.sleep(0.05)
    os.replace(tmp, path)
    while time.time() < deadline:
        if not alive(pid):
            raise RuntimeError(f"instance {pid} died (crash?) while running '{command}'")
        if os.path.exists(out):
            with open(out, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith(cid + " "):
                        text = line[len(cid) + 1:].strip()
                        return text.startswith("ok"), text
        time.sleep(0.1)
    return False, "timeout"


def status(pid):
    ok, text = cmd(pid, "status")
    fields = {}
    if ok:
        for tok in text.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                fields[k] = v
    return fields


def wait_for(pid, pred, timeout, what):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        try:
            last = status(pid)
        except RuntimeError:
            raise
        if last and pred(last):
            return last
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {what} on {pid}: {last}")


def parse_state(path):
    st = {"squad": {}, "char": {}, "entity": {}, "time": {}, "session": ""}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            tag = parts[0]
            if tag == "session":
                st["session"] = line.strip()
            elif tag == "time":
                st["time"] = dict(p.split("=", 1) for p in parts[1:])
            elif tag in ("squad", "char"):
                key = parts[1]
                d = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
                for k in ("pos", "dest", "latest", "rendered"):
                    if k in d:
                        d[k] = tuple(float(x) for x in d[k].split(","))
                st[tag][key] = d
            elif tag == "entity":
                d = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
                d["key"] = parts[2]
                st["entity"][parts[1]] = d
    return st


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def dump(pid, name, radius=3000):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.abspath(os.path.join(OUT_DIR, f"{name}.txt"))
    ok, text = cmd(pid, f"state {path} {radius}")
    if not ok:
        raise RuntimeError(f"state dump failed on {pid}: {text}")
    return parse_state(path)


def compare(h, c, label, pos_tol=3.0):
    """Desync report between host and client dumps. Returns a dict of metrics."""
    report = {"label": label}
    # squad
    sq_diff = []
    for k, hv in h["squad"].items():
        cv = c["squad"].get(k)
        if not cv or "pos" not in cv or "pos" not in hv:
            sq_diff.append((k, "missing"))
            continue
        d = dist(hv["pos"], cv["pos"])
        vit = abs(float(hv.get("blood", 0)) - float(cv.get("blood", 0)))
        dead = hv.get("vflags") != cv.get("vflags")
        sq_diff.append((k, round(d, 2), round(vit, 2), dead))
    report["squad"] = sq_diff
    # characters near the squad: what the host has vs what the client has
    hk, ck = set(h["char"]), set(c["char"])
    missing = sorted(hk - ck)
    extra = sorted(ck - hk)
    common = hk & ck
    pos_err = sorted((dist(h["char"][k]["pos"], c["char"][k]["pos"]), k) for k in common if "pos" in h["char"][k] and "pos" in c["char"][k])
    report["host_chars"] = len(hk)
    report["client_chars"] = len(ck)
    report["missing_on_client"] = len(missing)
    report["extra_on_client"] = len(extra)
    report["pos_err_max"] = round(pos_err[-1][0], 2) if pos_err else 0
    report["pos_err_over_tol"] = sum(1 for e, _ in pos_err if e > pos_tol)
    dead_mismatch = [k for k in common if h["char"][k].get("vflags") != c["char"][k].get("vflags")]
    report["vital_flag_mismatch"] = len(dead_mismatch)
    ht, ct = h["time"], c["time"]
    try:
        report["hours_diff"] = round(abs(float(ht["hours"]) - float(ct["hours"])), 4)
        report["paused"] = (ht.get("paused"), ct.get("paused"))
        report["speed"] = (ht.get("speed"), ct.get("speed"))
    except (KeyError, ValueError):
        pass
    report["missing_sample"] = missing[:5]
    report["extra_sample"] = extra[:5]
    report["worst"] = pos_err[-3:]
    # split the worst errors: network staleness (host now vs newest packet) and tracking (rendered vs local)
    detail = []
    for e, k in pos_err[-3:]:
        hc, cc = h["char"][k], c["char"][k]
        stale = round(dist(hc["pos"], cc["latest"]), 2) if "latest" in cc else None
        track = round(dist(cc["rendered"], cc["pos"]), 2) if "rendered" in cc else None
        detail.append((k, "flags h/c", hc.get("flags"), cc.get("flags"), "stale", stale, "tracking", track))
    report["worst_detail"] = detail
    return report


def print_report(r):
    log(f"=== {r['label']}")
    for k, v in r.items():
        if k != "label":
            log(f"    {k}: {v}")


def setup(save, name="Tester"):
    kill_all()
    host = launch()
    log("host pid", host)
    wait_for(host, lambda s: s.get("state") == "idle", 120, "host menu")
    time.sleep(3)
    ok, t = cmd(host, f"load {save}")
    log("load", ok, t)
    wait_for(host, lambda s: s.get("ready") == "1", 180, "host world")
    ok, t = cmd(host, "host")
    log("host", ok, t)
    cli = launch()
    log("client pid", cli)
    wait_for(cli, lambda s: s.get("state") == "idle", 120, "client menu")
    time.sleep(3)
    ok, t = cmd(cli, "join")
    log("join", ok, t)
    wait_for(cli, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 240, "client in host world")
    log("client connected")
    return host, cli


def frozen_check(host, cli, label):
    """Pause the host (real pause: game speed 0), let the client settle, compare: must be exact."""
    cmd(host, "pause 1")
    time.sleep(2.5)
    r = compare(dump(host, "h_" + label.replace(" ", "_")), dump(cli, "c_" + label.replace(" ", "_")), "FROZEN " + label, pos_tol=0.1)
    cmd(host, "pause 0")
    return r


def squad_pos(state, idx):
    keys = sorted(state["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    return state["squad"][keys[idx]]["pos"]


def scenario(host, cli, quick=False):
    reports = []
    time.sleep(8)
    reports.append(frozen_check(host, cli, "after join"))
    log("give", cmd(host, "give 2 0"))
    time.sleep(2)
    log("client move", cmd(cli, "moverel 0 40 25"))
    time.sleep(15)
    reports.append(frozen_check(host, cli, "client walked squad0"))
    log("host move", cmd(host, "moverel 1 300 -200"))
    time.sleep(25)
    reports.append(frozen_check(host, cli, "host walked squad1"))
    # an NPC that only the host has: the client must recreate it, then see it fall and die
    ok, text = cmd(host, "spawnnpc 12 8")
    log("host spawns an NPC", ok, text)
    spawned_key = text.split()[1] if ok else None
    time.sleep(6)
    r = frozen_check(host, cli, "host-only NPC appears")
    r["spawned_key_on_client"] = bool(spawned_key) and spawned_key in dump(cli, "c_spawn_lookup")["char"] or "check by netId"
    reports.append(r)
    log("ko", cmd(host, "ko"))
    time.sleep(5)
    reports.append(frozen_check(host, cli, "NPC knocked out"))
    log("kill", cmd(host, "kill"))
    time.sleep(5)
    reports.append(frozen_check(host, cli, "NPC killed"))
    if not quick:
        h = dump(host, "h_pre_tp")
        x, y, z = squad_pos(h, 2)
        log("teleport far", cmd(host, f"teleport 2 {x + 6000} {y + 50} {z + 4000}"))
        time.sleep(30)
        reports.append(frozen_check(host, cli, "squad2 far +30s"))
        log("speed", cmd(host, "speed 3"))
        time.sleep(40)
        log("speed", cmd(host, "speed 1"))
        time.sleep(3)
        reports.append(frozen_check(host, cli, "after x3 run"))
    for r in reports:
        print_report(r)
    return reports


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="what", required=True)
    r = sub.add_parser("run")
    r.add_argument("--save", default="kctest_base")
    r.add_argument("--keep", action="store_true", help="leave the instances running")
    r.add_argument("--quick", action="store_true")
    c = sub.add_parser("cmd")
    c.add_argument("pid", type=int)
    c.add_argument("command", nargs="+")
    a = ap.parse_args()
    if a.what == "cmd":
        print(cmd(a.pid, " ".join(a.command)))
        return
    host, cli = setup(a.save)
    try:
        scenario(host, cli, a.quick)
    finally:
        if not a.keep:
            kill_all()


if __name__ == "__main__":
    main()
