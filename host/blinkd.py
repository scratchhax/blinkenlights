#!/usr/bin/env python3
"""blinkd - feeds the blinkenlights panel with everything the LLM host is doing.

Samples llama.cpp, strata or Maya (found by process and port, or --url), AMD GPUs through sysfs, and the
kernel's /proc counters, normalizes it all, and sends a 92-byte frame 20 times a second to
the ESP32 over USB serial and/or UDP. The panel does all the animation itself.

    blinkd.py --udp blinkpanel.local            # the usual setup: panel on Wi-Fi
    blinkd.py --serial /dev/ttyUSB0             # panel on USB
    blinkd.py --stdout | ../preview/preview     # watch it in a terminal
    blinkd.py --simulate --stdout | ../preview/preview
    blinkd.py --dump                            # print the normalized signals once a second

Standard library only; written for Python 3.10 (Ubuntu 22.04).
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import random
import re
import socket
import struct
import sys
import termios
import threading
import time
import tty
import urllib.error
import urllib.parse
import urllib.request

# --------------------------------------------------------------------------- protocol
# Mirrors firmware/lib/blink/blink_protocol.h; keep the two in step.
LEVELS = [
    "gpu_busy", "gpu_mem_busy", "vram", "gpu_power", "gpu_temp", "gpu_clock",
    "swap", "cpu_iowait", "cpu_irq", "mem", "psi_mem", "psi_io", "psi_cpu",
    "disk_read", "disk_write", "net_rx", "net_tx", "ctx_switch", "interrupts", "maj_faults",
    "tok_s", "prefill", "context", "expert_hit", "queued", "running", "pcie",
    "paging", "loadavg", "cpu_freq", "spec_accept", "connections",
]
RAW_COUNTS = {"queued", "running", "connections"}  # sent as plain counts, not 0..1
COUNTERS = ["tokens", "prompt8", "req_start", "req_end", "drafts_accepted", "disk_bursts"]
ENGINE_NONE, ENGINE_LLAMA, ENGINE_STRATA, ENGINE_MAYA = 0, 1, 2, 3
STATE_DOWN, STATE_IDLE, STATE_READING, STATE_GENERATING = 0, 1, 2, 3
N_CORES, N_GPUS, N_THREADS = 16, 8, 14
FULL_TPS = 120.0  # tokens a second that count as "flat out" (--max-tps)
assert len(LEVELS) == 32 and len(COUNTERS) == 6


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def u8(v: float) -> int:
    return 0 if v != v else max(0, min(255, int(round(v * 255))))


def pack_frame(seq: int, engine: int, state: int, levels: dict, cores: list, gpus: list,
               threads: list, counters: dict, brightness: int = 0) -> bytes:
    lv = bytes(min(255, max(0, int(levels.get(n, 0)))) if n in RAW_COUNTS else u8(levels.get(n, 0.0))
               for n in LEVELS)
    body = (bytes([ord("B"), ord("L"), 1, seq & 0xFF, engine, state, min(len(gpus), N_GPUS), brightness & 0xFF]) + lv
            + bytes(u8(v) for v in (cores + [0.0] * N_CORES)[:N_CORES])
            + bytes(u8(v) for v in (gpus + [0.0] * N_GPUS)[:N_GPUS])
            + bytes(u8(v) for v in (threads + [0.0] * N_THREADS)[:N_THREADS])
            + struct.pack("<6H", *(int(counters.get(n, 0)) & 0xFFFF for n in COUNTERS)))
    return body + struct.pack("<H", crc16(body))


# --------------------------------------------------------------------------- helpers
def clamp01(v: float) -> float:
    return 0.0 if v < 0 else 1.0 if v > 1 else v


def lnorm(rate: float, lo: float, hi: float) -> float:
    """Log scale: lo and below -> 0, hi and above -> 1."""
    if rate <= lo:
        return 0.0
    return clamp01(math.log(rate / lo) / math.log(hi / lo))


def read(path: str) -> str | None:
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def read_int(path: str) -> int | None:
    s = read(path)
    try:
        return int(s.strip()) if s else None
    except ValueError:
        return None


class Rate:
    """Per-second rate of a cumulative counter."""

    def __init__(self):
        self.prev: float | None = None

    def __call__(self, value: float, dt: float) -> float:
        prev, self.prev = self.prev, value
        if prev is None or dt <= 0 or value < prev:
            return 0.0
        return (value - prev) / dt


# --------------------------------------------------------------------------- host: /proc
class ProcHost:
    """CPU, memory, pressure, disk, network, interrupts and friends from /proc and /sys."""

    def __init__(self):
        self.ncpu = os.cpu_count() or 1
        self.prev_cpu: dict[str, tuple[int, int, int, int]] = {}
        self.r_ctxt, self.r_intr, self.r_maj = Rate(), Rate(), Rate()
        self.r_swap, self.r_page, self.r_minflt = Rate(), Rate(), Rate()
        self.r_dread, self.r_dwrite, self.r_rx, self.r_tx = Rate(), Rate(), Rate(), Rate()
        self.maj_carry = 0.0
        self.disk_bursts = 0
        self.disks = [d for d in os.listdir("/sys/block")
                      if not re.match(r"(loop|ram|zram|dm-|md|sr|fd)", d)] if os.path.isdir("/sys/block") else []
        self.freq_max = {}
        for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq"):
            v = read_int(p)
            if v:
                self.freq_max[os.path.dirname(p)] = v
        self.slow_at = 0.0
        self.slow: dict = {}

    def sample(self, dt: float, levels: dict) -> list[float]:
        cores = self._stat(dt, levels)
        self._vmstat(dt, levels)
        self._diskstats(dt, levels)
        self._netdev(dt, levels)
        now = time.monotonic()
        if now >= self.slow_at:  # the slower-moving files at 2 Hz
            self.slow_at = now + 0.5
            self.slow = self._slow()
        levels.update(self.slow)
        return cores

    def _stat(self, dt: float, levels: dict) -> list[float]:
        text = read("/proc/stat") or ""
        busy: list[float] = []
        for line in text.splitlines():
            f = line.split()
            if not f:
                continue
            if f[0].startswith("cpu"):
                v = [int(x) for x in f[1:9]]
                total, idle = sum(v), v[3] + v[4]
                prev = self.prev_cpu.get(f[0])
                self.prev_cpu[f[0]] = (total, idle, v[4], v[5] + v[6])
                if not prev or total <= prev[0]:
                    frac, iow, irq = 0.0, 0.0, 0.0
                else:
                    d = total - prev[0]
                    frac = 1 - (idle - prev[1]) / d
                    iow = (v[4] - prev[2]) / d
                    irq = (v[5] + v[6] - prev[3]) / d
                if f[0] == "cpu":
                    levels["cpu"], levels["cpu_iowait"], levels["cpu_irq"] = frac, iow * 4, irq * 8
                else:
                    busy.append(clamp01(frac))
            elif f[0] == "ctxt":
                levels["ctx_switch"] = lnorm(self.r_ctxt(int(f[1]), dt), 5e3, 1e6)
            elif f[0] == "intr":
                levels["interrupts"] = lnorm(self.r_intr(int(f[1]), dt), 2e3, 5e5)
        if not busy:
            return [0.0] * N_CORES
        group = math.ceil(len(busy) / N_CORES)
        if group == 1:  # fewer than 16 cores: repeat them around
            return [busy[i % len(busy)] for i in range(N_CORES)]
        return [sum(busy[i * group:(i + 1) * group]) / max(1, len(busy[i * group:(i + 1) * group]))
                for i in range(N_CORES)]

    def _vmstat(self, dt: float, levels: dict) -> None:
        v = {}
        for line in (read("/proc/vmstat") or "").splitlines():
            k, _, n = line.partition(" ")
            if k in ("pgmajfault", "pgfault", "pswpin", "pswpout", "pgpgin", "pgpgout"):
                v[k] = int(n)
        # Swap traffic (pages a second) and page-cache churn: KB paged in/out plus minor faults.
        levels["swap"] = lnorm(self.r_swap(v.get("pswpin", 0) + v.get("pswpout", 0), dt), 1, 2e4)
        churn = self.r_page(v.get("pgpgin", 0) + v.get("pgpgout", 0), dt) + self.r_minflt(v.get("pgfault", 0), dt) * 4
        levels["paging"] = lnorm(churn, 200, 2e6)
        if "pgmajfault" in v:
            rate = self.r_maj(v["pgmajfault"], dt)
            levels["maj_faults"] = lnorm(rate, 1, 2e4)
            # a burst of major faults (a model paging in from disk) drops amber streaks
            self.maj_carry += rate * dt / 50
            n = int(self.maj_carry)
            self.maj_carry -= n
            self.disk_bursts += min(n, 10)

    def _diskstats(self, dt: float, levels: dict) -> None:
        rd = wr = 0
        for line in (read("/proc/diskstats") or "").splitlines():
            f = line.split()
            if len(f) > 10 and f[2] in self.disks:
                rd += int(f[5]) * 512
                wr += int(f[9]) * 512
        levels["disk_read"] = lnorm(self.r_dread(rd, dt) / 1e6, 0.5, 7000)
        levels["disk_write"] = lnorm(self.r_dwrite(wr, dt) / 1e6, 0.5, 7000)

    def _netdev(self, dt: float, levels: dict) -> None:
        rx = tx = 0
        for line in (read("/proc/net/dev") or "").splitlines()[2:]:
            name, _, rest = line.partition(":")
            if name.strip() == "lo":
                continue
            f = rest.split()
            if len(f) >= 9:
                rx += int(f[0])
                tx += int(f[8])
        levels["net_rx"] = lnorm(self.r_rx(rx, dt) / 1e6, 0.05, 1250)
        levels["net_tx"] = lnorm(self.r_tx(tx, dt) / 1e6, 0.05, 1250)

    def _slow(self) -> dict:
        out: dict = {}
        mem = {}
        for line in (read("/proc/meminfo") or "").splitlines():
            k, _, v = line.partition(":")
            mem[k] = int(v.split()[0]) if v.split() else 0
        if mem.get("MemTotal"):
            out["mem"] = 1 - mem.get("MemAvailable", 0) / mem["MemTotal"]
        for name, key in (("cpu", "psi_cpu"), ("memory", "psi_mem"), ("io", "psi_io")):
            m = re.search(r"some avg10=([\d.]+)", read(f"/proc/pressure/{name}") or "")
            out[key] = clamp01(float(m.group(1)) / 25) if m else 0.0
        load = (read("/proc/loadavg") or "0").split()
        out["loadavg"] = clamp01(float(load[0]) / self.ncpu)
        ratios = []
        for d, mx in self.freq_max.items():
            cur = read_int(d + "/scaling_cur_freq")
            if cur:
                ratios.append(cur / mx)
        out["cpu_freq"] = sum(ratios) / len(ratios) if ratios else 0.0
        return out


# --------------------------------------------------------------------------- host: AMD GPUs
class AmdGpus:
    """Every AMD GPU (vendor 0x1002) the amdgpu driver exposes in sysfs."""

    def __init__(self, pcie: bool = True):
        self.cards = []
        for card in sorted(glob.glob("/sys/class/drm/card[0-9]*"), key=lambda p: int(re.sub(r"\D", "", p) or 0)):
            if not re.fullmatch(r"card\d+", os.path.basename(card)):
                continue
            dev = os.path.join(card, "device")
            if (read(dev + "/vendor") or "").strip() != "0x1002" or not os.path.exists(dev + "/gpu_busy_percent"):
                continue
            hw = sorted(glob.glob(dev + "/hwmon/hwmon*"))
            self.cards.append({"dev": dev, "hwmon": hw[0] if hw else None})
        self.pcie_mbs = 0.0
        self.slow_at = 0.0
        self.slow: dict = {}
        if pcie and self.cards:
            threading.Thread(target=self._pcie_loop, daemon=True).start()

    def sample(self, levels: dict) -> list[float]:
        if not self.cards:
            return []
        busy = []
        mem_busy = []
        for c in self.cards:
            b = read_int(c["dev"] + "/gpu_busy_percent")
            busy.append((b or 0) / 100)
            m = read_int(c["dev"] + "/mem_busy_percent")
            if m is not None:
                mem_busy.append(m / 100)
        levels["gpu_busy"] = sum(busy) / len(busy)
        levels["gpu_mem_busy"] = max(mem_busy) if mem_busy else levels["gpu_busy"]
        now = time.monotonic()
        if now >= self.slow_at:
            self.slow_at = now + 0.5
            self.slow = self._slow()
        levels.update(self.slow)
        levels["pcie"] = lnorm(self.pcie_mbs, 1, 32000)
        return busy

    def _slow(self) -> dict:
        used = total = power = cap = 0
        temps, clocks = [], []
        for c in self.cards:
            used += read_int(c["dev"] + "/mem_info_vram_used") or 0
            total += read_int(c["dev"] + "/mem_info_vram_total") or 0
            hw = c["hwmon"]
            if hw:
                p = read_int(hw + "/power1_average") or read_int(hw + "/power1_input") or 0
                power += p
                cap += read_int(hw + "/power1_cap") or 0
                for t in glob.glob(hw + "/temp*_input"):
                    v = read_int(t)
                    if v:
                        temps.append(v / 1000)
            sclk = read(c["dev"] + "/pp_dpm_sclk") or ""
            mhz = [(int(m.group(1)), "*" in line) for line in sclk.splitlines()
                   for m in [re.search(r"(\d+)\s*Mhz", line, re.I)] if m]
            if mhz:
                cur = next((v for v, star in mhz if star), mhz[0][0])
                clocks.append(cur / max(v for v, _ in mhz))
        return {
            "vram": used / total if total else 0.0,
            "gpu_power": power / cap if cap else 0.0,
            "gpu_temp": clamp01((max(temps) - 30) / 65) if temps else 0.0,
            "gpu_clock": sum(clocks) / len(clocks) if clocks else 0.0,
        }

    def _pcie_loop(self) -> None:
        # pcie_bw blocks about a second while the GPU counts, so it gets its own thread.
        failures = 0
        while failures < 3:
            total = 0.0
            ok = False
            for c in self.cards:
                s = read(c["dev"] + "/pcie_bw")
                if s:
                    f = s.split()
                    if len(f) == 3:
                        total += (int(f[0]) + int(f[1])) * int(f[2]) / 1e6
                        ok = True
            if ok:
                self.pcie_mbs, failures = total, 0
            else:
                failures += 1
                time.sleep(1)


# --------------------------------------------------------------------------- host: engine process
class EngineProcess:
    """The process listening on the engine's port: its threads and its connections."""

    def __init__(self, port: int):
        self.port = port
        self.pid: int | None = None
        self.find_at = 0.0
        self.prev: dict[int, int] = {}
        self.clk = os.sysconf("SC_CLK_TCK")
        self.ncpu = os.cpu_count() or 1
        self.conn_at = 0.0
        self.connections = 0

    def _sockets(self, state: str) -> list[str]:
        port = f"{self.port:04X}"
        inodes = []
        for path in ("/proc/net/tcp", "/proc/net/tcp6"):
            for line in (read(path) or "").splitlines()[1:]:
                f = line.split()
                if len(f) > 9 and f[1].rsplit(":", 1)[-1] == port and f[3] == state:
                    inodes.append(f[9])
        return inodes

    def _find_pid(self) -> int | None:
        targets = {f"socket:[{i}]" for i in self._sockets("0A")}  # 0A = LISTEN
        if not targets:
            return None
        for pid in filter(str.isdigit, os.listdir("/proc")):
            try:
                for fd in os.listdir(f"/proc/{pid}/fd"):
                    if os.readlink(f"/proc/{pid}/fd/{fd}") in targets:
                        return int(pid)
            except OSError:
                continue
        return None

    def sample(self, dt: float, levels: dict) -> list[float]:
        now = time.monotonic()
        if now >= self.conn_at:
            self.conn_at = now + 0.5
            self.connections = len(self._sockets("01"))  # 01 = ESTABLISHED
        levels["connections"] = self.connections
        if self.pid is None or not os.path.exists(f"/proc/{self.pid}"):
            self.pid, self.prev = None, {}
            if now < self.find_at:
                return [0.0] * N_THREADS
            self.find_at = now + 5
            self.pid = self._find_pid()
            if self.pid is None:
                return [0.0] * N_THREADS
        cols = [0.0] * N_THREADS
        total = 0.0
        cur: dict[int, int] = {}
        try:
            tids = sorted(int(t) for t in os.listdir(f"/proc/{self.pid}/task"))
        except OSError:
            self.pid = None
            return cols
        for i, tid in enumerate(tids):
            s = read(f"/proc/{self.pid}/task/{tid}/stat")
            if not s:
                continue
            f = s[s.rfind(")") + 2:].split()
            ticks = int(f[11]) + int(f[12])
            cur[tid] = ticks
            if tid in self.prev and dt > 0:
                used = (ticks - self.prev[tid]) / (dt * self.clk)
                cols[i % N_THREADS] += used
                total += used
        self.prev = cur
        return [clamp01(c) for c in cols]


