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
import datetime 
import io
import csv 
import pytz 
import serial
import threading
import time
import serial.tools.list_ports 
import os
import sqlite3
import math
import struct
import re

# =====================================================================
#  مسیرها مستقل از پوشه‌ای که برنامه از آن اجرا می‌شود
#  (باگ: اجرای app.py از ریشه‌ی ریپو با خطای TemplateNotFound: index.html
#   می‌خورد چون Flask دنبال ./templates کنار فایل می‌گردد)
# =====================================================================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


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


TEMPLATE_DIR = _resolve_templates()
STATIC_DIR = os.path.join(BASE_DIR, 'static')

# دیتابیس‌های روزانه همیشه کنار app.py ساخته می‌شوند، نه در پوشه‌ی جاری
DAILY_DB_DIR = BASE_DIR


def daily_db_file(date_str):
    return os.path.join(DAILY_DB_DIR, f"{date_str}.db")


app = Flask(__name__, template_folder=TEMPLATE_DIR, static_folder=STATIC_DIR)
print(f"[INIT] templates: {TEMPLATE_DIR}")
app.config['SECRET_KEY'] = 'industrial_secret_key_v3_7_live_fix' 
app.config['MAX_CONTENT_LENGTH'] = 1024 * 1024 * 1024
# --- تنظیمات زمانی و دیتابیس جامع ---
TEHRAN_TZ = pytz.timezone('Asia/Tehran')
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


UNSAVED_LOG = os.path.join(BASE_DIR, "unsaved_records.log")


def record_to_line(num_value, fields, temp, humidity, date_str, time_str):
    """ساخت همان خط استاندارد پروژه از روی مقادیر یک رکورد"""
    sel = set(normalize_fields((fields or "").split(",")))
    parts = [f"NUM={num_value}"]
    for name in RESULT_FIELDS:
        parts.append(f"{name}={'OK' if name in sel else 'NOK'}")
    parts.append(f"Temp={temp}")
    parts.append(f"Humidity={humidity}")
    parts.append(f"Date={date_str}")
    parts.append(f"Time={time_str}")
    return ",".join(parts)


def stash_unsaved(line, reason):
    """
    اگر به هر دلیلی نوشتن در دیتابیس شکست بخورد، خط خام در یک فایل متنی
    نگه داشته می‌شود تا هیچ داده‌ای از بین نرود. با /api/recover_unsaved
    دوباره وارد دیتابیس می‌شود.
    """
    try:
        with open(UNSAVED_LOG, "a", encoding="utf-8") as fh:
            fh.write(f"{datetime.datetime.now().isoformat()}\t{reason}\t{line}\n")
        print(f"[DB] رکورد در {os.path.basename(UNSAVED_LOG)} نگه داشته شد: {reason}")
    except Exception as exc:
        print(f"[DB] حتی ذخیره‌ی پشتیبان هم ناموفق بود: {exc}")


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

def save_sensor_data(data_source):
    global _last_cleanup_ts
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
        
        # --- بخش اصلاح شده: ساخت Timestamp واقعی از روی فایل ---
        try:
            # ترکیب تاریخ و ساعت فایل برای ساخت آبجکت زمان
            dt_str = f"{i_date} {i_time}"
            # تبدیل رشته به آبجکت زمان (Real Device Time)
            real_timestamp = datetime.datetime.strptime(dt_str, '%Y-%m-%d %H:%M:%S')
            
            # نکته: چون دیتابیس شما از نوع DateTime بدون تایم‌زون است، 
            # اگر نیاز به تایم‌زون دارید اینجا اضافه کنید. فعلاً Native نگه می‌داریم.
        except Exception:
            # در صورت خطا در فرمت، همان زمان آپلود را بگذار
            real_timestamp = now_tehran
        # -------------------------------------------------------

        # هرچه غیر از کانال‌های تعریف‌شده باشد کنار گذاشته می‌شود
        nbcm_checked_list = normalize_fields(nbcm_checked_list)
        nbcm_str = ",".join(nbcm_checked_list)
        log_str = f"NUM:{num_int}, H:{h_val}, T:{t_val}"

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
            timestamp=real_timestamp,  # <--- تغییر مهم: استفاده از زمان واقعی دستگاه
            formatted_log=log_str
        )
        db.session.add(master_entry)
        db.session.commit()

        # 2. ذخیره در Daily DB (با ایندکس یکتا و INSERT OR IGNORE)
        insert_daily_rows(i_date, [(num_int, nbcm_str, t_val, h_val,
                                    i_date, i_time, real_timestamp)])

        return True

    except Exception as e:
        db.session.rollback()
        print(f"[DB_ERROR] {e}")
        # تضمین: داده هرگز گم نمی‌شود
        try:
            stash_unsaved(record_to_line(num_int, nbcm_str, t_val, h_val, i_date, i_time),
                          f"db_error: {e}")
        except Exception:
            pass
        return False
    
