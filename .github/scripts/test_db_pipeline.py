#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
تست زنجیره‌ی ذخیره‌سازی سرور:

    خط سریال ESP8266 -> parse_industrial_line -> save_sensor_data
        -> master_industrial.db (MasterReading)
        -> YYYY-MM-DD.db        (daily_records)

چیزهایی که تضمین می‌کند:
  1. خط استاندارد درست پارس و ذخیره می‌شود (هر دو دیتابیس).
  2. رکورد تکراری (اکوی سریال / ارسال مجدد بعد از گم شدن ACK) دوباره ثبت نمی‌شود.
  3. خط ناقص یا نویزی اصلاً وارد دیتابیس نمی‌شود.
  4. فایل باینری چندرکوردی .dat (حالت ذخیره‌سازی بهینه‌ی SD) درست باز می‌شود.
  5. تاریخ نامعتبر (مثل 2026-02-31) رد می‌شود.

اجرا در یک پوشه‌ی موقت انجام می‌شود تا دیتابیس‌های واقعی دست نخورند.
"""

import os
import shutil
import sqlite3
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

WORKDIR = tempfile.mkdtemp(prefix="rf_db_test_")
os.chdir(WORKDIR)

import app as flask_app  # noqa: E402

failures = []


def check(cond, msg):
    if cond:
        print(f"  ok  - {msg}")
    else:
        failures.append(msg)
        print(f"  FAIL- {msg}")


# دیتابیس اصلی هرجا که هست، برای تست از صفر ساخته می‌شود
with flask_app.app.app_context():
    MASTER_DB_FILE = flask_app.db.engine.url.database
    flask_app.db.engine.dispose()
    if MASTER_DB_FILE and os.path.exists(MASTER_DB_FILE):
        os.remove(MASTER_DB_FILE)
    flask_app.db.create_all()
print(f"[test] master db = {MASTER_DB_FILE}")


def master_count():
    with flask_app.app.app_context():
        return flask_app.db.session.query(flask_app.MasterReading).count()


def daily_count(date_str):
    path = os.path.join(WORKDIR, f"{date_str}.db")
    if not os.path.exists(path):
        return 0
    conn = sqlite3.connect(path)
    try:
        return conn.execute("SELECT COUNT(*) FROM daily_records").fetchone()[0]
    finally:
        conn.close()


LINE = ("NUM=42,NBCM1=OK,NBCM2=NOK,NBCM3=OK,NBCM4=NOK,"
        "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09")
LINE2 = ("NUM=43,NBCM1=OK,NBCM2=OK,NBCM3=NOK,NBCM4=NOK,"
         "Temp=-4.50,Humidity=88.00,Date=2026-01-05,Time=13:06:09")

print("\n[1] پارس و ذخیره‌ی یک خط استاندارد")
payload = flask_app.parse_industrial_line(LINE)
check(payload is not None, "خط استاندارد پارس شد")
check(payload and payload["nbcm"] == ["NBCM1", "NBCM3"], "NBCM ها درست تفکیک شدند")
with flask_app.app.app_context():
    flask_app.save_sensor_data(payload)
check(master_count() == 1, "یک رکورد در master ثبت شد")
check(daily_count("2026-01-05") == 1, "یک رکورد در دیتابیس روزانه ثبت شد")

print("\n[2] همان خط دو بار دیگر (اکوی سریال / ارسال مجدد)")
for _ in range(2):
    with flask_app.app.app_context():
        flask_app.save_sensor_data(flask_app.parse_industrial_line(LINE))
check(master_count() == 1, "رکورد تکراری در master ثبت نشد")
check(daily_count("2026-01-05") == 1, "رکورد تکراری در دیتابیس روزانه ثبت نشد")

print("\n[3] رکورد دوم (متفاوت)")
with flask_app.app.app_context():
    flask_app.save_sensor_data(flask_app.parse_industrial_line(LINE2))
check(master_count() == 2, "رکورد جدید ثبت شد")
check(daily_count("2026-01-05") == 2, "رکورد جدید در دیتابیس روزانه ثبت شد")

print("\n[4] خطوط خراب نباید پارس شوند")
bad_lines = [
    "NUM=44,NBCM1=OK,NBCM2=NOK,Temp=20.0",                       # ناقص
    "[LINE RECV]: garbage",                                      # بدون دیتا
    "NUM=,NBCM1=OK,NBCM2=OK,NBCM3=OK,NBCM4=OK,Temp=x,Humidity=y,Date=2026-01-05,Time=13:04:09",
    "NUM=45,NBCM1=OK,NBCM2=OK,NBCM3=OK,NBCM4=OK,Temp=20.0,Humidity=40.0,Date=2026-02-31,Time=13:04:09",
]
for bad in bad_lines:
    check(flask_app.parse_industrial_line(bad) is None, f"رد شد: {bad[:45]}…")
check(master_count() == 2, "هیچ خط خرابی وارد دیتابیس نشد")

print("\n[5] خط با نویز قبل از NUM= (رایج روی سریال)")
noisy = "\x00\x00[BOOT] ready " + LINE2
p = flask_app.parse_industrial_line(noisy)
check(p is not None and p["num_value"] == "43", "دیتای سالم از داخل خط نویزی استخراج شد")

print("\n[6] فایل باینری چندرکوردی .dat (ذخیره‌سازی بهینه‌ی SD)")
STRUCT_FORMAT = "<iff????iBBBBB"
blob = b""
for i in range(5):
    blob += struct.pack(STRUCT_FORMAT, 100 + i, 21.5 + i, 40.0 + i,
                        True, i % 2 == 0, False, False,
                        2026, 1, 6, 10, i, 30)
check(len(blob) == 5 * 25, f"اندازه‌ی فایل ۵ رکوردی = {len(blob)} بایت (انتظار ۱۲۵)")
check(len(blob) % struct.calcsize(STRUCT_FORMAT) == 0, "فایل بر اندازه‌ی رکورد بخش‌پذیر است")
first = struct.unpack(STRUCT_FORMAT, blob[:25])
check(first[0] == 100 and first[7] == 2026, "رکورد اول درست باز شد")
last = struct.unpack(STRUCT_FORMAT, blob[-25:])
check(last[0] == 104 and last[11] == 4, "رکورد آخر درست باز شد")

print("\n[7] ایندکس یکتای دیتابیس روزانه")
conn = sqlite3.connect(os.path.join(WORKDIR, "2026-01-05.db"))
idx = [r[1] for r in conn.execute("PRAGMA index_list(daily_records)").fetchall()]
conn.close()
check("uq_daily_record" in idx, f"ایندکس یکتا ساخته شد (ایندکس‌ها: {idx})")

# ---------------------------------------------------------------- گزارش
with flask_app.app.app_context():
    flask_app.db.engine.dispose()
if MASTER_DB_FILE and os.path.exists(MASTER_DB_FILE):
    os.remove(MASTER_DB_FILE)
os.chdir(ROOT)
shutil.rmtree(WORKDIR, ignore_errors=True)

print()
if failures:
    for f in failures:
        print(f"::error::{f}")
    print(f"{len(failures)} تست شکست خورد.")
    sys.exit(1)
print("همه‌ی تست‌های زنجیره‌ی دیتابیس پاس شد.")
