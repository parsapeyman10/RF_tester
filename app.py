# -*- coding: utf-8 -*-
"""
  Project: Flask WebServer Robust Industrial AP Receiver – Version 3.7 (Live Data Fix)
  Feature: Correct LATEST Data Retrieval, Strict Timestamp Sorting
  Engineer: [Peyman parsa] - Electronic Perspective
  
  Critical Fix:
  - Fixed '/api/sensor_data' to fetch the NEWEST 50 records (DESC sort -> Limit -> Reverse).
  - Ensured Dashboard always shows the absolute latest packet.
"""

from flask import Flask, render_template, request, redirect, url_for, flash, jsonify, Response
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

app = Flask(__name__)
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
    return f"{date_str}.db"

def cleanup_old_databases(days_to_keep=7):
    now = datetime.datetime.now(TEHRAN_TZ)
    for filename in os.listdir('.'):
        if filename.endswith('.db') and filename != 'master_industrial.db':
            try:
                date_part = filename.replace('.db', '')
                file_date = datetime.datetime.strptime(date_part, '%Y-%m-%d')
                file_date = TEHRAN_TZ.localize(file_date)
                if (now - file_date).days >= days_to_keep:
                    os.remove(filename)
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
    try:
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

        nbcm_str = ",".join(nbcm_checked_list)
        log_str = f"NUM:{num_int}, H:{h_val}, T:{t_val}"

        # 1. ذخیره در Master DB
        master_entry = MasterReading(
            num_value=num_int, nbcm_selected=nbcm_str,
            humidity=h_val, temp=t_val, time=i_time, date=i_date,
            timestamp=real_timestamp,  # <--- تغییر مهم: استفاده از زمان واقعی دستگاه
            formatted_log=log_str
        )
        db.session.add(master_entry)
        db.session.commit()

        # 2. ذخیره در Daily DB
        daily_db_path = f"{i_date}.db"
        
        with sqlite3.connect(daily_db_path) as conn:
            c = conn.cursor()
            c.execute('''CREATE TABLE IF NOT EXISTS daily_records (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        num_value INTEGER,
                        nbcm_selected TEXT,
                        temp TEXT,
                        humidity TEXT,
                        log_date TEXT,
                        log_time TEXT,
                        full_timestamp DATETIME
                    )''')

            c.execute("CREATE INDEX IF NOT EXISTS idx_date_time ON daily_records (log_date, log_time)")

            c.execute('''INSERT INTO daily_records 
                        (num_value, nbcm_selected, temp, humidity, log_date, log_time, full_timestamp)
                        VALUES (?, ?, ?, ?, ?, ?, ?)''',
                        (num_int, nbcm_str, t_val, h_val, i_date, i_time, real_timestamp)) # <--- تغییر مهم
            
            conn.commit()

        return True

    except Exception as e:
        db.session.rollback()
        print(f"[CRITICAL_DB_ERROR] {e}")
        return False
    
# --- مدیریت سریال ---
ser = None 
serial_thread = None
stop_event = threading.Event()
active_serial_port = None 
active_baud_rate = 115200 
DEFAULT_PORT = "COM9"
manual_disconnect = False

