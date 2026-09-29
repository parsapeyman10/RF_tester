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

# دیتابیس‌های روزانه هم در پوشه‌ی موقت ساخته شوند
flask_app.DAILY_DB_DIR = WORKDIR

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


LINE = ("NUM=42,BCM1_OPEN=OK,BCM1_CLOSE=NOK,BCM2_OPEN=OK,BCM2_CLOSE=NOK,"
        "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09")
LINE2 = ("NUM=43,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=NOK,BCM2_CLOSE=NOK,"
         "Temp=-4.50,Humidity=88.00,Date=2026-01-05,Time=13:06:09")

print("\n[1] پارس و ذخیره‌ی یک خط استاندارد")
payload = flask_app.parse_industrial_line(LINE)
check(payload is not None, "خط استاندارد پارس شد")
check(payload and payload["nbcm"] == ["BCM1_OPEN", "BCM2_OPEN"],
      f"چهار نتیجه‌ی تفکیکی پارس شد ({payload['nbcm'] if payload else None})")
_res = flask_app.build_bcm_results(",".join(payload["nbcm"])) if payload else {}
check(_res.get("BCM1") == {"open": True, "close": False, "ok": False},
      f"BCM1: باز شد ولی بسته نشد -> ok=False ({_res.get('BCM1')})")
check(_res.get("BCM2") == {"open": True, "close": False, "ok": False},
      f"BCM2: باز شد ولی بسته نشد ({_res.get('BCM2')})")
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
    "NUM=44,BCM1_OPEN=OK,BCM1_CLOSE=NOK,Temp=20.0",              # ناقص
    "[LINE RECV]: garbage",                                      # بدون دیتا
    "NUM=,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=OK,Temp=x,Humidity=y,Date=2026-01-05,Time=13:04:09",
    "NUM=45,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=OK,Temp=20.0,Humidity=40.0,Date=2026-02-31,Time=13:04:09",
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

print("\n[7b] دو دستگاه، هرکدام دو نتیجه (باز / بسته)")
check(len(flask_app.DEVICES) == 2, "دو دستگاه تعریف شده است")
full = flask_app.build_bcm_results("BCM1_OPEN,BCM1_CLOSE,BCM2_OPEN,BCM2_CLOSE")
check(full["BCM1"] == {"open": True, "close": True, "ok": True}, "BCM1 کامل OK")
check(full["BCM2"] == {"open": True, "close": True, "ok": True}, "BCM2 کامل OK")
half = flask_app.build_bcm_results("BCM1_OPEN,BCM2_CLOSE")
check(half["BCM1"]["open"] and not half["BCM1"]["close"] and not half["BCM1"]["ok"],
      "BCM1 فقط باز شد -> ناقص")
check(half["BCM2"]["close"] and not half["BCM2"]["open"] and not half["BCM2"]["ok"],
      "BCM2 فقط بسته شد -> ناقص")
none = flask_app.build_bcm_results("")
check(none["BCM1"] == {"open": False, "close": False, "ok": False}, "بدون فیدبک همه NOK")

print("\n[7c] مرجع زمان، RTC دستگاه است (نه ساعت سرور)")
_blob = struct.pack("<iff????iBBBBB", 777, 22.00, 43.00,
                    True, True, True, False, 2026, 3, 9, 7, 45, 12)
import io as _io
_resp = _c_dat = flask_app.app.test_client().post(
    "/upload_dat",
    data={"folder_upload": (_io.BytesIO(_blob), "20260309.dat")},
    content_type="multipart/form-data")
check(_resp.status_code == 200, f"/upload_dat -> {_resp.status_code}")
with flask_app.app.app_context():
    rec = flask_app.db.session.query(flask_app.MasterReading).filter_by(num_value=777).first()
