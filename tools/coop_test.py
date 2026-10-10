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
import threading
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


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def dump(pid, name, radius=3000):
    os.makedirs(OUT_DIR, exist_ok=True)
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
    time.sleep(6)
    for c in clis:
        cmd(c, "editdone")
    time.sleep(4)

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
        t = cmd(pid, "mapscene tetes")[1]
        heads = items(t, 1)
        check(f"tetes : {label} repere au-dessus des persos des joueurs, a leur couleur", len(heads) >= 1 and all(h.get("col") for h in heads), t[:300])
        t = cmd(pid, "mapscene barre")[1]
        log(f"{label} squad bar frames:", t[:300])
        frames = items(t, 1)
        check(f"barre : {label} cadres aux couleurs des joueurs seulement", all(f.get("owner") in owners for f in frames), t[:200])
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
    host to player, player to a map point) land where the client sees them; heal; money."""
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
    # 11. everyone at once, the host included
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
    time.sleep(6)
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


# ---- fix G5: client orders never reach the host's characters; the TÃƒÂ¢ches panel; trade windows
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
    for part in ([name] if name else []) + ["Building_Materials", "Matériaux_de_construction", "Materiaux_de_construction", "construction"]:
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
        log(f"materials for squad member {i}", cmd(host, f"giveitem {mat} 40 {i}"))
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
    TÃƒÂ¢ches panel (the cross): the job is gone on the host too and does not come back on the client."""
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
                  and moved < 5.0, f"{a} -> {b} (deplace de {moved:.1f})")
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
    time.sleep(10)
    cmd(host, f"kosquad {own}")
    time.sleep(4)
    log("tp", cmd(host, "tpplayer 2"))
    time.sleep(5)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    pcc = vec(cmd(cli, f"where {own}")[1])
    check("TP admin (perso a terre) : il arrive pres de l'hote", dist(p0, pc) < 30, f"{dist(p0, pc):.1f}")
    check("TP admin (perso a terre) : le client le voit au meme endroit", dist(pc, pcc) < 3, f"{dist(pc, pcc):.2f}")
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
    cmd(host, "tpplayer 2")
    time.sleep(3)
    p0, pc = vec(cmd(host, "where 0")[1]), vec(cmd(host, f"where {own}")[1])
    pcc = vec(cmd(cli, f"where {own}")[1])
    check("TP admin : le perso du joueur arrive pres de l'hote", dist(p0, pc) < 30, f"{dist(p0, pc):.1f}")
    check("TP admin : le client le voit au meme endroit", dist(pc, pcc) < 2, f"{dist(pc, pcc):.2f}")
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
    # a body lying on the ground is a ragdoll each game simulates itself: a few units apart is expected
    down = {k for k, v in hs_end["squad"].items() if int(v.get("vflags", 0) or 0) & 3}
    sq_bad = [t for t in rep["squad"] if not isinstance(t[1], (int, float)) or t[1] > (8.0 if t[0] in down else 0.1)]
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
    owner = d["entity"].get(akey, {}).get("owner")
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
    time.sleep(4)
    h1, c1 = pos(host, key), pos(cli, key)
    time.sleep(3)
    c2 = pos(cli, key)
    flat = lambda p: (p[0], 0, p[2]) if p else None
    check("poser PNJ : le corps est la ou celui de l'hote est tombe", dist(flat(h1), flat(c1)) < 5,
          f"hote {h1} / client {c1} (ecart en hauteur {abs(h1[1] - c1[1]):.1f})" if h1 and c1 else f"hote {h1} / client {c1}")
    check("poser PNJ : pas projete (immobile apres la chute)", dist(c1, c2) < 2, f"{c1} -> {c2}")
    check("poser PNJ : plus porte chez le client", cmd(cli, "carrying 0")[1] == "ok none")



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
    client sends its join at once; the host takes them one at a time (join queue), the others show
    their place; each closes its character editor when its turn comes (what was seen of the queue
    goes to JOIN_QUEUE). Each client must own a character of its own.
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
        log("every client joins at the same time: they go through the host's join queue one by one")
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
                        log(f"client {i + 1} in the join queue: position {q[0]}, waiting for {q[1]} ({q[2]})")
                    JOIN_QUEUE["seen"][c].add(q)
                elif s.get("state") == "loading":
                    busy.append(c)
                if s.get("state") == "connected" and s.get("ready") == "1":
                    log(f"client {i + 1} connected")
                    close_join_editor(host, c, f"Joueur{i + 2}", i)
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
    """How many items of template sid the characters of a dump carry (squad + others; only keys if given)."""
    n = 0
    for tag in ("squad", "char"):
        for k, v in state[tag].items():
            if keys is not None and k not in keys:
                continue
            for it in (v.get("inv") or "").split(";"):
                m = re.match(r"(.+)x(\d+)@", it)
                if m and m.group(1) == sid:
                    n += int(m.group(2))
    return n


def all_keys(state):
    return set(state["squad"]) | set(state["char"])


def char_pos(state, key):
    v = state["squad"].get(key) or state["char"].get(key) or {}
    return v.get("pos")


def keys_of_owner(state, owner):
    return [k for k, e in ((e["key"], e) for e in state["entity"].values()) if e.get("squad") == "1" and e.get("owner") == str(owner)]