# --------------------------------------------------------------------------- engines
def http_json(url: str, key: str | None, timeout: float = 0.5):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def http_text(url: str, key: str | None, timeout: float = 0.5) -> str:
    req = urllib.request.Request(url)
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def prometheus(text: str) -> dict[str, float]:
    out = {}
    for line in text.splitlines():
        if line and not line.startswith("#"):
            parts = line.rsplit(" ", 1)
            if len(parts) == 2 and "{" not in parts[0]:
                try:
                    out[parts[0]] = float(parts[1])
                except ValueError:
                    pass
    return out


ENGINE_CMDLINE = re.compile(r"llama-server|llama_server|strata|\bmaya\b|serve/server\.py|serve\.server", re.I)


def _listen_addr(local: str) -> tuple[str, int]:
    """'0100007F:1F90' from /proc/net/tcp -> ('127.0.0.1', 8080); any-address -> 127.0.0.1."""
    addr, port = local.split(":")
    if len(addr) == 8:
        ip = ".".join(str(int(addr[i:i + 2], 16)) for i in (6, 4, 2, 0))
        return ("127.0.0.1" if ip == "0.0.0.0" else ip), int(port, 16)
    return "127.0.0.1", int(port, 16)  # IPv6 any or loopback: IPv4 loopback reaches it too