# --- مدیریت سریال ---
ser = None 
serial_thread = None
stop_event = threading.Event()
active_serial_port = None 
active_baud_rate = 115200 
DEFAULT_PORT = "COM9"
manual_disconnect = False

# الگوی دقیق خطی که ESP8266 روی سریال می‌فرستد (۱۳ فیلد).
# استفاده از regex به‌جای split باعث می‌شود خطوط ناقص/به‌هم‌ریخته‌ی سریال
# (که موقع ریست برد یا نویز پیش می‌آید) اصلاً وارد دیتابیس نشوند.
INDUSTRIAL_LINE_RE = re.compile(
    r"NUM=(?P<num>-?\d+),"
    r"BCM1_OPEN=(?P<f1>[A-Za-z0-9]+),BCM1_CLOSE=(?P<f2>[A-Za-z0-9]+),"
    r"BCM2_OPEN=(?P<f3>[A-Za-z0-9]+),BCM2_CLOSE=(?P<f4>[A-Za-z0-9]+),"
    r"Temp=(?P<temp>-?\d+(?:\.\d+)?),Humidity=(?P<hum>-?\d+(?:\.\d+)?),"
    r"Date=(?P<y>\d{4})-(?P<mo>\d{1,2})-(?P<d>\d{1,2}),"
    r"Time=(?P<hh>\d{1,2}):(?P<mi>\d{1,2}):(?P<ss>\d{1,2})"
)