check(rec is not None, "رکورد آپلودشده ثبت شد")
if rec:
    check(rec.date == "2026-03-09" and rec.time == "07:45:12",
          f"تاریخ و ساعت از RTC خوانده شد ({rec.date} {rec.time})")
    check(rec.timestamp.strftime("%Y-%m-%d %H:%M:%S") == "2026-03-09 07:45:12",
          f"timestamp هم زمان RTC است نه زمان آپلود سرور ({rec.timestamp})")
    _r = flask_app.build_bcm_results(rec.nbcm_selected)
    check(_r["BCM1"]["ok"] and _r["BCM2"]["open"] and not _r["BCM2"]["close"],
          f"چهار نتیجه از فایل باینری درست خوانده شد ({rec.nbcm_selected})")

print("\n[7d] هیچ رکوردی گم نمی‌شود")
flask_app.stash_unsaved(LINE2.replace("NUM=43", "NUM=4300"), "تست دستی")
check(os.path.exists(flask_app.UNSAVED_LOG), "فایل پشتیبان ساخته شد")
with flask_app.app.test_request_context():
    _rec_resp = flask_app.api_recover_unsaved()
_j = _rec_resp.get_json() if hasattr(_rec_resp, "get_json") else {}
check(_j.get("recovered") == 1, f"رکورد نجات‌یافته برگشت ({_j})")
with flask_app.app.app_context():
    _back = flask_app.db.session.query(flask_app.MasterReading).filter_by(num_value=4300).first()
check(_back is not None, "رکورد بازیابی‌شده در دیتابیس است")

print("\n[7e] ترتیب همیشه بر اساس ساعت ذخیره‌شده توسط دستگاه است")
_mixed = [
    ("NUM=500,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=OK,"
     "Temp=20.00,Humidity=40.00,Date=2026-05-02,Time=09:00:00"),
    ("NUM=501,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=OK,"
     "Temp=20.00,Humidity=40.00,Date=2026-05-01,Time=23:30:00"),
    ("NUM=502,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=OK,BCM2_CLOSE=OK,"
     "Temp=20.00,Humidity=40.00,Date=2026-05-02,Time=17:45:00"),
]
# عمداً به ترتیب نامرتب وارد می‌شوند
for ln in _mixed:
    with flask_app.app.app_context():
        flask_app.save_sensor_data(flask_app.parse_industrial_line(ln))

with flask_app.app.app_context():
    rows = (flask_app.MasterReading.query
            .filter(flask_app.MasterReading.num_value.in_([500, 501, 502]))
            .order_by(*flask_app.device_clock_order()).all())
order = [r.num_value for r in rows]
# با SORT_BY_TIME_FIRST=True ترتیب بر اساس ساعت است:
#   502 = 17:45 ، 501 = 23:30 ، 500 = 09:00  ->  [501, 502, 500]
expected = [501, 502, 500] if flask_app.SORT_BY_TIME_FIRST else [502, 500, 501]
check(order == expected,
      f"ترتیب بر اساس ساعت دستگاه (SORT_BY_TIME_FIRST={flask_app.SORT_BY_TIME_FIRST}): {order}")

_c2 = flask_app.app.test_client()
_api = _c2.get("/api/sensor_data").get_json()
_seq = [(d["time"], d["date"]) if flask_app.SORT_BY_TIME_FIRST else (d["date"], d["time"])
        for d in _api if d["num_value"] in (500, 501, 502)]
check(_seq == sorted(_seq), f"خروجی نمودار صعودی مرتب است ({_seq})")

# داشبورد آخرین رکورد را از همان کوئری می‌گیرد (رندر سمت مرورگر است)
with flask_app.app.app_context():
    _last = flask_app.MasterReading.query.order_by(*flask_app.device_clock_order()).first()
_want = ("2026-05-01", "23:30:00") if flask_app.SORT_BY_TIME_FIRST else ("2026-05-02", "17:45:00")
check(_last is not None and (_last.date, _last.time) == _want,
      f"رکورد اولِ داشبورد طبق همین ترتیب انتخاب شد ({_last.date} {_last.time})")
