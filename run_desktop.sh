#!/usr/bin/env bash
# اجرای سرور RF_tester به‌صورت اپ دسکتاپ (پنجره‌ی pywebview)
set -e
cd "$(dirname "$0")"

if [ ! -x ".venv/bin/python" ]; then
  echo "[1/3] ساخت محیط مجازی..."
  python3 -m venv .venv
fi

echo "[2/3] نصب/بررسی پیش‌نیازها..."
.venv/bin/python -m pip install --upgrade pip >/dev/null
.venv/bin/python -m pip install -r requirements.txt pywebview

echo "[3/3] اجرای اپ دسکتاپ..."
exec .venv/bin/python desktop_app.py "$@"