# فریمورهای قدیمی که هنوز NBCM1..4 می‌فرستند هم پذیرفته می‌شوند
LEGACY_LINE_RE = re.compile(
    r"NUM=(?P<num>-?\d+),"
    r"NBCM1=(?P<f1>[A-Za-z0-9]+),NBCM2=(?P<f2>[A-Za-z0-9]+),"
    r"NBCM3=(?P<f3>[A-Za-z0-9]+),NBCM4=(?P<f4>[A-Za-z0-9]+),"
    r"Temp=(?P<temp>-?\d+(?:\.\d+)?),Humidity=(?P<hum>-?\d+(?:\.\d+)?),"
    r"Date=(?P<y>\d{4})-(?P<mo>\d{1,2})-(?P<d>\d{1,2}),"
    r"Time=(?P<hh>\d{1,2}):(?P<mi>\d{1,2}):(?P<ss>\d{1,2})"
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
        return {
            'num_value': g["num"],
            'nbcm': nbcm,
            'temp': g["temp"],
            'humidity': g["hum"],
            'date': date_str,
            'time': time_str,
        }
    except Exception as exc:
        print(f"[PARSE_ERR] {exc} :: {line[:120]}")
        return None


def read_serial_worker():
    global ser, active_serial_port, manual_disconnect
    
    while not stop_event.is_set():
        try:
            if (ser is None or not ser.is_open) and not manual_disconnect:
                available = [p.device.upper() for p in serial.tools.list_ports.comports()]
                target_port = active_serial_port if active_serial_port else (DEFAULT_PORT if DEFAULT_PORT in available else None)
                
                if target_port and target_port in available:
                    active_serial_port = target_port
                    try:
                        ser = serial.Serial(active_serial_port, active_baud_rate, timeout=1)
                        print(f"[SERIAL] Connected to {active_serial_port}")
                    except Exception as e:
                        print(f"[SERIAL_ERR] {e}")
                        time.sleep(2)
                else:
                    time.sleep(5)
                    continue

            if ser and ser.is_open and ser.in_waiting > 0:
                try:
                    raw = ser.readline().decode('utf-8', errors='ignore').strip()
                    if not raw: continue
                    payload = parse_industrial_line(raw)
                    if payload:
                        print(f"[RX] NUM={payload['num_value']} {payload['date']} {payload['time']}")
                        with app.app_context():
                            save_sensor_data(payload)
                    elif "NUM=" in raw:
                        # خط شبیه دیتا بود ولی فرمتش کامل نبود -> دور ریخته می‌شود
                        print(f"[RX_BAD] {raw[:120]}")
                except Exception as read_err:
                    print(f"[READ_ERR] {read_err}")
            time.sleep(0.01)

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
        STRUCT_FORMAT = '<iff????iBBBBB'
        EXPECTED_SIZE = 25
        
        master_buffer = []
        daily_buffer_map = {} 
        
        success_count = 0
        fail_count = 0
        skipped_count = 0
        
        upload_time_server = datetime.datetime.now(TEHRAN_TZ)

        for file in uploaded_files:
            filename = secure_filename(file.filename)
            if not filename.lower().endswith('.dat'): continue
            
            try:
                file_bytes = file.read()
                if len(file_bytes) > 0 and len(file_bytes) % EXPECTED_SIZE == 0:
                    for i in range(0, len(file_bytes), EXPECTED_SIZE):
                        chunk = file_bytes[i : i + EXPECTED_SIZE]
                        data = struct.unpack(STRUCT_FORMAT, chunk)
                        
                        # --- استخراج و کستینگ (Fix: تبدیل اجباری به رشته) ---
                        raw_num = data[0]
                        raw_temp = data[1]
                        raw_hum = data[2]

                        # 1. هندل کردن مقدار NUM
                        num_val = raw_num

                        # 2. هندل کردن دما (Temp) - تبدیل دقیق Float به String
                        # بررسی خطای NaN (برای حافظه های SPI خالی)
                        if math.isnan(raw_temp) or math.isinf(raw_temp):
                            temp_str = "0"
                            temp_val_float = 0.0
                        else:
                            # محدود سازی (Clamp)
                            safe_temp = max(-100.0, min(155.0, raw_temp))
                            temp_val_float = round(safe_temp, 2)
                            temp_str = str(temp_val_float) # <--- فیکس اصلی اینجاست

                        # 3. هندل کردن رطوبت (Humidity)
                        if math.isnan(raw_hum) or math.isinf(raw_hum):
                            hum_str = "0"
                            hum_val_float = 0.0
                        else:
                            safe_hum = max(0.0, min(100.0, raw_hum))
                            hum_val_float = round(safe_hum, 2)
                            hum_str = str(hum_val_float) # <--- فیکس اصلی اینجاست
                        
                        # لاژیک NBCM
                        # چهار نتیجه‌ی تفکیکی: باز/بسته برای هر دستگاه
                        # چهار بولینِ ساختار باینری به‌ترتیب:
                        # BCM1_OPEN, BCM1_CLOSE, BCM2_OPEN, BCM2_CLOSE
                        nbcm_list = [RESULT_FIELDS[k] for k in range(4) if data[3 + k]]
                        nbcm_str = ",".join(nbcm_list)

                        # زمان سنسور
                        year, month, day = data[7], data[8], data[9]
                        hour, minute, second = data[10], data[11], data[12]
                        
                        device_date_str = f"{year}-{month:02d}-{day:02d}"
                        device_time_str = f"{hour:02d}:{minute:02d}:{second:02d}"
                        
                        try:
                            sensor_dt = datetime.datetime.strptime(f"{device_date_str} {device_time_str}", '%Y-%m-%d %H:%M:%S')
                        except:
                            sensor_dt = upload_time_server

                        formatted_log = f"NUM:{num_val}, H:{hum_str}, T:{temp_str}"

                        # ذخیره در Master (ارسال رشته به جای عدد)
                        master_obj = MasterReading(
                            num_value=num_val, 
                            nbcm_selected=nbcm_str,
                            humidity=hum_str, # اینجا قبلا Float بود که باعث خطا می‌شد
                            temp=temp_str,    # اینجا قبلا Float بود که باعث خطا می‌شد
                            time=device_time_str,   
                            date=device_date_str,   
                            # ناهم‌خوانی: قبلاً Master زمان آپلود سرور را ثبت
                            # می‌کرد ولی دیتابیس روزانه زمان RTC دستگاه را.
                            # مرجع زمان در کل سیستم، RTC دستگاه است.
                            timestamp=sensor_dt,
                            formatted_log=formatted_log
                        )
                        # رد کردن رکوردی که قبلاً ثبت شده (آپلود دوباره‌ی همان پوشه)
                        if master_exists(num_val, device_date_str, device_time_str):
                            skipped_count += 1
                            continue
                        master_buffer.append(master_obj)
                        
                        # ذخیره در Daily (ارسال رشته برای یکدستی)
                        if device_date_str not in daily_buffer_map:
                            daily_buffer_map[device_date_str] = []
                        
                        daily_buffer_map[device_date_str].append(
                            (num_val, nbcm_str, temp_str, hum_str, device_date_str, device_time_str, sensor_dt)
                        )
                        success_count += 1
                else: fail_count += 1
            except Exception as e:
                print(f"[Batch Err] {filename}: {e}")
                fail_count += 1

        if success_count > 0:
            try:
                db.session.bulk_save_objects(master_buffer)
                db.session.commit()
                
                for log_date, records in daily_buffer_map.items():
                    inserted = insert_daily_rows(log_date, records)
                    print(f"[UPLOAD] {log_date}: {inserted}/{len(records)} رکورد جدید")
            except Exception as e:
                db.session.rollback()
                return jsonify({'status': 'error', 'message': str(e)}), 500

        return jsonify({'status': 'success', 'processed': success_count,
                        'failed': fail_count, 'skipped_duplicates': skipped_count})
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
                           bcm_results=build_bcm_results)

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
                'bcm_results': build_bcm_results(r.nbcm_selected)
            })
            
        return jsonify(output)
        
    except Exception as e:
        print(f"[API Error] {e}")
        return jsonify([])

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
            continue
        if master_exists(safe_int(parsed['num_value']), parsed['date'], parsed['time']):
            duplicates += 1
            continue
        if save_sensor_data(parsed):
            saved += 1
        else:
            invalid += 1

    print(f"[INGEST] from {device}: saved={saved} dup={duplicates} "
          f"bad={invalid} ignored={ignored}")
    return jsonify({'status': 'success', 'saved': saved,
                    'duplicates': duplicates, 'invalid': invalid,
                    'ignored': ignored})


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
        out_path = os.path.join(BASE_DIR, f'backup_{stamp}.db')

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


