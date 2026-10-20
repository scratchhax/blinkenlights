#!/usr/bin/env python3
"""Checks blinkd against fake llama.cpp and strata servers and the real /proc of this machine.

    python3 host/test_blinkd.py
"""
import json
import os
import socket
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blinkd  # noqa: E402


class FakeEngine(BaseHTTPRequestHandler):
    """llama.cpp, a llama-server router (one model loaded) or strata, generating 40 tokens a
    second in 2 second requests."""
    kind = "llama"
    t0 = time.monotonic()

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        t = time.monotonic() - self.t0
        cycle, ct = int(t // 2.5), t % 2.5
        busy = ct < 2.0
        reading = ct < 0.3
        decoded = 0 if reading or not busy else int((ct - 0.3) * 40)
        path = self.path
        if self.kind == "router":
            # A router needs ?model= on per-model endpoints and lists its models on /models.
            base, _, query = path.partition("?")
            if base == "/models":
                return self._send(200, {"data": [
                    {"id": "Other", "status": {"value": "unloaded"}},
                    {"id": "Big Model", "status": {"value": "loaded"}}]})
            if base in ("/slots", "/metrics"):
                if query != "model=Big%20Model":
                    return self._send(400, {"error": {"code": 400, "message": "model name is missing from the request"}})
                path = base
        if self.kind in ("llama", "router"):
            if path == "/health":
                return self._send(200, {"status": "ok"})
            if path == "/slots":
                return self._send(200, [{"id": 0, "n_ctx": 4096, "is_processing": busy,
                                         "next_token": {"n_decoded": decoded}},
                                        {"id": 1, "n_ctx": 4096, "is_processing": False,
                                         "next_token": {"n_decoded": 0}}])
            if path == "/metrics":
                return self._send(200, "llamacpp:requests_deferred 2\n"
                                       f"llamacpp:spec_decode_num_accepted_tokens_total {int(t * 20)}\n"
                                       f"llamacpp:spec_decode_num_draft_tokens_total {int(t * 30)}\n",
                                  "text/plain")
        else:
            if self.path == "/health":
                # Maya forks strata's server: it serves strata's /metrics but does not tag itself
                # in /health, so blinkd has to pick it out by the version key and no service.
                body = {"status": "ok", "service": "strata", "loaded": True} if self.kind == "strata" \
                    else {"status": "ok", "version": "1.1.18", "loaded": True}
                return self._send(200, body)
            if self.path == "/metrics":
                state = "reading" if busy and reading else "generating" if busy else "idle"
                return self._send(200, {
                    "engine": {"max_context": 8192},
                    "live": {"state": state, "queued": 1, "generated": decoded if busy else None,
                             "prompt_read": int(ct / 0.3 * 2000) if reading else None,
                             "prompt_total": 2000 if busy else None, "prompt_tokens": 2000 if busy else None,
                             "tok_s": 40.0 if state == "generating" else None},
                    "requests": [{"time": cycle, "hit_rate": 0.8, "file_mb": 640}],
                    "totals": {"requests": cycle, "drafts_accepted": int(t * 20), "drafts_offered": int(t * 30)},
                    "hardware": {"pcie_rx_mb": 100.0, "pcie_tx_mb": 20.0},
                })
        self._send(404, {"error": "not found"})


def serve(kind):
    handler = type(f"Fake{kind}", (FakeEngine,), {"kind": kind, "t0": time.monotonic()})
    srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class EngineTests(unittest.TestCase):
    def run_poller(self, kind, seconds=5.5):
        srv = serve(kind)
        try:
            p = blinkd.EnginePoller(f"http://127.0.0.1:{srv.server_address[1]}", None)
            p.start()
            states = set()
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                states.add(p.snapshot()[1])
                time.sleep(0.05)
            return p.snapshot(), states
        finally:
            srv.shutdown()
            srv.server_close()

    def test_llama(self):
        (engine, _, levels, counts), states = self.run_poller("llama")
        self.assertEqual(engine, blinkd.ENGINE_LLAMA)
        self.assertIn(blinkd.STATE_GENERATING, states)
        self.assertIn(blinkd.STATE_READING, states)
        self.assertGreater(counts["tokens"], 100)
        self.assertGreaterEqual(counts["req_start"], 2)
        self.assertGreaterEqual(counts["req_end"], 1)
        self.assertGreater(counts["drafts_accepted"], 0)
        self.assertGreater(counts["prompt8"], 0)
        self.assertEqual(levels["queued"], 2)

    def test_llama_router(self):
        (engine, _, levels, counts), states = self.run_poller("router")
        self.assertEqual(engine, blinkd.ENGINE_LLAMA)
        self.assertIn(blinkd.STATE_GENERATING, states)
        self.assertGreater(counts["tokens"], 100)
        self.assertEqual(levels["queued"], 2)

    def test_follows_busy_engine(self):
        # strata idle on one port, llama.cpp working on another (a model router in front of both):
        # the poller should end up watching llama.cpp.
        class IdleStrata(BaseHTTPRequestHandler):
            def do_GET(self):
                body = {"/health": {"service": "strata", "loaded": True},
                        "/metrics": {"live": {"state": "idle"}}}.get(self.path)
                data = json.dumps(body).encode()
                self.send_response(200 if body else 404)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        strata = ThreadingHTTPServer(("127.0.0.1", 0), IdleStrata)
        threading.Thread(target=strata.serve_forever, daemon=True).start()
        llama = serve("router")
        try:
            p = blinkd.EnginePoller([f"http://127.0.0.1:{strata.server_address[1]}",
                                     f"http://127.0.0.1:{llama.server_address[1]}"], None)
            p.start()
            end = time.monotonic() + 6
            while time.monotonic() < end and p.snapshot()[0] != blinkd.ENGINE_LLAMA:
                time.sleep(0.1)
            self.assertEqual(p.snapshot()[0], blinkd.ENGINE_LLAMA)
        finally:
            for srv in (strata, llama):
                srv.shutdown()
                srv.server_close()

    def test_strata(self):
        (engine, _, levels, counts), states = self.run_poller("strata")
        self.assertEqual(engine, blinkd.ENGINE_STRATA)
        self.assertIn(blinkd.STATE_GENERATING, states)
        self.assertGreater(counts["tokens"], 100)
        self.assertGreater(counts["prompt8"], 100)
        self.assertGreaterEqual(counts["req_end"], 1)
        self.assertGreater(counts["drafts_accepted"], 0)
        self.assertGreater(counts["disk_bursts"], 0)
        self.assertAlmostEqual(levels["expert_hit"], 0.8)
        self.assertEqual(levels["queued"], 1)

    def test_maya(self):
        # Maya is a fork of strata's server: same /metrics, different /health (a version key and
        # no service tag). The poller has to spot the engine difference but then read the metrics
        # exactly as it does strata.
        (engine, _, levels, counts), states = self.run_poller("maya")
        self.assertEqual(engine, blinkd.ENGINE_MAYA)
        self.assertIn(blinkd.STATE_GENERATING, states)
        self.assertGreater(counts["tokens"], 100)
        self.assertGreater(counts["prompt8"], 100)
        self.assertGreaterEqual(counts["req_end"], 1)
        self.assertGreater(counts["drafts_accepted"], 0)
        self.assertAlmostEqual(levels["expert_hit"], 0.8)
        self.assertEqual(levels["queued"], 1)

    def test_nothing_listening(self):
        p = blinkd.EnginePoller("http://127.0.0.1:9", None)
        p.start()
        time.sleep(1.5)
        self.assertEqual(p.snapshot()[:2], (blinkd.ENGINE_NONE, blinkd.STATE_DOWN))


class DiscoveryTests(unittest.TestCase):
    def test_finds_engine_by_process_name_on_any_port(self):
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "llama-server.py")  # the name discovery looks for
            with open(script, "w") as f:
                f.write("import sys, time\n"
                        f"sys.path.insert(0, {os.path.dirname(os.path.abspath(__file__))!r})\n"
                        "import test_blinkd\n"
                        "test_blinkd.serve('llama')\n"
                        "time.sleep(30)\n")
            child = subprocess.Popen([sys.executable, "-I", script])
            try:
                for _ in range(50):
                    urls = blinkd.discover_engines()
                    if urls:
                        break
                    time.sleep(0.1)
                self.assertEqual(len(urls), 1, urls)
                p = blinkd.EnginePoller(None, None)
                self.assertEqual(p._find()[0], blinkd.ENGINE_LLAMA)
                self.assertEqual(p.url, urls[0])
                self.assertNotEqual(p.port, 8080)
            finally:
                child.kill()
                child.wait()


