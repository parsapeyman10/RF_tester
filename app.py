# -*- coding: utf-8 -*-
"""
  Project: Flask WebServer Robust Industrial AP Receiver – Version 3.7 (Live Data Fix)
  Feature: Correct LATEST Data Retrieval, Strict Timestamp Sorting
  Engineer: [Peyman parsa] - Electronic Perspective
  
  Critical Fix:
  - Fixed '/api/sensor_data' to fetch the NEWEST 50 records (DESC sort -> Limit -> Reverse).
  - Ensured Dashboard always shows the absolute latest packet.
"""

from flask import Flask, render_template, request, redirect, url_for, flash, jsonify, Response, send_file
from flask_sqlalchemy import SQLAlchemy
from sqlalchemy.exc import IntegrityError
import datetime 
import io
import csv 
import pytz 
import serial
import threading
import time
import serial.tools.list_ports 
import os
import sys
import sqlite3
import math
import struct
import json
import re

# =====================================================================
#  مسیرها مستقل از پوشه‌ای که برنامه از آن اجرا می‌شود
#  (باگ: اجرای app.py از ریشه‌ی ریپو با خطای TemplateNotFound: index.html
#   می‌خورد چون Flask دنبال ./templates کنار فایل می‌گردد)
#
#  DATA_DIR از BASE_DIR جدا است تا خروجی PyInstaller (اپ دسکتاپ) هم
#  کار کند: قالب‌ها داخل بسته‌ی موقت (_MEIPASS) ولی دیتابیس‌ها، لاگ‌ها
#  و تنظیمات کنار خود فایل exe بمانند.
# =====================================================================
def _app_paths():
    if getattr(sys, 'frozen', False):          # اجرا از خروجی PyInstaller
        bundle_dir = getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
        data_dir = os.path.dirname(sys.executable)
    else:
        bundle_dir = os.path.dirname(os.path.abspath(__file__))
        data_dir = bundle_dir
    return bundle_dir, data_dir


BASE_DIR, DATA_DIR = _app_paths()


def _resolve_templates():
    cand = os.path.join(BASE_DIR, 'templates')
    if not os.path.isfile(os.path.join(cand, 'index.html')):
        print(f"[INIT] هشدار: پوشه‌ی قالب‌ها پیدا نشد: {cand}")
    return cand


# =====================================================================
#  نتیجه‌ی تست — تنها جای تعریف
#
#  هر سیکل برای هر دستگاه دو نتیجه‌ی جدا تولید می‌کند و نام فیلدها در کل
#  زنجیره (فریمور، سریال، دیتابیس، API، رابط کاربری) دقیقاً همین‌هاست:
#
#      BCM1_OPEN    BCM1_CLOSE    BCM2_OPEN    BCM2_CLOSE
# =====================================================================
RESULT_FIELDS = ("BCM1_OPEN", "BCM1_CLOSE", "BCM2_OPEN", "BCM2_CLOSE")
DEVICES = ("BCM1", "BCM2")

# (نام فیلد، دستگاه، حرکت، برچسب فارسی)
RESULT_MAP = (
    ("BCM1_OPEN",  "BCM1", "open",  "BCM1 باز شدن"),
    ("BCM1_CLOSE", "BCM1", "close", "BCM1 بسته شدن"),
    ("BCM2_OPEN",  "BCM2", "open",  "BCM2 باز شدن"),
    ("BCM2_CLOSE", "BCM2", "close", "BCM2 بسته شدن"),
)

# رکوردهای قدیمی با نام‌گذاری NBCMx ذخیره شده‌اند و باید خوانده شوند
LEGACY_FIELD_MAP = {
    "NBCM1": "BCM1_OPEN",
    "NBCM2": "BCM1_CLOSE",
    "NBCM3": "BCM2_OPEN",
    "NBCM4": "BCM2_CLOSE",
}


def normalize_fields(values):
    """نام‌های قدیمی NBCMx را به نام‌های جدید تبدیل و مرتب می‌کند"""
    out = []
    for v in values:
        v = (v or "").strip()
        if not v:
            continue
        v = LEGACY_FIELD_MAP.get(v, v)
        if v in RESULT_FIELDS and v not in out:
            out.append(v)
    return [f for f in RESULT_FIELDS if f in out]


def build_bcm_results(stored_fields):
    """
    از رشته‌ی ذخیره‌شده، نتیجه‌ی تفکیکی هر دستگاه را می‌سازد:

        {"BCM1": {"open": True, "close": False, "ok": False}, "BCM2": {...}}

    ok یعنی هم باز شدن و هم بسته شدن تأیید شده‌اند.
    """
    selected = set(normalize_fields((stored_fields or "").split(",")))
    out = {dev: {"open": False, "close": False, "ok": False} for dev in DEVICES}
    for field, dev, motion, _label in RESULT_MAP:
        if field in selected:
            out[dev][motion] = True
    for dev in DEVICES:
        out[dev]["ok"] = out[dev]["open"] and out[dev]["close"]
    return out


def compute_cycle_ok(stored_fields):
    """
    ستون «Cycle»: آیا این سیکل داده را کامل دریافت کرد؟
    یعنی AND هر چهار سیگنال: BCM1 (باز و بسته) AND BCM2 (باز و بسته).
    فقط وقتی هر دو BCM به‌طور کامل OK باشند True است.
    """
    res = build_bcm_results(stored_fields)
    return bool(res["BCM1"]["ok"] and res["BCM2"]["ok"])


TEMPLATE_DIR = _resolve_templates()
STATIC_DIR = os.path.join(BASE_DIR, 'static')

# دیتابیس‌های روزانه همیشه در پوشه‌ی داده (کنار app.py یا کنار exe)
# ساخته می‌شوند، نه در پوشه‌ی جاری
DAILY_DB_DIR = DATA_DIR


def daily_db_file(date_str):
    return os.path.join(DAILY_DB_DIR, f"{date_str}.db")


_flask_kwargs = {'template_folder': TEMPLATE_DIR, 'static_folder': STATIC_DIR}
if getattr(sys, 'frozen', False):
    # در خروجی exe، دیتابیس master کنار خود exe ساخته شود نه در پوشه‌ی موقت
    _flask_kwargs['instance_path'] = os.path.join(DATA_DIR, 'instance')
    os.makedirs(_flask_kwargs['instance_path'], exist_ok=True)
app = Flask(__name__, **_flask_kwargs)
print(f"[INIT] templates: {TEMPLATE_DIR}")
app.config['SECRET_KEY'] = 'industrial_secret_key_v3_7_live_fix' 
app.config['MAX_CONTENT_LENGTH'] = 1024 * 1024 * 1024
# --- تنظیمات زمانی و دیتابیس جامع ---
TEHRAN_TZ = pytz.timezone('Asia/Tehran')

# مسیر دیتابیس اصلی:
#   پیش‌فرض            → instance/master_industrial.db کنار پروژه
#                        (مستقل از پوشه‌ای که برنامه از آن اجرا می‌شود)
#   متغیر محیطی RF_MASTER_DB → مسیر مطلق دلخواه (برای تست‌ها و جداسازی،
#                        تا هرگز دیتابیس واقعی لمس نشود)
_master_env = os.environ.get('RF_MASTER_DB')
if _master_env:
    MASTER_DB_URI = 'sqlite:///' + os.path.abspath(_master_env).replace('\\', '/')
else:
    MASTER_DB_URI = 'sqlite:///master_industrial.db'

app.config['SQLALCHEMY_DATABASE_URI'] = MASTER_DB_URI
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False 

db = SQLAlchemy(app)

# --- مدل‌های داده ---
class MasterReading(db.Model):
    id = db.Column(db.Integer, primary_key=True) 
    num_value = db.Column(db.Integer, nullable=True, index=True) 
    nbcm_selected = db.Column(db.String(100), nullable=True) 
    humidity = db.Column(db.String(50), nullable=True)
    temp = db.Column(db.String(50), nullable=True)
    time = db.Column(db.String(50), nullable=True) 
    date = db.Column(db.String(50), nullable=True) 
    timestamp = db.Column(db.DateTime, nullable=False, index=True) 
    formatted_log = db.Column(db.String(500), nullable=True)
    # ستون «Cycle» — سیکلی که داده‌اش کامل دریافت شد: یعنی هم باز شدن و
    # هم بسته شدن، در هر دو BCM (BCM1 و BCM2) با موفقیت تأیید شده باشد.
    # True/False = AND چهار سیگنال (BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE)
    cycle = db.Column(db.Boolean, nullable=True, default=False, index=True)
    # ستون «Attempts» (پروتکل: cycle=<1|2|3>) — این یک مفهوم کاملاً جدا از
    # ستون Cycle بالاست: تعداد تلاش‌هایی که ESP32 برای این سیکل طول کشید
    # تا هر دو BCM را تایید کند (یا بعد از ۳ تلاش ناقص بماند).
    #   ۱ = بار اول موفق شد   ۲ = بار دوم موفق شد   ۳ = بعد از ۳ تلاش هم تمام شد
    # می‌تواند None باشد (رکوردهای قدیمی/فریمورهای قدیمی که این فیلد را نمی‌فرستند)
    cycle_attempt = db.Column(db.Integer, nullable=True)
    
class DailyRecordAdapter:
    def __init__(self, row):
        # row ساختاری است که از کوئری SELECT برمی‌گردد
        self.id = row[0]
        self.num_value = row[1]
        self.nbcm_selected = row[2] if row[2] else ""  # هندل کردن null
        self.temp = row[3]
        self.humidity = row[4]
        self.date = row[6] 
        self.time = row[7]
        # تبدیل رشته‌ی زمان ذخیره شده به آبجکت datetime
        # این کار ضروری است چون در HTML از متد .strftime() استفاده شده است
        time_str = row[5]
        try:
            # تلاش برای پارس کردن فرمت استاندارد (همراه با میکروثانیه)
            self.timestamp = datetime.datetime.strptime(time_str, '%Y-%m-%d %H:%M:%S.%f%z')
        except:
            try:
                # تلاش دوم: بدون تایم‌زون یا فرمت ساده‌تر
                self.timestamp = datetime.datetime.strptime(time_str.split('+')[0], '%Y-%m-%d %H:%M:%S.%f')
            except:
                try:
                     # تلاش سوم: فرمت ثانیه‌ای بدون اعشار
                     self.timestamp = datetime.datetime.strptime(time_str.split('.')[0], '%Y-%m-%d %H:%M:%S')
                except:
                     # در صورت خطا، زمان فعلی جایگزین می‌شود تا برنامه کرش نکند
                     self.timestamp = datetime.datetime.now()
                

# --- توابع مدیریت دیتابیس ---
def get_daily_db_path(date_str=None):
    if not date_str:
        date_str = datetime.datetime.now(TEHRAN_TZ).strftime('%Y-%m-%d')
    return daily_db_file(date_str)

