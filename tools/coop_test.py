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


def launch(fake_steam_id=None):
    clean_command_files()
    before = set(running_pids())
    env = dict(os.environ)
    env.pop("KC_FAKE_STEAM_ID", None)
    if fake_steam_id:
        env["KC_FAKE_STEAM_ID"] = str(fake_steam_id)
    subprocess.Popen([EXE], cwd=KENSHI, env=env)
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
    # bodies far from every player are not replicated on purpose (they are once someone comes close)
    bound = {e["key"] for e in h["entity"].values()}
    far_corpses = {k for k in hk - ck if k not in bound and int(h["char"][k].get("flags", "0")) & 8}
    report["far_corpses_unreplicated"] = len(far_corpses)
    missing = sorted(hk - ck - far_corpses)
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
    ok, t = cmd(host, "host")
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


def arrange_grid(pids):
    """Two rows of two windows (the second row overlaps the first a little on a 1080p screen)."""
    sw, sh = user32.GetSystemMetrics(0), user32.GetSystemMetrics(17)   # SM_CYFULLSCREEN: above the taskbar
    wins = [hwnd for pid in pids for hwnd, w, h in windows_of(pid) if w >= 640]
    if not wins:
        return
    r = wt.RECT()
    user32.GetWindowRect(wins[-1], ctypes.byref(r))
    W, H = r.right - r.left, r.bottom - r.top
    for i, hwnd in enumerate(wins):
        x = 0 if i % 2 == 0 else sw - W
        y = 0 if i < 2 else max(0, sh - H)
        user32.SetWindowPos(hwnd, 0, x, y, W, H, 0x0004)


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
    """Side by side: host on the left half of the screen, client on the right."""
    sw = user32.GetSystemMetrics(0)
    wins = [hwnd for pid in (host, cli) for hwnd, w, h in windows_of(pid) if w >= 640]
    if len(wins) != 2:
        return
    r = wt.RECT()
    user32.GetWindowRect(wins[1], ctypes.byref(r))   # the client's window was never squeezed by the screen edge
    W, H = r.right - r.left, r.bottom - r.top
    user32.SetWindowPos(wins[0], 0, 0, 0, W, H, 0x0004)        # SWP_NOZORDER
    user32.SetWindowPos(wins[1], 0, sw - W, 0, W, H, 0x0004)


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
    log("client places", sid, cmd(cli, f"buildplace {sid} 40 0 0"))
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
    log("host places", sid, cmd(host, f"buildplace {sid} -40 0 90"))
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
        npc = cmd(host, "where npc")[1].split()[-1]
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
    log("teleport the client's character far away", cmd(host, f"teleport {own} {x + 40000} {y + 300} {z + 30000}"))
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
                if not (int(hv["flags"]) & 1) or int(hv["flags"]) & 4:
                    continue
                samples += 1
                worst = max(worst, abs((yaw(hv["face"]) - yaw(cv["face"]) + 540) % 360 - 180))
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
    time.sleep(16)   # until it got there
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
    rep = compare(dump(host, "h_suite_end"), dump(cli, "c_suite_end"), "fin", pos_tol=0.1)
    cmd(host, "pause 0")
    sq_bad = [t for t in rep["squad"] if not isinstance(t[1], (int, float)) or t[1] > 0.1]
    check("fin : escouade identique (positions)", not sq_bad, sq_bad)
    check("fin : aucun etat vital different", rep["vital_flag_mismatch"] == 0, rep["vital_flag_mismatch"])
    check("fin : aucun inventaire different", rep["inventory_mismatch"] == 0, rep["inventory_mismatch_sample"])
    check("fin : personne ne manque chez le client", rep["missing_on_client"] == 0, rep["missing_sample"])
    # --- 11. the host's resync: the player reloads the host's world and is back with its character
    backs = host_log().count("is back with their character")
    log("resync", cmd(host, "resync 2"))
    ok_back = False
    for _ in range(90):
        time.sleep(1)
        if host_log().count("is back with their character") > backs and cmd(cli, "status")[1].find("state=connected") >= 0:
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


