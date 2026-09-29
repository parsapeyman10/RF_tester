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
    """خط دقیقاً با همان فرمت واحد فریمور (formatRecordLine)"""
    now = datetime.datetime.now() - datetime.timedelta(minutes=2 * (200 - rid))
    temp = 22.0 + random.uniform(-1.5, 3.5)
    hum = 45.0 + random.uniform(-5, 12)
    n1 = "OK" if random.random() > 0.15 else "NOK"
    n2 = "OK" if random.random() > 0.2 else "NOK"
    return (
        "NUM=%d,BCM1_OPEN=%s,BCM1_CLOSE=%s,BCM2_OPEN=OK,BCM2_CLOSE=NOK,"
        "Temp=%.2f,Humidity=%.2f,Date=%s,Time=%s"
        % (rid, n1, n2, temp, hum,
           now.strftime("%Y-%m-%d"), now.strftime("%H:%M:%S"))
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
                self.wfile.write(b"END\r\n")
            elif cmd in ("sync10", "syncall"):
                count = 10 if cmd == "sync10" else 25
                with _LOCK:
                    start = _NEXT_ID[0]
                    _NEXT_ID[0] += count
                for i in range(count):
                    self.wfile.write((_make_record(start + i) + "\r\n").encode())
                self.wfile.write(b"END\r\n")
            elif cmd == "info":
                self.wfile.write(b"DEVICE=RF_TESTER,FW=SIM,HEAP=999999\r\nEND\r\n")
            elif cmd in ("empty", "nodata"):
                self.wfile.write(b"NO_DATA\r\nEND\r\n")
            else:
                self.wfile.write(b"ERR:CMD\r\nEND\r\n")
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