# =====================================================================
#  متد ذخیره‌سازی (Data-Access Layer)
#  ------------------------------------------------------------------
#  هر رکورد دو جا نوشته می‌شود:
#    1) master_industrial.db  -> جدول MasterReading (تاریخچه‌ی کامل)
#    2) YYYY-MM-DD.db         -> جدول daily_records (فقط همان روز)
#
#  کلید یکتایی رکورد = (num_value, تاریخ دستگاه, ساعت دستگاه)
#  بنابراین اگر یک خط سریال دوباره برسد (اکوی ESP8266، ارسال مجدد بعد از
#  نبود ACK، یا آپلود دوباره‌ی همان فایل .dat) رکورد تکراری ثبت نمی‌شود.
# =====================================================================

DAILY_SCHEMA = """CREATE TABLE IF NOT EXISTS daily_records (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    num_value INTEGER,
                    nbcm_selected TEXT,
                    temp TEXT,
                    humidity TEXT,
                    log_date TEXT,
                    log_time TEXT,
                    full_timestamp DATETIME
                )"""

_daily_ready = set()
_last_cleanup_ts = 0.0


def open_daily_db(date_str):
    """اتصال آماده به دیتابیس روزانه: WAL + ایندکس یکتا + busy timeout."""
    path = daily_db_file(date_str)
    conn = sqlite3.connect(path, timeout=10)
    if path not in _daily_ready:
        c = conn.cursor()
        c.execute(DAILY_SCHEMA)
        try:
            c.execute("PRAGMA journal_mode=WAL")
            c.execute("PRAGMA synchronous=NORMAL")
        except Exception:
            pass
        c.execute("CREATE INDEX IF NOT EXISTS idx_date_time ON daily_records (log_date, log_time)")
        try:
            # جلوگیری از رکورد تکراری در سطح دیتابیس
            c.execute("""CREATE UNIQUE INDEX IF NOT EXISTS uq_daily_record
                         ON daily_records (num_value, log_date, log_time)""")
        except sqlite3.IntegrityError:
            # دیتابیس قدیمی که از قبل رکورد تکراری دارد؛ ایندکس یکتا ساخته نمی‌شود
            print(f"[DB] {path}: رکورد تکراری قدیمی موجود است، ایندکس یکتا رد شد")
        conn.commit()
        _daily_ready.add(path)
    return conn


def insert_daily_rows(date_str, rows):
    """درج دسته‌ای با نادیده گرفتن تکراری‌ها. خروجی: تعداد رکورد واقعاً درج‌شده."""
    if not rows:
        return 0
    conn = open_daily_db(date_str)
    try:
        c = conn.cursor()
        before = conn.total_changes
        c.executemany("""INSERT OR IGNORE INTO daily_records
                         (num_value, nbcm_selected, temp, humidity, log_date, log_time, full_timestamp)
                         VALUES (?, ?, ?, ?, ?, ?, ?)""", rows)
        conn.commit()
        return conn.total_changes - before
    finally:
        conn.close()


UNSAVED_LOG = os.path.join(DATA_DIR, "unsaved_records.log")


def record_to_line(num_value, fields, temp, humidity, date_str, time_str, cycle_attempt=None):
    """ساخت همان خط استاندارد پروژه از روی مقادیر یک رکورد"""
    # fields می‌تواند رشته‌ی «,»جداشده یا لیست باشد
    if isinstance(fields, (list, tuple, set)):
        fields = ",".join(fields)
    sel = set(normalize_fields((fields or "").split(",")))
    parts = [f"NUM={num_value}"]
    for name in RESULT_FIELDS:
        parts.append(f"{name}={'OK' if name in sel else 'NOK'}")
    parts.append(f"Temp={temp}")
    parts.append(f"Humidity={humidity}")
    parts.append(f"Date={date_str}")
    parts.append(f"Time={time_str}")
    if cycle_attempt is not None:
        parts.append(f"cycle={cycle_attempt}")
    return ",".join(parts)


def stash_unsaved(line, reason):
    """
    اگر به هر دلیلی نوشتن در دیتابیس شکست بخورد، خط خام در یک فایل متنی
    نگه داشته می‌شود تا هیچ داده‌ای از بین نرود. با /api/recover_unsaved
    دوباره وارد دیتابیس می‌شود.

    این تابع هرگز exception پرتاب نمی‌کند و True/False برمی‌گرداند —
    چون خودش آخرین حلقه‌ی نجات داده است.
    """
    try:
        if not line or not str(line).strip():
            return False
        with open(UNSAVED_LOG, "a", encoding="utf-8") as fh:
            fh.write(f"{datetime.datetime.now().isoformat()}\t{reason}\t{line}\n")
            fh.flush()
            os.fsync(fh.fileno())   # مقاوم در برابر قطع برق
        print(f"[DB] رکورد در {os.path.basename(UNSAVED_LOG)} نگه داشته شد: {reason}")
        return True
    except Exception as exc:
        print(f"[DB] حتی ذخیره‌ی پشتیبان هم ناموفق بود: {exc}")
        return False


# ترتیب مرتب‌سازی کل سیستم بر اساس «تاریخ و ساعتی که خودِ دستگاه ذخیره
# کرده» (RTC روی ESP32) — نه زمان آپلود و نه ساعت سرور.
#
# کلید مرتب‌سازی، لحظه‌ی کامل دستگاه است: تاریخ و بعد ساعت.
# چون هر دو با صفر پرشده‌اند (YYYY-MM-DD و HH:MM:SS) مقایسه‌ی رشته‌ای
# دقیقاً همان مقایسه‌ی زمانی است.
#
#   False -> تاریخ سپس ساعت  = لحظه‌ی کامل دستگاه   (حالت درست)
#   True  -> فقط ساعت، بدون توجه به روز             (رکوردهای روزها درهم می‌شوند)
SORT_BY_TIME_FIRST = False


def device_clock_order(newest_first=True):
    """کلیدهای مرتب‌سازی استاندارد بر اساس ساعت ذخیره‌شده توسط دستگاه"""
    if SORT_BY_TIME_FIRST:
        keys = (MasterReading.time, MasterReading.date)
    else:
        keys = (MasterReading.date, MasterReading.time)

    if newest_first:
        return tuple(k.desc() for k in keys)
    return tuple(k.asc() for k in keys)


def daily_order_sql(newest_first=True):
    """همان ترتیب، برای کوئری خام دیتابیس روزانه"""
    direction = "DESC" if newest_first else "ASC"
    if SORT_BY_TIME_FIRST:
        return f"ORDER BY log_time {direction}, log_date {direction}"
    return f"ORDER BY log_date {direction}, log_time {direction}"


def master_exists(num_value, date_str, time_str):
    """آیا این رکورد قبلاً در دیتابیس اصلی ثبت شده است؟"""
    try:
        return db.session.query(MasterReading.id).filter_by(
            num_value=num_value, date=date_str, time=time_str).first() is not None
    except Exception:
        return False


def cleanup_old_databases(days_to_keep=7):
    now = datetime.datetime.now(TEHRAN_TZ)
    for filename in os.listdir(DAILY_DB_DIR):
        if filename.endswith('.db') and filename != 'master_industrial.db':
            try:
                date_part = filename.replace('.db', '')
                file_date = datetime.datetime.strptime(date_part, '%Y-%m-%d')
                file_date = TEHRAN_TZ.localize(file_date)
                if (now - file_date).days >= days_to_keep:
                    os.remove(os.path.join(DAILY_DB_DIR, filename))
            except:
                continue

def get_available_dates():
    try:
        with app.app_context():
            # سورت DESC بسیار مهم است
            dates = db.session.query(MasterReading.date).distinct().order_by(MasterReading.date.desc()).all()
            return [d[0] for d in dates if d[0]]
    except Exception as e:
        print(f"[DB Error] {e}")
        return []

# --- توابع نرمال‌سازی ---
def normalize_value(val, min_v, max_v, default):
    try:
        if val is None: return str(default)
        # اصلاحیه: اضافه شدن c == '-' برای پشتیبانی از اعداد منفی
        clean_val = "".join(c for c in str(val) if c.isdigit() or c == '.' or c == '-')
        
        # هندل کردن حالت‌هایی مثل "--5" یا "-" خالی
        if not clean_val or clean_val == '-': return str(default)
            
        f_val = float(clean_val)
        f_val = max(min_v, min(max_v, f_val))
        return str(round(f_val, 1))
    except: return str(default)

def safe_int(val, default=0):
    try:
        if val is None: return default
        numeric_filter = "".join(filter(str.isdigit, str(val).strip()))
        return int(numeric_filter) if numeric_filter else default
    except: return default