def exp_soak(host, cli, minutes=15):
    """Long run at x3 speed: periodic whole-world comparisons, crash detection."""
    time.sleep(8)
    log("give", cmd(host, "give 2 0"))
    log("speed", cmd(host, "speed 3"))
    worst = {}
    for i in range(minutes):
        time.sleep(60)
        if i % 3 == 2:   # keep the squad on the move: the world streams in and out around it
            log("host move", cmd(host, f"moverel 1 {200 if i % 2 else -200} {150 if i % 4 < 2 else -150}"))
            log("client move", cmd(cli, f"moverel 0 {150 if i % 2 else -150} {-100 if i % 4 < 2 else 100}"))
        r = frozen_check(host, cli, f"soak {i + 1}", radius=10000000)
        cmd(host, "speed 3")
        summary = {k: r.get(k) for k in ("host_chars", "client_chars", "missing_on_client", "extra_on_client", "vital_flag_mismatch",
                                         "combat_mismatch", "inventory_mismatch", "weather_mismatch", "hours_diff", "paused", "speed")}
        log(f"soak {i + 1}:", summary)
        for k, v in summary.items():
            if isinstance(v, (int, float)) and k not in ("host_chars", "client_chars"):
                worst[k] = max(worst.get(k, 0), v)
    log("soak worst:", worst)


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


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="what", required=True)
    r = sub.add_parser("run")
    r.add_argument("--save", default="kctest_base")
    r.add_argument("--keep", action="store_true", help="leave the instances running")
    r.add_argument("--quick", action="store_true")
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
    sk = sub.add_parser("soak")
    sk.add_argument("--save", default="kctest_base")
    sk.add_argument("--minutes", type=int, default=15)
    wk = sub.add_parser("walk")
    wk.add_argument("--save", default="kctest_base")
    t = sub.add_parser("trace")
    t.add_argument("--save", default="kctest_base")
    e = sub.add_parser("bodies")
    e.add_argument("--save", default="kctest_base")
    cp = sub.add_parser("clientpickup")
    cp.add_argument("--save", default="kctest_base")
    cp.add_argument("--keep", action="store_true")
    bd = sub.add_parser("build", help="lot E: buildings placed, built, dismantled and bought by everyone")
    bd.add_argument("--save", default="kctest_base")
    bd.add_argument("--keep", action="store_true")
    td = sub.add_parser("trade", help="a client trades with a merchant: purchase, sale, stock everywhere")
    td.add_argument("--save", default="kctest_town")
    td.add_argument("--keep", action="store_true")
    td.add_argument("--merchant", default="Marchand", help="part of the merchant's name ('_' for spaces)")
    tk = sub.add_parser("talk")
    tk.add_argument("--save", default="kctest_base")
    tk.add_argument("--keep", action="store_true")
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
    fo = sub.add_parser("four", help="1 host + 3 clients")
    fo.add_argument("--save", default="kctest_base")
    fo.add_argument("--clients", type=int, default=3)
    pg = sub.add_parser("progress")
    pg.add_argument("--save", default="kctest_base")
    pg.add_argument("--keep", action="store_true")
    gr = sub.add_parser("ground")
    gr.add_argument("--save", default="kctest_base")
    gr.add_argument("--keep", action="store_true")
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
        elif a.what == "build":
            exp_build(host, cli)
        elif a.what == "trade":
            exp_trade(host, cli, a.merchant)
        elif a.what == "progress":
            exp_progress(host, cli)
        elif a.what == "clientpickup":
            exp_clientpickup(host, cli)
        elif a.what == "ground":
            exp_ground(host, cli)
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
        elif a.what == "dead":
            exp_dead(host, cli)
        else:
            scenario(host, cli, a.quick)
    finally:
        if not getattr(a, "keep", False):
            kill_all()


if __name__ == "__main__":
    main()
