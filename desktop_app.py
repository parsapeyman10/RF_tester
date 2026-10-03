# -*- coding: utf-8 -*-
"""
ربات دسکتاپ RF_tester — سرور + پنجره‌ی اپلیکیشن دسکتاپ در یک کلیک

با اجرای این فایل:
  1. همان سرور Flask (app.py) در پس‌زمینه بالا می‌آید (با waitress، پایدار)
     — شامل بازیابی رکوردهای نجات‌یافته و کارگرِ پورت سریال.
  2. یک پنجره‌ی دسکتاپِ مستقل باز می‌شود که همان داشبورد را نشان می‌دهد؛
     بدون مرورگر، بدون تایپ آدرس، بدون تب — مثل یک برنامه‌ی ویندوزی.
  3. با بستن پنجره، سرور هم به‌صورت تمیز خاموش می‌شود.

اجرا:
    python desktop_app.py             # پنجره‌ی دسکتاپ (نیاز به pywebview)
    python desktop_app.py --headless  # فقط سرور، بدون پنجره (برای تست/دیباگ)
    python desktop_app.py --port 5050 # تغییر پورت

نصب پیش‌نیاز پنجره:
    pip install pywebview

راه‌های یک‌کلیکی:
    run_desktop.bat     ← دبل‌کلیک روی ویندوز (اپ دسکتاپ)
    build_desktop.bat   ← ساخت فایل مستقل RF_Tester_HMI.exe بدون نیاز به پایتون

نکته: اگر سرور از قبل روی پورت روشن باشد، فقط پنجره به همان سرور
وصل می‌شود و چیزی دوباره اجرا نمی‌شود.
"""

import argparse
import os
import socket
import sys
import threading
import time
import webbrowser

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)

HOST = "127.0.0.1"
DEFAULT_PORT = 5000
TITLE = "HMI مدیریت گیرنده صنعتی — RF_tester"


def port_open(port, host=HOST):
    """آیا روی این پورت یک سرور در حال گوش دادن است؟"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.4)
            return s.connect_ex((host, port)) == 0
    except OSError:
        return False


def wait_for_port(port, timeout=90):
    """صبر می‌کند تا سروری که داخل thread اجرا شده آماده شود"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_open(port):
            return True
        time.sleep(0.25)
    return False


def run_server(port):
    """سرور واقعی (app.py) را اجرا می‌کند — قرار است داخل یک thread باشد"""
    import app as server

    # همان راه‌اندازیِ «python app.py»: جدول‌ها، بازیابی رکوردهای
    # نجات‌یافته، تنظیمات سریال و کارگرِ خواندن سریال
    server.bootstrap_server(recover_unsaved=True, start_serial=True)

    print(f"[DESKTOP] سرور روی http://0.0.0.0:{port} بالا آمد")
    try:
        from waitress import serve
        serve(server.app, host="0.0.0.0", port=port, threads=8)
    except ImportError:
        print("[DESKTOP] waitress نصب نیست؛ با سرور توسعه ادامه می‌دهیم")
        server.app.run(host="0.0.0.0", port=port, debug=False, use_reloader=False)


def open_desktop_window(url):
    """پنجره‌ی دسکتاپ را باز می‌کند؛ اگر pywebview نبود، مرورگر جایگزین می‌شود"""
    try:
        import webview  # pywebview
    except ImportError:
        print("[DESKTOP] pywebview نصب نیست — با این دستور نصب کنید:")
        print("           pip install pywebview")
        print(f"[DESKTOP] فعلاً مرورگر پیش‌فرض باز می‌شود: {url}")
        webbrowser.open(url)
        # صبر می‌کنیم تا کاربر کارش با سرور تمام شود
        try:
            while True:
                time.sleep(60)
        except KeyboardInterrupt:
            pass
        return

    webview.create_window(
        TITLE, url,
        width=1400, height=900, min_size=(1000, 640),
        background_color="#0f172a")
    webview.start()


def main():
    parser = argparse.ArgumentParser(description="اپ دسکتاپ RF_tester")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="پورت سرور (پیش‌فرض 5000)")
    parser.add_argument("--headless", action="store_true",
                        help="فقط سرور، بدون پنجره‌ی دسکتاپ")
    args = parser.parse_args()
    url = f"http://{HOST}:{args.port}/"

    if port_open(args.port):
        print(f"[DESKTOP] سرور از قبل روی پورت {args.port} روشن است؛ "
              f"پنجره به همان وصل می‌شود")
    else:
        print(f"[DESKTOP] در حال بالا آوردن سرور روی پورت {args.port} ...")
        threading.Thread(target=run_server, args=(args.port,), daemon=True).start()
        if not wait_for_port(args.port):
            print("[DESKTOP] سرور در ۹۰ ثانیه بالا نیامد؛ خروجی بالا را بررسی کنید")
            sys.exit(1)

    if args.headless:
        print(f"[DESKTOP] حالت بدون پنجره — {url}  (Ctrl+C برای توقف)")
        try:
            while True:
                time.sleep(60)
        except KeyboardInterrupt:
            print("\n[DESKTOP] توقف")
        return

    open_desktop_window(url)
    # پنجره بسته شد → thread سرور هم (daemon) همراه پروسه خاموش می‌شود
    print("[DESKTOP] پنجره بسته شد؛ سرور هم خاموش می‌شود.")


if __name__ == "__main__":
    main()
