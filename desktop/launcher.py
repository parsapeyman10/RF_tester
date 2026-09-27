# -*- coding: utf-8 -*-
"""
نقطه‌ی ورود بسته‌ی تک‌فایلی RFTester.pyz

    python RFTester.pyz          -> اپ بومی Tkinter (اگر موجود باشد) وگرنه نسخه‌ی مرورگری
    python RFTester.pyz --web    -> مستقیم نسخه‌ی مرورگری
    python RFTester.pyz --simulate
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main():
    args = sys.argv[1:]
    want_web = "--web" in args
    if want_web:
        args.remove("--web")
        sys.argv = [sys.argv[0]] + args
    else:
        try:
            import tkinter  # noqa: F401
        except Exception:
            print("[RF_tester] tkinter در دسترس نیست، نسخه‌ی مرورگری اجرا می‌شود.")
            want_web = True

    if want_web:
        import server
        server.main()
    else:
        import rf_tester_gui
        rf_tester_gui.main()


if __name__ == "__main__":
    main()