check(_c2.get("/").status_code == 200, "داشبورد بدون خطا رندر می‌شود")

print("\n[8] مسیر واحد دیتا: /api/ingest (همان چیزی که گوشی و دسکتاپ می‌فرستند)")
flask_app.app.config["TESTING"] = True
_c = flask_app.app.test_client()
_lines = [
    "NUM=900,BCM1_OPEN=OK,BCM1_CLOSE=OK,BCM2_OPEN=NOK,BCM2_CLOSE=NOK,Temp=25.00,Humidity=44.00,Date=2026-01-07,Time=08:00:00",
    "NUM=901,BCM1_OPEN=NOK,BCM1_CLOSE=OK,BCM2_OPEN=NOK,BCM2_CLOSE=NOK,Temp=25.50,Humidity=44.50,Date=2026-01-07,Time=08:02:00",
    "END",
    "چرند",
]
r1 = _c.post("/api/ingest", json={"lines": _lines})
check(r1.status_code == 200, f"/api/ingest -> {r1.status_code}")
j1 = r1.get_json()
check(j1["saved"] == 2, f"۲ رکورد ذخیره شد (خروجی: {j1})")
check(j1["invalid"] == 1, "خط نامعتبر شمرده شد")
check(daily_count("2026-01-07") == 2, "رکوردها در دیتابیس روزانه نشستند")

# همان دیتا دوباره (گوشی و کامپیوتر هر دو بفرستند) -> نباید تکراری ثبت شود
r2 = _c.post("/api/ingest", data="\n".join(_lines), content_type="text/plain")
j2 = r2.get_json()
check(j2["saved"] == 0 and j2["duplicates"] == 2, f"ارسال دوباره تکراری شمرده شد ({j2})")
check(daily_count("2026-01-07") == 2, "رکورد تکراری اضافه نشد")

print("\n[9] رندر شدن صفحات (جلوگیری از TemplateNotFound)")
check(os.path.isfile(os.path.join(flask_app.TEMPLATE_DIR, "index.html")),
      f"پوشه‌ی قالب‌ها پیدا شد: {flask_app.TEMPLATE_DIR}")
flask_app.app.config["TESTING"] = True
client = flask_app.app.test_client()
for route in ("/", "/history", "/plot_display", "/upload_dat",
              "/api/sensor_data", "/api/sensor_data_extended"):
    try:
        resp = client.get(route)
        check(resp.status_code == 200, f"{route} -> {resp.status_code}")
    except Exception as exc:
        check(False, f"{route} -> {type(exc).__name__}: {exc}")

print("\n[10] صفحه‌ی آرشیو روزانه واقعاً دیتا نشان می‌دهد")
resp = client.get("/history?date=2026-01-05")
body = resp.get_data(as_text=True)
check(resp.status_code == 200, f"/history?date=2026-01-05 -> {resp.status_code}")
check("خطا در بارگذاری فایل روزانه" not in body, "خطای بارگذاری فایل روزانه رخ نداد")
check("42" in body and "43" in body, "رکوردهای روزانه در صفحه دیده می‌شوند")

print("\n[11] پاک کردن آرشیو، دیتابیس روزانه را هم پاک می‌کند")
client.post("/clear_history?date=2026-01-05")
check(not os.path.exists(os.path.join(WORKDIR, "2026-01-05.db")),
      "فایل دیتابیس روزانه حذف شد")
with flask_app.app.app_context():
    left = flask_app.db.session.query(flask_app.MasterReading).filter_by(date="2026-01-05").count()
check(left == 0, f"رکوردهای آن روز از دیتابیس اصلی هم پاک شدند (باقی‌مانده: {left})")
body2 = client.get("/history?date=2026-01-05").get_data(as_text=True)
check(">42<" not in body2, "بعد از پاک کردن، رکورد قدیمی در جدول نیست")

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
