# -*- coding: utf-8 -*-
"""
شبیه‌ساز ESP32 در حالت هات‌اسپات (برای تست کلاینت‌ها بدون سخت‌افزار).

دقیقاً همان رفتار TaskInternalWiFiConnection / MODE_HOTSPOT_VIEW را بازی می‌کند:
دستور متنی خط‌به‌خط می‌گیرد، JSON برمی‌گرداند و سوکت را باز نگه می‌دارد.

    python3 desktop/simulator.py --port 9080
"""

from __future__ import annotations

import argparse
import datetime
import random
import socketserver
import threading

_LOCK = threading.Lock()
_NEXT_ID = [101]


def _make_record(rid: int) -> str:
    now = datetime.datetime.now() - datetime.timedelta(minutes=2 * (200 - rid))
    temp = 22.0 + random.uniform(-1.5, 3.5)
    hum = 45.0 + random.uniform(-5, 12)
    n1 = 1 if random.random() > 0.15 else 0
    n2 = 1 if random.random() > 0.2 else 0
    return (
        '{"ID":%d,"T":%.2f,"H":%.2f,"N1":%d,"N2":%d,"Time":"%s"}'
        % (rid, temp, hum, n1, n2, now.strftime("%Y-%m-%d %H:%M:%S"))
    )


class _Handler(socketserver.StreamRequestHandler):
    timeout = 30

    def handle(self):
        while True:
            line = self.rfile.readline()
            if not line:
                return
            cmd = line.decode("utf-8", "ignore").strip().lower()
            if cmd == "sync":
                with _LOCK:
                    rid = _NEXT_ID[0]
                    _NEXT_ID[0] += 1
                self.wfile.write((_make_record(rid) + "\r\n").encode())
            elif cmd == "sync10":
                with _LOCK:
                    start = _NEXT_ID[0]
                    _NEXT_ID[0] += 10
                self.wfile.write(b"[\r\n")
                for i in range(10):
                    sep = b",\r\n" if i < 9 else b"\r\n"
                    self.wfile.write(_make_record(start + i).encode() + sep)
                self.wfile.write(b"]\r\n")
            elif cmd in ("empty", "nodata"):
                self.wfile.write(b"NO_DATA\r\n")
            else:
                self.wfile.write(b"ERR:CMD\r\n")
            self.wfile.flush()


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start_background(host: str = "127.0.0.1", port: int = 9080) -> _Server:
    srv = _Server((host, port), _Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="ESP32 hotspot simulator")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9080)
    args = ap.parse_args()
    print(f"[SIM] ESP32 simulator listening on {args.host}:{args.port}")
    _Server((args.host, args.port), _Handler).serve_forever()