def parse_industrial_line(line):
    try:
        if "NUM=" in line:
            clean_line = line[line.find("NUM="):]
            parts = {p.split('=')[0].strip(): p.split('=')[1].strip() for p in clean_line.split(',') if '=' in p}
            return {
                'num_value': parts.get('NUM'),
                'nbcm': [f'NBCM{i}' for i in range(1, 5) if parts.get(f'NBCM{i}') == 'OK'],
                'temp': parts.get('Temp', '0'),
                'humidity': parts.get('Humidity', '0'),
                'date': parts.get('Date', 'N/A'),
                'time': parts.get('Time', 'N/A')
            }
    except: return None

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
                    print(f"[RX_RAW] {raw}")
                    if "NUM=" in raw: 
                        payload = parse_industrial_line(raw)
                        if payload:
                            with app.app_context(): save_sensor_data(payload)
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
                        nbcm_list = []
                        if data[3]: nbcm_list.append('NBCM1')
                        if data[4]: nbcm_list.append('NBCM2')
                        if data[5]: nbcm_list.append('NBCM3')
                        if data[6]: nbcm_list.append('NBCM4')
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
                            timestamp=upload_time_server,
                            formatted_log=formatted_log
                        )
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
                    daily_db_path = f"{log_date}.db"
                    with sqlite3.connect(daily_db_path) as conn:
                        c = conn.cursor()
                        c.execute('''CREATE TABLE IF NOT EXISTS daily_records (
                                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                                    num_value INTEGER,
                                    nbcm_selected TEXT,
                                    temp TEXT,
                                    humidity TEXT,
                                    log_date TEXT,
                                    log_time TEXT,
                                    full_timestamp DATETIME
                                )''')
                        c.executemany('''INSERT INTO daily_records 
                                    (num_value, nbcm_selected, temp, humidity, log_date, log_time, full_timestamp)
                                    VALUES (?, ?, ?, ?, ?, ?, ?)''', records)
                        conn.commit()
            except Exception as e:
                db.session.rollback()
                return jsonify({'status': 'error', 'message': str(e)}), 500

        return jsonify({'status': 'success', 'processed': success_count, 'failed': fail_count})
    return render_template('upload.html')

