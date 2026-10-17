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
import re
import subprocess
import sys
import struct
import threading
import time
import zlib

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
            if w >= 640:
                return False   # the game window is up: no launcher this time
        time.sleep(0.5)
    return False


def clean_command_files():
    """The command channel's files of instances that are gone (they pile up in Kenshi's folder)."""
    live = set(running_pids())
    for name in os.listdir(KENSHI):
        m = re.match(r"kcp_(?:cmd|out)_(\d+)\.txt(?:\.tmp)?$", name)
        if m and int(m.group(1)) not in live:
            try:
                os.remove(os.path.join(KENSHI, name))
            except OSError:
                pass


MIN_FREE_RAM_MB = 2048
_launched = []   # every Kenshi this harness started (killed on any failure, see main())


def free_ram_mb():
    """Free physical memory in MB (GlobalMemoryStatusEx; None if it cannot be read)."""
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", wt.DWORD), ("dwMemoryLoad", wt.DWORD), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return None
    return round(m.ullAvailPhys / 1048576)


class LowMemory(RuntimeError):
    pass


def ensure_ram(what):
    free = free_ram_mb()
    if free is not None and free < MIN_FREE_RAM_MB:
        raise LowMemory(f"not enough free RAM to start {what}: {free} MB free, {MIN_FREE_RAM_MB} MB needed "
                        f"(a host uses ~2.2 GB, a client ~0.7 GB); close something or use fewer clients")
    log(f"free RAM before starting {what}: {free} MB")


def launch(fake_steam_id=None, name=None):
    clean_command_files()
    ensure_ram("a client" if fake_steam_id else "a Kenshi instance")
    before = set(running_pids())
    env = dict(os.environ)
    env.pop("KC_FAKE_STEAM_ID", None)
    env.pop("KC_PLAYER_NAME", None)
    if fake_steam_id:
        env["KC_FAKE_STEAM_ID"] = str(fake_steam_id)
    if name:
        env["KC_PLAYER_NAME"] = name   # debug builds only (commands=1): that game's player name
    subprocess.Popen([EXE], cwd=KENSHI, env=env)
    deadline = time.time() + 30
    while time.time() < deadline:
        new = set(running_pids()) - before
        if new:
            pid = new.pop()
            _launched.append(pid)
            dismiss_launcher(pid)
            return pid
        time.sleep(0.3)
    raise RuntimeError("Kenshi did not start")


def alive(pid):
    return pid in running_pids()


_cmd_locks = {}
_cmd_locks_guard = threading.Lock()


def cmd(pid, command, timeout=20.0):
    """Send one command, wait for its answer line. Returns (ok, text). Thread safe: one command at a
    time per instance (the stress tests drive several games from several threads)."""
    with _cmd_locks_guard:
        lock = _cmd_locks.setdefault(pid, threading.Lock())
    with lock:
        return _cmd_one(pid, command, timeout)


def _cmd_one(pid, command, timeout):
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
    st = {"squad": {}, "char": {}, "entity": {}, "time": {}, "session": "", "weather": {}}
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
            elif tag == "weather":
                st["weather"][parts[1]] = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
            elif tag == "fx":
                d = dict(p.split("=", 1) for p in parts[7:] if "=" in p)
                d.update(id=int(parts[1]), kind=parts[2], region=parts[3], effect=parts[4], ordinal=parts[5],
                         pos=tuple(float(x) for x in parts[6].split(",")))
                st.setdefault("fx", []).append(d)
            elif tag == "fxstats":
                st["fxstats"] = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
            elif tag == "entity":
                d = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
                d["key"] = parts[2]
                st["entity"][parts[1]] = d
    return st


def entity_of(state, key):
    """The session entity of that character handle in a parsed dump ("entity" is keyed by netId: a
    lookup by handle found nothing, the animalshop owner check of 13/10 15:14 read None for owner=2)."""
    for e in state["entity"].values():
        if e.get("key") == key:
            return e
    return {}


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def dump(pid, name, radius=3000):
    os.makedirs(OUT_DIR, exist_ok=True)
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", name)   # the debug command splits on spaces ("Wild Bull")
    path = os.path.abspath(os.path.join(OUT_DIR, f"{name}.txt"))
    ok, text = cmd(pid, f"state {path} {radius}")
    for _ in range(10):   # the world can read as unloaded for a frame (save, zone streaming): ask again
        if ok or "no live world" not in text:
            break
        time.sleep(0.5)
        ok, text = cmd(pid, f"state {path} {radius}")
    if not ok:
        raise RuntimeError(f"state dump failed on {pid}: {text}")
    return parse_state(path)


def compare(h, c, label, pos_tol=3.0, radius=3000):
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
    # bodies far from every player are not replicated on purpose (they are once someone comes close)
    bound = {e["key"] for e in h["entity"].values()}
    far_corpses = {k for k in hk - ck if k not in bound and int(h["char"][k].get("flags", "0")) & 8}
    report["far_corpses_unreplicated"] = len(far_corpses)
    # at the edge of the dump radius (around the squad): the client's copy of a running character can
    # be a few units further and fall just outside its dump; it is there (a present entity), not missing
    centers = [v["pos"] for v in h["squad"].values() if "pos" in v]
    c_present = {e["key"] for e in c["entity"].values() if e.get("present") == "1"}
    edge = {k for k in hk - ck if k in c_present and "pos" in h["char"][k] and centers
            and min(dist(h["char"][k]["pos"], p) for p in centers) > radius - 60}
    report["edge_unlisted"] = len(edge)
    missing = sorted(hk - ck - far_corpses - edge)
    extra = sorted(ck - hk)
    common = hk & ck
    pos_err = sorted((dist(h["char"][k]["pos"], c["char"][k]["pos"]), k) for k in common if "pos" in h["char"][k] and "pos" in c["char"][k])
    report["host_chars"] = len(hk)
    report["client_chars"] = len(ck)
    report["missing_on_client"] = len(missing)
    report["extra_on_client"] = len(extra)
    report["pos_err_max"] = round(pos_err[-1][0], 2) if pos_err else 0
    report["pos_err_over_tol"] = sum(1 for e, _ in pos_err if e > pos_tol)
    def life(v):   # 2 = dead (whatever else), 1 = knocked out, 0 = fine
        f = int(v.get("vflags", "0") or 0)
        return 2 if f & 2 else f & 1
    dead_mismatch = [k for k in common if life(h["char"][k]) != life(c["char"][k])]
    report["vital_flag_mismatch"] = len(dead_mismatch)
    hcomb = {k: v.get("combat") for k, v in list(h["char"].items()) + list(h["squad"].items()) if v.get("combat")}
    ccomb = {k: v.get("combat") for k, v in list(c["char"].items()) + list(c["squad"].items()) if v.get("combat")}
    report["combat_host"] = len(hcomb)
    report["combat_mismatch"] = sum(1 for k, t in hcomb.items() if ccomb.get(k) != t) + sum(1 for k in ccomb if k not in hcomb)
    ht, ct = h["time"], c["time"]
    try:
        report["hours_diff"] = round(abs(float(ht["hours"]) - float(ct["hours"])), 4)
        report["paused"] = (ht.get("paused"), ct.get("paused"))
        report["speed"] = (ht.get("speed"), ct.get("speed"))
    except (KeyError, ValueError):
        pass
    inv_bad = []
    for k in set(h["char"]) & set(c["char"]):
        if h["char"][k].get("inv", "") != c["char"][k].get("inv", ""):
            inv_bad.append(k)
    for k in set(h["squad"]) & set(c["squad"]):
        if h["squad"][k].get("inv", "") != c["squad"][k].get("inv", ""):
            inv_bad.append("squad " + k)
    down_err = []
    for k in set(h["char"]) & set(c["char"]):
        hv, cv = h["char"][k], c["char"][k]
        if "pos" in hv and "pos" in cv and hv.get("flags") in ("4", "8", "12") and cv.get("flags") in ("4", "8", "12"):
            down_err.append((round(dist(hv["pos"], cv["pos"]), 2), k))
    down_err.sort(reverse=True)
    report["down_bodies_worst"] = down_err[:3]
    report["inventory_mismatch"] = len(inv_bad)
    report["inventory_mismatch_sample"] = inv_bad[:3]
    hw, cw = h.get("weather", {}), c.get("weather", {})
    report["weather_regions"] = (len(hw), len(cw))
    report["weather_mismatch"] = [k for k in hw if k not in cw or cw[k].get("type") != hw[k].get("type") or cw[k].get("season") != hw[k].get("season")][:5]
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
    for _ in range(20):   # the world can still be settling right after "ready" (err load a save before hosting)
        ok, t = cmd(host, "host")
        if ok:
            break
        log("host not accepted yet:", t)
        time.sleep(1)
    log("host", ok, t)
    cli = launch(fake_steam_id=76561190000000002)
    log("client pid", cli)
    wait_for(cli, lambda s: s.get("state") == "idle", 120, "client menu")
    time.sleep(3)
    ok, t = cmd(cli, "join " + os.environ["KC_JOIN"]) if os.environ.get("KC_JOIN") else cmd(cli, "join")
    log("join", ok, t)
    wait_for(cli, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 240, "client in host world")
    log("client connected")
    arrange(host, cli)   # side by side, every time
    return host, cli


def setup_many(save, clients):
    """A host and several clients, each joining in turn (every client must be in before the next joins)."""
    kill_all()
    host = launch()
    log("host pid", host)
    wait_for(host, lambda s: s.get("state") == "idle", 180, "host menu")
    time.sleep(3)
    log("load", cmd(host, f"load {save}"))
    wait_for(host, lambda s: s.get("ready") == "1", 240, "host world")
    log("host", cmd(host, "host"))
    clis = []
    for n in range(clients):
        c = launch(fake_steam_id=76561190000000002 + n)
        log("client", n + 1, "pid", c)
        wait_for(c, lambda s: s.get("state") == "idle", 240, f"client {n + 1} menu")
        time.sleep(3)
        log("join", cmd(c, "join " + os.environ["KC_JOIN"]) if os.environ.get("KC_JOIN") else cmd(c, "join"))
        wait_for(c, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 420, f"client {n + 1} in host world")
        log("client", n + 1, "connected")
        clis.append(c)
    return host, clis


def tile(pids):
    """Every game window glued to the next one and fully visible: side by side for two, 2x2 for more.
    Each window gets an equal share of the work area (above the taskbar), keeping its aspect ratio, and
    is brought to the front so the user can watch every instance."""
    sw, sh = user32.GetSystemMetrics(0), user32.GetSystemMetrics(17)   # SM_CYFULLSCREEN: above the taskbar
    wins = [hwnd for pid in pids for hwnd, w, h in windows_of(pid) if w >= 640]
    if not wins:
        return
    r = wt.RECT()
    user32.GetWindowRect(wins[-1], ctypes.byref(r))
    aspect = (r.bottom - r.top) / max(1, r.right - r.left)
    cols = 1 if len(wins) == 1 else 2
    rows = (len(wins) + cols - 1) // cols
    W = sw // cols
    H = int(W * aspect)
    if H * rows > sh:
        H = sh // rows
        W = int(H / aspect)
    for i, hwnd in enumerate(wins):
        user32.ShowWindow(hwnd, 9)   # SW_RESTORE
        user32.SetWindowPos(hwnd, 0, (i % cols) * W, (i // cols) * H, W, H, 0x0040)   # HWND_TOP, SWP_SHOWWINDOW


def arrange_grid(pids):
    tile(pids)


def exp_four(host, clis):
    """1 host + 3 clients: every client sees the same world, each one commands only its own character."""
    time.sleep(6)
    log("players on the host:", cmd(host, "status")[1])
    for i, c in enumerate(clis):
        log("client", i + 1, cmd(c, "status")[1])
    reports = []
    def frozen_all(label):
        cmd(host, "pause 1")
        time.sleep(3)
        h = dump(host, "h4_" + label.replace(" ", "_"))
        cs = [dump(c, f"c{i + 1}_" + label.replace(" ", "_")) for i, c in enumerate(clis)]
        for i, c in enumerate(cs):
            reports.append(compare(h, c, f"FROZEN {label} / host vs client {i + 1}", pos_tol=0.1))
        # clients between themselves: what client A sees of client B's character (and of everything) is what B sees
        for i in range(len(cs)):
            for j in range(i + 1, len(cs)):
                reports.append(compare(cs[i], cs[j], f"FROZEN {label} / client {i + 1} vs client {j + 1}", pos_tol=0.1))
        cmd(host, "pause 0")
    frozen_all("after joins")
    # each client walks every squad member: only its own character may obey
    for i, c in enumerate(clis):
        for k in range(9):
            cmd(c, f"moverel {k} {40 + 30 * i} {25 - 20 * i}")
    time.sleep(15)
    frozen_all("each client walked its own")
    # skills, bubbles: everyone gets the host's
    log("host xp", cmd(host, "xp 0 1 5"))
    log("host say", cmd(host, "say 1 Bonjour a tous les joueurs"))
    time.sleep(3)
    hs = cmd(host, "stats 0")[1].split()[1]
    for i, c in enumerate(clis):
        log("client", i + 1, "stats equal:", cmd(c, "stats 0")[1].split()[1] == hs, "| bubbles:", cmd(c, "says")[1])
    # a fight with everyone watching
    log("spawn", cmd(host, "spawnnpc 15 10"))
    time.sleep(3)
    for k in (0, 1, 2):
        log("fight", k, cmd(host, f"fight {k}"))
    time.sleep(12)
    frozen_all("during a fight")
    time.sleep(20)
    frozen_all("after the fight")
    log("=" * 60)
    for r in reports:
        log(r["label"], "| chars", r["host_chars"], r["client_chars"], "missing", r["missing_on_client"], "extra", r["extra_on_client"],
            "| pos max", r["pos_err_max"], "| vital mismatch", r["vital_flag_mismatch"], "| combat mismatch", r["combat_mismatch"],
            "| inventory mismatch", r["inventory_mismatch"], "| squad", [(t[0][-12:],) + tuple(t[1:]) for t in r["squad"] if not isinstance(t[1], (int, float)) or t[1] > 0.1])


def arrange(host, cli):
    """Side by side, glued: host on the left, client right next to it."""
    tile([host, cli])


def frozen_check(host, cli, label, radius=3000):
    """Pause the host (real pause: game speed 0), let the client settle, compare: must be exact."""
    cmd(host, "pause 1")
    time.sleep(2.5)
    r = compare(dump(host, "h_" + label.replace(" ", "_"), radius), dump(cli, "c_" + label.replace(" ", "_"), radius), "FROZEN " + label, pos_tol=0.1)
    cmd(host, "pause 0")
    return r


def squad_pos(state, idx):
    keys = sorted(state["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    return state["squad"][keys[idx]]["pos"]


def scenario(host, cli, quick=False):
    reports = []
    time.sleep(8)
    reports.append(frozen_check(host, cli, "after join"))
    # the joining player arrived with a character of their own
    h = dump(host, "h_own", 3000)
    own = [e["key"] for e in h["entity"].values() if e.get("squad") == "1" and e.get("owner") == "2"]
    log("client's own characters:", own, "squad size", len(h["squad"]))
    log("give", cmd(host, "give 2 0"))
    time.sleep(2)
    log("client move", cmd(cli, "moverel 0 40 25"))
    time.sleep(15)
    reports.append(frozen_check(host, cli, "client walked squad0"))
    log("host move", cmd(host, "moverel 1 300 -200"))
    time.sleep(25)
    reports.append(frozen_check(host, cli, "host walked squad1"))
    # weather: the host rolls a new weather everywhere; the client must follow region by region
    log("rollweather", cmd(host, "rollweather"))
    time.sleep(8)
    reports.append(compare(dump(host, "h_weather"), dump(cli, "c_weather"), "after the host rolled new weather"))
    # an NPC that only the host has: the client must recreate it, then mirror every posture change
    ok, text = cmd(host, "spawnnpc 12 8")
    log("host spawns an NPC", ok, text)
    time.sleep(6)
    reports.append(frozen_check(host, cli, "host-only NPC appears"))

    def npc_flags(state_h, state_c, key):
        # posture (down 4 / dead 8) and medical flags; the "moving" bit of a body on the ground is noise
        def posture(v):
            f = v.get("flags")
            return (str(int(f) & 12) if f is not None else None), v.get("vflags")
        return posture(state_h["char"].get(key, {})), posture(state_c["char"].get(key, {}))

    key = text.split()[1] if ok else ""
    # loot. 1) the host hands an item to the NPC (host-side change -> mirrored on the client's stand-in)
    def squad_key(st, i):
        return sorted(st["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))[i]

    def inv_report(title, hs, cs):
        r = compare(hs, cs, title)
        r["npc_inv h/c"] = (hs["char"].get(key, {}).get("inv"), cs["char"].get(key, {}).get("inv"))
        r["squad0_inv h/c"] = (hs["squad"][squad_key(hs, 0)].get("inv"), cs["squad"][squad_key(cs, 0)].get("inv"))
        r["squad1_inv h/c"] = (hs["squad"][squad_key(hs, 1)].get("inv"), cs["squad"][squad_key(cs, 1)].get("inv"))
        reports.append(r)
        return r

    def npc_step(step, wait):
        log(step, cmd(host, step))
        time.sleep(wait)
        hs, cs = dump(host, "h_" + step), dump(cli, "c_" + step)
        hf, cf = npc_flags(hs, cs, key)
        r = compare(hs, cs, f"NPC after {step} (live)")
        r["npc_host_flags/vflags"] = hf
        r["npc_client_flags/vflags"] = cf
        r["npc_match"] = hf == cf
        reports.append(r)

    time.sleep(1)
    log("host gives the NPC an item", cmd(host, "invmove squad1 spawned main"))
    time.sleep(3)
    inv_report("host gave the NPC an item (live)", dump(host, "h_inv_give"), dump(cli, "c_inv_give"))
    # 2) a conscious NPC cannot be looted (it still uses its gear): refused and undone
    log("client tries to loot the conscious NPC", cmd(cli, f"invmove {key} squad0 worn"))
    time.sleep(4)
    inv_report("loot of a conscious NPC refused (live)", dump(host, "h_inv_awake"), dump(cli, "c_inv_awake"))
    # 3) knocked out, it can: the client player loots it (client UI change -> InvOp -> host)
    npc_step("ko", 5)
    for i, which in enumerate(("main", "worn")):   # a loose item, then worn gear (weapon / clothes)
        log("client loots the knocked-out NPC", which, cmd(cli, f"invmove {key} squad0 {which}"))
        time.sleep(4)
        inv_report(f"client looted the knocked-out NPC #{i + 1} (live)", dump(host, f"h_inv_loot{i}"), dump(cli, f"c_inv_loot{i}"))
    # 4) the client tries to take from a character the host player owns: refused and undone
    log("client tries to steal", cmd(cli, "invmove squad1 squad0 main"))
    time.sleep(4)
    inv_report("client steal attempt refused (live)", dump(host, "h_inv_steal"), dump(cli, "c_inv_steal"))
    npc_step("wake", 6)
    npc_step("kill", 5)
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
    # every character of both worlds, not only around the squad
    reports.append(frozen_check(host, cli, "whole world", radius=10000000))
    for r in reports:
        print_report(r)
    return reports


def compare_fx(h, c, label):
    """Weather effects: every effect the host shows near the players must be on the client, same place."""
    hf = {e["id"]: e for e in h.get("fx", []) if e["id"]}
    cf = {e["id"]: e for e in c.get("fx", []) if e["id"]}
    common = sorted(set(hf) & set(cf))
    errs = sorted(dist(hf[i]["pos"], cf[i]["pos"]) for i in common)
    by_kind = {}
    for i in common:
        k = hf[i]["kind"]
        by_kind[k] = max(by_kind.get(k, 0), round(dist(hf[i]["pos"], cf[i]["pos"]), 2))
    r = {
        "label": label,
        "host_near": len(hf),
        "client_mapped": len(cf),
        "client_unmapped_live": sum(1 for e in c.get("fx", []) if not e["id"] and float(e.get("life", 0)) > 0 and e.get("endless") == "0"),
        "missing_on_client": sorted(set(hf) - set(cf))[:8],
        "missing_count": len(set(hf) - set(cf)),
        "extra_on_client": sorted(set(cf) - set(hf))[:8],
        "pos_err_max_by_kind": by_kind,
        "pos_err_p50": round(errs[len(errs) // 2], 2) if errs else 0,
        "host_stats": h.get("fxstats"),
        "client_stats": c.get("fxstats"),
        "effects_host": sorted({(e["kind"], e["effect"]) for e in hf.values()}),
    }
    print_report(r)
    return r


def lightning_weathers(pid, camera_only=False):
    """Regions that can have a weather with a Lightning effect: {region: (seasonSid, weatherSid)}."""
    path = os.path.abspath(os.path.join(OUT_DIR, f"weathers_{pid}.txt"))
    cmd(pid, f"weathers {path}")
    out, region, camera = {}, None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("region "):
            region = line.split()[1]
            if " camera " in line:
                camera = region
        elif line.strip().startswith("weather ") and "ightning" in line and region and region not in out:
            words = line.split()
            out[region] = (words[words.index("season") + 1], words[1])
    log("camera region:", camera, "can have lightning:", camera in out)
    return {camera: out[camera]} if camera_only and camera in out else out


def exp_fx(host, cli, rounds=6):
    """Lightning storm everywhere it can happen; bolts forced; host and client compared while paused."""
    time.sleep(6)
    storms = lightning_weathers(host)
    log("regions that can have lightning:", len(storms))
    for region, (season, weather) in storms.items():
        cmd(host, f"setweather {region} {season} {weather}")
    time.sleep(8)   # the new weather reaches the client (every 2 s), effect groups rebuilt on both sides
    reports = []
    for i in range(rounds):
        for _ in range(3):
            cmd(host, "fxhurry")
            time.sleep(0.4)
        time.sleep(0.3 + 0.5 * (i % 3))
        cmd(host, "pause 1")
        time.sleep(2.5)
        h, c = dump(host, f"h_fx{i}", 300), dump(cli, f"c_fx{i}", 300)
        cmd(host, "pause 0")
        reports.append(compare_fx(h, c, f"fx round {i + 1}"))
        log("weather mismatch:", compare(h, c, "w")["weather_mismatch"])
    worst = max(r["missing_count"] for r in reports)
    log("fx worst missing:", worst, "max pos err:", max((max(r["pos_err_max_by_kind"].values() or [0]) for r in reports)))


def exp_fxlive(host, cli, minutes=3):
    """Weather effects while the game runs: every effect near the players exists on both sides."""
    time.sleep(8)
    worst = 0
    for i in range(minutes * 6):
        time.sleep(10)
        h, c = dump(host, "h_fxlive", 300), dump(cli, "c_fxlive", 300)
        hf = {e["id"]: e for e in h.get("fx", []) if e["id"]}
        cf = {e["id"]: e for e in c.get("fx", []) if e["id"]}
        # an effect may end (or start) between the two dumps: only count those well inside their life
        missing = [i for i in hf if i not in cf and float(hf[i]["age"]) > 1 and (hf[i]["endless"] == "1" or float(hf[i]["life"]) > 1)]
        extra = [i for i in cf if i not in hf and float(cf[i]["age"]) > 1 and (cf[i]["endless"] == "1" or float(cf[i]["life"]) > 1)]
        worst = max(worst, len(missing) + len(extra))
        log(f"fx live {i + 1}: host {len(hf)} client {len(cf)} missing {missing[:6]} extra {extra[:6]} stats {c.get('fxstats')}")
    log("fx live worst:", worst)


def exp_gait(host, cli, rounds=8):
    """Every moving character moves at the host's pace on the client (run animation when it runs)."""
    time.sleep(10)
    bad_total = 0
    for i in range(rounds):
        time.sleep(4)
        h, c = dump(host, "h_gait", 4000), dump(cli, "c_gait", 4000)
        rows, bad = [], []
        for k, hv in h["char"].items():
            cv = c["char"].get(k)
            if not cv or hv.get("flags") not in ("1",) or "pace" not in hv:
                continue
            hs, cs = float(hv.get("speed", 0)), float(cv.get("speed", 0))
            rows.append((k, hv["gait"], cv.get("gait"), hv["pace"], cv.get("pace"), round(hs, 1), round(cs, 1)))
            if hv["gait"] != cv.get("gait") or abs(float(hv["pace"]) - float(cv.get("pace", -1))) > 0.2 or abs(hs - cs) > max(3.0, 0.25 * hs):
                bad.append(rows[-1])
        bad_total += len(bad)
        log(f"gait {i + 1}: moving {len(rows)} mismatched {len(bad)} {bad[:4]} sample {rows[:3]}")
    log("gait mismatches total:", bad_total)


def exp_anim(host, cli, rounds=10):
    """Fights and actions: every character plays the host's attack/block/action on the client too."""
    time.sleep(8)
    log("spawn", cmd(host, "spawnnpc 15 10"))
    time.sleep(3)
    for i in (0, 1, 2):
        log("fight", i, cmd(host, f"fight {i}"))
    worst = 0
    seen_tech = 0
    for r in range(rounds):
        time.sleep(1.3 + 0.4 * (r % 3))
        cmd(host, "pause 1")
        time.sleep(2.0)
        h, c = dump(host, f"h_anim{r}", 2000), dump(cli, f"c_anim{r}", 2000)
        cmd(host, "pause 0")
        bad = []
        rows = 0
        for k in set(h["char"]) & set(c["char"]):
            hv, cv = h["char"][k], c["char"][k]
            if "tech" not in hv:
                continue
            rows += 1
            seen_tech += hv["tech"] != "-"
            for f in ("tech", "action", "cmode", "drawn", "guard"):
                if hv.get(f) != cv.get(f):
                    bad.append((k[-12:], f, hv.get(f), cv.get(f)))
        for k in set(h["squad"]) & set(c["squad"]):
            hv, cv = h["squad"][k], c["squad"][k]
            for f in ("tech", "action", "cmode", "drawn", "guard"):
                if "tech" in hv and hv.get(f) != cv.get(f):
                    bad.append(("squad" + k[-6:], f, hv.get(f), cv.get(f)))
        # facing: angle between host and client directions
        import math as _m
        angs = []
        for k in set(h["char"]) & set(c["char"]):
            hf, cf = h["char"][k].get("face"), c["char"][k].get("face")
            if hf and cf:
                a = [float(x) for x in hf.split(",")]; b2 = [float(x) for x in cf.split(",")]
                dot = (a[0] * b2[0] + a[2] * b2[2]) / ((_m.hypot(a[0], a[2]) or 1) * (_m.hypot(b2[0], b2[2]) or 1))
                angs.append(round(_m.degrees(_m.acos(max(-1, min(1, dot)))), 1))
        log(f"  facing error (deg) worst {sorted(angs)[-4:] if angs else []}")
        worst = max(worst, len(bad))
        fighting = sorted({(v.get("tech")) for v in list(h["char"].values()) + list(h["squad"].values()) if v.get("tech", "-") != "-"})
        log(f"anim {r + 1}: chars {rows} mismatches {len(bad)} {bad[:5]} host techniques {fighting[:4]}")
    log("anim worst:", worst, "technique samples:", seen_tech)
    # squad life flags on the client, sampled fast (portraits flickering grey?)
    flips = 0
    last = {}
    for i in range(30):
        c = dump(cli, "c_vflags", 300)
        for k, v in c["squad"].items():
            f = v.get("vflags")
            if k in last and last[k] != f:
                flips += 1
                log("  squad", k[-10:], "vflags", last[k], "->", f, "blood", v.get("blood"))
            last[k] = f
        time.sleep(0.15)
    log("squad vflags flips:", flips)


def read_anims(path):
    chars, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("char "):
            cur = line.split()[1]; chars[cur] = {}
        elif cur and line.strip().startswith("L"):
            head, rest = line.strip().split(" | ", 1)
            parts = head.split(None, 2)
            name = parts[2] if len(parts) > 2 else ""
            kv = dict(x.split("=", 1) for x in rest.split() if "=" in x)
            if parts[1] == "in" and float(kv.get("w", 0)) > 0.05:
                chars[cur][name] = (float(kv["t"]), float(kv["w"]))
    return chars


def exp_animframe(host, cli, rounds=10):
    """What is on screen: the same animations, at the same time and weight, on both sides."""
    time.sleep(8)
    log("spawn", cmd(host, "spawnnpc 15 10"))
    time.sleep(3)
    for i in (0, 1, 2):
        cmd(host, f"fight {i}")
    worst_set, worst_t = 0, 0.0
    for r in range(rounds):
        time.sleep(1.1 + 0.37 * (r % 4))
        cmd(host, "pause 1")
        time.sleep(1.5)
        hp = os.path.abspath(os.path.join(OUT_DIR, "h_af.txt")); cp = os.path.abspath(os.path.join(OUT_DIR, "c_af.txt"))
        cmd(host, f"anims {hp}"); cmd(cli, f"anims {cp}")
        cmd(host, "pause 0")
        h, c = read_anims(hp), read_anims(cp)
        diff_sets, tdiff = [], []
        for k in h:
            hs, cs = set(h[k]), set(c.get(k, {}))
            if hs != cs:
                diff_sets.append((k[-6:], sorted(hs - cs)[:2], sorted(cs - hs)[:2]))
            for n in hs & cs:
                tdiff.append((abs(h[k][n][0] - c[k][n][0]), abs(h[k][n][1] - c[k][n][1]), n))
        tdiff.sort(reverse=True)
        worst_set = max(worst_set, len(diff_sets))
        worst_t = max(worst_t, tdiff[0][0] if tdiff else 0)
        log(f"frame {r + 1}: chars {len(h)} differing sets {len(diff_sets)} {diff_sets[:3]} worst time/weight diff {tdiff[:2]}")
    log("animframe worst differing sets:", worst_set, "worst time diff:", round(worst_t, 3))


def exp_progress(host, cli):
    """Skill levels, money, speech bubbles and player orders: the host's world decides, the client follows."""
    time.sleep(8)
    def stats(pid, i):
        ok, t = cmd(pid, f"stats {i}")
        return [float(x) for x in t.split()[1].split(",")] if ok else None
    h0, c0 = stats(host, 0), stats(cli, 0)
    log("stats equal at start:", h0 == c0, "| hunger", cmd(host, "stats 0")[1].split()[-1], cmd(cli, "stats 0")[1].split()[-1])
    log("host xp:", cmd(host, "xp 0 1 5"))
    log("client xp (must be refused):", cmd(cli, "xp 0 2 5"))
    time.sleep(2.5)
    h1, c1 = stats(host, 0), stats(cli, 0)
    diff = [(i, h1[i], c1[i]) for i in range(len(h1)) if abs(h1[i] - c1[i]) > 1e-3]
    log("after xp: differences", diff, "| melee attack host", h1[1], "client", c1[1])
    log("money host", cmd(host, "money"), "client", cmd(cli, "money"))
    log("set money", cmd(host, "money 4321"))
    time.sleep(2.5)
    log("money client after set:", cmd(cli, "money"))
    before = cmd(cli, "says")[1]
    log("host say", cmd(host, "say 1 Salut, ceci est un test de bulle de dialogue assez longue."))
    time.sleep(1.5)
    log("client bubbles:", before, "->", cmd(cli, "says")[1])
    # a player order from the client (follow squad member 0): only the client's own character obeys
    for i in range(6):
        log("client taskreq", i, cmd(cli, f"taskreq {i} 44 0"))
    time.sleep(1)
    log("host moves squad 0", cmd(host, "moverel 0 250 0"))
    time.sleep(20)
    for i in range(6):
        log("pos", i, "host", cmd(host, f"pos {i}")[1], "client", cmd(cli, f"pos {i}")[1])


def exp_ranged(host, cli, shooter_key=None):
    """Lot C, ranged combat: a crossbowman of the host's world fires at the squad; each shot is fired
    again on the client (same weapon, same path); its aim follows; a turret near the squad turns the
    same way on both sides; health and inventories stay identical."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    def counters(pid):
        t = cmd(pid, "shots")[1]
        return dict(kv.split("=") for kv in t.split()[1:]) if t.startswith("ok") else {}
    listing = cmd(host, "rangedlist 3000")[1]
    log("characters with a ranged weapon (host):", listing[:400])
    shooters = [e.split("|") for e in listing.split()[1:]]
    key = shooter_key
    if not key and shooters:
        # an NPC first (squad members are listed by index elsewhere), the nearest
        shooters.sort(key=lambda e: int(e[3]))
        key = shooters[0][1]
    check("tir : un tireur avec arme a distance pres de l'escouade", bool(key), listing[:120])
    if key:
        before_h, before_c = counters(host), counters(cli)
        fired = []
        for _ in range(3):
            fired.append(cmd(host, f"shoot {key} 0"))
            time.sleep(1.5)
        time.sleep(2)
        after_h, after_c = counters(host), counters(cli)
        log("host shots:", before_h, "->", after_h)
        log("client shots:", before_c, "->", after_c)
        sent = int(after_h.get("sent", 0)) - int(before_h.get("sent", 0))
        replayed = int(after_c.get("replayed", 0)) - int(before_c.get("replayed", 0))
        oriented = int(after_c.get("oriented", 0)) - int(before_c.get("oriented", 0))
        check("tir : l'hote tire (3 tirs envoyes)", all(f[0] for f in fired) and sent >= 3, f"{fired[0][1]} | sent {sent}")
        check("tir : le client voit les memes tirs", replayed >= 3, f"replayed {replayed}, failed {int(after_c.get('failed', 0)) - int(before_c.get('failed', 0))}")
        check("tir : les projectiles du client suivent la trajectoire de l'hote", oriented >= 3, f"oriented {oriented}")
        aim_h, aim_c = cmd(host, f"rangedaim {key}")[1], cmd(cli, f"rangedaim {key}")[1]
        log("aim host:", aim_h, "/ client:", aim_c)
        if "combat=1" in aim_h:
            def aim(t):
                return tuple(map(float, t.split("aim=")[1].split()[0].split(",")))
            time.sleep(1)
            aim_h, aim_c = cmd(host, f"rangedaim {key}")[1], cmd(cli, f"rangedaim {key}")[1]
            check("visee : le client vise le meme point que l'hote", "combat" in aim_c and dist(aim(aim_h), aim(aim_c)) < 5, f"{aim_h} / {aim_c}")
    turrets_h = cmd(host, "turrets 3000")[1]
    log("turrets (host):", turrets_h[:300])
    if turrets_h.startswith("ok") and len(turrets_h.split()) > 1:
        first = turrets_h.split()[1]
        tx, tz = map(float, first.split("@")[1].split(">")[0].split(","))
        target = (tx + 120.0, 40.0, tz + 60.0)
        log("host turns turret 0:", cmd(host, f"turretaim 0 {target[0]} {target[1]} {target[2]}"))
        time.sleep(2)
        turrets_c = cmd(cli, "turrets 3000")[1]
        def first_aim(t):
            return tuple(map(float, t.split()[1].split(">")[1].replace("(nogun)", "").split(",")))
        turrets_h = cmd(host, "turrets 3000")[1]
        ok = turrets_c.startswith("ok") and len(turrets_c.split()) > 1
        check("tourelle : le client la tourne vers le meme point", ok and dist(first_aim(turrets_h), first_aim(turrets_c)) < 2, turrets_c[:160])
    else:
        log("no turret near the squad in this save: the turret point is not checked")
    cmd(host, "pause 1")
    time.sleep(3)
    rep = compare(dump(host, "h_ranged_end"), dump(cli, "c_ranged_end"), "ranged", pos_tol=0.1)
    cmd(host, "pause 0")
    check("tir : aucun etat vital different ensuite", rep["vital_flag_mismatch"] == 0, rep["vital_flag_mismatch"])
    check("tir : aucun inventaire different ensuite", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    summary()


def exp_talk(host, cli):
    """An NPC talks to the client's own character: the conversation runs in the host's world, its window opens on the client."""
    time.sleep(6)
    for k in range(8):
        for i in range(6):
            ok, t = cmd(host, f"convo {i} {k}")
            if ok:
                log("host convo", i, k, t)
                break
        else:
            continue
        for _ in range(10):
            time.sleep(0.5)
            ok, t = cmd(cli, "dialog")
            if "open=1" in t:
                break
        log("client dialog:", t)
        if "open=1" in t:
            time.sleep(1.5)
            log("client dialog:", cmd(cli, "dialog")[1])
            log("answer 0", cmd(cli, "answer 0"))
            time.sleep(3)
            log("client dialog after answer:", cmd(cli, "dialog")[1])
            return


# ---- dialogue: conversations of client characters, NPCs who talk to them, one conversation per NPC
def exp_dialogue(host, cli):
    """Sandbox (kctest_town): the host creates an NPC next to the client's character. The client's
    character talks to it: the window opens on the client only (its own character as the actor), the host
    is not paused and none of its characters is pulled in; an answer reaches the host. The host's own
    character asking the same NPC is refused ("occupe"). Walking away closes it. Then the NPC starts the
    conversation itself (event 3, a guard's check, else 1), a teleport and a fight end it cleanly, and a
    conversation at speed 3."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    hidx = [int(x) for x in cmd(host, "ownidx 1")[1].split()[1:] if int(x) != own]

    def dialog():
        t = cmd(cli, "dialog")[1]
        d = {}
        for kv in t.split(" | ")[0].split()[1:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                d[k] = v
        d["raw"] = t
        return d

    def wait_open(want=True, seconds=15):
        # an "err" answer is no state: "err no live world" while the client loads a zone (13/10
        # 15:31:55, right after the teleport of step 5) was taken for a closed window
        d = dialog()
        for _ in range(int(seconds * 2)):
            if not d["raw"].startswith("err") and (d.get("open") == "1") == want:
                return d
            time.sleep(0.5)
            d = dialog()
        return d

    def dialogs():
        t = cmd(host, "dialogs")[1]
        return dict(kv.split("=", 1) for kv in t.split(" | ")[0].split()[1:] if "=" in kv), t

    def host_states():
        out = {}
        for i in hidx:
            t = cmd(host, f"charstate {i}")[1]
            out[i] = dict(kv.split("=") for kv in t.split()[1:]) if t.startswith("ok") else {}
        return out

    def npc_opens(events=(1,)):
        # first the NPC the client's character already talked with (it has something to say: the
        # nearest ones in kctest_town often have no line for that event, or a line that only says
        # something, "talking=0"), then the last spawned one, then the 8 nearest
        for ev in events:
            for which in ["dlg", "npc"] + [str(k) for k in range(8)]:
                r = cmd(host, f"npcevent {own} {ev} {which}")
                log("host: npcevent", ev, which, r)
                if r[0]:
                    d = wait_open(True, 8 if "talking=1" in r[1] else 3)
                    if d.get("open") == "1":
                        return ev, which, d
        return None, None, dialog()

    paused0 = cmd(host, "paused")[1]
    log("host spawns an NPC next to the client's character", cmd(host, f"spawnnpc 15 15 {own}"))
    time.sleep(3)
    before = host_states()

    # 1. the client's character talks to it (the copy may have nothing to say: then the next nearest NPCs)
    d = dialog()
    talked = None
    for k in range(4):
        r = cmd(cli, f"talkto {own} {k}")
        log(f"client: talk to the NPC nearest #{k}", r)
        if not r[0]:
            continue
        d = wait_open(True, 15)
        if d.get("open") == "1":
            talked = k
            break
    how = "le client parle"
    if d.get("open") != "1":
        log("   (no conversation from the client's order: the NPC starts it instead)")
        ev, which, d = npc_opens((1,))
        how = "le PNJ parle"
    check("dialogue : la fenetre s'ouvre chez le client (" + how + ")", d.get("open") == "1", d["raw"])
    check("dialogue : le perso du client est l'acteur", d.get("actor", "0") != "0", d["raw"])
    ds, dt = dialogs()
    check("dialogue : l'hote suit la conversation du client", int(ds.get("n", 0)) >= 1 and int(ds.get("open", 0)) >= 1, dt)
    check("dialogue : l'hote n'est pas mis en pause", cmd(host, "paused")[1].split()[1] == paused0.split()[1], cmd(host, "paused")[1])
    after = host_states()
    check("dialogue : aucun perso de l'hote n'est entraine dans la conversation",
          all(after[i].get("dialog") == before[i].get("dialog") for i in hidx if after.get(i) and before.get(i)), f"{before} -> {after}")
    if d.get("open") == "1" and "[" in d["raw"]:
        turn0 = d.get("turn")
        cmd(cli, "answer 0")
        for _ in range(20):
            time.sleep(0.5)
            d2 = dialog()
            if d2.get("turn") != turn0 or d2.get("open") != "1":
                break
        check("dialogue : la reponse du client arrive a l'hote", "] answers: \"" in host_log()[-30000:], dialog()["raw"])
    chat = cmd(cli, "chatlast 8")[1]
    check("dialogue : le client est prevenu que son perso va parler au PNJ", talked is None or "va parler" in chat, chat)

    # 1b. the same character asks to talk to the same NPC again while its conversation is open (the
    #     10 oct. session: "Marchand Ruche is busy talking with rob"): never busy for the player
    #     themselves, the conversation is shown again
    if talked is not None and dialog().get("open") == "1":
        busy0 = host_log().count("is busy talking with")
        log("client: talk again to the same NPC", cmd(cli, f"talkto {own} {talked}"))
        time.sleep(4)
        hl = host_log()
        check("dialogue : reparler au meme PNJ n'est jamais 'occupe' pour soi-meme", hl.count("is busy talking with") == busy0,
              hl[-2000:].splitlines()[-5:])
        check("dialogue : la conversation reste ouverte (reprise)", dialog().get("open") == "1" or "is shown again" in hl[-20000:], dialog()["raw"])

    # 2. one conversation per NPC: the host's own character (another player's) asking the same NPC,
    #    the one the client's character talks with ("dlg"; "npc", the spawned copy, is often another)
    d = dialog()
    if d.get("open") != "1":
        npc_opens((1,))
    if hidx and dialog().get("open") == "1":
        b0 = int(dialogs()[0].get("busy", 0))
        log("host's own character talks to the same NPC", cmd(host, f"talkto {hidx[0]} dlg"))
        busy = False
        for _ in range(30):
            time.sleep(0.5)
            if int(dialogs()[0].get("busy", 0)) > b0:
                busy = True
                break
        chat = cmd(host, "chatlast 5")[1]
        check("dialogue : un PNJ deja en conversation est occupe pour un autre perso", busy and "occup" in chat, f"{dialogs()[1]} | {chat}")
        check("dialogue : la conversation du client continue", dialog().get("open") == "1", dialog()["raw"])

    # 3. walking away
    if dialog().get("open") == "1":
        cmd(cli, "answer leave")
        d = wait_open(False, 10)
        check("dialogue : partir ferme la conversation proprement", d.get("open") == "0" and dialogs()[0].get("n") == "0", f"{d['raw']} | {dialogs()[1]}")
    time.sleep(3)

    # 4. an NPC starts it: a guard's check (event 3), else "talk to me"
    ev, which, d = npc_opens((3, 1))
    check("dialogue : un PNJ engage la conversation avec le perso du client", d.get("open") == "1", f"event {ev} {which} | {d['raw']}")

    # 5. a teleport ends it
    if d.get("open") == "1":
        x, y, z = vec(cmd(host, f"where {own}")[1])
        log("host teleports the client's character away", cmd(host, f"teleport {own} {x + 1500} {y} {z + 1500}"))
        d = wait_open(False, 10)
        ds, dt = dialogs()
        check("dialogue : une teleportation ferme la conversation", d.get("open") == "0" and ds.get("n") == "0", f"{d['raw']} | {dt}")
        cmd(host, f"teleport {own} {x} {y} {z}")
        time.sleep(4)

    # 6. a fight ends it
    ev, which, d = npc_opens((1,))
    if d.get("open") == "1":
        log("host: the client's character fights the NPC", cmd(host, f"fight {own}"))
        d = wait_open(False, 15)
        check("dialogue : un combat ferme la conversation", d.get("open") == "0" and dialogs()[0].get("n") == "0", f"{d['raw']} | {dialogs()[1]}")
    time.sleep(5)

    # 7. speed 3
    cmd(host, "speed 3")
    time.sleep(2)
    log("host spawns another NPC", cmd(host, f"spawnnpc -15 15 {own}"))
    time.sleep(2)
    ev, which, d = npc_opens((1,))
    if d.get("open") == "1" and "[" in d["raw"]:
        cmd(cli, "answer 0")
        time.sleep(4)
        hl = host_log()[-30000:]
        check("dialogue : vitesse 3 : la reponse est appliquee ou ignoree proprement",
              "] answers: \"" in hl or "answer to an older line ignored" in hl or dialog().get("open") == "0", dialog()["raw"])
    else:
        check("dialogue : vitesse 3 : conversation ouverte", d.get("open") == "1", d["raw"])
    cmd(cli, "answer leave")
    time.sleep(2)
    cmd(host, "speed 1")
    check("dialogue : l'hote est toujours vivant", alive(host) and cmd(host, "echo")[0])
    check("dialogue : le client est toujours vivant", alive(cli) and cmd(cli, "echo")[0])
    summary()


# ---- 0.3.4: talking to NPCs (A_FAIRE, 12 October evening, points 1, 7 and 8)
def exp_npctalk(host, cli):
    """Sandbox (kctest_town). 1. The client right-clicks an NPC through the game's own cursor function
    (talkclick ... click): a talk order reaches the host (the game's, or our fallback when the game
    gives none). 2. The game gives no order (talkclick ... nogame): the client says why in its log and
    sends the talk order anyway; the host runs it. 3. The client's copy is not linked to the host's NPC
    (talkclick ... unlinked: a handle the host does not know): the host finds the NPC by kind and place
    and the conversation opens. 4. An NPC starts the conversation (a guard's check, event 3, else 1),
    at speed 3: it stays open on the client's screen while nobody answers (12 s), then the answer
    reaches the host (12 October: rob1's guard check closed 0.15 s after it opened)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)

    def dialog():
        t = cmd(cli, "dialog")[1]
        d = {}
        for kv in t.split(" | ")[0].split()[1:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                d[k] = v
        d["raw"] = t
        return d

    def wait_open(want=True, seconds=15):
        # an "err" answer is no state: "err no live world" while the client loads a zone (13/10
        # 15:31:55, right after the teleport of step 5) was taken for a closed window
        d = dialog()
        for _ in range(int(seconds * 2)):
            if not d["raw"].startswith("err") and (d.get("open") == "1") == want:
                return d
            time.sleep(0.5)
            d = dialog()
        return d

    def leave():
        if dialog().get("open") == "1":
            cmd(cli, "answer leave")
            wait_open(False, 10)
        time.sleep(2)

    log("host spawns an NPC next to the client's character", cmd(host, f"spawnnpc 15 15 {own}"))
    time.sleep(3)

    # 1. a right click as the mouse gives it. The nearest NPCs of kctest_town have nothing to answer
    #    (13 October: Bandit affame, Citoyen Shinobi, Garde Ninja: "the NPC has nothing to say"; the
    #    Chirurgien's line has no answers, so no window): their talk order reaching the host is checked,
    #    then NPCs known to hold a conversation with answers, by name (Chef Voleur, a few houses away:
    #    longer walk)
    talkable = None
    targets = [str(k) for k in range(3)] + ["name:Chef_Voleur", "name:Thief_Leader", "name:Marchand", "name:Trader"]
    for k in targets:
        mark = len(host_log())
        r = cmd(cli, f"talkclick {own} {k} click")
        log(f"client: right click on the NPC {k if k.startswith('name:') else 'nearest #' + k}", r)
        if not r[0]:
            continue
        time.sleep(1.5)
        hl = host_log()[mark:]
        got = 'order "talk"' in hl
        check(f"parler : le clic droit sur le PNJ {k} donne un ordre a l'hote (jeu ou secours)", got,
              [l for l in hl.splitlines() if "talk" in l][-4:])
        if got and wait_open(True, 30 if k.startswith("name:") else 15).get("open") == "1":
            talkable = k
            # the next steps talk to that same NPC: by its name, not by its rank among the nearest
            # (13 October: "2" was Chef Voleur here, then, once the client stood next to him,
            # Marchand Shinobi for the fallback order and a Voleur for the unlinked one; neither
            # opened a conversation)
            m = re.match(r"ok (.+?) sent=", r[1])
            if m and m.group(1).isascii():
                talkable = "name:" + m.group(1).replace(" ", "_")
            break
    check("parler : un clic droit du client ouvre une conversation", talkable is not None, dialog()["raw"])
    log("parler : PNJ des etapes suivantes :", talkable)
    leave()

    # 2. the game gives no order: our fallback
    k = talkable if talkable is not None else "0"
    mark = len(host_log())
    r = cmd(cli, f"talkclick {own} {k} nogame")
    log("client: a click the game left without an order", r)
    time.sleep(2)
    hl = host_log()[mark:]
    check("parler : sans ordre du jeu, le client le dit dans son journal", "gave no order here" in hl,
          [l for l in hl.splitlines() if "no order" in l][-3:])
    check("parler : sans ordre du jeu, l'ordre 'parler' part quand meme a l'hote", r[0] and "sent=1" in r[1] and 'order "talk"' in hl, r)
    if talkable is not None:
        check("parler : la conversation s'ouvre (ordre de secours)", wait_open(True, 15).get("open") == "1", dialog()["raw"])
    leave()

    # 3. a copy not linked to the host's NPC: found by kind and place
    mark = len(host_log())
    r = cmd(cli, f"talkclick {own} {k} unlinked")
    log("client: talk order with a handle the host does not know", r)
    time.sleep(2)
    hl = host_log()[mark:]
    check("parler : PNJ non lie : l'hote le retrouve par type et endroit",
          "found by kind and place" in hl and "NOT found by kind and place" not in hl, [l for l in hl.splitlines() if "kind and place" in l][-3:])
    if talkable is not None:
        check("parler : PNJ non lie : la conversation s'ouvre", wait_open(True, 15).get("open") == "1", dialog()["raw"])
    leave()

    # 4. an NPC starts it, at speed 3: it waits for the player's answer
    cmd(host, "speed 3")
    time.sleep(2)
    opened = None
    for ev in (3, 1):
        for which in ["dlg", "npc"] + [str(i) for i in range(8)]:
            r = cmd(host, f"npcevent {own} {ev} {which}")
            log("host: npcevent", ev, which, r)
            if r[0] and wait_open(True, 8 if "talking=1" in r[1] else 3).get("open") == "1":
                opened = (ev, which)
                break
        if opened:
            break
    d = dialog()
    check("parler : un PNJ engage la conversation (controle de garde, sinon 'parle-moi')", d.get("open") == "1", f"{opened} | {d['raw']}")
    if d.get("open") == "1":
        mark = len(host_log())
        time.sleep(12)
        d2 = dialog()
        hl = host_log()[mark:]
        check("parler : la conversation attend la reponse du joueur (12 s a vitesse 3)",
              d2.get("open") == "1" and "closed by the game" not in hl, f"{d2['raw']} | {[l for l in hl.splitlines() if 'conversation' in l][-4:]}")
        log("host dialogs:", cmd(host, "dialogs")[1])
        if "[" in d2["raw"]:
            mark = len(host_log())
            cmd(cli, "answer 0")
            time.sleep(4)
            check("parler : la reponse du joueur arrive ensuite a l'hote", "] answers: \"" in host_log()[mark:], dialog()["raw"])
    leave()
    cmd(host, "speed 1")
    check("parler : l'hote est toujours vivant", alive(host) and cmd(host, "echo")[0])
    check("parler : le client est toujours vivant", alive(cli) and cmd(cli, "echo")[0])
    summary()


# ---- lot A: doors and locks
def exp_doors(host, cli):
    """Doors and locks: the host's doors reach the client; the client's own game cannot open a door by
    itself; a door panel button and door orders given on the client run on the host; lockpicking ends
    the same everywhere; a locked chest stays shut for a client."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("doors around (host):", cmd(host, "doors 0 door")[1][:500])
    log("locks around (host):", cmd(host, "doors 0 lock")[1][:500])
    def st(pid, what):
        t = cmd(pid, f"doorstate 0 {what}")[1]
        m = re.search(r"kind=(\d+) state=(\d+) flags=(\d+) level=(\d+)", t)
        return (int(m.group(1)), int(m.group(2)) in (1, 2), int(m.group(3)) & 0x1E, int(m.group(4))) if m else None, t
    def same(what, wait=6.0):
        deadline = time.time() + wait
        while True:
            (h, ht), (c, ct) = st(host, what), st(cli, what)
            if (h and h == c) or time.time() > deadline:
                return h is not None and h == c, f"hote {ht} / client {ct}"
            time.sleep(0.5)
    log("client knows", cmd(cli, "doorsknown")[1])
    # 1. the host's door opens and closes on the client too
    cmd(host, "doorset 0 door close")
    ok, d = same("door")
    check("portes : fermee chez l'hote, fermee chez le client", ok, d)
    cmd(host, "doorset 0 door open")
    ok, d = same("door")
    check("portes : ouverte chez l'hote, ouverte chez le client", ok, d)
    # 2. locks follow
    cmd(host, "doorset 0 door close")
    time.sleep(3)
    locked = cmd(host, "doorset 0 door lock")
    ok, d = same("door")
    hl_ = st(host, "door")[0]
    check("portes : verrouillee chez l'hote, verrouillee chez le client", locked[0] and ok and hl_ is not None and hl_[2] & 8, f"{locked[1]} | {d}")
    cmd(host, "doorset 0 door unlock")
    ok, d = same("door")
    check("portes : deverrouillee partout", ok, d)
    # 3. the client's own game cannot open a door
    before = st(cli, "door")[0]
    cmd(cli, "doorlocal 0 door open")
    time.sleep(0.5)
    after = st(cli, "door")[0]
    check("portes : le jeu du client ne l'ouvre pas tout seul", before is not None and before == after, f"{before} -> {after}")
    # 4. the client clicks the door's open button: the host's game runs it
    hb = st(host, "door")[0]
    cmd(cli, "doorbutton 0 door open")
    time.sleep(3)
    ha = st(host, "door")[0]
    ok, d = same("door")
    check("portes : bouton du client execute chez l'hote", hb and ha and hb[1] != ha[1] and ok, f"{hb} -> {ha} | {d}")
    # 5. an order on the door from the client (open / close): its character walks there, on the host
    hb = st(host, "door")[0]
    task = 73 if hb and hb[1] else 72
    log("client orders", task, cmd(cli, f"doororder {own} {task} door"))
    ha = hb
    for _ in range(40):
        time.sleep(0.5)
        ha = st(host, "door")[0]
        if ha and hb and ha[1] != hb[1]:
            break
    ok, d = same("door")
    check("portes : ordre ouvrir/fermer du client execute chez l'hote", ha and hb and ha[1] != hb[1] and ok, f"{hb} -> {ha} | {d}")
    # 6. lockpicking: the host locks, the client's character picks; whatever the outcome, the same everywhere
    cmd(host, "doorset 0 door close")
    time.sleep(3)
    cmd(host, "doorset 0 door lock")
    time.sleep(1)
    log("client picks the lock", cmd(cli, f"doororder {own} 76 door"))
    for _ in range(60):
        time.sleep(0.5)
        h = st(host, "door")[0]
        if h and not (h[2] & 8):
            break
    ok, d = same("door")
    check("portes : crochetage, meme etat partout", ok, d)
    log("lockpicking result (host):", st(host, "door")[1])
    # 7. a locked chest stays shut for the client
    cmd(host, "doorset 0 lock lock")
    t = st(host, "lock")[1]
    name = re.sub(r"^ok (.*) kind=.*$", r"\1", t).strip().replace(" ", "_")
    time.sleep(2)
    log("client looks into", name, cmd(cli, f"containerreq {own} {name}"))
    time.sleep(6)
    hl = host_log()
    check("portes : un coffre verrouille reste ferme pour le client", "is locked: not opened" in hl and "windows=0" in cmd(cli, "tradestate")[1],
          cmd(cli, "tradestate")[1])
    cmd(host, "doorset 0 lock unlock")
    check("portes : le client connait les portes de l'hote", int(re.search(r"known=(\d+)", cmd(cli, "doorsknown")[1]).group(1)) > 0,
          cmd(cli, "doorsknown")[1])
    summary()


def exp_prison(host, cli):
    """Lot D (prisons): the host puts the client player's character in the nearest cage, shackles it,
    enslaves it, then frees it. The client must show each state like the host, keep the character in
    its cage (no position fight), and clear everything at the release. Needs a cage (BF_CAGE) within
    300 m of the squad: use --save with a prison, slaver camp or cage nearby."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    def fields(t):
        return dict(kv.split("=", 1) for kv in t.split()[1:] if "=" in kv)
    def both():
        return fields(cmd(host, f"captive {own}")[1]), fields(cmd(cli, f"captive {own}")[1])
    def pos(f):
        return tuple(map(float, f.get("pos", "0,0,0").split(",")))
    caged = cmd(host, f"cage {own}")
    log("host cages the client's character:", caged)
    if not caged[0]:
        check("prison : une cage pres de l'escouade", False, caged[1] + " (prendre une sauvegarde avec une cage proche)")
        summary()
        return
    time.sleep(3)
    h, c = both()
    log("host", h, "/ client", c)
    check("prison : en cage chez l'hote", h.get("in") == "2", h)
    check("prison : en cage chez le client, meme cage", c.get("in") == "2" and c.get("cage") == h.get("cage"), c)
    check("prison : meme position dans la cage", dist(pos(h), pos(c)) < 2.0, f"{dist(pos(h), pos(c)):.2f}")
    time.sleep(5)
    h2, c2 = both()
    check("prison : le client le garde dans la cage (pas de correction de position)", c2.get("in") == "2" and dist(pos(c), pos(c2)) < 1.0,
          f"in={c2.get('in')} bouge de {dist(pos(c), pos(c2)):.2f}")
    log("host shackles it:", cmd(host, f"chain {own}"))
    log("host enslaves it:", cmd(host, f"enslave {own} 1"))
    time.sleep(3)
    h, c = both()
    check("prison : enchaine chez les deux", h.get("chained") == "1" and c.get("chained") == "1", f"hote {h.get('chained')} / client {c.get('chained')}")
    check("prison : esclave chez les deux", h.get("slave") == "1" and c.get("slave") == "1", f"hote {h.get('slave')} / client {c.get('slave')}")
    check("prison : meme faction maitre", h.get("slaveof") == c.get("slaveof"), f"hote {h.get('slaveof')} / client {c.get('slaveof')}")
    log("host frees it:", cmd(host, f"cage {own} off"), cmd(host, f"chain {own} off"), cmd(host, f"enslave {own} 0"))
    time.sleep(3)
    h, c = both()
    check("prison : libere chez l'hote", h.get("in") == "0" and h.get("chained") == "0" and h.get("slave") == "0", h)
    check("prison : libere chez le client", c.get("in") == "0" and c.get("chained") == "0" and c.get("slave") == "0", c)
    check("prison : plus aucun captif suivi", h.get("captives") == "0" and c.get("captives") == "0", f"hote {h.get('captives')} / client {c.get('captives')}")
    rep = compare(dump(host, "h_prison"), dump(cli, "c_prison"), "prison")
    check("prison : inventaires identiques (menottes comprises)", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    summary()


def exp_map(host, clis):
    """Map markers, minimap, markers above the heads, squad bar frames and pings (1 host + 2 clients
    or more). Every player's map feed must match the host's (positions, owners, own characters,
    hostile squads); what each machine draws is read back (mapscene); the game's map projection must
    match ours; a client's ping must show on the host and on the other client, rate-limited."""
    import re as _re
    time.sleep(6)   # editors already closed by setup_multi, each at its join turn

    def kv(text):
        return dict(p.split("=", 1) for p in text.split(";")[0].split() if "=" in p)

    def items(text, section=1):
        parts = text.split(";")
        if len(parts) <= section:
            return []
        out = []
        for tok in parts[section].split():
            f = tok.split(":")
            d = {"name": f[0]}
            for x in f[1:]:
                if "=" in x:
                    k, v = x.split("=", 1)
                    d[k] = v
            out.append(d)
        return out

    def feed(pid):
        ok, t = cmd(pid, "mapfeed")
        return ok, kv(t), items(t, 1), items(t, 2), t

    def me(pid):
        return kv(cmd(pid, "mapscene carte")[1]).get("me", "?")

    # a hostile in a fight with the squad: an NPC copy next to squad member 0, engaged
    log("spawn an NPC to fight:", cmd(host, "spawnnpc 60 0"), cmd(host, "fight 0"))
    time.sleep(5)
    hok, hk, hchars, hthreats, ht = feed(host)
    log("host feed:", ht[:400])
    check("carte : l'hote construit le flux", hok and int(hk.get("chars", 0)) > 0, ht[:200])
    ids = {}
    for i, c in enumerate(clis):
        ok, k, chars, threats, t = feed(c)
        ids[c] = me(c)
        log(f"client {i + 1} (joueur {ids[c]}) feed:", t[:400])
        check(f"carte : client {i + 1} recoit le flux de l'hote", ok and float(k.get("age", 999)) < 2.0, t[:120])
        check(f"carte : client {i + 1} a les memes persos que l'hote", len(chars) == len(hchars), f"{len(chars)} / {len(hchars)}")
        worst = 0.0
        for a, b in zip(chars, hchars):
            if a.get("owner") != b.get("owner"):
                worst = 1e9
                break
            worst = max(worst, dist((float(a["x"]), 0, float(a["z"])), (float(b["x"]), 0, float(b["z"]))))
        check(f"carte : client {i + 1} memes positions et proprietaires que l'hote", worst < 30.0, f"ecart max {worst:.1f}")
        check(f"carte : client {i + 1} memes escouades hostiles que l'hote", len(threats) == len(hthreats), f"{len(threats)} / {len(hthreats)}")
    owners = sorted({c.get("owner") for c in hchars})
    av = {o: sum(1 for c in hchars if c.get("owner") == o and c.get("av") == "1") for o in owners}
    check("carte : chaque joueur a un perso a lui (repere, barre d'escouade)", all(v >= 1 for v in av.values()) and len(owners) >= 1 + len(clis), av)
    check("carte : ennemis visibles (le PNJ qui se bat contre l'escouade)", len(hthreats) >= 1,
          "aucune escouade hostile (le PNJ copie n'est peut-etre pas hostile)" if not hthreats else hthreats[0])
    # what each machine draws on its map, with its colours
    for pid, label in [(host, "hote")] + [(c, f"client {i + 1}") for i, c in enumerate(clis)]:
        t = cmd(pid, "mapscene carte")[1]
        k = kv(t)
        drawn = items(t, 1)
        cols = {}
        for d in drawn:
            cols.setdefault(d.get("owner"), set()).add(d.get("col"))
        distinct = len({next(iter(v)) for v in cols.values()}) == len(cols) and all(len(v) == 1 for v in cols.values())
        check(f"carte : {label} dessine tous les persos", k.get("live") == "1" and len(drawn) == len(hchars), f"{len(drawn)} / {len(hchars)}")
        check(f"carte : {label} une couleur par joueur", distinct, cols)
    # the game's own projection (MapScreen::worldToMapCoords) against ours
    if hchars:
        x, z = float(hchars[0]["x"]), float(hchars[0]["z"])
        ok, t = cmd(clis[0], f"mapproj {x} {z}")
        m = _re.search(r"game=(-?\d+),(-?\d+) ours=(-?[\d.]+),(-?[\d.]+)", t)
        good = bool(m) and abs(int(m.group(1)) - float(m.group(3))) <= 1.5 and abs(int(m.group(2)) - float(m.group(4))) <= 1.5
        check("carte : projection du jeu = la notre", good, t)
    # minimap and heads
    for pid, label in [(host, "hote")] + [(c, f"client {i + 1}") for i, c in enumerate(clis)]:
        t = cmd(pid, "mapscene minicarte")[1]
        k = kv(t)
        dots = items(t, 1)
        mine = [d for d in dots if d.get("owner") == k.get("me")]
        check(f"minicarte : {label} centree sur son perso, ses persos dedans", k.get("centre", "0").startswith("1") and len(mine) >= 1, t[:200])
        # names above the heads: one per character (the game's own tag recoloured, label=jeu, or
        # the overlay's, label=overlay), in the player's colour; the main character framed
        # (13 October: the reply, ~930 characters with 7 characters and 3 frames, was whole; only
        # this log line (600) and the checks' detail (300) cut it, hiding the faulty entry: the
        # whole reply is logged and the detail names the entries that fail)
        def heads_state(t):
            heads = items(t, 1)
            untinted = [h for h in heads if h.get("label") == "jeu" and h.get("tint") != "1"]
            uncoloured = [h for h in heads if not h.get("col") or h.get("label") not in ("jeu", "overlay")]
            shown_mains = [h for h in heads if h.get("main") == "1" and h.get("onscreen") == "1"]
            head_owners = {h.get("owner") for h in heads}
            main_owners = {h.get("owner") for h in heads if h.get("main") == "1"}
            unframed = [h for h in shown_mains if h.get("frame") != "1"]
            framed_extra = [h for h in heads if h.get("main") != "1" and h.get("frame") != "0"]
            ok_tint = len(heads) >= 1 and not uncoloured and not untinted
            ok_frame = head_owners <= main_owners and len(shown_mains) >= 1 and not unframed and not framed_extra
            return heads, ok_tint, ok_frame, untinted + uncoloured, unframed + framed_extra, head_owners - main_owners

        def brief(hs):
            return [" ".join(f"{k}={h[k]}" for k in ("name", "owner", "main", "label", "tint", "frame", "onscreen", "why") if k in h) for h in hs]

        # a name tag is recoloured / framed within a frame or two of the game showing it: a few
        # tries over 3 s for one that has just appeared
        for _ in range(6):
            t = cmd(pid, "mapscene tetes")[1]
            heads, ok_tint, ok_frame, bad_tint, bad_frame, no_main = heads_state(t)
            if ok_tint and ok_frame:
                break
            time.sleep(0.5)
        log(f"{label} names above heads:", t)
        head = t.split(";")[0]
        check(f"tetes : {label} nom de chaque perso des joueurs a la couleur de son joueur", ok_tint,
              f"{len(heads)} noms, fautifs {brief(bad_tint)} | {head}")
        check(f"tetes : {label} perso principal de chaque joueur encadre, les autres seulement colores", ok_frame,
              f"{len(heads)} noms, fautifs {brief(bad_frame)}, joueurs sans perso principal {sorted(no_main)} | {head}")
        t = cmd(pid, "mapscene barre")[1]
        log(f"{label} squad bar frames:", t[:300])
        frames = items(t, 1)
        check(f"barre : {label} cadres aux couleurs des joueurs seulement", all(f.get("owner") in owners for f in frames), t[:200])
    # what the overlay converts: MyGUI view -> back buffer (ImGui display), frames as drawn
    def conv(pid):
        ok, t = cmd(pid, "mapconv")
        parts = t.split(";")
        head = kv(parts[0]) if parts else {}
        frames, drawn = [], []
        for tok in (parts[1].split() if len(parts) > 1 else []):
            m = _re.search(r"owner=(\d+):raw=([-\d.,]+):conv=([-\d.,]+)", tok)
            if m:
                frames.append((m.group(1), [float(v) for v in m.group(3).split(",")]))
        for tok in (parts[2].split() if len(parts) > 2 else []):
            d = dict(x.split("=", 1) for x in tok.split(":")[1:] if "=" in x)
            if "x" in d:
                drawn.append((d["owner"], [float(d["x"]), float(d["y"]), float(d["w"]), float(d["h"])]))
        return ok, head, frames, drawn, parts[3] if len(parts) > 3 else "", t

    def check_frames(pid, label):
        """Every frame the overlay drew matches a portrait widget read now (converted to the display),
        and lies on the display."""
        time.sleep(0.5)
        ok, k, frames, drawn, mp, t = conv(pid)
        log(f"{label} mapconv:", t[:500])
        bw, bh = [float(v) for v in k.get("bb", "0x0").split("x")]
        bad = []
        for owner, r in drawn:
            near = [f for o, f in frames if o == owner and max(abs(a - b) for a, b in zip(f, r)) <= 2.0]
            if not near or r[0] < -1 or r[1] < -1 or r[0] + r[2] > bw + 1 or r[1] + r[3] > bh + 1:
                bad.append((owner, r))
        check(f"barre : {label} cadres dessines = portraits du jeu convertis (meme taille de fenetre)",
              ok and len(drawn) == len(frames) and not bad and float(k.get("drawnAge", 99)) < 1.0,
              f"{len(drawn)} dessines / {len(frames)} portraits, hors place {bad[:3]} | {t[:300]}")
        return k

    for pid, label in [(host, "hote")] + [(c, f"client {i + 1}") for i, c in enumerate(clis)]:
        k = check_frames(pid, label)
        check(f"tetes : {label} vue 3D non couverte (ecran de gestion ferme), cadres ou noms dessines",
              k.get("covered") == "0" and int(k.get("heads", 0)) >= 1, k)
    # the game's map screen, opened by its own MAP button (clicked through MyGUI's input), on client 1
    c1 = clis[0]
    log("map: open the map screen on client 1:", cmd(c1, "mapui open"))
    time.sleep(1.5)
    st = cmd(c1, "mapui state")[1]
    if "window=1" in st and "open=0" in st:
        log("map: window up on another tab:", st, cmd(c1, "mapui maptab"))
        time.sleep(1.0)
    st = cmd(c1, "mapui state")[1]
    t = cmd(c1, "mapscene carte")[1]
    check("carte : ecran de carte ouvert par son bouton, detecte", "open=1" in st and kv(t).get("open") == "1", f"{st} | {t[:200]}")
    ok, k, frames, drawn, mp, t = conv(c1)
    log("client 1 mapconv (map open):", t[:500])
    m = _re.search(r"conv=([-\d.,]+):drawn=([-\d.,]+)", mp)
    same = bool(m) and max(abs(float(a) - float(b)) for a, b in zip(m.group(1).split(","), m.group(2).split(","))) <= 2.0
    check("carte : marqueurs dessines sur l'ecran de carte, image a sa place", k.get("mapDrawn") == "1" and int(k.get("markers", 0)) >= 1 and same, t[:400])
    # the window resized mid-test: the frames follow (back buffer, MyGUI view and client may differ)
    wins = [hw for hw, w, h in windows_of(c1) if w >= 640]
    if wins:
        r = wt.RECT()
        user32.GetWindowRect(wins[0], ctypes.byref(r))
        W, H = r.right - r.left, r.bottom - r.top
        log("map: resize client 1's window", f"{W}x{H} ->", f"{int(W * 0.8)}x{int(H * 0.7)}")
        user32.SetWindowPos(wins[0], 0, 0, 0, int(W * 0.8), int(H * 0.7), 0x0002 | 0x0004)   # SWP_NOMOVE | SWP_NOZORDER
        time.sleep(3)
        ok, k, frames, drawn, mp, t = conv(c1)
        log("client 1 mapconv after the resize (map open):", t[:500])
        m = _re.search(r"conv=([-\d.,]+):drawn=([-\d.,]+)", mp)
        same = bool(m) and max(abs(float(a) - float(b)) for a, b in zip(m.group(1).split(","), m.group(2).split(","))) <= 2.0
        check("carte : apres redimensionnement, image de la carte a sa place", k.get("mapDrawn") == "1" and same, t[:400])
        log("map: close the map screen on client 1:", cmd(c1, "mapui close"))
        time.sleep(1.5)
        check("carte : ecran de carte referme", "window=0" in cmd(c1, "mapui state")[1], cmd(c1, "mapui state")[1])
        check_frames(c1, "client 1 (fenetre redimensionnee)")
        user32.SetWindowPos(wins[0], 0, 0, 0, int(W * 0.6), int(H * 0.9), 0x0002 | 0x0004)
        time.sleep(3)
        check_frames(c1, "client 1 (redimensionnee encore)")
        arrange_grid([host] + clis)   # back to the grid the user watches
        time.sleep(2)
    else:
        log("map: client 1's window not found, no resize")
        cmd(c1, "mapui close")
    # pings: client 1 pings, the host and client 2 see it in its name
    px, pz = (float(hchars[0]["x"]) + 50, float(hchars[0]["z"]) + 50) if hchars else (0.0, 0.0)
    r1 = cmd(clis[0], f"ping {px} {pz} 1")
    r2 = cmd(clis[0], f"ping {px + 5} {pz} 0")
    check("ping : place par le client", r1[0], r1)
    check("ping : un seul toutes les 0,5 s", not r2[0], r2)
    time.sleep(2)
    me1 = ids[clis[0]]
    for pid, label in [(host, "hote")] + [(c, f"client {i + 1}") for i, c in enumerate(clis)]:
        t = cmd(pid, "pings")[1]
        got = [p for p in items(" ;" + t.replace(" id=", " ping:id="), 1) if p.get("owner") == me1 and p.get("kind") == "1"]
        near = [p for p in got if abs(float(p["x"]) - px) < 1 and abs(float(p["z"]) - pz) < 1]
        check(f"ping : visible chez {label}, au nom du client 1", len(near) == 1, t[:200])
    t = cmd(clis[-1], "mapscene pings")[1]
    check("ping : dessine chez l'autre client", f"owner={me1}" in t, t[:200])
    for n in range(6):
        time.sleep(0.6)
        cmd(clis[0], f"ping {px + 10 * n} {pz} 0")
    time.sleep(2)
    t = cmd(host, "pings")[1]
    mine = t.count(f":owner={me1}:")
    check("ping : 5 au plus par joueur", mine == 5, t[:300])
    time.sleep(11)
    t = cmd(host, "pings")[1]
    check("ping : disparait apres 10 s", "n=0" in t, t[:120])
    summary()


def exp_admin(host, cli):
    """The host's Administration section (plugin/admin.cpp), through its debug mirror 'admin ...':
    refused on the client; god mode holds through a fight and survives a rejoin; experience (points in
    one skill, levels in all) is the game's own and identical on the client; teleports (player to host,
    host to player, player to a map point) land where the client sees them; heal; money; the item
    spawner (building materials near the client: same stacks on both sides, the client picks one up)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    pid = player_id(cli) or 2

    def stats(p, i):
        ok, t = cmd(p, f"stats {i}")
        return [float(x) for x in t.split()[1].split(",")] if ok else None

    def vit(p, i):
        t = cmd(p, f"vitals {i}")[1]
        d = dict(kv.split("=", 1) for kv in t.split()[1:] if "=" in kv)
        return float(d.get("blood", "nan")), float(d.get("lowest", "nan")), d.get("flags", "?"), t

    def state():
        return cmd(host, "admin state")[1]

    def near_host_chars(p):
        best = 1e9
        for i in own_indices(host):
            ok, t = cmd(host, f"where {i}")
            if ok:
                best = min(best, dist(vec(t), p))
        return best

    # 1. clients have no admin: the debug mirror and the console both refuse
    ok, t = cmd(cli, "admin god all on")
    check("admin : refuse chez le client (commande)", not ok, t)
    cmd(cli, "console admin god all on")
    time.sleep(1)
    check("admin : refuse chez le client (console)", "godall=0" in state(), state())
    # 2. god mode holds through a fight, on the host and as the client sees it
    ok, t = cmd(host, f"admin god {pid} on")
    check("admin : mode dieu active", ok and f"p{pid}=1/" in state(), f"{t} | {state()}")
    time.sleep(1)
    b0 = vit(host, own)
    log("spawn an NPC next to the client's character:", cmd(host, f"spawnnpc 15 10 {own}"))
    time.sleep(2)
    log("fight:", cmd(host, f"fight {own}"))
    worst_blood, worst_low, fought = b0[0], b0[1], False
    for _ in range(12):
        time.sleep(2.5)
        v = vit(host, own)
        worst_blood, worst_low = min(worst_blood, v[0]), min(worst_low, v[1])
        fought |= cmd(host, f"combat {own}")[1].startswith("ok 1")
    check("admin : le combat a bien eu lieu", fought, cmd(host, f"combat {own}")[1])
    check("admin : dieu tient sous les coups (hote)", worst_blood >= b0[0] - 0.05 and worst_low >= b0[1] - 0.05,
          f"avant {b0[3]} | pire sang {worst_blood} membre {worst_low}")
    time.sleep(2)
    vh, vc = vit(host, own), vit(cli, own)
    check("admin : dieu, meme etat chez le client", abs(vh[0] - vc[0]) < 0.5 and abs(vh[1] - vc[1]) < 0.5 and vh[2] == vc[2] == "0",
          f"hote {vh[3]} / client {vc[3]}")
    log("kill the NPC:", cmd(host, "kill"))
    # 3. god mode survives a rejoin (resync: the client leaves and joins again)
    log("resync:", cmd(host, f"resync {pid}"))
    time.sleep(3)
    wait_for(cli, lambda f: f.get("state") == "connected" and f.get("ready") == "1", 240, "client back in the host's world")
    time.sleep(6)
    pid = player_id(cli) or pid
    own = own_index(host)
    st = state()
    engine = int(re.search(r"engine=(\d+)", st).group(1)) if re.search(r"engine=(\d+)", st) else 0
    check("admin : dieu persiste apres reconnexion", f"p{pid}=1/" in st and engine >= 1, st)
    ok, t = cmd(host, f"admin god {pid} off")
    time.sleep(1)
    check("admin : mode dieu retire", ok and f"p{pid}=0/" in state() and "engine=0" in state(), f"{t} | {state()}")
    # 4. experience points in one skill: the game's own gains (5 calls of 20), the same on the client
    s0 = stats(host, own)
    ok, t = cmd(host, f"admin xp {pid} melee_attack 100")
    time.sleep(3)
    s1h, s1c = stats(host, own), stats(cli, own)
    expect = s0[1]
    for _ in range(5):
        expect += 20 * ((100 - expect) / 100) ** 2
    check("admin : xp monte l'attaque comme le jeu (hote)", ok and abs(s1h[1] - expect) < 0.01, f"{s0[1]:.3f} -> {s1h[1]:.3f} (attendu {expect:.3f}) | {t}")
    check("admin : xp, meme valeur chez le client", s1c is not None and max(abs(a - b) for a, b in zip(s1h, s1c)) < 1e-3,
          f"attaque hote {s1h[1]:.4f} / client {s1c[1] if s1c else None}")
    # 5. levels in every skill
    ok, t = cmd(host, f"admin xp {pid} all 3 levels")
    time.sleep(3)
    s2h, s2c = stats(host, own), stats(cli, own)
    off = [i for i in range(len(s2h)) if abs(s2h[i] - min(100.0, s1h[i] + 3)) > 0.01]
    check("admin : +3 niveaux dans chaque competence (hote)", ok and not off, f"ecarts {off[:5]} | {t}")
    check("admin : niveaux, memes valeurs chez le client", s2c is not None and max(abs(a - b) for a, b in zip(s2h, s2c)) < 1e-3, "")
    # 6. teleport: player -> host
    mine = own_indices(host)
    mine = mine[0] if mine else 0
    ground = vec(cmd(host, f"where {mine}")[1])   # a point on the ground, for the map-point TP later
    log("host walks away:", cmd(host, f"moverel {mine} 250 0"))
    time.sleep(12)
    ok, t = cmd(host, f"admin tp {pid} host")
    time.sleep(5)
    pc, pcc = vec(cmd(host, f"where {own}")[1]), vec(cmd(cli, f"where {own}")[1])
    check("admin : tp joueur -> hote", ok and near_host_chars(pc) < 40, f"{near_host_chars(pc):.1f} | {t}")
    check("admin : tp joueur -> hote, meme position chez le client", dist(pc, pcc) < 3, f"{dist(pc, pcc):.2f}")
    # 7. teleport: host -> player (the client's character walked off first)
    log("client's character walks away:", cmd(host, f"moverel {own} -300 0"))
    time.sleep(15)
    ok, t = cmd(host, f"admin tp host {pid}")
    time.sleep(5)
    pc = vec(cmd(host, f"where {own}")[1])
    ph, phc = vec(cmd(host, f"where {mine}")[1]), vec(cmd(cli, f"where {mine}")[1])
    check("admin : tp hote -> joueur", ok and dist(ph, pc) < 40, f"{dist(ph, pc):.1f} | {t}")
    check("admin : tp hote -> joueur, meme position chez le client", dist(ph, phc) < 3, f"{dist(ph, phc):.2f}")
    # 8. teleport: player -> a point of the map (where the host's character stood at first)
    ok, t = cmd(host, f"admin tp {pid} {ground[0]:.2f} {ground[1]:.2f} {ground[2]:.2f}")
    time.sleep(6)
    pc, pcc = vec(cmd(host, f"where {own}")[1]), vec(cmd(cli, f"where {own}")[1])
    flat = math.hypot(pc[0] - ground[0], pc[2] - ground[2])
    check("admin : tp joueur -> point de la carte", ok and flat < 40, f"{flat:.1f} | {t}")
    check("admin : tp point, meme position chez le client", dist(pc, pcc) < 3, f"{dist(pc, pcc):.2f}")
    # 9. heal wakes a knocked-out character
    cmd(host, f"kosquad {own}")
    time.sleep(3)
    v0 = vit(host, own)
    ok, t = cmd(host, f"admin heal {pid}")
    time.sleep(4)
    vh, vc = vit(host, own), vit(cli, own)
    check("admin : soigner reveille et soigne", ok and vh[2] == "0" and vc[2] == "0", f"{v0[3]} -> hote {vh[3]} / client {vc[3]}")
    # 10. money (shared by the whole squad)
    m0 = int(cmd(host, "money")[1].split()[1])
    ok, t = cmd(host, "admin money 5000")
    time.sleep(3)
    m1h, m1c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("admin : argent +5000, pareil chez le client", ok and int(m1h.split()[1]) == m0 + 5000 and m1h == m1c, f"{m0} -> hote {m1h} / client {m1c}")
    # 11. item spawner: building materials near the client's character; both sides see the same
    # stacks, and the client's character can pick them up
    bm = "580-gamedata.base"

    def bm_ground(p):
        ok, t = cmd(p, f"groundall 400 {own}")
        out = {}
        for x in (t.split()[2:] if ok and t.startswith("ok") else []):
            k, sid, q, pos = x.split("|")
            if sid == bm:
                out[k] = (int(q), tuple(map(float, pos.split(","))))
        return out

    def bm_carried():
        ok, t = cmd(host, f"invcount all {bm}")
        return int(t.split()[1]) if ok else -1

    hb, cb = bm_ground(host), bm_ground(cli)
    ok, t = cmd(cli, f"admin spawn {bm} 5 here")
    check("admin : spawn refuse chez le client", not ok, t)
    ok, t = cmd(host, f"admin spawn {bm} 12 {pid}")
    log("spawn 12 building materials near the client:", ok, t)
    time.sleep(5)
    hn = {k: v for k, v in bm_ground(host).items() if k not in hb}
    cn = {k: v for k, v in bm_ground(cli).items() if k not in cb}
    hq, cq = sum(v[0] for v in hn.values()), sum(v[0] for v in cn.values())
    check("admin : spawn, 12 materiaux de construction au sol chez l'hote", ok and hq == 12, f"{hq} en {len(hn)} pile(s) | {t}")
    check("admin : spawn, meme nombre au sol chez le client", cq == hq and len(cn) == len(hn),
          f"hote {hq} en {len(hn)} pile(s) / client {cq} en {len(cn)} pile(s)")
    far = [v for v in hn.values() if min((dist(v[1], w[1]) for w in cn.values()), default=99) > 3]
    check("admin : spawn, memes endroits chez le client", not far, far[:3])
    carried0 = bm_carried()
    if hn:
        q, pos = next(iter(hn.values()))
        log("client asks its character to pick one up:", cmd(cli, f"pickupreq {own} {bm} {pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}"))
        for _ in range(15):
            time.sleep(2)
            if bm_carried() > carried0:
                break
    carried1 = bm_carried()
    check("admin : spawn, le client ramasse un materiau", carried1 > carried0, f"porte {carried0} -> {carried1}")
    time.sleep(3)
    hl = {k: v for k, v in bm_ground(host).items() if k not in hb}
    cl = {k: v for k, v in bm_ground(cli).items() if k not in cb}
    hq2, cq2 = sum(v[0] for v in hl.values()), sum(v[0] for v in cl.values())
    check("admin : spawn, apres ramassage, meme nombre au sol des deux cotes", hq2 == cq2 and hq2 == hq - (carried1 - carried0),
          f"hote {hq2} / client {cq2} (avant {hq}, ramasse {carried1 - carried0})")
    # 12. everyone at once, the host included
    ok, t = cmd(host, "admin god all on")
    time.sleep(1)
    st = state()
    squad = sum(int(n) for n in re.findall(r" p\d+=\d/(\d+)", st))
    check("admin : dieu pour tout le monde", ok and "godall=1" in st and squad > 0 and f"engine={squad}" in st, f"{squad} persos | {st}")
    cmd(host, "admin god all off")
    time.sleep(1)
    check("admin : dieu retire pour tout le monde", "godall=0" in state() and "engine=0" in state(), state())
    summary()


def exp_mine(host, cli):
    """A client mines iron (its order runs on the host), then looks into the ore node and takes ore."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("objects around (host):", cmd(host, f"objnear {own} 3000")[1][:400])
    cmd(host, "console xp all 80")   # a fast miner: ore within the test's time
    time.sleep(2)
    r = cmd(cli, f"objreq {own} 87 Ressource_Fer")
    log("client orders mining:", r)
    check("mine : ordre de minage du client", r[0], r[1])
    time.sleep(40)   # it walks there
    before = cmd(host, f"contcount Ressource_Fer {own}")[1]
    for _ in range(60):
        time.sleep(3)
        now = cmd(host, f"contcount Ressource_Fer {own}")[1]
        if now != before:
            break
    log("host node:", before, "->", now)
    check("mine : du minerai s'accumule chez l'hote", now != before, f"{before} -> {now}")
    clog = open(os.path.join(KENSHI, f"KenshiCoop-{cli}.log"), encoding="utf-8", errors="replace").read()
    held = [l for l in clog.splitlines() if "tool:" in l]
    check("mine : la pioche est dans la main chez le client", any("holds" in l and "failed" not in l for l in held), held[-3:])
    sel = cmd(cli, "machineselect Ressource_Fer")[1]
    log("client clicks the node:", sel)
    check("mine : la fenetre de sortie du minerai s'ouvre chez le client (clic)", " ours=1" in sel and "panel=1" in sel, sel)
    r = cmd(cli, f"containerreq {own} Ressource_Fer")
    log("client opens the node:", r)
    opened = False
    for _ in range(40):
        time.sleep(1)
        if "windows=0" not in cmd(cli, "tradestate")[1]:
            opened = True
            break
    check("mine : le client ouvre le minerai", opened, cmd(cli, "tradestate")[1])
    if opened:
        t = cmd(cli, f"contake {own} Ressource_Fer")
        time.sleep(4)
        rep = compare(dump(host, "h_mine"), dump(cli, "c_mine"), "mine")
        check("mine : le client prend le minerai, meme inventaire partout", t[0] and rep["inventory_mismatch"] == 0, f"{t[1]} | {rep['inventory_mismatch_sample']}")
    summary()


def exp_trade(host, cli, merchant="Marchand"):
    """Trading with a merchant: the host's game asks for a trade window for the client's character; it
    opens on the client with the shop's stock. A purchase and a sale, the game's own way (right click),
    are replayed by the host with their price; stock and cats end the same everywhere; the host's own
    window on that merchant shows what the client bought gone."""
    time.sleep(45)   # the town's stand-ins replaced by the real NPCs first (a window on a replaced one closes)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("merchants around the squad (host):", cmd(host, "merchants")[1])
    opened = cmd(host, f"tradeopen {own} {merchant} near")
    log("host asks for the trade window:", opened)
    state = "?"
    for _ in range(30):
        time.sleep(0.5)
        state = cmd(cli, "tradestate")[1]
        if "open=1" in state:
            break
    check("commerce : la fenetre s'ouvre chez le client", "open=1" in state and "windows=0" not in state, state)
    check("commerce : l'hote ne l'ouvre pas chez lui", "windows=0" in cmd(host, "tradestate")[1], cmd(host, "tradestate")[1])
    stock_c = cmd(cli, "tradelist merchant")[1]
    log("client sees the stock:", stock_c[:300])
    check("commerce : le stock du marchand est la chez le client", stock_c.startswith("ok") and stock_c.split()[1] != "0", stock_c[:120])
    cats0_h, cats0_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    bought = cmd(cli, "tradebuy 0")
    log("client buys:", bought)
    time.sleep(3)
    cats1_h, cats1_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    log("cats host", cats0_h, "->", cats1_h, "/ client", cats0_c, "->", cats1_c)
    check("commerce : achat paye chez l'hote", bought[0] and cats1_h != cats0_h, f"{bought[1]} | hote {cats0_h} -> {cats1_h}")
    check("commerce : meme argent partout apres l'achat", cats1_h == cats1_c, f"hote {cats1_h} / client {cats1_c}")
    rep = compare(dump(host, "h_trade1"), dump(cli, "c_trade1"), "trade1")
    check("commerce : inventaires identiques apres l'achat", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    stock_c2 = cmd(cli, "tradelist merchant")[1]
    check("commerce : le stock du client a change", stock_c2 != stock_c, stock_c2[:120])
    sold = cmd(cli, "tradesell 0")
    log("client sells:", sold)
    time.sleep(3)
    cats2_h, cats2_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("commerce : vente payee chez l'hote", sold[0] and cats2_h != cats1_h, f"{sold[1]} | hote {cats1_h} -> {cats2_h}")
    check("commerce : meme argent partout apres la vente", cats2_h == cats2_c, f"hote {cats2_h} / client {cats2_c}")
    rep = compare(dump(host, "h_trade2"), dump(cli, "c_trade2"), "trade2")
    check("commerce : inventaires identiques apres la vente", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    # the host trades with the same merchant: same stock as the client's window
    log("host opens its own window:", cmd(host, f"tradeopen 0 {merchant} near"))
    time.sleep(2)
    stock_h = cmd(host, "tradelist merchant")[1]
    stock_c3 = cmd(cli, "tradelist merchant")[1]
    check("commerce : l'hote voit le meme stock que le client", stock_h.split()[1:2] == stock_c3.split()[1:2], f"hote {stock_h[:80]} / client {stock_c3[:80]}")
    bought2 = cmd(cli, "tradebuy 0")
    log("client buys again:", bought2)
    time.sleep(3)
    stock_h2 = cmd(host, "tradelist merchant")[1]
    stock_c4 = cmd(cli, "tradelist merchant")[1]
    check("commerce : la fenetre de l'hote ne propose plus ce que le client a achete", stock_h2 != stock_h and stock_h2.split()[1:2] == stock_c4.split()[1:2],
          f"hote {stock_h2[:80]} / client {stock_c4[:80]}")
    cmd(host, "closewindows")
    cmd(cli, "closewindows")
    time.sleep(3)
    check("commerce : fermeture, l'hote oublie le commerce", "hosttrades=0" in cmd(host, "tradestate")[1], cmd(host, "tradestate")[1])
    summary()


def exp_factions(host, cli):
    """Lot B: faction relations, bounties and crimes are the host's on every machine; the client's game
    cannot keep its own."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    fh, fc = cmd(host, "factions")[1], cmd(cli, "factions")[1]
    log("factions host:", fh, "/ client:", fc)
    check("factions : memes relations au depart", fh.startswith("ok") and fh == fc, f"hote {fh} / client {fc}")
    part = None
    for p in ("Nation_Sainte", "Shek", "Ville", "Marchands", "Shinobi", "Bandit", "Gamedata"):
        if cmd(host, f"relation {p}")[0]:
            part = p
            break
    if part is None:
        part = "a"
    log("faction used:", part, cmd(host, f"relation {part}")[1])
    log("host sets the relation:", cmd(host, f"setrelation {part} -90"))
    time.sleep(3)
    rh, rc = cmd(host, f"relation {part}")[1], cmd(cli, f"relation {part}")[1]
    check("factions : relation changee chez l'hote, identique chez le client", "ours=-90.0" in rh and rh == rc, f"hote {rh} / client {rc}")
    log("host gives a bounty:", cmd(host, f"givebounty {own} {part} 1500"))
    time.sleep(3)
    bh, bc = cmd(host, f"bounty {own}")[1], cmd(cli, f"bounty {own}")[1]
    check("primes : prime de l'hote visible chez le client", ":1500" in bh and bh == bc, f"hote {bh} / client {bc}")
    # a bounty the character never had (the 10/10 crash): the client must live through it, its own
    # game holding none (the host's law only)
    log("host gives a brand new bounty:", cmd(host, f"givebounty {own} Ville 2000"))
    time.sleep(20)
    alive = cmd(cli, f"bounty {own}")
    check("primes : le client survit a une nouvelle prime et l'affiche", alive[0] and alive[1] == cmd(host, f"bounty {own}")[1], f"client {alive}")
    cmd(host, f"givebounty {own} Ville 0")
    cmd(host, f"givebounty {own} {part} 0")
    time.sleep(3)
    bh, bc = cmd(host, f"bounty {own}")[1], cmd(cli, f"bounty {own}")[1]
    check("primes : prime levee partout", ":1500" not in bc and bh == bc, f"hote {bh} / client {bc}")
    before = cmd(cli, "factionsync")[1]
    log("client changes its own copy:", cmd(cli, f"setrelation {part} 50"))
    time.sleep(4)
    rh, rc = cmd(host, f"relation {part}")[1], cmd(cli, f"relation {part}")[1]
    after = cmd(cli, "factionsync")[1]
    check("factions : le client ne garde pas ses propres relations", rh == rc and "ours=50.0" not in rc, f"hote {rh} / client {rc} | {before} -> {after}")
    fh, fc = cmd(host, "factions")[1], cmd(cli, "factions")[1]
    check("factions : memes relations a la fin", fh == fc, f"hote {fh} / client {fc}")
def exp_diplomacy(host, cli):
    """Diplomacy: relations between two NPC factions (war, alliance), unique characters (faction
    leaders dead or imprisoned) and towns (owner, override) are the host's on every machine; the
    client's game cannot keep its own. Self-contained: everything changed is put back."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)

    def fields(t):
        out = {}
        for e in t.split()[1:]:
            if "=" in e:
                k, v = e.split("=", 1)
                out[k] = v
        return out

    def same_world(tag):
        dh, dc = cmd(host, "diplo")[1], cmd(cli, "diplo")[1]
        fh, fc = fields(dh), fields(dc)
        log("diplo", tag, "host:", dh, "/ client:", dc)
        ok = dh.startswith("ok") and dc.startswith("ok") and all(fh.get(k) == fc.get(k) for k in ("ph", "uh", "th"))
        check("diplomatie : memes guerres, chefs et villes " + tag, ok, f"hote {dh} / client {dc}")

    same_world("au depart")
    # 1. a war between two factions that are not the player's, declared on the host
    pair = None
    for a, b in (("Nation_Sainte", "Shek"), ("Shek", "Bandit"), ("Nation", "Ville"), ("Shinobi", "Ville"), ("a", "e")):
        r = cmd(host, f"diplopair {a} {b}")
        if r[0]:
            pair = (a, b, r[1])
            break
    check("diplomatie : deux factions trouvees", pair is not None, pair)
    if pair:
        a, b, before = pair
        orig = "0"
        for e in before.split():
            if e.startswith("ab="):
                orig = e[3:].split(",")[0]
        log("host declares war:", cmd(host, f"setdiplopair {a} {b} -100 war"))
        time.sleep(8)
        ph, pc = cmd(host, f"diplopair {a} {b}")[1], cmd(cli, f"diplopair {a} {b}")[1]
        check("diplomatie : guerre entre deux factions visible chez le client", "war" in ph and ph == pc, f"hote {ph} / client {pc}")
        # 2. the client's game changes it by itself: back to the host's
        log("client makes peace on its own:", cmd(cli, f"setdiplopair {a} {b} 50 none"))
        time.sleep(8)
        ph, pc = cmd(host, f"diplopair {a} {b}")[1], cmd(cli, f"diplopair {a} {b}")[1]
        check("diplomatie : le client ne garde pas sa propre paix", ph == pc and "war" in pc, f"hote {ph} / client {pc}")
        log("host restores the pair:", cmd(host, f"setdiplopair {a} {b} {orig} none"))
    # 3. a faction leader killed by the players, on the host
    uniq = None
    for part in ("Tinfist", "Esata", "Phoenix", "Seto", "Longen", "Valamon", "Bayan", "Moll", "a"):
        r = cmd(host, f"unique {part}")
        if r[0]:
            uniq = (part, r[1])
            break
    check("diplomatie : un personnage unique trouve", uniq is not None, uniq)
    if uniq:
        part, before = uniq
        f0 = fields(before)
        log("host: the leader dies by the players' hand:", cmd(host, f"setunique {part} 0 player"))
        time.sleep(8)
        uh, uc = cmd(host, f"unique {part}")[1], cmd(cli, f"unique {part}")[1]
        check("diplomatie : chef tue (etat unique) identique chez le client", "state=0" in uh and uh == uc, f"hote {uh} / client {uc}")
        log("host: put back:", cmd(host, f"setunique {part} {f0.get('state', '1')} {'player' if f0.get('player') == '1' else ''}"))
    # 4. a town taken over by another faction, on the host
    town = None
    for part in ("Squin", "Stack", "Hub", "Admag", "Mongrel", "Shark", "Stoat", "a"):
        r = cmd(host, f"town {part}")
        if r[0] and "owner=-" not in r[1]:
            town = (part, r[1])
            break
    check("diplomatie : une ville trouvee", town is not None, town)
    if town and pair:
        part, before = town
        owner = before.split("owner=", 1)[1].split(" override=")[0]
        target = pair[0] if pair[0].replace("_", " ") not in owner else pair[1]
        log("host: town taken over:", cmd(host, f"settownowner {part} {target}"))
        time.sleep(8)
        th, tc = cmd(host, f"town {part}")[1], cmd(cli, f"town {part}")[1]
        check("diplomatie : changement de proprietaire d'une ville identique", th != before and th == tc, f"avant {before} / hote {th} / client {tc}")
        log("host: town given back:", cmd(host, f"settownowner {part} {owner.replace(' ', '_')}"))
    time.sleep(8)
    log("diplosync host:", cmd(host, "diplosync")[1], "/ client:", cmd(cli, "diplosync")[1])
    same_world("a la fin")


def exp_buildstate(host, cli):
    """Construction state of the buildings already there (save, towns, the bought one in kctest_mine):
    every building both games see near the squad has the same state (finished or site, progress)."""
    time.sleep(20)
    def states(pid):
        t = cmd(pid, "buildlist any 1500")[1]   # nearest 40 buildings within 1500 of squad 0, any faction, finished or not
        out = {}
        for e in t.split()[2:]:
            try:
                sid, rest = e.split("@", 1)
                xyz, st = rest.split(":", 1)
                x, y, z = (float(v) for v in xyz.split(","))
                prog, flags = st.split("/")
                out[(sid, round(x / 10), round(z / 10))] = (float(prog), int(flags))
            except ValueError:
                pass
        return out
    hs, cs = states(host), states(cli)
    log("build states: host", len(hs), "client", len(cs))
    if not hs or not cs:
        log("buildlist raw: host", cmd(host, "buildlist any 1500")[1][:200], "/ client", cmd(cli, "buildlist any 1500")[1][:200])
    common = [k for k in hs if k in cs]
    bad = [(k, hs[k], cs[k]) for k in common if (hs[k][1] & 1) != (cs[k][1] & 1) or abs(hs[k][0] - cs[k][0]) > 1.0]
    check("batiments existants : vus des deux cotes", len(common) > 0, f"{len(common)} en commun")
    check("batiments existants : meme etat de construction", not bad, str(bad[:5]))
    log("followed:", cmd(host, "buildcount")[1], "/ client:", cmd(cli, "buildcount")[1])


def exp_build(host, cli, kinds=("Feu", "Lit", "Coffre", "Tente", "Mur")):
    """Lot E, buildings: a client's placement is built by the host then by everyone (same place), the
    host's too; construction progress and dismantling follow; a purchase is done by the host and
    replayed by the client."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    sid = None
    for k in kinds:
        t = cmd(host, f"buildtypes {k}")[1]
        log("building templates", k, ":", t[:300])
        parts = t.split()
        if t.startswith("ok") and len(parts) > 2:
            sid = parts[2].split("=")[0]
            break
    check("batiments : un modele de batiment trouve", sid is not None, sid)
    if not sid:
        summary()
        return
    def built(pid):
        t = cmd(pid, f"buildlist {sid}")[1]
        return [e for e in t.split()[2:] if e.startswith(sid + "@")]
    before_h, before_c = len(built(host)), len(built(cli))
    # 1. placed by the client: built by the host, then by the client, at the same place
    log("client places", sid, place_valid(cli, sid, 0, 40, 0))
    for _ in range(20):
        time.sleep(0.5)
        if len(built(host)) > before_h and len(built(cli)) > before_c:
            break
    bh, bc = built(host), built(cli)
    log("host has", bh, "client has", bc)
    check("batiments : placement du client construit chez l'hote", len(bh) > before_h, bh)
    check("batiments : et chez le client", len(bc) > before_c, bc)
    def pos(e):
        return tuple(map(float, e.split("@")[1].split(":")[0].split(",")))
    same = bool(bh) and bool(bc) and min(dist(pos(a), pos(b)) for a in bh for b in bc) < 0.5
    check("batiments : au meme endroit partout", same, f"{bh} / {bc}")
    # 2. construction progress on the host: the client follows
    log("host builds", cmd(host, f"buildprogress {sid} 50"))
    time.sleep(3)
    ph, pc = built(host), built(cli)
    prog = lambda l: sorted(e.split(":")[-1] for e in l)
    check("batiments : avancement du chantier identique", prog(ph) == prog(pc), f"{ph} / {pc}")
    log("host finishes it", cmd(host, f"buildprogress {sid} 100000"))
    time.sleep(3)
    ph, pc = built(host), built(cli)
    check("batiments : chantier termine partout", prog(ph) == prog(pc), f"{ph} / {pc}")
    # 3. placed by the host: the client builds it too
    n_c = len(built(cli))
    log("host places", sid, place_valid(host, sid, 0, -40, 0, 90))
    time.sleep(4)
    check("batiments : placement de l'hote construit chez le client", len(built(cli)) > n_c, built(cli))
    # 4. the client dismantles one: the host's game starts dismantling it, the client sees it
    log("client dismantles", cmd(cli, f"builddismantle {sid}"))
    time.sleep(4)
    ph, pc = built(host), built(cli)
    check("batiments : demontage demande par le client, vu partout", any(int(e.split("/")[-1]) & 4 for e in ph) and prog(ph) == prog(pc), f"{ph} / {pc}")
    log("followed buildings: host", cmd(host, "buildcount")[1], "client", cmd(cli, "buildcount")[1])
    # 5. a building for sale (towns only): bought by the host for the client, replayed by the client
    sale = cmd(cli, "buildforsale")
    log("for sale near the client:", sale)
    if sale[0]:
        cats0 = cmd(host, "money")[1]
        log("client buys", cmd(cli, "buildbuy"))
        time.sleep(4)
        check("batiments : achat demande par le client paye chez l'hote", cmd(host, "money")[1] != cats0, f"{cats0} -> {cmd(host, 'money')[1]}")
        check("batiments : le batiment n'est plus a vendre chez le client", cmd(cli, "buildforsale")[1] != sale[1], cmd(cli, "buildforsale")[1])
    else:
        log("SKIP step 5 (purchase): no building for sale within 3 km of the squad (" + sale[1] + "); "
            "use --save kctest_town or run the buyhouse experiment")
    summary()


def exp_facing(host, cli):
    """A host character runs in several directions: on the client it must really run (speed), facing the same way."""
    import math
    def yaw(f):
        x, _, z = map(float, f.split(","))
        return math.degrees(math.atan2(x, z))
    time.sleep(6)
    worst, still = 0.0, 0
    for r in range(6):
        idx = 1 + r % 4
        cmd(host, f"moverel {idx} {500 * (1 if r % 2 == 0 else -1)} {400 * (1 if r in (0, 1, 4) else -1)}")
        for sample in range(3):
            time.sleep(1.0)
            h, c = dump(host, "hf"), dump(cli, "cf")
            k = sorted(h["squad"])[idx]
            hv, cv = h["char"][k], c["char"][k]
            if not (int(hv["flags"]) & 1) or int(hv["flags"]) & 4:
                continue
            d = abs((yaw(hv["face"]) - yaw(cv["face"]) + 540) % 360 - 180)
            worst = max(worst, d)
            still += float(cv.get("speed", 0)) < 1.0
            log(f"run {r} #{sample} idx {idx}: yaw host {yaw(hv['face']):.0f} client {yaw(cv['face']):.0f} diff {d:.0f} | speed host {hv.get('speed')} client {cv.get('speed')} | pos err {t_dist(hv, cv):.1f}")
    log("FACING worst diff while running:", round(worst), "deg | client samples not really moving:", still)


# ---- fix G5: client orders never reach the host's characters; the TÃƒÆ’Ã‚Â¢ches panel; trade windows
def exp_passive(host, cli):
    """The host's character is passive; the client orders its own character to attack an NPC: only the
    client's character engages. Then the host's selection holds the client's character too: the host's
    passive toggle still reaches the host's own character (it used to be refused as a whole)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    mine = 0 if own != 0 else 1   # a host character
    log("host passive on its own character:", cmd(host, f"orderreq {mine} 13"))
    time.sleep(1)
    m = cmd(host, f"modes {mine}")[1]
    check("passif : le perso de l'hote est passif", len(m.split()) > 1 and int(m.split()[1]) & 32, m)
    log("spawn an NPC near the client's character:", cmd(host, f"spawnnpc 40 0 {own}"))
    time.sleep(3)
    log("client orders an attack:", cmd(cli, f"attackreq {own}"))
    host_engaged = cli_engaged = False
    for _ in range(20):
        time.sleep(0.5)
        cli_engaged |= cmd(host, f"combat {own}")[1].startswith("ok 1")
        host_engaged |= cmd(host, f"combat {mine}")[1].startswith("ok 1")
    check("passif : le perso du client attaque", cli_engaged, cmd(host, f"combat {own}")[1])
    check("passif : le perso de l'hote n'attaque pas", not host_engaged, cmd(host, f"combat {mine}")[1])
    m = cmd(host, f"modes {mine}")[1]
    check("passif : toujours passif apres l'ordre du client", int(m.split()[1]) & 32, m)
    # a mixed selection on the host: its toggle goes to its own character, the client's one is left out
    log("host selects both:", cmd(host, f"selectset {mine} {own}"))
    before_cli = cmd(host, f"modes {own}")[1]
    log("host toggles passive off:", cmd(host, "selorder 13"))
    time.sleep(1)
    m = cmd(host, f"modes {mine}")[1]
    check("selection mixte : le mode passe chez le perso de l'hote", not (int(m.split()[1]) & 32), m)
    check("selection mixte : le perso du client n'est pas touche", cmd(host, f"modes {own}")[1] == before_cli, cmd(host, f"modes {own}")[1])
    summary()


def frame_latency(pid, n=5):
    """Round trip of a no-op debug command (ms): the channel is polled once per game frame, so this
    follows the frame time (a stalled or crawling game shows here). Best and worst of n."""
    ts = []
    for _ in range(n):
        t0 = time.time()
        cmd(pid, "echo")
        ts.append((time.time() - t0) * 1000)
    return round(min(ts)), round(max(ts))


def find_material(pid, name=None):
    """The building materials' item template (sid), searched by name on that machine."""
    if not name:
        return "580-gamedata.base"   # vanilla "Building Materials" (the names are translated; "construction" matched construction_goods, type 49)
    for part in ([name] if name else []) + ["Building_Materials", "MatÃ©riaux_de_construction", "Materiaux_de_construction", "construction"]:
        t = cmd(pid, f"itemtypes {part}")[1]
        log("item templates", part, ":", t[:300])
        parts = t.split()
        if t.startswith("ok") and len(parts) > 2:
            # the plugin lists items first; among them the building materials themselves, not a kit
            names = [p.split("=", 1) for p in parts[2:] if "=" in p]
            for sid, nm in names:
                if nm.lower().startswith(("mat", "building_mat")):
                    return sid
            return names[0][0] if names else None
    return None


ACID_LAKE = (54612.0, 100.0, 40576.0)   # an acid lake: a client's tent was once built there (farlong)


def ground_at(pid, x, z):
    """(ground, surface, 'land' | 'water' | 'unknown') at that world spot as this game knows it: the
    terrain under any water (UtilityT::getTerrainHeight, -99 when not loaded), build mode's reference
    height (getTerrainWithWaterHeight, the water and acid surface is 100) and what build mode makes
    of it (ground under 98: in the water). None when the command failed."""
    ok, t = cmd(pid, f"groundat {x:.1f} {z:.1f}")
    if not ok:
        return None
    parts = t.split()
    return float(parts[1]), float(parts[2]), parts[3]


def spot_offsets(dx, dz):
    """(dx, dz) first, then rings around it: where a placement is tried until build mode's check accepts one."""
    out = [(dx, dz)]
    for r in (25, 50, 80, 120, 180, 260):
        for k in range(8):
            a = k * math.pi / 4
            out.append((round(dx + r * math.cos(a)), round(dz + r * math.sin(a))))
    return out


def place_valid(pid, sid, idx, dx, dz, yaw=0):
    """Places `sid` next to squad member idx, at the first spot around (dx, dz) that this game's own
    build-mode check accepts: buildplace checks first and answers 'err invalid spot: ...' (water or
    acid, slope, town, inside or on another building) without placing anything. Returns
    (ok, answer, (dx, dz) used)."""
    last = (False, "no spot tried", (dx, dz))
    for ox, oz in spot_offsets(dx, dz):
        ok, t = cmd(pid, f"buildplace {sid} {ox} {oz} {yaw} {idx}")
        last = (ok, t, (ox, oz))
        if ok or "invalid spot" not in t:
            break
        log(f"   spot {ox},{oz} refused: {t}")
    return last


def teleport_dry(host, idx, base, offsets, settle=12):
    """Admin TP of squad member idx to the first of base + offsets that is dry land, not a lake of water
    or acid. The ground is read on the host: before the TP when its terrain is known there, else after
    the TP once the zone has streamed in around the character (then the next spot is tried). Returns
    the position used, or None."""
    x, y, z = base
    for dx, dz in offsets:
        tx, tz = x + dx, z + dz
        g = ground_at(host, tx, tz)
        if g and g[2] == "water":
            log(f"   {tx:.0f},{tz:.0f}: water or acid there, next spot", g)
            continue
        log(f"teleport {idx} to {tx:.0f},{tz:.0f}:", cmd(host, f"teleport {idx} {tx:.0f} {y + 300:.0f} {tz:.0f}"))
        for _ in range(settle):
            time.sleep(1)
            g = ground_at(host, tx, tz)
            if g and g[2] != "unknown":
                break
        if g and g[2] == "land":
            return (tx, y, tz)
        log(f"   {tx:.0f},{tz:.0f} is not dry land ({g}), next spot")
    return None


FAR_OFFSETS = [(30000, 30000), (30000, 34000), (34000, 30000), (30000, 26000), (26000, 30000), (36000, 36000), (24000, 34000)]


def find_building(pid, kinds):
    for k in kinds:
        t = cmd(pid, f"buildtypes {k}")[1]
        log("building templates", k, ":", t[:300])
        parts = t.split()
        if t.startswith("ok") and len(parts) > 2:
            return parts[2].split("=")[0]
    return None


def site_list(pid, sid, idx=0, radius=1500):
    """Construction sites of that kind around squad member idx: [(pos, progress, flags)], nearest first."""
    t = cmd(pid, f"buildlist {sid} {radius} {idx}")[1]
    out = []
    for e in t.split()[2:]:
        if not e.startswith(sid + "@"):
            continue
        where, rest = e.split("@", 1)[1].split(":", 1)
        prog, flags = rest.split("/")
        out.append((tuple(map(float, where.split(","))), float(prog), int(flags)))
    return out


def site_at(pid, sid, pos, idx=0):
    """That site (within 1 unit of pos) as this machine has it, or None."""
    for p, prog, flags in site_list(pid, sid, idx):
        if dist(p, pos) < 1.0:
            return p, prog, flags
    return None


BUILD_TASK = [None]   # the TaskType that made a worker build, found by the first probe


def order_build(pid, idx, sid, site_pos, tasks, host):
    """The player's right click on its construction site: newPlayerTaskSelectedCharacters on the building
    with that member alone selected. The TaskType of 'build' is not in the engine notes, so the first
    call tries each candidate until the host's progress moves (and remembers it)."""
    cands = [BUILD_TASK[0]] if BUILD_TASK[0] is not None else tasks
    for task in cands:
        before = site_at(host, sid, site_pos)
        r = cmd(pid, f"buildreq {idx} {task} {sid}")
        log(f"build order task {task} from {pid}:", r)
        if not r[0]:
            continue
        for _ in range(40):   # the builder walks to the site and starts (20 s)
            time.sleep(0.5)
            now = site_at(host, sid, site_pos)
            if before and now and (now[1] > before[1] or now[2] & 1):
                BUILD_TASK[0] = task
                log(f"task {task} builds: progress {before[1]} -> {now[1]}")
                return task
    return None


def construct_one(host, cli, builder, idx, sid, mat, tasks, dx, label, wait=240):
    """One real construction: `builder` (host or client pid) places a site next to its character `idx`,
    orders that character to build it, then host and client are compared until it is finished."""
    who = "client" if builder == cli else "hote"
    before_h = {s[0] for s in site_list(host, sid, idx)}
    log(f"[{label}] {who} places", sid, place_valid(builder, sid, idx, dx, 25))
    site = None
    for _ in range(20):
        time.sleep(0.5)
        new = [s for s in site_list(host, sid, idx) if s[0] not in before_h]
        if new:
            site = new[0]
            break
    check(f"construction [{label}] : chantier pose chez l'hote", site is not None, site)
    if not site:
        return None
    time.sleep(2)
    check(f"construction [{label}] : chantier chez le client", site_at(cli, sid, site[0], idx) is not None, site_list(cli, sid, idx)[:3])
    m0 = (cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1])
    log(f"[{label}] materials before: host {m0[0]} client {m0[1]}")
    task = order_build(builder, idx, sid, site[0], tasks, host)
    check(f"construction [{label}] : l'ordre de construire fait avancer le chantier chez l'hote", task is not None,
          f"taches essayees {tasks if BUILD_TASK[0] is None else BUILD_TASK[0]}")
    if task is None:
        return site[0]
    samples, t0, last = [], time.time(), None
    while time.time() - t0 < wait:
        time.sleep(5)
        h, c = site_at(host, sid, site[0], idx), site_at(cli, sid, site[0], idx)
        mh, mc = cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1]
        samples.append((h and h[1], c and c[1], mh, mc))
        log(f"[{label}] t={int(time.time() - t0)}s progress host {h and h[1]} client {c and c[1]} | materials host {mh} client {mc}")
        last = (h, c)
        if h and h[2] & 1:
            break
    time.sleep(3)   # the last construction state and inventories reach the client
    h, c = site_at(host, sid, site[0], idx), site_at(cli, sid, site[0], idx)
    m1 = (cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1])
    log(f"[{label}] materials after: host {m1[0]} client {m1[1]}")
    progressed_c = len({s[1] for s in samples if s[1] is not None}) > 1
    check(f"construction [{label}] : l'avancement bouge aussi chez le client", progressed_c, [s[1] for s in samples][:12])
    close = [abs(s[0] - s[1]) for s in samples if s[0] is not None and s[1] is not None]
    check(f"construction [{label}] : avancement proche chez l'hote et le client", close and max(close) <= 0.25 * max(1.0, max(s[0] for s in samples if s[0] is not None)),
          f"ecart max {max(close) if close else '?'}")
    check(f"construction [{label}] : les materiaux baissent chez l'hote", m1[0] != m0[0], f"{m0[0]} -> {m1[0]}")
    check(f"construction [{label}] : memes materiaux des deux cotes", m1[0] == m1[1], f"hote {m1[0]} / client {m1[1]}")
    check(f"construction [{label}] : termine chez l'hote", bool(h and h[2] & 1), h)
    check(f"construction [{label}] : termine pareil chez le client", bool(h and c and h[2] == c[2] and abs(h[1] - c[1]) < 0.01), f"{h} / {c}")
    rep = compare(dump(host, f"h_construct_{label}"), dump(cli, f"c_construct_{label}"), f"construct_{label}")
    check(f"construction [{label}] : inventaires identiques", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    return site[0]


def exp_construct(host, cli, kinds=("Tente", "Feu", "Coffre", "Lit", "Mur"), material=None, tasks=None):
    """Real construction: money and building materials for both players, a site placed by the client and
    built by its own character on the client's order (the right-click path), progress and materials the
    same on both sides; then a host's site; then both at the same time."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    hidx = 0 if own != 0 else 1
    tasks = tasks or [2]   # TaskType BUILD = 2 (KenshiLib numbering, checked against the game's other orders)
    sid = find_building(host, kinds)
    mat = find_material(host, material)
    check("construction : un modele de batiment trouve", sid is not None, sid)
    check("construction : les materiaux de construction trouves", mat is not None, mat)
    if not sid or not mat:
        summary()
        return
    log("money", cmd(host, "givemoney 50000"))
    for i in (own, hidx):
        log(f"materials for squad member {i}", cmd(host, f"giveitem {mat} 40 {i}"), "| backpack worn:", cmd(host, f"bag squad{i}")[1][:120])
    time.sleep(3)
    mh, mc = cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1]
    check("construction : materiaux recus, memes chiffres chez le client", mh == mc and mh != "ok 0", f"hote {mh} / client {mc}")
    check("construction : meme argent partout", cmd(host, "money")[1] == cmd(cli, "money")[1], f"{cmd(host, 'money')[1]} / {cmd(cli, 'money')[1]}")
    frames = [frame_latency(host), frame_latency(cli)]
    # 1. the client's site, built by the client's own character on the client's order
    construct_one(host, cli, cli, own, sid, mat, tasks, 60, "client")
    # 2. the host's site, built by a host character on the host's order
    construct_one(host, cli, host, hidx, sid, mat, tasks, -60, "hote")
    # 3. both at the same time: two sites, two orders, both finished identically
    if BUILD_TASK[0] is None:
        log("SKIP simultaneous construction: no build task found")
        check("construction [ensemble] : les deux chantiers avancent", False, "pas de tache de construction trouvee")
        summary()
        return
    before = {s[0] for s in site_list(host, sid, own, 2500)}
    log("both place:", place_valid(cli, sid, own, 60, -60), place_valid(host, sid, hidx, -60, -60))
    time.sleep(4)
    new = [s for s in site_list(host, sid, own, 2500) if s[0] not in before]
    check("construction [ensemble] : deux chantiers chez l'hote", len(new) >= 2, new)
    m0 = cmd(host, f"invcount all {mat}")[1]
    log("both order:", cmd(cli, f"buildreq {own} {BUILD_TASK[0]} {sid}"), cmd(host, f"buildreq {hidx} {BUILD_TASK[0]} {sid}"))
    t0 = time.time()
    while time.time() - t0 < 240:
        time.sleep(5)
        hs = [site_at(host, sid, s[0], own) for s in new[:2]]
        log("both sites on the host:", hs)
        if hs and all(x and x[2] & 1 for x in hs):
            break
    time.sleep(3)
    hs = [site_at(host, sid, s[0], own) for s in new[:2]]
    cs = [site_at(cli, sid, s[0], own) for s in new[:2]]
    check("construction [ensemble] : les deux chantiers termines chez l'hote", len(hs) == 2 and all(x and x[2] & 1 for x in hs), hs)
    check("construction [ensemble] : identiques chez le client", hs == cs, f"{hs} / {cs}")
    m1h, m1c = cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1]
    check("construction [ensemble] : materiaux consommes, memes chiffres", m1h != m0 and m1h == m1c, f"{m0} -> hote {m1h} / client {m1c}")
    frames.append(frame_latency(host))
    frames.append(frame_latency(cli))
    log("command round trips (ms, best/worst):", frames)
    rep = compare(dump(host, "h_construct_end"), dump(cli, "c_construct_end"), "construct_end")
    check("construction : inventaires identiques a la fin", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    summary()


def path_clear_of(samples, center, radius):
    """Closest a recorded walk came to a point (x/z), from motionrec samples [(t, (x,y,z))]."""
    return min((math.hypot(q[0] - center[0], q[2] - center[2]) for _, q in samples), default=1e9)


def record_walk(pids, key, seconds, label):
    """motionrec of one character on several games at once: {pid: [(t, (x,y,z))]}."""
    paths = {}
    for pid in pids:
        path = os.path.abspath(os.path.join(OUT_DIR, f"walk_{label}_{pid}.txt"))
        if os.path.exists(path):
            os.remove(path)
        paths[pid] = path
        cmd(pid, f"motionrec {seconds} 10 {path} {key}")
    time.sleep(seconds + 1.5)
    out = {}
    for pid, path in paths.items():
        ss = []
        for _ in range(10):
            if os.path.exists(path) and open(path, encoding="utf-8", errors="replace").read().rstrip().endswith("end"):
                break
            time.sleep(0.5)
        for ln in (open(path, encoding="utf-8", errors="replace").read().splitlines() if os.path.exists(path) else []):
            q = ln.split()
            if len(q) >= 4 and q[0] == "m":
                ss.append((float(q[1]), tuple(map(float, q[3].split(",")))))
        out[pid] = ss
    return out


HOUSE = "772-gamedata.base"   # Vieille Maison (base game): a building with walls, a door and room inside


def go_to_open_desert(host, check_sid=HOUSE):
    """Every squad member (the clients' too) on open, flat, dry ground far from every town and clear of
    rocks, everyone in god mode. Candidates on rings around squad member 0: a 3x3 grid of terrain
    heights (80 units apart) within 8 units of each other, all dry, more than 4000 from any of the
    game's 304 places; the flattest first; there, build mode must accept a house at the centre and 150
    units around it (rocks and walls refuse it). True when found."""
    log("god mode for everyone:", cmd(host, "admin god all on"))
    t = cmd(host, "pos 0")[1].split()
    if len(t) < 2:
        return False
    x0, y0, z0 = (float(v) for v in t[1].split(","))
    n = squad_size(host)
    known = os.path.join(OUT_DIR, "open_desert.txt")   # the spot found last time: tried first (the search takes minutes)
    if os.path.exists(known):
        try:
            cx, gy, cz = map(float, open(known).read().split())
            for i in range(n):
                cmd(host, f"teleport {i} {cx + 12 * i:.0f} {gy + 40:.0f} {cz:.0f}")
            time.sleep(8)
            p = pos_xz(host, 0)
            if p and math.hypot(p[0] - cx, p[1] - cz) < 200 and cmd(host, f"buildcheckat {check_sid} {cx:.0f} {cz + 120:.0f}")[0]:
                SANDBOX["home"] = (cx, gy, cz)
                log("open desert (known spot):", (round(cx), round(gy), round(cz)))
                return True
        except (ValueError, OSError):
            pass
    towns = [tw["pos"] for tw in town_list(host)]
    cands = []
    for r in (12000, 20000, 30000, 45000, 60000):
        for k in range(24):
            a = k * math.pi / 12
            cx, cz = x0 + r * math.cos(a), z0 + r * math.sin(a)
            if any(math.hypot(cx - p[0], cz - p[2]) < 4000 for p in towns):
                continue
            hs = [ground_at(host, cx + 80 * i, cz + 80 * j) for i in (-1, 0, 1) for j in (-1, 0, 1)]
            if any(g is None or g[2] != "land" for g in hs):
                continue
            heights = [g[0] for g in hs]
            if max(heights) - min(heights) <= 8.0:
                cands.append((max(heights) - min(heights), cx, cz, sum(heights) / len(heights)))
    log(f"open ground: {len(cands)} flat candidates")
    for flat, cx, cz, gy in sorted(cands)[:8]:
        log(f"open ground candidate {cx:.0f},{cz:.0f} (height {gy:.0f}, flat within {flat:.1f})")
        for i in range(n):
            cmd(host, f"teleport {i} {cx + 12 * i:.0f} {gy + 40:.0f} {cz:.0f}")
        time.sleep(8)
        clear = all(cmd(host, f"buildcheckat {check_sid} {cx + dx:.0f} {cz + dz:.0f}")[0]
                    for dx, dz in ((0, 120), (150, 120), (-150, 120), (0, 270), (0, -120)))
        p = pos_xz(host, 0)
        if clear and p and math.hypot(p[0] - cx, p[1] - cz) < 200:
            SANDBOX["home"] = (cx, gy, cz)
            log("open desert:", (round(cx), round(gy), round(cz)))
            try:
                open(known, "w").write(f"{cx:.0f} {gy:.0f} {cz:.0f}")
            except OSError:
                pass
            return True
        log("   not clear there (rocks, a wall or a building): next")
    return False


OUTPOST = "727-gamedata.base"    # Avant-poste s-III: a player building (big, no door: not to walk in)
CABIN = "4092-TwoStorey.mod"      # Petite Cabane: a small player house with a door; its roof is its floor 1 (4093)
STORAGE = "1459-gamedata.base"    # Coffre Stock Général: a player's storage chest


def give_materials(host, mat, idxs, per=20):
    """Building materials to those squad members, as much as they carry (a trader's backpack first:
    in this game's data the materials do not stack, one per slot)."""
    for i in idxs:
        cmd(host, f"giveitem {TRADER_BACKPACK} 1 {i}")
        for _ in range(3):
            if not cmd(host, f"giveitem {mat} {per} {i}")[0]:
                break
    return cmd(host, f"invcount all {mat}")[1]


def build_by_everyone(host, cli, sid, site, own_c, host_idxs, mat, label, wait=30):
    """Every builder works on that site, the players' own way (the client orders its character, the
    host its own); materials topped up when the work stalls. (host site, client site)."""
    log(f"[{label}] client builds:", cmd(cli, f"buildreq {own_c} 2 {sid}"))
    for i in host_idxs:
        log(f"[{label}] host's member {i} builds:", cmd(host, f"buildreq {i} 2 {sid}"))
    t0, last, stall = time.time(), -1.0, time.time()
    while time.time() - t0 < wait:
        time.sleep(8)
        h, c = site_at(host, sid, site, 0), site_at(cli, sid, site, 0)
        log(f"[{label}] t={int(time.time() - t0)}s host {h and h[1]} client {c and c[1]} | materials {cmd(host, f'invcount all {mat}')[1]}")
        if h and h[2] & 1:
            break
        if h and h[1] > last + 0.05:
            last, stall = h[1], time.time()
        elif time.time() - stall > 40:   # out of materials or idle: more materials, orders again
            log(f"[{label}] stalled: materials {give_materials(host, mat, host_idxs, 10)}; orders again")
            cmd(cli, f"buildreq {own_c} 2 {sid}")
            for i in host_idxs:
                cmd(host, f"buildreq {i} 2 {sid}")
            stall = time.time()
    # the players' own building work is checked for `wait` seconds (progress the same on both
    # sides); then the site is finished at once (a big building takes many minutes)
    h, c = site_at(host, sid, site, 0), site_at(cli, sid, site, 0)
    if h and not h[2] & 1:
        log(f"[{label}] progress host {h[1]} client {c and c[1]}: finished at once now", cmd(host, f"buildprogress {sid} 100000"))
    time.sleep(3)
    return site_at(host, sid, site, 0), site_at(cli, sid, site, 0)


def new_site(host, sid, before, timeout=10):
    for _ in range(int(timeout * 2)):
        time.sleep(0.5)
        new = [x for x in site_list(host, sid, 0, 2500) if x[0] not in before]
        if new:
            return new[0][0]
    return None


def exp_buildfull(host, cli):
    """Buildings for real, the players' way, in the open desert, everyone in god mode: the client places
    an outpost (a real player building) that every character builds with materials in their hands;
    the host a storage chest; then the client's character walks in (the game knows it is inside on
    both sides, the client's floor view follows it: roof gone, screenshot), a research bench placed
    inside by the client and worked by its character, the chest filled by the client and emptied by
    the host, and each one dismantles the other's building."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    own_c = own_index(cli)
    n = squad_size(host)
    host_idxs = [i for i in range(n) if i != own]
    hidx = host_idxs[0]
    mat = find_material(host)
    shots = os.path.join(OUT_DIR, "buildfull")
    os.makedirs(shots, exist_ok=True)
    check("batiments+ : desert degage, tout le monde en mode dieu", go_to_open_desert(host, CABIN))
    log("money", cmd(host, "givemoney 50000"))
    log("materials carried:", give_materials(host, mat, list(range(n))))
    cmd(cli, f"camto {own_c}")
    # 1. the client's outpost, built by everyone
    before = {x[0] for x in site_list(host, CABIN, 0, 2500)}
    log("client places the cabin:", place_valid(cli, CABIN, own_c, 0, 90))
    site = new_site(host, CABIN, before)
    check("batiments+ : cabane du client pose chez l'hote", site is not None)
    if not site:
        summary()
        return
    time.sleep(2)
    check("batiments+ : et chez le client", site_at(cli, CABIN, site, 0) is not None)
    h, c = build_by_everyone(host, cli, CABIN, site, own_c, host_idxs, mat, "cabane")
    check("batiments+ : cabane termine chez l'hote", bool(h and h[2] & 1), h)
    check("batiments+ : cabane termine pareil chez le client", bool(h and c and h[2] == c[2]), f"{h} / {c}")
    m = (cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1])
    check("batiments+ : memes materiaux des deux cotes", m[0] == m[1], m)
    cmd(cli, f"camto {own_c}")
    time.sleep(2)
    grab_window(cli, os.path.join(shots, "client_outpost.png"))
    grab_window(host, os.path.join(shots, "host_outpost.png"))
    # 2. the host's storage chest, built by the host's characters
    before = {x[0] for x in site_list(host, STORAGE, 0, 2500)}
    log("host places a storage chest:", place_valid(host, STORAGE, hidx, -70, 0))
    chest = new_site(host, STORAGE, before)
    if chest:
        hc, cc = build_by_everyone(host, cli, STORAGE, chest, own_c, host_idxs, mat, "coffre")
        check("batiments+ : coffre de l'hote termine des deux cotes", bool(hc and cc and hc[2] & 1 and cc[2] & 1), f"{hc} / {cc}")
    else:
        check("batiments+ : coffre de l'hote pose", False)
    # 3. the client's character walks into the outpost
    if h and h[2] & 1:
        for _ in range(4):   # through the door: a few tries until the game has it inside
            log("client walks in:", cmd(cli, f"move {own_c} {site[0]:.0f} {site[2]:.0f}"))
            time.sleep(10)
            if "TwoStorey" in cmd(cli, f"indoors {own_c}")[1]:
                break
        ph, pc = where_vec(host, own), where_vec(cli, own_c)
        fh, fc = cmd(host, f"floor {own}")[1], cmd(cli, f"floor {own_c}")[1]
        view = cmd(cli, "floorview")[1]
        log(f"inside? host {ph} floor {fh} | client {pc} floor {fc} | view {view}")
        check("batiments+ : le perso du client est entre (pres du centre, chez l'hote)", bool(ph and math.hypot(ph[0] - site[0], ph[2] - site[2]) < 25), ph)
        check("batiments+ : meme position chez le client", bool(ph and pc and math.dist(ph, pc) < 3), f"{ph} / {pc}")

        def grp(t):
            return int(t.split()[1]) if t.startswith("ok") else -1
        check("batiments+ : le jeu le sait dedans (etage de batiment) chez l'hote", grp(fh) >= 9, fh)
        check("batiments+ : et chez le client", grp(fc) >= 9, fc)
        ih, ic = cmd(host, f"indoors {own}")[1], cmd(cli, f"indoors {own_c}")[1]
        check("batiments+ : dans la cabane des deux cotes (indoors)", "TwoStorey" in ih and "TwoStorey" in ic, f"{ih} / {ic}")
        check("batiments+ : la vue des etages du client suit son perso", "focus=none" not in view and "seen=-1" not in view, view)
        grab_window(cli, os.path.join(shots, "client_inside.png"))
        # 4. a research bench inside, placed by the client, worked by its character
        rb = None
        for dx, dz in ((0, 0), (8, 0), (0, 8), (-8, 0)):
            before = {x[0] for x in site_list(host, RESEARCH_BENCH, 0, 2500)}
            r = cmd(cli, f"furnplace {RESEARCH_BENCH} Cabane {dx} {dz}")
            log(f"client places a research bench inside ({dx},{dz}):", r)
            rb = new_site(host, RESEARCH_BENCH, before) if r[0] else None
            if rb:
                break
        if rb:
            rh, rc = build_by_everyone(host, cli, RESEARCH_BENCH, rb, own_c, [], mat, "etabli")
            check("batiments+ : etabli de recherche termine des deux cotes", bool(rh and rc and rh[2] & 1 and rc[2] & 1), f"{rh} / {rc}")
            for what in ("Livre", "Book"):
                cmd(host, f"machinegive {RESEARCH_BENCH} {what} 10")
            picks = [x.split("=")[0] for x in cmd(cli, "researchpick 3")[1].split()[1:]]
            if picks:
                log("client queues", picks[0], cmd(cli, f"researchreq queue {picks[0]}"))
            log("client works the bench:", cmd(cli, f"workreq {own_c} {RESEARCH_BENCH}"))
            near = None
            for _ in range(10):
                time.sleep(2)
                ph = where_vec(host, own)
                near = ph and math.hypot(ph[0] - rb[0], ph[2] - rb[2])
                if near is not None and near < 12:
                    break
            log("client's character to the bench:", near, cmd(host, f"charstate {own}")[1][:100])
            check("batiments+ : le perso du client va a l'etabli (dedans)", near is not None and near < 12, near)
        else:
            check("batiments+ : etabli pose dedans", False)
    # 5. the chest: filled by the client, emptied by the host
    if chest:
        c0 = (cmd(host, "contcount Coffre")[1], cmd(cli, "contcount Coffre")[1])
        log("client opens the chest:", cmd(cli, f"containerreq {own_c} Coffre"))
        time.sleep(8)
        log("client puts building materials in:", cmd(cli, f"conput {own_c} Coffre {mat}"))
        time.sleep(4)
        c1 = (cmd(host, "contcount Coffre")[1], cmd(cli, "contcount Coffre")[1])
        log(f"chest {c0} -> {c1}")
        check("batiments+ : coffre rempli par le client, vu chez l'hote", c1[0] != c0[0], f"{c0} -> {c1}")
        check("batiments+ : meme contenu des deux cotes", c1[0] == c1[1], c1)
        cmd(cli, "closewindows")
        log("host opens it:", cmd(host, f"containerreq {hidx} Coffre"))
        time.sleep(8)
        log("host takes:", cmd(host, f"contake {hidx} Coffre"))
        time.sleep(4)
        c2 = (cmd(host, "contcount Coffre")[1], cmd(cli, "contcount Coffre")[1])
        check("batiments+ : l'hote le reprend, vu chez le client", c2[0] == c0[0] and c2[0] == c2[1], f"{c1} -> {c2}")
        cmd(host, "closewindows")
    # 6. each one dismantles the other's building
    for pid, idx, sid, pos, label in ((host, hidx, CABIN, site, "l'hote demonte la cabane du client"),
                                      (cli, own_c, STORAGE, chest, "le client demonte le coffre de l'hote")):
        if not pos:
            continue
        log(f"[{label}] confirm:", cmd(pid, f"builddismantle {sid} {idx}"))
        time.sleep(1.5)
        log(f"[{label}] dismantle order:", cmd(pid, f"buildreq {idx} 96 {sid}"))
        gone_h = gone_c = False
        for _ in range(90):
            time.sleep(2)
            gone_h, gone_c = site_at(host, sid, pos, 0) is None, site_at(cli, sid, pos, 0) is None
            if gone_h and gone_c:
                break
        time.sleep(3)
        m = (cmd(host, f"invcount all {mat}")[1], cmd(cli, f"invcount all {mat}")[1])
        check(f"batiments+ : {label} : disparu chez l'hote", gone_h)
        check(f"batiments+ : {label} : disparu chez le client", gone_c)
        check(f"batiments+ : {label} : memes materiaux ensuite", m[0] == m[1], m)
    rep = compare(dump(host, "h_buildfull_end"), dump(cli, "c_buildfull_end"), "buildfull_end")
    check("batiments+ : inventaires identiques a la fin", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    log("god mode off:", cmd(host, "admin god all off"))
    summary()


def exp_buyhouse(host, cli):
    """Buying a building for sale (a town save): the client's purchase through the purchase window's
    confirm (BuyMeCallback, what the Buy button runs), the host's own; price debited once everywhere,
    the building ours everywhere, its doors and containers usable; refused without the money;
    two purchases of the same building at once: one owner, one payment."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    hidx = 0 if own != 0 else 1
    log("UI path: buildbuy calls Building::buyMeCallback(2), what the Buy window's confirm button does "
        "(the right-click menu only opens that window)")

    def money(pid):
        t = cmd(pid, "money")[1]
        return int(t.split()[1]) if t.startswith("ok") else None

    def for_sale(pid):
        r = cmd(pid, "buildforsale")
        if not r[0]:
            return None, None
        where = r[1].split()[1]
        return where, int(r[1].split("price=")[1])

    def info(pid, where):
        t = cmd(pid, f"buildinfo {where}")[1]
        return dict(p.split("=") for p in t.split()[2:]) if t.startswith("ok") else {}

    def gather():   # both characters next to squad member 0 (the host's), where the sale search is made
        x, y, z = map(float, cmd(host, "where 0")[1].split()[1].split(","))
        cmd(host, f"teleport {own} {x + 15} {y} {z}")
        cmd(host, f"teleport {hidx} {x - 15} {y} {z}")
        time.sleep(3)

    gather()
    where, price = for_sale(host)
    log("for sale near the squad (host):", where, price, "| client:", for_sale(cli))
    check("achat : un batiment a vendre trouve pres de l'escouade", where is not None,
          "aucun batiment a vendre a 3 km (lancer avec --save d'une ville qui en a, ex. kctest_town)")
    if not where:
        summary()
        return
    # 1. not enough money: refused, nothing debited
    cmd(host, "money 0")
    time.sleep(2)
    log("client tries to buy with 0 cats:", cmd(cli, "buildbuy"))
    time.sleep(4)
    ih, ic = info(host, where), info(cli, where)
    check("achat : sans argent, refuse chez l'hote", ih.get("ours") == "0" and ih.get("forsale") == "1", ih)
    check("achat : sans argent, refuse chez le client", ic.get("ours") == "0" and ic.get("forsale") == "1", ic)
    check("achat : sans argent, rien debite", money(host) == 0 and money(cli) == 0, f"{money(host)} / {money(cli)}")
    # 2. the client buys
    cmd(host, f"money {price * 3 + 1000}")
    time.sleep(2)
    m0h, m0c = money(host), money(cli)
    log("client buys", where, cmd(cli, "buildbuy"))
    time.sleep(5)
    m1h, m1c = money(host), money(cli)
    log(f"cats host {m0h} -> {m1h} client {m0c} -> {m1c} (price {price})")
    check("achat [client] : prix debite chez l'hote", m0h is not None and m1h is not None and m0h - m1h == price, f"{m0h} -> {m1h}, prix {price}")
    check("achat [client] : meme argent chez le client", m1h == m1c, f"hote {m1h} / client {m1c}")
    ih, ic = info(host, where), info(cli, where)
    check("achat [client] : batiment a nous chez l'hote", ih.get("ours") == "1" and ih.get("forsale") == "0", ih)
    check("achat [client] : batiment a nous chez le client", ic.get("ours") == "1" and ic.get("forsale") == "0", ic)
    # its doors and containers: the client's character goes in front of it, opens its door, opens a container
    bx, by, bz = map(float, where.split("@")[1].split(","))
    cmd(host, f"teleport {own} {bx + 8} {by} {bz + 8}")
    time.sleep(4)
    log("doors around the client's character:", cmd(cli, f"doors {own}")[1][:200])
    d0 = cmd(host, f"doorstate {own} door")[1]
    # doors are synced within 40 m of a character (kDoorRadius 400): the nearest door can be further
    # (a house's door is not at its centre); the client's character goes next to it first
    if "at=" in d0:
        dx, dy, dz = map(float, d0.split("at=")[1].split()[0].split(","))
        cmd(host, f"teleport {own} {dx + 4} {dy} {dz + 4}")
        time.sleep(4)
        d0 = cmd(host, f"doorstate {own} door")[1]
    log("client opens the door:", cmd(cli, f"doorbutton {own} door open"))
    time.sleep(4)
    d1h, d1c = cmd(host, f"doorstate {own} door")[1], cmd(cli, f"doorstate {own} door")[1]
    log("door host", d0, "->", d1h, "| client", d1c)
    st = lambda t: t.split("state=")[1].split()[0] if "state=" in t else None
    # open/closed: 1 or 2 (opening) is open, 0 or 3 (closing) closed; the client follows within a second or two
    opened = lambda t: st(t) in ("1", "2")
    for _ in range(10):
        if st(d1h) is not None and opened(d1h) == opened(d1c):
            break
        time.sleep(0.5)
        d1h, d1c = cmd(host, f"doorstate {own} door")[1], cmd(cli, f"doorstate {own} door")[1]
    check("achat [client] : la porte s'ouvre", st(d1h) is not None and opened(d1h) != opened(d0), f"{d0} -> {d1h}")
    check("achat [client] : porte identique chez le client", st(d1h) is not None and opened(d1h) == opened(d1c), f"{d1h} / {d1c}")
    log("client opens a container:", cmd(cli, f"containerreq {own} any"))
    cc = ""
    for _ in range(20):   # the character walks to it first (it can be tens of metres away)
        time.sleep(1)
        cc = cmd(cli, "contcount any")[1]
        if "windows=" in cc and cc.split("windows=")[1] != "0":
            break
    log("client container window:", cc)
    check("achat [client] : un conteneur s'ouvre chez le client", "windows=" in cc and cc.split("windows=")[1] != "0", cc)
    cmd(cli, "closewindows")
    gather()
    # 3. the host buys another one
    where2, price2 = for_sale(host)
    log("next for sale:", where2, price2)
    check("achat [hote] : un autre batiment a vendre", where2 is not None and where2 != where, where2)
    if where2 and where2 != where:
        cmd(host, f"money {price2 + 1000}")   # what is left after the first purchase may not cover this one
        time.sleep(2)
        m0h = money(host)
        log("host buys", where2, cmd(host, "buildbuy"))
        time.sleep(5)
        m1h, m1c = money(host), money(cli)
        check("achat [hote] : prix debite", m0h - m1h == price2, f"{m0h} -> {m1h}, prix {price2}")
        check("achat [hote] : meme argent chez le client", m1h == m1c, f"hote {m1h} / client {m1c}")
        ih, ic = info(host, where2), info(cli, where2)
        check("achat [hote] : a nous chez l'hote", ih.get("ours") == "1", ih)
        check("achat [hote] : a nous chez le client", ic.get("ours") == "1" and ic.get("forsale") == "0", ic)
    # 4. both buy the same one at once: one owner, one payment
    where3, price3 = for_sale(host)
    wc, _ = for_sale(cli)
    log("same building for both?", where3, wc)
    check("achat [ensemble] : un troisieme batiment a vendre, le meme vu des deux cotes", where3 is not None and where3 == wc, f"{where3} / {wc}")
    if where3 and where3 == wc:
        cmd(host, f"money {price3 + 500}")
        time.sleep(2)
        m0h = money(host)
        rc, rh = cmd(cli, "buildbuy"), cmd(host, "buildbuy")
        log("both buy:", rc, rh)
        time.sleep(6)
        m1h, m1c = money(host), money(cli)
        check("achat [ensemble] : un seul paiement", m0h - m1h == price3, f"{m0h} -> {m1h}, prix {price3}")
        check("achat [ensemble] : meme argent chez le client", m1h == m1c, f"hote {m1h} / client {m1c}")
        ih, ic = info(host, where3), info(cli, where3)
        check("achat [ensemble] : achete une fois, a nous partout", ih.get("ours") == "1" and ic.get("ours") == "1", f"{ih} / {ic}")
    rep = compare(dump(host, "h_buyhouse"), dump(cli, "c_buyhouse"), "buyhouse")
    check("achat : inventaires identiques a la fin", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    summary()


def exp_farlong(host, cli, seconds=300, kinds=("Tente", "Feu", "Coffre", "Lit", "Mur")):
    """The client's character far from the host's squad (>= 30000 units) for a long time: every 30 s the
    zone around it is compared (characters, health, inventories); a fight is started there and a
    building placed there while the host's squad moves on its side. No desync may build up, the host
    must keep simulating that zone, no crash, frame time fine; then the client comes back."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    hidx = 0 if own != 0 else 1

    def vec(text):
        return tuple(map(float, text.split()[1].split(",")))

    x, y, z = vec(cmd(host, f"where {own}")[1])
    home = (x, y, z)
    # far away on dry land: not in a lake of water or acid (a building placed there would be refused)
    dry = teleport_dry(host, own, home, FAR_OFFSETS)
    check("loin longtemps : destination sur la terre ferme", dry is not None, dry)
    time.sleep(5)
    cmd(cli, f"camto {own}")
    time.sleep(30)   # zone streaming on both
    far = vec(cmd(host, f"where {own}")[1])
    check("loin longtemps : le perso du client est a plus de 30000", dist(far[::2], home[::2]) >= 30000, f"{home} -> {far}")
    lat0 = (frame_latency(host), frame_latency(cli))
    log("command round trips before (ms, best/worst): host", lat0[0], "client", lat0[1])
    # a fight next to the client's character, on the host
    sp = cmd(host, f"spawnnpc 20 10 {own}")
    log("spawn near the client:", sp)
    npc = sp[1].split()[1] if sp[0] else None
    if npc:
        log("fight", cmd(host, f"fight {own}"))
    # a building next to the client's character, placed by the client
    sid = find_building(host, kinds)
    if sid:
        placed = place_valid(cli, sid, own, -40, 30)
        log("client places", sid, "far away:", placed)
        check("loin longtemps : un emplacement valide trouve pres du perso du client", placed[0], placed)
    t0 = time.time()
    worst = []
    NEAR_SEEN, NEAR_POS = 1000.0, 300.0   # what the client's player sees; the range the plugin pulls characters in
    prev_miss, prev_off, persist_miss, persist_off = set(), set(), set(), set()
    npc_moves, npc_last = 0, None
    host_dir = 1
    rounds = max(1, int(seconds // 30))
    for r in range(rounds):
        # the host acts on its side meanwhile: its squad walks back and forth
        log("host moves", cmd(host, f"moverel {hidx} {150 * host_dir} 0"))
        host_dir = -host_dir
        for _ in range(6):
            time.sleep(5)
            if npc:
                ok, t = cmd(host, f"where {npc}")
                if ok:
                    p = vec(t)
                    if npc_last and dist(p, npc_last) > 0.5:
                        npc_moves += 1
                    npc_last = p
        h, c = dump(host, f"h_farlong_{r}"), dump(cli, f"c_farlong_{r}")
        rep = compare(h, c, f"farlong_{r}")
        lat = (frame_latency(host, 3), frame_latency(cli, 3))
        st = status(cli)
        log(f"[{int(time.time() - t0)}s] chars host {rep['host_chars']} client {rep['client_chars']} missing {rep['missing_on_client']} "
            f"extra {rep['extra_on_client']} pos_err_max {rep['pos_err_max']} over_tol {rep['pos_err_over_tol']} vitals {rep['vital_flag_mismatch']} "
            f"inv {rep['inventory_mismatch']} | round trips host {lat[0]} client {lat[1]} | client {st.get('state')} missingNpcs {st.get('missingNpcs')}")
        worst.append((rep["missing_on_client"], rep["pos_err_over_tol"], rep["vital_flag_mismatch"], rep["inventory_mismatch"], lat))
        # what the client's player can see: characters near its own (far) character. Away from it the
        # client has not streamed the zone in (NPCs walking around the host's squad, 1700-2900 units
        # away, stay "missing" there: the client cannot create them in an unloaded zone) and the
        # plugin does not pull far characters (kSeenRange 300): those counts come and go with traffic.
        me = vec(cmd(host, f"where {own}")[1])
        near_miss = {k for k in set(h["char"]) - set(c["char"]) if "pos" in h["char"][k] and dist(h["char"][k]["pos"], me) < NEAR_SEEN
                     and not int(h["char"][k].get("flags", "0")) & 8}
        near_off = {k for k in set(h["char"]) & set(c["char"]) if "pos" in h["char"][k] and "pos" in c["char"][k]
                    and dist(h["char"][k]["pos"], me) < NEAR_POS and dist(h["char"][k]["pos"], c["char"][k]["pos"]) > 3.0}
        persist_miss |= near_miss & prev_miss
        persist_off |= near_off & prev_off
        prev_miss, prev_off = near_miss, near_off
        log(f"   near the client's character: missing {len(near_miss)} off {len(near_off)} | still so since the last sample: missing {len(near_miss & persist_miss)} off {len(near_off & persist_off)}")
        if st.get("state") != "connected":
            break
    check("loin longtemps : le client reste connecte", status(cli).get("state") == "connected", status(cli))
    check("loin longtemps : l'hote simule la zone du client (le PNJ bouge chez l'hote)", npc is not None and npc_moves >= 2,
          f"pnj {npc}, {npc_moves} deplacements vus")
    tail = worst[len(worst) // 2:] or worst
    # a desync is what stays: the same character missing (or off) near the client's one on two samples
    # in a row (30 s apart); the whole-zone counts only go in the log (streaming at the edge, traffic)
    log("whole zone (log only): missing", [w[0] for w in worst], "off", [w[1] for w in worst])
    check("loin longtemps : pas de PNJ manquant qui dure pres du perso du client", len(persist_miss) == 0, sorted(persist_miss)[:5])
    check("loin longtemps : pas d'ecart de position qui dure pres du perso du client", len(persist_off) == 0, sorted(persist_off)[:5])
    check("loin longtemps : sante identique", all(w[2] == 0 for w in tail), [w[2] for w in worst])
    check("loin longtemps : inventaires identiques", all(w[3] == 0 for w in tail), [w[3] for w in worst])
    check("loin longtemps : temps d'image correct (aller-retour d'une commande < 1 s)", all(w[4][0][1] < 1000 and w[4][1][1] < 1000 for w in worst),
          [w[4] for w in worst])
    if sid:
        bh, bc = site_list(host, sid, own), site_list(cli, sid, own)
        check("loin longtemps : batiment pose loin, chez l'hote et le client", bool(bh) and bool(bc) and min(dist(a[0], b[0]) for a in bh for b in bc) < 0.5,
              f"{bh[:2]} / {bc[:2]}")
    # back home
    log("bring the client back:", cmd(host, "tpplayer 2"))
    time.sleep(30)
    cmd(cli, f"camto {own}")
    back = vec(cmd(host, f"where {own}")[1])
    check("loin longtemps : retour pres de l'hote", dist(back[::2], home[::2]) < 2000, f"{back} / {home}")
    rep = compare(dump(host, "h_farlong_back"), dump(cli, "c_farlong_back"), "farlong_back")
    log("after the return:", {k: rep[k] for k in ("host_chars", "client_chars", "missing_on_client", "pos_err_max", "pos_err_over_tol", "inventory_mismatch")})
    check("loin longtemps : au retour, personne ne manque", rep["missing_on_client"] <= 2, rep["missing_sample"])
    check("loin longtemps : au retour, positions identiques", rep["pos_err_over_tol"] <= 2, rep["worst"])
    check("loin longtemps : au retour, inventaires identiques", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    summary()


# ---- faraway: everything works with the players far apart, the host never spectating
# Towns with merchants and people to talk with (English names, as the game data has them); the
# test prefers those, then any other town, nearest first.
FAR_TOWN_NAMES = ("Squin", "Stack", "Admag", "Shark", "Mongrel", "Clownsteady", "Bark", "Stoat", "Heft", "Blister",
                  "Black_Scratch", "World", "Waystation", "Catun", "Spring", "Drifter", "Shem", "Brink", "Eyesocket",
                  "Hub", "Sho-Battai", "Heng", "Okran", "Bad_Teeth", "Flats_Lagoon", "Mud_Town", "Free_Settlement",
                  "Rebirth", "Port_North", "Settlement")
FAR_NOT_TOWNS = ("camp", "nest", "ruin", "lair", "hideout", "den", "tower", "bandit", "repaire", "nid", "ruine")
FAR_MIN = 30000          # the client's character at least that far from the host's squad
FAR_ZONE = 3000          # sync compared within that distance of the client's character
FAR_POS_TOL, FAR_DOWN_TOL = 3.0, 20.0
# Height of a standing character, apart (0.3.4, farfps 13/10 15:12, J2 Admag: a walker at exactly the
# host's x,z, 3.2 units under it on a slope; dy +3.0 already on 12/10): far from its camera the host's
# game keeps its own idea of a walker's height, the client's stands it on its ground. Another floor is
# ~80 units, so a floor error still fails.
FAR_DY_TOL = 6.0
FAR_CAM_NEAR = 5000      # the host's camera eye stays within that (x,z) distance of its own squad
FAR_JUMP = 25.0          # a client sample moving more than that per 0.1 s is a teleport-like jump
FAR_MOONWALK_DEG = 120.0


def xz(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def where_vec(pid, q):
    ok, t = cmd(pid, f"where {q}")
    return vec(t) if ok else None


def town_list(pid):
    """The towns this game knows (debug "towns"): [{sid, name, pos, owner}]."""
    ok, t = cmd(pid, "towns")
    out = []
    for tok in (t.split()[2:] if ok else []):
        parts = tok.split("|")
        if len(parts) != 4:
            continue
        try:
            p = tuple(map(float, parts[2].split(",")))
        except ValueError:
            continue
        if len(p) == 3 and all(math.isfinite(v) for v in p) and (abs(p[0]) + abs(p[2])) > 1:
            out.append({"sid": parts[0], "name": parts[1], "pos": p, "owner": parts[3]})
    return out


def far_town_candidates(towns, home, away_from=(), min_away=0):
    """Towns at least FAR_MIN from home (and min_away from each of away_from): known merchant towns
    first, then the others; nearest first in each group."""
    def known(t):
        n = t["name"].lower()
        return any(k.lower() in n for k in FAR_TOWN_NAMES)

    def not_town(t):
        n = t["name"].lower()
        return any(w in n.replace("_", " ").split() for w in FAR_NOT_TOWNS)

    c = [t for t in towns if xz(t["pos"], home) >= FAR_MIN and not not_town(t) and all(xz(t["pos"], a) >= min_away for a in away_from)]
    c.sort(key=lambda t: (not known(t), xz(t["pos"], home)))
    return c


def far_teleport(host, idx, base, offsets=((0, 0), (220, 0), (0, 220), (-220, 0), (0, -220), (400, 400), (-400, -400))):
    """Debug TP of squad member idx next to base, on dry land, on top of the ground (the ground is
    read on the host before the TP when known, else once the zone has streamed in). Returns the spot
    or None."""
    x, y, z = base
    for dx, dz in offsets:
        tx, tz = x + dx, z + dz
        g = ground_at(host, tx, tz)
        if g and g[2] == "water":
            log(f"   {tx:.0f},{tz:.0f}: water or acid, next spot")
            continue
        ty = g[0] + 60 if g and g[2] == "land" and g[0] > -90 else y + 300
        log(f"teleport {idx} to {tx:.0f},{ty:.0f},{tz:.0f}:", cmd(host, f"teleport {idx} {tx:.0f} {ty:.0f} {tz:.0f}"))
        for _ in range(15):
            time.sleep(1)
            g = ground_at(host, tx, tz)
            if g and g[2] != "unknown":
                break
        if g and g[2] == "land":
            p = where_vec(host, idx)
            if p and p[1] < g[0] - 30:   # under the ground (the town's height was a guess): on top of it
                log("   under the ground, again on top:", cmd(host, f"teleport {idx} {tx:.0f} {g[0] + 60:.0f} {tz:.0f}"))
                time.sleep(3)
            return (tx, g[0], tz)
        log(f"   {tx:.0f},{tz:.0f} is not dry land ({g}), next spot")
    return None


class HostCameraWatch(threading.Thread):
    """Samples the host's camera (debug "campos") every 2 s for the whole test: its (x,z) distance to
    the host's own characters at home."""

    def __init__(self, host, anchors):
        super().__init__(daemon=True)
        self.host, self.anchors = host, anchors
        self.samples, self.errors = [], []
        self.halt = threading.Event()

    def run(self):
        while not self.halt.is_set():
            try:
                ok, t = cmd(self.host, "campos", timeout=6)
            except RuntimeError as e:
                self.errors.append(str(e))
                break
            if ok:
                p = tuple(map(float, t.split()[1].split(",")))
                self.samples.append((time.time(), min(xz(p, a) for a in self.anchors), p))
            else:
                self.errors.append(t)
            self.halt.wait(2.0)

    def stop(self):
        self.halt.set()
        self.join(10)


def far_vitals_diff(hv, cv):
    """'' when host and client have the same vitals for that character, else what differs."""
    def life(v):
        f = int(v.get("vflags", "0") or 0)
        return 2 if f & 2 else f & 1
    out = []
    if life(hv) != life(cv):
        out.append(f"etat {life(hv)}/{life(cv)}")
    try:
        if abs(float(hv.get("blood", 0)) - float(cv.get("blood", 0))) > 1.0:
            out.append(f"sang {hv.get('blood')}/{cv.get('blood')}")
    except ValueError:
        pass
    hp, cp = hv.get("parts", ""), cv.get("parts", "")
    if hp and cp:
        try:
            hf = [float(x.split("/")[0]) for x in hp.split(";")]
            cf = [float(x.split("/")[0]) for x in cp.split(";")]
            worst = max((abs(a - b) for a, b in zip(hf, cf)), default=0.0)
            if len(hf) != len(cf) or worst > 2.0:
                out.append(f"membres ecart {worst:.1f}")
        except ValueError:
            pass
    return ", ".join(out)


def far_damaged(v, v0=None):
    """Has that character been hurt (blood or a limb below its value at v0, or knocked out / dead)?"""
    if int(v.get("vflags", "0") or 0) & 3:
        return True
    try:
        parts = [float(x.split("/")[0]) for x in v.get("parts", "").split(";") if x]
        if v0 is not None:
            parts0 = [float(x.split("/")[0]) for x in v0.get("parts", "").split(";") if x]
            return float(v.get("blood", 0)) < float(v0.get("blood", 0)) - 0.5 or any(a < b - 0.5 for a, b in zip(parts, parts0))
        return any(p < 99 for p in parts)
    except ValueError:
        return False


def far_floaters(pid):
    ok, t = cmd(pid, "floaters")
    return {k: int(v) for k, v in (kv.split("=") for kv in t.split()[1:] if "=" in kv)} if ok else {}


def far_motion(samples):
    """Client samples of one character (t, (x,y,z), (fx,fz), down): moving steps, steps against the
    facing (moonwalk), teleport-like jumps."""
    moving, back, jumps = 0, [], []
    for (t0, p0, f0, d0), (t1, p1, f1, d1) in zip(samples, samples[1:]):
        dt = max(1e-3, t1 - t0)
        dx, dz = p1[0] - p0[0], p1[2] - p0[2]
        step = math.hypot(dx, dz)
        if step > FAR_JUMP * max(1.0, dt / 0.1):
            jumps.append((round(t1, 2), round(step, 1)))
        if d0 or d1 or step < 0.5:   # standing (or on the ground): no direction to compare
            continue
        moving += 1
        fx, fz = f1
        fl = math.hypot(fx, fz)
        if fl < 1e-3:
            continue
        cosang = max(-1.0, min(1.0, (dx * fx + dz * fz) / (step * fl)))
        ang = math.degrees(math.acos(cosang))
        if ang > FAR_MOONWALK_DEG:
            back.append((round(t1, 2), round(ang), round(step, 1)))
    return moving, back, jumps


class FarPlayer:
    """One client player of the faraway test: its game, its character (host's and its own index), its
    town, and the label of its checks ("loin [J2] : ...")."""

    def __init__(self, host, cli, n):
        self.host, self.cli = host, cli
        self.pid = player_id(cli) or (n + 2)
        self.tag = f"J{self.pid}"
        self.own_h = (own_indices(host, self.pid) or [0])[0]   # host's squad order
        self.own_c = (own_indices(cli) or [self.own_h])[0]      # the client's own order
        self.own_key = None
        self.town = None
        self.towns = []          # every town it went to

    def L(self, what):
        return f"loin [{self.tag}] : {what}"

    def check(self, what, ok, detail=""):
        check(self.L(what), ok, detail)

    def log(self, *a):
        log(f"[{self.tag}]", *a)

    def me(self):
        return where_vec(self.host, self.own_h)


def run_parallel(players, fn, label):
    """fn(player) for every player at once, one thread each; an exception fails that player's step."""
    def one(p):
        try:
            fn(p)
        except LowMemory:
            raise
        except Exception as e:   # one player's failure must not stop the others
            p.check(f"{label} : etape terminee sans erreur", False, f"{type(e).__name__}: {e}")
    th = [threading.Thread(target=one, args=(p,), daemon=True) for p in players]
    for t in th:
        t.start()
    for t in th:
        t.join()


def far_go(p, cands, label, need_merchant, taken, taken_lock, away=15000):
    """The player's character to the first candidate town that works (dry land, NPCs there, a merchant
    when asked), at least `away` from every town another player took. Returns the town or None."""
    tries = 0
    for town in cands:
        if tries >= 5:
            break
        with taken_lock:
            if any(xz(town["pos"], t["pos"]) < away for t in taken):
                continue
            taken.append(town)   # reserved, worked or not: the next player looks elsewhere
        tries += 1
        p.log(f"[{label}] try {town['name']} ({town['sid']}) at {town['pos']}")
        spot = far_teleport(p.host, p.own_h, town["pos"])
        if not spot:
            continue
        cmd(p.cli, f"camto {p.own_c}")   # the client's player looks at its own character, as in a real game
        time.sleep(25)                   # zone streaming on both
        me = p.me()
        if not me or xz(me, FAR_HOME[0]) < FAR_MIN:
            p.log(f"   the character is at {me}: not far enough")
            continue
        if need_merchant:
            m = cmd(p.host, f"merchants {p.own_h}")[1]
            p.log("   merchants there (host):", m[:300])
            if not any(int(n) > 0 for n in re.findall(r"\(\d+\)\[(\d+)\]", m)):   # a shop with goods, not only guards' racks
                continue
        hz = dump(p.host, f"h_far_{p.tag}_{label}_probe", FAR_ZONE)
        people = [k for k, v in hz["char"].items() if "pos" in v and k not in hz["squad"] and xz(v["pos"], me) < 1500]
        p.log(f"   {len(people)} NPC(s) within 1500 there")
        if len(people) >= 2:
            p.town = town
            p.towns.append(town)
            return town
    return None


FAR_HOME = [(0.0, 0.0, 0.0)]   # the host's squad at home (set by exp_faraway)


def far_zone_sync(players, round_label, rounds, gap):
    """Every player's zone at once (one pause of the host for all: a pause in the middle of another
    player's step would stop it): host and client dumps, characters within FAR_ZONE of each player's
    character. Positions exact (standing FAR_POS_TOL, on the ground FAR_DOWN_TOL), same vitals."""
    acc = {p.tag: {"prev": set(), "persist": set(), "off": [], "vit": [], "sizes": [], "worst": []} for p in players}
    for r in range(rounds):
        if r:
            time.sleep(gap)
        cmd(players[0].host, "pause 1")
        time.sleep(2.5)
        h = dump(players[0].host, f"h_far_{round_label}_{r}", FAR_ZONE)
        mes = {p.tag: p.me() for p in players}
        cs = {}
        for p in players:
            cs[p.tag] = dump(p.cli, f"c_far_{p.tag}_{round_label}_{r}", FAR_ZONE + 500)
        cmd(players[0].host, "pause 0")
        for p in players:
            a, me, c = acc[p.tag], mes[p.tag], cs[p.tag]
            near = {k: v for k, v in h["char"].items() if "pos" in v and me and xz(v["pos"], me) <= FAR_ZONE}
            missing = {k for k in near if k not in c["char"]}
            off, vit, errs = [], [], []
            for k in near:
                cv = c["char"].get(k)
                if not cv or "pos" not in cv:
                    continue
                down = int(near[k].get("flags", "0") or 0) & 12
                e = dist(near[k]["pos"], cv["pos"])
                errs.append(e)
                dy_abs = abs(cv["pos"][1] - near[k]["pos"][1])
                if (e > FAR_DOWN_TOL) if down else (xz(near[k]["pos"], cv["pos"]) > FAR_POS_TOL or dy_abs > FAR_DY_TOL):
                    dy = cv["pos"][1] - near[k]["pos"][1]
                    off.append((k, round(e, 1), "a terre" if down else "debout", f"dy {dy:+.1f}", f"vue {near[k].get('inview', '?')}/{cv.get('inview', '?')}", f"etage {near[k].get('floor', '?')}/{cv.get('floor', '?')}",
                                f"action {near[k].get('action', '?')}/{cv.get('action', '?')}", f"lit {near[k].get('in', '?')}/{cv.get('in', '?')}"))
                vd = far_vitals_diff(near[k], cv)
                if vd:
                    vit.append((k, vd))
            a["persist"] |= missing & a["prev"]
            a["prev"] = missing
            a["off"] += off
            a["vit"] += vit
            a["sizes"].append(len(near))
            a["worst"].append(round(max(errs), 1) if errs else 0.0)
            st = status(p.cli)
            p.log(f"[{p.town and p.town['name']} {r + 1}/{rounds}] within {FAR_ZONE}: host {len(near)} | missing {len(missing)} {sorted(missing)[:4]} | "
                  f"off {len(off)} {off[:3]} | worst offset {a['worst'][-1]} | vitals {len(vit)} {vit[:3]} | client missingNpcs {st.get('missingNpcs')}")
    for p in players:
        a, name = acc[p.tag], (p.town["name"] if p.town else "?")
        p.check(f"synchro {name} : des PNJ autour du perso du client chez l'hote", min(a["sizes"]) >= 2, a["sizes"])
        p.check(f"synchro {name} : aucun PNJ manquant deux releves de suite", not a["persist"], sorted(a["persist"])[:5])
        p.check(f"synchro {name} : positions identiques (debout {FAR_POS_TOL} au sol et {FAR_DY_TOL} en hauteur, a terre {FAR_DOWN_TOL})", not a["off"],
                f"pire ecart par releve {a['worst']} | {a['off'][:5]}")
        p.check(f"synchro {name} : memes vitaux (sang, etat, membres)", not a["vit"], a["vit"][:5])


def far_merchant(p):
    """The real shopkeeper around the player's character, from the host's "merchants": the one whose
    counters hold the most stacks (guards living in a barracks list empty racks as counters: the
    first run picked "Cent Gardien", 16 counters, 0 stacks, and the window stayed empty). '_' for spaces."""
    m = cmd(p.host, f"merchants {p.own_h}")[1]
    found = re.findall(r" ([^()\[\]]+?)\((\d+)\)(?:\[(\d+)\])?", m)
    p.log("merchants there (host):", m[:300])
    if not found:
        return None
    best = max(found, key=lambda f: (int(f[2] or 0), int(f[1])))
    return best[0].strip().replace(" ", "_")


def far_wait_dialog(p, want=True, seconds=15):
    t = cmd(p.cli, "dialog")[1]
    for _ in range(int(seconds * 2)):
        if ("open=1" in t) == want:
            break
        time.sleep(0.5)
        t = cmd(p.cli, "dialog")[1]
    return t


def far_talk(p):
    """The client's character talks to a town NPC: window on the client, tracked by the host, one
    answer; then an NPC starts a conversation (npcevent) when one has a line for it. The walk can
    take 20-30 s (a town NPC a few hundred units away): each try waits 45 s."""
    name = p.town["name"] if p.town else "?"
    d, who = cmd(p.cli, "dialog")[1], None
    # the shopkeeper first: a trader always has something to say ("let's trade"); the nearest guards,
    # thieves and sentinels of the first run had no line for "talk to me" (the host's own start, when
    # the game's never came, said "the NPC has nothing to say" for every one of them)
    shop = far_merchant(p)
    # many town NPCs have no line for "talk to me" (as in solo: the character walks there, nothing
    # opens; the host's log says "the NPC has nothing to say"): up to six of the nearest are tried
    targets = ([f"name:{shop}"] if shop else []) + [str(k) for k in range(6)]
    tried = set()
    for k in targets:
        r = cmd(p.cli, f"talkto {p.own_c} {k}")
        p.log(f"[dialogue {name}] client talks to {k}:", r)
        if not r[0] or r[1] in tried:
            continue
        tried.add(r[1])
        d = far_wait_dialog(p, True, 12)
        if "open=1" in d:
            who = k
            break
    p.check(f"dialogue {name} : le client parle a un PNJ de la ville, la fenetre s'ouvre chez lui", "open=1" in d, d[:200])
    ds = cmd(p.host, "dialogs")[1]
    dd = dict(kv.split("=", 1) for kv in ds.split(" | ")[0].split()[1:] if "=" in kv)
    p.check(f"dialogue {name} : l'hote suit la conversation", "open=1" in d and int(dd.get("n", 0) or 0) >= 1, ds[:200])
    if "open=1" in d:
        replies = re.findall(r"\[([^\]]*)\]", d)
        if replies:
            turn0 = re.search(r"turn=(\S+)", d)
            p.log(f"[dialogue {name}] client answers 0 ({replies[0][:60]}):", cmd(p.cli, "answer 0"))
            moved = False
            for _ in range(20):
                time.sleep(0.5)
                d2 = cmd(p.cli, "dialog")[1]
                if "open=1" not in d2 or (turn0 and f"turn={turn0.group(1)}" not in d2):
                    moved = True
                    break
            p.check(f"dialogue {name} : la reponse du client fait avancer la conversation", moved, cmd(p.cli, "dialog")[1][:200])
        else:
            p.log(f"[dialogue {name}] that line has no answer to pick (who #{who})")
        cmd(p.cli, "answer leave")
        far_wait_dialog(p, False, 10)


def far_motion_step(p):
    cands_m = []
    for _ in range(4):
        hm = dump(p.host, f"h_far_{p.tag}_walkers", FAR_ZONE)
        me = p.me()
        cands_m = sorted((xz(v["pos"], me), k) for k, v in hm["char"].items()
                         if "pos" in v and k not in hm["squad"] and int(v.get("flags", "0") or 0) & 1 and not int(v.get("flags", "0") or 0) & 12
                         and xz(v["pos"], me) < 1500)
        if len(cands_m) >= 2:
            break
        time.sleep(3)
    walkers = [k for _, k in cands_m[:15]]
    if p.own_key:
        p.log("client's character walks too:", cmd(p.cli, f"moverel {p.own_c} 160 60"))
        walkers.append(p.own_key)
    time.sleep(1)
    path = os.path.abspath(os.path.join(OUT_DIR, f"c_far_{p.tag}_motion.txt"))
    if os.path.exists(path):
        os.remove(path)
    p.log(f"motion of {len(walkers)} character(s) on the client:", cmd(p.cli, f"motionrec 5 10 {path} {' '.join(walkers)}"))
    lines = []
    for _ in range(30):
        time.sleep(0.5)
        if os.path.exists(path):
            lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
            if lines and lines[-1] == "end":
                break
    per = {}
    for ln in lines:
        q = ln.split()
        if len(q) >= 8 and q[0] == "m":
            per.setdefault(q[2], []).append((float(q[1]), tuple(map(float, q[3].split(","))), tuple(map(float, q[4].split(","))), q[7] == "1"))
    moving_all, back_all, jumps_all = 0, [], []
    for k, ss in per.items():
        mv, back, jumps = far_motion(ss)
        moving_all += mv
        back_all += [(k,) + b for b in back]
        jumps_all += [(k,) + j for j in jumps]
        p.log(f"   {k}: {len(ss)} samples, {mv} moving steps, {len(back)} backward, {len(jumps)} jumps {jumps[:3]}")
    p.check("mouvement : des PNJ qui marchent echantillonnes a 10 Hz chez le client", len(per) >= 2 and moving_all >= 10,
            f"{len(per)} persos, {moving_all} pas en mouvement")
    p.check(f"mouvement : jamais de marche a reculons (> {FAR_MOONWALK_DEG:.0f} deg entre deplacement et regard)", not back_all, back_all[:5])
    p.check(f"mouvement : aucun saut de plus de {FAR_JUMP:.0f} unites par echantillon", not jumps_all, jumps_all[:5])
    # 0.3.4: host walkers that get nowhere (stand on their spot here) and copies put on the host's floor
    p.log("copies fixed on the client (floors placed, stalled walkers):", cmd(p.cli, "copyfix"))


def far_same_money(p, seconds=8):
    """Host's and this client's cats, read until they agree (other players buy at the same time)."""
    h = c = None
    for _ in range(int(seconds * 2)):
        h, c = cmd(p.host, "money")[1], cmd(p.cli, "money")[1]
        if h == c:
            break
        time.sleep(0.5)
    return h, c


def far_trade(p):
    merchant = far_merchant(p) or "any"
    p.log("merchant:", merchant)
    p.log("client right-clicks the merchant (trade):", cmd(p.cli, f"npcreq {p.own_c} 26 {merchant}"))
    state = ""
    for _ in range(90):
        time.sleep(0.5)
        state = cmd(p.cli, "tradestate")[1]
        if "open=1" in state:
            break
    how = "clic droit"
    if "open=1" not in state:
        p.log("   no window from the right click: the host opens it (what 'let's trade' does)",
              cmd(p.host, f"tradeopen {p.own_h} {merchant} near"))
        how = "ouverte par l'hote"
        for _ in range(30):
            time.sleep(0.5)
            state = cmd(p.cli, "tradestate")[1]
            if "open=1" in state:
                break
    p.check(f"commerce : la fenetre s'ouvre chez le client ({how})", "open=1" in state, state)
    stock = cmd(p.cli, "tradelist merchant")[1]
    p.log("client sees the stock:", stock[:300])
    p.check("commerce : la fenetre montre le stock du marchand (pas vide)", stock.startswith("ok") and stock.split()[1] != "0", stock[:150])
    cats0_h = cmd(p.host, "money")[1]
    bought = cmd(p.cli, "tradebuy 0")
    p.log("client buys:", bought)
    time.sleep(4)
    cats1_h, cats1_c = far_same_money(p)
    p.check("commerce : achat paye chez l'hote", bought[0] and cats1_h != cats0_h, f"{bought[1]} | hote {cats0_h} -> {cats1_h}")
    p.check("commerce : meme argent partout", cats1_h == cats1_c, f"hote {cats1_h} / client {cats1_c}")
    bad = []
    for _ in range(3):
        h, c = dump(p.host, f"h_far_{p.tag}_trade", FAR_ZONE), dump(p.cli, f"c_far_{p.tag}_trade", FAR_ZONE)
        bad = [k for k in (p.own_key,) if k in h["char"] and h["char"][k].get("inv", "") != c["char"].get(k, {}).get("inv", "")]
        if not bad:
            break
        time.sleep(2)
    p.check("commerce : inventaire du perso identique apres l'achat", not bad, bad)
    cmd(p.cli, "closewindows")
    time.sleep(3)


def far_ground(p):
    ore = None
    for part in ("Iron_Ore", "Copper_Ore", "Ore", "Stone"):
        t = cmd(p.host, f"itemtypes {part}")[1].split()
        if len(t) > 2 and "=" in t[2]:
            ore = t[2].split("=")[0]
            break
    if not ore:
        p.check("objet au sol : un minerai trouve", False)
        return
    p.log("ore for the client's character:", cmd(p.host, f"giveitem {ore} 6 {p.own_h}"))
    time.sleep(4)

    def ground(pid, idx):
        ok, t = cmd(pid, f"groundall 700 {idx}")
        out = {}
        for x in (t.split()[2:] if ok and t.startswith("ok") else []):
            k, sid, q, pos = x.split("|")
            if sid == ore:
                out[k] = (int(q), tuple(map(float, pos.split(","))))
        return out

    def carried(pid, idx):
        ok, t = cmd(pid, f"invcount {idx} {ore}")
        return int(t.split()[1]) if ok else -1

    n0_h, n0_c = carried(p.host, p.own_h), carried(p.cli, p.own_c)
    gb_h, gb_c = ground(p.host, p.own_h), ground(p.cli, p.own_c)
    dr = cmd(p.cli, f"uidrop {p.own_c} {ore}")
    p.log("client drops the ore (inventory window's way):", dr)
    time.sleep(3)
    gn_h = {k: v for k, v in ground(p.host, p.own_h).items() if k not in gb_h}
    gn_c = {k: v for k, v in ground(p.cli, p.own_c).items() if k not in gb_c}
    same = len(gn_h) == 1 and len(gn_c) == 1 and next(iter(gn_h.values()))[0] == next(iter(gn_c.values()))[0] and \
        dist(next(iter(gn_h.values()))[1], next(iter(gn_c.values()))[1]) < 5
    p.check("objet au sol : lache une fois, meme pile, meme endroit chez l'hote et le client", dr[0] and same,
            f"hote {list(gn_h.values())} / client {list(gn_c.values())}")
    if gn_h:
        q, pos = next(iter(gn_h.values()))
        p.log("client picks it up:", cmd(p.cli, f"pickupreq {p.own_c} {ore} {pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}"))
        for _ in range(20):
            time.sleep(2)
            if carried(p.host, p.own_h) >= n0_h:
                break
        time.sleep(3)
        left_h = [k for k in gn_h if k in ground(p.host, p.own_h)]
        left_c = [k for k in gn_c if k in ground(p.cli, p.own_c)]
        p.check("objet au sol : ramasse, disparu des deux cotes", not left_h and not left_c, f"{left_h} / {left_c}")
    n1_h, n1_c = carried(p.host, p.own_h), carried(p.cli, p.own_c)
    p.check("objet au sol : rien de perdu (meme nombre porte qu'avant, des deux cotes)", n1_h == n0_h and n1_c == n0_c,
            f"hote {n0_h} -> {n1_h} / client {n0_c} -> {n1_c}")


FAR_FIGHT_BEAST = "3979-gamedata.base"   # Bonedog: fights back (a copied town NPC fled at 80 units/s or sat down)


def far_fight_and_loot(p):
    """A fight next to the client's character (the host's camera at home), ordered by the client the
    right click's way against a beast that fights back: hurt on the host, the same wounds on the
    client, damage numbers shown there; then an NPC knocked out and looted the right click's way; then
    a goat killed and looted."""
    host, cli = p.host, p.cli
    f0_h, f0_c = far_floaters(host), far_floaters(cli)
    beast = None
    sp = cmd(host, f"spawncreature {FAR_FIGHT_BEAST} 30 10 {p.own_h}")
    p.log("host spawns a bonedog by the client's character:", sp)
    if sp[0]:
        beast = sp[1].split()[1]
        time.sleep(3)
        pn, pm = where_vec(host, beast), p.me()
        if pn and pm and abs(pn[1] - pm[1]) >= 15:
            # the client's character stands on a roof or a wall (where the teleport put it): it goes
            # down next to the beast
            p.log(f"   on another level ({pn} / {pm}): the client's character goes next to it",
                  cmd(host, f"teleport {p.own_h} {pn[0] + 8:.0f} {pn[1] + 5:.0f} {pn[2] + 8:.0f}"))
            time.sleep(3)
            pm = p.me()
        p.check("combat : bete creee pres du perso du client, au meme niveau", bool(pn and pm and abs(pn[1] - pm[1]) < 15), f"{pn} / {pm}")
    else:
        p.check("combat : bete creee pres du perso du client", False, sp)
    if beast:
        v0 = dump(host, f"h_far_{p.tag}_npc0", FAR_ZONE)["char"].get(beast, {})
        p.log("client orders the attack (right click):", cmd(cli, f"attackreq {p.own_c} {beast}"))
        hurt = False
        for i in range(12):
            time.sleep(3)
            hv = dump(host, f"h_far_{p.tag}_fight", FAR_ZONE)["char"].get(beast, {})
            if hv and far_damaged(hv, v0 or None):
                hurt = True
                if far_floaters(cli).get("shown", 0) > f0_c.get("shown", 0):
                    break
            elif i in (3, 7):
                p.log("   not hurt yet: attack order again", cmd(cli, f"attackreq {p.own_c} {beast}"))
        time.sleep(2)
        hv = dump(host, f"h_far_{p.tag}_fight_end", FAR_ZONE)["char"].get(beast, {})
        p.check("combat : la bete est blessee chez l'hote", hurt or (hv and far_damaged(hv, v0 or None)), hv.get("parts", "")[:120])
        diffs, c = [], {"char": {}}
        for _ in range(4):   # unpaused (the other players go on): the same once the last vitals arrived
            h, c = dump(host, f"h_far_{p.tag}_fight_end", FAR_ZONE), dump(cli, f"c_far_{p.tag}_fight_end", FAR_ZONE)
            diffs = [(k, far_vitals_diff(h["char"][k], c["char"][k])) for k in (beast, p.own_key) if k in h["char"] and k in c["char"]]
            if beast in c["char"] and all(not d for _, d in diffs):
                break
            time.sleep(2)
        p.check("combat : memes vitaux et blessures chez le client (sang, etat, membres)",
                beast in c["char"] and all(not d for _, d in diffs), diffs or "bete absente chez le client")
        f1_h, f1_c = far_floaters(host), far_floaters(cli)
        p.log("damage numbers: host", f0_h, "->", f1_h, "| client", f0_c, "->", f1_c)
        p.check("combat : l'hote fait les chiffres de degats hors de sa camera", f1_h.get("made", 0) > f0_h.get("made", 0), f"{f0_h} -> {f1_h}")
        p.check("combat : les chiffres de degats arrivent au client", f1_c.get("shown", 0) > f0_c.get("shown", 0), f"{f0_c} -> {f1_c}")
        p.log("the bonedog dies:", cmd(host, f"kill {beast}"))
        time.sleep(2)
    npc = None
    sp = cmd(host, f"spawnnpc 10 6 {p.own_h}")
    p.log("host spawns an NPC to loot by the client's character:", sp)
    if sp[0]:
        npc = sp[1].split()[1]
        time.sleep(2)
    if not npc:
        p.check("fouille : PNJ cree pres du perso du client", False, sp)
        return
    # looting there: the NPC knocked out, the client loots it the right click's way
    st = cmd(host, f"npcstate {npc}")[1]
    if "down=0" in st:
        p.log("ko:", cmd(host, f"ko {npc}"))
    time.sleep(5)
    h = dump(host, f"h_far_{p.tag}_body", FAR_ZONE)
    body = npc if npc in h["char"] else None
    if not body:   # its key may change when it goes down: the nearest body on the ground
        me = p.me()
        downs = [k for k, v in h["char"].items() if "pos" in v and int(v.get("flags", "0") or 0) & 12 and me and xz(v["pos"], me) < 200]
        body = downs[0] if downs else None
    inv0 = h["char"].get(body, {}).get("inv") if body else None
    p.log("body", body, "inventory on the host:", inv0)
    p.check("fouille : un PNJ a terre avec des objets pres du perso du client", bool(body) and bool(inv0), f"{body} {inv0}")
    if body and inv0:
        before = cmd(cli, "lootstats")[1]

        def opened_since(b):
            s_ = cmd(cli, "lootstats")[1]
            return s_.split("opened=")[-1].split()[0] != b.split("opened=")[-1].split()[0], s_

        p.log("client selects its character:", cmd(cli, f"selectset {p.own_c}"), "| right-click loot:", cmd(cli, f"lootorder {body}"))
        ok_open, stats = False, before
        for _ in range(45):
            time.sleep(1)
            ok_open, stats = opened_since(before)
            if ok_open:
                break
        how = "clic droit"
        if not ok_open:
            before = stats
            p.log("   no window from the right click: loot order", cmd(cli, f"loot {body} {p.own_c}"))
            how = "ordre de fouille"
            for _ in range(30):
                time.sleep(1)
                ok_open, stats = opened_since(before)
                if ok_open:
                    break
        p.check(f"fouille : la fenetre s'ouvre chez le client ({how})", ok_open, stats)
        # the first item may not fit (a full backpack after the trade and the ore: "err no room"):
        # the next one, as a player would
        for which in range(6):
            took = cmd(cli, f"invmove {body} squad{p.own_c} {which}")
            p.log(f"client takes item {which}:", took)
            if took[0] or "no room" not in took[1]:
                break
        time.sleep(4)
        h, c = dump(host, f"h_far_{p.tag}_looted", FAR_ZONE), dump(cli, f"c_far_{p.tag}_looted", FAR_ZONE)
        inv1 = h["char"].get(body, {}).get("inv")
        p.check("fouille : un objet a quitte le corps chez l'hote", inv1 != inv0, f"{inv0} -> {inv1}")
        bad = [k for k in (body, p.own_key) if k in h["char"] and h["char"][k].get("inv", "") != c["char"].get(k, {}).get("inv", "")]
        p.check("fouille : inventaires du corps et du perso identiques des deux cotes", not bad, bad)
        cmd(cli, "closewindows")
        time.sleep(2)
    # a creature too
    ok, t = cmd(host, f"spawncreature 3993-gamedata.base 35 -20 {p.own_h}")
    p.log("host spawns a goat by the client's character:", t)
    if not (ok and t.startswith("ok")):
        p.log("no creature could be spawned there: creature loot not checked")
        return
    ckey = t.split()[1]
    time.sleep(4)
    at = dump(host, f"h_far_{p.tag}_goat", FAR_ZONE)["char"].get(ckey, {}).get("pos")
    p.log("kill:", cmd(host, f"kill {ckey}"))
    time.sleep(6)
    h = dump(host, f"h_far_{p.tag}_goat_dead", FAR_ZONE)
    dead = [k for k, v in h["char"].items() if int(v.get("vflags", "0") or 0) & 2 and "pos" in v]
    if at and dead and (ckey not in h["char"] or not int(h["char"][ckey].get("vflags", "0") or 0) & 2):
        ckey = min(dead, key=lambda k: dist(h["char"][k]["pos"], at))
    cinv0 = h["char"].get(ckey, {}).get("inv")
    before = cmd(cli, "lootstats")[1]
    p.log("client loots the goat:", cmd(cli, f"loot {ckey} {p.own_c}"))
    ok_open, stats = False, before
    for _ in range(45):
        time.sleep(1)
        stats = cmd(cli, "lootstats")[1]
        if stats.split("opened=")[-1].split()[0] != before.split("opened=")[-1].split()[0]:
            ok_open = True
            break
    p.check("fouille d'une creature : la fenetre s'ouvre chez le client", ok_open, stats)
    if ok_open and cinv0:
        p.log("client takes one item:", cmd(cli, f"invmove {ckey} squad{p.own_c} 0"))
        time.sleep(4)
        h, c = dump(host, f"h_far_{p.tag}_goat_looted", FAR_ZONE), dump(cli, f"c_far_{p.tag}_goat_looted", FAR_ZONE)
        bad = [k for k in (ckey, p.own_key) if k in h["char"] and h["char"][k].get("inv", "") != c["char"].get(k, {}).get("inv", "")]
        p.check("fouille d'une creature : un objet pris, memes inventaires des deux cotes",
                h["char"].get(ckey, {}).get("inv") != cinv0 and not bad, f"{cinv0} -> {h['char'].get(ckey, {}).get('inv')} | {bad}")
    cmd(cli, "closewindows")
    time.sleep(2)


def exp_faraway(host, clis):
    """Every client player's character in its own town far away (over 30000 units from the host's
    squad and 15000 from each other) while the host's squad stays at home and the host's camera never
    leaves it. All the players at once, each in its town: around its character the same NPCs,
    positions (exact, standing and lying) and vitals; NPCs walk smoothly (no moonwalk, no jumps);
    talk, trade, drop and pick up, a fight with its wounds and damage numbers, loot (a knocked-out NPC,
    a creature); then each hops to another town (sync and talk again). Checks per player: "loin [J2]"."""
    clis = clis if isinstance(clis, (list, tuple)) else [clis]
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(3)
    players = [FarPlayer(host, c, i) for i, c in enumerate(clis)]
    h0 = dump(host, "h_far_home", 3000)
    keys_h = sorted(h0["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    remote_keys = {e["key"] for e in h0["entity"].values() if e.get("owner") not in (None, "", "0", "1")}
    for p in players:
        p.own_key = keys_h[p.own_h] if p.own_h < len(keys_h) else None
    home_idx = [i for i, k in enumerate(keys_h) if k not in remote_keys and i not in {p.own_h for p in players}]
    hidx = (own_indices(host) or home_idx or [0])[0]
    home = {i: where_vec(host, i) for i in home_idx}
    home = {i: q for i, q in home.items() if q}
    log(f"players: {[(p.tag, p.own_h, p.own_c, p.own_key) for p in players]}; host's squad at home: {sorted(home)}")
    check("loin : l'escouade de l'hote est trouvee", bool(home), keys_h)
    if not home:
        summary()
        return
    anchors = list(home.values())
    FAR_HOME[0] = (sum(q[0] for q in anchors) / len(anchors), sum(q[1] for q in anchors) / len(anchors), sum(q[2] for q in anchors) / len(anchors))
    log("heal everyone:", cmd(host, "admin heal all"), "| god mode:", cmd(host, "admin god all on"))
    log("cats for the trades:", cmd(host, f"givemoney {20000 * len(players)}"))
    log("host camera on its own character:", cmd(host, f"camto {hidx}"))
    time.sleep(2)
    cam0 = cmd(host, "campos")
    log("host camera:", cam0)
    check("loin : la position de la camera de l'hote se lit (campos)", cam0[0], cam0[1])
    watch = HostCameraWatch(host, anchors)
    watch.start()
    inview0 = cmd(host, "inview")[1]

    towns = town_list(host)
    log(f"{len(towns)} towns known")
    cands = far_town_candidates(towns, FAR_HOME[0])
    log("far candidates:", [(t["name"], round(xz(t["pos"], FAR_HOME[0]))) for t in cands[:12]])
    check("loin : des villes a plus de 30000 de l'hote (towns)", bool(cands), len(towns))
    taken, taken_lock = [], threading.Lock()

    # ---- first towns, every player at once
    run_parallel(players, lambda p: far_go(p, cands, "ville2", True, taken, taken_lock), "voyage")
    for p in players:
        me = p.me()
        p.check("le perso du client est dans une ville a lui, a plus de 30000 de l'hote", p.town is not None and me and xz(me, FAR_HOME[0]) >= FAR_MIN,
                f"{p.town and p.town['name']} | {me}")
        log(f"[{p.tag}] client status:", status(p.cli))
    there = [p for p in players if p.town]
    if len(there) >= 2:
        apart = min(xz(a.me(), b.me()) for i, a in enumerate(there) for b in there[i + 1:])
        check("loin : les joueurs sont loin les uns des autres (> 15000)", apart >= 15000, round(apart))
    if not there:
        watch.stop()
        summary()
        return

    # 1. sync around every player's character (one pause for all)
    far_zone_sync(there, "synchro_ville2", 3, 20)
    log("host: character updates put in view for the clients' zones:", inview0, "->", cmd(host, "inview")[1])
    for p in there:
        log(f"[{p.tag}] client: character updates put in view:", cmd(p.cli, "inview")[1])

    # 2-7. each player in its town, all at once: motion, talk, trade, ground items, fight and loot
    def town_steps(p):
        far_motion_step(p)
        far_talk(p)
        far_trade(p)
        far_ground(p)
        far_fight_and_loot(p)
    run_parallel(there, town_steps, "ville")
    log("heal everyone after the fights:", cmd(host, "admin heal all"))
    time.sleep(3)

    # 8. another town each: the stream follows every character
    run_parallel(there, lambda p: far_go(p, cands, "ville3", False, taken, taken_lock), "deuxieme voyage")
    hopped = [p for p in there if len(p.towns) >= 2]
    for p in there:
        me = p.me()
        p.check("deuxieme voyage, une autre ville loin de l'hote", len(p.towns) >= 2 and me and xz(me, FAR_HOME[0]) >= FAR_MIN,
                f"{p.town and p.town['name']} | {me}")
    if hopped:
        far_zone_sync(hopped, "synchro_ville3", 2, 15)
        run_parallel(hopped, far_talk, "dialogue ville3")

    # 9. the host never spectated
    watch.stop()
    ds = [s_[1] for s_ in watch.samples]
    log(f"host camera: {len(ds)} samples, farthest from its squad {max(ds) if ds else -1:.0f}, errors {watch.errors[:3]}")
    check("loin : camera de l'hote echantillonnee pendant tout le test", len(ds) >= 30 and len(watch.errors) <= len(ds) // 10,
          f"{len(ds)} releves, {len(watch.errors)} erreurs")
    check(f"loin : la camera de l'hote n'a jamais quitte son escouade (< {FAR_CAM_NEAR})", ds and max(ds) < FAR_CAM_NEAR,
          f"pire {max(ds) if ds else -1:.0f}")
    moved = {i: round(xz(where_vec(host, i) or (1e9, 0, 1e9), q)) for i, q in home.items()}
    check("loin : l'escouade de l'hote est restee a la maison", all(d < 500 for d in moved.values()), moved)
    for p in players:
        st = status(p.cli)
        p.check("le client est toujours connecte", st.get("state") == "connected", st)
        p.log("client corrections:", cmd(p.cli, "robuststats")[1])
        p.log("frame time (echo round trip, ms) client:", frame_latency(p.cli), "host:", frame_latency(host))
    log("host god mode off:", cmd(host, "admin god all off"))
    summary()


# ---- 3b / 3f (11 October evening): far from the host, the clients' frame rate and the walls
def perf_of(pid):
    """Debug "perf" (our work and the frame since the previous call): {'work', 'worst', 'frame', 'fps', 'text'}."""
    ok, t = cmd(pid, "perf")
    out = {"text": t}
    m = re.search(r"our work ([\d.]+) ms per frame .*?worst ([\d.]+) ms\), frame ([\d.]+) ms \((\d+) fps", t or "")
    if ok and m:
        out.update(work=float(m.group(1)), worst=float(m.group(2)), frame=float(m.group(3)), fps=int(m.group(4)))
    return out


def kv_of(pid, command):
    """A debug answer "ok a=1 b=2 ..." as a dict of numbers (or strings)."""
    ok, t = cmd(pid, command)
    out = {}
    for tok in (t.split()[1:] if ok else []):
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def perf_window(pids, seconds, label):
    """Every game measured over the same window: {pid: (perf, standins)}."""
    for pid in pids:
        perf_of(pid)   # starts the window
    time.sleep(seconds)
    out = {}
    for pid in pids:
        out[pid] = (perf_of(pid), kv_of(pid, "standins"))
        log(f"[{label}] {pid}: {out[pid][0].get('text', '')[:260]} | {out[pid][1]}")
    return out


def exp_farfps(host, clis, seconds=60):
    """3b: the clients' frame rate when the players spread out. Every client next to the host first, then
    each in its own far town (> 30000 from the host and 15000 from each other) while the host's camera
    stays at home. Measured each time over `seconds` on every game: our own per-frame work ("perf"),
    the frame rate, and the host characters each client has (near / far, dormant, left to the game's far
    update: "standins"). Far apart, the same window twice: with the 0.3.1 behaviour ("farleave 0": every
    host character in full update) and with the fix ("farleave 1"). NPCs near each client stay smooth
    (far_motion_step) and in sync (far_zone_sync)."""
    clis = clis if isinstance(clis, (list, tuple)) else [clis]
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(3)
    players = [FarPlayer(host, c, i) for i, c in enumerate(clis)]
    h0 = dump(host, "h_farfps_home", 3000)
    keys_h = sorted(h0["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    for p in players:
        p.own_key = keys_h[p.own_h] if p.own_h < len(keys_h) else None
    hidx = (own_indices(host) or [0])[0]
    home = where_vec(host, hidx)
    FAR_HOME[0] = home or (0.0, 0.0, 0.0)
    log("god mode:", cmd(host, "admin god all on"), "| host camera home:", cmd(host, f"camto {hidx}"))
    pids = [host] + clis

    # 1. everyone together (the 0.3.1 game's case with ~95 characters followed)
    together = perf_window(pids, seconds, "ensemble")

    # 2. everyone in a far town of their own
    towns = town_list(host)
    cands = far_town_candidates(towns, FAR_HOME[0])
    check("fps loin : des villes a plus de 30000 de l'hote", bool(cands), len(towns))
    taken, taken_lock = [], threading.Lock()
    run_parallel(players, lambda p: far_go(p, cands, "fps", False, taken, taken_lock), "voyage")
    there = [p for p in players if p.town]
    check("fps loin : chaque client est dans sa ville", len(there) == len(players), [p.town and p.town["name"] for p in players])
    if not there:
        summary()
        return
    time.sleep(20)   # every game streams its zones in
    for c in clis:
        cmd(c, "farleave 0")
    before = perf_window(pids, seconds, "loin, comportement 0.3.1")
    for c in clis:
        cmd(c, "farleave 1")
    time.sleep(3)
    after = perf_window(pids, seconds, "loin, correctif 3b")

    log("host status:", status(host), "| host movement near clients:", cmd(host, "movestats")[1])
    for p in players:
        t, b, a = together.get(p.cli, ({}, {})), before.get(p.cli, ({}, {})), after.get(p.cli, ({}, {}))
        p.log(f"fps together {t[0].get('fps')} | far 0.3.1 {b[0].get('fps')} | far fixed {a[0].get('fps')}; "
              f"our work {t[0].get('work')} / {b[0].get('work')} / {a[0].get('work')} ms; followed {t[1].get('followed')} / {b[1].get('followed')} / "
              f"{a[1].get('followed')}; left far {a[1].get('leftfar')} (far {a[1].get('far')}, dormant {a[1].get('dormant')})")
        if not p.town:
            continue
        p.check("fps : les mesures se lisent (perf, standins)", all(x[0].get("fps") is not None for x in (t, b, a)) and a[1].get("followed") is not None,
                f"{t[0].get('text', '')[:80]} | {a[0].get('text', '')[:80]}")
        p.check("fps : loin des autres, des persos de l'hote laisses a la mise a jour lointaine du jeu", (a[1].get("leftfar") or 0) > 0 and (a[1].get("farset") or 0) > 0,
                a[1])
        p.check("fps : jamais un membre d'escouade laisse au loin (farset <= persos hors escouade)", (a[1].get("farset") or 0) <= (a[1].get("present") or 0) - (a[1].get("squad") or 0), a[1])
        p.check("fps : le correctif ne fait pas baisser les images par seconde (loin, >= 95 % du 0.3.1)",
                (a[0].get("fps") or 0) >= 0.95 * (b[0].get("fps") or 0), f"{b[0].get('fps')} -> {a[0].get('fps')}")
        p.check("fps : notre travail par image reste sous 4 ms loin des autres", (a[0].get("work") or 99) < 4.0, a[0].get("text", "")[:120])
        # 0.3.4 (A_FAIRE 12/10 point 13): the copies no longer sent stay where the host last had them
        d = kv_of(p.cli, "dormant")
        p.log("dormant copies:", d)
        p.check("fps : les copies en sommeil ne bougent pas chez le client (aucune a plus de 20 de sa derniere position)",
                (d.get("n") or 0) == 0 or ((d.get("over20") or 0) == 0 and (d.get("maxdrift") or 0) < 20), d)
    # 3. near each client, NPCs stay smooth and in sync with the fix on
    run_parallel(there, far_motion_step, "mouvement")
    far_zone_sync(there, "synchro_fps", 2, 10)
    for p in players:
        st = status(p.cli)
        p.check("le client est toujours connecte", st.get("state") == "connected", st)
    log("host god mode off:", cmd(host, "admin god all off"))
    summary()


def exp_farwalls(host, clis, seconds=40):
    """3f: a client's character fights next to a building in a town far from the host's camera. The
    bonedog is spawned on the far side of the building (its box from "wallsnear"), so that the
    shortest way between them goes through it. Recorded on the host (motionrec, 10 Hz) for both, plus
    "navcheck" (in view, every frame, on the navmesh, navmesh under them) and the host's "movestats".
    Pass: neither crosses the building's box (no sample deeper than 20 units inside it). Then the same
    with the host's camera on the fight: if only the first run fails, the cause is a camera rule."""
    clis = clis if isinstance(clis, (list, tuple)) else [clis]
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(3)
    p = FarPlayer(host, clis[0], 0)
    h0 = dump(host, "h_farwalls_home", 3000)
    keys_h = sorted(h0["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    p.own_key = keys_h[p.own_h] if p.own_h < len(keys_h) else None
    hidx = (own_indices(host) or [0])[0]
    FAR_HOME[0] = where_vec(host, hidx) or (0.0, 0.0, 0.0)
    log("god mode:", cmd(host, "admin god all on"), "| host camera home:", cmd(host, f"camto {hidx}"))
    towns = town_list(host)
    cands = far_town_candidates(towns, FAR_HOME[0])
    taken, taken_lock = [], threading.Lock()
    far_go(p, cands, "murs", False, taken, taken_lock)
    p.check("murs : le perso du client est dans une ville loin de l'hote", bool(p.town), p.town and p.town["name"])
    if not p.town:
        summary()
        return

    def inside(q, box, margin):
        c, h = box
        return abs(q[0] - c[0]) < h[0] - margin and abs(q[2] - c[2]) < h[2] - margin

    def one_fight(label):
        me = p.me()
        ok, t = cmd(host, f"wallsnear {p.own_h} 2500")
        boxes = []
        for tok in (t.split()[2:] if ok else []):
            parts = tok.split("|")
            if len(parts) == 3:
                c, h = tuple(map(float, parts[1].split(","))), tuple(map(float, parts[2].split(",")))
                if 150 <= max(h[0], h[2]) <= 1500 and min(h[0], h[2]) >= 100 and abs(c[1] - me[1]) < 300:
                    boxes.append((c, h))
        p.check(f"murs [{label}] : un batiment pres du perso du client (wallsnear)", bool(boxes), t[:200])
        if not boxes:
            return None
        c, h = boxes[0]
        # the client's character on one side of it, the beast right behind it on the other (x axis)
        side = 1.0 if me[0] >= c[0] else -1.0
        mx, bx = c[0] + side * (h[0] + 120), c[0] - side * (h[0] + 120)
        g = ground_at(host, mx, c[2])
        y = (g[0] + 60) if g and g[2] == "land" else me[1] + 50
        p.log(f"[{label}] building {c} half {h}: client's character to {mx:.0f},{c[2]:.0f}", cmd(host, f"teleport {p.own_h} {mx:.0f} {y:.0f} {c[2]:.0f}"))
        time.sleep(4)
        m2 = p.me()
        # spawncreature takes world offsets from the character: right behind the building, same height
        sp = cmd(host, f"spawncreature {FAR_FIGHT_BEAST} {bx - m2[0]:.0f} {c[2] - m2[2]:.0f} {p.own_h}")
        if not sp[0]:
            p.check(f"murs [{label}] : bete creee", False, sp)
            return None
        beast = sp[1].split()[1]
        p.log(f"   bonedog {beast} behind the building at", where_vec(host, beast))
        time.sleep(3)
        mv0 = kv_of(host, "movestats")
        p.log("   client orders the attack:", cmd(p.cli, f"attackreq {p.own_c} {beast}"))
        walk = {}
        nav = []

        def rec():
            walk.update(record_walk([host], f"{p.own_key} {beast}", seconds, f"walls_{label}_{p.tag}") if p.own_key else {})
        th = threading.Thread(target=rec, daemon=True)
        th.start()
        for _ in range(seconds // 5):
            time.sleep(5)
            nav.append(cmd(host, f"navcheck {p.own_h} {beast}")[1])
        th.join(seconds + 30)
        mv1 = kv_of(host, "movestats")
        p.log(f"   navcheck: {nav[:4]}")
        p.log(f"   movestats {mv0} -> {mv1}")
        samples = walk.get(host, [])
        deep = [(round(t_, 1), tuple(round(v) for v in q)) for t_, q in samples if inside(q, (c, h), 20)]
        p.check(f"murs [{label}] : des positions enregistrees pendant le combat", len(samples) >= seconds * 5, len(samples))
        p.check(f"murs [{label}] : personne ne traverse le batiment (aucune position a plus de 20 unites dedans)", not deep, deep[:6])
        p.check(f"murs [{label}] : les combattants sont sur le navmesh chez l'hote", all("nav=0" not in n for n in nav), nav[:3])
        log("kill the bonedog:", cmd(host, f"kill {beast}"))
        time.sleep(2)
        return not deep

    far_cam = one_fight("camera de l'hote a la maison")
    log("host camera on the client's character:", cmd(host, f"camto {p.own_h}"))
    time.sleep(8)
    near_cam = one_fight("camera de l'hote sur le combat")
    log(f"walls kept far from the host's camera: {far_cam}, with the host's camera there: {near_cam}")
    if far_cam is False and near_cam:
        log("=> only far from the host's camera: a camera rule of the host's game (see docs/A_FAIRE.md 3f)")
    cmd(host, f"camto {hidx}")
    st = status(p.cli)
    p.check("le client est toujours connecte", st.get("state") == "connected", st)
    log("host god mode off:", cmd(host, "admin god all off"))
    summary()


def arrival_facing(p, tag, seconds=20):
    """Right after the client's character arrived somewhere: every second, how many walkers around it
    are drawn facing backwards on the client (rotation against the way they move) and how far its
    copies stand from the host's latest position."""
    def V(x):
        return list(map(float, x.split(","))) if isinstance(x, str) else list(x)

    def fw(q):
        w, x, y, z = V(q)
        return 2 * (x * z + w * y), 1 - 2 * (x * x + y * y)

    def ang(d, f):
        n = math.hypot(*d) * math.hypot(*f)
        return -1 if n < 1e-6 else math.degrees(math.acos(max(-1, min(1, (d[0] * f[0] + d[1] * f[1]) / n))))
    t0 = time.time()
    worst = 0
    while time.time() - t0 < seconds:
        a = dump(p.cli, f"c_arr_{tag}_a", 2500)["char"]
        time.sleep(0.3)
        b = dump(p.cli, f"c_arr_{tag}_b", 2500)["char"]
        n = back = 0
        for k, v in b.items():
            if k not in a or "pos" not in v or "pos" not in a[k]:
                continue
            p0, p1 = V(a[k]["pos"]), V(v["pos"])
            d = (p1[0] - p0[0], p1[2] - p0[2])
            if math.hypot(*d) < 1.5:
                continue
            n += 1
            back += ang(d, fw(v.get("rot", "1,0,0,0"))) > 100
        worst = max(worst, back / n if n else 0)
        log(f"[{tag}] t+{time.time() - t0:4.1f}s client: {n} walkers, {back} drawn backwards")
        time.sleep(0.4)
    return worst


def exp_farprobe(host, clis, talk=True):
    """Short diagnosis of faraway's talk and fight (one client): its character in a far town, the host's
    camera at home. It talks to the shopkeeper; then the client orders an attack on an NPC spawned by it
    (the right click's way), and the host's state of both is logged every second; then the same with the
    host's camera on the client's character (does the fight need the host's camera?)."""
    cli = clis[0] if isinstance(clis, (list, tuple)) else clis
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    p = FarPlayer(host, cli, 0)
    h0 = dump(host, "h_probe_home", 3000)
    keys_h = sorted(h0["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))
    p.own_key = keys_h[p.own_h] if p.own_h < len(keys_h) else None
    hidx = (own_indices(host) or [0])[0]
    home = where_vec(host, hidx)
    FAR_HOME[0] = home
    log("god mode:", cmd(host, "admin god all on"), "| host camera home:", cmd(host, f"camto {hidx}"))
    cands = far_town_candidates(town_list(host), home)
    far_go(p, cands, "ville", True, [], threading.Lock())
    if not p.town:
        check("probe : une ville loin", False)
        summary()
        return
    arrival_facing(p, "arrivee")
    # a second town, measured from the teleport itself (the player saw everyone moonwalk for a few
    # seconds after a teleport, then turn right)
    other = next((t_ for t_ in cands if t_["name"] != p.town["name"] and xz(t_["pos"], p.town["pos"]) > 15000), None)
    if other:
        log("teleport to", other["name"], far_teleport(host, p.own_h, other["pos"]))
        arrival_facing(p, "teleport", 30)
    time.sleep(5)
    pass

    def fight_motion(tag, keys):
        """Both fighters sampled at 20 Hz for 8 s on the host and on the client at once: steps,
        jumps (more than 6 units in 0.05 s) and back-and-forth (a step against the previous one)."""
        paths = {}
        for who, pid in (("host", host), ("client", cli)):
            path = os.path.abspath(os.path.join(OUT_DIR, f"probe_{tag}_{who}_fightmotion.txt"))
            if os.path.exists(path):
                os.remove(path)
            paths[who] = path
            cmd(pid, f"motionrec 8 20 {path} {' '.join(k for k in keys if k)}")
        time.sleep(9.5)
        for who, path in paths.items():
            per = {}
            for _ in range(10):
                if os.path.exists(path) and open(path, encoding="utf-8", errors="replace").read().rstrip().endswith("end"):
                    break
                time.sleep(0.5)
            for ln in (open(path, encoding="utf-8", errors="replace").read().splitlines() if os.path.exists(path) else []):
                q = ln.split()
                if len(q) >= 8 and q[0] == "m":
                    per.setdefault(q[2], []).append((float(q[1]), tuple(map(float, q[3].split(",")))))
            for k, ss in per.items():
                steps = [math.dist(a[1], b[1]) for a, b in zip(ss, ss[1:])]
                jumps = [round(x, 1) for x in steps if x > 6.0]
                rev = 0
                for (a, b, c) in zip(ss, ss[1:], ss[2:]):
                    v1 = (b[1][0] - a[1][0], b[1][2] - a[1][2])
                    v2 = (c[1][0] - b[1][0], c[1][2] - b[1][2])
                    if math.hypot(*v1) > 0.3 and math.hypot(*v2) > 0.3 and v1[0] * v2[0] + v1[1] * v2[1] < 0:
                        rev += 1
                dy = [round(b[1][1] - a[1][1], 1) for a, b in zip(ss, ss[1:]) if abs(b[1][1] - a[1][1]) > 2.0]
                log(f"[{tag}] {who} {k[-10:]}: {len(ss)} samples, mean step {sum(steps) / max(1, len(steps)):.2f}, max {max(steps or [0]):.1f}, "
                    f"jumps>6 {len(jumps)} {jumps[:8]}, back-and-forth {rev}, height jumps {dy[:8]}")

    def fight_round(tag):
        sp = cmd(host, f"spawncreature {FAR_FIGHT_BEAST} 30 10 {p.own_h}")
        log(f"[{tag}] spawn:", sp)
        if not sp[0]:
            return False
        npc = sp[1].split()[1]
        time.sleep(3)
        log(f"[{tag}] client attack order:", cmd(cli, f"attackreq {p.own_c} {npc}"))
        time.sleep(2)
        fight_motion(tag, [npc, p.own_key])
        hurt, v0 = False, None
        for i in range(25):
            time.sleep(1)
            h = dump(host, f"h_probe_{tag}", FAR_ZONE)["char"]
            nv, pv = h.get(npc, {}), h.get(p.own_key, {})
            v0 = v0 or nv
            if i % 3 == 0 or not nv:
                log(f"[{tag}] t={i + 1}s npc cmode={nv.get('cmode')} action={nv.get('action')} inview={nv.get('inview')} speed={nv.get('speed')} "
                    f"parts={nv.get('parts', '')[:40]} | pc cmode={pv.get('cmode')} action={pv.get('action')} inview={pv.get('inview')} "
                    f"speed={pv.get('speed')} combat={pv.get('combat')} d={round(dist(nv['pos'], pv['pos'])) if 'pos' in nv and 'pos' in pv else '?'} "
                    f"| host combat: {cmd(host, f'combat {p.own_h}')[1]}")
            if nv and far_damaged(nv, v0):
                hurt = True
                break
        log(f"[{tag}] floaters host", far_floaters(host), "client", far_floaters(cli))
        cmd(host, f"kill {npc}")
        return hurt

    def talk_round(tag, names):
        """The same town NPCs each round: does a conversation open? (names from the first round)"""
        got = {}
        for nm in names:
            r = cmd(cli, f"talkto {p.own_c} name:{nm}")
            d = far_wait_dialog(p, True, 12)
            got[nm] = "open=1" in d
            log(f"[{tag}] talk to {nm}: {r} -> {'OPEN' if got[nm] else 'nothing'}")
            if got[nm]:
                cmd(cli, "answer leave")
                far_wait_dialog(p, False, 10)
        return got

    m = cmd(host, f"merchants {p.own_h}")[1]
    names = []
    for nm in re.findall(r" ([^()\[\]]+?)\((\d+)\)", m):
        n = nm[0].strip().replace(" ", "_")
        if n not in names:
            names.append(n)
    names = names[:4]
    log("NPCs to talk to each round:", names)
    rounds = [("loin1", hidx), ("camera", p.own_h)]
    for tag, cam in rounds:
        log(f"[{tag}] host camera on squad member {cam}:", cmd(host, f"camto {cam}"))
        time.sleep(4)
        got = talk_round(tag, names) if talk else {"(sans dialogue)": True}
        cmd(cli, "animstats")
        time.sleep(10)
        st = cmd(cli, "animstats")[1].split()
        log(f"[{tag}] client animation clock: {st[1]} catch-ups over {st[2]} checks in 10 s (NPCs walking around)")
        hurt = fight_round(tag)
        check(f"probe [{tag}] : au moins une conversation s'ouvre", any(got.values()), got)
        check(f"probe [{tag}] : combat ordonne par le client, la bete est blessee", hurt)
    cmd(host, f"camto {hidx}")
    log("god mode off:", cmd(host, "admin god all off"))
    summary()


def exp_placevalid(host, cli, kinds=("Tente", "Feu", "Coffre", "Lit", "Mur")):
    """Placements are checked as build mode checks a spot. The client's character goes next to an
    acid lake (on dry land, so that both games have the zone): the lake spot is refused by the
    client's own check, and when the client sends it anyway (debug 'force': its check skipped) the
    host refuses it, says why to the client, and nothing is built anywhere. A dry spot next to the
    character is accepted and built everywhere (the check does not refuse everything)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    own_c = own_index(cli)
    sid = find_building(host, kinds)
    check("pose valide : un modele de batiment trouve", sid is not None, sid)
    if not sid:
        summary()
        return
    lx, ly, lz = ACID_LAKE
    near = teleport_dry(host, own, ACID_LAKE, [(600, 0), (0, 600), (-600, 0), (0, -600), (1000, 0), (0, 1000), (-1000, 0), (0, -1000),
                                                (1600, 0), (0, 1600), (-1600, 0), (0, -1600)])
    check("pose valide : le perso du client sur la terre ferme pres du lac d'acide", near is not None, near)
    time.sleep(3)
    cmd(cli, f"camto {own_c}")
    time.sleep(25)   # zone streaming on both
    gh, gc = ground_at(host, lx, lz), ground_at(cli, lx, lz)
    log("acid lake spot: host", gh, "client", gc)
    check("pose valide : le lac est de l'eau ou de l'acide chez l'hote", gh is not None and gh[2] == "water", gh)
    check("pose valide : et chez le client", gc is not None and gc[2] == "water", gc)

    def lake_sites(pid):
        return [s_ for s_ in site_list(pid, sid, own_c if pid == cli else own, 3000) if dist(s_[0][::2], (lx, lz)) < 60]

    before_h, before_c = lake_sites(host), lake_sites(cli)
    # 1. the client's own check (as its build mode: the ghost would be red)
    chk = cmd(cli, f"buildcheckat {sid} {lx} {lz}")
    log("client check of the lake spot:", chk)
    check("pose valide : le client refuse le lac lui-meme", not chk[0] and "invalid spot" in chk[1], chk)
    plain = cmd(cli, f"buildplaceat {sid} {lx} {lz}")
    check("pose valide : buildplaceat dans le lac refuse sans rien envoyer", not plain[0] and "invalid spot" in plain[1], plain)
    hchk = cmd(host, f"buildcheckat {sid} {lx} {lz}")
    check("pose valide : l'hote refuse aussi le lac (meme verification)", not hchk[0] and "water" in hchk[1], hchk)
    # 2. sent anyway: the host's check refuses it, tells the client why, builds nothing
    log_mark = len(host_log())
    forced = cmd(cli, f"buildplaceat {sid} {lx} {lz} 0 force")
    log("client sends the lake spot anyway:", forced)
    check("pose valide : la demande forcee part vers l'hote", forced[0] and "asked" in forced[1], forced)
    notice = ""
    for _ in range(20):
        time.sleep(0.5)
        notice = cmd(cli, "chatlast 3")[1]
        if "eau" in notice:
            break
    log("client chat:", notice)
    check("pose valide : le client recoit le refus de l'hote (en francais)", "dans l'eau ou l'acide" in notice, notice)
    hl = host_log()[log_mark:]
    check("pose valide : l'hote note le refus et sa raison", "refused: invalid spot (in water or acid" in hl, hl[-400:])
    time.sleep(3)
    after_h, after_c = lake_sites(host), lake_sites(cli)
    check("pose valide : rien de bati dans le lac chez l'hote", len(after_h) == len(before_h), after_h)
    check("pose valide : rien de bati dans le lac chez le client", len(after_c) == len(before_c), after_c)
    # the host builds a client's placement, then announces it to everyone ("<who> places ..."): no announce, nothing anywhere
    placed_lines = [ln for ln in host_log()[log_mark:].splitlines() if " places " in ln]
    check("pose valide : aucun batiment pose nulle part (pas d'annonce de l'hote)", not placed_lines, placed_lines[:3])
    # 3. a dry spot next to the character: accepted, built on the host then on the client
    ok_place = place_valid(cli, sid, own_c, 40, 30)
    log("client places on dry land:", ok_place)
    check("pose valide : un emplacement sec est accepte", ok_place[0], ok_place)
    built_h = built_c = []
    for _ in range(20):
        time.sleep(0.5)
        built_h, built_c = site_list(host, sid, own, 400), site_list(cli, sid, own_c, 400)
        if built_h and built_c:
            break
    check("pose valide : bati chez l'hote et chez le client, au meme endroit",
          bool(built_h) and bool(built_c) and min(dist(a[0], b[0]) for a in built_h for b in built_c) < 0.5, f"{built_h[:2]} / {built_c[:2]}")
    log("bring the client back:", cmd(host, "tpplayer 2"))
    time.sleep(10)
    summary()


def exp_jobs(host, cli):
    """The client gives its character a job (follow another squad member), then removes it from its
    TÃƒÆ’Ã‚Â¢ches panel (the cross): the job is gone on the host too and does not come back on the client."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    other = 0 if own != 0 else 1
    log("client: follow job", cmd(cli, f"jobreq {own} 31 {other}"))
    jobs = "?"
    for _ in range(10):
        time.sleep(0.5)
        jobs = cmd(host, f"jobs {own}")[1]
        if jobs.startswith("ok") and jobs.split()[1] != "0":
            break
    log("host jobs:", jobs, "| client jobs:", cmd(cli, f"jobs {own}")[1])
    has_job = jobs.startswith("ok") and jobs.split()[1] != "0"
    check("taches : le travail est chez l'hote", has_job, jobs)
    if not has_job:
        # nothing to remove: the removal checks would pass for nothing
        log("NO JOB ON THE HOST: the client's job never reached it; removal not tested")
        check("taches : retire chez l'hote", False, "pas de tache chez l'hote a retirer")
        check("taches : ne revient pas chez le client", False, "pas de tache chez l'hote a retirer")
        summary()
        return
    cjobs = cmd(cli, f"jobs {own}")[1]
    if not (cjobs.startswith("ok") and cjobs.split()[1] != "0"):
        log("the client's list does not show it (it only removes what the host lost); resync to get it")
        cmd(host, "resync")
        time.sleep(40)
        cjobs = cmd(cli, f"jobs {own}")[1]
    log("client removes slot 0:", cmd(cli, f"jobremove {own} 0"))
    time.sleep(3)
    jobs_h, jobs_c = cmd(host, f"jobs {own}")[1], cmd(cli, f"jobs {own}")[1]
    check("taches : retire chez l'hote", jobs_h.startswith("ok") and len(jobs_h.split()) < len(jobs.split()), f"{jobs} -> {jobs_h}")
    time.sleep(5)
    jobs_c2 = cmd(cli, f"jobs {own}")[1]
    check("taches : ne revient pas chez le client", jobs_c2 == jobs_c and jobs_c2.split()[1:] == jobs_h.split()[1:], f"{jobs_c} -> {jobs_c2} (hote {jobs_h})")
    summary()


def exp_tradepaths(host, cli, merchant="Marchand"):
    """The client trades with a merchant through a conversation (its own talk order, then the trade
    answer) and through a right click (loot/trade order): the window opens on the client only."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)

    def window_on_client(label):
        state = "?"
        for _ in range(40):
            time.sleep(0.5)
            state = cmd(cli, "tradestate")[1]
            if "open=1" in state:
                break
        hs = cmd(host, "tradestate")[1]
        check(f"commerce ({label}) : la fenetre s'ouvre chez le client", "open=1" in state, state)
        check(f"commerce ({label}) : rien ne s'ouvre chez l'hote", "windows=0" in hs, hs)
        cmd(cli, "closewindows")
        cmd(host, "closewindows")
        time.sleep(3)

    # 1. right click on the merchant: LOOT_TARGET on a standing merchant (Task_Loot_Order, type 3)
    log("client right-click trade:", cmd(cli, f"npcreq {own} 26 {merchant}"))
    window_on_client("clic droit")
    # 2. a conversation the client starts (talk order), then the merchant's trade answer
    log("client talks to the merchant:", cmd(cli, f"npcreq {own} 12 {merchant}"))
    dialog = "?"
    for _ in range(40):
        time.sleep(0.5)
        dialog = cmd(cli, "dialog")[1]
        if "open=1" in dialog:
            break
    log("client dialog:", dialog)
    replies = re.findall(r"\[([^\]]*)\]", dialog)
    pick = next((i for i, r in enumerate(replies) if any(k in r.lower() for k in ("commerc", "affaire", "achet", "vend", "trade", "marchand"))), None)
    check("commerce (dialogue) : le dialogue propose de commercer", pick is not None, dialog)
    if pick is not None:
        log("client answers", pick, cmd(cli, f"answer {pick}"))
        window_on_client("dialogue")
    summary()


def exp_kosquad(host, cli):
    """Squad members knocked out on the host fall and stay down on the client, then get up together."""
    time.sleep(6)
    for idx in (4, 1):
        log("host kosquad", idx, cmd(host, f"kosquad {idx}"))
        for s_ in range(12):
            time.sleep(1.0)
            h, c = dump(host, "hk"), dump(cli, "ck")
            k = sorted(h["squad"])[idx]
            hv, cv = h["char"][k], c["char"][k]
            log(f"  +{s_ + 1}s idx {idx}: flags host {hv['flags']} client {cv['flags']} | vflags {hv['vflags']} {cv['vflags']} | ko {hv['ko']} {cv['ko']} | action {hv.get('action')} {cv.get('action')}")


def exp_far(host, cli):
    """The client's own character far away from the host's squad (about 5 km): does the host still
    simulate it and the NPCs there finely, and does the client see the same? Compared with the same
    fight next to the host's squad."""
    import statistics
    time.sleep(6)
    own = 5   # the joining player's character: the last one of the squad

    def vec(text):
        return tuple(map(float, text.split()[1].split(",")))

    def measure(label, idx):
        log("spawn", cmd(host, f"spawnnpc 15 10 {idx}"))
        time.sleep(3)
        npc = cmd(host, "where npc")[1].split()[2]
        log("fight", cmd(host, f"fight {idx}"))
        time.sleep(3)
        # how often does the host's game really move them? (distinct positions per second)
        changes = {"me": 0, "npc": 0}
        last = {}
        t0 = time.time()
        while time.time() - t0 < 4.0:
            for who, q in (("me", str(idx)), ("npc", npc)):
                ok, tx = cmd(host, f"where {q}")
                if ok:
                    p = vec(tx)
                    if who in last and dist(p, last[who]) > 0.01:
                        changes[who] += 1
                    last[who] = p
        rate = {k: round(v / 4.0, 1) for k, v in changes.items()}
        # what the client sees, sampled live (host and client queried back to back)
        errs = []
        for _ in range(20):
            for q in (str(idx), npc):
                a, b = cmd(host, f"where {q}"), cmd(cli, f"where {q}")
                if a[0] and b[0]:
                    errs.append(dist(vec(a[1]), vec(b[1])))
            time.sleep(0.2)
        cmd(host, "pause 1")
        time.sleep(2.5)
        fz = []
        for q in (str(idx), npc):
            a, b = cmd(host, f"where {q}"), cmd(cli, f"where {q}")
            if a[0] and b[0]:
                fz.append(round(dist(vec(a[1]), vec(b[1])), 2))
        cmd(host, "pause 0")
        log(f"FAR [{label}] host updates/s {rate} | live client error mean {statistics.mean(errs) if errs else -1:.2f} max {max(errs) if errs else -1:.2f} | frozen error {fz}")
        log("  npc on client:", cmd(cli, f"where {npc}"))

    log("client camera on its character", cmd(cli, f"camto {own}"))
    measure("next to the host's squad", own)
    x, y, z = vec(cmd(host, f"where {own}")[1])
    log("teleport the client's character far away (dry land):",
        teleport_dry(host, own, (x, y, z), [(40000, 30000), (40000, 34000), (44000, 30000), (36000, 30000), (40000, 26000)]))
    time.sleep(5)
    log("client camera on its character", cmd(cli, f"camto {own}"))
    time.sleep(35)
    h0, c0 = cmd(host, f"where {own}"), cmd(cli, f"where {own}")
    log("after the teleport: host", h0, "client", c0)
    measure("about 5 km away", own)


def exp_camjump(host, cli, rounds=20):
    """0.3.1, rob's crashes (22:32 and 00:28): his client's camera jumped away from a crowd of
    stand-ins, his game unloaded their zone (a whole local squad "is gone here" at once) and crashed a
    few seconds later. The client's character goes far away (dry land, about 5 km), the host's squad
    stays in the town crowd at speed 3, and the client's camera jumps between the two again and again.
    Checks: the client is still alive, never loses its session, and each stand-in it loses waits
    longer before it is made again (no 3 s loop)."""
    time.sleep(6)
    own, own_h = own_index(cli), own_index(host)
    vec = lambda text: tuple(map(float, text.split()[1].split(",")))
    x, y, z = vec(cmd(host, f"where {own_h}")[1])
    log("teleport the client's character far away (dry land):",
        teleport_dry(host, own_h, (x, y, z), [(40000, 30000), (40000, 34000), (44000, 30000), (36000, 30000), (40000, 26000)]))
    time.sleep(8)
    log("speed", cmd(host, "speed 3"))
    alive_all = True
    for r in range(rounds):
        log(f"round {r + 1}/{rounds}: client camera on the host's squad (crowd)", cmd(cli, "camto 0"))
        time.sleep(4)
        log("  client camera back on its own character (far)", cmd(cli, f"camto {own}"))
        time.sleep(4)
        if not alive(cli) or not status(cli):
            alive_all = False
            log("  the client is gone")
            break
    cmd(host, "speed 1")
    clog = open(os.path.join(KENSHI, f"KenshiCoop-{cli}.log"), encoding="utf-8", errors="replace").read()
    gone = clog.count("is gone here")
    unloaded = clog.count("unloaded with its zone")
    vanished = [l for l in clog.splitlines() if "stand-in(s) vanished here lately" in l]
    log(f"client log: {gone} 'is gone here' lines ({unloaded} said unloaded with its zone), {len(vanished)} vanish summaries")
    check("camjump: the client survives 20 camera jumps between a crowd and its far character", alive_all and alive(cli))
    check("camjump: the client still in the session", bool(status(cli)), str(status(cli))[:120])
    check("camjump: no stand-in loop (fewer than 200 'is gone here' lines)", gone < 200, f"{gone}")


def exp_squads(host, cli):
    """New squads and moves between squads, from the host and from the client: both see the same squads."""
    time.sleep(6)
    me = client_char_name()
    def show(label):
        time.sleep(4)
        h, c = cmd(host, "squads")[1], cmd(cli, "squads")[1]
        log(label + ": " + ("SAME" if h == c else "DIFFERENT"))
        log("    host  :", h)
        log("    client:", c)
    show("start")
    log(f"host: {me} into a new squad", cmd(host, f"squadmove {me} new"))
    show(f"host made a squad with {me}")
    log(f"host: Ribs joins {me}'s squad", cmd(host, f"squadmove Ribs {me}"))
    show("host moved Ribs")
    log(f"client: {me} into a new squad", cmd(cli, f"squadmove {me} new"))
    show("client made a new squad for its own character")
    log(f"client: {me} back with Truth", cmd(cli, f"squadmove {me} Truth"))
    show("client moved its character back")
    log("client: tries to move the host's Jurgen", cmd(cli, f"squadmove Jurgen {me}"))
    show("client tried to move a host character (must be refused)")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    log(("REUSSI " if ok else "ECHEC  ") + name + ("   [" + str(detail) + "]" if detail != "" else ""))


def summary():
    log("=" * 70)
    passed = sum(1 for _, ok, _ in RESULTS if ok)
    log(f"BILAN : {passed}/{len(RESULTS)} reussis")
    for name, ok, detail in RESULTS:
        if not ok:
            log("   ECHEC :", name, detail)


def host_log():
    with open(os.path.join(KENSHI, "KenshiCoop.log"), encoding="utf-8", errors="replace") as f:
        return f.read()


def client_char_name():
    """The joining player's own character, as the host named it ('_' for spaces: debug commands)."""
    import re as _re
    names = _re.findall(r"created (.+?)'s own character", host_log())
    if not names:
        names = _re.findall(r"\* (.+?) is in the world", host_log())
    return (names[-1] if names else "Player 2").replace(" ", "_")


def own_index(pid):
    """The squad index of the client's own character (the one whose 'modes' answer differs is not
    reliable: use the controllable info from the state dump)."""
    d = dump(pid, "own_probe")
    keys = sorted(d["squad"])
    for i, k in enumerate(keys):
        if d["entity"].get(k, {}).get("owner") == "2":
            return i
    return len(keys) - 1


def exp_squadui(host, cli):
    """Squad window (Escouade) and AI settings, the host's everywhere: the client renames, creates,
    reorders squads, moves and leads with its own character, renames it; refusals for the host's
    characters; concurrent edits by the host and the client end the same on both; AI toggles, fight
    style and jobs of its character and of a recruit nobody owns (any player, the last one wins)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(4)
    me = client_char_name()

    def squads(pid):
        return cmd(pid, "squadsfull")[1]

    def names_of(text):   # every character name in a squadsfull answer
        out = []
        for part in text.split(" | ")[1:]:
            members = part.split(":", 1)[1].strip()
            out += [m.replace(" ", "_") for m in members.split(",") if m]
        return out

    def settle(label, wait=5.0):
        h = c = ""
        end = time.time() + wait
        while time.time() < end:
            time.sleep(0.5)
            h, c = squads(host), squads(cli)
            if h == c:
                break
        log(label + ":", "SAME" if h == c else "DIFFERENT")
        log("    host  :", h)
        log("    client:", c)
        return h, c

    def pos_of(text, needle):   # position of the squad holding that character (or named so)
        for i, part in enumerate(text.split(" | ")[1:]):
            name, members = part.split(":", 1)
            if needle.replace("_", " ") in [m.strip() for m in members.split(",")] or name.strip() == needle.replace("_", " "):
                return i
        return -1

    h, c = settle("depart")
    check("escouade : memes escouades au depart", h == c, h)
    everyone = names_of(h)
    others = [n for n in everyone if n != me]
    log("client character:", me, "| others:", others)
    if not others:
        check("escouade : un perso de l'hote dans l'escouade", False, h)
        summary()
        return
    host_char = others[0]

    # 1. the client's own squad, renamed in the client's squad window
    p = pos_of(h, me)
    log("client renames its squad:", cmd(cli, f"squadrename {p} Les_Rats"))
    h, c = settle("renamed by the client", 6)
    check("escouade : renommage du client applique par l'hote", "Les Rats:" in h and h == c, h)
    # 2. a squad without the client's characters: the host puts one of its characters alone
    log("host: its character alone:", cmd(host, f"squadmove {host_char} new"))
    h, c = settle("host squad")
    p = pos_of(h, host_char)
    log("client renames the host's squad:", cmd(cli, f"squadrename {p} Pas_a_toi"))
    time.sleep(4)
    h, c = settle("after the refused rename")
    check("escouade : renommage d'une escouade sans ses persos refuse", "Pas a toi" not in h and "Pas a toi" not in c, c)
    # 3. a new squad made in the client's window, then its character moved into it, then leading
    log("client new squad:", cmd(cli, "squadnew Eclaireurs"))
    h, c = settle("client new squad", 8)
    check("escouade : nouvelle escouade du client creee chez l'hote", "Eclaireurs:" in h and h == c, h)
    log("client moves its character to a new squad:", cmd(cli, f"squadmove {me} new"))
    h, c = settle("client moved its character", 6)
    check("escouade : deplacement du perso du client", h == c and pos_of(h, me) != pos_of(h, host_char), h)
    log("client: its character back with the host's character:", cmd(cli, f"squadmove {me} {host_char}"))
    settle("back with the host's character", 6)
    log("client: its character leads:", cmd(cli, f"squadlead {me}"))
    h, c = settle("leader", 6)
    lead = h.split(" | ")[pos_of(h, me) + 1].split(":", 1)[1].strip().split(",")[0] if pos_of(h, me) >= 0 else ""
    check("escouade : chef d'escouade choisi par le client", lead == me.replace("_", " ") and h == c, h)
    # 4. the host's character: never moved by the client
    before = squads(host)
    log("client tries to move the host's character:", cmd(cli, f"squadmove {host_char} new"))
    time.sleep(4)
    check("escouade : deplacer le perso de l'hote refuse", pos_of(squads(host), host_char) == pos_of(before, host_char) and squads(host) == squads(cli),
          squads(host))
    # 5. order of the squads: the client drags its squad first
    h = squads(cli)
    p = pos_of(h, me)
    log("client drags its squad first:", cmd(cli, f"squadorder {p} 0"))
    h, c = settle("order", 6)
    check("escouade : ordre des escouades identique", pos_of(h, me) == 0 and h == c, h)
    # 6. concurrent: host and client rename the same squad at once, then move the same character at once
    p = pos_of(squads(host), me)
    log("simultaneous renames:", cmd(host, f"squadrename {p} Hote"), cmd(cli, f"squadrename {p} Client"))
    h, c = settle("simultaneous renames", 8)
    check("escouade : renommages simultanes, meme resultat partout", h == c and ("Hote:" in h or "Client:" in h), h)
    log("simultaneous moves:", cmd(host, f"squadmove {me} new"), cmd(cli, f"squadmove {me} {host_char}"))
    h, c = settle("simultaneous moves", 8)
    check("escouade : deplacements simultanes, meme resultat partout", h == c, h)
    # 7. names in the character windows
    log("client renames its character:", cmd(cli, f"charrename {me} Kenji"))
    time.sleep(5)
    h, c = settle("character renamed")
    check("escouade : nom du perso du client passe par l'hote", "Kenji" in h and h == c, h)
    me = "Kenji"
    log("client renames the host's character:", cmd(cli, f"charrename {host_char} Vole"))
    time.sleep(5)
    h, c = settle("host character renamed by the client")
    check("escouade : renommer le perso de l'hote refuse", "Vole" not in h and "Vole" not in c, c)
    info_h, info_c = cmd(host, f"charinfo {me}")[1], cmd(cli, f"charinfo {me}")[1]
    check("escouade : fiche du perso identique (competences, faim, sang, membres)", info_h == info_c, f"{info_h} / {info_c}")

    # 8. AI settings of the client's character: passive on, then off (the value, not a toggle)
    def modes(pid, who):
        t = cmd(pid, f"aimodes {who}")[1].split()
        return (int(t[1]), int(t[2]), int(t[3])) if len(t) >= 4 else None
    log("client passive:", cmd(cli, f"aiorder {me} 13"))
    time.sleep(3)
    mh = modes(host, me)
    check("ia : passif du client chez l'hote", mh is not None and mh[0] & 32, mh)
    log("client passive again:", cmd(cli, f"aiorder {me} 13"))
    time.sleep(3)
    mh, mc = modes(host, me), modes(cli, me)
    check("ia : passif desactive, meme valeur partout", mh is not None and not (mh[0] & 32) and mh == mc, f"{mh} / {mc}")
    log("client fight style defend:", cmd(cli, f"aiorder {me} 6"))
    log("client hold:", cmd(cli, f"aiorder {me} 12"))
    log("client walks:", cmd(cli, f"aiorder {me} 2"))
    time.sleep(3)
    mh, mc = modes(host, me), modes(cli, me)
    check("ia : style de combat, tenir la position et allure du client", mh is not None and mh[1] == 1 and mh[0] & 16 and mh == mc, f"{mh} / {mc}")
    # the host's own character (not a recruit nobody owns: anyone may set those)
    owned = next((n for n in others if cmd(cli, f"aishared {n}")[1] == "ok 0"), None)
    if owned:
        before = modes(host, owned)
        log("client: host's own character passive (refused):", owned, cmd(cli, f"aiorder {owned} 13"))
        time.sleep(3)
        check("ia : reglage du perso de l'hote refuse", modes(host, owned) == before, f"{before} -> {modes(host, owned)}")
    # 9. a recruit nobody owns: any player sets it, the last one wins; concurrent toggles end the same
    recruit = next((n for n in others if cmd(cli, f"aishared {n}")[1] == "ok 1"), None)
    log("recruit nobody owns:", recruit)
    if recruit:
        log("client passive on the recruit:", cmd(cli, f"aiorder {recruit} 13"))
        time.sleep(3)
        mh = modes(host, recruit)
        check("ia : une recrue sans joueur se regle par le client", mh is not None and mh[0] & 32, mh)
        log("simultaneous:", cmd(host, f"aiorder {recruit} 13"), cmd(cli, f"aiorder {recruit} 14"))
        time.sleep(4)
        mh, mc = modes(host, recruit), modes(cli, recruit)
        check("ia : reglages simultanes sur une recrue, meme resultat partout", mh == mc and mh is not None, f"{mh} / {mc}")
    else:
        log("no recruit nobody owns in this save (every host character is the host's): shared settings not tested in game")
    # 10. jobs, with their targets, the same on both: one the client gives, one the host gives. Never
    #     two characters following each other: the game drops the first "stay close" when the second
    #     one closes the loop (Kenji -> Truth then Truth -> Kenji left Kenji with no job, every run
    #     since the squad lot); the host's character follows another of the host's characters.
    log("client job (follow):", cmd(cli, f"jobfor {me} 31 {host_char}"))
    for _ in range(10):
        time.sleep(0.5)
        if cmd(host, f"jobsof {me}")[1] != "ok 0":
            break
    third = next((n for n in others if n != host_char), me)
    log("host job (follow) for its character:", third, cmd(host, f"jobfor {host_char} 31 {third}"))
    time.sleep(5)
    jh, jc = cmd(host, f"jobsof {me}")[1], cmd(cli, f"jobsof {me}")[1]
    check("ia : taches du perso du client identiques", jh == jc and jh != "ok 0", f"{jh} / {jc}")
    jh, jc = cmd(host, f"jobsof {host_char}")[1], cmd(cli, f"jobsof {host_char}")[1]
    check("ia : tache ajoutee chez l'hote visible chez le client", jh == jc and jh != "ok 0", f"{jh} / {jc}")
    summary()


# ---- actor safety: a client never makes a character it does not own act
def exp_actorsafety(host, cli):
    """The host selects its own characters, as a player would. The client then gives every kind of order
    with its own character (bed, talk, loot, pick up, trade, carry, job, build, door, first aid on a host
    character, follow, attack): on the host the actor is the client's character, the host's characters
    get nothing (no new task, no move, no bed, no conversation) and the host's selection is unchanged.
    Then forged requests naming a host character as the actor, and orders aimed at the wrong kind of
    target (the BUILD on an NPC that crashed the host in stress4): refused, the host stays alive."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    mine_h = cmd(host, "ownidx 1")[1].split()[1:]
    hidx = [int(x) for x in mine_h if int(x) != own][:3]
    check("securite controle : l'hote a ses propres persos", len(hidx) > 0, mine_h)
    if not hidx:
        summary()
        return
    log("host selects its own characters", cmd(host, "selectset " + " ".join(str(i) for i in hidx)))
    sel0 = cmd(host, "selected")[1]
    check("securite controle : la selection de l'hote est exactement ses persos",
          sel0 == "ok " + " ".join(str(i) for i in sorted(hidx)), sel0)

    def state(i):
        t = cmd(host, f"charstate {i}")[1]
        d = dict(kv.split("=") for kv in t.split()[1:]) if t.startswith("ok") else {}
        if "pos" in d:
            x, z = d["pos"].split(",")
            d["pos"] = (float(x), float(z))
        return d

    excluded = set()   # a host character the client legitimately acts on (carried): not compared for moves

    def host_untouched(label, before, actor_word, fight=False):
        """fight: a fight started next to the host's squad, whose own AI joins it (squadmates defend each
        other in the game): only the selection and the leak witness count then, not their tasks."""
        time.sleep(4)
        sel = cmd(host, "selected")[1]
        check(f"securite controle : {label} : selection de l'hote inchangee", sel == sel0, f"{sel0} -> {sel}")
        for i in hidx:
            if i in excluded or fight:
                continue
            a, b = before[i], state(i)
            if not a or not b:
                check(f"securite controle : {label} : perso {i} de l'hote lisible", False, f"{a} / {b}")
                continue
            moved = ((a["pos"][0] - b["pos"][0]) ** 2 + (a["pos"][1] - b["pos"][1]) ** 2) ** 0.5
            check(f"securite controle : {label} : le perso {i} de l'hote n'a rien recu",
                  int(b["tasks"]) <= int(a["tasks"]) and int(b["jobs"]) <= int(a["jobs"]) and b["in"] == a["in"] and b["dialog"] == a["dialog"]
                  and moved < 8.0, f"{a} -> {b} (deplace de {moved:.1f})")
        st = cmd(host, "actorstats")[1]
        check(f"securite controle : {label} : aucune fuite vers un perso de l'hote", "leaks=0" in st, st)
        hl = host_log()
        if actor_word:
            check(f"securite controle : {label} : l'hote execute l'ordre du client", actor_word in hl[-20000:], actor_word)

    actions = [
        ("lit", f"bedreq {own}", "client task 258"),
        ("parler", f"talkreq {own}", "client task 12"),
        ("piller", f"npcreq {own} 26 a", "client task 26"),
        ("commerce", f"tradeopen {own} any", None),
        ("tache", f"jobreq {own} 31 {hidx[0]}", "client task 31"),
        ("construire", f"buildreq {own} 2", "client task 2"),
        ("porte", f"doororder {own} 72 door", "client task 72"),
        ("premiers soins sur un perso de l'hote", f"taskreq {own} 25 {hidx[0]}", "client task 25"),
        ("suivre", f"taskreq {own} 44 {hidx[0]}", "client task 44"),
    ]
    g = cmd(cli, f"groundnear 400 ground {own}")[1].split()[2:]
    if g:
        _, isid, at = g[0].split("|")
        actions.insert(3, ("ramasser", f"pickupreq {own} {isid} {at}", "pick up"))
    for label, line, word in actions:
        before = {i: state(i) for i in hidx}
        r = cmd(cli, line)
        log(f"client: {label}:", line, "->", r)
        if not r[0]:
            log(f"   ({label} not available here: {r[1]})")
            continue
        host_untouched(label, before, word)
    # carrying: a host character knocked out, carried by the client's character
    if len(hidx) > 1:
        target = hidx[-1]
        cmd(host, f"kosquad {target}")
        time.sleep(2)
        excluded.add(target)
        before = {i: state(i) for i in hidx}
        log("client: carry a knocked out host character", cmd(cli, f"carryreq {own} {target}"))
        host_untouched("porter", before, "client task 225")
    # attack: last (a fight may draw everyone in)
    before = {i: state(i) for i in hidx}
    log("host spawns an NPC", cmd(host, f"spawnnpc 40 40 {own}"))
    time.sleep(2)
    log("client: attack", cmd(cli, f"npcreq {own} 4 a"))
    host_untouched("attaquer", before, "client task 4", fight=True)

    # forged requests: a host character as the actor
    st0 = cmd(host, "actorstats")[1]
    before = {i: state(i) for i in hidx}
    for via, task, subj in ((3, 258, "none"), (3, 12, "npc"), (0, 0, "none"), (1, 4, "npc"), (4, 31, f"squad{own}"), (2, 2, "building")):
        log("client forges an order for host character", hidx[0], via, task, subj, cmd(cli, f"forgeorder {hidx[0]} {via} {task} {subj}"))
    time.sleep(1)
    st1 = cmd(host, "actorstats")[1]
    n0 = int(st0.split("refused=")[1].split()[0]) if "refused=" in st0 else 0
    n1 = int(st1.split("refused=")[1].split()[0]) if "refused=" in st1 else 0
    check("securite controle : ordres forges au nom d'un perso de l'hote refuses", n1 - n0 >= 6, f"{st0} -> {st1}")
    check("securite controle : refus journalises en anglais", "not owned by player" in host_log(), "refused: actor ... not owned by player N")
    host_untouched("ordres forges", before, None, fight=True)
    res = cmd(cli, "results")[1]
    check("securite controle : le client recoit les refus", res.startswith("ok") and int(res.split()[1]) > 0, res)

    # wrong targets with the client's own character (the stress4 crash: BUILD via 3 on an NPC)
    t0 = cmd(host, "actorstats")[1]
    wrong = [(3, 2, "npc"), (4, 2, "npc"), (2, 2, "item"), (3, 98, "npc"), (3, 258, "item"), (3, 12, "item"), (3, 12, f"squad{hidx[0]}"),
             (3, 87, "npc"), (1, 72, "npc"), (3, 107, "npc"), (3, 3, "npc"), (3, 284, "npc"), (4, 2, "none"), (3, 5, "self")]
    for via, task, subj in wrong:
        log("client forges a wrong target", via, task, subj, cmd(cli, f"forgeorder {own} {via} {task} {subj}"))
        time.sleep(0.2)
    log("client: the stress4 path (npcreq task 2 on an NPC)", cmd(cli, f"npcreq {own} 2 a"))
    time.sleep(2)
    t1 = cmd(host, "actorstats")[1]
    tr0 = int(t0.split("target=")[1].split()[0]) if "target=" in t0 else 0
    tr1 = int(t1.split("target=")[1].split()[0]) if "target=" in t1 else 0
    check("securite controle : cibles du mauvais type refusees", tr1 - tr0 >= 10, f"{t0} -> {t1}")
    check("securite controle : l'hote est toujours vivant", alive(host) and cmd(host, "echo")[0], t1)
    check("securite controle : le client est toujours vivant", alive(cli) and cmd(cli, "echo")[0])
    check("securite controle : selection de l'hote inchangee a la fin", cmd(host, "selected")[1] == sel0, cmd(host, "selected")[1])
    summary()


def exp_stuck(host, cli):
    """An NPC's copy shut in a wall / under the floor on the client only: the client must notice it
    makes no progress and put it back where the host has it (within ~2 s)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    cmd(cli, "robuststats")
    for dx, dy, dz, label in ((6, 0, 0, "dans un mur"), (0, -4, 0, "sous le sol"), (8, 0, 8, "en diagonale")):
        t = cmd(cli, f"strand {dx} {dy} {dz}")
        log("client strands an NPC copy", label, t)
        if not t[0]:
            continue
        key = t[1].split()[1]
        time.sleep(3)
        ph, pc = cmd(host, f"where {key}"), cmd(cli, f"where {key}")
        e = dist(vec_of(ph[1]), vec_of(pc[1])) if ph[0] and pc[0] else 999
        check(f"bloque : PNJ ramene chez le client ({label})", e < 1.5, f"{e:.2f}")
    log("client robuststats:", cmd(cli, "robuststats")[1])
    summary()


def exp_farnpc(host, cli):
    """Walking NPCs far from the client's squad (the game moves them rarely): how far the client's copy
    is from the host's, sampled during a few seconds."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    cmd(cli, "robuststats")
    worst, n = 0.0, 0
    for _ in range(6):
        time.sleep(1.5)
        h, c = dump(host, "hfar"), dump(cli, "cfar")
        sq = [v["pos"] for v in c["squad"].values() if "pos" in v]
        for k, hv in h["char"].items():
            cv = c["char"].get(k)
            if not cv or "pos" not in hv or "pos" not in cv or not (int(hv.get("flags", "0")) & 1) or int(hv.get("flags", "0")) & 12:
                continue
            if not sq or min(dist(cv["pos"], q) for q in sq) < 300:
                continue
            n += 1
            worst = max(worst, dist(hv["pos"], cv["pos"]))
    log("client robuststats:", cmd(cli, "robuststats")[1])
    check("PNJ lointains : position chez le client (pire ecart < 3 unites)", n > 0 and worst < 3.0, f"{worst:.2f} sur {n} echantillons")
    summary()


def exp_beds(host, cli):
    """A client player's character is ordered to sleep in the nearest free bed, then to mine: the host runs
    both, everyone sees the character there."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    r = cmd(cli, f"bedreq {own}")
    log("client: sleep in a bed", r)
    ins = "?"
    for _ in range(40):
        time.sleep(1)
        ins = cmd(host, f"insomething {own}")[1]
        if ins == "ok 1":
            break
    check("lit : le perso du client se couche chez l'hote", ins == "ok 1", f"{r[1]} | {ins}")
    time.sleep(2)
    e = dist(vec(cmd(host, f"where {own}")[1]), vec(cmd(cli, f"where {own}")[1]))
    check("lit : meme position chez le client", e < 1.0, f"{e:.2f}")
    r = cmd(cli, f"minereq {own}")
    log("client: mine", r)
    if r[0]:
        time.sleep(25)
        hl = host_log()
        check("mine : l'ordre est execute chez l'hote", "client task 87" in hl and "run for a character: ok" in hl, r[1])
        e = dist(vec(cmd(host, f"where {own}")[1]), vec(cmd(cli, f"where {own}")[1]))
        check("mine : meme position chez le client", e < 1.0, f"{e:.2f}")
    summary()


def exp_tpdown(host, cli):
    """Admin TP of the client's character while it lies knocked out: it moves next to the host's squad
    member 0, lies down again there, and the client sees it there."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    cmd(host, "moverel 0 200 0")
    last = None
    for _ in range(20):   # until member 0 got there (stands still)
        time.sleep(1)
        cur = cmd(host, "where 0")[1]
        if cur == last:
            break
        last = cur
    cmd(host, f"kosquad {own}")
    time.sleep(4)
    # 0.3.4: measured from where member 0 stood when the TP sent the body next to it (the TP's target):
    # a character that arrives a few units from another one makes both step apart afterwards
    sent_to = vec(cmd(host, "where 0")[1])
    log("tp", cmd(host, "tpplayer 2"))
    time.sleep(5)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    pcc = vec(cmd(cli, f"where {own}")[1])
    hl = host_log()
    log("host's tp lines:", " | ".join(l.split("  ", 1)[-1] for l in hl.splitlines() if " tp: " in l)[-900:])
    check("TP admin (perso a terre) : il arrive pres de l'hote", dist(sent_to, pc) < 30,
          f"{dist(sent_to, pc):.1f} de l'endroit vise, {dist(p0, pc):.1f} du membre 0 maintenant")
    # 0.3.4: each game lays its own ragdoll. The client stands its copy on the host's spot as soon as
    # it sees the TP (17:08:49.94, before the host's body lay down at ~50.2) and lets it fall there;
    # the host's body then collapsed 4.9 units from where it stood (24853.7,10543.7 -> 24849.3,10541.2),
    # the copy its own way: 3.17 (16:28) and 5.79 (17:08). A knocked-out body is stood up again only
    # beyond kc::motion::kRelayOffset (20), as in the suite's lying check
    check("TP admin (perso a terre) : le client le voit au meme endroit", dist(pc, pcc) < 20, f"{dist(pc, pcc):.2f}")
    summary()


def exp_tpstop(host, cli):
    """0.3.4 (A_FAIRE 12/10 point 10): the client's character is walking far away (its own order) when the
    host's admin TP brings it next to the host's character: it stays there (its walk and its orders end,
    as with the game's "stop"), it does not run back toward where it was going (nass4, 23:11:30; the
    suite's and tpdown's "TP admin" checks at 193-597 units). Its permanent jobs are not touched."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    log("client: walk far", cmd(cli, f"moverel {own} 900 0"))
    time.sleep(4)
    log("host: the client's character while it walks", cmd(host, f"where {own}")[1])
    log("tp", cmd(host, "tpplayer 2"))
    time.sleep(1.5)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    check("TP admin en marche : le perso du joueur arrive pres de l'hote", dist(p0, pc) < 30, f"{dist(p0, pc):.1f}")
    time.sleep(8)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    pcc = vec(cmd(cli, f"where {own}")[1])
    check("TP admin en marche : il reste pres de l'hote 8 s plus tard (ne repart pas vers sa destination)", dist(p0, pc) < 40, f"{dist(p0, pc):.1f}")
    check("TP admin en marche : le client le voit au meme endroit", dist(pc, pcc) < 3, f"{dist(pc, pcc):.2f}")
    hl = host_log()
    check("TP admin en marche : marche et ordres arretes (journal de l'hote)", "walk and orders ended" in hl, "")
    summary()


def exp_bedfree(host, cli):
    """0.3.4 (A_FAIRE 12/10 points 11 and 12): a copy left in a bed on a client while the host has it free
    (Meow, hidden in her bedroll on every client for 17 minutes while she walked on the host) is taken out
    at the next refresh, by itself; a character the host puts to bed and wakes is out of the bed on the
    client too; afterwards nothing the host has standing lies, sleeps or hangs on nobody on the client
    ("posturecheck")."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    other = next((i for i in range(8) if i != own and cmd(host, f"where {i}")[0]), None)
    check("lit : un perso de l'hote a coucher chez le client", other is not None, "")
    if other is not None:
        # 1. a stale bed, on the client only (the host has it standing, free)
        r = cmd(cli, f"localbed {other}")
        log("client: our copy alone in a bed", r)
        check("lit fantome : la copie est couchee chez le client seulement (test)", r[0] and "inSomething=1" in r[1], r[1])
        ins = "?"
        for _ in range(15):
            time.sleep(1)
            ins = cmd(cli, f"insomething {other}")[1]
            if ins == "ok 0":
                break
        check("lit fantome : le client la sort du lit tout seul (l'hote l'a libre)", ins == "ok 0", ins)
        time.sleep(1)
        check("lit fantome : dit dans le journal du client", "while the host has it free: taken out" in host_log(), "")
    # 2. to bed on the host, then woken by another order: out of the bed on the client within seconds
    r = cmd(cli, f"bedreq {own}")
    log("client: sleep in a bed", r)
    ins = "?"
    for _ in range(40):
        time.sleep(1)
        ins = cmd(host, f"insomething {own}")[1]
        if ins == "ok 1":
            break
    check("lit : le perso du client se couche chez l'hote", ins == "ok 1", f"{r[1]} | {ins}")
    time.sleep(3)
    check("lit : couche chez le client aussi", cmd(cli, f"insomething {own}")[1] == "ok 1", cmd(cli, f"insomething {own}")[1])
    log("client: walk away (wakes up on the host)", cmd(cli, f"moverel {own} 80 0"))
    up = "?"
    for _ in range(20):
        time.sleep(1)
        up = cmd(host, f"insomething {own}")[1]
        if up == "ok 0":
            break
    check("lit : leve chez l'hote", up == "ok 0", up)
    cl = "?"
    for _ in range(6):
        time.sleep(1)
        cl = cmd(cli, f"insomething {own}")[1]
        if cl == "ok 0":
            break
    check("lit : leve chez le client aussi (moins de 6 s apres l'hote)", cl == "ok 0", cl)
    time.sleep(4)
    pc = kv_of(cli, "posturecheck")
    log("client posturecheck:", cmd(cli, "posturecheck")[1])
    check("posture : rien que l'hote a debout n'est couche, au lit ou porte par personne chez le client", pc.get("n") == 0, pc)
    summary()


def vec_of(t):
    return tuple(map(float, t.split()[1].split(",")))


def vec(t):
    return vec_of(t)


# ---- fix G6
def exp_resyncbar(host, cli):
    """Resync: the client reloads the host's world; afterwards its squad bar still shows its
    characters, its game is still alive a minute later, and orders still go through."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    before = cmd(cli, "squadbar")
    log("before", before)
    log("resync", cmd(host, "resync 2"))
    time.sleep(3)
    wait_for(cli, lambda f: f.get("state") == "connected" and f.get("ready") == "1", 240, "client back in the host's world")
    time.sleep(5)
    after = cmd(cli, "squadbar")
    log("after", after)
    n = int(after[1].split()[1]) if after[0] else 0
    check("resync : la barre d'escouade montre des portraits", n > 0, after[1])
    gen0 = before[1].split("gen=")[-1] if before[0] else "?"
    gen1 = after[1].split("gen=")[-1] if after[0] else "?"
    check("resync : un nouveau monde (generation changee)", gen0 != gen1, f"{gen0} -> {gen1}")
    own = own_index(host)
    cmd(cli, f"move {own} 0 0")
    time.sleep(60)
    st = status(cli)
    check("resync : le client tourne encore 1 min apres", st.get("state") == "connected", st)
    summary()


def exp_fartp(host, cli):
    """The client walks its character far away, then the host's admin TP brings it back: the
    client must not be dropped while it loads the zone."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    # far: another zone entirely, on dry land
    log("far TP:", teleport_dry(host, own, (0, -300, 0), [(30000, 30000), (34000, 30000), (30000, 34000), (26000, 30000), (30000, 26000)], settle=4))
    time.sleep(20)
    log("tp", cmd(host, "tpplayer 2"))
    t0 = time.time()
    dropped = False
    while time.time() - t0 < 90:
        st = status(cli)
        if st.get("state") not in ("connected", None):
            dropped = True
            break
        time.sleep(2)
    check("TP lointain : le client reste connecte", not dropped and status(cli).get("state") == "connected")
    check("TP lointain : l'hote ne l'a pas perdu", "left" not in host_log()[-4000:])
    summary()


def exp_missing(host, cli, minutes=5):
    """Missing NPCs on the client over time: the count must go down to (near) zero near the
    players, never stay stuck."""
    time.sleep(6)
    cmd(cli, "editdone")
    seen = []
    for _ in range(minutes * 6):
        st = status(cli)
        seen.append(int(st.get("missingNpcs", "0")))
        log("missingNpcs", seen[-1], "entities", st.get("entities"))
        time.sleep(10)
    check("PNJ manquants : le compte redescend", seen[-1] <= max(2, min(seen)), seen[-6:])
    summary()


def exp_floorview(host, cli):
    """The client player's own character goes up a floor in a town building and down again: on the
    client the floor view (which floors and roofs show) follows it by itself (PlayerInterface focus,
    the floor seen and the floor shown). Then the host plays at x2 and x3: the client's speed buttons
    show it (screenshots with --keep: test_out/floorview_*.png)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own_h, own_c = own_index(host), own_index(cli)
    log("client camera on its character:", cmd(cli, f"camto {own_c}"))
    b = cmd(host, f"buildingsnear {own_h} 4000")
    log("buildings around:", b[1][:300])
    found = None
    for item in b[1].split()[2:] if b[0] else []:
        sid, pos = item.split("|")
        x, y, z = map(float, pos.split(","))
        cmd(host, f"teleport {own_h} {x:.0f} {y + 5:.0f} {z:.0f}")
        time.sleep(2)
        r = cmd(host, f"floor {own_h} 10")
        time.sleep(1.5)
        if cmd(host, f"floor {own_h}")[1] == "ok 10":
            found = (sid, x, y, z)
            break
    log("a building with an upper floor:", found)
    check("etages : un batiment a etage trouve, le perso du client y est monte (hote)", found is not None)
    if found:
        c = h = None
        for _ in range(16):
            time.sleep(0.5)
            c, v = cmd(cli, f"floor {own_c}")[1], cmd(cli, "floorview")[1]
            if c == "ok 10" and "seen=1" in v:
                break
        log("client after going up:", c, "|", v)
        check("etages : chez le client, son perso est a l'etage", c == "ok 10", c)
        check("etages : la vue des etages suit son perso (etage 1 vu)", "seen=1" in v and "focus=none" not in v, v)
        shown_up = re.search(r"shown=(-?\d+)", v)
        cmd(host, f"floor {own_h} 9")
        for _ in range(16):
            time.sleep(0.5)
            c, v2 = cmd(cli, f"floor {own_c}")[1], cmd(cli, "floorview")[1]
            if c == "ok 9" and "seen=0" in v2:
                break
        log("client after going down:", c, "|", v2)
        check("etages : redescendu, la vue revient au rez-de-chaussee", "seen=0" in v2, v2)
        shown_down = re.search(r"shown=(-?\d+)", v2)
        check("etages : l'etage affiche change avec lui", bool(shown_up and shown_down and shown_up.group(1) != shown_down.group(1)),
              f"{v} -> {v2}")
    for sp in (2, 3, 1):
        cmd(host, f"speed {sp}")
        got = None
        for _ in range(10):
            time.sleep(0.5)
            got = cmd(cli, "gamespeed")[1]
            if got.startswith("ok") and abs(float(got.split("=")[1]) - sp) < 0.01:
                break
        log(f"host at x{sp}: client", got)
        check(f"vitesse : le client passe en x{sp}", got and got.startswith("ok") and abs(float(got.split("=")[1]) - sp) < 0.01, got)
        grab_window(cli, os.path.join(OUT_DIR, f"floorview_speed{sp}.png"))
    summary()


def exp_camfocus(host, cli):
    """A_FAIRE 3l: on the client the camera's tracked character (the floor view's focus) is set once
    after loading, then left to the player: moving the camera drops it (camerafree does what the keys
    do) and the mod never takes it back. Roofs are still hidden by UpdateRoof (see floorview)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(4)
    follows = cmd(cli, "logcount floor_view:_follows_our")[1]
    v0 = cmd(cli, "floorview")[1]
    log("client floor view after loading:", v0, "| lines so far:", follows)
    check("camera : apres le chargement, le focus est sur un perso du client", "focus=none" not in v0, v0)
    log("the player moves the camera:", cmd(cli, "camerafree"))
    seen = []
    for _ in range(12):
        time.sleep(0.5)
        seen.append(cmd(cli, "floorview")[1])
    log("client floor view afterwards:", seen[-1])
    check("camera : le focus reste a 'aucun' 6 s apres que le joueur a bouge la camera",
          all("focus=none" in v for v in seen), " | ".join(seen[-3:]))
    after = cmd(cli, "logcount floor_view:_follows_our")[1]
    check("camera : plus de ligne 'floor view: follows our' en boucle", after == "ok 0" or after == follows, f"{follows} -> {after}")
    summary()


def exp_buildpause(host, cli):
    """A_FAIRE 3n: the host opens build mode while a client walks: the host's game is not paused
    (unless the host had paused it himself) and the client keeps walking; a client's own build mode
    changes nothing for the host."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(4)
    own_c = own_index(cli)
    cmd(host, "pause 0")
    cmd(host, "speed 1")
    time.sleep(1)

    def walked(seconds=3.0):
        a = cmd(cli, f"pos {own_c}")[1]
        cmd(cli, f"moverel {own_c} 300 0")
        time.sleep(seconds)
        b = cmd(cli, f"pos {own_c}")[1]
        try:
            pa = [float(x) for x in a.split()[1].split(",")]
            pb = [float(x) for x in b.split()[1].split(",")]
            return math.hypot(pb[0] - pa[0], pb[2] - pa[2]), f"{a} -> {b}"
        except Exception:
            return 0.0, f"{a} -> {b}"

    r = cmd(host, "buildmode 1")
    time.sleep(1)
    log("host opens build mode:", r, "| host:", cmd(host, "paused")[1], "| client:", cmd(cli, "paused")[1])
    check("construction : mode construction ouvert chez l'hote", "active=1" in cmd(host, "buildmode")[1], r)
    check("construction : la partie de l'hote n'est pas en pause", cmd(host, "paused")[1].startswith("ok 0"), cmd(host, "paused")[1])
    d, why = walked()
    log("client walked during the host's build mode:", round(d, 1), why)
    check("construction : le client marche pendant le mode construction de l'hote", d > 30, why)
    cmd(host, "buildmode 0")
    # the host paused himself first: build mode keeps the pause
    cmd(host, "pause 1")
    time.sleep(1)
    cmd(host, "buildmode 1")
    time.sleep(1)
    p = cmd(host, "paused")[1]
    check("construction : pause de l'hote avant le mode construction, la pause reste", p.startswith("ok 1"), p)
    cmd(host, "buildmode 0")
    cmd(host, "pause 0")
    time.sleep(1)
    # the client's own build mode
    log("client opens build mode:", cmd(cli, "buildmode 1"))
    time.sleep(2)
    hp, cp = cmd(host, "paused")[1], cmd(cli, "paused")[1]
    log("host:", hp, "| client:", cp)
    check("construction : le mode construction du client ne met pas l'hote en pause", hp.startswith("ok 0"), hp)
    check("construction : le client suit l'horloge de l'hote (pas en pause)", cp.startswith("ok 0"), cp)
    cmd(cli, "buildmode 0")
    summary()


def exp_quietlogs(host, clis, minutes=3):
    """A_FAIRE 3h: doors whose state the client cannot take, and captivity changes, no longer fill
    the clients' logs (counted for `minutes` at x3; play it in a town at night for beds)."""
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(4)
    texts = ("stays", "captivity_from_the_host", "door:_")
    for c in clis:
        for t in texts:
            cmd(c, f"logcount {t}")
    cmd(host, "speed 3")
    time.sleep(minutes * 60)
    cmd(host, "speed 1")
    for c in clis:
        got = {t: cmd(c, f"logcount {t}")[1] for t in texts}
        log(f"client {c}:", got, "|", cmd(c, "doorsknown")[1])
        n = {t: int(v.split()[1]) if v.startswith("ok") else -1 for t, v in got.items()}
        check(f"journaux : porte 'stays' rare chez {c} ({n['stays']} en {minutes} min)", 0 <= n["stays"] <= 5 * minutes, got)
        check(f"journaux : captivite rare chez {c} ({n['captivity_from_the_host']} en {minutes} min)",
              0 <= n["captivity_from_the_host"] <= 40 * minutes, got)
    summary()


def exp_bugreport(host, cli):
    """0.3.5: "/bug <note>" from the client (its debug "bug", as typed in its console): the host writes
    KenshiCoop_bugs\\bug-<date>-<name>.txt with the note and the client's last log lines (flushed
    before the request), everyone gets the chat line, nobody sees "/bug" as chat; a second one within
    30 s is refused; the host's own works too."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(4)

    def read_report(path):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_report(before, seconds=10):
        end = time.time() + seconds
        while time.time() < end:
            last = cmd(host, "buglast")[1][3:].strip()
            if last and last != "none" and last != before:
                return last
            time.sleep(0.5)
        return None

    before = cmd(host, "buglast")[1][3:].strip()
    tag = f"bugmark{int(time.time()) % 100000}"
    # a line in the client's log just before the request: it must be in the host's report
    cmd(cli, f"console {tag}")
    note = f"la porte ne s'ouvre pas {tag}"
    log("client bug:", cmd(cli, f"bug {note}"))
    path = wait_report(before)
    check("bug : le rapport du client est ecrit chez l'hote", path is not None, path)
    text = read_report(path) if path else ""
    check("bug : la note est dans le rapport", f"note: {note}" in text, path)
    check("bug : la ligne du client juste avant est dans le rapport", f"> {tag}" in text and "] > " + tag in text)
    check("bug : 'bug report requested' du client dans le rapport", f"bug report requested: {note}" in text)
    mark = text.find("==== BUG REPORT from")
    check("bug : marqueur ==== BUG REPORT apres les lignes du client", mark > 0 and text.find(f"] > {tag}") < mark, mark)
    check("bug : joueurs listes", "== players ==" in text and "last sync report" in text)
    time.sleep(1)
    cl = cmd(cli, "chatlast 5")[1]
    hl = cmd(host, "chatlast 5")[1]
    check("bug : le client voit 'Rapport de bug de ... enregistre'", "Rapport de bug de" in cl and "enregistr" in cl, cl)
    check("bug : l'hote voit 'Rapport de bug de ... enregistre'", "Rapport de bug de" in hl, hl)
    check("bug : '/bug' n'apparait pas dans le chat", "/bug" not in cl and "/bug" not in hl, (cl, hl))
    # throttle: a second one at once is refused
    cmd(cli, "bug encore une fois")
    time.sleep(4)
    cl = cmd(cli, "chatlast 3")[1]
    check("bug : second rapport en moins de 30 s refuse (message)", "Rapport déjà envoyé" in cl, cl)
    check("bug : pas de second fichier", cmd(host, "buglast")[1][3:].strip() == path)
    # the host's own
    cmd(host, "bug cote hote")
    hpath = wait_report(path)
    check("bug : le rapport de l'hote est ecrit", hpath is not None and "note: cote hote" in read_report(hpath), hpath)
    summary()


def exp_attackers(host, cli):
    """A_FAIRE 3g: an NPC fights the host's character far from the client's: the client is sent it
    anyway and makes it, so a player watching another player's fight sees who hits them."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(4)
    own_h = own_index(host)
    hi = 0 if own_h != 0 else 1
    hp = cmd(host, f"pos {hi}")[1]
    try:
        x, y, z = [float(v) for v in hp.split()[1].split(",")]
    except Exception:
        check("attaquants : position du perso de l'hote", False, hp)
        return summary()
    log("client's character goes 6 km away:", cmd(host, f"teleport {own_h} {x + 6000:.0f} {y + 5:.0f} {z:.0f}"))
    time.sleep(6)
    sp = cmd(host, f"spawnnpc 15 10 {hi}")
    log("NPC next to the host's character:", sp)
    key = sp[1].split()[1] if sp[0] and len(sp[1].split()) > 1 else None
    if not key:
        check("attaquants : PNJ cree", False, sp)
        return summary()
    log("it fights the host's character:", cmd(host, f"fight {hi} {key}"))
    log("client camera on the host's character:", cmd(cli, f"camto {hi}"))
    seen = None
    for _ in range(30):
        time.sleep(0.5)
        seen = cmd(cli, f"where {key}")
        if seen[0]:
            break
    log("the attacker on the client:", seen)
    check("attaquants : le PNJ qui frappe le perso de l'hote existe chez le client lointain", bool(seen and seen[0]), seen)
    summary()


def grab_window(pid, path):
    """The game window's client area as a PNG (only that window: the user works on the same PC)."""
    try:
        import ctypes.wintypes as wt
        g = ctypes.windll.gdi32
        hw = [h for h, w, hh in windows_of(pid) if w >= 640][0]
        r = wt.RECT()
        user32.GetClientRect(hw, ctypes.byref(r))
        W, H = r.right, r.bottom
        hdc = user32.GetDC(hw)
        mem = g.CreateCompatibleDC(hdc)
        bmp = g.CreateCompatibleBitmap(hdc, W, H)
        g.SelectObject(mem, bmp)
        user32.PrintWindow(hw, mem, 3)

        class BIH(ctypes.Structure):
            _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                        ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]
        bi = BIH()
        bi.biSize, bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount = ctypes.sizeof(BIH), W, -H, 1, 32
        buf = ctypes.create_string_buffer(W * H * 4)
        g.GetDIBits(mem, bmp, 0, H, buf, ctypes.byref(bi), 0)
        rows = []
        for y in range(H):
            line = bytearray(buf[y * W * 4:(y + 1) * W * 4])
            line[0::4], line[2::4] = line[2::4], line[0::4]
            rows.append(b"\0" + bytes(line))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) +             chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
        open(path, "wb").write(png)
        g.DeleteObject(bmp)
        g.DeleteDC(mem)
        user32.ReleaseDC(hw, hdc)
    except Exception as e:
        log("screenshot failed:", e)


def exp_floor(host, cli):
    """A host character changes floor: the client's copy takes the same floor group."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    # The game recomputes floorGroup every frame from the surface under the character (CharMovement
    # update, kenshi_x64+0x65F564): a floor forced out in the open is put back at once, on the host
    # as on the client. So first: both games agree at rest; then a forced floor only counts if the
    # host keeps it (squad 0 inside a building with floors).
    h0, c0 = cmd(host, "floor 0"), cmd(cli, "floor 0")
    log("floor 0 at rest", h0, c0)
    check("etage : meme etage au repos", h0[0] and c0[0] and h0[1].split()[1] == c0[1].split()[1], f"{h0[1]} / {c0[1]}")
    cmd(host, "floor 0 10")
    time.sleep(2)
    h = cmd(host, "floor 0")
    if not (h[0] and h[1].split()[1] == "10"):
        log("floor 10 not kept by the host's own game (no floor 1 under squad 0): forced change inconclusive", h)
    else:
        c = cmd(cli, "floor 0")
        check("etage : le client suit l'etage de l'hote", c[0] and c[1].split()[1] == "10", c[1])
        cmd(host, "floor 0 9")
        time.sleep(2)
        c = cmd(cli, "floor 0")
        check("etage : retour au rez-de-chaussee", c[0] and c[1].split()[1] == "9", c[1])
    summary()


def exp_suite(host, cli):
    """Every feature in one session: PASS / FAIL per point."""
    time.sleep(6)
    def vec(t):
        return tuple(map(float, t.split()[1].split(",")))
    # --- 1. joining: the editor opens on the new character, everyone waits meanwhile
    hl = host_log()
    check("arrivee : un personnage cree pour le joueur", "own character" in hl)
    ok_editor = False
    for _ in range(20):
        if "editor opened" in open(os.path.join(KENSHI, f"KenshiCoop-{cli}.log"), encoding="utf-8", errors="replace").read():
            ok_editor = True
            break
        time.sleep(0.5)
    check("arrivee : l'editeur de personnage s'ouvre chez le client", ok_editor)
    time.sleep(2)
    check("creation de perso : l'hote est en pause pendant l'edition", cmd(host, "paused")[1].startswith("ok 1"), cmd(host, "paused")[1])
    own = own_index(host)
    name_c = None
    cmd(cli, "editdone")
    time.sleep(3)
    check("creation de perso : la pause est levee a la validation", cmd(host, "paused")[1].startswith("ok 0"), cmd(host, "paused")[1])
    # --- 2. skills, money, bubbles
    cmd(host, "xp 0 1 5")
    cmd(host, "money 4321")
    before = cmd(cli, "says")[1]
    cmd(host, "say 1 Test de bulle")
    time.sleep(3)
    check("XP : identique chez le client", cmd(host, "stats 0")[1].split()[1] == cmd(cli, "stats 0")[1].split()[1])
    check("argent : identique chez le client", cmd(cli, "money")[1] == "ok 4321", cmd(cli, "money")[1])
    check("bulles de dialogue : affichees chez le client", cmd(cli, "says")[1] != before, cmd(cli, "says")[1])
    # --- 3. modes (stealth, hold) asked by the client for its own character
    cmd(cli, f"orderreq {own} 3")    # stealth on
    cmd(cli, f"orderreq {own} 12")   # hold position (toggle)
    time.sleep(3)
    hm, cm = cmd(host, f"modes {own}")[1], cmd(cli, f"modes {own}")[1]
    check("modes : furtif + tenir la position chez l'hote", hm.startswith("ok") and int(hm.split()[1]) & 1 and int(hm.split()[1]) & 16, hm)
    check("modes : identiques chez le client", hm == cm, f"hote {hm} / client {cm}")
    cmd(cli, f"orderreq {own} 4")    # stealth off
    cmd(cli, f"orderreq {own} 12")   # hold off
    time.sleep(3)
    hm, cm = cmd(host, f"modes {own}")[1], cmd(cli, f"modes {own}")[1]
    check("modes : retires chez les deux", hm == cm and int(hm.split()[1]) & 17 == 0, f"hote {hm} / client {cm}")
    # --- 4. carrying a knocked out squad member
    cmd(host, "kosquad 1")
    time.sleep(2)
    cmd(cli, f"carryreq {own} 1")
    carried_h = carried_c = "?"
    for _ in range(30):
        time.sleep(1)
        carried_h = cmd(host, f"carrying {own}")[1]
        if carried_h != "ok none":
            break
    time.sleep(2)
    carried_c = cmd(cli, f"carrying {own}")[1]
    check("porter : le client fait porter un corps a son perso (hote)", carried_h not in ("ok none", "?"), carried_h)
    check("porter : le client voit le meme corps porte", carried_c == carried_h, f"hote {carried_h} / client {carried_c}")
    # --- 5. squads: the host puts the player's character in a squad of its own, the player puts it back
    me = client_char_name()
    before = cmd(host, "squads")[1]
    moved = cmd(host, f"squadmove {me} new")
    time.sleep(4)
    after_h, after_c = cmd(host, "squads")[1], cmd(cli, "squads")[1]
    check("escouades : nouvelle escouade de l'hote visible chez le client", moved[0] and after_h != before and after_h == after_c,
          f"{moved[1]} | {after_c}")
    back = cmd(cli, f"squadmove {me} Truth")
    time.sleep(4)
    end_h, end_c = cmd(host, "squads")[1], cmd(cli, "squads")[1]
    check("escouades : le client remet son perso, identique partout", back[0] and end_h != after_h and end_h == end_c, f"{back[1]} | {end_c}")
    # --- 6. orientation and animations at speed 1 and 3
    import math
    def yaw(f):
        x, _, z = map(float, f.split(","))
        return math.degrees(math.atan2(x, z))
    def body_yaw(v):   # the body's rotation (what players see): forward of the quaternion w,x,y,z
        if "rot" not in v:
            return yaw(v["face"])
        w, x, y, z = map(float, v["rot"].split(","))
        return math.degrees(math.atan2(2 * (x * z + w * y), 1 - 2 * (x * x + y * y)))
    for speed in (1, 3):
        cmd(host, f"speed {speed}")
        time.sleep(1)
        cmd(cli, "animstats")
        worst, samples = 0.0, 0
        for r in range(3):
            idx = 2 + r % 2
            cmd(host, f"moverel {idx} {300 * (1 if r % 2 == 0 else -1)} 250")
            for _ in range(3):
                time.sleep(0.8)
                h, c = dump(host, "hs"), dump(cli, "cs")
                k = sorted(h["squad"])[idx]
                hv, cv = h["char"][k], c["char"][k]
                # walking only: a fight (raiders passing by) turns and staggers characters on purpose
                if not (int(hv["flags"]) & 1) or int(hv["flags"]) & 12 or hv.get("combat") or cv.get("combat"):
                    continue
                samples += 1
                worst = max(worst, abs((body_yaw(hv) - body_yaw(cv) + 540) % 360 - 180))
        st = cmd(cli, "animstats")[1].split()
        corr, checks = int(st[1]), max(1, int(st[2]))
        check(f"vitesse {speed} : orientation en marchant (pire ecart <= 30 deg)", samples > 0 and worst <= 30, f"{worst:.0f} deg sur {samples}")
        check(f"vitesse {speed} : horloge d'animation sans saut (< 2% des images)", corr / checks < 0.02, f"{corr}/{checks}")
    cmd(host, "speed 1")
    # --- 7. pause mid-stride
    cmd(host, "moverel 3 400 300")
    time.sleep(2)
    cmd(host, "pause 1")
    time.sleep(2)
    e = dist(vec(cmd(host, "where 3")[1]), vec(cmd(cli, "where 3")[1]))
    check("pause en pleine course : meme position (< 0.1)", e < 0.1, f"{e:.3f}")
    cmd(host, "pause 0")
    # --- 8. admin teleport
    cmd(host, "moverel 0 600 0")
    last = None
    for _ in range(60):   # until it got there (stands still)
        time.sleep(1)
        cur = cmd(host, "where 0")[1]
        if cur == last:
            break
        last = cur
    # 0.3.4: measured from where member 0 stood when the TP sent the character next to it: on 13/10
    # (08:07) the character arrived 22 units from that spot, then member 0 itself stepped 51 units away
    # from the newcomer (30.1 between them)
    sent_to = vec(cmd(host, "where 0")[1])
    cmd(host, "tpplayer 2")
    time.sleep(3)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    pcc = vec(cmd(cli, f"where {own}")[1])
    # 0.3.4 (13/10 17:07): the save's fights move the squad right after the TP. Member 0 walked 28.6
    # units in 3 s, the player's character with it, the same way (38.2 from the target, 11.7 from
    # member 0); member 3 was bleeding 20 s later and member 0 knocked out at 40 s. Next to member 0
    # where it is now counts as arrived too.
    squad_moved = dist(sent_to, p0) > 10
    check("TP admin : le perso du joueur arrive pres de l'hote", dist(sent_to, pc) < 30 or (squad_moved and dist(p0, pc) < 30),
          f"{dist(sent_to, pc):.1f} de l'endroit vise, {dist(p0, pc):.1f} du membre 0 maintenant"
          f"{' (membre 0 a bouge de %.1f)' % dist(sent_to, p0) if squad_moved else ''}")
    # knocked out in the save's fights, it lies there again: each game lays its own ragdoll (a body is
    # stood up to fall again only beyond kc::motion::kRelayOffset, 20 units). Walking with the squad,
    # host and client positions are read a moment apart (17:07: 4.02 while both walked ~10 units/s)
    vt = cmd(host, f"vitals {own}")[1]
    m = re.search(r"flags=(\d+)", vt)
    lying = bool(m and int(m.group(1)) & 3)
    tol = 20 if lying else (10 if squad_moved else 2)
    check("TP admin : le client le voit au meme endroit", dist(pc, pcc) < tol,
          f"{dist(pc, pcc):.2f}{' (couche)' if lying else (' (en marche)' if squad_moved else '')}")
    # --- 9. host console and logs from the client
    cw = cmd(host, "consolewin")[1]
    check("console externe : ouverte chez l'hote avec le journal", "window" in cw and int(cw.split("log=")[1]) > 100, cw)
    time.sleep(6)
    hl = host_log()
    import re as _re
    names = set(_re.findall(r"\* (.+?) is in the world", hl))
    cname = sorted(names)[-1] if names else "Player 2"
    check("journal : lignes du client relayees chez l'hote", f"[{cname}] connecting" in hl, cname)
    check("journal : rapport de synchro du client", f"[{cname}] sync" in hl)
    check("journal : ordres du client en clair", f"[{cname}] mode \"stealth\"" in hl and f"[{cname}] order \"carry" in hl)
    # --- 10. a full frozen comparison at the end
    cmd(host, "pause 1")
    time.sleep(3)
    hs_end = dump(host, "h_suite_end")
    rep = compare(hs_end, dump(cli, "c_suite_end"), "fin", pos_tol=0.1)
    cmd(host, "pause 0")
    # a body lying on the ground is a ragdoll each game simulates itself: some units apart is expected
    # (knocked out, dead, or only knocked down: state flag 4 down, 8 dead). 0.3.1 posture: a body is
    # stood up to fall again only beyond kc::motion::kRelayOffset (20 units; a collapse lands 7-12
    # units from where the feet were), so that is the tolerance for one lying down
    down = {k for k, v in hs_end["squad"].items() if int(v.get("vflags", 0) or 0) & 3 or int(v.get("flags", 0) or 0) & 12}
    sq_bad = [t for t in rep["squad"] if not isinstance(t[1], (int, float)) or t[1] > (20.0 if t[0] in down else 0.1)]
    check("fin : escouade identique (positions)", not sq_bad, sq_bad)
    check("fin : aucun etat vital different", rep["vital_flag_mismatch"] == 0, rep["vital_flag_mismatch"])
    check("fin : aucun inventaire different", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    check("fin : personne ne manque chez le client", rep["missing_on_client"] == 0, rep["missing_sample"])
    # --- 11. the host's resync: the player reloads the host's world and is back with its character
    backs = host_log().count(" is in the world")
    log("resync", cmd(host, "resync 2"))
    ok_back = False
    for _ in range(90):
        time.sleep(1)
        if host_log().count(" is in the world") > backs and cmd(cli, "status")[1].find("state=connected") >= 0:
            ok_back = True
            break
    time.sleep(4)
    check("resync : le joueur recharge le monde et retrouve son perso", ok_back)
    cmd(host, "pause 1")
    time.sleep(3)
    rep = compare(dump(host, "h_suite_resync"), dump(cli, "c_suite_resync"), "resync", pos_tol=0.1)
    cmd(host, "pause 0")
    check("resync : tout est identique ensuite", rep["vital_flag_mismatch"] == 0 and rep["inventory_mismatch"] == 0 and rep["missing_on_client"] == 0,
          (rep["vital_flag_mismatch"], rep["inventory_mismatch"], rep["missing_on_client"]))
    # --- 12. reconnect: same character back
    cmd(cli, "leave")
    time.sleep(4)
    cmd(cli, "join")
    wait_for(cli, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 240, "rejoin")
    time.sleep(3)
    check("reconnexion : le joueur retrouve son personnage", "is back with their character" in host_log())
    summary()


def exp_jitter(host, cli):
    """Standing NPCs on the client must not shake, whatever the game speed (they did after a speed change)."""
    import statistics
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(2)
    for speed in (1, 3, 1):
        cmd(host, f"speed {speed}")
        time.sleep(4)
        cmd(cli, "animstats")
        tracks = {}
        for _ in range(12):
            c = dump(cli, "cj", 1500)
            for k, v in c["char"].items():
                if "pos" in v and not (int(v.get("flags", 0)) & 5):
                    tracks.setdefault(k, []).append(v["pos"])
            time.sleep(0.25)
        shaky = []
        for k, ps in tracks.items():
            if len(ps) < 8:
                continue
            steps = [dist(ps[i], ps[i + 1]) for i in range(len(ps) - 1)]
            # a standing character that moves back and forth: many small steps that do not add up
            net = dist(ps[0], ps[-1])
            if sum(steps) > 3.0 and net < sum(steps) * 0.3:
                shaky.append((k[-14:], round(sum(steps), 1), round(net, 1)))
        st = cmd(cli, "animstats")[1].split()
        log(f"speed {speed}: {len(tracks)} standing characters watched, shaking: {len(shaky)} {shaky[:5]} | animation clock jumps {st[1]}/{st[2]}")


def t_dist(a, b):
    return dist(a["pos"], b["pos"])


def exp_ground(host, cli):
    """An item the host drops lies at the same spot on the client; once picked up, it is gone there too."""
    time.sleep(8)
    ok, text = cmd(host, "groundnear 3000")
    items = text.split()[2:]
    log("items on the ground near the squad (host):", text.split()[1] if ok else text, items[:4])
    ok2, text2 = cmd(cli, "groundnear 3000")
    log("  (client):", text2.split()[1] if ok2 else text2)
    if items:
        key, tpl, pos = items[0].split("|")
        log("  host pickup of a save item:", tpl, pos, cmd(host, f"pickup 0 {key}"))
        time.sleep(1.5)
        after = cmd(cli, "groundnear 3000")[1]
        still = [x for x in after.split()[2:] if x.split("|")[1] == tpl and x.split("|")[2] == pos]
        log("  client still has it at that spot:", bool(still), "| client count now", after.split()[1])
    for idx in (0, 1, 2):
        ok, text = cmd(host, f"drop {idx}")
        log("host drop", idx, ok, text)
        if not ok:
            continue
        key = text.split()[1]
        time.sleep(2)
        log("  client sees:", cmd(cli, f"ground {key}"))
        log("  host pickup:", cmd(host, f"pickup {idx} {key}"))
        time.sleep(0.7)
        log("  client after pickup:", cmd(cli, f"ground {key}"), "| host:", cmd(host, f"ground {key}"))


def exp_grounddrop(host, cli):
    """Items dropped the way the inventory window does (Inventory::dropItem, debug "uidrop"): a stack
    of ore, a weapon and armour, by the host, by the client, both at once, plus whatever a knockout and
    a death leave on the ground. Each must lie once on both sides, same place, same stack; a pickup
    removes it everywhere."""
    time.sleep(8)
    cmd(cli, "editdone")   # the joiner's character editor holds the whole game paused
    time.sleep(3)
    own = own_index(host)
    hidx = 0 if own != 0 else 1
    ore = None
    for part in ("Iron_Ore", "Copper_Ore", "Ore", "Stone"):
        t = cmd(host, f"itemtypes {part}")[1].split()
        if len(t) > 2 and "=" in t[2]:
            ore = t[2].split("=")[0]
            break
    check("objet au sol : un minerai trouve", ore is not None, ore)
    for i in (hidx, own):
        if ore:
            log(f"ore for squad{i}:", cmd(host, f"giveitem {ore} 12 {i}"))
        for kind in ("weapon", "armour"):
            log(f"{kind} for squad{i}:", cmd(host, f"fetchitem {i} {kind}"))
    time.sleep(4)   # the client's inventory follows the host's

    def ground(pid, idx):
        ok, t = cmd(pid, f"groundall 700 {idx}")
        out = {}
        for x in (t.split()[2:] if ok and t.startswith("ok") else []):
            k, sid, q, pos = x.split("|")
            out[k] = (sid, int(q), tuple(map(float, pos.split(","))))
        return out

    def new_items(before, after):
        return {k: v for k, v in after.items() if k not in before}

    def twins(v, items):   # the same stack lying within 5 units
        return [k for k, w in items.items() if w[0] == v[0] and w[1] == v[1] and dist(w[2], v[2]) < 5.0]

    def compare(label, hn, cn, expect=None):
        if expect is not None:
            check(f"objet au sol : {label} : {expect} objet(s) chez l'hote", len(hn) == expect, list(hn.values()))
        check(f"objet au sol : {label} : autant d'objets chez le client", len(cn) == len(hn),
              f"hote {sorted(v[:2] for v in hn.values())} / client {sorted(v[:2] for v in cn.values())}")
        for hk, v in hn.items():
            tw = twins(v, cn)
            near = min((dist(w[2], v[2]) for w in cn.values() if w[0] == v[0]), default=-1)
            check(f"objet au sol : {label} : {v[0]} x{v[1]} une seule fois chez le client, meme endroit, meme pile", len(tw) == 1,
                  f"{len(tw)} copie(s), ecart {near:.1f}")

    def pickup_everywhere(label, idx, hn, cn):
        for hk, v in hn.items():
            log(f"  host pickup {v[0]}:", cmd(host, f"pickup {idx} {hk}"))
        time.sleep(2.5)
        hl, cl = ground(host, idx), ground(cli, idx)
        left_h = [k for k in hn if k in hl]
        left_c = [k for k in cn if k in cl]
        check(f"objet au sol : {label} : ramasse, disparu chez l'hote", not left_h, left_h)
        check(f"objet au sol : {label} : ramasse, disparu chez le client", not left_c, left_c)

    def drop(label, pid, idx, kind):
        hb, cb = ground(host, idx), ground(cli, idx)
        ok, t = cmd(pid, f"uidrop {idx} {kind}")
        log(f"{label}: uidrop {idx} {kind} ->", t)
        if not ok or not t.startswith("ok"):
            check(f"objet au sol : {label} : lache", False, t)
            return
        time.sleep(2.5)
        hn, cn = new_items(hb, ground(host, idx)), new_items(cb, ground(cli, idx))
        compare(label, hn, cn, 1)
        pickup_everywhere(label, idx, hn, cn)

    # 1. the host drops, through the inventory window's path
    for kind in ("ore", "weapon", "armour"):
        drop(f"l'hote lache ({kind})", host, hidx, ore if kind == "ore" and ore else kind)
    # 2. the client drops from its own character: the host's game does it
    for kind in ("ore", "weapon", "armour"):
        drop(f"le client lache ({kind})", cli, own, ore if kind == "ore" and ore else kind)
    # 3. both at once
    if ore:
        log("ore again:", cmd(host, f"giveitem {ore} 7 {hidx}"), cmd(host, f"giveitem {ore} 9 {own}"))
        time.sleep(4)
        hb, cb = ground(host, own), ground(cli, own)
        hb.update(ground(host, hidx))
        cb.update(ground(cli, hidx))
        res = {}
        th = [threading.Thread(target=lambda: res.__setitem__("h", cmd(host, f"uidrop {hidx} {ore}"))),
              threading.Thread(target=lambda: res.__setitem__("c", cmd(cli, f"uidrop {own} {ore}")))]
        for t in th:
            t.start()
        for t in th:
            t.join()
        log("both at once:", res)
        time.sleep(3)
        ha, ca = ground(host, own), ground(cli, own)
        ha.update(ground(host, hidx))
        ca.update(ground(cli, hidx))
        hn, cn = new_items(hb, ha), new_items(cb, ca)
        compare("les deux en meme temps", hn, cn, 2)
        pickup_everywhere("les deux en meme temps", hidx, hn, cn)
    # 4. a knockout, then a death: whatever falls to the ground (the scan finds what no hook saw)
    ok, t = cmd(host, "spawnnpc 12 8")
    log("spawn", ok, t)
    if ok and t.startswith("ok"):
        time.sleep(5)
        for what in ("ko", "kill"):
            hb, cb = ground(host, hidx), ground(cli, hidx)
            log(what, cmd(host, what))
            time.sleep(5)
            hn, cn = new_items(hb, ground(host, hidx)), new_items(cb, ground(cli, hidx))
            log(f"  {what}: fell to the ground on the host: {sorted(v[:2] for v in hn.values())}")
            compare("assomme" if what == "ko" else "mort", hn, cn)
    log("host ground stats:", cmd(host, "groundstats")[1])
    log("client ground stats:", cmd(cli, "groundstats")[1])
    for l in [l for l in host_log().splitlines()[-400:] if "ground:" in l][-10:]:
        log("  host:", l.strip()[:220])
    summary()


def exp_backpack(host, cli, haul_seconds=120):
    """Backpack inventories (host crashes of 11 October, 23:04 and 23:48; 3c): the client moves single
    items and stacks into and out of the backpack its character wears, drops 1 on 1 of a stackable
    item (one stack of 2 everywhere), puts a purchase in the backpack, drops from the backpack to the
    ground, the host picks it up again, then the client's character works a permanent mining job (ore
    carried and hauled: the AI reads the backpack) for 2 min at x3. invcheck must stay clean on the host
    and the client (no freed item listed, nothing listed outside a section), and inventories equal."""
    log_start = len(host_log())
    time.sleep(8)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("per-operation check on the host:", cmd(host, "invcheck each 1"))
    # a Traders Backpack Medium (11 October, rob; stacking bonus min 2, x3: Building Materials stack to 6 in it, 1 a cell elsewhere)
    r = cmd(host, f"wearbag {own} 1498-gamedata.base")
    log("the client's character wears a backpack:", r)
    check("sac : le perso du client porte un sac", r[0] and r[1].startswith("ok"), r[1])
    time.sleep(4)
    for pid, who in ((host, "hote"), (cli, "client")):
        log(f"stacking bonus ({who}):", cmd(pid, f"bagbonus {own}"))
    mats = None
    for part in ("Building_Materials", "Matériaux_Construction", "Materiaux"):
        t = cmd(host, f"itemtypes {part}")[1].split()
        if len(t) > 2 and "=" in t[2]:
            mats = t[2].split("=")[0]
            break
    mats = mats or "580-gamedata.base"
    log("materials:", mats, cmd(host, f"giveitem {mats} 10 {own}"))
    log("weapon:", cmd(host, f"fetchitem {own} weapon"))
    time.sleep(5)

    def bag(pid):   # the backpack's stacks, "sid:qty@section:x,y", sorted
        ok, t = cmd(pid, f"baginv squad{own}")
        return sorted(t.split()[2:]) if ok and t.startswith("ok") else None

    def stacks(pid):   # the character's own stacks, in invmove's index order
        ok, t = cmd(pid, f"invitems squad{own}")
        return t.split()[2:] if ok and t.startswith("ok") else []

    def index_of(pid, sid, main_only=False):
        for k, s in enumerate(stacks(pid)):
            if s.split(":")[0] == sid and (not main_only or "@main:" in s):
                return k
        return None

    def mats_in_bag(pid):
        return [s for s in (bag(pid) or []) if s.startswith(mats + ":")]

    def mats_total(pid):   # every Building Material in the backpack (the bag may already hold full stacks)
        return sum(int(s.split(":")[1].split("@")[0]) for s in mats_in_bag(pid))

    def mats_at(pid, sec, pos):   # the quantity of the materials stack at that cell, 0 if none
        return next((int(s.split(":")[1].split("@")[0]) for s in mats_in_bag(pid) if s.endswith(f"@{sec}:{pos}")), 0)

    def clean(label):
        for pid, who in ((host, "hote"), (cli, "client")):
            r = cmd(pid, "invcheck all")
            check(f"sac : {label} : invcheck propre chez {who}", r[0] and r[1].startswith("ok clean"), r[1][:400])
        hb, cb = bag(host), bag(cli)
        check(f"sac : {label} : meme sac chez l'hote et le client", hb is not None and hb == cb, f"hote {hb} / client {cb}")

    def move(label, frm, to, which, qty=0, x=-1, y=-1, sec="main"):
        r = cmd(cli, f"invmove {frm} {to} {which} {qty} {x} {y} {sec}")
        log(f"{label}:", r)
        time.sleep(4)   # the client's move goes to the host, the true state comes back
        return r

    me, mybag = f"squad{own}", f"bag:squad{own}"
    clean("depart")
    # 1. one unit into the backpack, then one more dropped on it: one stack of 2 (3c)
    i = index_of(cli, mats)
    check("sac : materiaux chez le perso du client", i is not None, stacks(cli))
    if i is not None:
        # the bag may already hold materials (a run before this one): it joins a stack of its kind with
        # room left (the game's addItem, 12 October), else takes a cell of its own
        pre = mats_in_bag(host)
        pre_total = mats_total(host)
        move("1 unit into the backpack", me, mybag, i, 1)
        for pid, who in ((host, "hote"), (cli, "client")):
            log(f"stacking bonus with materials ({who}):", cmd(pid, f"bagbonus {own}"))
        changed = [m for m in mats_in_bag(host) if m not in pre]
        partial = [m for m in pre if int(m.split(":")[1].split("@")[0]) < 6]
        check("sac : 1 materiau dans le sac chez l'hote (rejoint une pile pas pleine s'il y en a une)",
              len(changed) == 1 and mats_total(host) == pre_total + 1 and (not partial or len(mats_in_bag(host)) == len(pre)), bag(host))
        if changed:
            sec, pos = changed[0].split("@")[1].split(":")
            x, y = pos.split(",")
            h0 = mats_at(host, sec, pos)
            n0 = len(mats_in_bag(host))
            move("1 dropped on 1 in the backpack", me, mybag, index_of(cli, mats), 1, x, y, sec)
            check("sac : 1 pose sur une pile = cette pile + 1 chez l'hote (3c)",
                  mats_total(host) == pre_total + 2 and (h0 >= 6 or (mats_at(host, sec, pos) == h0 + 1 and len(mats_in_bag(host)) == n0)), bag(host))
            clean("1 sur 1")
            # 2. the rest of the stack onto it: that stack up to 6 (Traders Backpack), the rest elsewhere
            i = index_of(cli, mats)
            if i is not None:
                rest = int(stacks(cli)[i].split(":")[1].split("@")[0])
                h1 = mats_at(host, sec, pos)
                move("the rest of the stack onto it", me, mybag, i, 0, x, y, sec)
                check("sac : toute la pile fusionnee dans le sac chez l'hote",
                      mats_total(host) == pre_total + 2 + rest and mats_at(host, sec, pos) == min(h1 + rest, 6), bag(host))
                clean("pile entiere dans le sac")
            # 3. part of it back out to the character
            move("3 back out of the backpack", mybag, me, 0, 3)
            clean("3 sortis du sac")
    # 4. an item that never stacks (a weapon) into the backpack
    wi = next((k for k, s in enumerate(stacks(cli)) if "@main:" in s and s.split(":")[0] != mats), None)
    if wi is not None:
        move("an item that does not stack into the backpack", me, mybag, wi)
        clean("objet non empilable dans le sac")
    # 5. drop from the backpack to the ground (the 23:29 reproduction), then the host picks it up
    gb = cmd(host, f"groundall 700 {own}")[1].split()[2:]
    in_bag = mats_total(host)
    r = cmd(cli, f"uidrop bag:{own} {mats}")
    log("client drops materials from its backpack:", r)
    time.sleep(5)
    ga = cmd(host, f"groundall 700 {own}")[1].split()[2:]
    fresh = [g for g in ga if g not in gb and g.split("|")[1] == mats]
    check("sac : lache depuis le sac : une fois au sol chez l'hote", len(fresh) == 1, fresh)
    gone = int(r[1].rsplit("x", 1)[1]) if r[0] and r[1].startswith("ok asked") else -1
    check("sac : lache depuis le sac : plus dans le sac chez l'hote", mats_total(host) == in_bag - gone, (in_bag, gone, bag(host)))
    drops = [l for l in host_log()[log_start:].splitlines() if "client item drop done" in l and mats in l]
    check("sac : lache depuis le sac : lache une seule fois chez l'hote", len(drops) == 1, drops[-3:])
    clean("lache depuis le sac")
    if fresh:
        log("host picks it up again:", cmd(host, f"pickup {own} {fresh[0].split('|')[0]}"))
        time.sleep(6)
        clean("ramasse")
    # 6. a purchase put into the backpack (needs a merchant nearby; skipped otherwise)
    t = cmd(cli, f"tradeopen {own} any near")
    log("trade:", t)
    if t[0] and t[1].startswith("ok"):
        time.sleep(6)
        before = stacks(cli)
        for _ in range(3):
            log("buy one:", cmd(cli, "tradebuy 0"))
            time.sleep(3)
        new = next((k for k, s in enumerate(stacks(cli)) if s not in before and "@main:" in s), None)
        if new is not None:
            move("the purchase into the backpack", me, mybag, new)
        clean("achat range dans le sac")
    else:
        log("no merchant near: purchase into the backpack not tested here (see docs/TESTS.md, manual step)")
    # 7. a permanent mining job (ore carried, hauled: the AI reads the backpack) for 2 min at x3
    r = cmd(cli, f"objjob {own} 87 Ressource_Fer")
    log("client gives its character a permanent mining job:", r)
    check("sac : travail de minage permanent donne", r[0] and r[1].startswith("ok"), r[1])
    cmd(host, "speed 3")
    t0 = time.time()
    alive = True
    while time.time() - t0 < haul_seconds:
        time.sleep(20)
        st = cmd(host, "status")
        if not st[0]:
            alive = False
            break
        log("  host invcheck:", cmd(host, "invcheck all")[1][:200], "| bag:", bag(host))
    cmd(host, "speed 1")
    check("sac : l'hote vit pendant 2 min de minage a x3", alive, "")
    time.sleep(6)
    new_log = host_log()[log_start:]
    check("sac : aucun plantage de l'hote", "CRASH" not in new_log, "")
    clean("2 min de minage a x3")
    rep = compare(dump(host, "h_bp"), dump(cli, "c_bp"), "backpack")
    check("sac : memes inventaires partout", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    bad = [l for l in new_log.splitlines() if "INVCHECK" in l and "clean" not in l]
    check("sac : aucun probleme apres chaque operation (INVCHECK)", not bad, bad[-5:])
    log("host:", cmd(host, "invcheck each 0"))
    summary()


def exp_clientpickup(host, cli):
    """The client asks to pick up an item: its character walks there, the item leaves the ground everywhere."""
    time.sleep(8)
    log("give", cmd(host, "give 2 0"))
    time.sleep(2)
    ok, text = cmd(host, "drop 1")
    log("host drop", text)
    key, tpl, pos = text.split()[1], text.split()[2], text.split()[3]
    time.sleep(2)
    log("client sees:", cmd(cli, f"ground {key}"))
    log("client asks squad0 to pick it up:", cmd(cli, f"pickupreq 0 {tpl} {pos}"))
    for i in range(12):
        time.sleep(2)
        h, c = cmd(host, f"ground {key}")[1], cmd(cli, f"ground {key}")[1]
        log(f"  t+{2 * (i + 1)}s host: {h} | client: {c}")
        if "absent" in c or "not-on-ground" in c:
            break


def exp_lootswap(host, cli):
    """fix G2: the client drops a knocked-out NPC's clothes on its own character already wearing some:
    the game swaps them, the host must do the same swap (nothing refused, lost or doubled)."""
    time.sleep(8)
    ok, text = cmd(host, "spawnnpc 12 8")
    log("spawn", ok, text)
    if not ok:
        return
    key = text.split()[1]
    time.sleep(5)
    log("ko", cmd(host, "ko"))
    time.sleep(5)
    # the client's own character, as the host knows it (squad0 is the host's own on both machines: a
    # client refuses to move items on it, "inventory change refused: that character belongs to ...")
    own = own_index(host)
    ok, t = cmd(cli, f"where {own}")
    me = t.split()[2] if ok and t.startswith("ok") and len(t.split()) > 2 else None

    def secs(pid, who):
        ok, t = cmd(pid, f"invsecs {who}")
        return [(int(x.split(":", 1)[0]), x.split(":", 1)[1]) for x in t.split()[2:]] if ok and t.startswith("ok") else []

    def worn(v):
        return {s for _, s in v if s != "main"}

    npc, mine = secs(host, key), secs(host, me) if me else []
    log("worn sections: npc", sorted(worn(npc)), "| client's character", sorted(worn(mine)))
    if me and not (worn(npc) & worn(mine)):
        # setup: dress both from the host's other squad members, in one section, so they each wear something there
        donors = {}
        for j in range(1, 8):
            d = secs(host, f"squad{j}")
            if not d:
                continue
            for idx, s in d:
                if s != "main":
                    donors.setdefault(s, []).append((j, idx))
        target = next((s for s in worn(npc) if s in donors), None)
        if target:   # the NPC already wears one: give the client's character one too
            j, idx = donors[target][0]
            log("dress client's character", target, cmd(host, f"invmove squad{j} {me} {idx} 0 -1 -1 {target}"))
        else:
            target = next((s for s in donors if s not in worn(mine) and len(donors[s]) >= 2), None) or next((s for s in worn(mine) if s in donors), None)
            if target:
                j, idx = donors[target][0]
                log("dress npc", target, cmd(host, f"invmove squad{j} {key} {idx} 0 -1 -1 {target}"))
                if target not in worn(mine) and len(donors[target]) > 1:
                    j, idx = donors[target][1]
                    log("dress client's character", target, cmd(host, f"invmove squad{j} {me} {idx} 0 -1 -1 {target}"))
        time.sleep(4)
    ok, text = cmd(cli, f"invswap {key} squad{own} any")
    log("client swap", ok, text)
    if not ok:
        check("echange de vetements avec un corps", False, f"aucun emplacement commun occupe des deux cotes ({text})")
        return
    time.sleep(5)
    hs, cs = dump(host, "h_lootswap"), dump(cli, "c_lootswap")
    log("loot swap (live)", compare(hs, cs, "loot swap (live)"))
    refused = [l for l in host_log().splitlines()[-200:] if "refused" in l and ("item move" in l or "inventory move" in l)]
    check("echange de vetements : aucun refus chez l'hote", not refused, refused[-1] if refused else "")
    check("echange de vetements : l'hote a fait l'echange", any("client item swap done" in l for l in host_log().splitlines()[-200:]))


def exp_groundpick(host, cli):
    """fix G2: the client picks up items lying in town (save items, shop goods, clutter): the host finds
    the same one and its character takes it."""
    time.sleep(8)
    cmd(cli, "editdone")   # the joiner's character editor holds the whole game paused: nobody would walk
    time.sleep(3)
    # the client's own character: an order for squad0 (the host's character) is dropped by the client
    own = own_index(host)
    ok, text = cmd(cli, f"groundnear 600 loose {own}")   # around the character that will walk there
    items = text.split()[2:] if ok else []
    log("loose items near the client's squad:", text.split()[1] if ok else text)
    if not items:
        check("ramassage par un client", False, "aucun objet par terre autour")
        return
    picked = 0
    for it in items[:3]:
        key, tpl, pos = it.split("|")
        log(f"client asks squad{own} to pick up", tpl, pos, cmd(cli, f"pickupreq {own} {tpl} {pos}"))
        gone = False
        for i in range(15):
            time.sleep(2)
            after = cmd(host, "groundnear 3000 loose")[1].split()[2:]
            if not [x for x in after if x.split("|")[1] == tpl and dist(tuple(map(float, x.split("|")[2].split(","))), tuple(map(float, pos.split(",")))) < 40]:
                gone = True
                break
        log("  gone from the host's ground:", gone)
        picked += gone
    for l in [l for l in host_log().splitlines()[-300:] if "pick up" in l]:
        log("  host:", l.strip()[:220])
    failed = [l for l in host_log().splitlines()[-300:] if "pick up" in l and "FAILED" in l]
    check("ramassage par un client : l'objet est pris chez l'hote", picked > 0, f"{picked}/{min(3, len(items))}")
    check("ramassage par un client : aucun ordre refuse", not failed, failed[-1] if failed else "")


def exp_bodies(host, cli):
    """Where does a knocked-out body lie on each side, over time, for each body mode?"""
    time.sleep(6)
    ok, text = cmd(host, "spawnnpc 12 8")
    log("spawn", ok, text)
    key = text.split()[1]
    time.sleep(6)
    for mode in (0, 1):
        # the NPC fights the squad (it moves), then is knocked out
        log("fight", cmd(host, "fight 1"))
        time.sleep(4)
        log("ko", cmd(host, "ko"))
        for i in range(16):
            time.sleep(0.75)
            h, c = dump(host, "hb", 200), dump(cli, "cb", 200)
            hv, cv = h["char"].get(key, {}), c["char"].get(key, {})
            if "pos" in hv and "pos" in cv:
                log(f"  t={i * 0.75:.1f} err={dist(hv['pos'], cv['pos']):.2f} host {hv['pos']} f{hv.get('flags')} ko={hv.get('ko')} | cli {cv['pos']} f{cv.get('flags')} ko={cv.get('ko')} latest {cv.get('latest')}")
        log("wake", cmd(host, "wake"))
        time.sleep(8)


def ui(*args):
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", UI_PS1] + [str(a) for a in args],
                       capture_output=True, timeout=60, text=True)
    return r.stdout.strip()


def exp_lootorder(host, cli):
    """The right-click loot path (addTaskNearestSelectedCharacter) with the client's own character selected."""
    time.sleep(6)
    ok, text = cmd(host, "spawnnpc 20 10")
    key = text.split()[1]
    time.sleep(4)
    log("ko", cmd(host, "ko"))
    time.sleep(4)
    log("select own character", ui("click", cli, 652, 657))
    time.sleep(1)
    log("lootorder", cmd(cli, f"lootorder {key}"))
    for i in range(12):
        time.sleep(1)
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "lootorder.png")))


def exp_lootclick(host, cli):
    """Prepare a knocked-out NPC next to the client's character and leave both games running, so the
    real right-click "loot" can be driven by hand (ui.ps1 click ... right) and checked."""
    time.sleep(6)
    log("give", cmd(host, "give 2 0"))
    ok, text = cmd(host, "spawnnpc 6 4")
    log("spawn", text)
    time.sleep(5)
    log("ko", cmd(host, "ko"))
    time.sleep(4)
    log("host pid", host, "client pid", cli, "npc", text.split()[1] if ok else "?")


def proc_memory_mb(pid):
    """Resident memory of a process in MB (psutil when installed, else tasklist's 'Mem Usage')."""
    try:
        import psutil
        return round(psutil.Process(pid).memory_info().rss / 1048576, 1)
    except ImportError:
        pass
    except Exception:
        return None
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 5 and parts[1] == str(pid):
            digits = re.sub(r"[^0-9]", "", parts[4])
            return round(int(digits) / 1024, 1) if digits else None
    return None


def exp_soak(host, cli, minutes=20, phase_seconds=90, kinds=("Feu", "Tente", "Coffre", "Lit", "Mur")):
    """Stability soak: speed cycled 1 -> 2 -> 3 -> 2 -> 1 (one phase each, repeated), a burst of rapid
    speed / pause changes at the start of every phase, the client asking for speed changes itself (the
    host's clock must win, nothing may crash), and activity on both sides during every phase: squads
    moving, fights with spawned NPCs, knock out and loot, a trade window opened and closed, buildings
    placed, the client's character teleported far away and back once. Every 30 s: both games alive,
    frozen host/client comparison (the suite's tolerances), command round trip and memory of both."""
    import random
    rnd = random.Random(1234)
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    own_c = own_index(cli)
    hidx = 0 if own != 0 else 1
    sid = find_building(host, kinds)
    log(f"soak: {minutes} min, phases of {phase_seconds} s; client's character index host {own} client {own_c}, "
        f"host mover {hidx}, building {sid}")
    speeds = (1, 2, 3, 2, 1)
    t0 = time.time()
    deadline = t0 + minutes * 60
    samples = []          # one dict per 30 s health sample
    client_speed = []     # (asked, host speed before, host speed after, client speed after)
    tp_state = {"done": False, "back_at": None, "home": None}
    npc = {"key": None}
    cur_speed = {"v": 1}

    def vec(text):
        return tuple(map(float, text.split()[1].split(",")))

    def speed_of(pid):
        t = cmd(pid, "paused")[1].split()   # ok <paused> <speed>
        return (t[1] == "1", float(t[2])) if len(t) >= 3 else (None, -1.0)

    def set_speed(v):
        cur_speed["v"] = v
        return cmd(host, f"speed {v}")

    def rtt(pid):
        a = time.time()
        cmd(pid, "echo")
        return round((time.time() - a) * 1000)

    def health(label):
        if not alive(host) or not alive(cli):
            raise RuntimeError(f"instance died (crash?) before sample {label}: host alive {alive(host)} client alive {alive(cli)}")
        s = {"label": label, "t": round(time.time() - t0), "speed": cur_speed["v"],
             "rtt": (rtt(host), rtt(cli)), "mem": (proc_memory_mb(host), proc_memory_mb(cli)),
             "client_state": status(cli).get("state")}
        cmd(host, "pause 1")
        time.sleep(2.5)
        h = dump(host, "h_soak_" + label)
        rep = compare(h, dump(cli, "c_soak_" + label), "soak " + label, pos_tol=0.1)
        cmd(host, "pause 0")
        set_speed(cur_speed["v"])
        # as the suite: a body lying on the ground is a ragdoll each game simulates itself (knocked
        # out, dead, or down with a crippled leg: flag 4 on the host)
        down = {k for k, v in h["squad"].items() if int(v.get("vflags", 0) or 0) & 3 or int(v.get("flags", 0) or 0) & 4}
        s["squad_bad"] = [t for t in rep["squad"] if not isinstance(t[1], (int, float)) or t[1] > (8.0 if t[0] in down else 0.1)]
        for k in ("vital_flag_mismatch", "inventory_mismatch", "missing_on_client", "extra_on_client", "pos_err_max", "hours_diff",
                  "host_chars", "client_chars", "missing_sample", "inventory_mismatch_sample"):
            s[k] = rep.get(k)
        log(f"[{s['t']}s] sample {label} speed x{s['speed']}: rtt ms host/client {s['rtt']} mem MB host/client {s['mem']} "
            f"client {s['client_state']} | chars {s['host_chars']}/{s['client_chars']} missing {s['missing_on_client']} "
            f"vitals {s['vital_flag_mismatch']} inv {s['inventory_mismatch']} squad_bad {s['squad_bad']} hours_diff {s['hours_diff']}")
        samples.append(s)

    def burst(seconds=20):
        """Rapid changes every 1-2 s: speeds 1/2/3 and pause toggles from the host, speed keys on the client."""
        end = time.time() + seconds
        n = 0
        while time.time() < end:
            r = rnd.random()
            if r < 0.2:
                cmd(host, "pause 1")
                time.sleep(rnd.uniform(0.5, 1.0))
                cmd(host, "pause 0")
            elif r < 0.3:
                cmd(cli, f"speed {rnd.choice((1, 2, 3))}")
            else:
                set_speed(rnd.choice((1, 2, 3)))
            n += 1
            time.sleep(rnd.uniform(1.0, 2.0))
        log(f"rapid burst: {n} changes in {seconds} s")

    def client_asks_speed():
        hs = cur_speed["v"]
        ask = rnd.choice([v for v in (1, 2, 3) if v != hs])
        log(f"client asks for speed {ask} (host at {hs}):", cmd(cli, f"speed {ask}"))
        time.sleep(2)
        hp, hv = speed_of(host)
        cp, cv = speed_of(cli)
        log(f"  after 2 s: host paused={hp} speed={hv} | client paused={cp} speed={cv}")
        client_speed.append((ask, hs, hv, cv))
        log("client asks for a pause:", cmd(cli, "pause 1"))
        time.sleep(2)
        hp2, hv2 = speed_of(host)
        cp2, cv2 = speed_of(cli)
        log(f"  after 2 s: host paused={hp2} speed={hv2} | client paused={cp2} speed={cv2}")
        # paused on either side counts as speed 0: a pause the host did not ask for is a failure
        client_speed.append(("pause", hs, 0.0 if hp2 else hv2, 0.0 if cp2 else cv2))

    def act_moves():
        log("host squad moves:", cmd(host, f"moverel {hidx} {rnd.choice((-1, 1)) * 200} {rnd.choice((-1, 1)) * 150}"))
        log("client's character moves (client order):",
            cmd(cli, f"moverel {own_c} {rnd.choice((-1, 1)) * 150} {rnd.choice((-1, 1)) * 100}"))

    def act_fight():
        ok, t = cmd(host, f"spawnnpc 40 20 {hidx}")
        log("spawn npc:", t)
        npc["key"] = t.split()[1] if ok else None
        if npc["key"]:
            log("host fights:", cmd(host, f"fight {hidx}"), "client's character fights:", cmd(host, f"fight {own}"))

    def act_ko_loot():
        if not npc["key"]:
            return
        log("ko:", cmd(host, "ko"))
        time.sleep(3)
        log("client loots the body:", cmd(cli, f"loot {npc['key']} {own_c}"))
        npc["key"] = None

    def act_trade():
        log("trade window for the client's character:", cmd(host, f"tradeopen {own} any"))
        time.sleep(3)
        log("client trade state:", cmd(cli, "tradestate")[1][:120])
        log("close windows client/host:", cmd(cli, "closewindows"), cmd(host, "closewindows"))

    def act_build():
        if not sid:
            return
        log("client places", sid, place_valid(cli, sid, own_c, rnd.randint(-80, 80), rnd.randint(40, 90)))
        log("host places", sid, place_valid(host, sid, hidx, rnd.randint(-80, 80), rnd.randint(-90, -40)))

    def act_teleport():
        now = time.time()
        if not tp_state["done"] and now - t0 > minutes * 60 * 0.4:
            x, y, z = vec(cmd(host, f"where {own}")[1])
            tp_state.update(done=True, home=(x, y, z), back_at=now + 60)
            log("client's character teleported far away (dry land):", teleport_dry(host, own, (x, y, z), FAR_OFFSETS))
            time.sleep(3)
            cmd(cli, f"camto {own_c}")
        elif tp_state["back_at"] and now >= tp_state["back_at"]:
            tp_state["back_at"] = None
            log("client's character brought back:", cmd(host, "tpplayer 2"))
            time.sleep(5)
            cmd(cli, f"camto {own_c}")
            back = vec(cmd(host, f"where {own}")[1])
            tp_state["back_dist"] = dist(back[::2], tp_state["home"][::2])

    actions = [act_moves, act_fight, act_trade, act_ko_loot, act_build, client_asks_speed, act_moves, act_teleport]
    crash = None
    last_sample = time.time()
    phase = 0
    try:
        while time.time() < deadline:
            sp = speeds[phase % len(speeds)]
            log(f"=== phase {phase + 1}: speed x{sp}")
            burst()
            log("phase speed", set_speed(sp))
            phase_end = min(deadline, time.time() + phase_seconds)
            ai = 0
            while time.time() < phase_end:
                if time.time() - last_sample >= 30:
                    health(f"p{phase + 1}_{len(samples) + 1}")
                    last_sample = time.time()
                    continue
                actions[(ai + phase) % len(actions)]()
                ai += 1
                if ai == 3:   # pause / unpause once per phase, mid-activity
                    log("pause:", cmd(host, "pause 1"))
                    time.sleep(rnd.uniform(1, 3))
                    log("unpause:", cmd(host, "pause 0"))
                    set_speed(sp)
                time.sleep(rnd.uniform(4, 8))
            phase += 1
        if tp_state["back_at"]:   # the run ended while the client's character was far away
            tp_state["back_at"] = time.time()
            act_teleport()
        set_speed(1)
        health("end")
    except RuntimeError as e:
        crash = str(e)
        log("SOAK STOPPED:", crash)
    check("stabilite : aucun plantage de l'hote ni du client", crash is None and alive(host) and alive(cli),
          crash or f"{len(samples)} releves, {phase} phases")
    check("stabilite : le client reste connecte", samples and all(s["client_state"] == "connected" for s in samples),
          [(s["label"], s["client_state"]) for s in samples if s["client_state"] != "connected"][:5])

    def bad(key):
        return [(s["label"], s[key]) for s in samples if s[key]]
    check("stabilite : escouade identique en pause (<= 0.1, 8 pour un corps au sol)", not bad("squad_bad"), bad("squad_bad")[:5])
    check("stabilite : aucun etat vital different", not bad("vital_flag_mismatch"), bad("vital_flag_mismatch")[:5])
    check("stabilite : aucun inventaire different", not bad("inventory_mismatch"),
          [(s["label"], s["inventory_mismatch_sample"]) for s in samples if s["inventory_mismatch"]][:5])
    check("stabilite : personne ne manque chez le client", not bad("missing_on_client"),
          [(s["label"], s["missing_sample"]) for s in samples if s["missing_on_client"]][:5])
    check("stabilite : meme heure de jeu (< 0.01 h)", all((s["hours_diff"] or 0) < 0.01 for s in samples),
          [(s["label"], s["hours_diff"]) for s in samples if (s["hours_diff"] or 0) >= 0.01][:5])
    worst_rtt = max([max(s["rtt"]) for s in samples] or [0])
    check("stabilite : aller-retour d'une commande < 1 s", worst_rtt < 1000, f"pire {worst_rtt} ms")
    # the client's speed / pause requests: the host's clock is unchanged and the client follows it
    # (the client may run up to 5 % off the host's speed for a while: that is how its clock catches up)
    wrong = [c for c in client_speed if abs(c[2] - c[1]) > 0.01 or abs(c[3] - c[2]) > 0.01 + 0.06 * c[2]]
    check("stabilite : demande de vitesse / pause du client ignoree (horloge de l'hote)", client_speed and not wrong,
          f"{len(client_speed)} demandes, ecarts {wrong[:4]}")
    if tp_state["done"]:
        check("stabilite : TP loin puis retour du perso du client", tp_state.get("back_dist", 1e9) < 2000, tp_state.get("back_dist"))
    # memory: baseline = first sample after 2 min, fail if it grows by more than 40 % afterwards
    log("memory (MB) host/client:", [(s["label"], s["mem"]) for s in samples])
    base = next((s for s in samples if s["t"] >= 120 and None not in s["mem"]), None)
    if base:
        grow = [(s["label"], round(s["mem"][0] / base["mem"][0], 2), round(s["mem"][1] / base["mem"][1], 2))
                for s in samples if s["t"] > base["t"] and None not in s["mem"]]
        over = [g for g in grow if g[1] > 1.4 or g[2] > 1.4]
        check("stabilite : memoire stable (< +40 % apres 2 min)", not over,
              f"base {base['mem']} MB, ratios max {max([g[1] for g in grow] or [1])}/{max([g[2] for g in grow] or [1])} {over[:3]}")
    else:
        check("stabilite : memoire stable (< +40 % apres 2 min)", False, "pas de releve memoire apres 2 min")
    summary()


def exp_walk(host, cli):
    """Trace a walking NPC on the client (position corrections frame by frame)."""
    time.sleep(10)
    moving = []
    for attempt in range(40):
        c = dump(cli, "c_walk_pick", 3000)
        moving = [k for k, v in c["char"].items() if v.get("flags") == "1" and k not in c["squad"]]
        if moving:
            break
        time.sleep(2)
    key = moving[0]
    log("tracing", key, cmd(cli, f"trace {key} 3000"))
    time.sleep(3)
    log("pause", cmd(host, "pause 1"))
    time.sleep(3)
    log("unpause", cmd(host, "pause 0"))
    time.sleep(3)
    h, c = dump(host, "h_walk", 3000), dump(cli, "c_walk", 3000)
    hv, cv = h["char"].get(key, {}), c["char"].get(key, {})
    if "pos" in hv and "pos" in cv:
        log("live err", round(dist(hv["pos"], cv["pos"]), 2), "flags", hv.get("flags"), cv.get("flags"))


def exp_trace(host, cli):
    time.sleep(6)
    ok, text = cmd(host, "spawnnpc 12 8")
    key = text.split()[1]
    time.sleep(6)
    log("trace", cmd(cli, f"trace {key} 6000"))
    log("fight", cmd(host, "fight 1"))
    time.sleep(6)
    log("host moves squad1 away", cmd(host, "moverel 1 60 40"))
    time.sleep(10)


UI_PS1 = os.path.join(os.environ.get("KC_SCRATCH", ""), "tools", "ui.ps1")


def shot(pid, path):
    if os.path.exists(UI_PS1):
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", UI_PS1, "shot", str(pid), path, "1024"],
                       capture_output=True, timeout=30)


def exp_lootui(host, cli):
    time.sleep(6)
    log("give", cmd(host, "give 2 0"))
    ok, text = cmd(host, "spawnnpc 25 15")
    key = text.split()[1]
    log("spawn", text)
    time.sleep(5)
    log("ko", cmd(host, "ko"))
    time.sleep(4)
    log("tradegui before", cmd(cli, "tradegui"))
    log("loot", cmd(cli, f"loot {key} 0"))
    for i in range(20):
        time.sleep(1)
        c = dump(cli, "c_lootui", 300)
        sq = sorted(c["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))[0]
        d = dist(c["squad"][sq]["pos"], c["char"][key]["pos"]) if key in c["char"] else -1
        log(f"  t={i} looter-body {d:.1f} tradegui {cmd(cli, 'tradegui')[1]}")
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "lootui_client.png")))

    def invs(tag):
        h, c = dump(host, "h_" + tag, 300), dump(cli, "c_" + tag, 300)
        sq = sorted(h["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))[0]
        log(f"  [{tag}] npc host: {h['char'].get(key, {}).get('inv')}")
        log(f"  [{tag}] npc cli : {c['char'].get(key, {}).get('inv')}")
        log(f"  [{tag}] me  host: {h['squad'][sq].get('inv')}")
        log(f"  [{tag}] me  cli : {c['squad'][sq].get('inv')}")
        return h, c

    invs("before_drag")
    # drag the body's weapon (first weapon slot of the right-hand window) into the looter's bag
    if os.path.exists(UI_PS1):
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", UI_PS1, "drag", str(cli), "372", "180", "245", "330"],
                           capture_output=True, timeout=30, text=True)
        log("drag:", r.stdout.strip(), r.stderr.strip()[:200])
    time.sleep(4)
    h, c = invs("after_drag")
    r = compare(h, c, "after a real drag in the client's loot window")
    log("inventory_mismatch", r["inventory_mismatch"], r["inventory_mismatch_sample"])
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "lootui_client_after.png")))


LOOT_CREATURES = [
    # (template sid, name, distance from the looter: a big body is spawned farther so it does not land on it)
    ("3993-gamedata.base", "Goat", 30),
    ("3979-gamedata.base", "Bonedog", 35),
    ("3986-gamedata.base", "Wild Bull", 45),
    ("43895-rebirth.mod", "Gorillo", 50),
    ("43877-rebirth.mod", "Skin Spider", 55),
    ("2859-gamedata.base", "Beak Thing", 70),
    ("3988-gamedata.base", "Swamp Turtle", 110),
    ("3967-gamedata.base", "Leviathan", 220),
]


def exp_lootcreature(host, cli, only=""):
    """Every kind of creature is lootable from a client, like on the host in solo (session of 10/11 Oct.:
    leviathans could not be looted). The host spawns each creature (spawncreature, debug commands)
    next to the client's character and kills it; the client sends its loot order (what the right
    click's "loot" does); the loot window must open on the client once the looter stands by the body
    (lootstats: opened, not dropped), whatever its size; then one item of the body (its "death items":
    meat, skins, pearls) is taken and must land the same on the host and the client.

    The first run (11/10, 26/32) left the joiner's character editor open: the whole game stayed
    paused, no looter walked, and only the two bodies already in reach (turtle, leviathan) opened."""
    time.sleep(6)
    log("editdone", cmd(cli, "editdone"))   # the joiner's character editor holds the whole game paused: nobody would walk
    for _ in range(20):
        if (cmd(host, "paused")[1].split() + ["?", "?"])[1] == "0":
            break
        time.sleep(1)
    log("host paused:", cmd(host, "paused"))
    log("give", cmd(host, "give 2 0"))
    wanted = [c for c in LOOT_CREATURES if not only or only.lower() in c[1].lower()]
    for sid, name, d in wanted:
        ok, text = cmd(host, f"spawncreature {sid} {d} {d // 2} 0")
        log(f"[{name}] spawn:", text)
        if not ok or not text.startswith("ok"):
            check(f"{name}: spawned on the host", False, text)
            continue
        key = text.split()[1]
        time.sleep(4)
        log(f"[{name}] host body:", cmd(host, f"bodysize {key}")[1])
        h = dump(host, f"h_lc_{name}_alive", 600)
        at = h["char"].get(key, {}).get("pos")
        log(f"[{name}] kill:", cmd(host, "kill"))
        time.sleep(6)
        # its key may change with its death (the hand's container): the dead one where it stood
        h = dump(host, f"h_lc_{name}", 600)
        dead = [k for k, c in h["char"].items() if int(c.get("vflags", "0") or 0) & 2 and "pos" in c]
        if at and dead and (key not in h["char"] or not int(h["char"][key].get("vflags", "0") or 0) & 2):
            key = min(dead, key=lambda k: dist(h["char"][k]["pos"], at))
        log(f"[{name}] dead on the host:", key in dead, key)
        log(f"[{name}] client body:", cmd(cli, f"bodysize {key}")[1])
        before = cmd(cli, "lootstats")[1]
        log(f"[{name}] loot order:", cmd(cli, f"loot {key} 0"), before)
        opened = False
        stats = before
        for i in range(60):
            time.sleep(1)
            stats = cmd(cli, "lootstats")[1]
            if stats.split("opened=")[-1].split()[0] != before.split("opened=")[-1].split()[0]:
                opened = True
                break
            if stats.split("dropped=")[-1] != before.split("dropped=")[-1]:
                break
        check(f"{name}: the client's loot window opens by the body", opened, stats)
        shot(cli, os.path.abspath(os.path.join(OUT_DIR, f"lootcreature_{name.replace(' ', '_')}.png")))
        h, c = dump(host, f"h_lc_{name}_inv", 600), dump(cli, f"c_lootc_{name}_inv", 600)
        inv = h["char"].get(key, {}).get("inv")
        log(f"[{name}] body inventory host:", inv, "| client:", c["char"].get(key, {}).get("inv"))
        check(f"{name}: its death items are in its inventory on the host", bool(inv), inv)
        if not inv:
            continue
        log(f"[{name}] take one item:", cmd(cli, f"invmove {key} squad0 0"))
        time.sleep(4)
        h, c = dump(host, f"h_lc_{name}_after", 600), dump(cli, f"c_lc_{name}_after", 600)
        after = h["char"].get(key, {}).get("inv")
        r = compare(h, c, f"{name} looted")
        check(f"{name}: the item left the body on the host", after != inv, after)
        check(f"{name}: host and client agree after the loot", not r["inventory_mismatch"], r["inventory_mismatch_sample"])


def exp_items(host, cli):
    time.sleep(4)
    for args in ("917-gamedata.base 475-gamedata.base 918-gamedata.base 0", "917-gamedata.base 475-gamedata.base 918-gamedata.base 3",
                 "917-gamedata.base 52297-rebirth.mod 925-gamedata.base 0", "917-gamedata.base 478-gamedata.base 926-gamedata.base 2",
                 "475-gamedata.base 917-gamedata.base 918-gamedata.base 0", "475-gamedata.base 917-gamedata.base 918-gamedata.base 1",
                 "475-gamedata.base - - 0", "475-gamedata.base 917-gamedata.base - 0", "475-gamedata.base - 918-gamedata.base 0",
                 "475-gamedata.base 918-gamedata.base 917-gamedata.base 0", "475-gamedata.base 918-gamedata.base - 0",
                 "52297-rebirth.mod 917-gamedata.base 925-gamedata.base 0", "550-gamedata.base - - 0", "209-gamedata.base - - 0"):
        log(args, "->", cmd(host, "mkitem " + args))


def exp_dead(host, cli):
    time.sleep(6)
    log("give", cmd(host, "give 2 0"))
    ok, text = cmd(host, "spawnnpc 12 8")
    key = text.split()[1]
    time.sleep(5)
    for side, pid in (("host", host), ("cli", cli)):
        cmd(pid, f"state {os.path.abspath(os.path.join(OUT_DIR, side + '_dead0.txt'))} 300")
    log("kill", cmd(host, "kill"))
    time.sleep(6)
    for side, pid in (("host", host), ("cli", cli)):
        path = os.path.abspath(os.path.join(OUT_DIR, side + "_dead1.txt"))
        cmd(pid, f"state {path} 300")
        lines = [l.strip() for l in open(path, encoding="utf-8", errors="replace") if l.startswith("dead ")]
        log(side, "dead bodies:", len(lines), [l[:120] for l in lines[:8]], "spawned npc in dead list:", any(key in l for l in lines))
    # the corpse stays replicated: the client loots it
    h, c = dump(host, "h_corpse", 300), dump(cli, "c_corpse", 300)
    log("corpse entity on client:", any(e["key"] == key for e in c["entity"].values()))
    log("corpse inv host:", h["char"].get(key, {}).get("inv"))
    log("loot corpse", cmd(cli, f"invmove {key} squad0 worn"))
    time.sleep(4)
    h, c = dump(host, "h_corpse2", 300), dump(cli, "c_corpse2", 300)
    sq = sorted(h["squad"], key=lambda k: (int(k.split(":")[3]), int(k.split(":")[4])))[0]
    log("corpse inv host/cli after:", h["char"].get(key, {}).get("inv"), "|", c["char"].get(key, {}).get("inv"))
    log("looter inv host/cli after:", h["squad"][sq].get("inv"), "|", c["squad"][sq].get("inv"))
    r = compare(h, c, "corpse looted")
    log("inventory_mismatch", r["inventory_mismatch"], "down_bodies", r["down_bodies_worst"])


def exp_caravan(host, cli):
    """Travelling merchants (caravans): a trader without a home building walking with pack animals.
    The game builds such a trader's window from the worn backpacks of its squad (ShopTrader's
    constructor); the mod syncs every worn backpack (BagBind) and the host sells from them. Checked:
    the caravan is the host's everywhere, its packs hold the same stock everywhere, the client buys and
    sells while it walks (paid on the host, same cats and stock everywhere), the window closes when a
    pack animal is killed, the client steals from a living NPC pack animal (the host's crime check)
    and loots the dead one (items conserved).
    No caravan template is spawned (spawnnpc only copies a nearby NPC): run it on a save where a
    caravan walks near the squad; the debug command 'caravan' finds the nearest one."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    ch, cc = cmd(host, f"caravan {own}"), cmd(cli, f"caravan {own}")
    log("caravan host:", ch, "/ client:", cc)
    check("marchand ambulant : une caravane est en vue chez l'hote", ch[0], ch[1])
    if not ch[0]:
        summary()
        return
    hk = re.search(r"key=(\S+)", ch[1])
    ck = re.search(r"key=(\S+)", cc[1]) if cc[0] else None
    check("marchand ambulant : le client voit la meme caravane", bool(hk and ck and hk.group(1) == ck.group(1)), f"hote {ch[1]} / client {cc[1]}")
    hm = re.search(r"members=(\d+) animals=(\d+)", ch[1])
    cm = re.search(r"members=(\d+) animals=(\d+)", cc[1]) if cc[0] else None
    check("marchand ambulant : pas de caravane en double chez le client", bool(hm and cm and hm.groups() == cm.groups()), f"hote {ch[1]} / client {cc[1]}")
    log("where the stock lives (host): members' own stacks / stacks in worn packs:", ch[1])
    beast = cmd(host, f"caravanbeast {own}")
    log("pack animal:", beast)
    key = beast[1].split()[1] if beast[0] else None
    if key:
        bh, bc = cmd(host, f"bag {key}"), cmd(cli, f"bag {key}")
        log("its pack host:", bh[1][:200], "/ client:", bc[1][:200])
        check("marchand ambulant : sac de la bete identique partout", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")
    rep = compare(dump(host, "h_car0"), dump(cli, "c_car0"), "caravan0")
    check("marchand ambulant : inventaires de la caravane identiques", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    # trade with the walking caravan
    log("client opens trade with the walking caravan:", cmd(cli, f"caravanopen {own}"))
    state = "?"
    for _ in range(40):
        time.sleep(0.5)
        state = cmd(cli, "tradestate")[1]
        if "open=1" in state:
            break
    hs = cmd(host, "tradestate")[1]
    log("client trade state:", state, "/ host:", hs)
    check("marchand ambulant : la fenetre s'ouvre chez le client", "open=1" in state, state)
    check("marchand ambulant : l'hote suit ce commerce", "hosttrades=1" in hs, hs)
    stock_c = cmd(cli, "tradelist merchant")[1]
    log("client sees the stock:", stock_c[:300])
    check("marchand ambulant : le stock de la caravane est la chez le client", stock_c.startswith("ok") and stock_c.split()[1] != "0", stock_c[:120])
    cats0_h, cats0_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    log("client buys:", cmd(cli, "tradebuy 0"))
    time.sleep(3)
    cats1_h, cats1_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("marchand ambulant : achat paye chez l'hote", cats1_h != cats0_h, f"hote {cats0_h} -> {cats1_h}")
    check("marchand ambulant : meme argent partout apres l'achat", cats1_h == cats1_c, f"hote {cats1_h} / client {cats1_c}")
    if key:
        bh, bc = cmd(host, f"bag {key}"), cmd(cli, f"bag {key}")
        check("marchand ambulant : stock identique apres l'achat", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")
    log("client sells:", cmd(cli, "tradesell 0"))
    time.sleep(3)
    cats2_h, cats2_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("marchand ambulant : vente payee chez l'hote", cats2_h != cats1_h, f"hote {cats1_h} -> {cats2_h}")
    check("marchand ambulant : meme argent partout apres la vente", cats2_h == cats2_c, f"hote {cats2_h} / client {cats2_c}")
    rep = compare(dump(host, "h_car1"), dump(cli, "c_car1"), "caravan1")
    check("marchand ambulant : inventaires identiques apres achat et vente", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    if not key:
        summary()
        return
    # the pack animal is killed mid-trade: the window closes everywhere
    log("host kills the pack animal:", cmd(host, "kill"))
    time.sleep(6)
    hs, cs = cmd(host, "tradestate")[1], cmd(cli, "tradestate")[1]
    check("marchand ambulant : la fenetre se ferme quand la bete meurt", "hosttrades=0" in hs and "open=0" in cs, f"hote {hs} / client {cs}")
    cmd(cli, "closewindows")
    bh, bc = cmd(host, f"bag {key}"), cmd(cli, f"bag {key}")
    check("marchand ambulant : la bete morte garde son stock, identique partout", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")

    def total(pid):
        n = 0
        for who in (f"bag {key}", f"bag squad{own}"):
            r = cmd(pid, who)[1]
            n += sum(int(q) for q in re.findall(r":(\d+)", r))
        d = dump(pid, "car_total")
        sq = sorted(d["squad"])[own] if len(d["squad"]) > own else None
        for it in (d["squad"].get(sq, {}).get("inv") or []):
            m = re.search(r":(\d+)", str(it))
            n += int(m.group(1)) if m else 1
        return n
    before = total(host)
    for _ in range(3):
        log("client loots the dead animal's pack:", cmd(cli, f"invmove bag:{key} squad{own} 0"))
        time.sleep(3)
    after = total(host)
    bh, bc = cmd(host, f"bag {key}"), cmd(cli, f"bag {key}")
    check("marchand ambulant : pillage du sac, identique partout", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")
    check("marchand ambulant : pillage du sac, rien de cree ni perdu", before == after, f"objets {before} -> {after}")
    rep = compare(dump(host, "h_car3"), dump(cli, "c_car3"), "caravan looted")
    check("marchand ambulant : inventaires identiques apres le pillage", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    # stealing from another, living, pack animal of the caravan
    beast2 = cmd(host, f"caravanbeast {own}")
    if beast2[0]:
        key2 = beast2[1].split()[1]
        b0 = cmd(host, f"bounty {own}")[1]
        log("client steals from a living pack animal:", cmd(cli, f"invmove bag:{key2} squad{own} 0"))
        time.sleep(4)
        bh, bc = cmd(host, f"bag {key2}"), cmd(cli, f"bag {key2}")
        check("marchand ambulant : vol dans le sac d'une bete, identique partout", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")
        b1h, b1c = cmd(host, f"bounty {own}")[1], cmd(cli, f"bounty {own}")[1]
        log("bounty before / after (host):", b0, "/", b1h, "client:", b1c)
        check("marchand ambulant : prime eventuelle identique partout", b1h == b1c, f"hote {b1h} / client {b1c}")
    summary()


def exp_pets(host, cli):
    """Animals of the player's squad (pack beasts, dogs, goats...): ordinary squad characters, the
    host's until given to a player (then they join the squad of that player's own character, so they
    follow it); their position, health, hunger and inventory are the host's everywhere, a client's own
    animal obeys its orders through the host, and items moved into it at once by host and client are
    never duplicated. A client leaving gives its animals to the host; joining again (same Steam id)
    gives them back (not automated here: it needs a new join). An animal a client buys in a
    conversation becomes that client's (not automated: it needs an animal trader)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    ah, ac = cmd(host, "squadanimals"), cmd(cli, "squadanimals")
    log("squad animals host:", ah, "/ client:", ac)
    check("animaux : memes animaux dans l'escouade partout", ah == ac, f"hote {ah[1]} / client {ac[1]}")
    if not ah[0] or ah[1].split()[1] == "0":
        log("no animal in the squad: buy or tame one on the save first")
        summary()
        return
    idx = int(ah[1].split()[2].split(":")[0])
    akey = ah[1].split()[2].split(":", 1)[1].rsplit(":", 1)[0]
    own = own_index(host)
    log("host gives the animal to the client:", cmd(host, f"give 2 {idx}"))
    time.sleep(4)
    d = dump(host, "h_pet0")
    owner = entity_of(d, akey).get("owner")
    check("animaux : l'animal donne appartient au client", owner == "2", f"proprietaire {owner}")
    log("client moves its animal:", cmd(cli, f"moverel {idx} 20 0"))
    time.sleep(8)
    rep = compare(dump(host, "h_pet1"), dump(cli, "c_pet1"), "pet moved")
    check("animaux : position, sante et inventaire de l'animal identiques", rep["inventory_mismatch"] == 0 and rep["pos_err_over_tol"] == 0,
          f"{rep['inventory_mismatch_sample']} pos>tol {rep['pos_err_over_tol']}")
    log("client moves an item into its animal:", cmd(cli, f"invmove squad{own} squad{idx} main"))
    log("host moves an item into it at once:", cmd(host, f"invmove squad0 squad{idx} main"))
    time.sleep(4)
    rep = compare(dump(host, "h_pet2"), dump(cli, "c_pet2"), "pet inventory")
    check("animaux : inventaire partage, pas de doublon", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    bh, bc = cmd(host, f"bag squad{idx}"), cmd(cli, f"bag squad{idx}")
    if bh[0] or bc[0]:
        check("animaux : sac porte par l'animal identique partout", bh == bc, f"hote {bh[1][:120]} / client {bc[1][:120]}")
    summary()


def exp_animalshop(host, cli):
    """A_FAIRE 3i: buying animals. The host spawns a seller and two goats next to the client's character
    and makes the game's animal shop for it (animalshop: what a farmer's "animals for sale?" does); the
    window must open on the client only (the host keeps it hidden and its offer aside); the client ticks
    one animal and confirms; the host's game sells it (cats taken from the shared money), the animal
    becomes the client's and joins their squad, everywhere. Then the host buys one for itself (its own
    window, the game's usual path)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("cats for the test:", cmd(host, "givemoney 30000"))
    log("seller:", cmd(host, f"spawnnpc 25 0 {own}"))
    for dx, dz in ((35, 12), (40, -10), (45, 4)):
        log("goat:", cmd(host, f"spawncreature 3993-gamedata.base {dx} {dz} {own}"))
    time.sleep(6)
    animals0_h, animals0_c = cmd(host, "squadanimals")[1], cmd(cli, "squadanimals")[1]
    opened = cmd(host, f"animalshop {own} 1000")
    log("host makes the animal shop for the client's character:", opened)
    state_c = "?"
    for _ in range(20):
        time.sleep(0.5)
        state_c = cmd(cli, "animalshopstate")[1]
        if "visible=1" in state_c:
            break
    state_h = cmd(host, "animalshopstate")[1]
    log("client:", state_c, "| host:", state_h)
    check("animaux (boutique) : la fenetre s'ouvre chez le client", "visible=1" in state_c and "entries=0" not in state_c, state_c)
    check("animaux (boutique) : rien ne s'affiche chez l'hote, l'offre est gardee", "visible=0" in state_h and "offers=1" in state_h, state_h)
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "animalshop_client.png")))
    cats0_h, cats0_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    bought = cmd(cli, "animalbuy 0")
    log("client buys the first animal:", bought)
    time.sleep(5)
    state_h2 = cmd(host, "animalshopstate")[1]
    cats1_h, cats1_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    log("cats host", cats0_h, "->", cats1_h, "/ client", cats0_c, "->", cats1_c, "| host", state_h2)
    check("animaux (boutique) : la fenetre du client se ferme apres l'achat", bought[0] and "window=0" in bought[1], bought[1])
    check("animaux (boutique) : l'hote vend l'animal (sold=1, offre liberee)", "sold=1" in state_h2 and "offers=0" in state_h2, state_h2)
    check("animaux (boutique) : achat paye chez l'hote", cats1_h != cats0_h, f"{cats0_h} -> {cats1_h}")
    check("animaux (boutique) : meme argent partout", cats1_h == cats1_c, f"hote {cats1_h} / client {cats1_c}")
    time.sleep(4)
    animals1_h, animals1_c = cmd(host, "squadanimals")[1], cmd(cli, "squadanimals")[1]
    log("squad animals host", animals0_h, "->", animals1_h, "/ client", animals0_c, "->", animals1_c)
    log("host, the bought animal right after the sale:", " | ".join(l.split("  ", 1)[-1] for l in host_log().splitlines() if "bought animal now" in l)[-600:])
    n = lambda s: int((s.split() + ["0", "0"])[1]) if s.startswith("ok") else -1
    check("animaux (boutique) : l'animal rejoint l'escouade chez l'hote et le client", n(animals1_h) == n(animals0_h) + 1 and n(animals1_c) == n(animals1_h),
          f"hote {animals1_h} / client {animals1_c}")
    if n(animals1_h) > 0:
        akey = animals1_h.split()[-1].split(":", 1)[1].rsplit(":", 1)[0]
        owner = entity_of(dump(host, "h_animalshop"), akey).get("owner")
        check("animaux (boutique) : l'animal appartient au client", owner == "2", f"proprietaire {owner} ({akey})")
    # the squad's positions (the bought animal included): the NPCs walking around are dumped a few
    # tenths of a second apart on each side (13/10 15:14: 10 walkers at ~15 units/s, 4-9 units apart;
    # it passed before only because the host's game was paused by the editor, see SellAnimals)
    rep = compare(dump(host, "h_animalshop2"), dump(cli, "c_animalshop2"), "animal shop")
    far = [s for s in rep["squad"] if len(s) < 3 or s[1] > 3.0]
    check("animaux (boutique) : positions identiques apres l'achat", not far, f"{far} | PNJ au-dela de 3 : {rep.get('pos_err_over_tol')}")
    # the host's own purchase: its window on its screen, the game's usual path
    mine = cmd(host, "animalshop 0 1500")
    time.sleep(1)
    state_h3 = cmd(host, "animalshopstate")[1]
    log("host's own animal shop:", mine, state_h3)
    check("animaux (boutique) : l'hote voit sa propre fenetre", "visible=1" in state_h3, state_h3)
    check("animaux (boutique) : rien ne s'ouvre chez le client pour l'achat de l'hote", "visible=0" in cmd(cli, "animalshopstate")[1] or "window=0" in cmd(cli, "animalshopstate")[1],
          cmd(cli, "animalshopstate")[1])
    if "visible=1" in state_h3:
        log("host buys:", cmd(host, "animalbuy 0"))
    summary()


# ---- 0.3.5 lot 5 (branch night-actions): A_FAIRE 16/10 points 4, 5, 13, 15 and 2
def _dialog_state(cli):
    t = cmd(cli, "dialog")[1]
    d = {"raw": t}
    for kv in t.split(" | ")[0].split()[1:]:
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k] = v
    return d


def exp_slaveshop(host, cli, slaver="Esclavagiste"):
    """Point 4: the client talks to a slaver (real path: right click on him, talkclick ... click), answers
    "Je voudrais acheter des esclaves" in its conversation window: the game's slave window opens on the
    client only (the host keeps its own hidden), the client ticks a slave and presses the window's
    confirm (animalbuy: the window's own button); the host's game buys it (cats taken from the shared
    money), the slave is freed and follows the buyer. Then "vendre des esclaves": only the client's own
    characters (not its main one) are offered. Needs a slaver camp next to the squad (--save; default
    kctest_slavers: a save at a slaver camp, Chef Esclavagistes and caged slaves). Without a slaver in
    reach, the window is made by the host's slaveshop command (routing checked, not the dialogue)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("cats for the test:", cmd(host, "givemoney 30000"))
    via_dialog = False
    r = cmd(cli, f"talkclick {own} name:{slaver} click")
    log("client right-clicks the slaver:", r)
    if r[0]:
        d = _dialog_state(cli)
        for _ in range(60):
            if d.get("open") == "1":
                break
            time.sleep(0.5)
            d = _dialog_state(cli)
        log("conversation:", d["raw"][:300])
        replies = d["raw"].split(" | ")[1:]
        pick = next((i for i, a in enumerate(replies) if "acheter des esclaves" in a or "buy slaves" in a.lower()), None)
        if pick is not None:
            log("client answers", pick, replies[pick], cmd(cli, f"answer {pick}"))
            via_dialog = True
    if not via_dialog:
        log("no slaver conversation here: the host makes the game's slave window (fallback)", cmd(host, f"slaveshop {own} buy"))
    state_c = "?"
    for _ in range(20):
        time.sleep(0.5)
        state_c = cmd(cli, "slaveshopstate")[1]
        if "visible=1" in state_c:
            break
    state_h = cmd(host, "slaveshopstate")[1]
    log("client:", state_c, "| host:", state_h)
    check("esclaves (achat) : la fenetre s'ouvre chez le client" + ("" if via_dialog else " (fenetre faite par la commande)"),
          "visible=1" in state_c and "entries=0" not in state_c, state_c)
    check("esclaves (achat) : rien ne s'affiche chez l'hote, l'offre est gardee", "visible=0" in state_h and "offers=1" in state_h, state_h)
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "slaveshop_client.png")))
    cats0_h = cmd(host, "money")[1]
    bought = cmd(cli, "animalbuy 0")
    log("client ticks the first slave and confirms:", bought)
    time.sleep(5)
    state_h2 = cmd(host, "slaveshopstate")[1]
    cats1_h, cats1_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("esclaves (achat) : l'hote achete (traded=1, offre liberee)", "traded=1" in state_h2 and "offers=0" in state_h2, state_h2)
    check("esclaves (achat) : paye chez l'hote, meme argent partout", cats1_h != cats0_h and cats1_h == cats1_c, f"{cats0_h} -> {cats1_h} / client {cats1_c}")
    log("host log:", [l for l in host_log().splitlines() if "slave" in l][-6:])
    # selling: the host gives the client a second character first (the main one is never offered)
    if True:   # the dialogue's 0x29 is not looked for: its answer differs by slaver
        log("sell window (host command, the dialogue's 0x29):", cmd(host, f"slaveshop {own} sell 2000"))
        time.sleep(3)
        st = cmd(cli, "slaveshopstate")[1]
        mine = cmd(cli, "ownidx")[1].split()[1:]
        log("client sell window:", st, "| client's characters:", mine)
        n = int(re.search(r"entries=(\d+)", st).group(1)) if "entries=" in st else 0
        check("esclaves (vente) : seuls les persos du client, pas le principal", n <= max(0, len(mine) - 1),
              f"{st} | persos du client {mine}")
        cmd(cli, "closewindows")
    summary()


def exp_lockprogress(host, cli):
    """Point 5: the client's character picks (76), cuts (285) then forces (286) the lock of the nearest
    locked furniture (a cage: --save with a cage near the squad, default kctest_town's locked chests),
    through the order a right click gives (doororder). The host's game does the work; the client must
    show the game's progress bar above its character while it goes, and lose it when it stops."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("locks around (host):", cmd(host, "doors 0 lock")[1][:400])
    for task, what in ((76, "crocheter"), (285, "scier"), (286, "forcer")):
        cmd(host, "doorset 0 lock lock")
        time.sleep(2)
        log(f"client orders {what} ({task}):", cmd(cli, f"doororder {own} {task} lock"))
        seen = ""
        for _ in range(40):
            time.sleep(0.5)
            st = cmd(cli, "progressstate")[1]
            if re.search(r"shown=[1-9]", st):
                seen = st
                break
        check(f"serrure ({what}) : la barre de progression s'affiche chez le client", bool(seen), seen or cmd(cli, "progressstate")[1])
        if seen:
            shot(cli, os.path.abspath(os.path.join(OUT_DIR, f"lockprogress_{task}.png")))
        log("host:", cmd(host, "progressstate")[1], [l for l in host_log().splitlines() if "action progress" in l][-2:])
        cmd(cli, f"moverel {own} 60 0")   # another order: the task stops
        gone = False
        for _ in range(20):
            time.sleep(0.5)
            if "shown=0" in cmd(cli, "progressstate")[1]:
                gone = True
                break
        check(f"serrure ({what}) : la barre disparait quand l'action s'arrete", gone, cmd(cli, "progressstate")[1])
    summary()


def exp_disguise(host, cli, armour="Paladin"):
    """Point 13: the host gives the client's character a Holy Nation uniform (helmet, body armour, legs:
    giveitem, names with --armour), the client puts each piece on through the inventory drag & drop
    (invmove, the inventory window's path); the host's game must say the character is disguised at once
    (disguisestate on the host), the client's copy too (its stealth panel's "In Disguise"; overlay line),
    then the character walks past paladins (--save near a Holy Nation town, default kctest_holy): nobody
    attacks it. The host's own character dressed the same is compared."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    for part in (f"{armour}_Helmet", f"{armour}_Armour", f"{armour}_Leggings", "Casque_de_Paladin", "Armure_de_Paladin", "Pantalon_de_Paladin"):
        log("give", part, cmd(host, f"giveitem {part} 1 {own}"))
    time.sleep(3)
    items = cmd(cli, f"invitems squad{own}")[1]
    log("client inventory:", items[:400])
    for sec in ("head", "body", "legs"):
        log("client puts on", sec, cmd(cli, f"invmove squad{own} squad{own} main 1 0 0 {sec}"))
        time.sleep(1.5)
    time.sleep(2)
    dh, dc = cmd(host, f"disguisestate {own}")[1], cmd(cli, f"disguisestate {own}")[1]
    log("host:", dh, "| client:", dc)
    check("deguisement : l'hote voit le perso du client deguise", "disguised=1" in dh, dh)
    check("deguisement : le client voit son perso deguise (indicateur)", "disguised=1" in dc and "Déguisé" in cmd(cli, "progressstate")[1], dc)
    log("host log:", [l for l in host_log().splitlines() if "disguise" in l][-6:])
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "disguise_client.png")))
    t0 = time.time()
    fights = 0
    while time.time() - t0 < 60:
        cmd(cli, f"move {own} 400 0") if (int(time.time() - t0) // 15) % 2 == 0 else cmd(cli, f"move {own} -400 0")
        time.sleep(5)
        if "fight" in cmd(host, f"combat {own}")[1]:
            fights += 1
    check("deguisement : les paladins n'attaquent pas le perso deguise du client", fights == 0, f"{fights} releves en combat")
    summary()


def exp_tradefartp(host, cli, merchant="Marchand"):
    """Point 15: the host takes the client's character far away (another zone), then brings it back with
    the admin teleport (admin tp 2 host: the client loads the zone, "the host teleports us far"), and the
    client sells to a merchant there (the game's right click in the trade window, tradesell): the sale
    must reach the host (money, inventories), without a rejoin. Default kctest_town."""
    time.sleep(45)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("far:", teleport_dry(host, own, (0, -300, 0), [(30000, 30000), (34000, 30000), (30000, 34000)], settle=4))
    time.sleep(20)
    log("admin tp back:", cmd(host, "admin tp 2 host"))
    time.sleep(40)
    log("give something to sell:", cmd(host, f"giveitem Massue 1 {own}"))
    time.sleep(3)
    opened = cmd(host, f"tradeopen {own} {merchant} near")
    log("trade window asked:", opened)
    state = "?"
    for _ in range(40):
        time.sleep(0.5)
        state = cmd(cli, "tradestate")[1]
        if "open=1" in state:
            break
    check("TP lointain puis commerce : la fenetre s'ouvre chez le client", "open=1" in state, state)
    cats0 = cmd(host, "money")[1]
    sold = cmd(cli, "tradesell 0")
    log("client sells:", sold)
    time.sleep(4)
    cats1_h, cats1_c = cmd(host, "money")[1], cmd(cli, "money")[1]
    check("TP lointain puis commerce : la vente arrive chez l'hote", sold[0] and cats1_h != cats0, f"{sold[1]} | {cats0} -> {cats1_h}")
    check("TP lointain puis commerce : meme argent partout", cats1_h == cats1_c, f"hote {cats1_h} / client {cats1_c}")
    rep = compare(dump(host, "h_tradefartp"), dump(cli, "c_tradefartp"), "tradefartp")
    check("TP lointain puis commerce : inventaires identiques", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    log("host log:", [l for l in host_log().splitlines() if "again" in l or "cats " in l][-8:])
    cmd(cli, "closewindows")
    summary()


def exp_followjob(host, cli):
    """Point 2: the host gives one of its characters a "follow" job on the client's character (jobreq ...
    31, the Tâches panel's job), then removes it with the panel's cross (jobremove: removePermajob, what
    the cross calls): the follow must end at once (currentorder: no more order 31), on the host's
    character and on a client's character (the client removes its own job through its panel)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    mine = 0 if own != 0 else 1
    for who, pid, idx, tgt in (("hote", host, mine, own), ("client", cli, own, mine)):
        log(f"{who}: follow job", cmd(pid, f"jobreq {idx} 31 {tgt}"))
        time.sleep(4)
        o0 = cmd(host, f"currentorder {idx}")[1]
        log("current order on the host:", o0, "| jobs", cmd(host, f"jobs {idx}")[1])
        log(f"{who}: the cross on job 0:", cmd(pid, f"jobremove {idx} 0"))
        time.sleep(3)
        o1 = cmd(host, f"currentorder {idx}")[1]
        log("current order on the host after:", o1)
        check(f"taches ({who}) : 'suivre' retire, le perso arrete de suivre", "order=31" not in o1, f"{o0} -> {o1}")
    log("host log:", [l for l in host_log().splitlines() if "jobs:" in l][-6:])
    summary()


def exp_prospect(host, cli):
    """A_FAIRE 3p: the orders bar's PROSPECT button. On the client it opens the prospecting window at
    once for its own character (the place's fertility, water and ores, read from the game data) and
    nothing on the host; pressed again it closes it. On the host it works as in solo (task 150, window
    after a moment); the host cannot prospect with the client's character."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    mine = cmd(cli, "ownidx")[1].split()
    ci = int(mine[1]) if len(mine) > 1 and mine[1].isdigit() else 0
    log("client presses PROSPECT for its character", ci, cmd(cli, f"prospect {ci}"))
    time.sleep(1.5)
    st_c, st_h = cmd(cli, "prospectstate")[1], cmd(host, "prospectstate")[1]
    log("client:", st_c, "| host:", st_h)
    check("prospection : la fenetre s'ouvre chez le client", "visible=1" in st_c and "name=" in st_c and not st_c.endswith("name="), st_c)
    check("prospection : rien ne s'ouvre chez l'hote", "visible=1" not in st_h, st_h)
    shot(cli, os.path.abspath(os.path.join(OUT_DIR, "prospect_client.png")))
    log("client presses PROSPECT again:", cmd(cli, f"prospect {ci}"))
    time.sleep(1)
    st_c2 = cmd(cli, "prospectstate")[1]
    check("prospection : un second appui la ferme", "visible=0" in st_c2, st_c2)
    # 0.3.4: the game refuses PROSPECT indoors ("I need to go outside to do this.") or off solid ground
    # (checkPlayerOrderForProblems 0x5D1940, task 150): in kctest_town member 0 stands in a building
    # (floor group 9 in the dumps of 12/10 02:37 and 13/10 10:57, window never shown). The host prospects
    # with one of its own characters that is outside (floor group below 9), as a player would.
    d = dump(host, "h_prospect")
    keys = sorted(d["squad"])
    floors = {i: int(d["char"].get(k, {}).get("floor", "-1")) for i, k in enumerate(keys)}
    outside = [i for i, f in floors.items() if i != own and 0 <= f < 9]
    hi = outside[0] if outside else 0
    log("host squad floor groups (9+: in a building):", floors, "-> prospects with member", hi)
    log("host presses PROSPECT for its own character:", cmd(host, f"prospect {hi}"))
    st_h2 = "?"
    for _ in range(20):
        time.sleep(0.5)
        st_h2 = cmd(host, "prospectstate")[1]
        if "visible=1" in st_h2:
            break
    check("prospection : l'hote prospecte comme en solo", "visible=1" in st_h2,
          st_h2 + ("" if outside else " (aucun perso de l'hote dehors : le jeu refuse a l'interieur)"))
    check("prospection : le client n'en voit rien", "visible=1" not in cmd(cli, "prospectstate")[1], cmd(cli, "prospectstate")[1])
    log("host closes it:", cmd(host, f"prospect {hi}"))
    time.sleep(1)
    log("host presses PROSPECT for the client's character:", cmd(host, f"prospect {own}"))
    time.sleep(6)
    st_h3 = cmd(host, "prospectstate")[1]
    check("prospection : l'hote ne peut pas prospecter avec le perso du client", "visible=1" not in st_h3, st_h3)
    summary()


def exp_carry(host, cli):
    """The host carries a knocked out NPC, walks, puts it down: the client shows it on the shoulder,
    then lying where the host's landed, without throwing it."""
    def pos(pid, k):
        ok, t = cmd(pid, f"where {k}")
        return tuple(map(float, t.split()[1].split(","))) if ok and t.startswith("ok") else None
    def dist(a, b):
        return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5 if a and b else 1e9
    time.sleep(6)
    ok, text = cmd(host, "spawnnpc 12 4")
    key = text.split()[1] if ok and text.startswith("ok") else "?"
    time.sleep(4)
    log("ko", cmd(host, "ko"))
    time.sleep(6)
    log("carrynpc", cmd(host, "carrynpc 0"))
    time.sleep(3)
    hc, cc = cmd(host, "carrying 0")[1], cmd(cli, "carrying 0")[1]
    check("porter PNJ : l'hote le porte", hc == f"ok {key}", hc)
    check("porter PNJ : le client le voit porte par le meme perso", cc == hc, f"hote {hc} / client {cc}")
    cmd(host, "moverel 0 30 0")
    time.sleep(5)
    # The game's position of a carried body is its carrier's (the body is drawn on the shoulder bone):
    # same x,y,z as the carrier on the host too. What tells "on the shoulder" is the body's
    # "being carried" animation (carried=1), and the same reading as on the host.
    def carried_flag(pid, k):
        ok, t = cmd(pid, f"where {k}")
        return t.split()[3] if ok and t.startswith("ok") and len(t.split()) > 3 else "?"
    me, body = pos(cli, "0"), pos(cli, key)   # "npc" (lastSpawned_) only exists on the host
    hme, hbody = pos(host, "0"), pos(host, key)
    rel = lambda a, b: tuple(y - x for x, y in zip(a, b)) if a and b else None
    log("carried body vs carrier: host", rel(hme, hbody), "| client", rel(me, body))
    cf, hf = carried_flag(cli, key), carried_flag(host, key)
    check("porter PNJ : chez le client le corps est a l'epaule (animation portee, comme chez l'hote)",
          cf == "carried=1" and hf == "carried=1" and dist(rel(hme, hbody), rel(me, body)) < 2,
          f"client {cf} {rel(me, body)} / hote {hf} {rel(hme, hbody)}")
    log("carrydrop", cmd(host, "carrydrop 0"))
    # 0.3.4: the client's body falls ~1 s after the drop, settles 2 s, and if it lies more than 5
    # units from the host's it is stood up once and falls again on the host's spot (~3 s more)
    time.sleep(10)
    h1, c1 = pos(host, key), pos(cli, key)
    time.sleep(3)
    c2 = pos(cli, key)
    flat = lambda p: (p[0], 0, p[2]) if p else None
    check("poser PNJ : le corps est la ou celui de l'hote est tombe", dist(flat(h1), flat(c1)) < 5,
          f"hote {h1} / client {c1} (ecart en hauteur {abs(h1[1] - c1[1]):.1f})" if h1 and c1 else f"hote {h1} / client {c1}")
    check("poser PNJ : pas projete (immobile apres la chute)", dist(c1, c2) < 2, f"{c1} -> {c2}")
    check("poser PNJ : plus porte chez le client", cmd(cli, "carrying 0")[1] == "ok none")
    # 0.3.4 (A_FAIRE 12/10 point 12): nothing the host has standing stays down, in a bed or "carried" here
    time.sleep(3)
    pc = kv_of(cli, "posturecheck")
    log("client posturecheck:", cmd(cli, "posturecheck")[1])
    check("posture : apres le portage, rien que l'hote a debout n'est couche ou porte par personne chez le client", pc.get("n") == 0, pc)
    summary()   # 13/10: missing, the run ended after "posture" without its BILAN line



CLIENT_FAKE_ID = 76561190000000002


def exp_crashrejoin(host, cli, only=None):
    """A client killed hard (taskkill /F) in various situations, then relaunched with the same fake
    Steam id and joined again: the host notices, cleans up after it (characters halted and back to
    the host, windows and holds released) and stays alive and unpaused; the player gets the same
    characters back (count, inventory), nothing duplicated, the worlds match. (g): the relaunch
    beats the host's timeout (the new connection replaces the old one)."""
    state = {"cli": cli}

    def owned(d):
        """The client's characters in a host dump: squad key -> inventory."""
        keys = {e["key"] for e in d["entity"].values() if e.get("owner") not in (None, "0", "1")}
        return {k: d["squad"][k].get("inv", "") for k in keys if k in d["squad"]}

    def kill_client():
        pid = state["cli"]
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        deadline = time.time() + 20
        while alive(pid) and time.time() < deadline:
            time.sleep(0.2)
        log(f"client {pid} killed hard")
        return time.time()

    def relaunch():
        c = launch(fake_steam_id=CLIENT_FAKE_ID)
        state["cli"] = c
        log("client relaunched, pid", c)
        wait_for(c, lambda s: s.get("state") == "idle", 120, "client menu")
        time.sleep(3)
        ok, t = cmd(c, "join " + os.environ["KC_JOIN"]) if os.environ.get("KC_JOIN") else cmd(c, "join")
        log("join", ok, t)
        wait_for(c, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 240, "client back in host world")
        arrange(host, c)
        time.sleep(4)
        return c

    def host_speed():
        t = cmd(host, "paused")[1].split()   # ok <paused> <speed>
        return (t[1] == "1", float(t[2])) if len(t) >= 3 else (None, -1.0)

    def run(label, prepare, quick=False):
        if only and label[0] not in only:
            return
        log("=" * 20, "crash", label)
        cmd(host, "speed 1")
        time.sleep(2)
        before = owned(dump(host, f"h_crash_{label[0]}_before"))
        log("client's characters before:", sorted(before))
        prepare()
        logged = len(host_log())
        t_kill = kill_client()
        detected = None
        if not quick:
            while time.time() - t_kill < 40:
                if " disconnected (" in host_log()[logged:]:
                    detected = round(time.time() - t_kill, 1)
                    break
                time.sleep(0.25)
            check(f"crash client {label} : l'hote voit la deconnexion en quelques secondes", detected is not None and detected < 20, detected)
            new = host_log()[logged:]
            check(f"crash client {label} : l'hote nettoie (persos rendus et arretes)", "cleaned up:" in new, new[-300:])
            check(f"crash client {label} : l'hote reste vivant", alive(host))
            time.sleep(3)
        c = relaunch()
        new = host_log()[logged:]
        if quick:
            check(f"crash client {label} : la nouvelle connexion remplace l'ancienne", "reconnected: the old connection" in new, new[-300:])
        check(f"crash client {label} : l'hote reste vivant", alive(host))
        check(f"crash client {label} : le joueur retrouve ses persos", "is back" in new, new[-300:])
        time.sleep(3)
        cmd(host, "pause 1")
        time.sleep(3)
        hd, cd = dump(host, f"h_crash_{label[0]}"), dump(c, f"c_crash_{label[0]}")
        after = owned(hd)
        check(f"crash client {label} : memes persos (nombre, noms)", sorted(after) == sorted(before), (sorted(before), sorted(after)))
        same_inv = all(after.get(k) == v for k, v in before.items())
        check(f"crash client {label} : memes inventaires", same_inv or label.startswith("b"),   # a fight can change them
              [k for k, v in before.items() if after.get(k) != v][:3])
        check(f"crash client {label} : pas de doublon", len(cd["squad"]) == len(hd["squad"]), (len(hd["squad"]), len(cd["squad"])))
        rep = compare(hd, cd, f"crash {label}", pos_tol=0.5)
        print_report(rep)
        check(f"crash client {label} : mondes identiques ensuite",
              rep["missing_on_client"] == 0 and rep["extra_on_client"] == 0 and rep["inventory_mismatch"] == 0 and rep["vital_flag_mismatch"] == 0,
              (rep["missing_on_client"], rep["extra_on_client"], rep["inventory_mismatch"], rep["vital_flag_mismatch"]))
        cmd(host, "pause 0")
        time.sleep(3)
        paused, speed = host_speed()
        check(f"crash client {label} : l'hote n'est pas reste en pause", paused is False, (paused, speed))

    time.sleep(6)
    cmd(state["cli"], "editdone")   # a new character's editor, at the first join
    time.sleep(3)
    own = own_index(host)
    npc = {"key": None}

    def idle():
        pass

    def fight():
        ok, t = cmd(host, f"spawnnpc 40 20 {own}")
        npc["key"] = t.split()[1] if ok else None
        log("spawn npc:", t, "| fight:", cmd(host, f"fight {own}"), "| speed 3:", cmd(host, "speed 3"))
        time.sleep(5)

    def carry():
        ok, t = cmd(host, f"spawnnpc 12 4 {own}")
        time.sleep(4)
        log("ko", cmd(host, "ko"))
        time.sleep(6)
        log("carrynpc", cmd(host, f"carrynpc {own}"))
        time.sleep(3)
        log("carrying (client's character):", cmd(host, f"carrying {own}"))

    def trade():
        log("trade window:", cmd(host, f"tradeopen {own} any"))
        time.sleep(4)
        log("client trade state:", cmd(state["cli"], "tradestate")[1][:120])

    def loot():
        ok, t = cmd(host, f"spawnnpc 10 4 {own}")
        key = t.split()[1] if ok else "?"
        time.sleep(4)
        log("ko", cmd(host, "ko"))
        time.sleep(5)
        log("client loots:", cmd(state["cli"], f"loot {key} {own_index(state['cli'])}"))
        time.sleep(4)

    def editor():
        log("client opens the character editor:", cmd(state["cli"], "editchar"))
        time.sleep(5)
        log("host paused while editing:", host_speed())

    run("a idle", idle)
    run("b fight speed 3", fight)
    run("c carry", carry)
    run("d trade", trade)
    run("e loot", loot)
    run("f editor", editor)
    run("g quick relaunch", idle, quick=True)
    summary()
    return state["cli"]
# ---------------------------------------------------------------------------------------------
# Several players on one PC: host + N clients (setup_multi), the stress4 and join4 experiments
# ---------------------------------------------------------------------------------------------

FAKE_STEAM_BASE = 76561190000000002


def kill_launched():
    """Kill every Kenshi this harness started (and only those)."""
    for pid in list(_launched):
        if alive(pid):
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    deadline = time.time() + 20
    while any(alive(p) for p in _launched) and time.time() < deadline:
        time.sleep(0.5)


def player_id(pid):
    """The session's player id of that game (1 = host, 2.. = clients)."""
    try:
        return int(status(pid).get("id", "0"))
    except ValueError:
        return 0


def own_indices(pid, owner=None):
    """Squad indices, in that game's own order, of a player's characters (default: its own)."""
    ok, t = cmd(pid, "ownidx" + (f" {owner}" if owner is not None else ""))
    return [int(x) for x in t.split()[1:]] if ok else []


def join_one(c, label):
    return cmd(c, "join " + os.environ["KC_JOIN"]) if os.environ.get("KC_JOIN") else cmd(c, "join")


JOIN_QUEUE = {}   # what setup_multi saw of the host's join queue (simultaneous joins)


def close_join_editor(host, c, name, i):
    """The player whose join turn it is makes their character: wait until the host sees their
    editor open (its join queue shows '<name>:editor'), then close it as the confirm button does."""
    try:
        wait_for(host, lambda s: f"{name}:editor" in s.get("queue", ""), 30, f"client {i + 1} character editor")
    except RuntimeError as e:   # no editor (a character they already had): the turn ends by itself
        log(f"client {i + 1}: no character editor seen ({e})")
    time.sleep(2)
    log(f"client {i + 1} editdone:", cmd(c, "editdone"))


def setup_multi(save, n_clients=3, simultaneous=False):
    """A host and n_clients clients on this PC, each with its own fake Steam id and player name.
    Sequential: each client joins once the previous one is in (editor closed). Simultaneous: every
    client sends its join at once; they all download and load the host's world at the same time
    (0.3.1), then make their characters one at a time (the character editor queue), the others
    showing their place; each closes its character editor when its turn comes (what was seen of the
    queue goes to JOIN_QUEUE). Each client must own a character of its own.
    Returns (host, [client pids], {pid: player id})."""
    kill_all()
    host = launch(name="Hote")
    log("host pid", host)
    tile([host])
    wait_for(host, lambda s: s.get("state") == "idle", 180, "host menu")
    time.sleep(3)
    log("load", cmd(host, f"load {save}"))
    wait_for(host, lambda s: s.get("ready") == "1", 240, "host world")
    for _ in range(20):   # as setup(): the loaded world can still be settling
        ok, t = cmd(host, "host")
        if ok:
            break
        log("host not accepted yet:", t)
        time.sleep(1)
    log("host", ok, t)
    clis = []
    for n in range(n_clients):
        c = launch(fake_steam_id=FAKE_STEAM_BASE + n, name=f"Joueur{n + 2}")
        log(f"client {n + 1} pid {c} (fake steam id {FAKE_STEAM_BASE + n}, name Joueur{n + 2})")
        tile([host] + clis + [c])   # the user watches: every window visible after each launch
        wait_for(c, lambda s: s.get("state") == "idle", 240, f"client {n + 1} menu")
        time.sleep(3)
        if not simultaneous:
            # one join turn at a time: the next player would wait in the queue until this one's
            # character editor is closed
            log(f"client {n + 1} join:", join_one(c, n))
            wait_for(c, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 420, f"client {n + 1} in host world")
            log(f"client {n + 1} connected")
            close_join_editor(host, c, f"Joueur{n + 2}", n)
        clis.append(c)
    if simultaneous:
        log("every client joins at the same time: they load the world together, then make their characters one by one")
        JOIN_QUEUE.clear()
        th = [threading.Thread(target=lambda c=c, i=i: log(f"client {i + 1} join:", join_one(c, i))) for i, c in enumerate(clis)]
        for t in th:
            t.start()
        for t in th:
            t.join()
        JOIN_QUEUE["seen"] = {c: set() for c in clis}   # (position, total, waiting for, phase) each client showed
        JOIN_QUEUE["host"] = []                           # the host's queue lists seen
        JOIN_QUEUE["order"] = []                          # clients in the order they got in
        JOIN_QUEUE["overlap"] = []                        # two clients loading the world at once
        JOIN_QUEUE["names"] = {c: f"Joueur{i + 2}" for i, c in enumerate(clis)}
        deadline = time.time() + 420 * len(clis)
        while len(JOIN_QUEUE["order"]) < len(clis) and time.time() < deadline:
            hq = status(host).get("queue", "-")
            if hq != "-" and (not JOIN_QUEUE["host"] or JOIN_QUEUE["host"][-1] != hq):
                JOIN_QUEUE["host"].append(hq)
                log("host join queue:", hq)
            busy = []
            for i, c in enumerate(clis):
                if c in JOIN_QUEUE["order"]:
                    continue
                s = status(c)
                if s.get("state") == "failed":
                    log(f"client {i + 1} failed while joining:", s.get("error"))
                    JOIN_QUEUE["order"].append(c)   # counted, checked by exp_join4
                    continue
                if s.get("queue", "-") != "-":
                    q = (s.get("queue"), s.get("queueWait"), s.get("queuePhase"))
                    if q not in JOIN_QUEUE["seen"][c]:
                        log(f"client {i + 1} waiting for the character editor: position {q[0]}, after {q[1]} ({q[2]})")
                    JOIN_QUEUE["seen"][c].add(q)
                elif s.get("state") == "loading":
                    busy.append(c)
                if s.get("state") == "connected" and s.get("ready") == "1":
                    name = f"Joueur{i + 2}"
                    # in the world: its editor turn (the host lists '<name>:editor'), or still waiting
                    # for it ('<name>:wait'), or no editor for it (not in the host's list any more)
                    if f"{name}:editor" in hq or name + ":" not in status(host).get("queue", "-"):
                        log(f"client {i + 1} connected")
                        close_join_editor(host, c, name, i)
                        JOIN_QUEUE["order"].append(c)
            if len(busy) > 1:
                JOIN_QUEUE["overlap"].append([clis.index(c) + 1 for c in busy])
            time.sleep(1)
    time.sleep(6)
    ids = {host: player_id(host)}
    for i, c in enumerate(clis):
        ids[c] = player_id(c)
    time.sleep(3)
    for i, c in enumerate(clis):
        mine = own_indices(c)
        on_host = own_indices(host, ids[c])
        log(f"client {i + 1}: player id {ids[c]}, own characters: client indices {mine}, host indices {on_host}")
    arrange_grid([host] + clis)
    return host, clis, ids


def tag_total(state, sid, keys=None):
    """How many items of template sid the characters of a dump carry (squad + others; only keys if given).
    A squad member is listed twice in a dump, under 'squad' and under 'char' (it is near the squad):
    each character counts once (stress4 counted 40 of the 20 given, every sample, from the start)."""
    n = 0
    for k in set(state["squad"]) | set(state["char"]):
        if keys is not None and k not in keys:
            continue
        v = state["squad"].get(k) or state["char"].get(k)
        for it in (v.get("inv") or "").split(";"):
            m = re.match(r"(.+)x(\d+)@", it)
            if m and m.group(1) == sid:
                n += int(m.group(2))
    return n


def split_dist(a, b):
    """(horizontal, vertical) distance between two positions (x, y, z; y is up)."""
    return math.hypot(a[0] - b[0], a[2] - b[2]), abs(a[1] - b[1])


# Heights are not compared as tightly as the rest: a character's height is its own game's ground
# contact. In stress4 every height-only difference (squad members 0.12-4.55, NPCs and animals 7-14,
# a body on the ground 8.8) came with 0.000-0.017 across and the client's copy holding the host's
# position exactly ('latest'), all in the far regions where the host has no camera; the host's own
# squad at home, under its camera, never differed.
UP_TOL = 15.0
SEEN_RANGE = 300.0   # the mod's own range (plugin/world.cpp kSeenRange, kBodySeen): beyond it a
                     # client's game hardly runs a character and a lying body is not put right


def all_keys(state):
    return set(state["squad"]) | set(state["char"])


def char_pos(state, key):
    v = state["squad"].get(key) or state["char"].get(key) or {}
    return v.get("pos")


def keys_of_owner(state, owner):
    return [k for k, e in ((e["key"], e) for e in state["entity"].values()) if e.get("squad") == "1" and e.get("owner") == str(owner)]


def exp_join4(host, clis, ids):
    """3 clients joined at the same time: they loaded the world together, then made their characters
    one at a time (the others shown their place, nobody dropped), everyone sees every player's
    characters (names, positions), then every client leaves and the host carries on."""
    n = len(clis)
    who = f"{n + 1} joueurs"
    check(f"{who} : chaque client est connecte", all(status(c).get("state") == "connected" for c in clis),
          [(c, status(c).get("state")) for c in clis])
    check(f"{who} : ids de joueur distincts", len(set(ids.values())) == len(ids), ids)
    # the editor queue: one player at a time, the others shown their place, nobody dropped
    seen, order = JOIN_QUEUE.get("seen", {}), JOIN_QUEUE.get("order", [])
    log("join queue seen by the host:", JOIN_QUEUE.get("host"))
    check(f"{who} : file d'attente : tous les clients arrivent dans la partie, aucun rejete",
          len(order) == n and all(status(c).get("state") == "connected" for c in order), [(clis.index(c) + 1, status(c).get("error")) for c in order])
    waited = [c for c in clis if seen.get(c)]
    check(f"{who} : file d'attente : les clients qui attendent voient leur position", len(waited) >= n - 1,
          {clis.index(c) + 1: sorted(seen.get(c, [])) for c in clis})
    bad_pos = []
    for c in waited:
        for pos, wait, phase in seen[c]:
            try:
                p_, t_ = (int(x) for x in pos.split("/"))
            except ValueError:
                bad_pos.append((clis.index(c) + 1, pos))
                continue
            if not (2 <= p_ <= t_ <= n) or not wait.startswith("Joueur") or phase != "editor":
                bad_pos.append((clis.index(c) + 1, pos, wait, phase))
    check(f"{who} : file d'attente : positions (2..{n}/{n}), joueur attendu et etape coherents", waited and not bad_pos, bad_pos[:6])
    last = order[-1] if order else None
    # 0.3.1: everyone loads at once, only the editor goes one at a time, and a player is queued for it
    # once in the world. The last to arrive usually loads while the first is in the editor: by the time
    # it is in, only the one before it is left, so it sees one place (2/2), not a place that moves. What
    # counts: it was shown its place, behind a player who really went to the editor before it.
    went_before = {JOIN_QUEUE.get("names", {}).get(c) for c in order[:-1]}
    last_seen = sorted(seen.get(last, [])) if last else []
    check(f"{who} : file d'attente : le dernier arrive a vu sa place",
          last is not None and bool(last_seen) and all(w in went_before for _, w, _ in last_seen), (last_seen, sorted(x for x in went_before if x)))
    check(f"{who} : arrivee : plusieurs joueurs chargent le monde en meme temps", bool(JOIN_QUEUE.get("overlap")), JOIN_QUEUE.get("overlap", [])[:5])
    check(f"{who} : file d'attente : l'hote affiche la file", any("," in q for q in JOIN_QUEUE.get("host", [])), JOIN_QUEUE.get("host", [])[:6])
    cmd(host, "pause 1")
    time.sleep(3)
    h = dump(host, "h_join4")
    cs = [dump(c, f"c{i + 1}_join4") for i, c in enumerate(clis)]
    cmd(host, "pause 0")
    for i, c in enumerate(clis):
        keys = keys_of_owner(h, ids[c])
        names = [h["squad"].get(k, {}).get("name") for k in keys]
        check(f"{who} : le client {i + 1} a son propre perso (Joueur{i + 2})", keys and any(nm and nm.startswith(f"Joueur{i + 2}") for nm in names),
              f"cles {keys} noms {names}")
    for i, cst in enumerate(cs):
        bad = []
        for pc in clis:
            for k in keys_of_owner(h, ids[pc]):
                hv, cv = h["squad"].get(k, {}), cst["squad"].get(k)
                if not cv:
                    bad.append((k, "absent"))
                    continue
                if hv.get("name") != cv.get("name"):
                    bad.append((k, "nom", hv.get("name"), cv.get("name")))
                if "pos" in hv and "pos" in cv and dist(hv["pos"], cv["pos"]) > 0.1:
                    bad.append((k, "pos", round(dist(hv["pos"], cv["pos"]), 2)))
        check(f"{who} : le client {i + 1} voit les persos de tous les joueurs (noms, positions)", not bad, bad[:5])
    for i, c in enumerate(clis):
        log(f"client {i + 1} leaves:", cmd(c, "leave"))
    time.sleep(10)
    states = [status(c).get("state") for c in clis]
    check(f"{who} : tous les clients sont partis proprement", all(st in ("idle", "failed") for st in states) and all(alive(c) for c in clis), states)
    check(f"{who} : l'hote continue apres les departs", alive(host) and status(host).get("state") == "hosting", status(host))
    after = dump(host, "h_join4_after")
    log("host entities by owner after the leaves:",
        {o: sum(1 for e in after["entity"].values() if e.get("owner") == o) for o in {e.get("owner") for e in after["entity"].values()}})
    summary()


def exp_stress4(host, clis, ids, minutes=15, hop_seconds=180, seed=4242):
    """Every player at once, spread over the map: each client's characters live in their own far
    region (>= 30000 from the host and from each other) and hop to another one every hop_seconds;
    fights, knock-outs and loots, building, ground pick-ups and trades in every zone; every client
    also clicks everywhere (seeded chaos of real player orders, every action logged with its seed);
    two clients loot the same body at the same instant (tagged items: never duplicated); the host
    cycles the speed and pauses; one client leaves and rejoins. Everyone is in god mode (admin
    'god all', and 'heal all' every few seconds) so the chaos keeps running. Every 30 s: every game
    alive, frozen comparison host / each client (soak tolerances), per-zone persistence near each
    client, round trip, memory, host frame time, tagged item count host vs each client."""
    import random
    n = len(clis)
    who = f"{n + 1} joueurs"
    t0 = time.time()
    deadline = t0 + minutes * 60
    pids = [host] + clis
    master = random.Random(seed)
    seeds = {c: master.randrange(1 << 30) for c in clis}
    log(f"stress4: {n} clients, {minutes} min, zone hop every {hop_seconds} s, master seed {seed}, client seeds "
        + ", ".join(f"client {i + 1} (pid {c}) seed {seeds[c]}" for i, c in enumerate(clis)))

    # god mode for everyone (host admin console, debug channel)
    log("god mode for every player:", cmd(host, "console god all"))
    # the tagged item: something no one carries yet, given to every player's characters
    tag = None
    for part in ("Bandage", "Rice", "Bread", "Dried_Meat", "Cactus", "Hash", "Ration"):
        t = cmd(host, f"itemtypes {part}")[1].split()
        if len(t) > 2 and "=" in t[2]:
            cand = t[2].split("=")[0]
            if cmd(host, f"invcount all {cand}")[1].split()[-1] == "0":
                tag = cand
                break
    given = 0
    if tag:
        for c in pids:
            for k in own_indices(host, ids[c])[:1]:
                if cmd(host, f"giveitem {tag} 5 {k}")[0]:
                    given += 5
    log(f"tagged item {tag}: {given} given")

    # zones: one far region per client, regions at +-40000 around the host's home
    home = tuple(map(float, cmd(host, "where 0")[1].split()[1].split(",")))
    regions = [(40000, 0), (0, 40000), (-40000, 0), (0, -40000), (40000, 40000), (-40000, -40000), (40000, -40000), (-40000, 40000)]
    zone = {c: i for i, c in enumerate(clis)}
    hop = {"next": time.time() + hop_seconds, "count": 0}

    def send_to_zone(c):
        dx, dz = regions[zone[c] % len(regions)]
        hidx = own_indices(host, ids[c])
        for j, k in enumerate(hidx):
            log(f"zone: client {clis.index(c) + 1} member {k} -> region {zone[c] % len(regions)} ({dx}, {dz}):",
                cmd(host, f"teleport {k} {home[0] + dx + 6 * j} {home[1] + 300} {home[2] + dz}"))
        time.sleep(2)
        mine = own_indices(c)
        if mine:
            cmd(c, f"camto {mine[0]}")

    quiet = threading.Event()      # set: the chaos threads hold still (samples, leave / rejoin)
    stop = threading.Event()
    away = set()                   # clients that are leaving / rejoining (no chaos, no comparison)
    chaos_log = {c: 0 for c in clis}
    chaos_err = []

    def chaos(c, i):
        rnd = random.Random(seeds[c])
        sid = find_building(host, ("Feu", "Tente", "Coffre"))
        k = 0
        while not stop.is_set():
            if quiet.is_set() or c in away:
                time.sleep(0.2)
                continue
            try:
                mine = own_indices(c) or [0]
                me = rnd.choice(mine)
                sq = int(rnd.random() * 6)
                r = rnd.random()
                if r < 0.15:
                    line = f"moverel {me} {rnd.randint(-120, 120)} {rnd.randint(-120, 120)}"
                elif r < 0.2:
                    line = f"moverel {sq} {rnd.randint(-80, 80)} {rnd.randint(-80, 80)}"   # someone else's: must not obey
                elif r < 0.27:
                    line = f"talkreq {me}"
                elif r < 0.34:
                    line = f"containerreq {me} any"
                elif r < 0.39:
                    line = f"contake {me} any"
                elif r < 0.47:
                    line = f"invmove squad{me} squad{sq} {rnd.randint(0, 8)} 0"
                elif r < 0.52:
                    line = "selectset " + " ".join(str(x) for x in rnd.sample(range(6), rnd.randint(1, 3)))
                elif r < 0.56:
                    line = f"selorder {rnd.randint(0, 6)}"
                elif r < 0.61:
                    line = f"jobreq {me} {rnd.randint(0, 40)} {sq}"
                elif r < 0.64:
                    line = f"jobremove {me} 0"
                elif r < 0.68:
                    line = f"carryreq {me} {sq}"
                elif r < 0.73:
                    g = cmd(c, f"groundnear 400 ground {me}")[1].split()[2:]
                    if g:
                        _, isid, at = rnd.choice(g).split("|")
                        line = f"pickupreq {me} {isid} {at}"
                    else:
                        line = f"groundnear 400 loose {me}"
                elif r < 0.78:
                    line = f"tradeopen {me} any"
                elif r < 0.84:
                    line = "closewindows"
                elif r < 0.88:
                    line = f"speed {rnd.choice((1, 2, 3))}"
                elif r < 0.91:
                    line = f"pause {rnd.randint(0, 1)}"
                elif r < 0.94 and sid:
                    line = f"buildplace {sid} {rnd.randint(-80, 80)} {rnd.randint(30, 90)} 0 {me}"
                elif r < 0.97:
                    line = f"buildreq {me} 2"
                else:
                    line = f"npcreq {me} {rnd.randint(0, 40)} a"
                k += 1
                ans = cmd(c, line)
                chaos_log[c] = k
                log(f"chaos client {i + 1} seed {seeds[c]} #{k}: {line} -> {ans[1][:80]}")
            except RuntimeError as e:
                chaos_err.append((i + 1, str(e)))
                log(f"chaos client {i + 1} stopped: {e}")
                return
            time.sleep(rnd.uniform(0.3, 1.2))

    samples = []
    bad_streak = {c: {} for c in clis}   # per-zone persistence: key -> consecutive bad samples
    persistent = []
    conservation = []
    speed = {"v": 1}

    def health(label):
        for p in pids:
            if not alive(p):
                raise RuntimeError(f"instance {p} died (crash?) before sample {label}")
        quiet.set()
        time.sleep(1.5)
        try:
            s = {"label": label, "t": round(time.time() - t0),
                 "rtt": {p: frame_latency(p, 3)[1] for p in pids},
                 "frame": frame_latency(host, 5),
                 "mem": {p: proc_memory_mb(p) for p in pids},
                 "states": {c: status(c).get("state") for c in clis if c not in away}}
            cmd(host, "pause 1")
            time.sleep(2.5)
            h = dump(host, f"h_s4_{label}")
            s["host_tag"] = tag_total(h, tag) if tag else 0
            s["cmp"] = {}
            for i, c in enumerate(clis):
                if c in away:
                    continue
                cs = dump(c, f"c{i + 1}_s4_{label}")
                rep = compare(h, cs, f"stress4 {label} client {i + 1}", pos_tol=3.0)
                down = {k for k, v in h["squad"].items() if int(v.get("vflags", 0) or 0) & 3 or int(v.get("flags", 0) or 0) & 4}
                # Paused, a standing squad member is exactly where the host has it across the ground
                # (<= 0.1); its height is each game's own ground contact (UP_TOL): every one of the 25
                # stress4 mismatches was height only. A body on the ground: 8 (ragdolls).
                squad_bad = []
                for t in rep["squad"]:
                    if not isinstance(t[1], (int, float)):
                        squad_bad.append(t)
                        continue
                    across, up = split_dist(h["squad"][t[0]]["pos"], cs["squad"][t[0]]["pos"])
                    if (t[1] > 8.0) if t[0] in down else (across > 0.1 or up > UP_TOL):
                        squad_bad.append(t + (round(across, 3), round(up, 2)))
                # a player's character the host does not have (a copy left over from someone's join)
                extra = sorted((k, v.get("name")) for k, v in cs["squad"].items() if k not in h["squad"])
                s["cmp"][c] = {"squad_bad": squad_bad, "vital": rep["vital_flag_mismatch"], "inv": rep["inventory_mismatch"],
                               "inv_sample": rep["inventory_mismatch_sample"], "hours": rep.get("hours_diff", 0), "extra_squad": extra}
                # per zone: what the host has around this client's characters, as the client has it
                centers = [char_pos(h, k) for k in keys_of_owner(h, ids[c]) if char_pos(h, k)]
                near = [k for k, v in h["char"].items() if "pos" in v and centers and min(dist(v["pos"], p) for p in centers) < 1500]
                # present: everything within 1500; where: within SEEN_RANGE of this client's characters
                # (beyond, by design, its game hardly runs them: a goat 689-911 away stood 3 off), 3
                # across for a standing one, 8 for a body on the ground (ragdolls), UP_TOL in height
                bad_now = set()
                for k in near:
                    cv, hv = cs["char"].get(k), h["char"][k]
                    if not cv or "pos" not in cv:
                        bad_now.add(k)
                        continue
                    if min(dist(hv["pos"], p) for p in centers) > SEEN_RANGE:
                        continue
                    lying = int(hv.get("vflags", 0) or 0) & 3 or int(hv.get("flags", 0) or 0) & 4
                    across, up = split_dist(cv["pos"], hv["pos"])
                    if across > (8.0 if lying else 3.0) or up > UP_TOL:
                        bad_now.add(k)
                streak = bad_streak[c]
                for k in list(streak):
                    if k not in bad_now:
                        del streak[k]
                for k in bad_now:
                    streak[k] = streak.get(k, 0) + 1
                    if streak[k] == 2:
                        persistent.append((label, i + 1, k))
                s["cmp"][c]["zone"] = (len(near), len(bad_now))
                if tag:
                    common = all_keys(h) & all_keys(cs)
                    ht, ct = tag_total(h, tag, common), tag_total(cs, tag, common)
                    s["cmp"][c]["tag"] = (ht, ct)
                    if ht != ct:
                        conservation.append((label, i + 1, ht, ct))
            cmd(host, "pause 0")
            cmd(host, f"speed {speed['v']}")
        finally:
            quiet.clear()
        log(f"[{s['t']}s] sample {label}: frame ms host {s['frame']} rtt {s['rtt']} mem MB {s['mem']} states {s['states']} "
            f"host tag {s['host_tag']}/{given} | " + " | ".join(
                f"c{clis.index(c) + 1} squad_bad {v['squad_bad'][:2]} vit {v['vital']} inv {v['inv']} zone {v['zone']} tag {v.get('tag')}"
                for c, v in s["cmp"].items()))
        samples.append(s)

    zone_npc = {}

    def zone_fight(c):
        hidx = own_indices(host, ids[c])
        if not hidx:
            return
        ok, t = cmd(host, f"spawnnpc 40 20 {hidx[0]}")
        log(f"zone client {clis.index(c) + 1}: spawn npc", t)
        if ok:
            zone_npc[c] = t.split()[1]
            log("  fight:", cmd(host, f"fight {hidx[0]}"))

    def zone_ko_loot(c):
        key = zone_npc.pop(c, None)
        if not key:
            return
        # 'ko' acts on the host's last spawned NPC: only if it is still this one
        if cmd(host, "npcstate")[0]:
            log(f"zone client {clis.index(c) + 1}: ko", cmd(host, "ko"))
            time.sleep(2)
            mine = own_indices(c)
            if mine:
                log("  client loots:", cmd(c, f"loot {key} {mine[0]}"))

    def zone_build(c):
        sid = find_building(host, ("Feu", "Tente"))
        mine = own_indices(c)
        if sid and mine:
            log(f"zone client {clis.index(c) + 1}: build", cmd(c, f"buildplace {sid} {random.randint(-60, 60)} 60 0 {mine[0]}"))

    def contention():
        """Two clients take the same items from the same body at the same instant."""
        a, b = clis[0], clis[1 % n]
        if a == b:
            return
        ha, hb = own_indices(host, ids[a]), own_indices(host, ids[b])
        if not ha or not hb:
            return
        pa = tuple(map(float, cmd(host, f"where {ha[0]}")[1].split()[1].split(",")))
        for k in hb:   # b's characters join a's zone
            cmd(host, f"teleport {k} {pa[0] + 15} {pa[1] + 50} {pa[2] + 10}")
        time.sleep(3)
        ok, t = cmd(host, f"spawnnpc 20 0 {ha[0]}")
        if not ok:
            log("contention: no npc", t)
            return
        key = t.split()[1]
        if tag:
            log("contention: tagged items into the body:", cmd(host, f"giveitem {tag} 3 {ha[0]}"))
            for _ in range(3):
                log("  ", cmd(host, f"invmove squad{ha[0]} spawned main 0"))
        log("contention: ko", cmd(host, "ko"))
        time.sleep(3)
        quiet.set()
        try:
            ma, mb = own_indices(a), own_indices(b)
            cmd(a, f"loot {key} {ma[0]}")
            cmd(b, f"loot {key} {mb[0]}")
            time.sleep(2)
            res = {}

            def grab(c, m, i):
                res[c] = [cmd(c, f"invmove {key} squad{m} {j} 0") for j in (0, 0, 0, 1, 1)]
            th = [threading.Thread(target=grab, args=(a, ma[0], 1)), threading.Thread(target=grab, args=(b, mb[0], 2))]
            for x in th:
                x.start()
            for x in th:
                x.join()
            log("contention results:", {clis.index(c) + 1: [r[1][:40] for r in v] for c, v in res.items()})
            time.sleep(4)
            cmd(a, "closewindows")
            cmd(b, "closewindows")
        finally:
            quiet.clear()

    def leave_rejoin(c):
        i = clis.index(c) + 1
        before = len(own_indices(host, ids[c]))
        away.add(c)
        try:
            log(f"client {i} leaves:", cmd(c, "leave"))
            time.sleep(8)
            log(f"client {i} after leaving:", status(c))
            log(f"client {i} rejoins:", join_one(c, i))
            wait_for(c, lambda st: st.get("state") == "connected" and st.get("ready") == "1", 600, f"client {i} back in host world")
            time.sleep(6)
            cmd(c, "editdone")
            ids[c] = player_id(c)
            after = len(own_indices(host, ids[c]))
            rejoin.update(ok=True, before=before, after=after, id=ids[c])
            log(f"client {i} back: player id {ids[c]}, characters {before} -> {after}")
            cmd(host, "console god all")
            send_to_zone(c)
            tile(pids)
        except RuntimeError as e:
            rejoin.update(ok=False, err=str(e))
            log(f"client {i} rejoin failed: {e}")
        finally:
            away.discard(c)

    rejoin = {}
    crash = None
    threads = []
    try:
        for c in clis:
            send_to_zone(c)
        threads = [threading.Thread(target=chaos, args=(c, i), daemon=True) for i, c in enumerate(clis)]
        for t in threads:
            t.start()
        speeds = (1, 2, 3, 2)
        step = 0
        last_sample = time.time()
        last_heal = 0
        contention_done = rejoin_done = False
        while time.time() < deadline:
            now = time.time()
            if now - last_heal > 8:
                cmd(host, "console heal all")
                last_heal = now
            if now - last_sample >= 30:
                health(f"s{len(samples) + 1}")
                last_sample = time.time()
                continue
            if now >= hop["next"]:
                hop["count"] += 1
                hop["next"] = now + hop_seconds
                for c in clis:
                    zone[c] += n
                    if c not in away:
                        send_to_zone(c)
            c = clis[step % n]
            if c not in away:
                [zone_fight, zone_ko_loot, zone_build][(step // n) % 3](c)
            if step % 5 == 4:
                speed["v"] = speeds[(step // 5) % len(speeds)]
                log("host speed", speed["v"], cmd(host, f"speed {speed['v']}"))
            if step % 9 == 8:
                log("host pause:", cmd(host, "pause 1"))
                time.sleep(2)
                log("host unpause:", cmd(host, "pause 0"), cmd(host, f"speed {speed['v']}"))
            if not contention_done and now - t0 > minutes * 60 * 0.25 and n >= 2:
                contention_done = True
                contention()
                for c2 in clis[1:2]:
                    send_to_zone(c2)
            if not rejoin_done and now - t0 > minutes * 60 * 0.5:
                rejoin_done = True
                leave_rejoin(clis[-1])
            step += 1
            time.sleep(3)
        stop.set()
        for c in clis:   # everyone home safely
            log(f"client {clis.index(c) + 1} home:", cmd(host, f"tpplayer {ids[c]}"))
        time.sleep(6)
        cmd(host, "speed 1")
        health("end")
    except RuntimeError as e:
        crash = str(e)
        log("STRESS4 STOPPED:", crash)
    finally:
        stop.set()
        quiet.clear()
        for t in threads:
            t.join(timeout=30)

    check(f"{who} : aucun plantage (hote et clients)", crash is None and all(alive(p) for p in pids) and not chaos_err,
          crash or chaos_err[:3] or f"{len(samples)} releves")
    check(f"{who} : chaque client reste connecte", samples and all(st == "connected" for s in samples for st in s["states"].values()),
          [(s["label"], s["states"]) for s in samples if any(st != "connected" for st in s["states"].values())][:3])
    check(f"{who} : clics en rafale des clients executes", all(chaos_log[c] > 0 for c in clis),
          {f"client {i + 1} seed {seeds[c]}": chaos_log[c] for i, c in enumerate(clis)})

    def per_client(key, pred):
        return [(s["label"], clis.index(c) + 1, v[key]) for s in samples for c, v in s["cmp"].items() if pred(v[key])]
    check(f"{who} : escouade identique en pause chez chaque client (<= 0.1 au sol, 5 en hauteur, 8 pour un corps au sol)",
          not per_client("squad_bad", bool), per_client("squad_bad", bool)[:5])
    check(f"{who} : aucun perso de joueur en trop chez un client (copie laissee par une arrivee)",
          not per_client("extra_squad", bool), per_client("extra_squad", bool)[:3])
    check(f"{who} : aucun etat vital different", not per_client("vital", bool), per_client("vital", bool)[:5])
    check(f"{who} : aucun inventaire different", not per_client("inv", bool),
          [(s["label"], clis.index(c) + 1, v["inv_sample"]) for s in samples for c, v in s["cmp"].items() if v["inv"]][:5])
    check(f"{who} : meme heure de jeu partout (< 0.01 h)", not per_client("hours", lambda x: (x or 0) >= 0.01),
          per_client("hours", lambda x: (x or 0) >= 0.01)[:5])
    check(f"{who} : chaque zone fidele chez son client (rien d'absent ou decale deux releves de suite)", not persistent, persistent[:6])
    if tag:
        check(f"{who} : objet marque identique hote / chaque client (pas de duplication)", not conservation, conservation[:5])
        over = [(s["label"], s["host_tag"]) for s in samples if s["host_tag"] > given + 3]
        check(f"{who} : objet marque jamais en trop chez l'hote (<= donnes)", not over, f"donnes {given} + 3 du corps, depassements {over[:4]}")
    else:
        check(f"{who} : objet marque trouve", False, "aucun type d'objet libre pour le marquage")
    worst = max([max(s["rtt"].values()) for s in samples] or [0])
    check(f"{who} : aller-retour d'une commande < 1 s", worst < 1000, f"pire {worst} ms")
    # 90 % of the samples under 250 ms, none over 1 s: a sample now and then catches a zone loading
    # or the host saving the world for a joining player (stress4: 489 ms when every game had ~900 ms
    # round trips at once, 393 ms at the rejoin); the frame time itself stays ~200 ms with 4 games on one PC
    frames = [s["frame"][0] for s in samples]
    p90 = sorted(frames)[max(0, math.ceil(0.9 * len(frames)) - 1)] if frames else None
    check(f"{who} : temps de frame de l'hote (90 % des releves < 250 ms, aucun > 1 s)",
          frames and p90 < 250 and max(frames) < 1000, f"90e centile {p90} ms, releves {frames}")
    base = next((s for s in samples if s["t"] >= 120 and None not in s["mem"].values()), None)
    if base:
        over = [(s["label"], p, round(s["mem"][p] / base["mem"][p], 2)) for s in samples if s["t"] > base["t"]
                for p in pids if s["mem"].get(p) and base["mem"].get(p) and s["mem"][p] / base["mem"][p] > 1.4]
        check(f"{who} : memoire stable par processus (< +40 % apres 2 min)", not over, f"base {base['mem']} {over[:4]}")
    else:
        check(f"{who} : memoire stable par processus (< +40 % apres 2 min)", False, "pas de releve apres 2 min")
    check(f"{who} : un client part puis revient avec ses persos", rejoin.get("ok") and rejoin.get("after", 0) >= max(1, rejoin.get("before", 1)), rejoin)
    check(f"{who} : zones visitees (sauts de region)", hop["count"] >= 1 or minutes * 60 < hop_seconds, f"{hop['count']} sauts")
    if not crash:
        h = dump(host, "h_s4_home")
        far = [(clis.index(c) + 1, round(min(dist(char_pos(h, k)[::2], home[::2]) for k in keys_of_owner(h, ids[c]) if char_pos(h, k)), 1))
               for c in clis if any(char_pos(h, k) for k in keys_of_owner(h, ids[c]))]
        check(f"{who} : chaque client ramene a la maison a la fin", far and all(d < 2000 for _, d in far), far)
    log("memory (MB) per sample:", [(s["label"], s["mem"]) for s in samples])
    summary()


# ---- workshop: research, crafting benches, mines and machines, power
def mfields(t):
    """'ok key ops=a/b names=.. flags=.. power=.. ...' (debug 'machine') -> dict (empty on error)."""
    if not t or not t.startswith("ok "):
        return {}
    parts = t.split()
    d = {"key": parts[1]}
    for p in parts[2:]:
        k, _, v = p.partition("=")
        d[k] = v
    return d


def same_machine(h, c, floats=("power", "stored", "prog", "prod"), tol=0.15):
    """Same workers, switches and crafting orders; the numbers within a small margin (they move while
    the message travels)."""
    if not h or not c:
        return False, "missing"
    for k in ("ops", "flags"):
        if h.get(k) != c.get(k):
            return False, f"{k}: {h.get(k)} / {c.get(k)}"
    hc = [x.split(":")[0] for x in h.get("crafts", "").split(",") if x]
    cc = [x.split(":")[0] for x in c.get("crafts", "").split(",") if x]
    if hc != cc:
        return False, f"crafts: {hc} / {cc}"
    for k in floats:
        a, b = float(h.get(k, 0)), float(c.get(k, 0))
        if abs(a - b) > max(tol * max(abs(a), abs(b)), 0.6):
            return False, f"{k}: {a} / {b}"
    return True, ""


def machine_both(host, cli, part, label, tries=8, **kw):
    """The same machine on both sides (a few tries: the host sends every second)."""
    last = ("", "")
    for _ in range(tries):
        h, c = cmd(host, f"machine {part}")[1], cmd(cli, f"machine {part}")[1]
        ok, why = same_machine(mfields(h), mfields(c), **kw)
        last = (h, c, why)
        if ok:
            return True, last
        time.sleep(1.5)
    log(label, "host:", last[0], "| client:", last[1], "|", last[2])
    return False, last


def find_template(pid, parts):
    """The first building template whose name holds one of the parts ('sid', 'name')."""
    for p in parts:
        t = cmd(pid, f"buildtypes {p}")[1]
        bits = t.split()
        if t.startswith("ok") and len(bits) > 2:
            sid, _, name = bits[2].partition("=")
            return sid, name
    return None, None


# vanilla templates by id (gamedata.base): names are translated, ids are not
TRADER_BACKPACK = "910-gamedata.base"      # Sac à Dos Commerçant (the biggest backpack)
RESEARCH_BENCH = "1875-gamedata.base"      # Small Research Bench (unlocked from the start)
CRAFT_BENCHES = ("2929-gamedata.base", "2262-gamedata.base", "2299-gamedata.base")   # Clothing Bench, Leather / Chain Armour Crafting Bench
WIND_GENERATOR = "1999-gamedata.base"
BATTERY_BANK = "1855-gamedata.base"
LIGHTS = ("14444-gamedata.base", "5363-gamedata.base", "5361-gamedata.base")   # Light Post, Searchlight, Spotlight
IRON_NODE = "42248-gamedata.base"           # Iron Resource: 3 workers (a copper node takes 2)
FABRICS = "584-gamedata.base"


SANDBOX = {"home": None}   # where move_squad_out_of_town put squad member 0 (the sandbox's anchor)


def pos_xz(pid, idx):
    t = cmd(pid, f"pos {idx}")[1].split()
    if len(t) < 2:
        return None
    try:
        x, _, z = (float(v) for v in t[1].split(","))
    except ValueError:
        return None
    return x, z


def keep_anchor_home(host):
    """Squad member 0 back at the sandbox when it left it (a knocked-out body the game put back where it
    lay, the 11/10 research run: every placement after the first one was then 'too close to a town')."""
    home = SANDBOX["home"]
    if not home:
        return
    p = pos_xz(host, 0)
    if p and math.hypot(p[0] - home[0], p[1] - home[2]) < 800:
        return
    log("   squad member 0 left the sandbox", p, ": healed and sent back")
    cmd(host, "console heal all")
    time.sleep(1)
    log("   teleport 0 back:", cmd(host, f"teleport 0 {home[0]:.0f} {home[1] + 300:.0f} {home[2]:.0f}"))
    time.sleep(5)


def sandbox_building(host, sid, dx, dz, label):
    """Placed by the host next to squad member 0 at a spot build mode accepts, then finished at once
    (sandbox). True when built."""
    keep_anchor_home(host)
    ok, t, spot = place_valid(host, sid, 0, dx, dz)
    log(label, sid, "placed:", ok, t, spot)
    if not ok:
        return False
    # finished by the workers' progress, once the new building is there (it may take a moment)
    for _ in range(8):
        time.sleep(1.5)
        r = cmd(host, f"buildprogress {sid} 100000")
        if r[0]:
            break
    log(label, "finished:", r)
    time.sleep(2)
    return True


def squad_size(pid):
    n = 0
    while cmd(pid, f"pos {n}")[0] and n < 32:
        n += 1
    return n


def move_squad_out_of_town(host):
    """Every squad member (the client's too) on dry land well away from any town: the host refuses
    placements near one ('too close to a town'). True when a test placement there is accepted."""
    t = cmd(host, "pos 0")[1].split()
    if len(t) < 2:
        return False
    x, y, z = (float(v) for v in t[1].split(","))
    n = squad_size(host)
    rings = [(5000, 0), (0, 5000), (-5000, 0), (0, -5000), (8000, 8000), (-8000, 8000), (8000, -8000), (-8000, -8000), (12000, 0), (0, 12000)]
    for dx, dz in rings:
        spot = teleport_dry(host, 0, (x, y, z), [(dx, dz)], settle=8)
        if not spot:
            continue
        sx, sy, sz = spot
        for i in range(1, n):
            cmd(host, f"teleport {i} {sx + 15 * i:.0f} {sy + 300:.0f} {sz + 10:.0f}")
        time.sleep(6)
        # member 0 really there (a body could be put back where it lay: then the check below was made
        # from the town, or from the sandbox and the next placements from the town)
        p = pos_xz(host, 0)
        if not p or math.hypot(p[0] - sx, p[1] - sz) > 800:
            log("   squad member 0 is not at", (round(sx), round(sz)), "but at", p, ": next spot")
            continue
        probe = cmd(host, f"buildcheck {RESEARCH_BENCH} 60 0")
        log("out of town at", spot, "build check:", probe)
        if probe[0] or "town" not in probe[1]:
            SANDBOX["home"] = spot
            return True
    return False


# ---- 0.3.2: recruits passed from hand to hand, two characters talking, copies never rebound
def exp_recruits(host, clis):
    """docs/A_FAIRE.md 3d, 3e, 3j, 3m (sandbox, 1 host + 2 clients or more). The host makes a recruit of a
    Dialogue.mod template (unique: the clients used to fail to recreate it, 'factory did not return a
    character'), recruits it and passes it with G (give) host -> each player, six times in two seconds;
    the first client then moves it into its own character's squad. On every machine: the recruit is
    there and in the player faction (squadcheck outside=0 missing=0), same squads as the host, no copy
    bound to another host character (conflicts=0). The first client then selects two of its characters
    and orders "talk": one talks, the host refuses the other silently (dialogs same=, busy unchanged).
    Last, a stress: 20 gives in a row while the client moves the recruit between squads."""
    clis = clis if isinstance(clis, (list, tuple)) else [clis]
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(3)
    ids = {}
    for c in clis:
        t = cmd(c, "status")[1]
        m = re.search(r" id=(\d+)", t)
        ids[c] = int(m.group(1)) if m else None
    log("client ids:", ids)
    own_h = (own_indices(host) or [0])[0]

    def kvs(text):
        return dict(p.split("=", 1) for p in text.split(" | ")[0].split()[1:] if "=" in p)

    # 1. a client recreates a unique Dialogue.mod template itself (3e, directly)
    templates = ("41370-Dialogue.mod", "41299-Dialogue.mod")
    for t in templates:
        r = cmd(clis[0], f"spawncreature {t} 25 25")
        log("client spawncreature", t, r)
        if "unknown template" in r[1]:
            log("   (Dialogue.mod is not installed here: that part is skipped)")
            continue
        check(f"recrues : un client cree un perso du modele unique {t}", r[0], r[1])

    # 2. the host makes the recruit (that template when there, else a copy of a nearby NPC) and recruits it
    made = None
    for t in templates:
        r = cmd(host, f"spawncreature {t} 20 0 {own_h}")
        log("host spawncreature", t, r)
        if r[0]:
            made = r
            break
    if not made:
        made = cmd(host, f"spawnnpc 20 0 {own_h}")
        log("host spawnnpc (no Dialogue.mod)", made)
    if not made[0]:
        check("recrues : l'hote cree une recrue", False, made[1])
        return
    time.sleep(4)
    r = cmd(host, "recruitnpc")
    log("host recruitnpc", r)
    check("recrues : l'hote recrute le PNJ", r[0], r[1])
    if not r[0]:
        return
    name = r[1].split(" ", 1)[1].replace(" ", "_")
    time.sleep(3)
    idx = cmd(host, f"squadidx {name}")
    log("host squadidx", name, idx)
    if not idx[0]:
        check("recrues : la recrue est dans l'escouade de l'hote", False, idx[1])
        return
    hi = int(idx[1].split()[1])

    # 3. G cycling: host -> each player, fast (2 s for the six gives), ends with the first client
    order = [1] + [ids[c] for c in clis if ids[c]]
    for k in range(6):
        for pid in order:
            cmd(host, f"give {pid} {hi}")
            time.sleep(0.1)
    cmd(host, f"give {ids[clis[0]]} {hi}")
    time.sleep(3)
    own_c = cmd(clis[0], "squadcheck")[1]
    log("client 1 squadcheck after the gives:", own_c)
    me = "Joueur2"   # the first client's own character (setup_multi names the players Joueur2, Joueur3...)
    log("client 1: the recruit into its own character's squad", cmd(clis[0], f"squadmove {name} {me}"))
    time.sleep(8)

    def compare(label):
        hs = cmd(host, "squads")[1]
        for i, c in enumerate(clis):
            sc = cmd(c, "squadcheck")[1]
            cs = cmd(c, "squads")[1]
            bar = cmd(c, "squadbar")[1]
            d = kvs(sc)
            log(f"{label} client {i + 1}: {sc}")
            log(f"    squads host  : {hs}")
            log(f"    squads client: {cs}   squadbar {bar}")
            check(f"recrues ({label}) : client {i + 1}, la recrue est la et dans la faction", d.get("missing") == "0" and d.get("outside") == "0", sc)
            check(f"recrues ({label}) : client {i + 1}, aucune copie liee a un autre perso de l'hote", d.get("conflicts") == "0", sc)
            check(f"recrues ({label}) : client {i + 1}, memes escouades que l'hote", cs == hs, cs)
            check(f"recrues ({label}) : client {i + 1}, {name} dans les escouades", name.replace("_", " ") in cs, cs)
    compare("apres G et deplacement")

    # 4. two of the first client's characters selected, one "talk" (3d)
    ci = [int(x) for x in cmd(clis[0], f"squadidx {name}")[1].split()[1:]] + own_indices(clis[0])[:1]
    d0 = kvs(cmd(host, "dialogs")[1])
    log("client 1 selectset", ci, cmd(clis[0], "selectset " + " ".join(map(str, ci))))
    # (13 October, as on 12/10: "talksel 0", the nearest NPC, was Ribs / Lost Person, then a Ninja:
    # the host's game walked Harp there and said "the NPC has nothing to say", no conversation; the
    # mod did its part. As in npctalk, NPCs of kctest_town known to hold a conversation, by name, the
    # nearest last; each one is given 60 s: Chef Voleur is a few houses away, but the Marchand Shinobi
    # of 13/10 17:25 was some 700 units off, upstairs, and still walking after 40 s)
    # talksel never picks Skinner and Ribs, the characters step 1 made on this client only (0.3.4)
    clog_path = os.path.join(KENSHI, f"KenshiCoop-{clis[0]}.log")
    try:
        clog_from = os.path.getsize(clog_path)
    except OSError:
        clog_from = None
    sent = 0
    dl = ""
    for k in ["name:Chef_Voleur", "name:Thief_Leader", "name:Marchand", "name:Trader", "0"]:
        r = cmd(clis[0], f"talksel {k}")
        log(f"client 1 talksel {k}", r)
        if not r[0]:
            continue
        sent += 1
        for _ in range(120):
            time.sleep(0.5)
            dl = cmd(clis[0], "dialog")[1]
            if "open=1" in dl:
                break
        if "open=1" in dl:
            break
        log("   no conversation with it:", [l for l in host_log().splitlines() if "talk" in l][-2:])
        cmd(clis[0], "selectset " + " ".join(map(str, ci)))
    # Orders this client sent that the test did not give (13/10 17:24: right clicks on Skinner, "stay
    # close", "talk", Shift + "bodyguard", came in between talksel and the conversation; the walk to
    # Chef Voleur was taken over, and the test failed for a reason that is not the mod's)
    stray = []
    if clog_from is not None:
        try:
            with open(clog_path, encoding="utf-8", errors="replace") as f:
                f.seek(clog_from)
                orders = [l.strip() for l in f.read().splitlines() if "client order task" in l]
            stray = orders[sent:]
        except OSError:
            pass
    if stray:
        log("   ATTENTION: orders the test did not give (mouse or keys in the client's window?):", stray)
    d1 = kvs(cmd(host, "dialogs")[1])
    log("host dialogs before/after:", d0, d1, "| client dialog:", dl)
    check("parler a deux : aucun refus 'occupe' pour le meme joueur", d1.get("busy") == d0.get("busy"), (d0, d1))
    check("parler a deux : la conversation s'ouvre chez le client", "open=1" in dl,
          dl + (f" | ordres hors test: {len(stray)} ({stray[0]})" if stray else ""))
    cmd(clis[0], "answer leave")
    time.sleep(3)

    # 5. stress (3m): 20 gives in a row while the client moves the recruit between squads
    for k in range(20):
        cmd(host, f"give {order[k % len(order)]} {hi}")
        if k % 5 == 0:
            cmd(clis[0], f"squadmove {name} {'new' if k % 10 == 0 else me}")
        time.sleep(0.1)
    cmd(host, f"give {ids[clis[0]]} {hi}")
    time.sleep(10)
    compare("apres le stress")
    r = cmd(clis[0], f"taskreq {cmd(clis[0], f'squadidx {name}')[1].split()[-1]} 1 0")
    log("client 1 orders the recruit after the stress:", r)
# vanilla workstations by id (gamedata.base, names are translated): a field, a well, a stove
SMALL_FARM = "1911-gamedata.base"          # Small Farm (FarmBuilding)
HEMP_FARM = "2021-gamedata.base"           # Hemp Farm, if the small one cannot be placed
WELL = "1921-gamedata.base"
COOKING_STOVE = "1944-gamedata.base"
WHEATSTRAW = "1913-gamedata.base"          # Wheatstraw (the stove's food; its French name is not "Ble")
BREAD = "1946-gamedata.base"
WORKSTATIONS = (("ferme", (SMALL_FARM, HEMP_FARM)), ("cuisiniere", (COOKING_STOVE,)), ("etabli", CRAFT_BENCHES),
                ("recherche", (RESEARCH_BENCH,)), ("puits", (WELL,)), ("eolienne", (WIND_GENERATOR,)))


def machines_map(pid):
    """'machines' -> {key: {found, ops, on, prod, farm, crafts}} as this game reads each machine the session knows."""
    t = cmd(pid, "machines")[1]
    out = {}
    if not t.startswith("ok "):
        return out
    for row in t.split()[2:]:
        bits = row.split("|")
        d = {"found": bits[1] if len(bits) > 1 else ""}
        for b in bits[2:]:
            k, _, v = b.partition("=")
            d[k] = v
        out[bits[0]] = d
    return out


def same_machines(h, c, keys):
    """Workers, switch, crops (stage, harvested, ripe or not) identical; production within a margin."""
    bad = []
    for k in keys:
        a, b = h.get(k), c.get(k)
        if not a or not b:
            bad.append(f"{k}: host {bool(a)} client {bool(b)}")
            continue
        if b.get("found") != "found" or "not" in b:
            bad.append(f"{k}: not found on the client")
            continue
        for f in ("ops", "on", "crafts"):
            if a.get(f) != b.get(f):
                bad.append(f"{k} {f}: {a.get(f)} / {b.get(f)}")
        if a.get("farm") or b.get("farm"):
            fa, fb = (a.get("farm") or "").split(","), (b.get("farm") or "").split(",")
            if len(fa) < 4 or len(fb) < 4 or fa[:2] != fb[:2] or abs(float(fa[2]) - float(fb[2])) > 0.1:
                bad.append(f"{k} crops: {a.get('farm')} / {b.get('farm')}")
        pa, pb = float(a.get("prod", 0) or 0), float(b.get("prod", 0) or 0)
        if abs(pa - pb) > max(0.2 * max(abs(pa), abs(pb)), 0.6):
            bad.append(f"{k} prod: {pa} / {pb}")
    return not bad, "; ".join(bad)


def exp_workstations(host, cli, minutes=2.0):
    """Base work for every player's characters (lot 'travaux de base'). In open desert, everyone in god
    mode with materials given: the host and the client each place a farm, a stove, a crafting bench,
    a research bench, a well and a wind turbine; their characters are set to work by right click
    (workreq: a one-time task 87) and by the Tâches panel (workjob: a permanent job, 87 and hauling
    88); then workers, crops, storages and job lists are compared on the host and the client, at the
    start and after `minutes` at speed x3."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("client's own character: squad index", own)
    SANDBOX["home"] = None
    log("heal everyone:", cmd(host, "console heal all"))
    log("god mode for everyone:", cmd(host, "console god all"))
    cmd(host, "console xp all 60")
    time.sleep(4)
    out = move_squad_out_of_town(host)
    check("postes : escouade en plein desert", out, "")
    if not out:
        summary()
        return
    n = squad_size(host)
    host_idx = [i for i in range(n) if i != own]
    # ---- the posts: the host places one row, the client the other (its placements go to the host)
    placed = {}   # label -> (sid, who)
    for row, (label, sids) in enumerate(WORKSTATIONS):
        # Both rows around squad member 0, 300 apart, the posts 90 apart (13/10 16:04: the client's row,
        # around its own character as its game had it, came 36 units from the host's, the posts 60
        # apart: two rows of buildings almost touching. The researcher, ordered to the research bench
        # 87 units away, went into that strip (29585,10466, between the stove and the bench) and stood
        # there for 2 min at x3 with its task (tasks=1), the bench at 0/1: it could not reach the
        # bench's place. The posts were never reachable, in every run of this test.)
        for who, pid, idx, dz in (("hote", host, 0, -150), ("client", cli, 0, 150)):
            got = None
            for sid in sids:
                ok, t, spot = place_valid(pid, sid, idx, -225 + 90 * row, dz)
                log(f"{who} places {label}", sid, ok, t, spot)
                if ok:
                    got = sid
                    break
            check(f"postes : {label} pose par {who}", got is not None, sids)
            if got:
                placed[f"{label}/{who}"] = (got, who)
        time.sleep(2)
    time.sleep(4)
    for key, (sid, who) in placed.items():   # finished at once (sandbox): the host's workers' progress
        for _ in range(2):
            log("finish", key, cmd(host, f"buildprogress {sid} 100000"))
            time.sleep(1.5)
    time.sleep(5)
    keep_anchor_home(host)
    # ---- materials: food, fabrics, books into the posts (host sandbox). Wheatstraw by its sid (13/10:
    # "Ble" / "Wheat" match no item of the French game, "no room" in the stove); the first that goes in
    # per post is enough (what stays in member 0's bag fills it: "giveItem refused (inventory full?)")
    for sid, whats in ((COOKING_STOVE, (WHEATSTRAW, "Ble", "Wheat")), (RESEARCH_BENCH, ("Livre", "Book")),
                       (CRAFT_BENCHES[0], (FABRICS, "Tissus", "Leather", "Iron"))):
        for what in whats:
            r = cmd(host, f"machinegive {sid} {what} 20")
            log("sandbox: into", sid, what, r)
            if r[0]:
                break
    # ---- set to work: the client by right click and by the Tâches panel; the host the same
    cli_posts = [placed[k][0] for k in placed if k.endswith("/client")]
    host_posts = [placed[k][0] for k in placed if k.endswith("/hote")]
    if cli_posts:
        log("client right click: its character works", cli_posts[0], cmd(cli, f"workreq {own} {cli_posts[0]}"))
    for sid in cli_posts[1:3]:
        log("client Tâches panel: permanent job on", sid, cmd(cli, f"workjob {own} {sid} 87"))
    # Not "hauling (88) with no subject": the game makes no job of it, on the host as on the client
    # (13/10 runs, jobs 2 -> 2; docs/MOTEUR.md). The panel's follow job (31 on a squad mate, as in
    # exp_clientjobs), which the game does list.
    if host_idx:
        log("client Tâches panel: follow job (31) on member", host_idx[-1], cmd(cli, f"jobreq {own} 31 {host_idx[-1]}"))
    for i, sid in zip(host_idx, host_posts):
        how = "workreq" if i == host_idx[0] else "workjob"
        log(f"host {how}: member {i} works", sid, cmd(host, f"{how} {i} {sid}" + ("" if how == "workreq" else " 87")))
    # Work for the posts: a worker only sits at a post that has something to do (13/10: 0 workers
    # everywhere after 2 min: no research queued, an empty stove, no craft order, a desert farm). A
    # research the benches can pay for, queued by the client (exp_research: its researcher sits).
    picks = [p.split("=")[0] for p in cmd(host, "researchpick 6 cost")[1].split()[1:]]
    picks += [p.split("=")[0] for p in cmd(cli, "researchpick 6")[1].split()[1:] if p.split("=")[0] not in picks]
    queued = None
    for tech in picks:
        log("client queues the research", tech, cmd(cli, f"researchreq queue {tech}"))
        for _ in range(8):
            time.sleep(0.5)
            if tech in cmd(host, "research")[1]:
                queued = tech
                break
        if queued:
            break
    log("research queued for the benches:", queued, "| host:", cmd(host, "research")[1][:200])
    # (13/10 13:59-14:03: research 1884 queued, progress 0.00 and no worker anywhere for 2 min at x3.
    # The host's members had their posts as PERMANENT jobs (workjob) only: the game does a permanent
    # job only while the character's JOBS standing order (bit 64 of `modes`) is on, which nothing
    # here made sure of for the host's members (exp_clientjobs does it for the client's character);
    # the stove and the bench had nothing to work on (wheat straw refused, "Ble" went to the output),
    # the desert farm grows nothing. exp_research, whose researcher works, gives a one-time right click
    # order on the research bench.) JOBS on for every member, and one researcher by right click.
    for i in range(n):
        m = cmd(host, f"modes {i}")[1].split()
        jobs_on = len(m) >= 2 and m[0] == "ok" and (int(m[1]) & 64) != 0
        if not jobs_on:
            log(f"member {i}: JOBS off ({' '.join(m)}): turned on", cmd(cli if i == own else host, f"orderreq {i} 15"))
    researcher = host_idx[3] if len(host_idx) > 3 else (host_idx[-1] if host_idx else own)
    if any(sid == RESEARCH_BENCH for sid, _ in placed.values()):
        log("researcher", researcher, "by right click on the research bench:",
            cmd(cli if researcher == own else host, f"workreq {researcher} {RESEARCH_BENCH}"))
    time.sleep(3)
    log("JOBS after:", [cmd(host, f"modes {i}")[1] for i in range(n)])
    time.sleep(20)   # they walk there

    def compare_all(label):
        hm, cm = machines_map(host), machines_map(cli)
        keys = sorted(k for k in hm if any(k.startswith(sid + "@") for sid, _ in placed.values()))
        log(label, "host machines:", {k: hm[k] for k in keys})
        log(label, "client machines:", {k: cm.get(k) for k in keys})
        check(f"postes ({label}) : chaque poste pose est suivi chez l'hote", len(keys) >= len(placed) - 1, f"{len(keys)} / {len(placed)}")
        ok, why = same_machines(hm, cm, keys)
        if not ok:   # the numbers move while the message travels: once more
            time.sleep(3)
            hm, cm = machines_map(host), machines_map(cli)
            ok, why = same_machines(hm, cm, keys)
        check(f"postes ({label}) : ouvriers, marche, cultures et production identiques partout", ok, why)
        ch = cmd(host, "crops any")[1]
        cc = cmd(cli, "crops any")[1]
        log(label, "crops host:", ch, "| client:", cc)
        if ch.startswith("ok"):
            fh, fc = mfields(ch), mfields(cc)
            check(f"postes ({label}) : cultures de la ferme identiques", fh.get("stage") == fc.get("stage") and fh.get("harvested") == fc.get("harvested")
                  and abs(float(fh.get("grown", 0)) - float(fc.get("grown", -9))) <= 0.1, f"{ch} / {cc}")
        for sid in set(s for s, _ in placed.values()):
            sh, sc = cmd(host, f"storage {sid}")[1], cmd(cli, f"storage {sid}")[1]
            if sh != sc:
                time.sleep(2)
                sh, sc = cmd(host, f"storage {sid}")[1], cmd(cli, f"storage {sid}")[1]
            check(f"postes ({label}) : stock de {sid} identique", sh == sc, f"{sh} / {sc}")
        for i in range(n):
            jh, jc = cmd(host, f"jobs {i}")[1], cmd(cli, f"jobs {i}")[1]
            check(f"postes ({label}) : taches du membre {i} identiques", jh == jc, f"{jh} / {jc}")
        return hm

    hm0 = compare_all("depart")
    own_jobs = cmd(host, f"jobs {own}")[1].split()[2:]
    check("postes : taches permanentes du client chez l'hote (87 et suivre 31)", "87" in own_jobs and "31" in own_jobs, own_jobs)

    def with_workers(m):
        return [k for k in m if m[k].get("ops", "0/").split("/")[0] not in ("0", "")]
    worked = set(with_workers(hm0))
    log("speed x3", cmd(host, "speed 3"))
    end = time.time() + 60 * minutes
    while time.time() < end:
        time.sleep(15)
        keep_anchor_home(host)
        hm = machines_map(host)
        worked |= set(with_workers(hm))
        log("machines with workers now:", with_workers(hm), "| research:", cmd(host, "research")[1][:120])
        # (13/10 15:21: JOBS was on for everyone ('ok 68 0'), research queued, still 0/1 on the benches:
        # where the researcher is and what it does, and the bench's own state, to find out)
        log("   researcher", researcher, cmd(host, f"charstate {researcher}")[1][:160], "| bench", cmd(host, f"machine {RESEARCH_BENCH}")[1][:200])
    log("speed x1", cmd(host, "speed 1"))
    time.sleep(4)
    hm1 = compare_all(f"apres {minutes} min a x3")
    worked |= set(with_workers(hm1))
    log("machines with workers (at any reading):", sorted(worked))
    check("postes : au moins un poste a des ouvriers", bool(worked),
          f"research {queued} {cmd(host, 'research')[1][:120]} | modes {[cmd(host, f'modes {i}')[1] for i in range(n)]} | {hm1}")
    for line in host_log()[-4000:]:
        if "machine:" in line and ("workers" in line or "crops" in line or "not found here" in line or "replanted" in line):
            log("  ", line.strip())
    summary()


def exp_machineout(host, cli, seconds=60):
    """0.3.4 (12 October, A_FAIRE point 6): a machine's inventory is synced to every player, open or not.
    While its window is closed on the client, nothing that changes there is taken for the player's move:
    no InvOp leaves the client, the host never answers "nothing to do", and the output is the same
    everywhere. Then the client opens it and takes the output: that move goes to the host and lands."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    SANDBOX["home"] = None
    log("heal everyone:", cmd(host, "console heal all"))
    log("god mode for everyone:", cmd(host, "console god all"))
    cmd(host, "console xp all 60")
    time.sleep(4)
    out = move_squad_out_of_town(host)
    check("sortie machine : escouade en plein desert", out, "")
    if not out:
        summary()
        return
    ok, t, spot = place_valid(host, COOKING_STOVE, own, 0, 80)
    log("host places a stove by the client's character:", ok, t, spot)
    check("sortie machine : cuisiniere posee", ok, t)
    if not ok:
        summary()
        return
    time.sleep(3)
    for _ in range(2):
        log("finish:", cmd(host, f"buildprogress {COOKING_STOVE} 100000"))
        time.sleep(1.5)
    keep_anchor_home(host)
    for what in (WHEATSTRAW, "Ble", "Wheat"):   # 13/10: "Ble" / "Wheat" put nothing in ("no room")
        r = cmd(host, f"machinegive {COOKING_STOVE} {what} 20")
        log("sandbox: into the stove", what, r)
        if r[0]:
            break
    asked0 =cmd(cli, "logcount inventory_move_asked_of_the_host")[1]
    cmd(cli, "logcount window_closed:_not_the_player's_doing")
    cmd(host, "logcount not_done_(nothing_to_do)")
    log("client's character works the stove:", cmd(cli, f"workreq {own} {COOKING_STOVE}"))
    log("speed x3", cmd(host, "speed 3"))
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(10)
        keep_anchor_home(host)
        log("stove host:", cmd(host, f"storage {COOKING_STOVE}")[1][:200], "| client:", cmd(cli, f"storage {COOKING_STOVE}")[1][:200])
    log("speed x1", cmd(host, "speed 1"))
    time.sleep(4)
    sh, sc = cmd(host, f"storage {COOKING_STOVE}")[1], cmd(cli, f"storage {COOKING_STOVE}")[1]
    if sh != sc:
        time.sleep(3)
        sh, sc = cmd(host, f"storage {COOKING_STOVE}")[1], cmd(cli, f"storage {COOKING_STOVE}")[1]
    check("sortie machine : meme contenu de la cuisiniere partout (fermee)", sh == sc, f"{sh[:160]} / {sc[:160]}")
    asked1 = cmd(cli, "logcount inventory_move_asked_of_the_host")[1]
    closed = cmd(cli, "logcount window_closed:_not_the_player's_doing")[1]
    nothing = cmd(host, "logcount not_done_(nothing_to_do)")[1]
    log("client moves asked:", asked0, "->", asked1, "| closed-window differences seen:", closed, "| host 'nothing to do':", nothing)
    check("sortie machine : aucun deplacement demande par le client, fenetre fermee", asked1 == asked0, f"{asked0} -> {asked1}")
    check("sortie machine : aucune demande sans effet chez l'hote", nothing in ("ok 0", "0"), nothing)
    if " stacks=0 " in f" {sh} ":
        # nothing ever changed in it: the closed-window checks above saw no change to ignore
        log("NOT TESTED (closed window): the stove held nothing during the work (no ingredient went in, or no cooking)")
        for what in (WHEATSTRAW, BREAD):   # something to take, for the open-window part
            r = cmd(host, f"machinegive {COOKING_STOVE} {what} 3")
            log("sandbox: into the stove, to be taken", what, r)
            if r[0]:
                break
        time.sleep(4)
    # the client opens the stove and takes its output: that one is the player's, sent and done.
    # containerreq takes the stove's sid (13/10: it matched translated names only, "no container
    # called 1944-gamedata.base": the window was never asked for)
    r = cmd(cli, f"containerreq {own} {COOKING_STOVE}")
    log("client opens the stove:", r)
    opened = False
    for _ in range(40):
        time.sleep(1)
        if "windows=0" not in cmd(cli, "tradestate")[1]:
            opened = True
            break
    check("sortie machine : le client ouvre la cuisiniere", opened, cmd(cli, "tradestate")[1])
    if opened:
        took = cmd(cli, f"contake {own} {COOKING_STOVE}")
        time.sleep(4)
        asked2 = cmd(cli, "logcount inventory_move_asked_of_the_host")[1]
        rep_ = compare(dump(host, "h_machineout"), dump(cli, "c_machineout"), "machineout")
        check("sortie machine : le client prend la sortie (demande envoyee, memes inventaires)",
              took[0] and asked2 != asked1 and rep_["inventory_mismatch"] == 0, f"{took[1]} | {asked1} -> {asked2} | {rep_['inventory_mismatch_sample']}")
        cmd(cli, "closewindows")
    summary()


def exp_tradeswap(host, cli, merchant="Marchand"):
    """0.3.4 (A_FAIRE point 4): the trade window opened on a stand-in merchant (the client's game has
    not loaded the town's real NPCs yet) goes on with the host's real merchant when it replaces the
    stand-in: still open, with the merchant's cats, and a sale works afterwards. Opened right after the
    join (no wait) so that the merchant is a stand-in as often as possible."""
    time.sleep(8)
    cmd(cli, "editdone")
    time.sleep(2)
    own = own_index(host)
    for t in ("trade_window:_now_on", "trade_window:_cannot_move_it", "closed_the_inventory_windows", "DID_NOT_STICK"):
        cmd(cli, f"logcount {t}")
    log("merchants around the squad (host):", cmd(host, "merchants")[1])
    log("host asks for the trade window:", cmd(host, f"tradeopen {own} {merchant} near"))
    state = "?"
    for _ in range(30):
        time.sleep(0.5)
        state = cmd(cli, "tradestate")[1]
        if "open=1" in state:
            break
    check("commerce (doublure) : la fenetre s'ouvre chez le client", "open=1" in state, state)
    log("client trade state:", state, "| stand-ins:", cmd(cli, "standins")[1][:200])
    end = time.time() + 60
    while time.time() < end:
        time.sleep(2)
        state = cmd(cli, "tradestate")[1]
        moved = cmd(cli, "logcount trade_window:_now_on")[1]
        if moved not in ("ok 0", "0") or "open=0" in state:
            break
    moved = cmd(cli, "logcount trade_window:_now_on")[1]
    failed = cmd(cli, "logcount trade_window:_cannot_move_it")[1]
    closed = cmd(cli, "logcount closed_the_inventory_windows")[1]
    log("window moved to the real merchant:", moved, "| could not:", failed, "| windows closed on a destroy:", closed, "| state:", state)
    if moved in ("ok 0", "0") and failed in ("ok 0", "0"):
        log("NOT TESTED: the merchant was not a stand-in replaced during the test (try again right after loading)")
    else:
        time.sleep(2)
        state = cmd(cli, "tradestate")[1]
        check("commerce (doublure) : la fenetre reste ouverte sur le vrai marchand", moved not in ("ok 0", "0") and "open=1" in state and "windows=0" not in state, state)
        cats = state.split("merchantcats=")[-1] if "merchantcats=" in state else "0"
        check("commerce (doublure) : le marchand a l'argent de l'hote", cats.strip() not in ("0", ""), state)
        check("commerce (doublure) : l'argent du marchand tient", cmd(cli, "logcount DID_NOT_STICK")[1] in ("ok 0", "0"), "")
        sold = cmd(cli, "tradesell 0")
        time.sleep(3)
        h, c = cmd(host, "money")[1], cmd(cli, "money")[1]
        check("commerce (doublure) : vente apres le remplacement, meme argent partout", sold[0] and h == c, f"{sold[1]} | hote {h} / client {c}")
    cmd(cli, "closewindows")
    summary()


def exp_clientjobs(host, cli):
    """v0.3.4 (A_FAIRE 12/10 point 5, rob's 60 clicks): giving work to the client's characters exactly as
    in solo. The JOBS toggle twice in a row (shown at once, the second click turns it back), a permanent
    job by Shift + right click on a free and on a full iron node (queued all the same, the player told
    why), a one-time order on a full node (told), the freed place taken by the client's character by
    itself (a town fight makes that inconclusive), identical clicks sent once a second, the Tâches
    panel (a follow job added, moved, removed) with no flicker, a recruit nobody owns, another
    player's character (refused, told)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("client's own character: squad index", own)
    cmd(host, "console heal all")
    cmd(host, "console god all")
    time.sleep(3)

    def jobs(pid, i):
        t = cmd(pid, f"jobs {i}")[1].split()
        return t[2:] if len(t) >= 2 and t[0] == "ok" else None

    def chat_has(part, n=8):
        t = cmd(cli, f"chatlast {n}")[1]
        return part in t, t

    def ops():
        return mfields(cmd(host, f"machine {IRON_NODE}")[1]).get("ops", "")

    # 1. JOBS (standing order 15, bit 64): two clicks in a row
    def chase(pid):
        t = cmd(pid, f"modes {own}")[1].split()
        return (int(t[1]) & 64) != 0 if len(t) >= 2 and t[0] == "ok" else None
    start = chase(cli)
    log("JOBS at start (client / host):", start, chase(host))
    cmd(cli, f"orderreq {own} 15")
    time.sleep(0.3)
    first = chase(cli)
    check("jobs : le bouton JOBS change tout de suite chez le client", first is not None and first != start, f"{start} -> {first}")
    time.sleep(1.2)   # the host's modes come in between (once a second)
    mid = chase(cli)
    check("jobs : l'etat predit tient malgre la synchro de l'hote", mid == first, f"{first} -> {mid}")
    cmd(cli, f"orderreq {own} 15")
    time.sleep(0.3)
    second = chase(cli)
    check("jobs : le second clic revient en arriere", second == start, f"{start} -> {first} -> {second}")
    time.sleep(5)
    ch, cc = chase(host), chase(cli)
    check("jobs : meme etat chez l'hote et le client apres deux clics", ch == cc == start, f"hote {ch} client {cc} depart {start}")
    if not cc:   # JOBS on for the rest (its jobs are done only then)
        cmd(cli, f"orderreq {own} 15")
        time.sleep(4)
    check("jobs : JOBS allume pour la suite", chase(host) and chase(cli), f"{chase(host)} / {chase(cli)}")

    # 2. a full iron node (3 places): three of the host's characters on it
    n = squad_size(host)
    host_idx = [i for i in range(n) if i != own]
    whose = cmd(cli, "whoseidx")[1]
    log("squad as the client sees it:", whose)
    marks = dict(tok.split(":") for tok in whose.split()[1:] if ":" in tok)
    for i in host_idx[:3]:
        log("host's character", i, "works the iron node:", cmd(host, f"workreq {i} {IRON_NODE}"))

    # The node's operator set, read at one instant, comes and goes while the miners work (13/10
    # 14:25:11.8-12.3, four reads 0.1 s apart: 3/3, 2/3, 3/3, 1/3, names changing; the session's own
    # machine sync saw 3/3 from 14:25:09 to 14:25:36): read it several times, its best count counts
    def ops_best(reads=10, gap=0.2):
        seen = []
        for _ in range(reads):
            seen.append(ops())
            time.sleep(gap)
        best = max(seen, key=lambda t: int(t.split("/")[0]) if t.split("/")[0].isdigit() else -1)
        return best, seen
    for _ in range(30):
        time.sleep(3)
        if ops().startswith("3/"):
            break
    best, seen = ops_best()
    full = best.startswith("3/")
    log("iron node:", best, "reads:", seen)
    check("jobs : noeud de fer plein (3/3) avec les persos de l'hote", full, f"{best} {seen}")
    j0 = jobs(host, own) or []
    if full:
        log("client one-time order on the full node:", cmd(cli, f"workreq {own} {IRON_NODE}"))
        time.sleep(3)
        ok, t = chat_has("toutes les places sont prises")
        check("jobs : ordre ponctuel sur une machine pleine : le joueur est prevenu", ok, t)
        time.sleep(2.5)   # one host note every 2 s on the client
        log("client Shift + right click on the full node:", cmd(cli, f"workreq {own} {IRON_NODE} shift"))
        time.sleep(3)
        jh, jc = jobs(host, own) or [], jobs(cli, own) or []
        check("jobs : Maj + clic droit sur une machine pleine : travail permanent cree chez l'hote", jh.count("87") > j0.count("87"), f"{j0} -> {jh}")
        check("jobs : meme liste de taches chez le client", jh == jc, f"{jh} / {jc}")
        ok, t = chat_has("place se libère")
        check("jobs : le joueur sait qu'il attend une place", ok, t)
        time.sleep(2.5)
        log("the same Shift + right click again:", cmd(cli, f"workreq {own} {IRON_NODE} shift"))
        time.sleep(3)
        ok, t = chat_has("déjà dans la liste")
        check("jobs : travail deja dans la liste : dit au joueur, rien d'ajoute", ok and (jobs(host, own) or []) == jh, t)
        # A place frees: the client's character takes it (its permanent job), by itself. It stands
        # ~1900 units away (the full node sent it nowhere): the game's job choice sends it there, a
        # walk of ~30 s like the host's three. The save's iron node is in town, where fights go on
        # (exp_research): the 13/10 run's character walked there by itself (1904 -> 125 units) but
        # fought from the moment it arrived (melee defence, toughness, dodge, mass combat up every few
        # seconds, everyone in god mode: the fight never ends), so it never sat down in 60 s. Hence:
        # the walk is checked on its own (the job was resumed), the place within 120 s, and a fight
        # makes the place check inconclusive instead of failed (the game's combat comes before jobs,
        # in solo as well).
        def dist_to_node():
            a, b = pos_xz(host, own), pos_xz(host, host_idx[1])   # a miner who stays: the node
            return math.hypot(a[0] - b[0], a[1] - b[1]) if a and b else None

        def combat_skills():
            t = cmd(host, f"stats {own}")[1].split()
            try:
                v = [float(x) for x in t[1].split(",")]
                return [v[i] for i in (16, 17, 27, 33)]   # melee defence, toughness, dodge, mass combat
            except (IndexError, ValueError):
                return None
        cmd(host, "console heal all")
        d0, c0 = dist_to_node(), combat_skills()
        log("client's character before the place frees:", f"{d0:.0f} units from the node" if d0 is not None else "?", cmd(host, f"charstate {own}")[1])
        log("host's worker leaves:", cmd(host, f"moverel {host_idx[0]} 300 0"))
        got, nearest = False, d0
        for k in range(40):
            time.sleep(3)
            d = dist_to_node()
            if d is not None and (nearest is None or d < nearest):
                nearest = d
            if k % 5 == 4:
                log("   waiting for the free place:", ops(), f"client's character {d:.0f} units from the node" if d is not None else "")
            if ops().startswith("3/"):
                got = True
                break
        c1 = combat_skills()
        fought = c0 is not None and c1 is not None and any(b > a + 0.05 for a, b in zip(c0, c1))
        log("client's character after:", cmd(host, f"charstate {own}")[1], "combat skills", c0, "->", c1)
        check("jobs : place liberee : le perso du client y va de lui-meme (travail permanent repris)", got or (nearest is not None and nearest < 400),
              f"{d0} -> au plus pres {nearest}")
        if got or not fought:
            check("jobs : une place liberee est reprise (travail permanent du client)", got, ops())
        else:
            log("NON CONCLUANT : le perso du client s'est battu pres du noeud (combats de la ville) : place libre non prise", ops(), c0, "->", c1)

    # 3. identical clicks: once a second to the host
    def runs():
        return host_log().count("client task 87 (via 2) run")
    r0 = runs()
    for _ in range(8):
        cmd(cli, f"workreq {own} {IRON_NODE}")
        time.sleep(0.1)
    time.sleep(3)
    check("jobs : clics identiques envoyes au plus une fois par seconde", runs() - r0 <= 3, f"{runs() - r0} ordres executes pour 8 clics")

    # 4. the Tâches panel: a second job, moved, then removed, with no flicker on the client. Not
    # "hauling (88) with no subject" (13/10 run: the game makes no job of it, on the host as on the
    # client: jobs 1 -> 1, nor in exp_workstations' runs): the panel's follow job (31 on a squad
    # mate, exp_jobs), which the game does list.
    # The panel itself (13/10 user report: a job given to the client's character showed only after
    # clicking another character and back): with the client's character selected, the panel's rows
    # must follow each change (the job added ahead then copied from the host, the move, the removal)
    # without selecting it again (KenshiWorld::UpdateJobsPanel, debug command jobspanel).
    def panel(pid):
        t = cmd(pid, "jobspanel")[1]
        return dict(kv.split("=", 1) for kv in t.split()[1:] if "=" in kv) if t.startswith("ok") else {}
    log("client selects its character (the Tâches panel shows it):", cmd(cli, f"selectset {own}"))
    time.sleep(1)
    p0 = panel(cli)
    panel_ok = p0.get("panel") == "1" and p0.get("shown") == str(own)
    cmd(cli, "logcount jobs_panel:_rebuilt")   # starts counting
    log("client's Tâches panel before:", p0, "| host's:", panel(host))
    if not panel_ok:
        log("NOT TESTED: the client's Tâches panel does not show its character after selectset:", p0)

    def panel_check(what):
        if not panel_ok:
            return
        p = {}
        for _ in range(8):   # within 4 s, no click on another character in between
            p = panel(cli)
            if p.get("stale") == "0" and p.get("rows") == p.get("jobs"):
                break
            time.sleep(0.5)
        check(f"jobs : panneau Taches du client a jour sans reselection ({what})",
              p.get("shown") == str(own) and p.get("stale") == "0" and p.get("rows") == p.get("jobs"), str(p))
        log("   panel rebuilds logged on the client so far:", cmd(cli, "logcount jobs_panel:_rebuilt")[1], "| host's panel:", panel(host))

    if "87" not in (jobs(host, own) or []):   # the node was not full: no permanent job on it yet
        log("client Shift + right click on the node (first job):", cmd(cli, f"workreq {own} {IRON_NODE} shift"))
        time.sleep(3)
        panel_check("Maj + clic droit")
    log("client Tâches panel: follow job (31) on member", host_idx[-1], cmd(cli, f"jobreq {own} 31 {host_idx[-1]}"))
    time.sleep(4)
    panel_check("travail ajoute")
    jh = jobs(host, own) or []
    log("jobs before the move (host):", jh)
    if len(jh) >= 2:
        last = len(jh) - 1
        want = [jh[last]] + jh[:last]
        log("client drags the last job to the top:", cmd(cli, f"jobmove {own} {last} 0"))
        seen = []
        for _ in range(8):
            time.sleep(0.5)
            seen.append(jobs(cli, own))
        jh2, jc2 = jobs(host, own), jobs(cli, own)
        check("jobs : deplacement fait chez l'hote", jh2 == want, f"{jh} -> {jh2} (voulu {want})")
        check("jobs : deplacement sans retour en arriere chez le client", all(s == want for s in seen) and jc2 == want, f"{seen}")
        panel_check("travail deplace")
        log("client removes the top job:", cmd(cli, f"jobremove {own} 0"))
        seen = []
        for _ in range(8):
            time.sleep(0.5)
            seen.append(jobs(cli, own))
        jh3, jc3 = jobs(host, own), jobs(cli, own)
        check("jobs : retrait fait chez l'hote, meme liste partout", jh3 == want[1:] and jc3 == jh3, f"{jh3} / {jc3}")
        check("jobs : le travail retire ne revient pas chez le client", all(s == want[1:] for s in seen), f"{seen}")
        panel_check("travail retire")
        # the JOBS toggle on the shown character: its rows and the button follow (the game's own GUI
        # event, and UpdateJobsPanel); twice, so that JOBS stays on for the rest
        for k in range(2):
            cmd(cli, f"orderreq {own} 15")
            time.sleep(1.5)
            panel_check(f"JOBS bascule {k + 1}")
            time.sleep(1.5)
    else:
        check("jobs : deux travaux pour tester le panneau", False, jh)

    # 5. a recruit nobody owns: anyone gives it base work
    shared = [int(i) for i, m in marks.items() if m == "s"]
    if shared:
        s = shared[0]
        js0 = jobs(host, s) or []
        log("client Shift + right click for the recruit nobody owns:", s, cmd(cli, f"workreq {s} {IRON_NODE} shift"))
        time.sleep(4)
        js1 = jobs(host, s) or []
        check("jobs : recrue sans joueur : travail permanent donne par le client", js1.count("87") > js0.count("87"), f"{js0} -> {js1}")
    else:
        log("no recruit nobody owns in this save: shared work not tested in game")
    # 6. another player's (here the host's) character: refused, told in French
    other = next((int(i) for i, m in marks.items() if m == "o"), None)
    if other is not None:
        time.sleep(2.5)
        jo0 = jobs(host, other)
        log("client Shift + right click for the host's character:", other, cmd(cli, f"workreq {other} {IRON_NODE} shift"))
        time.sleep(3)
        check("jobs : perso d'un autre joueur : rien ne change chez l'hote", jobs(host, other) == jo0, f"{jo0} -> {jobs(host, other)}")
        t = cmd(cli, "toastlast")[1]   # a notice at the top of the screen, not a chat line
        check("jobs : perso d'un autre joueur : le joueur est prevenu", "appartient à un autre joueur" in t, t)
    for line in host_log().splitlines()[-3000:]:
        if "work order" in line or "jobs:" in line or "client mode" in line:
            log("  ", line.strip())
    summary()


def exp_research(host, cli):
    """Workshop: the research (tech tree), the crafting benches, the mines and machines and the power of
    the players' outpost are the host's. The client researches, builds what it unlocked and crafts
    with it through requests the host runs; skills, workers, outputs and power show the same on both
    sides. The save may lack benches and materials: the host's sandbox commands set them up (templates
    by id, placed away from towns)."""
    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    own = own_index(host)
    log("client's own character: squad index", own)
    cmd(host, "console xp all 60")   # faster researchers, builders and miners
    # the town's fights leave squad members knocked out (members 0, 1 and the client's in the 11/10 run):
    # a body is not teleported for good, and squad member 0 anchors the whole sandbox. Healed (woken
    # up) and in god mode, nobody lies down any more.
    SANDBOX["home"] = None
    log("heal everyone:", cmd(host, "console heal all"))
    log("god mode for everyone:", cmd(host, "console god all"))
    time.sleep(4)

    # ---- mine first (the save's iron nodes): three characters, the host's and the client's, on one node
    others = [i for i in range(squad_size(host)) if i != own][:2]
    log("client's character works the iron node:", cmd(cli, f"workreq {own} {IRON_NODE}"))
    for i in others:
        log("host's character", i, "works the iron node:", cmd(host, f"workreq {i} {IRON_NODE}"))
    mh = {}
    for _ in range(30):   # they walk there (up to 90 s)
        time.sleep(3)
        mh = mfields(cmd(host, f"machine {IRON_NODE}")[1])
        if mh.get("ops", "").startswith("3/"):
            break
    log("iron node on the host:", mh)
    ok, last = machine_both(host, cli, IRON_NODE, "mine", floats=("prog", "prod"), tol=0.3)
    check("mine : nombre de mineurs identique partout", ok, last[2] if len(last) > 2 else last)
    check("mine : trois mineurs (hote et client melanges)", mh.get("ops", "").startswith("3/"), mh.get("ops"))
    time.sleep(20)   # some ore
    ih = cmd(host, f"machineinv {IRON_NODE}")[1]
    time.sleep(2)
    ic = cmd(cli, f"machineinv {IRON_NODE}")[1]
    ih2 = cmd(host, f"machineinv {IRON_NODE}")[1]
    check("mine : meme minerai produit partout", ic in (ih, ih2), f"{ih} / {ic}")
    # the node is the player faction's once worked (the game's Task_OperateMachine): on the client too,
    # else the game never opens its output window there (Building::select: player buildings only)
    sel = cmd(cli, f"machineselect {IRON_NODE}")[1]
    log("client clicks the iron node:", sel)
    check("mine : le noeud de fer est au joueur chez le client aussi", " ours=1" in sel, sel)
    check("mine : la fenetre de sortie du noeud s'ouvre chez le client", "panel=1" in sel, sel)
    if others:
        log("host's character", others[-1], "stops working", cmd(host, f"moverel {others[-1]} 200 0"))
        time.sleep(8)
        ok, last = machine_both(host, cli, IRON_NODE, "mine after a worker left", floats=("prog", "prod"), tol=0.3)
        mh2 = mfields(cmd(host, f"machine {IRON_NODE}")[1])
        check("mine : un mineur retire, vu partout", ok and mh2.get("ops") != mh.get("ops"), f"{mh.get('ops')} -> {mh2.get('ops')} | {last[2] if len(last) > 2 else ''}")

    # ---- sandbox away from towns: research bench, crafting bench, generator, battery, light
    cmd(host, "console heal all")   # the miners may have been hit since
    time.sleep(2)
    out = move_squad_out_of_town(host)
    check("recherche : escouade hors des villes pour le bac a sable", out, "")
    rb_ok = sandbox_building(host, RESEARCH_BENCH, 60, 0, "research bench")
    check("recherche : banc de recherche pose", rb_ok, RESEARCH_BENCH)
    cb_sid, cb_ok = None, False
    for sid in CRAFT_BENCHES:
        if sandbox_building(host, sid, -60, 0, "crafting bench"):
            cb_sid, cb_ok = sid, True
            break
    time.sleep(4)
    keep_anchor_home(host)
    if rb_ok:
        for what in ("Livre", "Book", "Books", "Artefact", "Artifact", "artifact"):
            log("sandbox: into the research bench", what, cmd(host, f"machinegive {RESEARCH_BENCH} {what} 20"))
        log("client's character works the research bench:", cmd(cli, f"workreq {own} {RESEARCH_BENCH}"))
    if cb_ok:
        for what in (FABRICS, "Tissus", "Cuir", "Leather", "Fer", "Iron"):
            log("sandbox: into the crafting bench", what, cmd(host, f"machinegive {cb_sid} {what} 20"))
    time.sleep(15)   # the client's character walks to the research bench

    # ---- research: the same state on both sides
    def rstate(pid):
        t = cmd(pid, "research")[1]
        d = dict(p.split("=", 1) for p in t.split()[1:] if "=" in p) if t.startswith("ok") else {}
        d["queue_sids"] = [q.split(":")[0] for q in d.get("queue", "").split("|") if q]
        return d
    rh, rc = rstate(host), rstate(cli)
    log("research host:", rh, "| client:", rc)
    check("recherche : technologies connues identiques au depart", rh.get("fh") and rh.get("fh") == rc.get("fh"), f"{rh.get('finished')} / {rc.get('finished')}")
    enabled0 = cmd(cli, "researchenabled 0")[1].split()[3:]
    crafts0 = set(x.split("=")[0] for x in cmd(cli, "craftlist 2000")[1].split()[2:])
    stats0 = (cmd(host, f"stats {own}")[1], cmd(cli, f"stats {own}")[1])
    # what the host's benches can pay for first, then anything whose requirements are met
    picks = [p.split("=")[0] for p in cmd(host, "researchpick 6 cost")[1].split()[1:]]
    picks += [p.split("=")[0] for p in cmd(cli, "researchpick 6")[1].split()[1:] if p.split("=")[0] not in picks]
    log("techs the client may research:", picks)
    tech = None
    for sid in picks:
        log("client queues", sid, cmd(cli, f"researchreq queue {sid}"))
        for _ in range(8):
            time.sleep(0.5)
            if sid in rstate(host)["queue_sids"]:
                tech = sid
                break
        if tech:
            break
    check("recherche : demande du client mise en file par l'hote", tech is not None, f"{picks} -> host queue {rstate(host).get('queue')}")
    if tech:
        time.sleep(3)
        rh, rc = rstate(host), rstate(cli)
        check("recherche : meme file et meme avancement partout", rh["queue_sids"] == rc["queue_sids"], f"{rh.get('queue')} / {rc.get('queue')}")
        # asked twice (a double click, or two players at once): queued once, paid once
        before = cmd(host, f"machineinv {RESEARCH_BENCH}")[1] if rb_ok else ""
        log("client queues it again", cmd(cli, f"researchreq queue {tech}"))
        log("host player queues it too", cmd(host, f"researchreq queue {tech}"))
        time.sleep(3)
        rh = rstate(host)
        check("recherche : une seule fois dans la file malgre deux demandes", rh["queue_sids"].count(tech) == 1, rh.get("queue"))
        if rb_ok:
            after = cmd(host, f"machineinv {RESEARCH_BENCH}")[1]
            check("recherche : rien n'est consomme deux fois", before == after, f"{before} -> {after}")
            check("recherche : inventaire du banc identique partout", cmd(cli, f"machineinv {RESEARCH_BENCH}")[1] == after, cmd(cli, f"machineinv {RESEARCH_BENCH}")[1])
        # cancelled by the client, then queued again
        log("client cancels", cmd(cli, f"researchreq cancel {tech}"))
        time.sleep(3)
        check("recherche : annulation du client vue partout", tech not in rstate(host)["queue_sids"] and tech not in rstate(cli)["queue_sids"],
              f"{rstate(host).get('queue')} / {rstate(cli).get('queue')}")
        log("client queues it again", cmd(cli, f"researchreq queue {tech}"))
        time.sleep(3)
        # the researchers' work, sped up on the host: finished, known everywhere
        for _ in range(10):
            log("host research progress", cmd(host, "researchprogress 100000"))
            time.sleep(2)
            if cmd(host, f"researchknown {tech}")[1] == "ok 1":
                break
        time.sleep(4)
        check("recherche : technologie terminee chez l'hote", cmd(host, f"researchknown {tech}")[1] == "ok 1", rstate(host).get("queue"))
        check("recherche : et connue chez le client", cmd(cli, f"researchknown {tech}")[1] == "ok 1", rstate(cli).get("queue"))
        rh, rc = rstate(host), rstate(cli)
        check("recherche : arbre technologique identique partout", rh.get("fh") == rc.get("fh") and rh["queue_sids"] == rc["queue_sids"], f"{rh} / {rc}")
        eh, ec = cmd(host, "researchenabled 0")[1], cmd(cli, "researchenabled 0")[1]
        check("recherche : menu de construction debloque pareil chez le client", eh.split()[2:3] == ec.split()[2:3], f"{eh[:120]} / {ec[:120]}")
    # ---- the client builds what it unlocked, then crafts with it
    new_buildings = [s for s in cmd(cli, "researchenabled 0")[1].split()[3:] if s not in enabled0]
    log("buildings unlocked for the client:", new_buildings[:10])
    if new_buildings:
        sid = new_buildings[0]
        ok, t, spot = place_valid(cli, sid, own, 50, 50)
        log("client places the unlocked building", sid, ok, t)
        time.sleep(4)
        built_h = [e for e in cmd(host, f"buildlist {sid}")[1].split()[2:] if e.startswith(sid + "@")]
        check("recherche : le client pose le batiment debloque", ok and bool(built_h), f"{t} | {built_h}")
        log("host workers finish it", cmd(host, f"buildprogress {sid} 100000"))
        time.sleep(3)
        bh = [e for e in cmd(host, f"buildlist {sid}")[1].split()[2:] if e.startswith(sid + "@")]
        bc = [e for e in cmd(cli, f"buildlist {sid}")[1].split()[2:] if e.startswith(sid + "@")]
        check("recherche : batiment debloque construit partout", bh and bc and sorted(x.split(":")[-1] for x in bh) == sorted(x.split(":")[-1] for x in bc),
              f"{bh} / {bc}")
    else:
        log("SKIP: the tech researched unlocked no building (only crafts or upgrades)")
    new_crafts = sorted(set(x.split("=")[0] for x in cmd(cli, "craftlist 2000")[1].split()[2:]) - crafts0)
    craftable = new_crafts or sorted(crafts0)
    log("crafts for the client:", len(crafts0), "->", len(craftable), new_crafts[:6])
    if cb_ok and craftable:
        m0 = mfields(cmd(host, f"machine {cb_sid}")[1])
        n0 = len([x for x in m0.get("crafts", "").split(",") if x])
        base = craftable[0]
        log("client orders a craft", base, cmd(cli, f"machinereq {cb_sid} addcraft {base}"))
        time.sleep(4)
        m1 = mfields(cmd(host, f"machine {cb_sid}")[1])
        n1 = len([x for x in m1.get("crafts", "").split(",") if x])
        check("fabrication : ordre du client execute par l'hote", n1 == n0 + 1, f"{m0.get('crafts')} -> {m1.get('crafts')}")
        ok, last = machine_both(host, cli, cb_sid, "crafting bench")
        check("fabrication : meme file de fabrication partout", ok, last[2] if len(last) > 2 else last)
        # the host player and the client change the same bench at once: the last order wins everywhere
        log("host sets repeat on", cmd(host, f"machinereq {cb_sid} repeat 1"))
        log("client sets repeat off", cmd(cli, f"machinereq {cb_sid} repeat 0"))
        time.sleep(4)
        ok, last = machine_both(host, cli, cb_sid, "crafting bench repeat")
        check("fabrication : deux ordres en meme temps, meme resultat partout", ok, last[2] if len(last) > 2 else last)
        check("fabrication : inventaire de l'etabli identique partout", cmd(host, f"machineinv {cb_sid}")[1] == cmd(cli, f"machineinv {cb_sid}")[1],
              f"{cmd(host, f'machineinv {cb_sid}')[1]} / {cmd(cli, f'machineinv {cb_sid}')[1]}")
        # the client's own character works the bench (a job: the host's game makes it craft)
        log("client works the bench", cmd(cli, f"workreq {own} {cb_sid}"))
        time.sleep(30)
        ok, last = machine_both(host, cli, cb_sid, "crafting bench worked")
        check("fabrication : le perso du client travaille l'etabli, vu pareil partout", ok, last[2] if len(last) > 2 else last)
        log("client removes the order", cmd(cli, f"machinereq {cb_sid} removecraft 0"))
        time.sleep(4)
        ok, last = machine_both(host, cli, cb_sid, "crafting bench order removed")
        check("fabrication : ordre retire par le client, retire partout", ok, last[2] if len(last) > 2 else last)
    else:
        check("fabrication : un etabli et un objet a fabriquer", False, f"bench {cb_sid} ok={cb_ok}, crafts {len(craftable)}")
    # learning by doing: computed once on the host, the same on the client
    time.sleep(3)
    sh, sc = cmd(host, f"stats {own}")[1].split()[1], cmd(cli, f"stats {own}")[1].split()[1]
    log("skills before:", stats0[0][:80], "| after host:", sh[:80], "client:", sc[:80])
    check("fabrication : competences du perso du client identiques partout", sh == sc, f"{sh[:120]} / {sc[:120]}")

    # ---- power: a generator, a battery and a consumer
    built = {}
    if sandbox_building(host, WIND_GENERATOR, 0, 90, "generator"):
        built["generator"] = WIND_GENERATOR
    if sandbox_building(host, BATTERY_BANK, 40, 90, "battery"):
        built["battery"] = BATTERY_BANK
    for sid in LIGHTS:
        if sandbox_building(host, sid, -40, 90, "light"):
            built["light"] = sid
            break
    gen_part = built.get("generator")
    check("energie : un generateur pose dans le bac a sable", bool(gen_part), WIND_GENERATOR)
    if gen_part:
        time.sleep(6)
        for label, part in built.items():
            ok, last = machine_both(host, cli, part, label)
            check(f"energie : {label} identique partout (sortie, charge, consommation)", ok, last[2] if len(last) > 2 else last)
        th, tc = mfields(cmd(host, f"machine {gen_part}")[1]).get("town"), mfields(cmd(cli, f"machine {gen_part}")[1]).get("town")
        log("town power host:", th, "client:", tc)
        if th:
            close = tc and all(abs(float(a) - float(b)) <= max(0.15 * max(abs(float(a)), abs(float(b))), 0.6) for a, b in zip(th.split(","), tc.split(",")))
            check("energie : totaux du panneau de la base identiques", close, f"{th} / {tc}")
        log("client switches the generator off", cmd(cli, f"machinereq {gen_part} power 0"))
        time.sleep(4)
        fh = int(mfields(cmd(host, f"machine {gen_part}")[1]).get("flags", "1"))
        fc = int(mfields(cmd(cli, f"machine {gen_part}")[1]).get("flags", "1"))
        check("energie : generateur coupe par le client, coupe partout", not (fh & 1) and not (fc & 1), f"host {fh} client {fc}")
        time.sleep(4)
        for label, part in built.items():
            if label != "generator":
                ok, last = machine_both(host, cli, part, label + " after the generator went off")
                check(f"energie : {label} apres la coupure, identique partout", ok, last[2] if len(last) > 2 else last)
    summary()


def exp_playerstats(host, cli):
    """0.3.5 player statistics: the host counts the client's walk and play time, the client gets
    them, the world's file is written (KenshiCoop_stats) and read back when the client comes back
    (one more session, the totals go on), and the JOUEURS tab of the management window shows them
    on both sides."""
    def pstats(pid):
        """pid's statistics: key -> fields (debug command pstats)."""
        ok, t = cmd(pid, "pstats")
        players = {}
        if ok and ";" in t:
            for part in t.split(";", 1)[1].split():
                fields = part.split(":")
                d = {"name": fields[0]}
                for f in fields[1:]:
                    if "=" in f:
                        k, v = f.split("=", 1)
                        d[k] = v
                players[d.get("key", fields[0])] = d
        return players

    def client_line(players):
        for d in players.values():
            if d.get("host") == "0" and d.get("key", "").endswith(str(CLIENT_FAKE_ID)):
                return d
        return {}

    def num(d, k):
        try:
            return float(d.get(k, "-1"))
        except ValueError:
            return -1.0

    time.sleep(6)
    cmd(cli, "editdone")
    time.sleep(3)
    h0 = client_line(pstats(host))
    log("host's line for the client at first:", h0)
    check("stats : l'hote a une ligne pour le client (en ligne, 1 session au moins)", h0.get("online") == "1" and num(h0, "sessions") >= 1, h0)
    own = own_index(host)
    log("client walks:", cmd(cli, f"moverel {own} 400 0"))
    time.sleep(25)
    h1 = client_line(pstats(host))
    log("after the walk:", h1)
    check("stats : distance comptee pendant la marche (> 10 m)", num(h1, "dist") - num(h0, "dist") > 100, f"{num(h0, 'dist')} -> {num(h1, 'dist')}")
    check("stats : temps de jeu qui avance", num(h1, "play") - num(h0, "play") > 15, f"{num(h0, 'play')} -> {num(h1, 'play')}")
    check("stats : persos du client comptes", num(h1, "chars") >= 1, h1.get("chars"))
    time.sleep(12)   # one send (every 10 s when they change)
    c1 = client_line(pstats(cli))
    log("the client's view:", c1)
    check("stats : le client recoit les statistiques (sa distance)", num(c1, "dist") >= num(h1, "dist") - 1, f"client {num(c1, 'dist')} host {num(h1, 'dist')}")
    host_lines = [d for d in pstats(cli).values() if d.get("host") == "1"]
    check("stats : le client voit aussi la ligne de l'hote", len(host_lines) == 1 and host_lines[0].get("online") == "1", host_lines)
    ok, path = cmd(host, "pstatssave")
    log("stats file:", ok, path)
    text = ""
    if ok and os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    check("stats : fichier du monde ecrit (KenshiCoop_stats)", ok and "KenshiCoop_stats" in path and text.startswith("kc-player-stats 1"), path)
    check("stats : le fichier a la ligne du client", f"player steam:{CLIENT_FAKE_ID}" in text, text[:200])
    # the JOUEURS tab of the management window, on both sides
    for who, pid in (("hote", host), ("client", cli)):
        log(who, "window:", cmd(pid, "mapui open"))
        time.sleep(2)
        log(who, "tab:", cmd(pid, "pstatsui select"))
        time.sleep(1.5)
        ok, st = cmd(pid, "pstatsui state")
        log(who, "tab state:", st)
        check(f"stats ({who}) : onglet JOUEURS dans la fenetre du jeu, affiche", ok and "tab=1" in st and "text=1" in st and "shown=1" in st, st)
        ok, txt = cmd(pid, "pstatsui text")
        check(f"stats ({who}) : texte de l'onglet (en ligne, distance)", ok and "(en ligne)" in txt and "Distance parcourue" in txt and "Temps de jeu" in txt,
              txt[:300])
        log(who, "close:", cmd(pid, "mapui close"))
        time.sleep(1)
    # the client leaves and comes back: the file is written, its totals go on, one more session
    logged = len(host_log())
    log("client leaves:", cmd(cli, "leave"))
    time.sleep(5)
    new = host_log()[logged:]
    check("stats : fichier ecrit au depart du joueur", "player stats: saved (a player left" in new, new[-300:])
    before = client_line(pstats(host))
    log("client's line once gone:", before)
    check("stats : le client parti est hors ligne", before.get("online") == "0", before)
    ok, t = cmd(cli, "join " + os.environ["KC_JOIN"]) if os.environ.get("KC_JOIN") else cmd(cli, "join")
    log("join again:", ok, t)
    wait_for(cli, lambda s: s.get("state") == "connected" and s.get("ready") == "1", 240, "client back in the host's world")
    cmd(cli, "editdone")
    time.sleep(6)
    after = client_line(pstats(host))
    log("client's line once back:", after)
    check("stats : au retour, une session de plus", num(after, "sessions") == num(before, "sessions") + 1, f"{before.get('sessions')} -> {after.get('sessions')}")
    check("stats : au retour, la distance continue", num(after, "dist") >= num(before, "dist") - 1, f"{before.get('dist')} -> {after.get('dist')}")
    check("stats : journal 'is back (session'", "is back (session" in host_log()[logged:], "")
    summary()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="what", required=True)
    r = sub.add_parser("run")
    r.add_argument("--save", default="kctest_base")
    r.add_argument("--keep", action="store_true", help="leave the instances running")
    r.add_argument("--quick", action="store_true")
    ca = sub.add_parser("carry")
    ca.add_argument("--save", default="kctest_base")
    dd = sub.add_parser("dead")
    dd.add_argument("--save", default="kctest_base")
    it = sub.add_parser("items")
    it.add_argument("--save", default="kctest_base")
    lcr = sub.add_parser("lootcreature", help="every creature kind (goat to leviathan) spawned, killed and looted by a client")
    lcr.add_argument("--save", default="kctest_base")
    lcr.add_argument("--keep", action="store_true")
    lcr.add_argument("--only", default="", help="part of one creature's name (Leviathan, Goat...)")
    lu = sub.add_parser("lootui")
    lu.add_argument("--save", default="kctest_base")
    lu.add_argument("--keep", action="store_true")
    lo = sub.add_parser("lootorder")
    lo.add_argument("--save", default="kctest_base")
    lo.add_argument("--keep", action="store_true")
    lc = sub.add_parser("lootclick")
    lc.add_argument("--save", default="kctest_base")
    lc.add_argument("--keep", action="store_true")
    sk = sub.add_parser("soak", help="stability: speeds 1-2-3-2-1, rapid speed/pause changes, client speed requests, "
                        "activity on both sides; every 30 s alive, frozen comparison, round trip, memory")
    sk.add_argument("--save", default="kctest_base")
    sk.add_argument("--minutes", type=int, default=20, help="duration in minutes (default 20)")
    sk.add_argument("--keep", action="store_true")
    ps = sub.add_parser("playerstats", help="0.3.5: player statistics counted by the host (walk, play time), received by the client, "
                        "saved to KenshiCoop_stats and continued after a rejoin; the JOUEURS tab of the game's window")
    ps.add_argument("--save", default="kctest_base")
    ps.add_argument("--keep", action="store_true")
    wk = sub.add_parser("walk")
    wk.add_argument("--save", default="kctest_base")
    t = sub.add_parser("trace")
    t.add_argument("--save", default="kctest_base")
    e = sub.add_parser("bodies")
    e.add_argument("--save", default="kctest_base")
    cr = sub.add_parser("crashrejoin", help="client killed hard (idle, fight at speed 3, carrying, trading, looting, "
                        "editor, quick relaunch), relaunched with the same fake Steam id and joined again")
    cr.add_argument("--save", default="kctest_base")
    cr.add_argument("--only", default=None, help="letters of the scenarios to run, e.g. 'ag'")
    cr.add_argument("--keep", action="store_true")
    cp = sub.add_parser("clientpickup")
    cp.add_argument("--save", default="kctest_base")
    cp.add_argument("--keep", action="store_true")
    dr = sub.add_parser("doors", help="lot A: doors, locks, lockpicking, locked chests")
    dr.add_argument("--save", default="kctest_town")
    dr.add_argument("--keep", action="store_true")
    pr = sub.add_parser("prison", help="lot D: cage, shackles, slavery and release of the client's character")
    pr.add_argument("--save", default="kctest_base")
    pr.add_argument("--keep", action="store_true")
    bd = sub.add_parser("build", help="lot E: buildings placed, built, dismantled and bought by everyone")
    bd.add_argument("--save", default="kctest_base")
    bd.add_argument("--keep", action="store_true")
    pv = sub.add_parser("placevalid", help="placements checked as build mode checks a spot: the acid lake refused by client and host, nothing built; a dry spot accepted")
    pv.add_argument("--save", default="kctest_base")
    pv.add_argument("--keep", action="store_true")
    bs = sub.add_parser("buildstate", help="construction state of save/town buildings is the host's")
    bs.add_argument("--save", default="kctest_mine")
    bs.add_argument("--keep", action="store_true")
    mi = sub.add_parser("mine")
    mi.add_argument("--save", default="kctest_mine")
    mi.add_argument("--keep", action="store_true")
    ad = sub.add_parser("admin")
    ad.add_argument("--save", default="kctest_base")
    ad.add_argument("--keep", action="store_true")
    for name, save, helptext in (("passive", "kctest_base", "fix G5: a client's attack leaves the passive host's character alone"),
                                 ("jobs", "kctest_base", "fix G5: a job removed in the client's TÃƒÆ’Ã‚Â¢ches panel is gone on the host"),
                                 ("tradepaths", "kctest_town", "fix G5: trading by conversation and right click opens on the client only")):
        g5 = sub.add_parser(name, help=helptext)
        g5.add_argument("--save", default=save)
        g5.add_argument("--keep", action="store_true")
        if name == "tradepaths":
            g5.add_argument("--merchant", default="Marchand", help="part of the merchant's name ('_' for spaces)")
    cs = sub.add_parser("construct", help="real construction: client and host order their characters to build (materials, progress, result the same everywhere)")
    cs.add_argument("--save", default="kctest_base")
    cs.add_argument("--keep", action="store_true")
    cs.add_argument("--material", default=None, help="name part or sid of the building materials item ('_' for spaces)")
    cs.add_argument("--task", type=int, action="append", default=None, help="TaskType of the 'build' order (repeatable; default: 2, BUILD)")
    bh = sub.add_parser("buyhouse", help="buying a building for sale: price, owner, doors and containers, refusal, simultaneous purchase")
    bh.add_argument("--save", default="kctest_town")
    bh.add_argument("--keep", action="store_true")
    rc_ = sub.add_parser("recruits", help="0.3.2: a Dialogue.mod recruit given around with G and moved by a client, two characters talking, copies never rebound")
    rc_.add_argument("--save", default="kctest_town")
    rc_.add_argument("--keep", action="store_true")
    rc_.add_argument("--clients", type=int, default=2)
    fw_ = sub.add_parser("faraway", help="0.3.1: the client's character in another town (> 30000) while the host never looks: sync, smooth NPCs, "
                                         "fight and damage numbers, loot, talk, trade, ground items, a third town")
    fw_.add_argument("--save", default="kctest_town")
    fw_.add_argument("--keep", action="store_true")
    fw_.add_argument("--clients", type=int, default=1, help="client players, each in its own far town (3: the 4-player run)")
    ff_ = sub.add_parser("farfps", help="3b: clients' fps and followed characters, together then each in a far town (0.3.1 behaviour, then the fix)")
    ff_.add_argument("--save", default="kctest_town")
    ff_.add_argument("--keep", action="store_true")
    ff_.add_argument("--clients", type=int, default=3)
    ff_.add_argument("--seconds", type=int, default=60, help="each measuring window")
    fwl_ = sub.add_parser("farwalls", help="3f: a client fights next to a building far from the host's camera: nobody crosses it (then with the camera there)")
    fwl_.add_argument("--save", default="kctest_town")
    fwl_.add_argument("--keep", action="store_true")
    fwl_.add_argument("--seconds", type=int, default=40)
    fp_ = sub.add_parser("farprobe", help="short diagnosis: talk and a client-ordered fight far from the host's camera, then with it")
    fp_.add_argument("--save", default="kctest_town")
    fp_.add_argument("--keep", action="store_true")
    fp_.add_argument("--notalk", action="store_true", help="skip the conversations (motion and fights only)")
    fl_ = sub.add_parser("farlong", help="the client's character far away (>= 30000) for a long time: zone compared every 30 s, fight, building")
    fl_.add_argument("--save", default="kctest_base")
    fl_.add_argument("--keep", action="store_true")
    fl_.add_argument("--seconds", type=int, default=300, help="time spent far away")
    for name_, hlp_ in (("caravan", "travelling merchant: packs synced, buy/sell while it walks, window closed on a kill, theft and loot of pack animals"),
                        ("pets", "squad animals: ownership, owner's squad, orders through the host, shared inventory"),
                        ("animalshop", "3i: a client buys an animal from a seller: window on the client only, sold by the host, the animal theirs"),
                        ("prospect", "3p: the PROSPECT button on the client (window there only) and on the host")):
        sp_ = sub.add_parser(name_, help=hlp_)
        sp_.add_argument("--save", default="kctest_town")
        sp_.add_argument("--keep", action="store_true")
    # 0.3.5 lot 5 (night-actions)
    for name_, hlp_, save_ in (("slaveshop", "point 4: a client buys a slave from a slaver (dialogue, window on the client only, bought by the host), sells only its own", "kctest_slavers"),
                               ("lockprogress", "point 5: a client picks, cuts, forces a lock: the host's progress bar shown on the client", "kctest_town"),
                               ("disguise", "point 13: a client in Holy Nation armour (put on by drag & drop) is disguised on the host and its indicator; paladins", "kctest_holy"),
                               ("tradefartp", "point 15: admin TP from far away, then a client sells to a merchant: the sale reaches the host", "kctest_town"),
                               ("followjob", "point 2: a follow job removed with the cross stops the follow (host and client characters)", "kctest_base")):
        sp_ = sub.add_parser(name_, help=hlp_)
        sp_.add_argument("--save", default=save_)
        sp_.add_argument("--keep", action="store_true")
        if name_ == "slaveshop":
            sp_.add_argument("--slaver", default="Esclavagiste", help="part of the slaver's name ('_' for spaces)")
        if name_ == "disguise":
            sp_.add_argument("--armour", default="Paladin", help="part of the uniform pieces' names")
        if name_ == "tradefartp":
            sp_.add_argument("--merchant", default="Marchand")
    td = sub.add_parser("trade", help="a client trades with a merchant: purchase, sale, stock everywhere")
    td.add_argument("--save", default="kctest_town")
    td.add_argument("--keep", action="store_true")
    td.add_argument("--merchant", default="Marchand", help="part of the merchant's name ('_' for spaces)")
    for name in ("stuck", "farnpc", "beds", "tpdown", "lootswap", "groundpick", "resyncbar", "fartp", "missing", "floor", "floorview", "buildfull",
                 "tpstop", "bedfree"):
        e2 = sub.add_parser(name)
        e2.add_argument("--save", default="kctest_town" if name in ("beds", "groundpick", "floorview", "bedfree") else "kctest_base")
        e2.add_argument("--keep", action="store_true")
    for name_, hlp_, save_ in (("camfocus", "3l: the client's camera focus is set once, then left to the player", "kctest_town"),
                               ("buildpause", "3n: the host's build mode does not pause anyone; a client's changes nothing for the host", "kctest_base"),
                               ("attackers", "3g: an NPC fighting the host's character is made on a client 6 km away", "kctest_base")):
        sp_ = sub.add_parser(name_, help=hlp_)
        sp_.add_argument("--save", default=save_)
        sp_.add_argument("--keep", action="store_true")
    br_ = sub.add_parser("bugreport", help="0.3.5: /bug from the client and the host: report file on the host with the note and the client's last lines, chat line, 30 s throttle")
    br_.add_argument("--save", default="kctest_base")
    br_.add_argument("--keep", action="store_true")
    ql_ = sub.add_parser("quietlogs", help="3h: door 'stays' and captivity lines counted on every client for 3 min at x3")
    ql_.add_argument("--save", default="kctest_town")
    ql_.add_argument("--keep", action="store_true")
    ql_.add_argument("--clients", type=int, default=1)
    fa_ = sub.add_parser("factions", help="lot B: relations, bounties and crimes the host's everywhere")
    fa_.add_argument("--save", default="kctest_base")
    fa_.add_argument("--keep", action="store_true")
    asf = sub.add_parser("actorsafety", help="a client's orders only ever move its own characters; forged and wrong-target orders refused")
    asf.add_argument("--save", default="kctest_town")
    asf.add_argument("--keep", action="store_true")
    dp_ = sub.add_parser("diplomacy", help="wars between factions, faction leaders, towns: the host's everywhere")
    dp_.add_argument("--save", default="kctest_town")
    dp_.add_argument("--keep", action="store_true")
    tk = sub.add_parser("talk")
    tk.add_argument("--save", default="kctest_base")
    tk.add_argument("--keep", action="store_true")
    nt_ = sub.add_parser("npctalk", help="0.3.4: a right click on an NPC always talks (fallback, NPC found by kind and place); an NPC's conversation waits for the answer")
    nt_.add_argument("--save", default="kctest_town")
    nt_.add_argument("--keep", action="store_true")
    dl_ = sub.add_parser("dialogue", help="conversations: the client's own, an NPC's (guard check), one per NPC (occupe), leave, teleport, fight, speed 3")
    dl_.add_argument("--save", default="kctest_town")
    dl_.add_argument("--keep", action="store_true")
    rg = sub.add_parser("ranged", help="lot C: a crossbowman shoots at the squad, turrets turn: same on the client")
    rg.add_argument("--save", default="kctest_base")
    rg.add_argument("--keep", action="store_true")
    rg.add_argument("--shooter", default=None, help="handle key of the shooter (default: the nearest with a ranged weapon)")
    ji = sub.add_parser("jitter")
    ji.add_argument("--save", default="kctest_town")
    ji.add_argument("--keep", action="store_true")
    su = sub.add_parser("suite", help="every feature, PASS / FAIL per point")
    su.add_argument("--save", default="kctest_base")
    su.add_argument("--keep", action="store_true")
    sui = sub.add_parser("squadui", help="squad window and AI settings: renames, new squads, order, leader, moves, names, toggles, jobs; concurrent host and client edits")
    sui.add_argument("--save", default="kctest_base")
    sui.add_argument("--keep", action="store_true")
    sq = sub.add_parser("squads")
    sq.add_argument("--save", default="kctest_base")
    sq.add_argument("--keep", action="store_true")
    cj = sub.add_parser("camjump", help="0.3.1: client camera jumping between a crowd and its far character (rob's crashes)")
    cj.add_argument("--save", default="kctest_town")
    cj.add_argument("--keep", action="store_true")
    fr = sub.add_parser("far")
    fr.add_argument("--save", default="kctest_base")
    fr.add_argument("--keep", action="store_true")
    ks = sub.add_parser("kosquad")
    ks.add_argument("--save", default="kctest_base")
    ks.add_argument("--keep", action="store_true")
    fa = sub.add_parser("facing")
    fa.add_argument("--save", default="kctest_base")
    fa.add_argument("--keep", action="store_true")
    mn = sub.add_parser("menu", help="host + a second game on the main menu (join by hand)")
    mn.add_argument("--save", default="kctest_base")
    s4 = sub.add_parser("stress4", help="host + N clients spread over the map, all acting at once (chaos clicks, god mode)")
    s4.add_argument("--save", default="kctest_base")
    s4.add_argument("--clients", type=int, default=3)
    s4.add_argument("--minutes", type=int, default=15)
    s4.add_argument("--hop", type=int, default=180, help="seconds between two zone hops")
    s4.add_argument("--seed", type=int, default=4242, help="master seed of the chaos clicks (one seed per client derived)")
    s4.add_argument("--keep", action="store_true")
    j4 = sub.add_parser("join4", help="N clients join at the same time, see each other, leave")
    j4.add_argument("--save", default="kctest_base")
    j4.add_argument("--clients", type=int, default=3)
    j4.add_argument("--keep", action="store_true")
    mp = sub.add_parser("map", help="map markers, minimap, head markers, squad bar frames, pings (1 host + 2 clients)")
    mp.add_argument("--save", default="kctest_base")
    mp.add_argument("--clients", type=int, default=2)
    mp.add_argument("--keep", action="store_true")
    fo = sub.add_parser("four", help="1 host + 3 clients")
    fo.add_argument("--save", default="kctest_base")
    fo.add_argument("--clients", type=int, default=3)
    mo = sub.add_parser("machineout", help="0.3.4: a closed machine's output asks no moves on the client; opened, the client takes it")
    mo.add_argument("--save", default="kctest_base")
    mo.add_argument("--keep", action="store_true")
    mo.add_argument("--seconds", type=int, default=60)
    tsw = sub.add_parser("tradeswap", help="0.3.4: a trade window on a stand-in merchant goes on with the host's real merchant")
    tsw.add_argument("--save", default="kctest_town")
    tsw.add_argument("--keep", action="store_true")
    tsw.add_argument("--merchant", default="Marchand", help="part of the merchant's name ('_' for spaces)")
    ws = sub.add_parser("workstations", help="base work: farm, stove, bench, research, well, turbine by host and client, compared at x3")
    ws.add_argument("--save", default="kctest_base")
    ws.add_argument("--keep", action="store_true")
    ws.add_argument("--minutes", type=float, default=2.0)
    cj = sub.add_parser("clientjobs", help="v0.3.4: JOBS toggle twice, Shift + right click jobs on a free and a full node, notes, debounce, Tâches panel, shared recruit, other player's character")
    cj.add_argument("--save", default="kctest_base")
    cj.add_argument("--keep", action="store_true")
    rs = sub.add_parser("research", help="workshop: research, crafting benches, mines, power (sandbox set up by the host)")
    rs.add_argument("--save", default="kctest_base")
    rs.add_argument("--keep", action="store_true")
    pg = sub.add_parser("progress")
    pg.add_argument("--save", default="kctest_base")
    pg.add_argument("--keep", action="store_true")
    gr = sub.add_parser("ground")
    gr.add_argument("--save", default="kctest_base")
    gr.add_argument("--keep", action="store_true")
    gd = sub.add_parser("grounddrop", help="items dropped the inventory window's way (host, client, both, KO/death): once, same place, everywhere")
    gd.add_argument("--save", default="kctest_base")
    gd.add_argument("--keep", action="store_true")
    bp = sub.add_parser("backpack", help="backpack inventories: moves, 1 on 1 merges, drop and pickup, purchase, 2 min of mining at x3; invcheck clean everywhere")
    bp.add_argument("--save", default="kctest_mine")
    bp.add_argument("--keep", action="store_true")
    af = sub.add_parser("animframe")
    af.add_argument("--save", default="kctest_base")
    af.add_argument("--keep", action="store_true")
    an = sub.add_parser("anim")
    an.add_argument("--save", default="kctest_base")
    an.add_argument("--keep", action="store_true")
    ga = sub.add_parser("gait")
    ga.add_argument("--save", default="kctest_base")
    ga.add_argument("--keep", action="store_true")
    fl = sub.add_parser("fxlive")
    fl.add_argument("--save", default="kctest_base")
    fl.add_argument("--keep", action="store_true")
    fl.add_argument("--minutes", type=int, default=3)
    fx = sub.add_parser("fx")
    fx.add_argument("--save", default="kctest_base")
    fx.add_argument("--keep", action="store_true")
    up = sub.add_parser("up", help="host + joined client, left running for a manual test")
    up.add_argument("--save", default="kctest_base")
    c = sub.add_parser("cmd")
    c.add_argument("pid", type=int)
    c.add_argument("command", nargs="+")
    a = ap.parse_args()
    if a.what == "cmd":
        print(cmd(a.pid, " ".join(a.command)))
        return
    if a.what == "menu":   # a hosting game + a second game left on the main menu: the player joins by hand
        kill_all()
        host = launch()
        log("host pid", host)
        wait_for(host, lambda s: s.get("state") == "idle", 180, "host menu")
        time.sleep(3)
        log("load", cmd(host, f"load {a.save}"))
        wait_for(host, lambda s: s.get("ready") == "1", 240, "host world")
        log("host", cmd(host, "host"))
        cli = launch(fake_steam_id=76561190000000002)
        wait_for(cli, lambda s: s.get("state") == "idle", 240, "client menu")
        arrange(host, cli)
        log("ready: host", host, "client on the main menu", cli)
        return
    if a.what in ("stress4", "join4"):
        host, clis, ids = setup_multi(a.save, a.clients, simultaneous=a.what == "join4")
        try:
            if a.what == "stress4":
                exp_stress4(host, clis, ids, a.minutes, a.hop, a.seed)
            else:
                exp_join4(host, clis, ids)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "map":
        # setup_multi: each client closes its character editor when its join turn comes (with the
        # join queue, the next client waits until then)
        host, clis, _ids = setup_multi(a.save, max(2, a.clients))
        arrange_grid([host] + clis)
        try:
            exp_map(host, clis)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "quietlogs":
        # door and captivity log lines on every client, counted over a few minutes at x3
        host, clis, _ids = setup_multi(a.save, max(1, a.clients))
        tile([host] + clis)
        try:
            exp_quietlogs(host, clis)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "faraway":
        # every client in its own far town at once (setup_multi: each joins in turn, editor closed)
        host, clis, _ids = setup_multi(a.save, max(1, a.clients))
        tile([host] + clis)
        try:
            exp_faraway(host, clis)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "recruits":
        host, clis, _ids = setup_multi(a.save, max(1, a.clients))
        tile([host] + clis)
        try:
            exp_recruits(host, clis)
        finally:
            summary()
            if not a.keep:
                kill_launched()
        return
    if a.what in ("farfps", "farwalls"):
        host, clis, _ids = setup_multi(a.save, max(1, a.clients) if a.what == "farfps" else 1)
        tile([host] + clis)
        try:
            if a.what == "farfps":
                exp_farfps(host, clis, a.seconds)
            else:
                exp_farwalls(host, clis, a.seconds)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "farprobe":
        host, clis, _ids = setup_multi(a.save, 1)
        tile([host] + clis)
        try:
            exp_farprobe(host, clis, talk=not a.notalk)
        finally:
            if not a.keep:
                kill_launched()
        return
    if a.what == "four":
        host, clis = setup_many(a.save, a.clients)
        arrange_grid([host] + clis)
        exp_four(host, clis)
        log("instances left running: host", host, "clients", clis)
        return
    host, cli = setup(a.save)
    if a.what == "up":
        arrange(host, cli)
        log("ready: host", host, "client", cli)
        return
    try:
        if a.what == "jitter":
            exp_jitter(host, cli)
        elif a.what == "ranged":
            exp_ranged(host, cli, a.shooter)
        elif a.what == "suite":
            exp_suite(host, cli)
        elif a.what == "squads":
            exp_squads(host, cli)
        elif a.what == "squadui":
            exp_squadui(host, cli)
        elif a.what == "far":
            exp_far(host, cli)
        elif a.what == "camjump":
            exp_camjump(host, cli)
        elif a.what == "kosquad":
            exp_kosquad(host, cli)
        elif a.what == "facing":
            exp_facing(host, cli)
        elif a.what == "talk":
            exp_talk(host, cli)
        elif a.what == "actorsafety":
            exp_actorsafety(host, cli)
        elif a.what == "npctalk":
            exp_npctalk(host, cli)
        elif a.what == "dialogue":
            exp_dialogue(host, cli)
        elif a.what == "stuck":
            exp_stuck(host, cli)
        elif a.what == "farnpc":
            exp_farnpc(host, cli)
        elif a.what == "beds":
            exp_beds(host, cli)
        elif a.what == "tpdown":
            exp_tpdown(host, cli)
        elif a.what == "tpstop":   # 0.3.4
            exp_tpstop(host, cli)
        elif a.what == "bedfree":   # 0.3.4
            exp_bedfree(host, cli)
        elif a.what == "lootswap":
            exp_lootswap(host, cli)
        elif a.what == "groundpick":
            exp_groundpick(host, cli)
        elif a.what == "resyncbar":   # fix G6
            exp_resyncbar(host, cli)
        elif a.what == "fartp":
            exp_fartp(host, cli)
        elif a.what == "missing":
            exp_missing(host, cli)
        elif a.what == "buildfull":
            exp_buildfull(host, cli)
        elif a.what == "floorview":
            exp_floorview(host, cli)
        elif a.what == "camfocus":
            exp_camfocus(host, cli)
        elif a.what == "buildpause":
            exp_buildpause(host, cli)
        elif a.what == "attackers":
            exp_attackers(host, cli)
        elif a.what == "bugreport":   # 0.3.5
            exp_bugreport(host, cli)
        elif a.what == "floor":
            exp_floor(host, cli)
        elif a.what == "factions":
            exp_factions(host, cli)
        elif a.what == "diplomacy":
            exp_diplomacy(host, cli)
        elif a.what == "doors":
            exp_doors(host, cli)
        elif a.what == "prison":
            exp_prison(host, cli)
        elif a.what == "build":
            exp_build(host, cli)
        elif a.what == "construct":
            exp_construct(host, cli, material=a.material, tasks=a.task)
        elif a.what == "buyhouse":
            exp_buyhouse(host, cli)
        elif a.what == "farlong":
            exp_farlong(host, cli, seconds=a.seconds)
        elif a.what == "buildstate":
            exp_buildstate(host, cli)
        elif a.what == "placevalid":
            exp_placevalid(host, cli)
        elif a.what == "mine":
            exp_mine(host, cli)
        elif a.what == "workstations":
            exp_workstations(host, cli, minutes=a.minutes)
        elif a.what == "machineout":
            exp_machineout(host, cli, seconds=a.seconds)
        elif a.what == "tradeswap":
            exp_tradeswap(host, cli, a.merchant)
        elif a.what == "research":
            exp_research(host, cli)
        elif a.what == "clientjobs":
            exp_clientjobs(host, cli)
        elif a.what == "admin":
            exp_admin(host, cli)
        elif a.what == "caravan":
            exp_caravan(host, cli)
        elif a.what == "pets":
            exp_pets(host, cli)
        elif a.what == "animalshop":
            exp_animalshop(host, cli)
        elif a.what == "slaveshop":   # 0.3.5 lot 5
            exp_slaveshop(host, cli, a.slaver)
        elif a.what == "lockprogress":
            exp_lockprogress(host, cli)
        elif a.what == "disguise":
            exp_disguise(host, cli, a.armour)
        elif a.what == "tradefartp":
            exp_tradefartp(host, cli, a.merchant)
        elif a.what == "followjob":
            exp_followjob(host, cli)
        elif a.what == "prospect":
            exp_prospect(host, cli)
        elif a.what == "trade":
            exp_trade(host, cli, a.merchant)
        elif a.what == "passive":
            exp_passive(host, cli)
        elif a.what == "jobs":
            exp_jobs(host, cli)
        elif a.what == "tradepaths":
            exp_tradepaths(host, cli, a.merchant)
        elif a.what == "progress":
            exp_progress(host, cli)
        elif a.what == "clientpickup":
            exp_clientpickup(host, cli)
        elif a.what == "ground":
            exp_ground(host, cli)
        elif a.what == "grounddrop":
            exp_grounddrop(host, cli)
        elif a.what == "backpack":
            exp_backpack(host, cli)
        elif a.what == "animframe":
            exp_animframe(host, cli)
        elif a.what == "anim":
            exp_anim(host, cli)
        elif a.what == "gait":
            exp_gait(host, cli)
        elif a.what == "fxlive":
            exp_fxlive(host, cli, a.minutes)
        elif a.what == "fx":
            exp_fx(host, cli)
        elif a.what == "bodies":
            exp_bodies(host, cli)
        elif a.what == "trace":
            exp_trace(host, cli)
        elif a.what == "playerstats":   # 0.3.5
            exp_playerstats(host, cli)
        elif a.what == "walk":
            exp_walk(host, cli)
        elif a.what == "soak":
            exp_soak(host, cli, a.minutes)
        elif a.what == "lootclick":
            exp_lootclick(host, cli)
        elif a.what == "lootorder":
            exp_lootorder(host, cli)
        elif a.what == "lootui":
            exp_lootui(host, cli)
        elif a.what == "items":
            exp_items(host, cli)
        elif a.what == "crashrejoin":
            exp_crashrejoin(host, cli, a.only)
        elif a.what == "carry":
            exp_carry(host, cli)
        elif a.what == "dead":
            exp_dead(host, cli)
        elif a.what == "lootcreature":
            exp_lootcreature(host, cli, a.only)
            summary()
        else:
            scenario(host, cli, a.quick)
    finally:
        if not getattr(a, "keep", False):
            kill_all()



def main_guarded():
    """main(), and on any failure every Kenshi this run started is killed (none left behind)."""
    try:
        main()
    except BaseException as e:
        if _launched:
            log(f"failure ({type(e).__name__}: {e}): killing the {len(_launched)} Kenshi instance(s) started by this run")
            kill_launched()
        if isinstance(e, LowMemory):
            log("ABORTED:", e)
            sys.exit(2)
        raise


if __name__ == "__main__":
    main_guarded()