def discover_engines() -> list[str]:
    """URLs of TCP ports that a llama-server or strata process is listening on."""
    listening: dict[str, tuple[str, int]] = {}
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        for line in (read(path) or "").splitlines()[1:]:
            f = line.split()
            if len(f) > 9 and f[3] == "0A":
                listening[f"socket:[{f[9]}]"] = _listen_addr(f[1])
    urls = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        cmd = (read(f"/proc/{pid}/cmdline") or "").replace("\0", " ")
        if not ENGINE_CMDLINE.search(cmd):
            continue
        try:
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    hit = listening.get(os.readlink(f"/proc/{pid}/fd/{fd}"))
                except OSError:
                    continue
                if hit:
                    urls.append(f"http://{hit[0]}:{hit[1]}")
        except OSError:  # another user's process: run blinkd as root or as that user
            continue
    return sorted(set(urls))


class EnginePoller(threading.Thread):
    """Polls whichever engine answers and keeps a snapshot for the main loop. It tries the given
    URLs first, then any port a llama-server or strata process listens on, then llama.cpp's default
    port, so switching engines needs no restart."""

    def __init__(self, urls: list[str] | str | None, key: str | None):
        super().__init__(daemon=True)
        if isinstance(urls, str):
            urls = [urls]
        self.given = [u.rstrip("/") for u in (urls or [])]
        self.url = self.given[0] if self.given else "http://127.0.0.1:8080"
        self.key = key
        self.lock = threading.Lock()
        self.engine = ENGINE_NONE
        self.state = STATE_DOWN
        self.levels: dict = {}
        self.counts = {n: 0 for n in COUNTERS}
        self._reset()

    def _reset(self) -> None:
        self.prev_decoded: dict = {}
        self.prev_busy: dict = {}
        self.prev_metrics: dict = {}
        self.tok_ema = 0.0
        self.accept_ema = 0.0
        self.prompt_carry = 0.0
        self.metrics_ok = True
        self.metrics_retry = 0.0
        self.strata_prev: dict = {}
        self.last_request_time = None
        self.router = False  # llama-server in router mode (--models-preset): ask for the loaded model
        self.model: str | None = None
        self.model_at = 0.0

    def snapshot(self) -> tuple[int, int, dict, dict]:
        with self.lock:
            return self.engine, self.state, dict(self.levels), dict(self.counts)

    def _bump(self, name: str, n: float) -> None:
        self.counts[name] += int(n)

    @property
    def port(self) -> int:
        return int(self.url.rsplit(":", 1)[-1].split("/")[0]) if self.url.count(":") >= 2 else 80

    def _candidates(self) -> list[str]:
        return list(dict.fromkeys(self.given + discover_engines() + ["http://127.0.0.1:8080"]))

    @staticmethod
    def _needs_model(err) -> bool:
        """A router's 'model name is missing' answer: an HTTP 400, or an error body with any status."""
        if isinstance(err, urllib.error.HTTPError):
            return err.code == 400
        return isinstance(err, dict) and "error" in err

    def _router_model(self, url: str) -> str | None:
        """The model a llama-server router has loaded right now, if any."""
        for m in http_json(url + "/models", self.key, 1.0).get("data") or []:
            st = m.get("status")
            if (st.get("value") if isinstance(st, dict) else st) == "loaded":
                return m.get("id")
        return None

    def _slots_any(self, url: str) -> list:
        """/slots of a plain llama-server, or of the loaded model behind a router."""
        try:
            slots = http_json(url + "/slots", self.key, 1.0)
            if not self._needs_model(slots):
                return slots
        except urllib.error.HTTPError as e:
            if not self._needs_model(e):
                raise
        model = self._router_model(url)
        if not model:
            return []
        return http_json(url + "/slots?model=" + urllib.parse.quote(model), self.key, 1.0)

    def _llama_query(self) -> str:
        return "?model=" + urllib.parse.quote(self.model) if self.router and self.model else ""

    def _llama_slots(self) -> list:
        """/slots for the watched llama.cpp. A router (several models behind one port) answers 400
        without a model name, so from then on ask it which model is loaded every couple of seconds."""
        if not self.router:
            try:
                slots = http_json(self.url + "/slots", self.key)
                if not self._needs_model(slots):
                    return slots
            except urllib.error.HTTPError as e:
                if not self._needs_model(e):
                    raise
            self.router = True
            print(f"blinkd: {self.url} is a llama-server router; following its loaded model", file=sys.stderr)
        now = time.monotonic()
        if now >= self.model_at:
            self.model_at = now + 2.0
            model = self._router_model(self.url)
            if model != self.model:
                if model:
                    print(f"blinkd: router model is {model}", file=sys.stderr)
                self.model = model
                self.prev_decoded, self.prev_busy = {}, {}
                self.metrics_ok, self.metrics_retry = True, 0.0
        if not self.model:
            return []
        slots = http_json(self.url + "/slots" + self._llama_query(), self.key)
        return slots if isinstance(slots, list) else []

    def _busy(self, url: str, engine: int) -> bool:
        """Whether the engine at url is reading a prompt or generating right now."""
        try:
            if engine == ENGINE_LLAMA:
                return any(s.get("is_processing") for s in self._slots_any(url))
            live = http_json(url + "/metrics", self.key, 1.0).get("live") or {}
            return live.get("state") in ("reading", "generating")
        except Exception:
            return False

    def _find(self) -> tuple[int, int]:
        """Pick the engine to watch. Several can be up at once (say strata and llama.cpp behind a
        model router), so prefer one that is busy, then the one already watched, then the first
        that answers."""
        up, down = [], None
        for url in self._candidates():
            try:
                engine, state = self._detect(url)
            except Exception:
                continue
            if engine == ENGINE_NONE:
                continue
            if state == STATE_DOWN:
                down = down or (url, engine, state)
                continue
            up.append((not self._busy(url, engine), url != self.url, url, engine, state))
        if up:
            _, _, url, engine, state = min(up, key=lambda f: (f[0], f[1]))
        elif down:
            url, engine, state = down
        else:
            return ENGINE_NONE, STATE_DOWN
        if url != self.url or engine != self.engine:
            names = {ENGINE_LLAMA: "llama.cpp", ENGINE_STRATA: "strata", ENGINE_MAYA: "Maya"}
            print(f"blinkd: watching {names[engine]} at {url}", file=sys.stderr)
        if url != self.url:
            self.router, self.model, self.model_at = False, None, 0.0
        self.url = url
        return engine, state

    def _busy_elsewhere(self) -> bool:
        """Another engine is working while the watched one is quiet: time to switch."""
        if self.state in (STATE_READING, STATE_GENERATING):
            return False
        for url in self._candidates():
            if url == self.url:
                continue
            try:
                engine, state = self._detect(url)
            except Exception:
                continue
            if engine == ENGINE_NONE or state == STATE_DOWN:
                continue
            if self.state == STATE_DOWN or self._busy(url, engine):  # watched one unloaded, or the other busy
                return True
        return False

    def run(self) -> None:
        while True:
            engine, state = self._find()
            with self.lock:
                if engine != self.engine:
                    self._reset()
                self.engine, self.state = engine, state
                if engine == ENGINE_NONE or state == STATE_DOWN:
                    self.levels = {}
            if engine == ENGINE_NONE or state == STATE_DOWN:
                time.sleep(2)
                continue
            # poll the engine until it stops answering, then detect again
            fails = 0
            last = time.monotonic()
            next_scan = last + 1.5
            while fails < 5:
                period = 0.05 if engine == ENGINE_LLAMA else 0.1
                time.sleep(max(0.0, last + period - time.monotonic()))
                now = time.monotonic()
                dt, last = now - last, now
                try:
                    if engine == ENGINE_LLAMA:
                        self._poll_llama(dt)
                    else:
                        self._poll_strata(dt)
                    fails = 0
                except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
                    fails += 1
                    if fails == 5:
                        print(f"blinkd: lost {self.url}: {e!r}", file=sys.stderr)
                if now >= next_scan:
                    next_scan = now + 1.5
                    if self._busy_elsewhere():
                        break

    def _detect(self, url: str) -> tuple[int, int]:
        try:
            h = http_json(url + "/health", None, 1.0)
        except urllib.error.HTTPError as e:
            return (ENGINE_LLAMA, STATE_DOWN) if e.code == 503 else (ENGINE_NONE, STATE_DOWN)
        if isinstance(h, dict):
            if h.get("service") == "strata":
                return ENGINE_STRATA, STATE_IDLE if h.get("loaded", True) else STATE_DOWN
            # Maya forks strata's server but serves strata's /metrics without naming itself in
            # /health; a version key with no service tag is the only thing that tells them apart.
            if h.get("version") and "service" not in h:
                return ENGINE_MAYA, STATE_IDLE
        return ENGINE_LLAMA, STATE_IDLE

    # ----- llama.cpp: /slots at 20 Hz, /metrics (needs --metrics) at 4 Hz
    def _poll_llama(self, dt: float) -> None:
        slots = self._llama_slots()
        tokens = starts = ends = 0
        running = 0
        reading = generating = False
        ctx = 0.0
        for s in slots:
            sid = s.get("id")
            nt = s.get("next_token") or {}
            if isinstance(nt, list):
                nt = nt[0] if nt else {}
            nd = int(nt.get("n_decoded") or 0)
            busy = bool(s.get("is_processing"))
            was = self.prev_busy.get(sid, False)
            prev = self.prev_decoded.get(sid)
            if busy and not was:
                starts += 1
                prev = 0
            if was and not busy:
                ends += 1
            if prev is not None:
                tokens += nd - prev if nd >= prev else nd
            self.prev_busy[sid], self.prev_decoded[sid] = busy, nd
            if busy:
                running += 1
                if nd == 0:
                    reading = True
                else:
                    generating = True
                if s.get("n_ctx"):
                    ctx = max(ctx, nd / s["n_ctx"])
        inst = tokens / dt if dt > 0 else 0
        self.tok_ema += (inst - self.tok_ema) * 0.25
        lv = {"tok_s": clamp01(self.tok_ema / FULL_TPS), "context": clamp01(ctx), "running": running,
              "prefill": 0.5 if reading else 0.0, "expert_hit": 1.0}

        now = time.monotonic()
        m = None
        if (self.metrics_ok or now >= self.metrics_retry) and now >= self.prev_metrics.get("_at", 0):
            try:
                m = prometheus(http_text(self.url + "/metrics" + self._llama_query(), self.key))
                self.metrics_ok = True
            except urllib.error.HTTPError:  # started without --metrics
                self.metrics_ok, self.metrics_retry = False, now + 30
            self.prev_metrics["_at"] = now + 0.25
        if m:
            lv["queued"] = int(m.get("llamacpp:requests_deferred", 0))
            acc = m.get("llamacpp:spec_decode_num_accepted_tokens_total", 0)
            drafted = m.get("llamacpp:spec_decode_num_draft_tokens_total", 0)
            pa, pd = self.prev_metrics.get("acc"), self.prev_metrics.get("drafted")
            if pa is not None and acc >= pa:
                self._bump("drafts_accepted", acc - pa)
                if drafted > pd:
                    self.accept_ema += ((acc - pa) / (drafted - pd) - self.accept_ema) * 0.3
            self.prev_metrics.update(acc=acc, drafted=drafted)
            self.prev_metrics["prompt_tps"] = m.get("llamacpp:prompt_tokens_seconds", 0)
        lv["spec_accept"] = self.accept_ema
        lv.setdefault("queued", self.levels.get("queued", 0))

        # llama.cpp reports prompt progress only when the prompt is done, so rain while reading
        if reading:
            per_s = 10 + 30 * clamp01(self.prev_metrics.get("prompt_tps", 0) / 2000)
            self.prompt_carry += per_s * dt
        n = int(self.prompt_carry)
        self.prompt_carry -= n

        with self.lock:
            self.state = STATE_GENERATING if generating else STATE_READING if reading else STATE_IDLE
            self.levels = lv
            self._bump("tokens", tokens)
            self._bump("req_start", starts)
            self._bump("req_end", ends)
            self._bump("prompt8", n)

    # ----- strata: its JSON /metrics (what its Monitor tab reads) at 10 Hz
    def _poll_strata(self, dt: float) -> None:
        m = http_json(self.url + "/metrics", self.key)
        live = m.get("live") or {}
        states = {"idle": STATE_IDLE, "reading": STATE_READING, "generating": STATE_GENERATING,
                  "unloaded": STATE_DOWN}
        state = states.get(live.get("state"), STATE_IDLE)
        p = self.strata_prev
        busy = state in (STATE_READING, STATE_GENERATING)

        gen = live.get("generated") or 0
        tokens = 0
        if "gen" in p and busy:
            tokens = gen - p["gen"] if gen >= p["gen"] else gen
        read_ = live.get("prompt_read") or 0
        prompt = 0
        if "read" in p and busy:
            prompt = read_ - p["read"] if read_ >= p["read"] else read_
        self.prompt_carry += prompt / 8
        n_prompt = int(self.prompt_carry)
        self.prompt_carry -= n_prompt

        running = int(live.get("running") or (1 if busy else 0))
        starts = max(0, running - p.get("running", 0)) if "running" in p else 0
        totals = m.get("totals") or {}
        done = int(totals.get("requests") or 0)
        ends = max(0, done - p["done"]) if "done" in p else 0
        drafts = int(totals.get("drafts_accepted") or 0)
        offered = int(totals.get("drafts_offered") or 0)
        d_acc = max(0, drafts - p["drafts"]) if "drafts" in p else 0
        if "offered" in p and offered > p["offered"]:
            self.accept_ema += (d_acc / (offered - p["offered"]) - self.accept_ema) * 0.3
        p.update(gen=gen if busy else 0, read=read_ if busy else 0, running=running, done=done,
                 drafts=drafts, offered=offered)

        lv = {"tok_s": clamp01((live.get("tok_s") or 0) / FULL_TPS), "running": running,
              "queued": int(live.get("queued") or 0) + int(live.get("waiting") or 0),
              "spec_accept": self.accept_ema}
        if live.get("prompt_total"):
            lv["prefill"] = clamp01((live.get("prompt_read") or 0) / live["prompt_total"])
        ctx_max = (m.get("engine") or {}).get("max_context")
        if ctx_max and busy:
            lv["context"] = clamp01(((live.get("prompt_tokens") or 0) + gen) / ctx_max)
        disk = 0
        reqs = m.get("requests") or []
        if reqs:
            newest = reqs[0]
            hit = newest.get("hit_rate")
            lv["expert_hit"] = hit if hit is not None else 1.0
            if newest.get("time") != self.last_request_time:
                if self.last_request_time is not None:
                    disk = min(30, int((newest.get("file_mb") or 0) / 64))  # SSD expert reads
                self.last_request_time = newest.get("time")
        else:
            lv["expert_hit"] = 1.0
        hw = m.get("hardware") or {}
        if hw.get("pcie_rx_mb") is not None:
            lv["pcie_strata"] = lnorm((hw.get("pcie_rx_mb") or 0) + (hw.get("pcie_tx_mb") or 0), 1, 32000)

        with self.lock:
            self.state = state
            self.levels = lv
            self._bump("tokens", tokens)
            self._bump("prompt8", n_prompt)
            self._bump("req_start", starts)
            self._bump("req_end", ends)
            self._bump("drafts_accepted", d_acc)
            self._bump("disk_bursts", disk)