def exp_join4(host, clis, ids):
    """3 clients joined at the same time: the host's join queue took them one at a time (the others
    shown their place, nobody dropped), everyone sees every player's characters (names, positions),
    then every client leaves and the host carries on."""
    n = len(clis)
    who = f"{n + 1} joueurs"
    check(f"{who} : chaque client est connecte", all(status(c).get("state") == "connected" for c in clis),
          [(c, status(c).get("state")) for c in clis])
    check(f"{who} : ids de joueur distincts", len(set(ids.values())) == len(ids), ids)
    # the join queue: one player at a time, the others shown their place, nobody dropped
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
            if not (2 <= p_ <= t_ <= n) or not wait.startswith("Joueur") or phase not in ("saving", "loading", "editor"):
                bad_pos.append((clis.index(c) + 1, pos, wait, phase))
    check(f"{who} : file d'attente : positions (2..{n}/{n}), joueur attendu et etape coherents", waited and not bad_pos, bad_pos[:6])
    last = order[-1] if order else None
    check(f"{who} : file d'attente : le dernier arrive a vu sa position avancer",
          last is not None and len({q[0] for q in seen.get(last, set())}) >= 2, sorted(seen.get(last, [])) if last else None)
    check(f"{who} : file d'attente : un seul joueur charge le monde a la fois", not JOIN_QUEUE.get("overlap"), JOIN_QUEUE.get("overlap", [])[:5])
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
                squad_bad = [t for t in rep["squad"] if not isinstance(t[1], (int, float)) or t[1] > (8.0 if t[0] in down else 0.1)]
                s["cmp"][c] = {"squad_bad": squad_bad, "vital": rep["vital_flag_mismatch"], "inv": rep["inventory_mismatch"],
                               "inv_sample": rep["inventory_mismatch_sample"], "hours": rep.get("hours_diff", 0)}
                # per zone: what the host has around this client's characters, as the client has it
                centers = [char_pos(h, k) for k in keys_of_owner(h, ids[c]) if char_pos(h, k)]
                near = [k for k, v in h["char"].items() if "pos" in v and centers and min(dist(v["pos"], p) for p in centers) < 1500]
                bad_now = set()
                for k in near:
                    cv = cs["char"].get(k)
                    if not cv or "pos" not in cv or dist(cv["pos"], h["char"][k]["pos"]) > 3.0:
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
    check(f"{who} : escouade identique en pause chez chaque client (<= 0.1, 8 pour un corps au sol)",
          not per_client("squad_bad", bool), per_client("squad_bad", bool)[:5])
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
    frames = [s["frame"][0] for s in samples]
    check(f"{who} : temps de frame de l'hote (meilleur aller-retour < 250 ms)", frames and max(frames) < 250, f"releves {frames}")
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
                                 ("jobs", "kctest_base", "fix G5: a job removed in the client's TÃƒÂ¢ches panel is gone on the host"),
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
    fl_ = sub.add_parser("farlong", help="the client's character far away (>= 30000) for a long time: zone compared every 30 s, fight, building")
    fl_.add_argument("--save", default="kctest_base")
    fl_.add_argument("--keep", action="store_true")
    fl_.add_argument("--seconds", type=int, default=300, help="time spent far away")
    for name_, hlp_ in (("caravan", "travelling merchant: packs synced, buy/sell while it walks, window closed on a kill, theft and loot of pack animals"),
                        ("pets", "squad animals: ownership, owner's squad, orders through the host, shared inventory")):
        sp_ = sub.add_parser(name_, help=hlp_)
        sp_.add_argument("--save", default="kctest_town")
        sp_.add_argument("--keep", action="store_true")
    td = sub.add_parser("trade", help="a client trades with a merchant: purchase, sale, stock everywhere")
    td.add_argument("--save", default="kctest_town")
    td.add_argument("--keep", action="store_true")
    td.add_argument("--merchant", default="Marchand", help="part of the merchant's name ('_' for spaces)")
    for name in ("stuck", "farnpc", "beds", "tpdown", "lootswap", "groundpick", "resyncbar", "fartp", "missing", "floor"):
        e2 = sub.add_parser(name)
        e2.add_argument("--save", default="kctest_town" if name in ("beds", "groundpick") else "kctest_base")
        e2.add_argument("--keep", action="store_true")
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
    sq = sub.add_parser("squads")
    sq.add_argument("--save", default="kctest_base")
    sq.add_argument("--keep", action="store_true")
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
    pg = sub.add_parser("progress")
    pg.add_argument("--save", default="kctest_base")
    pg.add_argument("--keep", action="store_true")
    gr = sub.add_parser("ground")
    gr.add_argument("--save", default="kctest_base")
    gr.add_argument("--keep", action="store_true")
    gd = sub.add_parser("grounddrop", help="items dropped the inventory window's way (host, client, both, KO/death): once, same place, everywhere")
    gd.add_argument("--save", default="kctest_base")
    gd.add_argument("--keep", action="store_true")
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
        host, clis = setup_many(a.save, max(2, a.clients))
        arrange_grid([host] + clis)
        try:
            exp_map(host, clis)
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
        elif a.what == "far":
            exp_far(host, cli)
        elif a.what == "kosquad":
            exp_kosquad(host, cli)
        elif a.what == "facing":
            exp_facing(host, cli)
        elif a.what == "talk":
            exp_talk(host, cli)
        elif a.what == "actorsafety":
            exp_actorsafety(host, cli)
        elif a.what == "stuck":
            exp_stuck(host, cli)
        elif a.what == "farnpc":
            exp_farnpc(host, cli)
        elif a.what == "beds":
            exp_beds(host, cli)
        elif a.what == "tpdown":
            exp_tpdown(host, cli)
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
        elif a.what == "admin":
            exp_admin(host, cli)
        elif a.what == "caravan":
            exp_caravan(host, cli)
        elif a.what == "pets":
            exp_pets(host, cli)
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