class HostTests(unittest.TestCase):
    def test_proc_sources(self):
        host, gpus = blinkd.ProcHost(), blinkd.AmdGpus(pcie=False)
        levels = {}
        for _ in range(3):
            cores = host.sample(0.05, levels)
            gpus.sample(levels)
            time.sleep(0.05)
        self.assertEqual(len(cores), blinkd.N_CORES)
        for k in ("swap", "paging", "mem", "disk_read", "net_rx", "ctx_switch", "interrupts", "loadavg"):
            self.assertIn(k, levels)
            self.assertTrue(0 <= levels[k] <= 1, (k, levels[k]))

    def test_engine_process_found_by_port(self):
        srv = serve("llama")
        try:
            proc = blinkd.EngineProcess(srv.server_address[1])
            levels = {}
            proc.sample(0.1, levels)
            self.assertEqual(proc.pid, os.getpid())
            time.sleep(0.1)
            threads = proc.sample(0.1, levels)
            self.assertEqual(len(threads), blinkd.N_THREADS)
        finally:
            srv.shutdown()
            srv.server_close()


class UdpTests(unittest.TestCase):
    def test_sends_frames_to_a_hostname(self):
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.bind(("127.0.0.1", 0))
        rx.settimeout(2)
        try:
            sink = blinkd.UdpSink("localhost", rx.getsockname()[1])
            frame = blinkd.pack_frame(1, 0, 1, {}, [], [], [], {})
            sink.send(frame)
            self.assertEqual(rx.recv(256), frame)
        finally:
            rx.close()
            sink.sock.close()


class FrameTests(unittest.TestCase):
    def test_layout_and_crc(self):
        f = blinkd.pack_frame(7, 1, 3, {"gpu_busy": 1.0, "running": 3}, [0.5] * 16, [1.0], [0.0] * 14,
                              {"tokens": 70000})
        self.assertEqual(len(f), 92)
        self.assertEqual(f[:3], b"BL\x01")
        self.assertEqual(f[8], 255)                                   # gpu_busy
        self.assertEqual(f[8 + blinkd.LEVELS.index("running")], 3)    # raw count
        self.assertEqual(int.from_bytes(f[78:80], "little"), 70000 & 0xFFFF)
        self.assertEqual(int.from_bytes(f[90:92], "little"), blinkd.crc16(f[:90]))
        self.assertEqual(blinkd.crc16(b"123456789"), 0x29B1)          # CRC-16/CCITT-FALSE check value
        self.assertEqual(f[7], 0)                                     # brightness: panel decides
        self.assertEqual(blinkd.pack_frame(0, 0, 0, {}, [], [], [], {}, brightness=128)[7], 128)


if __name__ == "__main__":
    unittest.main(verbosity=2)