def save_sensor_data(data_source, raw_line=None):
    """
    ذخیره‌ی یک رکورد در هر دو دیتابیس.

    raw_line: خط خامِ استاندارد (NUM=...). اگر ذخیره در دیتابیس شکست
    بخورد، همین خط — بی‌هیچ وابستگی به متغیرهای میانی — در
    unsaved_records.log نگه داشته می‌شود تا هیچ داده‌ای گم نشود.
    """
    global _last_cleanup_ts
    # مقدار اولیه‌ی همه‌ی متغیرها؛ اگر exception در همان اول کار رخ دهد
    # handler خطا دیگر به NameError نمی‌خورد و بکاپ ساخته می‌شود
    num_int = 0
    nbcm_str = ""
    t_val = "0"
    h_val = "0"
    i_date = None
    i_time = None
    try:
        # پاکسازی دیتابیس‌های قدیمی حداکثر هر ۱۰ دقیقه، نه به ازای هر رکورد
        if time.time() - _last_cleanup_ts > 600:
            _last_cleanup_ts = time.time()
            cleanup_old_databases()

        # --- استخراج داده‌ها ---
        if hasattr(data_source, 'getlist'):
            nbcm_checked_list = data_source.getlist('nbcm')
            num_val_raw = data_source.get('num_value')
        else:
            nbcm_checked_list = data_source.get('nbcm', [])
            num_val_raw = data_source.get('num_value')

        num_int = safe_int(num_val_raw)
        h_val = normalize_value(data_source.get('humidity'), -100, 100, 0)
        t_val = normalize_value(data_source.get('temp'), -100, 155, 0)

        now_tehran = datetime.datetime.now(TEHRAN_TZ)

        # دریافت زمان و تاریخ از پکت (یا استفاده از زمان حال در صورت نبودن)
        i_time = data_source.get('time') or now_tehran.strftime('%H:%M:%S')
        i_date = data_source.get('date') or now_tehran.strftime('%Y-%m-%d')

        # تعداد تلاش‌هایی که ESP32 برای این سیکل طول کشید (فیلد «cycle=»
        # در پروتکل). اختیاری است؛ اگر نیامده بود None می‌ماند.
        cyc_raw = data_source.get('cycle_attempt')
        cycle_attempt_val = None
        if cyc_raw not in (None, ""):
            try:
                cycle_attempt_val = max(1, min(3, int(cyc_raw)))
            except (TypeError, ValueError):
                cycle_attempt_val = None

        # --- زمان ثبت در دیتابیس = لحظه‌ی واقعیِ ساعت سرور ---
        # قبلاً اینجا از تاریخ/ساعتِ دستگاه (RTC) ساخته می‌شد؛ طبق درخواست،
        # «زمان ثبت» باید همان ساعت سرور (کامپیوتر) در لحظه‌ی ذخیره باشد.
        # تاریخ/ساعت دستگاه هنوز در ستون‌های جداگانه‌ی date/time نگه داشته
        # می‌شود و مبنای مرتب‌سازی و نمایش «ساعت دستگاه (RTC)» است.
        real_timestamp = now_tehran.replace(tzinfo=None)

        # هرچه غیر از کانال‌های تعریف‌شده باشد کنار گذاشته می‌شود
        nbcm_checked_list = normalize_fields(nbcm_checked_list)
        nbcm_str = ",".join(nbcm_checked_list)
        log_str = f"NUM:{num_int}, H:{h_val}, T:{t_val}"

        # خط استاندارد برای بکاپ — از همین لحظه آماده است
        backup_line = raw_line or record_to_line(
            num_int, nbcm_str, t_val, h_val, i_date, i_time, cycle_attempt_val)

        # --- جلوگیری از رکورد تکراری ---
        # منابع تکرار: اکوی سریال، ارسال مجدد ESP32 وقتی ACK گم می‌شود،
        # یا آپلود دوباره‌ی همان فایل. کلید یکتایی = NUM + تاریخ + ساعت دستگاه
        if master_exists(num_int, i_date, i_time):
            print(f"[DB] تکراری رد شد: NUM={num_int} {i_date} {i_time}")
            return True

        # 1. ذخیره در Master DB
        master_entry = MasterReading(
            num_value=num_int, nbcm_selected=nbcm_str,
            humidity=h_val, temp=t_val, time=i_time, date=i_date,
            timestamp=real_timestamp,  # زمان ثبت = ساعت سرور در لحظه‌ی ذخیره
            formatted_log=log_str,
            cycle=compute_cycle_ok(nbcm_str),
            cycle_attempt=cycle_attempt_val
        )
        db.session.add(master_entry)
        try:
            db.session.commit()
        except IntegrityError:
            # دو درخواست هم‌زمان از یک رکورد: ایندکس یکتا جلوی دوباره‌نویسی را گرفت
            db.session.rollback()
            print(f"[DB] تکراری موازی رد شد: NUM={num_int} {i_date} {i_time}")
            return True

        # 2. ذخیره در Daily DB (با ایندکس یکتا و INSERT OR IGNORE)
        #    اگر این مرحله شکست بخورد داده از دست نرفته — در master هست؛
        #    فقط یک هشدار چاپ می‌شود.
        try:
            insert_daily_rows(i_date, [(num_int, nbcm_str, t_val, h_val,
                                        i_date, i_time, real_timestamp)])
        except Exception as daily_err:
            print(f"[DB] ذخیره در دیتابیس روزانه ناموفق بود (رکورد در master سالم است): {daily_err}")

        return True

    except Exception as e:
        try:
            db.session.rollback()
        except Exception:
            pass
        print(f"[DB_ERROR] {e}")
        # تضمین: داده هرگز گم نمی‌شود — خط خام (اگر بود) وگرنه خط بازسازی‌شده
        line = raw_line
        if not line and i_date and i_time:
            line = record_to_line(num_int, nbcm_str, t_val, h_val, i_date, i_time)
        stash_unsaved(line, f"db_error: {e}")
        return False
    
# --- مدیریت سریال ---
ser = None 
serial_thread = None
stop_event = threading.Event()
active_serial_port = None 
active_baud_rate = 115200 
DEFAULT_PORT = "COM9"
manual_disconnect = False

# =====================================================================
#  لایه‌ی پروتکل لینک با ESP8266 (نسخه ۲ — انتقال مطمئن سه‌مرحله‌ای)
#
#  مسیر داده:   ESP32 --TCP--> ESP8266 --Serial--> سرور
#  مسیر تایید:  سرور --Serial("ACK <num>")--> ESP8266 --TCP--> ESP32
#
#  قواعد:
#   1) تا وقتی پورت سریال باز نشده، سرور هیچ «درخواست داده‌ای» نمی‌دهد؛
#      فقط بعد از اتصال، خط «SRV_READY <sid>» را می‌فرستد (هر ۱۰ ثانیه
#      تکرار می‌شود تا گیرنده تازه‌وصل/ریست‌شده هم بفهمد).
#   2) <sid> شناسه‌ی نشست است که یکسان در هر سه لایه حرکت می‌کند:
#      سرور -> ESP8266 -> ESP32. هر بار باز شدن پورت، شناسه‌ی تازه.
#   3) برای هر رکوردی که واقعاً در دیتابیس ثبت شد (یا از قبل بود) خط
#      «ACK <num>» برمی‌گردد. تا ACK نیامده، فرستنده همان رکورد را
#      دوباره می‌فرستد — دیتابیس رکورد تکراری را ACK می‌کند و دوبله
#      ثبت نمی‌شود، پس هیچ داده‌ای از دست نمی‌رود.
#   4) خطی که پارس نشد ACK نمی‌گیرد (تا دوباره ارسال شود) ولی در
#      unsaved_records.log هم نگه داشته می‌شود.
# =====================================================================
serial_write_lock = threading.Lock()
serial_session_id = None          # شناسه‌ی نشست فعلی (None = پورت بسته)
SRV_READY_PERIOD = 10.0           # ضربان «آماده‌ام» روی سریال (ثانیه)


def make_session_id():
    """شناسه‌ی نشست — کوتاه و خوانا، مثل S3F9A2"""
    import secrets
    return "S" + secrets.token_hex(3).upper()


def send_serial_line(line):
    """
    ارسال یک خط پروتکل به ESP8266 روی سریال.
    اگر پورت بسته باشد بی‌صدا False برمی‌گردد — نجات داده به عهده‌ی
    سازوکار retry فرستنده است، نه این تابع.
    """
    try:
        with serial_write_lock:
            if ser and ser.is_open:
                ser.write((line + "\n").encode("utf-8"))
                return True
    except Exception as exc:
        print(f"[SERIAL-TX] ارسال ناموفق ({line}): {exc}")
    return False

# =====================================================================
#  تنظیم از‌راه‌دور زمان‌بندی سیکل ESP32 (CYCLE_PERIOD_MS / RELAY_RETRY_GAP_MS)
#
#  به‌جای هاردکد در فرم‌ور، این دو مقدار از داشبورد قابل تغییرند و از
#  مسیر سرور --Serial--> ESP8266 --TCP--> ESP32 به‌صورت یک خط
#  «CFG CYCLE_PERIOD_MS=<ms>;RELAY_RETRY_GAP_MS=<ms>» فرستاده می‌شوند.
#  ESP32 خودش مقدار را اعتبارسنجی/clamp و در NVS دائمی می‌کند.
#
#  چون این مسیر «ارسال و فراموش» است (نه مثل داده‌ها که ACK دارند)، برای
#  اطمینان از رسیدن حتی اگر لحظه‌ی تنظیم، ESP32 آنلاین نباشد، همین خط
#  همراه هر ضربان SRV_READY (هر ۱۰ ثانیه) هم دوباره فرستاده می‌شود؛ به
#  محض اینکه ESP32 وصل شود، در اولین ضربان بعدی مقدار را می‌گیرد.
# =====================================================================
CYCLE_CONFIG_FILE = os.path.join(DATA_DIR, 'cycle_config.json')
DEFAULT_CYCLE_PERIOD_MS = 120000     # باید با DEFAULT_CYCLE_PERIOD_MS فرم‌ور ESP32 یکی باشد
DEFAULT_RELAY_RETRY_GAP_MS = 2000    # باید با DEFAULT_RELAY_RETRY_GAP_MS فرم‌ور ESP32 یکی باشد
MIN_CYCLE_PERIOD_MS, MAX_CYCLE_PERIOD_MS = 5000, 3600000
MIN_RELAY_RETRY_GAP_MS, MAX_RELAY_RETRY_GAP_MS = 200, 60000

cycle_config = {
    'cycle_period_ms': DEFAULT_CYCLE_PERIOD_MS,
    'relay_retry_gap_ms': DEFAULT_RELAY_RETRY_GAP_MS,
}


def load_cycle_config():
    global cycle_config
    if not os.path.exists(CYCLE_CONFIG_FILE):
        return
    try:
        with open(CYCLE_CONFIG_FILE, encoding='utf-8') as fh:
            saved = json.load(fh)
        cycle_config['cycle_period_ms'] = int(saved.get('cycle_period_ms', DEFAULT_CYCLE_PERIOD_MS))
        cycle_config['relay_retry_gap_ms'] = int(saved.get('relay_retry_gap_ms', DEFAULT_RELAY_RETRY_GAP_MS))
        print(f"[CFG] زمان‌بندی سیکل قبلی بازیابی شد: {cycle_config}")
    except Exception as exc:
        print(f"[CFG] خواندن تنظیمات زمان‌بندی ناموفق: {exc}")


def save_cycle_config():
    try:
        with open(CYCLE_CONFIG_FILE, 'w', encoding='utf-8') as fh:
            json.dump(cycle_config, fh)
    except Exception as exc:
        print(f"[CFG] ذخیره‌ی تنظیمات زمان‌بندی ناموفق: {exc}")


def build_cfg_line():
    return (f"CFG CYCLE_PERIOD_MS={cycle_config['cycle_period_ms']};"
            f"RELAY_RETRY_GAP_MS={cycle_config['relay_retry_gap_ms']}")


# =====================================================================
#  تنظیمات پورت سریال ماندگار می‌شوند
#  قبلاً فقط در حافظه بود؛ با هر بار بستن سرور، پورت و باود از دست
#  می‌رفت و باید دوباره دستی انتخاب می‌شد.
# =====================================================================
SERIAL_CONFIG_FILE = os.path.join(DATA_DIR, 'serial_config.json')


def save_serial_config():
    try:
        with open(SERIAL_CONFIG_FILE, 'w', encoding='utf-8') as fh:
            json.dump({'port': active_serial_port,
                       'baud_rate': active_baud_rate}, fh)
        print(f"[SERIAL] تنظیمات ذخیره شد: {active_serial_port} @ {active_baud_rate}")
    except Exception as exc:
        print(f"[SERIAL] ذخیره‌ی تنظیمات ناموفق: {exc}")


def load_serial_config():
    global active_serial_port, active_baud_rate
    if not os.path.exists(SERIAL_CONFIG_FILE):
        return
    try:
        with open(SERIAL_CONFIG_FILE, encoding='utf-8') as fh:
            cfg = json.load(fh)
        if cfg.get('port'):
            active_serial_port = cfg['port']
        active_baud_rate = int(cfg.get('baud_rate', active_baud_rate))
        print(f"[SERIAL] تنظیمات قبلی بازیابی شد: {active_serial_port} @ {active_baud_rate}")
    except Exception as exc:
        print(f"[SERIAL] خواندن تنظیمات ناموفق: {exc}")