# --------------------------------------------------------------------------- simulation
class Simulator:
    """Fake engine and host activity, so the panel can be tuned with no model running.
    Alternates llama.cpp, strata and Maya each cycle so all three color schemes show."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.counts = {n: 0 for n in COUNTERS}
        self.carry = {n: 0.0 for n in COUNTERS}
        self.phase = None
        self.temp = 0.4

    def _add(self, name: str, amount: float) -> None:
        self.carry[name] += amount
        n = int(self.carry[name])
        self.carry[name] -= n
        self.counts[name] += n

    def sample(self, dt: float):
        t = time.monotonic() - self.t0
        cycle_len = 14.0
        cycle, ct = int(t // cycle_len), t % cycle_len
        engine = (ENGINE_LLAMA, ENGINE_STRATA, ENGINE_MAYA)[cycle % 3]
        expert = engine in (ENGINE_STRATA, ENGINE_MAYA)  # strata's server (and its Maya fork)
        if ct < 3:
            phase, state = "idle", STATE_IDLE
        elif ct < 4.5:
            phase, state = "read", STATE_READING
        elif ct < 12.5:
            phase, state = "gen", STATE_GENERATING
        else:
            phase, state = "idle2", STATE_IDLE
        if phase != self.phase:
            if phase == "read":
                self._add("req_start", 1)
            if phase == "idle2":
                self._add("req_end", 1)
            self.phase = phase
        tps = 32 + 10 * math.sin(t * 0.7) if phase == "gen" else 0
        busy = {"idle": 0.03, "idle2": 0.05, "read": 0.75, "gen": 0.95}[phase]
        if phase == "gen":
            self._add("tokens", tps * dt)
            self._add("drafts_accepted", tps * dt * 0.55)
        if phase == "read":
            self._add("prompt8", 3000 / 8 / 1.5 * dt)
            if expert:
                self._add("disk_bursts", 4 * dt)
        self.temp += ((0.75 if busy > 0.5 else 0.35) - self.temp) * 0.02
        jit = lambda a: clamp01(a + random.uniform(-0.08, 0.08))
        lv = {
            "gpu_busy": jit(busy), "gpu_mem_busy": jit(busy * 0.7), "vram": 0.82, "gpu_power": jit(busy * 0.9),
            "gpu_temp": self.temp, "gpu_clock": jit(0.3 + busy * 0.6), "swap": jit(0.05),
            "cpu_iowait": jit(0.4 if phase == "read" else 0.02), "cpu_irq": jit(0.1), "mem": 0.6,
            "psi_mem": 0.0, "psi_io": jit(0.5 if phase == "read" else 0.0), "psi_cpu": jit(0.05),
            "disk_read": jit(0.8 if phase == "read" else 0.1), "disk_write": jit(0.08),
            "net_rx": jit(0.2), "net_tx": jit(0.3 if phase == "gen" else 0.1),
            "ctx_switch": jit(0.3 + busy * 0.5), "interrupts": jit(0.2 + busy * 0.6),
            "maj_faults": jit(0.6 if phase == "read" else 0.0), "tok_s": clamp01(tps / FULL_TPS),
            "prefill": clamp01((ct - 3) / 1.5) if phase == "read" else 0.0,
            "context": clamp01((ct - 3) / 20) if phase in ("read", "gen") else 0.0,
            "expert_hit": 0.85 if expert else 1.0,
            "queued": 1 if 6 < ct < 9 else 0, "running": 1 if phase in ("read", "gen") else 0,
            "pcie": jit(busy * 0.5), "paging": jit(0.2 + busy * 0.3), "loadavg": jit(0.2 + busy * 0.3),
            "cpu_freq": jit(0.5 + busy * 0.4), "spec_accept": 0.55, "connections": 1,
        }
        cores = [jit(0.05 + busy * random.random() * 0.5) for _ in range(N_CORES)]
        gpus = [jit(busy), jit(busy * 0.9)]
        threads = [jit(busy * random.uniform(0.3, 1.0)) for _ in range(N_THREADS)]
        return engine, state, lv, cores, gpus, threads, dict(self.counts)


# --------------------------------------------------------------------------- outputs
class SerialSink:
    def __init__(self, path: str, baud: int):
        self.path, self.baud, self.fd, self.retry_at = path, baud, None, 0.0

    def _open(self) -> None:
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        speed = getattr(termios, f"B{self.baud}")
        attrs[2] = (attrs[2] & ~termios.HUPCL) | termios.CLOCAL  # don't reset the ESP32 on close
        attrs[4] = attrs[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        self.fd = fd
        print(f"blinkd: serial {self.path} open at {self.baud}", file=sys.stderr)

    def send(self, data: bytes) -> None:
        if self.fd is None:
            if time.monotonic() < self.retry_at:
                return
            try:
                self._open()
            except OSError as e:
                self.retry_at = time.monotonic() + 2
                print(f"blinkd: serial {self.path}: {e}", file=sys.stderr)
                return
        try:
            os.write(self.fd, data)
        except BlockingIOError:
            pass  # buffer full: drop this frame, the next one carries the same counters
        except OSError as e:
            print(f"blinkd: serial {self.path}: {e}; reconnecting", file=sys.stderr)
            os.close(self.fd)
            self.fd, self.retry_at = None, time.monotonic() + 2


class UdpSink:
    """Frames to the panel over Wi-Fi. The host may be a name such as blinkpanel.local; it is
    looked up again every minute, so the panel can change address (DHCP) without a restart.
    A broadcast address such as 192.168.1.255 reaches the panel whatever address it has."""

    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.addr: tuple[str, int] | None = None
        self.resolve_at = 0.0
        self.warned = False

    def _resolve(self) -> None:
        self.resolve_at = time.monotonic() + (60 if self.addr else 5)
        try:
            ip = socket.getaddrinfo(self.host, self.port, socket.AF_INET, socket.SOCK_DGRAM)[0][4][0]
        except OSError as e:
            if not self.warned:
                hint = " (for .local names: sudo apt install libnss-mdns)" if self.host.endswith(".local") else ""
                print(f"blinkd: cannot resolve {self.host}: {e}{hint}", file=sys.stderr)
                self.warned = True
            return
        if not self.addr or self.addr[0] != ip:
            print(f"blinkd: sending to {self.host} at {ip}:{self.port}", file=sys.stderr)
        self.addr, self.warned = (ip, self.port), False

    def send(self, data: bytes) -> None:
        if time.monotonic() >= self.resolve_at:
            self._resolve()
        if self.addr:
            try:
                self.sock.sendto(data, self.addr)
            except OSError:
                pass


class StdoutSink:
    def send(self, data: bytes) -> None:
        try:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        except BrokenPipeError:
            sys.exit(0)


# --------------------------------------------------------------------------- main loop
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", action="append",
                    help="llama.cpp, strata or Maya server to try first (repeat for several); "
                         "any other running engine is still found by its process and port")
    ap.add_argument("--api-key", default=os.environ.get("BLINKD_API_KEY"), help="engine API key, if one is set")
    ap.add_argument("--serial", help="serial device of the ESP32, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--udp", help="panel on Wi-Fi: blinkpanel.local or its IP address")
    ap.add_argument("--udp-port", type=int, default=4210)
    ap.add_argument("--stdout", action="store_true", help="write frames to stdout (for preview)")
    ap.add_argument("--rate", type=float, default=20, help="frames per second (default %(default)s)")
    ap.add_argument("--brightness", type=float, default=0, metavar="PERCENT",
                    help="overall panel brightness, 1-100 %% (default: the panel's own setting)")
    ap.add_argument("--max-tps", type=float, default=FULL_TPS,
                    help="generation speed that drives the panel flat out (default %(default)s)")
    ap.add_argument("--simulate", action="store_true", help="fake activity instead of reading this machine")
    ap.add_argument("--no-pcie", action="store_true", help="skip AMD pcie_bw sampling")
    ap.add_argument("--dump", action="store_true", help="print the signals once a second")
    args = ap.parse_args()

    sinks: list = []
    if args.serial:
        sinks.append(SerialSink(args.serial, args.baud))
    if args.udp:
        sinks.append(UdpSink(args.udp, args.udp_port))
    if args.stdout:
        sinks.append(StdoutSink())
    if not sinks and not args.dump:
        ap.error("give at least one of --serial, --udp, --stdout or --dump")

    brightness = 0 if args.brightness <= 0 else max(1, min(255, round(args.brightness * 2.55)))
    sim = Simulator() if args.simulate else None
    if not sim:
        globals()["FULL_TPS"] = args.max_tps
        poller = EnginePoller(args.url, args.api_key)
        host, gpus_src, proc = ProcHost(), AmdGpus(pcie=not args.no_pcie), EngineProcess(poller.port)
        poller.start()
        print(f"blinkd: {len(gpus_src.cards)} AMD GPU(s), {len(host.disks)} disk(s), watching {', '.join(args.url) if args.url else 'for llama.cpp or strata'}",
              file=sys.stderr)

    period = 1.0 / args.rate
    seq = 0
    last = time.monotonic()
    next_dump = last + 1
    while True:
        now = time.monotonic()
        dt, last = now - last, now
        if sim:
            engine, state, levels, cores, gpus, threads, counters = sim.sample(dt)
        else:
            levels = {}
            cores = host.sample(dt, levels)
            gpus = gpus_src.sample(levels)
            if proc.port != poller.port:  # the engine moved (or was found on another port)
                proc.port, proc.pid = poller.port, None
            threads = proc.sample(dt, levels)
            engine, state, elv, counters = poller.snapshot()
            pcie_strata = elv.pop("pcie_strata", None)
            if pcie_strata is not None and not gpus_src.pcie_mbs:
                levels["pcie"] = pcie_strata
            levels.update(elv)
            counters["disk_bursts"] += host.disk_bursts
        frame = pack_frame(seq, engine, state, levels, cores, gpus, threads, counters, brightness)
        for s in sinks:
            s.send(frame)
        seq += 1
        if args.dump and now >= next_dump:
            next_dump = now + 1
            names = {0: "none", 1: "llama.cpp", 2: "strata", 3: "Maya"}
            states = {0: "down", 1: "idle", 2: "reading", 3: "generating"}
            shown = " ".join(f"{n}={levels[n]:.2f}" if n not in RAW_COUNTS else f"{n}={levels[n]}"
                             for n in LEVELS if n in levels)
            print(f"[{names[engine]} {states[state]}] {shown} counters={counters}", file=sys.stderr)
        time.sleep(max(0.0, period - (time.monotonic() - now)))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