@app.route('/')
def home():
    # اصلاحیه صنعتی: دریافت آخرین رکورد بر اساس "زمان دستگاه"
    # سورت نزولی روی تاریخ و سپس ساعت
    last = MasterReading.query.order_by(
        MasterReading.date.desc(), 
        MasterReading.time.desc()
    ).first()
    
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
        daily_db_path = f"{target_date}.db"
        
        if os.path.exists(daily_db_path):
            try:
                label = f"آرشیو روزانه (Source: {target_date}.db)"
                
                # اتصال به دیتابیس روزانه
                with sqlite3.connect(daily_db_path) as conn:
                    c = conn.cursor()
                    
                    # دریافت داده‌ها با استفاده از ایندکس زمانی که قبلاً ساختیم
                    # ترتیب نزولی (DESC) یعنی جدیدترین داده‌ها اول نمایش داده شوند
                    c.execute("""
                        SELECT id, num_value, nbcm_selected, temp, humidity, full_timestamp 
                        FROM daily_records 
                        ORDER BY log_date DESC, log_time DESC
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
            readings = query.order_by(MasterReading.timestamp.desc()).all()
            
    else:
        # حالت پیش‌فرض: نمایش تمام داده‌ها از مستر
        readings = MasterReading.query.order_by(MasterReading.timestamp.desc()).all()

    return render_template('history.html', readings=readings, label=label, dates=available_dates, current_date=target_date)

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
        readings = MasterReading.query.order_by(
            MasterReading.date.desc(), 
            MasterReading.time.desc()
        ).limit(50).all()
        
        # گام 2: معکوس کردن لیست برای نمایش درست در نمودار (از چپ به راست: قدیم به جدید)
        readings = readings[::-1]
        
        output = []
        for r in readings:
            # پارس کردن وضعیت‌های NBCM برای روشن/خاموش کردن چراغ‌ها
            nbcm_map = {
                f"NBCM{i}": ("active" if r.nbcm_selected and f"NBCM{i}" in r.nbcm_selected else "notactive") 
                for i in range(1, 5)
            }
            
            output.append({
                'num_value': r.num_value, 
                'temp': r.temp, 
                'humidity': r.humidity,
                'time': r.time,       # زمان دستگاه
                'date': r.date,       # تاریخ دستگاه
                'timestamp': r.timestamp, # زمان آپلود (صرفا جهت اطلاع)
                'nbcm_statuses': nbcm_map
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
        readings = MasterReading.query.order_by(MasterReading.date.desc(), MasterReading.time.desc()).limit(1000).all()
        # چون limit دیتای آخر را می‌آورد، باید لیست را برعکس کنیم تا در نمودار از چپ به راست باشد
        readings = readings[::-1]
    else:
        # اگر فیلتر داشت، طبق فیلتر عمل کن
        if start_date:
            query = query.filter(MasterReading.date >= start_date)
        if end_date:
            query = query.filter(MasterReading.date <= end_date)
        readings = query.order_by(MasterReading.date.asc(), MasterReading.time.asc()).all()
    
    output = []
    for r in readings:
        # پارس کردن وضعیت‌های NBCM
        nbcm_map = {}
        for i in range(1, 5):
            key = f"NBCM{i}"
            # بررسی اینکه آیا در رشته ذخیره شده وجود دارد یا خیر
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
            'date': r.date
        })

    return jsonify(output)

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
    cw.writerow(['ID', 'NUM', 'NBCM', 'Temp', 'Humidity', 'Time', 'Date', 'Timestamp'])
    
    query = MasterReading.query
    if target_date:
        query = query.filter(MasterReading.date == target_date)
    recs = query.order_by(MasterReading.timestamp.desc()).all()
    
    for r in recs:
        cw.writerow([r.id, r.num_value, r.nbcm_selected, r.temp, r.humidity, r.time, r.date, r.timestamp])
    
    return Response(si.getvalue(), mimetype="text/csv", headers={"Content-Disposition": f"attachment; filename=report.csv"})

@app.route('/clear_history', methods=['POST'])
def clear_history():
    target_date = request.args.get('date')
    try:
        if target_date:
            MasterReading.query.filter(MasterReading.date == target_date).delete()
            flash(f"داده‌های تاریخ {target_date} حذف شد.", "info")
        else:
            MasterReading.query.delete()
            flash("کل دیتابیس پاکسازی شد.", "warning")
        db.session.commit()
    except Exception as e:
        db.session.rollback()
        flash(f"خطا: {e}", "danger")
    return redirect(url_for('history', date=target_date))

@app.route('/plot_display')
def plot_display():
    return render_template('plot_display.html')

@app.route('/api/sensor_data')
def get_sensor_data_api():
    try:
        # دریافت 50 داده آخر
        # نکته مهم: سورت باید بر اساس تاریخ و ساعت سنسور باشد، نه زمان آپلود
        # چون ممکن است فایل‌ها پس و پیش آپلود شوند
        readings = MasterReading.query.order_by(
            MasterReading.date.desc(), 
            MasterReading.time.desc()
        ).limit(50).all()
        
        # معکوس کردن لیست برای نمایش درست در نمودار (چپ به راست)
        readings = readings[::-1]
        
        data = []
        for r in readings:
            # --- فوت کوزه‌گری ---
            # ساختن زمان واقعی برای محور X از روی ستون‌های date و time
            # این همان چیزی است که پلاتر نیاز دارد
            real_sensor_time = f"{r.date} {r.time}"
            
            data.append({
                'id': r.id,
                'num_value': r.num_value,
                'temp': r.temp,
                'humidity': r.humidity,
                'timestamp': real_sensor_time, # ارسال زمان سنسور به جای زمان ثبت
                'created_at': r.timestamp,     # زمان ثبت (اگر جایی نیاز شد)
                'nbcm_statuses': parse_nbcm(r.nbcm_selected)
            })
            
        return jsonify(data)
    except Exception as e:
        print(f"API Error: {e}")
        return jsonify([])

# تابع کمکی برای پارس کردن NBCM ها (اگر ندارید اضافه کنید)
def parse_nbcm(nbcm_str):
    status = {'NBCM1': 'inactive', 'NBCM2': 'inactive', 'NBCM3': 'inactive', 'NBCM4': 'inactive'}
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
    
    app.run(debug=True, port=5000, use_reloader=False)