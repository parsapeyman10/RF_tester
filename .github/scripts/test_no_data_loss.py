#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
تست تضمین «هیچ داده‌ای از دست نمی‌رود»:

    هر خطی که NUM= داشته باشد یا در دیتابیس ذخیره می‌شود، یا تکراری
    شمرده می‌شود، یا در unsaved_records.log نگه داشته می‌شود — هیچ‌وقت
    بی‌ردپا دور ریخته نمی‌شود.

چیزهایی که تضمین می‌کند:
  1.  اینجست انبوه: همه‌ی خطوط سالم در هر دو دیتابیس ثبت می‌شوند.
  2.  ارسال دوباره‌ی همان خطوط: تکراری شمرده می‌شوند (نه از دست رفتن، نه دوبله).
  3.  خطِ دارای NUM= که فرمتش کامل نیست: در unsaved_records.log نجات داده می‌شود.
  4.  Temp=nan (حافظه‌ی SPI خالی روی ESP32): رکورد کلأً دور ریخته نمی‌شود.
  5.  فایل .dat با انتهای خراب: رکوردهای کاملِ داخلش نجات داده می‌شوند.
  6.  شکست ثبتِ دسته‌ای در آپلود: فال‌بک تک‌به‌تک همه را ذخیره می‌کند (نه خطای ۵۰۰).
  7.  خطِ نصف‌شده‌ی سریال: با بافر خط کامل بازسازی می‌شود.
  8.  خطِ خراب سریال: در فایل پشتیبان می‌ماند.
  9.  خطای زودهنگام در ذخیره‌سازی: خط خام در فایل پشتیبان می‌ماند.
 10.  ایندکس یکتای master در برابر رکورد تکراریِ هم‌زمان مقاوم است.

