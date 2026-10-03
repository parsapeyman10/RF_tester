#!/usr/bin/env bash
# راه‌اندازی سرور RF_tester روی لینوکس/مک
set -e
cd "$(dirname "$0")"

if [ ! -x ".venv/bin/python" ]; then
  echo "[1/3] ساخت محیط مجازی..."
  python3 -m venv .venv
fi

echo "[2/3] نصب/بررسی پیش‌نیازها..."
.venv/bin/python -m pip install --upgrade pip >/dev/null
.venv/bin/python -m pip install -r requirements.txt

echo "[3/3] اجرای سرور..."
echo "  محلی : http://127.0.0.1:5000"
echo "  شبکه : http://$(hostname -I 2>/dev/null | awk '{print $1}'):5000"
exec .venv/bin/python app.py "$@"