# =====================================================================
#  الگوی دقیق خطی که ESP8266 روی سریال می‌فرستد (۱۳ فیلد).
#  استفاده از regex به‌جای split باعث می‌شود خطوط ناقص/به‌هم‌ریخته‌ی سریال
#  (که موقع ریست برد یا نویز پیش می‌آید) اصلاً وارد دیتابیس نشوند.
#
#  نکته: Temp/Humidity می‌تواند nan یا inf باشد — ESP32 برای حافظه‌های
#  خالی SPI با %.2f مقدار «nan» چاپ می‌کند. قبلاً چنین خطی کلأً رد
#  می‌شد (از دست رفتن رکورد سالم)؛ حالا پذیرفته و در normalize_value
#  به مقدار پیش‌فرض تبدیل می‌شود (همان رفتار آپلود فایل .dat).
# =====================================================================
_FLOAT_RE = r"-?(?:\d+(?:\.\d+)?|nan|NaN|NAN|inf|Inf|INF)"

#  فیلد انتهایی «,cycle=<1|2|3>» اختیاری است (رله‌ی ESP32 تعداد تلاش‌هایی
#  که برای تایید هر دو BCM طول کشید را می‌فرستد). اختیاری گذاشته شده تا
#  خط‌های فریمورهای قدیمی‌تر (قبل از این قابلیت) یا رکوردهای قدیمیِ
#  stash‌شده در unsaved_records.log هم بدون خطا پارس شوند.
_CYCLE_SUFFIX_RE = r"(?:,cycle=(?P<cyc>\d+))?"

INDUSTRIAL_LINE_RE = re.compile(
    r"NUM=(?P<num>-?\d+),"
    r"BCM1_OPEN=(?P<f1>[A-Za-z0-9]+),BCM1_CLOSE=(?P<f2>[A-Za-z0-9]+),"
    r"BCM2_OPEN=(?P<f3>[A-Za-z0-9]+),BCM2_CLOSE=(?P<f4>[A-Za-z0-9]+),"
    rf"Temp=(?P<temp>{_FLOAT_RE}),Humidity=(?P<hum>{_FLOAT_RE}),"
    r"Date=(?P<y>\d{4})-(?P<mo>\d{1,2})-(?P<d>\d{1,2}),"
    r"Time=(?P<hh>\d{1,2}):(?P<mi>\d{1,2}):(?P<ss>\d{1,2})"
    + _CYCLE_SUFFIX_RE
)

# فریمورهای قدیمی که هنوز NBCM1..4 می‌فرستند هم پذیرفته می‌شوند
LEGACY_LINE_RE = re.compile(
    r"NUM=(?P<num>-?\d+),"
    r"NBCM1=(?P<f1>[A-Za-z0-9]+),NBCM2=(?P<f2>[A-Za-z0-9]+),"
    r"NBCM3=(?P<f3>[A-Za-z0-9]+),NBCM4=(?P<f4>[A-Za-z0-9]+),"
    rf"Temp=(?P<temp>{_FLOAT_RE}),Humidity=(?P<hum>{_FLOAT_RE}),"
    r"Date=(?P<y>\d{4})-(?P<mo>\d{1,2})-(?P<d>\d{1,2}),"
    r"Time=(?P<hh>\d{1,2}):(?P<mi>\d{1,2}):(?P<ss>\d{1,2})"
    + _CYCLE_SUFFIX_RE
)

TRUE_TOKENS = ("OK", "1", "TRUE", "YES")


def parse_industrial_line(line):
    """یک خط سریال را به دیکشنری استاندارد تبدیل می‌کند (یا None اگر معتبر نبود)."""
    if not line:
        return None
    m = INDUSTRIAL_LINE_RE.search(line) or LEGACY_LINE_RE.search(line)
    if not m:
        return None
    g = m.groupdict()
    try:
        # هر چهار فیلد معنی دارند: باز/بسته برای هر یک از دو دستگاه
        nbcm = [RESULT_FIELDS[i] for i in range(4)
                if g[f"f{i + 1}"].strip().upper() in TRUE_TOKENS]
        date_str = "%04d-%02d-%02d" % (int(g["y"]), int(g["mo"]), int(g["d"]))
        time_str = "%02d:%02d:%02d" % (int(g["hh"]), int(g["mi"]), int(g["ss"]))
        # اعتبارسنجی واقعی تاریخ (مثلاً 2026-02-31 رد می‌شود)
        datetime.datetime.strptime(f"{date_str} {time_str}", "%Y-%m-%d %H:%M:%S")
        # «cycle=» یعنی تعداد تلاش‌هایی که ESP32 برای تایید هر دو BCM طول
        # کشید (۱/۲/۳). اختیاری است؛ اگر نیامده بود None می‌ماند (مثلاً
        # فریمور قدیمی یا ورودی دستی از صفحه‌ی /paste).
        cyc_raw = g.get("cyc")
        cycle_attempt = None
        if cyc_raw not in (None, ""):
            try:
                cycle_attempt = max(1, min(3, int(cyc_raw)))
            except (TypeError, ValueError):
                cycle_attempt = None
        return {
            'num_value': g["num"],
            'nbcm': nbcm,
            'temp': g["temp"],
            'humidity': g["hum"],
            'date': date_str,
            'time': time_str,
            'cycle_attempt': cycle_attempt,
        }
    except Exception as exc:
        print(f"[PARSE_ERR] {exc} :: {line[:120]}")
        return None


class SerialLineBuffer:
    """
    بافر خطوط سریال.

    چرا لازم است؟ خواندنِ «یک readline در هر لوپ» دو مشکل داشت:
      1) اگر نیمه‌ی یک خط خوانده می‌شد، بقیه‌اش در خواندنِ بعدی می‌آمد و
         هر دو تکه بی‌اعتبار می‌شدند (دو رکورد گم‌شده).
      2) نرخ تخلیه‌ی بافر کند بود و در ارسال پشت‌سرهمِ ESP8266 بایت‌ها
         سرریز و گم می‌شدند.
    این کلاس بایت‌ها را انباشته و فقط خطوط «کامل» (جداشده با \\n) تحویل
    می‌دهد؛ نیمه‌خط برای رسیدن بقیه‌ی بایت‌ها نگه داشته می‌شود.
    """

    def __init__(self):
        self._buf = b""

    def feed(self, chunk):
        """بایت جدید اضافه می‌کند و فهرست خطوط کامل را برمی‌گرداند"""
        if not chunk:
            return []
        self._buf += chunk
        lines = []
        while b"\n" in self._buf:
            raw, self._buf = self._buf.split(b"\n", 1)
            text = raw.decode("utf-8", errors="ignore").strip()
            if text:
                lines.append(text)
        return lines

    def pending(self):
        """آیا نیمه‌خطی در انتظار بقیه‌ی بایت‌ها مانده است؟"""
        return len(self._buf) > 0


def handle_serial_line(line):
    """
    یک خط کامل سریال را پردازش می‌کند.

    خطوط کنترلی لینک (SRV_HELLO / SRV_PING) همین‌جا جواب داده می‌شوند.
    برای خطوط داده: فقط وقتی «ACK <num>» برمی‌گردانیم که رکورد واقعاً در
    دیتابیس ثبت شده (یا از قبل موجود) باشد — یعنی فرستنده تا موفقیت
    واقعی تایید نمی‌گیرد و هیچ داده‌ای از دست نمی‌رود.

    هیچ خطی که NUM= داشته باشد دور ریخته نمی‌شود: اگر فرمتش کامل نبود
    در unsaved_records.log نگه داشته می‌شود تا بعداً بررسی شود.
    """
    # --- خطوط کنترلی لینک (بدون NUM=) ---
    if line.startswith("SRV_HELLO"):
        # گیرنده تازه بوت/وصل شده؛ وضعیت آماده بودن و شناسه‌ی نشست را بده
        send_serial_line(f"SRV_READY {serial_session_id or make_session_id()}")
        send_serial_line(build_cfg_line())  # زمان‌بندی جاری هم همراهش برود
        return True
    if line.startswith("SRV_PING"):
        send_serial_line(f"SRV_PONG {serial_session_id or ''}".strip())
        return True

    payload = parse_industrial_line(line)
    if payload:
        print(f"[RX] NUM={payload['num_value']} {payload['date']} {payload['time']}")
        with app.app_context():
            ok = save_sensor_data(payload, raw_line=line)
        if ok:
            # ثبت شد (یا تکراری بود) -> تایید به فرستنده؛
            # اگر خطای دیتابیس بود ACK نمی‌رود تا دستگاه دوباره بفرستد
            send_serial_line(f"ACK {payload['num_value']}")
        return ok
    if "NUM=" in line:
        print(f"[RX_BAD] {line[:120]}")
        stash_unsaved(line, "serial_parse_fail")
    return False


def read_serial_worker():
    global ser, active_serial_port, manual_disconnect, serial_session_id
    line_buf = SerialLineBuffer()
    last_ready_sent = 0.0

    while not stop_event.is_set():
        try:
            if (ser is None or not ser.is_open) and not manual_disconnect:
                available = [p.device.upper() for p in serial.tools.list_ports.comports()]
                target_port = active_serial_port if active_serial_port else (DEFAULT_PORT if DEFAULT_PORT in available else None)
                
                if target_port and target_port in available:
                    active_serial_port = target_port
                    try:
                        ser = serial.Serial(active_serial_port, active_baud_rate, timeout=0.2)
                        line_buf = SerialLineBuffer()   # بافرِ پورت قبلی دیگر معتبر نیست
                        # نشست تازه: شناسه‌ی جدید ساخته و اعلام آمادگی می‌شود.
                        # این تنها جایی است که سرور «درخواست داده» می‌دهد —
                        # یعنی فقط بعد از اینکه پورت واقعاً باز شده است.
                        serial_session_id = make_session_id()
                        last_ready_sent = time.time()
                        send_serial_line(f"SRV_READY {serial_session_id}")
                        send_serial_line(build_cfg_line())  # زمان‌بندی جاری هم همراهش برود
                        print(f"[SERIAL] Connected to {active_serial_port} "
                              f"(session {serial_session_id})")
                    except Exception as e:
                        print(f"[SERIAL_ERR] {e}")
                        time.sleep(2)
                else:
                    time.sleep(5)
                    continue

            if ser and ser.is_open:
                waiting = ser.in_waiting
                if waiting > 0:
                    # همه‌ی بایت‌های موجود یک‌جا خوانده می‌شوند تا بافر
                    # سریال در ارسال‌های پشت‌سرهم سرریز نکند
                    chunk = ser.read(waiting if waiting < 8192 else 8192)
                    for line in line_buf.feed(chunk):
                        try:
                            handle_serial_line(line)
                        except Exception as line_err:
                            print(f"[READ_ERR] {line_err}")
                    last_ready_sent = time.time()   # داده دارد می‌رسد؛ لینک زنده است
                elif line_buf.pending():
                    # نیمه‌خطی داریم؛ چند لحظه صبر تا بقیه‌ی خط برسد
                    time.sleep(0.02)
                else:
                    # ضربان «آماده‌ام»: گیرنده‌ی تازه‌وصل یا ریست‌شده بدون
                    # معطلی می‌فهمد که سرور پشت پورت نشسته است
                    if time.time() - last_ready_sent >= SRV_READY_PERIOD:
                        send_serial_line(f"SRV_READY {serial_session_id}")
                        send_serial_line(build_cfg_line())  # تضمین رسیدن حتی اگر ESP32 دیرتر وصل شده باشد
                        last_ready_sent = time.time()
                    time.sleep(0.01)
            else:
                if serial_session_id is not None:
                    serial_session_id = None   # پورت بسته شد؛ نشست تمام شد
                time.sleep(0.2)

        except Exception as e:
            if ser: 
                try: ser.close() 
                except: pass
            ser = None
            time.sleep(10)