اجرا در یک پوشه‌ی موقت انجام می‌شود تا دیتابیس‌های واقعی دست نخورند.
"""

import io
import os
import shutil
import struct
import sys
import tempfile
import unittest.mock as mock
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

WORKDIR = tempfile.mkdtemp(prefix="rf_noloss_")

# ایزوله‌سازی کامل: دیتابیس اصلی هم داخل پوشه‌ی موقت ساخته می‌شود
# (RF_MASTER_DB باید قبل از import app تنظیم شود)
os.environ['RF_MASTER_DB'] = os.path.join(WORKDIR, "test_master.db")

import app as flask_app  # noqa: E402

flask_app.DAILY_DB_DIR = WORKDIR
flask_app.UNSAVED_LOG = os.path.join(WORKDIR, "unsaved_records.log")

with flask_app.app.app_context():
    flask_app.db.create_all()
    flask_app._ensure_master_unique_index()

client = flask_app.app.test_client()

failures = []


def check(cond, msg):
    if cond:
        print(f"  ok  - {msg}")
    else:
        failures.append(msg)
        print(f"  FAIL- {msg}")


def master_count():
    with flask_app.app.app_context():
        return flask_app.MasterReading.query.count()


def master_first(**kw):
    with flask_app.app.app_context():
        return flask_app.MasterReading.query.filter_by(**kw).first()


def unsaved_lines():
    if not os.path.exists(flask_app.UNSAVED_LOG):
        return []
    with open(flask_app.UNSAVED_LOG, encoding="utf-8") as fh:
        return [l.rstrip("\n") for l in fh if l.strip()]


def line(num, date="2026-05-01", t="10:00:00", temp="24.50", hum="44.00",
         f1="OK", f2="OK", f3="NOK", f4="NOK"):
    return (f"NUM={num},BCM1_OPEN={f1},BCM1_CLOSE={f2},"
            f"BCM2_OPEN={f3},BCM2_CLOSE={f4},Temp={temp},Humidity={hum},"
            f"Date={date},Time={t}")


# =====================================================================
print("\n[1] اینجست انبوه — همه‌ی خطوط سالم ذخیره می‌شوند")
batch = [line(i, t=f"10:{i:02d}:00") for i in range(1, 11)]
r = client.post('/api/ingest', data="\n".join(batch),
                headers={'Content-Type': 'text/plain'}).get_json()
check(r['saved'] == 10, f"۱۰ خط سالم → saved=10 (پاسخ: {r})")
check(master_count() == 10, "۱۰ رکورد در master ثبت شد")
import sqlite3  # noqa: E402
with sqlite3.connect(os.path.join(WORKDIR, "2026-05-01.db")) as c:
    n_daily = c.execute("SELECT COUNT(*) FROM daily_records").fetchone()[0]
check(n_daily == 10, "۱۰ رکورد در دیتابیس روزانه ثبت شد")

print("\n[2] ارسال دوباره — تکراری، نه دوبله و نه از دست رفتن")
r = client.post('/api/ingest', data="\n".join(batch),
                headers={'Content-Type': 'text/plain'}).get_json()
check(r['saved'] == 0 and r['duplicates'] == 10, "۱۰ خط تکراری درست شمرده شد")
check(master_count() == 10, "دیتابیس دوبله نشد")

print("\n[3] خطِ دارای NUM= با فرمت ناقص — در فایل پشتیبان نجات می‌یابد")
bad = "NUM=999,BCM1_OPEN=OK,Temp=25,Humidity=40,Date=2026-05-01,Time=23:59:59"
before = len(unsaved_lines())
r = client.post('/api/ingest', data=bad,
                headers={'Content-Type': 'text/plain'}).get_json()
check(r['invalid'] == 1 and r.get('stashed') == 1,
      f"پاسخ درست است: invalid=1, stashed=1 (پاسخ: {r})")
check(len(unsaved_lines()) == before + 1 and bad in unsaved_lines()[-1],
      "خط ناقص در unsaved_records.log نگه داشته شد")
check(master_count() == 10, "چیز ناخواسته‌ای وارد دیتابیس نشد")

print("\n[4] Temp=nan (حافظه‌ی SPI خالی) — رکورد دور ریخته نمی‌شود")
r = client.post('/api/ingest', data=line(1000, t="23:59:58", temp="nan"),
                headers={'Content-Type': 'text/plain'}).get_json()
check(r['saved'] == 1, f"خط nan پذیرفته شد (پاسخ: {r})")
row = master_first(num_value=1000)
check(row is not None and row.temp == "0", "temp=nan به مقدار پیش‌فرض 0 تبدیل شد")

print("\n[5] فایل .dat با انتهای خراب — رکوردهای کامل نجات می‌یابند")
# فرمت فعلی (۲۸ بایتی): CycleAttempt + Indicator + Buzzer در انتها
rec1 = struct.pack('<iff????iBBBBBB??', 2001, 24.5, 40.0, 1, 0, 1, 0, 2026, 5, 2, 9, 0, 0, 1, True, False)
rec2 = struct.pack('<iff????iBBBBBB??', 2002, 24.6, 41.0, 0, 1, 0, 1, 2026, 5, 2, 9, 0, 1, 1, False, True)
blob = rec1 + rec2 + b"\x01\x02\x03"   # دو رکورد سالمِ متمایز + ۳ بایت خراب
before = master_count()
r = client.post('/upload_dat',
                data={'folder_upload': (io.BytesIO(blob), '2026-05-02.dat')},
                content_type='multipart/form-data').get_json()
check(r['processed'] == 2 and r.get('partial_files') == 1,
      f"۲ رکورد سالمِ فایلِ خراب ذخیره شد (پاسخ: {r})")
check(master_count() == before + 2, "هر دو رکورد در master ثبت شد")

print("\n[6] شکست ثبتِ دسته‌ای — فال‌بک تک‌به‌تک همه را نجات می‌دهد")
blob2 = struct.pack('<iff????iBBBBBB??', 3001, 24.5, 40.0, 1, 1, 1, 1, 2026, 5, 3, 8, 0, 0, 1, True, True) \
      + struct.pack('<iff????iBBBBBB??', 3002, 24.5, 40.0, 0, 0, 0, 0, 2026, 5, 3, 8, 0, 1, 1, False, False)
before = master_count()
with mock.patch.object(flask_app.db.session, 'bulk_save_objects',
                       side_effect=RuntimeError("db locked")):
    resp = client.post('/upload_dat',
                       data={'folder_upload': (io.BytesIO(blob2), '2026-05-03.dat')},
                       content_type='multipart/form-data')
r = resp.get_json()
check(resp.status_code == 200, "دیگر خطای ۵۰۰ برگردانده نمی‌شود")
check(r['processed'] == 2, f"فال‌بک تک‌به‌تک هر دو رکورد را ذخیره کرد (پاسخ: {r})")
check(master_count() == before + 2, "هیچ رکوردی از دست نرفت")

print("\n[7] خطِ نصف‌شده‌ی سریال — با بافر بازسازی می‌شود")
buf = flask_app.SerialLineBuffer()
whole = line(5000, date="2026-05-04", t="11:22:33")
got = buf.feed(whole[:40].encode())          # نصف اول
check(got == [], "نیمه‌خط تحویل داده نمی‌شود")
got = buf.feed(whole[40:].encode() + b"\n")  # نصف دوم + پایان خط
check(got == [whole], "خط کامل از دو تکه بازسازی شد")
got = buf.feed(b"[BOOT] reset\n")
check(got == ["[BOOT] reset"], "خط بعدی هم درست جدا شد")

print("\n[8] خطِ خراب سریال — در فایل پشتیبان می‌ماند")
before = len(unsaved_lines())
check(flask_app.handle_serial_line(whole) is True,
      "خط سالم سریال ذخیره شد")
check(master_first(num_value=5000) is not None, "رکورد سریال در master ثبت شد")
bad_serial = "NUM=6000,BCM1_OPEN=OK,Temp=25"
check(flask_app.handle_serial_line(bad_serial) is False,
      "خط خراب سریال رد شد")
check(bad_serial in unsaved_lines()[-1],
      "خط خراب سریال در unsaved_records.log نجات پیدا کرد")

print("\n[9] خطای زودهنگام در ذخیره‌سازی — خط خام در فایل پشتیبان می‌ماند")
class BoomSource:
    def get(self, *a, **kw):
        raise RuntimeError("boom")
before = len(unsaved_lines())
rawl = line(7000, date="2026-05-05", t="12:00:00")
with flask_app.app.app_context():
    ok = flask_app.save_sensor_data(BoomSource(), raw_line=rawl)
check(ok is False, "ذخیره ناموفق گزارش شد")
check(rawl in unsaved_lines()[-1],
      "خط خام با raw_line در فایل پشتیبان نجات پیدا کرد (بدون NameError)")

print("\n[10] ایندکس یکتای master — در برابر تکراریِ هم‌زمان مقاوم است")
from sqlalchemy.exc import IntegrityError  # noqa: E402
with flask_app.app.app_context():
    flask_app.db.session.add(flask_app.MasterReading(
        num_value=1, nbcm_selected="", humidity="40", temp="24",
        time="10:01:00", date="2026-05-01",
        timestamp=flask_app.datetime.datetime(2026, 5, 1, 10, 1),
        formatted_log=""))
    try:
        flask_app.db.session.commit()
        dup_blocked = False
    except IntegrityError:
        flask_app.db.session.rollback()
        dup_blocked = True
check(dup_blocked, "دیتابیس جلوی رکورد تکراری هم‌زمان را گرفت")

print("\n[11] صفحات بعد از تغییرات هنوز سالم رندر می‌شوند")
for url in ('/', '/history', '/plot_display', '/upload_dat', '/paste'):
    code = client.get(url).status_code
    check(code == 200, f"{url} → {code}")

# ---------------------------------------------------------------------
print()
if failures:
    print(f"✘ {len(failures)} مورد شکست خورد:")
    for f in failures:
        print("   -", f)
    sys.exit(1)

print("✔ تضمین «هیچ داده‌ای از دست نمی‌رود» در همه‌ی مسیرها برقرار است.")
shutil.rmtree(WORKDIR, ignore_errors=True)
