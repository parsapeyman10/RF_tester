# -*- coding: utf-8 -*-
"""
RF_tester — کلاینت دسکتاپ (نسخه‌ی مرورگری)

یک وب‌سرور کوچک فقط با کتابخانه‌ی استاندارد پایتون (بدون هیچ نصب اضافه‌ای).
UI در مرورگر باز می‌شود، ولی ارتباط TCP خام با ESP32 را خودِ سرور پایتون
انجام می‌دهد (مرورگر نمی‌تواند TCP خام بزند).

اجرا:
    python3 desktop/server.py                 # اتصال واقعی به ESP32
    python3 desktop/server.py --simulate      # با شبیه‌ساز، بدون سخت‌افزار

سپس مرورگر روی  http://127.0.0.1:8080
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esp_protocol  # noqa: E402

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
DEFAULTS = {"host": "192.168.1.1", "port": 80}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=WEB_DIR, **kw)

    def log_message(self, fmt, *args):  # لاگ کوتاه‌تر
        sys.stderr.write("[web] " + (fmt % args) + "\n")

    def _json(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/defaults"):
            return self._json(200, DEFAULTS)
        return super().do_GET()

    def do_POST(self):
        if not self.path.startswith("/api/query"):
            return self._json(404, {"error": "not found"})

        try:
            length = int(self.headers.get("Content-Length") or 0)
            req = json.loads(self.rfile.read(length) or b"{}")
        except Exception as exc:
            return self._json(400, {"error": f"bad request: {exc}"})

        host = (req.get("host") or DEFAULTS["host"]).strip()
        port = int(req.get("port") or DEFAULTS["port"])
        cmd = req.get("cmd") or esp_protocol.CMD_SYNC_LAST
        if cmd not in (esp_protocol.CMD_SYNC_LAST, esp_protocol.CMD_SYNC_10):
            return self._json(400, {"error": "دستور نامعتبر"})

        try:
            raw = esp_protocol.query(host, port, cmd)
        except Exception as exc:
            return self._json(200, {
                "ok": False,
                "error": f"{type(exc).__name__}: {exc}",
                "hint": "به هات‌اسپات ESP32 وصل هستید؟ IP و پورت درست است؟",
            })

        if not raw:
            return self._json(200, {"ok": False, "error": "پاسخی نیامد (تایم‌اوت)"})
        if esp_protocol.is_no_data(raw):
            return self._json(200, {"ok": True, "records": [], "raw": raw,
                                    "note": "دستگاه رکوردی روی SD ندارد"})

        records = [r.to_dict() for r in esp_protocol.parse_records(raw)]
        return self._json(200, {"ok": True, "records": records, "raw": raw})


def main():
    ap = argparse.ArgumentParser(description="RF_tester desktop client")
    ap.add_argument("--host", default="0.0.0.0", help="آدرس bind وب‌سرور")
    ap.add_argument("--port", type=int, default=8080, help="پورت وب‌سرور")
    ap.add_argument("--esp-host", default=DEFAULTS["host"], help="IP دستگاه ESP32")
    ap.add_argument("--esp-port", type=int, default=DEFAULTS["port"], help="پورت دستگاه")
    ap.add_argument("--simulate", action="store_true",
                    help="بالا آوردن شبیه‌ساز ESP32 و اتصال به آن")
    ap.add_argument("--no-browser", action="store_true", help="مرورگر باز نشود")
    args = ap.parse_args()

    DEFAULTS["host"] = args.esp_host
    DEFAULTS["port"] = args.esp_port

    if args.simulate:
        import simulator
        simulator.start_background("127.0.0.1", 9080)
        DEFAULTS["host"] = "127.0.0.1"
        DEFAULTS["port"] = 9080
        print("[SIM] شبیه‌ساز ESP32 روی 127.0.0.1:9080 بالا آمد")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://127.0.0.1:{args.port}"
    print(f"[RF_tester] UI آماده است: {url}")
    print(f"[RF_tester] دستگاه هدف: {DEFAULTS['host']}:{DEFAULTS['port']}")

    if not args.no_browser and os.environ.get("DISPLAY", "1"):
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[RF_tester] خاموش شد.")


if __name__ == "__main__":
    main()