# --- Routes ---
# --- افزودن کتابخانه مورد نیاز برای امنیت فایل (در بالای فایل ایمپورت شود بهتر است اما اینجا هم کار می‌کند) ---
from werkzeug.utils import secure_filename


# --- نسخه نهایی اصلاح شده: تفکیک زمان ثبت و زمان سنسور ---
@app.route('/upload_dat', methods=['GET', 'POST'])
def upload_dat_page():
    if request.method == 'POST':
        if 'folder_upload' not in request.files:
            return jsonify({'status': 'error', 'message': 'No files received'}), 400
        
        uploaded_files = request.files.getlist('folder_upload')
        # فرمت قدیمی (قبل از قابلیت cycle=): ۲۵ بایت، بدون تعداد تلاش
        STRUCT_FORMAT_OLD = '<iff????iBBBBB'
        EXPECTED_SIZE_OLD = struct.calcsize(STRUCT_FORMAT_OLD)   # 25
        # فرمت جدید: یک بایت اضافه در انتها برای cycle_attempt (۱/۲/۳)
        STRUCT_FORMAT_NEW = '<iff????iBBBBBB'
        EXPECTED_SIZE_NEW = struct.calcsize(STRUCT_FORMAT_NEW)   # 26

        def detect_record_format(data_len):
            """
            فایل‌های .dat قبل از این آپدیت ۲۵ بایت/رکورد بودند؛ از این به بعد
            ۲۶ بایت/رکورد (با cycle_attempt اضافه) خواهند بود. چون نمی‌توان
            مطمئن بود فایل آپلودی با کدام فریمور ساخته شده، اندازه‌ای که طول
            فایل را دقیقاً (بدون باقی‌مانده) می‌پوشاند انتخاب می‌شود —
            فرمت جدید در اولویت است.
            """
            if data_len % EXPECTED_SIZE_NEW == 0:
                return STRUCT_FORMAT_NEW, EXPECTED_SIZE_NEW
            if data_len % EXPECTED_SIZE_OLD == 0:
                return STRUCT_FORMAT_OLD, EXPECTED_SIZE_OLD
            return STRUCT_FORMAT_NEW, EXPECTED_SIZE_NEW  # پیش‌فرض؛ دنباله‌ی ناقص بعداً trim می‌شود

        master_buffer = []
        
        success_count = 0      # رکوردهای سالمِ جدید که برای ذخیره آماده شدند
        fail_count = 0         # فایل‌هایی که اصلاً رکورد کاملی نداشتند
        skipped_count = 0      # رکوردهای تکراری (از قبل در دیتابیس)
        partial_files = 0      # فایل‌هایی که انتهایشان رکورد ناقص داشت
        stashed_count = 0      # رکوردهایی که فقط در فایل پشتیبان ماندند
        
        upload_time_server = datetime.datetime.now(TEHRAN_TZ)

        for file in uploaded_files:
            filename = secure_filename(file.filename)
            if not filename.lower().endswith('.dat'): continue
            
            try:
                file_bytes = file.read()
                STRUCT_FORMAT, EXPECTED_SIZE = detect_record_format(len(file_bytes))
                has_cycle_attempt = (EXPECTED_SIZE == EXPECTED_SIZE_NEW)
                # اگر انتهای فایل خراب/ناقص بود، رکوردهای «کامل» داخلش
                # نجات داده می‌شوند — قبلاً کل فایل دور ریخته می‌شد.
                usable = (len(file_bytes) // EXPECTED_SIZE) * EXPECTED_SIZE
                if usable == 0:
                    fail_count += 1
                    continue
                if usable < len(file_bytes):
                    partial_files += 1
                    print(f"[UPLOAD] {filename}: {len(file_bytes) - usable} بایت انتهایی "
                          f"ناقص بود؛ {usable // EXPECTED_SIZE} رکورد کامل نجات داده شد")

                for i in range(0, usable, EXPECTED_SIZE):
                    chunk = file_bytes[i : i + EXPECTED_SIZE]
                    data = struct.unpack(STRUCT_FORMAT, chunk)
                    
                    # --- استخراج و کستینگ (تبدیل اجباری به رشته) ---
                    num_val = data[0]
                    raw_temp = data[1]
                    raw_hum = data[2]

                    # دما (Temp) — بررسی NaN/Inf برای حافظه‌های SPI خالی
                    if math.isnan(raw_temp) or math.isinf(raw_temp):
                        temp_str = "0"
                    else:
                        temp_str = str(round(max(-100.0, min(155.0, raw_temp)), 2))

                    # رطوبت (Humidity)
                    if math.isnan(raw_hum) or math.isinf(raw_hum):
                        hum_str = "0"
                    else:
                        hum_str = str(round(max(0.0, min(100.0, raw_hum)), 2))
                    
                    # چهار نتیجه‌ی تفکیکی: باز/بسته برای هر دستگاه
                    # چهار بولینِ ساختار باینری به‌ترتیب:
                    # BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE
                    nbcm_str = ",".join(RESULT_FIELDS[k] for k in range(4) if data[3 + k])

                    # زمان سنسور (RTC دستگاه)
                    year, month, day = data[7], data[8], data[9]
                    hour, minute, second = data[10], data[11], data[12]
                    
                    device_date_str = f"{year}-{month:02d}-{day:02d}"
                    device_time_str = f"{hour:02d}:{minute:02d}:{second:02d}"

                    formatted_log = f"NUM:{num_val}, H:{hum_str}, T:{temp_str}"

                    # تعداد تلاش (cycle_attempt): فقط فایل‌های فرمت جدید (۲۶ بایت)
                    # این بایت آخر را دارند؛ فایل‌های قدیمی None می‌مانند
                    cyc_val = None
                    if has_cycle_attempt:
                        cyc_raw = data[13]
                        if cyc_raw:
                            cyc_val = max(1, min(3, int(cyc_raw)))

                    # رد کردن رکوردی که قبلاً ثبت شده (آپلود دوباره‌ی همان پوشه)
                    if master_exists(num_val, device_date_str, device_time_str):
                        skipped_count += 1
                        continue

                    master_buffer.append(MasterReading(
                        num_value=num_val,
                        nbcm_selected=nbcm_str,
                        humidity=hum_str,   # قبلاً Float بود که باعث خطا می‌شد
                        temp=temp_str,      # قبلاً Float بود که باعث خطا می‌شد
                        time=device_time_str,
                        date=device_date_str,
                        # زمان ثبت = ساعت سرور در لحظه‌ی آپلود (نه RTC دستگاه)
                        timestamp=upload_time_server.replace(tzinfo=None),
                        formatted_log=formatted_log,
                        cycle=compute_cycle_ok(nbcm_str),
                        cycle_attempt=cyc_val
                    ))
                    success_count += 1
            except Exception as e:
                print(f"[Batch Err] {filename}: {e}")
                fail_count += 1

        if success_count > 0:
            try:
                db.session.bulk_save_objects(master_buffer)
                db.session.commit()
            except Exception as e:
                # ثبت دسته‌ای شکست خورد -> رکورد به رکورد تلاش می‌کنیم؛
                # قبلاً کل دسته با خطای ۵۰۰ از بین می‌رفت و هیچ ردی نمی‌ماند.
                db.session.rollback()
                print(f"[UPLOAD] ثبت دسته‌ای ناموفق ({e})؛ تلاش تک‌به‌تک...")
                saved_one_by_one = 0
                for m in master_buffer:
                    try:
                        db.session.add(m)
                        db.session.commit()
                        saved_one_by_one += 1
                    except IntegrityError:
                        # تکراری واقعی (ثبت هم‌زمان) — نه خطا، نه از دست رفتن
                        db.session.rollback()
                        skipped_count += 1
                    except Exception as e2:
                        db.session.rollback()
                        stash_unsaved(
                            record_to_line(m.num_value, m.nbcm_selected, m.temp,
                                           m.humidity, m.date, m.time),
                            f"upload_db_error: {e2}")
                        stashed_count += 1
                success_count = saved_one_by_one

            # دیتابیس روزانه از روی همان رکوردهای master پر می‌شود؛
            # خرابیِ آن یعنی از دست رفتن داده نیست (master مرجع است)
            try:
                daily_map = {}
                for m in master_buffer:
                    daily_map.setdefault(m.date, []).append(
                        (m.num_value, m.nbcm_selected, m.temp, m.humidity,
                         m.date, m.time, m.timestamp))
                for log_date, records in daily_map.items():
                    inserted = insert_daily_rows(log_date, records)
                    print(f"[UPLOAD] {log_date}: {inserted}/{len(records)} رکورد جدید")
            except Exception as e:
                print(f"[UPLOAD] دیتابیس روزانه ناموفق (رکوردها در master سالم‌اند): {e}")

        return jsonify({'status': 'success', 'processed': success_count,
                        'failed': fail_count, 'skipped_duplicates': skipped_count,
                        'partial_files': partial_files, 'stashed': stashed_count})
    return render_template('upload.html')

@app.route('/')
def home():
    # اصلاحیه صنعتی: دریافت آخرین رکورد بر اساس "زمان دستگاه"
    # سورت نزولی روی تاریخ و سپس ساعت
    last = MasterReading.query.order_by(*device_clock_order()).first()
    
    return render_template('index.html', last_data=last)

@app.route('/history')
def history():
    target_date = request.args.get('date')
    readings = []
    label = "آرشیو کامل (Master DB)"
    
    # لیست تاریخ‌های موجود را همچنان از مستر می‌گیریم (چون مرجع جامع است)
    available_dates = get_available_dates()

    if target_date:
        # سناریوی صنعتی: خواندن از فایل روزانه (Distributed Data Access)
        daily_db_path = daily_db_file(target_date)
        
        if os.path.exists(daily_db_path):
            try:
                label = f"آرشیو روزانه (Source: {target_date}.db)"
                
                # اتصال به دیتابیس روزانه
                with sqlite3.connect(daily_db_path) as conn:
                    c = conn.cursor()
                    
                    # دریافت داده‌ها با استفاده از ایندکس زمانی که قبلاً ساختیم
                    # ترتیب نزولی (DESC) یعنی جدیدترین داده‌ها اول نمایش داده شوند
                    # باگ: قبلاً فقط ۶ ستون انتخاب می‌شد ولی DailyRecordAdapter
                    # به row[6] (تاریخ) و row[7] (ساعت) نیاز دارد -> IndexError
                    # و صفحه‌ی آرشیو روزانه همیشه خالی نمایش داده می‌شد.
                    c.execute(f"""
                        SELECT id, num_value, nbcm_selected, temp, humidity,
                               full_timestamp, log_date, log_time
                        FROM daily_records
                        {daily_order_sql()}
                    """)
                    
                    rows = c.fetchall()
                    
                    # تبدیل داده‌های خام به آبجکت‌های سازگار با HTML
                    readings = [DailyRecordAdapter(row) for row in rows]
                    
            except Exception as e:
                flash(f"خطا در بارگذاری فایل روزانه: {e}", "danger")
                readings = []
        else:
            # حالت Fallback: اگر فایل روزانه نبود، از مستر بخوان
            # (مثلاً برای داده‌های قدیمی که قبل از این آپدیت ثبت شده‌اند)
            label = f"آرشیو فیلتر شده (Master DB): {target_date}"
            query = MasterReading.query.filter(MasterReading.date == target_date)
            readings = query.order_by(*device_clock_order()).all()
            
    else:
        # حالت پیش‌فرض: نمایش تمام داده‌ها از مستر
        readings = MasterReading.query.order_by(*device_clock_order()).all()

    return render_template('history.html', readings=readings, label=label,
                           dates=available_dates, current_date=target_date,
                           bcm_results=build_bcm_results, cycle_ok=compute_cycle_ok)

@app.route('/submit_form', methods=['POST'])
def submit_form():
    if save_sensor_data(request.form):
        flash("اطلاعات ثبت شد.", "success")
    else:
        flash("خطا در ثبت.", "danger")
    return redirect(url_for('home'))

@app.route('/api/sensor_data')
def get_sensor_data():
    try:
        # گام 1: دریافت 50 رکورد آخر بر اساس "زمان دستگاه"
        # این کوئری تضمین می‌کند جدیدترین دیتای تولید شده در سنسور خوانده شود
        readings = MasterReading.query.order_by(*device_clock_order()).limit(50).all()
        
        # گام 2: معکوس کردن لیست برای نمایش درست در نمودار (از چپ به راست: قدیم به جدید)
        readings = readings[::-1]
        
        output = []
        for r in readings:
            # پارس کردن وضعیت‌های NBCM برای روشن/خاموش کردن چراغ‌ها
            nbcm_map = {
                key: ("active" if r.nbcm_selected and key in r.nbcm_selected else "notactive")
                for key in RESULT_FIELDS
            }
            
            output.append({
                'num_value': r.num_value, 
                'temp': r.temp, 
                'humidity': r.humidity,
                'time': r.time,       # زمان دستگاه
                'date': r.date,       # تاریخ دستگاه
                'timestamp': r.timestamp, # زمان آپلود (صرفا جهت اطلاع)
                'nbcm_statuses': nbcm_map,
                'bcm_results': build_bcm_results(r.nbcm_selected),
                'cycle': bool(r.cycle) if r.cycle is not None else compute_cycle_ok(r.nbcm_selected),
                'cycle_attempt': r.cycle_attempt
            })
            
        return jsonify(output)
        
    except Exception as e:
        print(f"[API Error] {e}")
        return jsonify([])


# =====================================================================
#  آمار تجمعیِ همه‌ی رکوردهای ثبت‌شده (از ابتدا تا الان) برای BCM1 و BCM2
#  به تفکیک باز شدن (OPEN) و بسته شدن (CLOSE):
#      «کار کرده»  = همان حرکت در آن رکورد با موفقیت تأیید شده (OK)
#      «کار نکرده» = همان حرکت تأیید نشده (NOK)
#  برای نمایش در مکعب دما/رطوبت در بالای داشبورد
# =====================================================================
@app.route('/api/bcm_stats')
def api_bcm_stats():
    try:
        from sqlalchemy import func, or_

        # نکته‌ی مهم: رکوردهای قدیمی ممکن است با نام‌گذاری قدیمی
        # NBCM1..NBCM4 ذخیره شده باشند (قبل از مهاجرت به BCM1_OPEN...).
        # هر دو فرمت (جدید و قدیمی) با OR در یک کوئری شمرده می‌شوند تا
        # هم درست باشد و هم روی جدول‌های بزرگ کند نشود (کل جدول به پایتون
        # کشیده نمی‌شود، فقط COUNT در خود دیتابیس انجام می‌شود).
        NEW_TO_LEGACY = {v: k for k, v in LEGACY_FIELD_MAP.items()}

        def patterns_for(field_name):
            pats = [field_name]
            legacy = NEW_TO_LEGACY.get(field_name)
            if legacy:
                pats.append(legacy)
            return pats

        def count_match(*field_names):
            """تعداد رکوردهایی که همه‌ی field_name ها حضور دارند (AND)؛
            هر فیلد می‌تواند با نام جدید یا معادل قدیمی‌اش ذخیره شده باشد (OR)."""
            q = db.session.query(func.count(MasterReading.id))
            for name in field_names:
                pats = patterns_for(name)
                q = q.filter(or_(*[MasterReading.nbcm_selected.like(f"%{p}%") for p in pats]))
            return q.scalar() or 0

        total = db.session.query(func.count(MasterReading.id)).scalar() or 0

        bcm1_open_ok = count_match("BCM1_OPEN")
        bcm1_close_ok = count_match("BCM1_CLOSE")
        bcm2_open_ok = count_match("BCM2_OPEN")
        bcm2_close_ok = count_match("BCM2_CLOSE")

        # عملکرد صحیح = AND باز و بسته با هم؛ یعنی فقط وقتی هر دو حرکت
        # با موفقیت تأیید شده باشند، آن چرخه «کار کرده» حساب می‌شود
        bcm1_correct_ok = count_match("BCM1_OPEN", "BCM1_CLOSE")
        bcm2_correct_ok = count_match("BCM2_OPEN", "BCM2_CLOSE")

        def stat(ok):
            return {'ok': ok, 'fail': max(total - ok, 0)}

        return jsonify({
            'total': total,
            'BCM1': {'open': stat(bcm1_open_ok), 'close': stat(bcm1_close_ok),
                     'correct': stat(bcm1_correct_ok)},
            'BCM2': {'open': stat(bcm2_open_ok), 'close': stat(bcm2_close_ok),
                     'correct': stat(bcm2_correct_ok)},
        })
    except Exception as e:
        print(f"[API Error] bcm_stats: {e}")
        _empty = {'open': {'ok': 0, 'fail': 0}, 'close': {'ok': 0, 'fail': 0},
                  'correct': {'ok': 0, 'fail': 0}}
        # خطای واقعی هم در پاسخ برگردانده می‌شود تا بدون نیاز به دسترسی به
        # کنسول سرور هم بشود علت صفر ماندن شمارش را فهمید
        return jsonify({'total': 0, 'BCM1': _empty, 'BCM2': dict(_empty),
                        'error': str(e)})


# در فایل app.py، این تابع را جایگزین تابع get_master_data کنید

@app.route('/api/master_data')
def get_master_data():
    start_date = request.args.get('start_date')
    end_date = request.args.get('end_date')
    
    query = MasterReading.query
    
    # اگر فیلتر تاریخ نداشتیم، به جای 1 روز، 1000 رکورد آخر را بیاور (برای سرعت و پر بودن نمودار)
    if not start_date and not end_date:
        # دریافت 1000 رکورد آخر بر اساس زمان سنسور
        readings = MasterReading.query.order_by(*device_clock_order()).limit(1000).all()
        # چون limit دیتای آخر را می‌آورد، باید لیست را برعکس کنیم تا در نمودار از چپ به راست باشد
        readings = readings[::-1]
    else:
        # اگر فیلتر داشت، طبق فیلتر عمل کن
        if start_date:
            query = query.filter(MasterReading.date >= start_date)
        if end_date:
            query = query.filter(MasterReading.date <= end_date)
        readings = query.order_by(*device_clock_order(newest_first=False)).all()
    
    output = []
    for r in readings:
        # پارس کردن وضعیت‌های NBCM
        nbcm_map = {}
        for key in RESULT_FIELDS:
            nbcm_map[key] = "active" if r.nbcm_selected and key in r.nbcm_selected else "inactive"

        # *** بخش حیاتی: ساخت فرمت استاندارد ISO با حرف T ***
        if r.date and r.time:
            # خروجی مثل: 2026-01-01T12:30:45
            iso_timestamp = f"{r.date}T{r.time}"
        else:
            iso_timestamp = r.timestamp.strftime('%Y-%m-%dTH:%M:%S')

        output.append({
            'num_value': r.num_value, 
            'temp': r.temp, 
            'humidity': r.humidity,
            'timestamp': iso_timestamp,  # <--- این متغیر کلیدی است
            'nbcm_statuses': nbcm_map,
            'bcm_results': build_bcm_results(r.nbcm_selected),
            'cycle': bool(r.cycle) if r.cycle is not None else compute_cycle_ok(r.nbcm_selected),
            'cycle_attempt': r.cycle_attempt,
            'date': r.date
        })

    return jsonify(output)

# =====================================================================
#  ورودی واحد دیتا — «هر وسیله‌ای وصل شد، دیتا برود توی app.py»
#
#  گوشی، لپ‌تاپ، اسکریپت پایتون یا هر کلاینت دیگری که به ESP32 وصل
#  می‌شود، همان خط‌های استاندارد NUM=... را می‌گیرد و عیناً به اینجا
#  POST می‌کند. یعنی مسیر داده در گوشی و سیستم دقیقاً یکی است:
#
#      ESP32 --(NUM=... خط)--> کلاینت --POST /api/ingest--> دیتابیس
#      ESP32 --(NUM=... خط)--> ESP8266 --Serial--------------> دیتابیس
#
#  بدنه‌ی درخواست می‌تواند متن خام (چند خط) یا JSON باشد:
#      {"lines": ["NUM=...", "NUM=..."]}
#  پاسخ: تعداد ذخیره‌شده / تکراری / نامعتبر
# =====================================================================
@app.route('/api/ingest', methods=['POST'])
def api_ingest():
    raw_lines = []
    payload_json = request.get_json(silent=True)

    if isinstance(payload_json, dict) and 'lines' in payload_json:
        raw_lines = payload_json.get('lines') or []
    elif isinstance(payload_json, list):
        raw_lines = payload_json
    else:
        body = request.get_data(as_text=True) or ''
        raw_lines = body.splitlines()

    saved = 0
    duplicates = 0
    invalid = 0
    ignored = 0
    stashed = 0
    device = request.headers.get('X-Device', request.remote_addr or 'unknown')

    for line in raw_lines:
        line = (line or '').strip()
        # خط خالی، نشانگرهای پروتکل، بلاک‌های ``` و هر خطی که اصلاً رکورد
        # نیست (مثل لاگ‌های [HEALTH]) بی‌سروصدا رد می‌شوند
        if not line or line in ('END', 'NO_DATA') or 'NUM=' not in line:
            ignored += 1
            continue
        parsed = parse_industrial_line(line)
        if not parsed:
            invalid += 1
            # هیچ داده‌ای بی‌ردپا دور ریخته نمی‌شود: خطِ ناقص در
            # unsaved_records.log نگه داشته می‌شود
            if stash_unsaved(line, "ingest_parse_fail"):
                stashed += 1
            continue
        if master_exists(safe_int(parsed['num_value']), parsed['date'], parsed['time']):
            duplicates += 1
            continue
        if save_sensor_data(parsed, raw_line=line):
            saved += 1
        else:
            # خط خام همین حالا داخل save_sensor_data در فایل پشتیبان است
            invalid += 1
            stashed += 1

    print(f"[INGEST] from {device}: saved={saved} dup={duplicates} "
          f"bad={invalid} ignored={ignored} stashed={stashed}")
    return jsonify({'status': 'success', 'saved': saved,
                    'duplicates': duplicates, 'invalid': invalid,
                    'ignored': ignored, 'stashed': stashed})


@app.after_request
def no_cache_html(resp):
    """
    صفحه‌های HTML هیچ‌وقت کش نشوند.
    بدون این، مرورگر نسخه‌ی قدیمی داشبورد را نگه می‌داشت و تغییرات
    (مثل نمودار جدید) تا Ctrl+F5 دیده نمی‌شد.
    """
    if resp.content_type and resp.content_type.startswith('text/html'):
        resp.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate, max-age=0'
        resp.headers['Pragma'] = 'no-cache'
        resp.headers['Expires'] = '0'
    return resp


@app.route('/save_data')
def save_data():
    """
    «سیو دیتا» — گرفتن نسخه‌ی پشتیبان از دیتابیس اصلی.

    از API پشتیبان‌گیری خود SQLite استفاده می‌شود تا حتی وقتی سرور مشغول
    نوشتن است، فایل خروجی سالم و یکدست باشد.
    """
    try:
        db.session.commit()
        src_path = db.engine.url.database
        if not src_path or not os.path.exists(src_path):
            return jsonify({'status': 'error', 'message': 'فایل دیتابیس پیدا نشد'}), 404

        stamp = datetime.datetime.now(TEHRAN_TZ).strftime('%Y%m%d_%H%M%S')
        out_path = os.path.join(DATA_DIR, f'backup_{stamp}.db')

        src = sqlite3.connect(src_path)
        dst = sqlite3.connect(out_path)
        with dst:
            src.backup(dst)      # نسخه‌ی یکدست حتی هنگام نوشتن
        dst.close()
        src.close()

        print(f"[BACKUP] نسخه‌ی پشتیبان ساخته شد: {os.path.basename(out_path)}")
        return send_file(out_path, as_attachment=True,
                         download_name=f'RF_tester_{stamp}.db')
    except Exception as exc:
        print(f"[BACKUP] خطا: {exc}")
        return jsonify({'status': 'error', 'message': str(exc)}), 500


@app.route('/import_csv', methods=['POST'])
def import_csv():
    """
    ورود همان CSV‌ای که خودِ سیستم با «خروجی اکسل» می‌سازد.

    ستون‌های مورد انتظار (ترتیب مهم نیست، ستون اضافه اشکالی ندارد):
        NUM, BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE,
        Temp, Humidity, Time, Date

    مقدار هر سیگنال می‌تواند OK/NOK یا 1/0 یا true/false باشد.
    ستون‌های ID و *_OK و Timestamp نادیده گرفته می‌شوند چون از روی
    بقیه دوباره ساخته می‌شوند.
    """
    if 'csv_file' not in request.files:
        return jsonify({'status': 'error', 'message': 'فایلی ارسال نشد'}), 400

    saved = duplicates = invalid = 0
    errors = []

    for fh in request.files.getlist('csv_file'):
        if not fh.filename:
            continue
        try:
            text = fh.read().decode('utf-8-sig', errors='ignore')
        except Exception as exc:
            errors.append(f"{fh.filename}: {exc}")
            continue

        reader = csv.DictReader(io.StringIO(text))
        if not reader.fieldnames:
            errors.append(f"{fh.filename}: سرستون پیدا نشد")
            continue

        # نگاشت نام ستون‌ها بدون حساسیت به بزرگی/کوچکی و فاصله
        cols = {(c or '').strip().upper(): c for c in reader.fieldnames}

        def pick(row, *names):
            for n in names:
                real = cols.get(n)
                if real is not None and row.get(real) not in (None, ''):
                    return str(row[real]).strip()
            return ''

        for row in reader:
            num = pick(row, 'NUM', 'NUM_VALUE')
            date_s = pick(row, 'DATE')
            time_s = pick(row, 'TIME')
            if not num or not date_s or not time_s:
                invalid += 1
                continue

            fields = []
            for name in RESULT_FIELDS:
                legacy = [k for k, v in LEGACY_FIELD_MAP.items() if v == name]
                val = pick(row, name, *legacy).upper()
                if val in TRUE_TOKENS:
                    fields.append(name)

            # ستون «Attempts»/«CYCLE_ATTEMPT» اختیاری است (خروجی اکسل جدید آن
            # را دارد؛ فایل‌های قدیمی‌تر ندارند و None می‌ماند)
            cyc_raw = pick(row, 'ATTEMPTS', 'CYCLE_ATTEMPT')
            cyc_val = None
            if cyc_raw:
                try:
                    cyc_val = max(1, min(3, int(float(cyc_raw))))
                except (TypeError, ValueError):
                    cyc_val = None

            payload = {
                'num_value': num,
                'nbcm': fields,
                'temp': pick(row, 'TEMP') or '0',
                'humidity': pick(row, 'HUMIDITY', 'HUM') or '0',
                'date': date_s,
                'time': time_s,
                'cycle_attempt': cyc_val,
            }
            # خط استاندارد برای بکاپ‌گیری در صورت شکست ذخیره
            raw_line = record_to_line(safe_int(num), fields,
                                      payload['temp'], payload['humidity'],
                                      date_s, time_s, cyc_val)

            if master_exists(safe_int(num), date_s, time_s):
                duplicates += 1
                continue
            if save_sensor_data(payload, raw_line=raw_line):
                saved += 1
            else:
                invalid += 1

    print(f"[CSV] ورود از فایل: saved={saved} dup={duplicates} bad={invalid}")
    result = {'status': 'success', 'saved': saved,
              'duplicates': duplicates, 'invalid': invalid}
    if errors:
        result['errors'] = errors
    return jsonify(result)


@app.route('/paste')
def paste_page():
    """صفحه‌ی افزودن دستی رکورد با چسباندن متن خام سریال"""
    return render_template('paste.html')


@app.route('/api/recover_unsaved', methods=['POST', 'GET'])
def api_recover_unsaved():
    """رکوردهایی که قبلاً در فایل پشتیبان مانده‌اند را دوباره وارد دیتابیس می‌کند"""
    if not os.path.exists(UNSAVED_LOG):
        return jsonify({'status': 'success', 'recovered': 0, 'remaining': 0})

    with open(UNSAVED_LOG, encoding='utf-8') as fh:
        lines = fh.readlines()

    recovered, remaining = 0, []
    for raw in lines:
        parts = raw.rstrip("\n").split("\t")
        payload = parse_industrial_line(parts[-1]) if parts else None
        if payload and save_sensor_data(payload):
            recovered += 1
        else:
            remaining.append(raw)

    with open(UNSAVED_LOG, "w", encoding="utf-8") as fh:
        fh.writelines(remaining)

    print(f"[DB] بازیابی: {recovered} رکورد برگشت، {len(remaining)} باقی ماند")
    return jsonify({'status': 'success', 'recovered': recovered,
                    'remaining': len(remaining)})


@app.route('/api/server_time')
def api_server_time():
    """ساعت خودِ کامپیوتری که سرور روی آن اجرا می‌شود (جدا از ساعت دستگاه)"""
    now = datetime.datetime.now(TEHRAN_TZ)
    return jsonify({
        'server_time': now.strftime('%H:%M:%S'),
        'server_date': now.strftime('%Y-%m-%d'),
        'iso': now.isoformat(),
    })


@app.route('/api/serial_ports')
def list_serial_ports():
    ports = [port.device for port in serial.tools.list_ports.comports()]
    return jsonify({
        'available_ports': ports,
        'active_port': active_serial_port,
        'baud_rate': active_baud_rate,
        'baud_rates': [9600, 19200, 38400, 57600, 115200, 230400],
        'connection_status': 'Connected' if (ser and ser.is_open) else 'Disconnected',
        # شناسه‌ی نشست لینک — همان شناسه‌ای که به ESP8266 و از آنجا به
        # ESP32 می‌رسد؛ برای مطمئن شدن از اینکه هر سه با یک id پیش می‌روند
        'session_id': serial_session_id,
        'link_protocol': 'v2'
    })

@app.route('/api/set_serial_config', methods=['POST'])
def set_serial_config():
    global active_serial_port, active_baud_rate, serial_thread, manual_disconnect
    data = request.get_json()
    stop_event.set()
    if ser: ser.close()
    if serial_thread: serial_thread.join(timeout=2)
    active_serial_port = data.get('port')
    active_baud_rate = data.get('baud_rate', 115200)
    manual_disconnect = False
    save_serial_config()          # تنظیمات برای دفعه‌ی بعد ذخیره می‌شود

    stop_event.clear()
    serial_thread = threading.Thread(target=read_serial_worker, daemon=True)
    serial_thread.start()
    return jsonify({'status': 'success', 'port': active_serial_port,
                    'baud_rate': active_baud_rate})

@app.route('/api/close_serial_port', methods=['POST'])
def close_serial_port():
    global ser, active_serial_port, manual_disconnect
    try:
        manual_disconnect = True
        save_serial_config()
        if ser and ser.is_open: ser.close()
        active_serial_port = None
        return jsonify({'status': 'success'})
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/api/cycle_config')
def get_cycle_config():
    """مقادیر فعلی زمان‌بندی سیکل ESP32 (برای نمایش در فرم تنظیمات داشبورد)."""
    return jsonify({
        'cycle_period_ms': cycle_config['cycle_period_ms'],
        'relay_retry_gap_ms': cycle_config['relay_retry_gap_ms'],
        'min_cycle_period_ms': MIN_CYCLE_PERIOD_MS,
        'max_cycle_period_ms': MAX_CYCLE_PERIOD_MS,
        'min_relay_retry_gap_ms': MIN_RELAY_RETRY_GAP_MS,
        'max_relay_retry_gap_ms': MAX_RELAY_RETRY_GAP_MS,
        'serial_connected': bool(ser and ser.is_open),
    })


@app.route('/api/set_cycle_config', methods=['POST'])
def set_cycle_config():
    """
    تنظیم از‌راه‌دور CYCLE_PERIOD_MS / RELAY_RETRY_GAP_MS.

    مقدار معتبرسازی و در فایل محلی ذخیره می‌شود (تا بعد از ری‌استارت سرور
    هم بماند)، سپس بلافاصله روی سریال به سمت ESP8266 فرستاده می‌شود. چون
    این مسیر ACK ندارد، همین مقدار همراه هر ضربان SRV_READی بعدی (هر ۱۰
    ثانیه) هم دوباره فرستاده می‌شود تا حتی اگر ESP32 لحظه‌ی تنظیم آنلاین
    نبوده، به محض وصل‌شدن مقدار را بگیرد.
    """
    data = request.get_json(silent=True) or request.form
    try:
        cycle_ms = int(data.get('cycle_period_ms'))
        gap_ms = int(data.get('relay_retry_gap_ms'))
    except (TypeError, ValueError):
        return jsonify({'status': 'error', 'message': 'مقادیر باید عدد صحیح (میلی‌ثانیه) باشند'}), 400

    if not (MIN_CYCLE_PERIOD_MS <= cycle_ms <= MAX_CYCLE_PERIOD_MS):
        return jsonify({'status': 'error',
                        'message': f'CYCLE_PERIOD_MS باید بین {MIN_CYCLE_PERIOD_MS} و {MAX_CYCLE_PERIOD_MS} باشد'}), 400
    if not (MIN_RELAY_RETRY_GAP_MS <= gap_ms <= MAX_RELAY_RETRY_GAP_MS):
        return jsonify({'status': 'error',
                        'message': f'RELAY_RETRY_GAP_MS باید بین {MIN_RELAY_RETRY_GAP_MS} و {MAX_RELAY_RETRY_GAP_MS} باشد'}), 400

    cycle_config['cycle_period_ms'] = cycle_ms
    cycle_config['relay_retry_gap_ms'] = gap_ms
    save_cycle_config()

    sent = send_serial_line(build_cfg_line())
    print(f"[CFG] زمان‌بندی سیکل تنظیم شد: {cycle_config} (ارسال فوری روی سریال: {'موفق' if sent else 'پورت بسته/در صف ضربان بعدی'})")

    return jsonify({'status': 'success', 'cycle_config': cycle_config, 'sent_immediately': sent})


@app.route('/export_excel')
def export_excel():
    target_date = request.args.get('date')
    si = io.StringIO()
    cw = csv.writer(si)
    cw.writerow(['ID', 'NUM',
                 'BCM1_OPEN', 'BCM1_CLOSE', 'BCM1_OK',
                 'BCM2_OPEN', 'BCM2_CLOSE', 'BCM2_OK',
                 'Cycle', 'Attempts',
                 'Temp', 'Humidity', 'Time', 'Date', 'Timestamp'])
    
    query = MasterReading.query
    if target_date:
        query = query.filter(MasterReading.date == target_date)
    recs = query.order_by(*device_clock_order()).all()
    
    for r in recs:
        res = build_bcm_results(r.nbcm_selected)
        cycle_val = r.cycle if r.cycle is not None else (res['BCM1']['ok'] and res['BCM2']['ok'])
        cw.writerow([
            r.id, r.num_value,
            'OK' if res['BCM1']['open'] else 'NOK',
            'OK' if res['BCM1']['close'] else 'NOK',
            'OK' if res['BCM1']['ok'] else 'NOK',
            'OK' if res['BCM2']['open'] else 'NOK',
            'OK' if res['BCM2']['close'] else 'NOK',
            'OK' if res['BCM2']['ok'] else 'NOK',
            'OK' if cycle_val else 'NOK',
            r.cycle_attempt if r.cycle_attempt is not None else '',
            r.temp, r.humidity, r.time, r.date, r.timestamp])
    
    return Response(si.getvalue(), mimetype="text/csv", headers={"Content-Disposition": f"attachment; filename=report.csv"})

@app.route('/clear_history', methods=['POST'])
def clear_history():
    target_date = request.args.get('date')
    try:
        if target_date:
            MasterReading.query.filter(MasterReading.date == target_date).delete()
            db.session.commit()
            # باگ: قبلاً فقط Master پاک می‌شد و چون صفحه‌ی آرشیو اول از
            # دیتابیس روزانه می‌خواند، داده‌ها انگار اصلاً حذف نمی‌شدند.
            _daily = daily_db_file(target_date)
            if os.path.exists(_daily):
                try:
                    os.remove(_daily)
                    _daily_ready.discard(_daily)
                except Exception as rm_err:
                    print(f"[DB] حذف {_daily} ناموفق: {rm_err}")
            flash(f"داده‌های تاریخ {target_date} حذف شد.", "info")
        else:
            MasterReading.query.delete()
            db.session.commit()
            for _f in os.listdir(DAILY_DB_DIR):
                if re.fullmatch(r"\d{4}-\d{2}-\d{2}\.db", _f):
                    try:
                        os.remove(os.path.join(DAILY_DB_DIR, _f))
                    except Exception:
                        pass
            _daily_ready.clear()
            flash("کل دیتابیس پاکسازی شد.", "warning")
    except Exception as e:
        db.session.rollback()
        flash(f"خطا: {e}", "danger")
    return redirect(url_for('history', date=target_date))

@app.route('/plot_display')
def plot_display():
    return render_template('plot_display.html')

# تابع کمکی برای پارس کردن NBCM ها (اگر ندارید اضافه کنید)
def parse_nbcm(nbcm_str):
    status = {key: 'inactive' for key in RESULT_FIELDS}
    if nbcm_str:
        selected = nbcm_str.split(',')
        for item in selected:
            if item.strip() in status:
                status[item.strip()] = 'active'
    return status


def _ensure_master_unique_index():
    """
    ایندکس یکتا روی کلید (num, date, time) در دیتابیس اصلی.

    مثل دیتابیس روزانه، سطح دیتابیس هم در برابر رکورد تکراری مقاوم می‌شود؛
    اگر دو درخواست هم‌زمان از یک رکورد برسد، دیتابیس جلوی دوباره‌نویسی را
    می‌گیرد. اگر دیتابیس قدیمی رکورد تکراری داشته باشد، ساخته‌شدن ایندکس
    رد می‌شود و برنامه مثل قبل با چکِ نرمی ادامه می‌دهد.
    """
    try:
        from sqlalchemy import text
        with db.engine.begin() as conn:
            conn.execute(text(
                "CREATE UNIQUE INDEX IF NOT EXISTS uq_master_record "
                "ON master_reading (num_value, date, time)"))
        return True
    except Exception as exc:
        print(f"[DB] ایندکس یکتای master ساخته نشد (رکورد تکراری قدیمی؟): {exc}")
        return False


def _ensure_master_cycle_column():
    """
    مهاجرتِ ستون «Cycle» روی دیتابیس‌های قدیمی‌تر.

    db.create_all() فقط جدول‌های جدید را می‌سازد و ستون جدید را به جدول
    از قبل موجود اضافه نمی‌کند؛ پس اگر دیتابیس قدیمی باشد، این تابع با
    ALTER TABLE ستون cycle را اضافه می‌کند و مقدار آن را برای رکوردهای
    قدیمی از روی nbcm_selected محاسبه و پر می‌کند (backfill).
    """
    try:
        from sqlalchemy import text
        with db.engine.begin() as conn:
            cols = [row[1] for row in conn.execute(text("PRAGMA table_info(master_reading)"))]
            if "cycle" in cols:
                return True  # از قبل وجود دارد، کاری لازم نیست
            conn.execute(text("ALTER TABLE master_reading ADD COLUMN cycle BOOLEAN DEFAULT 0"))
        # Backfill: مقدار Cycle رکوردهای قدیمی را از روی nbcm_selected حساب کن
        updated = 0
        for row in MasterReading.query.all():
            row.cycle = compute_cycle_ok(row.nbcm_selected)
            updated += 1
        db.session.commit()
        print(f"[DB] ستون Cycle اضافه شد و برای {updated} رکورد قدیمی محاسبه شد.")
        return True
    except Exception as exc:
        print(f"[DB] افزودن ستون Cycle ناموفق بود: {exc}")
        db.session.rollback()
        return False


def _ensure_master_attempt_column():
    """
    مهاجرتِ ستون «cycle_attempt» (تعداد تلاش ۱/۲/۳) روی دیتابیس‌های قدیمی‌تر.

    برخلاف ستون Cycle، این مقدار از روی داده‌ی قدیمی قابل بازسازی نیست
    (چون فریمورهای قبلی اصلاً این عدد را نمی‌فرستادند)؛ پس رکوردهای قدیمی
    با NULL باقی می‌مانند و در UI به‌صورت «—» نمایش داده می‌شوند.
    """
    try:
        from sqlalchemy import text
        with db.engine.begin() as conn:
            cols = [row[1] for row in conn.execute(text("PRAGMA table_info(master_reading)"))]
            if "cycle_attempt" in cols:
                return True  # از قبل وجود دارد، کاری لازم نیست
            conn.execute(text("ALTER TABLE master_reading ADD COLUMN cycle_attempt INTEGER"))
        print("[DB] ستون cycle_attempt (تعداد تلاش) اضافه شد.")
        return True
    except Exception as exc:
        print(f"[DB] افزودن ستون cycle_attempt ناموفق بود: {exc}")
        db.session.rollback()
        return False


def bootstrap_server(recover_unsaved=True, start_serial=True):
    """
    راه‌اندازی مشترک بین «python app.py» و اپ دسکتاپ (desktop_app.py):
      1. ساخت جدول‌ها و ایندکس یکتا
      2. برگرداندن رکوردهای مانده در unsaved_records.log
      3. بازیابی تنظیمات سریال و شروع کارگرِ خواندن سریال
    """
    global serial_thread

    with app.app_context():
        db.create_all()
        _ensure_master_unique_index()
        _ensure_master_cycle_column()
        _ensure_master_attempt_column()

        # رکوردهایی که در اجرای قبلی ذخیره نشده بودند، برگردانده شوند
        if recover_unsaved and os.path.exists(UNSAVED_LOG):
            try:
                api_recover_unsaved()
            except Exception as exc:
                print(f"[DB] بازیابی خودکار ناموفق: {exc}")

    # زمان‌بندی سیکل مستقل از وضعیت سریال بارگذاری می‌شود تا /api/cycle_config
    # همیشه آخرین مقدار ذخیره‌شده را نشان دهد، حتی اگر سریال هنوز استارت نشده
    load_cycle_config()

    if start_serial:
        load_serial_config()      # آخرین پورت و باود انتخاب‌شده
        stop_event.clear()
        serial_thread = threading.Thread(target=read_serial_worker, daemon=True)
        serial_thread.start()



if __name__ == '__main__':
    bootstrap_server()

    # host='0.0.0.0' لازم است تا گوشی هم بتواند به سرور وصل شود؛
    # با مقدار پیش‌فرض (127.0.0.1) فقط از خود همان کامپیوتر در دسترس بود.
    print('[INIT] سرور روی http://0.0.0.0:5000 بالا آمد '
          '(از گوشی: http://<IP کامپیوتر>:5000)')

    # حالت Production: پایدارتر برای روشن ماندن طولانی‌مدت
    #   python app.py --prod        یا       RF_PROD=1 python app.py
    if '--prod' in sys.argv or os.environ.get('RF_PROD') == '1':
        try:
            from waitress import serve
            print('[INIT] حالت Production (waitress) — بدون دیباگر، پایدار')
            serve(app, host='0.0.0.0', port=5000, threads=8)
        except ImportError:
            print('[INIT] waitress نصب نیست؛ با سرور توسعه ادامه می‌دهیم')
            app.run(host='0.0.0.0', debug=False, port=5000, use_reloader=False)
    else:
        app.run(host='0.0.0.0', debug=True, port=5000, use_reloader=False)