@app.route('/api/serial_ports')
def list_serial_ports():
    ports = [port.device for port in serial.tools.list_ports.comports()]
    return jsonify({'available_ports': ports, 'active_port': active_serial_port, 'connection_status': 'Connected' if (ser and ser.is_open) else 'Disconnected'})

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
    # اگر از اجرای قبلی رکورد نجات‌یافته‌ای مانده، همین اول برش گردان
    if os.path.exists(UNSAVED_LOG):
        with app.app_context():
            try:
                with app.test_request_context():
                    api_recover_unsaved()
            except Exception as exc:
                print(f"[DB] بازیابی خودکار ناموفق: {exc}")

    stop_event.clear()
    serial_thread = threading.Thread(target=read_serial_worker, daemon=True)
    serial_thread.start()
    return jsonify({'status': 'success', 'port': active_serial_port})

@app.route('/api/close_serial_port', methods=['POST'])
def close_serial_port():
    global ser, active_serial_port, manual_disconnect
    try:
        manual_disconnect = True
        if ser and ser.is_open: ser.close()
        active_serial_port = None
        return jsonify({'status': 'success'})
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500

@app.route('/export_excel')
def export_excel():
    target_date = request.args.get('date')
    si = io.StringIO()
    cw = csv.writer(si)
    cw.writerow(['ID', 'NUM',
                 'BCM1_OPEN', 'BCM1_CLOSE', 'BCM1_OK',
                 'BCM2_OPEN', 'BCM2_CLOSE', 'BCM2_OK',
                 'Temp', 'Humidity', 'Time', 'Date', 'Timestamp'])
    
    query = MasterReading.query
    if target_date:
        query = query.filter(MasterReading.date == target_date)
    recs = query.order_by(*device_clock_order()).all()
    
    for r in recs:
        res = build_bcm_results(r.nbcm_selected)
        cw.writerow([
            r.id, r.num_value,
            'OK' if res['BCM1']['open'] else 'NOK',
            'OK' if res['BCM1']['close'] else 'NOK',
            'OK' if res['BCM1']['ok'] else 'NOK',
            'OK' if res['BCM2']['open'] else 'NOK',
            'OK' if res['BCM2']['close'] else 'NOK',
            'OK' if res['BCM2']['ok'] else 'NOK',
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


if __name__ == '__main__':
    with app.app_context():
        db.create_all() 
    
    stop_event.clear()
    serial_thread = threading.Thread(target=read_serial_worker, daemon=True)
    serial_thread.start()
    
    # host='0.0.0.0' لازم است تا گوشی هم بتواند به سرور وصل شود؛
    # با مقدار پیش‌فرض (127.0.0.1) فقط از خود همان کامپیوتر در دسترس بود.
    print('[INIT] سرور روی http://0.0.0.0:5000 بالا آمد '
          '(از گوشی: http://<IP کامپیوتر>:5000)')
    app.run(host='0.0.0.0', debug=True, port=5000, use_reloader=False)
