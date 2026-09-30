#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
تست سازگاری پروتکل زنجیره‌ی داده:

    ESP32  --(TCP, snprintf)-->  ESP8266  --(Serial, printf)-->  Flask(app.py)
                                   ^ sscanf                        ^ parse_industrial_line

اگر یکی از این سه فرمت تغییر کند و بقیه به‌روز نشوند، سیستم بی‌صدا از کار
می‌افتد (ESP8266 هیچ‌وقت OK نمی‌فرستد -> فایل از SD پاک نمی‌شود، یا دیتا
هرگز وارد دیتابیس نمی‌شود). این اسکریپت دقیقاً همین را می‌گیرد.

همچنین اندازه‌ی باینری struct مورد استفاده در آپلود فایل‌های .dat
(STRUCT_FORMAT در app.py) با struct پک‌شده‌ی ESP32 مقایسه می‌شود.
"""

import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ESP32_INO = os.path.join(ROOT, "esp32_controller_FIXED", "esp32_controller_FIXED.ino")
ESP8266_INO = os.path.join(ROOT, "esp8266_receiver_FIXED", "esp8266_receiver_FIXED.ino")

KEYS = ["NUM", "BCM1_OPEN", "BCM1_CLOSE", "BCM2_OPEN", "BCM2_CLOSE",
        "Temp", "Humidity", "Date", "Time"]

failures = []
warnings = []


def read(path):
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        return f.read()


def check(cond, msg):
    if not cond:
        failures.append(msg)
    return cond


def warn(cond, msg):
    if not cond:
        warnings.append(msg)


esp32 = read(ESP32_INO)
esp8266 = read(ESP8266_INO)
app_src = read(os.path.join(ROOT, 'app.py'))

# ---------------------------------------------------------------- 1) کلیدها
for blob, name in ((esp32, "ESP32 snprintf"), (esp8266, "ESP8266")):
    for key in KEYS:
        check(key + "=" in blob, f"[{name}] کلید {key}= در فرمت پیدا نشد")

# ------------------------------------------------- 2) تعداد فیلدهای sscanf
m = re.search(r"itemsParsed\s*==\s*(\d+)", esp8266)
check(m is not None, "[ESP8266] شرط تعداد فیلدهای sscanf پیدا نشد")
if m:
    check(int(m.group(1)) == 13,
          f"[ESP8266] sscanf باید دقیقاً 13 فیلد بدهد ولی {m.group(1)} چک شده است")

# ------------------------------------ 3) پارس واقعی خط توسط سرور Flask
# دیتابیس تست: قبل از import جداسازی می‌شود تا رکوردهای تستِ داینامیک
# (بخش 5f) به دیتابیس واقعی پروژه راه پیدا نکنند
import tempfile  # noqa: E402
_TEST_DB_DIR = tempfile.mkdtemp(prefix="rf_protocol_test_")
os.environ["RF_MASTER_DB"] = os.path.join(_TEST_DB_DIR, "master.db")

sys.path.insert(0, ROOT)
import app as flask_app  # noqa: E402  (import بعد از تنظیم مسیر)

sample = ("NUM=42,BCM1_OPEN=OK,BCM1_CLOSE=NOK,BCM2_OPEN=OK,BCM2_CLOSE=NOK,"
          "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09")

payload = flask_app.parse_industrial_line(sample)
check(payload is not None, "parse_industrial_line خط استاندارد را پارس نکرد")
if payload:
    check(payload["num_value"] == "42", f"NUM اشتباه پارس شد: {payload['num_value']}")
    # چهار فیلد پروتکل = باز/بسته برای هر یک از دو دستگاه
    check(payload["nbcm"] == ["BCM1_OPEN", "BCM2_OPEN"], f"فیلدها اشتباه: {payload['nbcm']}")
    check(flask_app.DEVICES == ("BCM1", "BCM2"), "دو دستگاه تعریف شده است")
    check(len(flask_app.RESULT_MAP) == 4, "چهار نتیجه‌ی تفکیکی نگاشت شده است")
    check(flask_app.RESULT_FIELDS ==
          ("BCM1_OPEN", "BCM1_CLOSE", "BCM2_OPEN", "BCM2_CLOSE"),
          "نام فیلدها صریح است")
    _old = ("NUM=42,NBCM1=OK,NBCM2=NOK,NBCM3=OK,NBCM4=NOK,"
            "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09")
    check(flask_app.parse_industrial_line(_old) is not None,
          "خط فریمور قدیمی (NBCMx) هم هنوز پذیرفته می‌شود")
    check(payload["temp"] == "23.45", f"Temp اشتباه: {payload['temp']}")
    check(payload["humidity"] == "51.20", f"Humidity اشتباه: {payload['humidity']}")
    check(payload["date"] == "2026-01-05", f"Date اشتباه: {payload['date']}")
    check(payload["time"] == "13:04:09", f"Time اشتباه: {payload['time']}")

# خط خراب نباید چیزی برگرداند
check(flask_app.parse_industrial_line("[LOG]: ID:42 | T:23") is None,
      "خط بدون NUM= نباید پارس شود")

# ------------------------------------------- 4) اندازه‌ی struct فایل .dat
# ESP32: #pragma pack(1) struct { int; float; float; 4x bool; int; 5x uint8 }
expected_size = struct.calcsize("<iff????iBBBBB")
check(expected_size == 25, f"چیدمان struct پایتون 25 بایت نیست: {expected_size}")

m = re.search(r"EXPECTED_SIZE\s*=\s*(\d+)", read(os.path.join(ROOT, "app.py")))
check(m is not None, "EXPECTED_SIZE در app.py پیدا نشد")
if m:
    check(int(m.group(1)) == expected_size,
          f"EXPECTED_SIZE={m.group(1)} با struct پک‌شده‌ی ESP32 ({expected_size}) نمی‌خواند")
check("#pragma pack(1)" in esp32, "[ESP32] struct باید با pragma pack(1) پک شده باشد")

# ------------------------- 5) ساختار فریمور: فازها و مانیتورینگ
# ساختار درخواستی:
#   تحریک رله۱ -> مانیتورینگ هم‌زمان BCM1 و BCM2 -> قطع رله۱
#   تحریک رله۲ -> مانیتورینگ هم‌زمان BCM1 و BCM2 -> قطع رله۲
ph_start = esp32.find("static void runPhase(")
ph_end = esp32.find("static bool deviceDone(")
phase_block = esp32[ph_start:ph_end] if ph_start != -1 else ""
check(bool(phase_block), "تابع runPhase پیدا شد")
if phase_block:
    i_on = phase_block.find("digitalWrite(pin, HIGH)")
    i_begin = phase_block.find("beginFeedbackWindow()")
    i_end = phase_block.find("endFeedbackWindow()")
    i_off = phase_block.find("digitalWrite(pin, LOW)")
    check(-1 < i_on < i_begin < i_end < i_off,
          f"ترتیب رله/مانیتورینگ درست است (on={i_on} mon+={i_begin} mon-={i_end} off={i_off})")
    check("fbSeen[phase][d]" in phase_block,
          "در هر فاز، فیدبک هر دو دستگاه هم‌زمان خوانده می‌شود")

cy_start = esp32.find("static void runTestCycle(")
cy_end = esp32.find("void TaskRelayControl(void *pv) {")
cycle_block = esp32[cy_start:cy_end] if cy_start != -1 else ""
check(bool(cycle_block), "تابع runTestCycle پیدا شد")
if cycle_block:
    check("for (int phase = 0; phase < PHASE_COUNT; phase++)" in cycle_block,
          "هر تلاش هر دو فاز (رله۱ و رله۲) را اجرا می‌کند")
    check("if (allDone)" in cycle_block and "break;" in cycle_block,
          "به‌محض OK شدن هر دو دستگاه، تکرار متوقف می‌شود")
    check("phaseNeeded" not in cycle_block,
          "هیچ فازی رد نمی‌شود: در هر سیکل هر دو فرمان داده می‌شود")
    check("got[phase][d] = true" in cycle_block or "got[phase][d] = true" in phase_block,
          "نتیجه‌ی فیدبک بین تلاش‌ها حفظ می‌شود (تجمعی)")

check("const uint8_t RELAY_PINS[PHASE_COUNT] = { 2, 4 };" in esp32,
      "دو رله به‌عنوان دو فرمان تعریف شده‌اند")
check("{ 13, 16 }" in esp32 and "{ 15, 17 }" in esp32,
      "جدول فیدبک: هر فاز، یک پین برای هر BCM")
check("const uint8_t RELAY_MAX_ATTEMPTS = 3;" in esp32, "سیکل حداکثر سه بار تکرار می‌شود")

# ------------------------- 5b) پایداری وای‌فای
check("WiFi.setSleep(false)" in esp32, "[ESP32] Modem-Sleep خاموش است")
check("WiFi.setAutoReconnect(true)" in esp32, "[ESP32] اتصال مجدد خودکار فعال است")
check("channel, bssid, false)" in esp32,
      "[ESP32] اتصال با کانال و BSSID دقیق و به‌صورت دومرحله‌ای انجام می‌شود")
check("conf.sta.pmf_cfg.capable = pmfCapable" in esp32 and
      "conf.sta.pmf_cfg.required = false" in esp32,
      "[ESP32] PMF کنترل‌شده است (پیش‌فرض خاموش = رفع 4WAY_HANDSHAKE_TIMEOUT)")
check("esp_wifi_connect()" in esp32, "[ESP32] اتصال بعد از اعمال تنظیمات شروع می‌شود")
check("bool pmfCapable = (failStreak == 4 || failStreak == 5)" in esp32,
      "[ESP32] نردبان تلاش: اگر PMF خاموش جواب نداد، روشن هم امتحان می‌شود")
check("rssi <= -75" in esp32, "[ESP32] کیفیت سیگنال بعد از اتصال گزارش می‌شود")
check("راهنمای عیب‌یابی اتصال" in esp32,
      "[ESP32] بعد از ۱۰ شکست راهنمای کامل چاپ می‌شود")
check("const bool FORCE_MAX_TX_POWER = true" in esp32,
      "[ESP32] توان فرستنده روی حداکثر است (بیشترین برد)")
check("AP_TX_POWER = 20.5" in esp8266, "[ESP8266] توان اکسس‌پوینت روی حداکثر است")
check("ESP_ERR_WIFI_STOP_STATE" in esp32,
      "[ESP32] گذار AP->STA کنترل‌شده است (رفع خطای netstack cb reg)")
check("WIFI_BACKOFF_MIN_MS" in esp32 and "WIFI_BACKOFF_MAX_MS" in esp32,
      "[ESP32] فاصله‌ی تلاش‌ها نمایی است (بدون شخم زدن شبکه)")
check("struct LinkStats" in esp32, "[ESP32] آمار لینک نگه داشته می‌شود")
check("WiFi.onEvent(onWiFiEvent)" in esp32, "[ESP32] رویدادهای وای‌فای لاگ می‌شوند")
check("uploadClient.connected()" in esp32, "[ESP32] اتصال TCP بین رکوردها باز می‌ماند")
check("void wifiService(" in esp32,
      "[ESP32] کل منطق لینک در یک سرویس واحد جمع شده")
# همه‌ی اتصال‌های داده باید داخل همان یک تابع باشند؛ بیرونش فقط اتصال NTP مجاز است
_body = esp32.split("static bool wifiConnectOnce() {")[1].split("\nvoid ")[0]
_outside = esp32.count("WiFi.begin(") - _body.count("WiFi.begin(")
check(_outside <= 1,
      f"[ESP32] اتصال داده فقط در یک تابع انجام می‌شود (بیرون: {_outside})")
check("HAMMER" not in esp32, "[ESP32] کد چکش‌کاری (بازمانده‌ی متد قدیمی) حذف شده")
check("staticIP" not in esp32, "[ESP32] مسیر IP ثابت حذف شده و فقط DHCP می‌ماند")
check("portMAX_DELAY);" not in esp32.split("BIT_WIFI_PERMIT,")[1][:120],
      "[ESP32] تسک شبکه دیگر تا پایان سیکل بلاک نمی‌شود")
check('uploadClient.println("PING")' in esp32, "[ESP32] keep-alive ارسال می‌شود")
check('strcmp(rxBuffer, "PING")' in esp8266 and 'println("PONG")' in esp8266,
      "[ESP8266] به PING پاسخ PONG می‌دهد و آن را خطای فرمت نمی‌شمارد")
check("wifiDropCount" in esp32, "[ESP32] تعداد قطعی‌ها شمرده و گزارش می‌شود")
check("WiFi.setSleepMode(WIFI_NONE_SLEEP)" in esp8266, "[ESP8266] حالت خواب AP خاموش است")
check("AP_CHANNEL" in esp8266, "[ESP8266] کانال AP ثابت است")
check("CLIENT_IDLE_TIMEOUT_MS = 60000" in esp8266,
      "[ESP8266] تایم‌اوت بی‌کاری از ۱۰ به ۶۰ ثانیه رفته")

# ------------------------- 5d) سطح صنعتی سمت گیرنده
check("#define STATUS_ENABLE" in esp8266,
      "[ESP8266] خروجی وضعیت مستقل از حالت دیباگ است")
check("struct ReceiverStats" in esp8266 and "stats.linesOk" in esp8266,
      "[ESP8266] آمار کارکرد نگه داشته می‌شود")
check("onSoftAPModeStationConnected" in esp8266 and "onSoftAPModeStationDisconnected" in esp8266,
      "[ESP8266] پیوستن و جدا شدن کلاینت رویدادی گزارش می‌شود")
check("LOW_HEAP_LIMIT" in esp8266 and "ESP.restart()" in esp8266,
      "[ESP8266] محافظ حافظه‌ی بحرانی دارد")
check("FW_VERSION" in esp8266 and "FW_VERSION" in esp32,
      "[هر دو برد] نسخه‌ی فریمور در بوت چاپ می‌شود")
check("stats.apRestarts" in esp8266, "[ESP8266] بازسازی‌های AP شمرده می‌شود")

# ------------------------- 5c) فایل‌های راه‌اندازی سرور
import os as _os
_root = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
for _f in ("requirements.txt", "run_server.bat", "run_server.sh",
           "templates/index.html", "templates/history.html",
           "templates/plot_display.html", "templates/upload.html"):
    check(_os.path.isfile(_os.path.join(_root, _f)), f"فایل لازم سرور موجود است: {_f}")

# ------------------------- 5e) ترتیب فیلدها در هر سه لایه یکی باشد
def field_order(text, keys):
    """ترتیب ظهور کلیدها در یک رشته"""
    found = [(text.find(k), k) for k in keys if text.find(k) != -1]
    return [k for _, k in sorted(found)]

ORDER = ["NUM=", "BCM1_OPEN=", "BCM1_CLOSE=", "BCM2_OPEN=", "BCM2_CLOSE=",
         "Temp=", "Humidity=", "Date=", "Time="]

# لایه ۱: snprintf فریمور ESP32
# [2] یعنی بعد از «تعریف» تابع، نه پروتوتایپ
_esp32_fmt = esp32.split("void formatRecordLine(")[2][:900]
check(field_order(_esp32_fmt, ORDER) == ORDER,
      f"[ESP32] ترتیب فیلدهای خروجی درست است ({field_order(_esp32_fmt, ORDER)})")

# لایه ۲: sscanf و printf گیرنده
_rx_parse = esp8266.split("int itemsParsed = sscanf(")[1][:600]
check(field_order(_rx_parse, ORDER) == ORDER,
      f"[ESP8266] ترتیب فیلدهای ورودی درست است ({field_order(_rx_parse, ORDER)})")
_rx_out = esp8266.split("void sendDataToComputer() {")[1][:800]
check(field_order(_rx_out, ORDER) == ORDER,
      "[ESP8266] ترتیب فیلدهای خروجی سریال درست است")

# لایه ۳: رجکس سرور
_srv_re = app_src.split("INDUSTRIAL_LINE_RE = re.compile(")[1][:700]
check(field_order(_srv_re, ORDER) == ORDER,
      f"[app.py] ترتیب فیلدها در رجکس درست است ({field_order(_srv_re, ORDER)})")

# و در نهایت: یک خط واقعی با همان ترتیب باید پارس شود
_line = ("NUM=9,BCM1_OPEN=OK,BCM1_CLOSE=NOK,BCM2_OPEN=NOK,BCM2_CLOSE=OK,"
         "Temp=21.00,Humidity=55.50,Date=2026-09-29,Time=08:30:00")
_p = flask_app.parse_industrial_line(_line)
check(_p is not None and _p["nbcm"] == ["BCM1_OPEN", "BCM2_CLOSE"],
      f"خط نمونه با همان ترتیب درست پارس شد ({_p['nbcm'] if _p else None})")

# ------------------------- 6) فرمت واحد دیتا برای همه‌ی مقصدها
check("void formatRecordLine(" in esp32, "[ESP32] تابع واحد formatRecordLine تعریف شده")
check(esp32.count("formatRecordLine(") >= 3,
      "[ESP32] هم مسیر آپلود و هم حالت دیتا از همان فرمت‌کننده استفاده می‌کنند")
check(esp32.count('snprintf(buf, sizeof(buf),\n                           "NUM=') == 0,
      "[ESP32] نسخه‌ی تکراری فرمت NUM= باقی نمانده")
for cmd in ('"sync"', '"sync10"', '"syncall"', '"info"'):
    check(cmd in esp32, f"[ESP32] دستور {cmd} در حالت دیتا پشتیبانی می‌شود")
check('remote.println("END")' in esp32, "[ESP32] پاسخ با خط END بسته می‌شود")

# ------------------------- 7) تنظیم هسته‌ها و ناظر پایداری
check('xTaskCreatePinnedToCore(TaskInternalWiFiConnection, "WiFiConn", 10240, NULL, 2, NULL, 0)' in esp32,
      "[ESP32] تسک شبکه روی هسته‌ی ۰ پین شده")
check('xTaskCreatePinnedToCore(TaskDigitalRead, "DigiRead", 4096, NULL, 6, NULL, 1)' in esp32,
      "[ESP32] تسک خواندن فیدبک روی هسته‌ی ۱ با بالاترین اولویت")
check("void TaskHealthMonitor(" in esp32, "[ESP32] ناظر سلامت (ران‌تایم دائمی) اضافه شده")

# ------------------------- ذخیره‌سازی دو سطحی (SD و حافظه‌ی داخلی)
check("#include <LittleFS.h>" in esp32, "[ESP32] حافظه‌ی داخلی به‌عنوان جایگزین اضافه شده")
check("fs::FS *gFs" in esp32, "[ESP32] همه‌ی کار با فایل از یک لایه رد می‌شود")
check("bool initStorage()" in esp32, "[ESP32] انتخاب حافظه در بوت انجام می‌شود")
check("void probeSdCard()" in esp32, "[ESP32] بعد از جا زدن کارت، خودکار تشخیص داده می‌شود")
check("copyFileToSd" in esp32, "[ESP32] داده‌های حافظه‌ی داخلی به کارت منتقل می‌شوند")
check(esp32.count("SD.open(") <= 1,
      "[ESP32] دسترسی مستقیم به SD باقی نمانده (همه از gFs رد می‌شوند)")
check("mountInternalFs()" in esp32.split("void saveRecord(")[2][:1800],
      "[ESP32] اگر کارت وسط کار قطع شود، روی حافظه‌ی داخلی ادامه می‌دهد")

# ------------------------- پاک‌سازی کارت حافظه
check("EraseResult eraseStorage()" in esp32, "[ESP32] تابع پاک‌سازی کارت وجود دارد")
check('setupServer.on("/formatsd"' in esp32, "[ESP32] پاک‌سازی از پورتال در دسترس است")
check('cmd.equalsIgnoreCase("format CONFIRM")' in esp32,
      "[ESP32] دستور پاک‌سازی در حالت دیتا نیاز به تأیید دارد")
check('buf.equalsIgnoreCase("FORMAT SD") || buf.equalsIgnoreCase("FORMAT")' in esp32, "[ESP32] فرمان سریال FORMAT / FORMAT SD وجود دارد")
check("xSemaphoreTake(xSDMutex" in esp32.split("EraseResult eraseStorage() {")[1][:600],
      "[ESP32] پاک‌سازی با قفل SD انجام می‌شود (بدون تداخل با نوشتن)")
check("ESP.getFreeHeap()" in esp32, "[ESP32] پایش حافظه فعال است")

# --------------------------------- 8) هشدارهای آماده‌سازی برای Production
warn("#define DEBUG_ENABLE false" in esp8266,
     "[ESP8266] DEBUG_ENABLE روی true است: اکوی خام سریال باعث ثبت چندباره‌ی "
     "هر رکورد در دیتابیس Flask می‌شود. قبل از فلش نهایی false شود.")

warn("#define DEBUG_MODE 0" in esp32,
     "[ESP32] DEBUG_MODE روی 1 است (فقط برای دیباگ مناسب است).")

# ------------------------- 5f) پروتکل لینک v2 — انتقال مطمئن سه‌مرحله‌ای
# زنجیره: ESP32 --TCP--> ESP8266 --Serial--> سرور (Flask)
# چهار قاعده‌ی زنجیره:
#   ۱) سرور تا پورت سریال باز نشود هیچ درخواست/اعلامی نمی‌دهد
#   ۲) شناسه‌ی نشست (sid) یکسان در هر سه لایه حرکت می‌کند
#   ۳) وقتی شرایط اوکی شد، ارسال با سرعت کامل (پنجره‌ی ۸تایی)
#   ۴) روی هر مشکل، ارسال تکرار تا موفق — هیچ data loss

# --- لایه‌ی سرور (app.py) ---
check("def make_session_id(" in app_src, "[app.py] سازنده‌ی شناسه‌ی نشست وجود دارد")
check("def send_serial_line(" in app_src, "[app.py] ارسال خط پروتکل با قفل نوشتن سریال")
check("SRV_READY_PERIOD = 10.0" in app_src, "[app.py] ضربان SRV_READY هر ۱۰ ثانیه")
check('send_serial_line(f"SRV_READY {serial_session_id}")' in app_src,
      "[app.py] بعد از باز شدن پورت، SRV_READY با شناسه‌ی نشست اعلام می‌شود")
check("serial_session_id = make_session_id()" in app_src,
      "[app.py] هر بار باز شدن پورت، نشست تازه ساخته می‌شود")
check("serial_session_id = None" in app_src,
      "[app.py] با بسته شدن پورت، نشست باطل می‌شود")
_srv_handler = app_src.split("def handle_serial_line(")[1][:2600]
check("SRV_HELLO" in _srv_handler and "SRV_READY" in _srv_handler,
      "[app.py] به سلام بوت گیرنده (SRV_HELLO) با SRV_READY پاسخ داده می‌شود")
check("SRV_PING" in _srv_handler and "SRV_PONG" in _srv_handler,
      "[app.py] به ضربان گیرنده (SRV_PING) با SRV_PONG پاسخ داده می‌شود")
check('send_serial_line(f"ACK {payload[\'num_value\']}")' in _srv_handler,
      "[app.py] ACK فقط بعد از ثبت واقعی (یا تکراری بودن) رکورد برمی‌گردد")
check("ok = save_sensor_data(payload, raw_line=line)" in _srv_handler,
      "[app.py] نتیجه‌ی ذخیره چک می‌شود؛ خطای DB یعنی بدون ACK (برای retry)")
check("'session_id': serial_session_id" in app_src,
      "[app.py] شناسه‌ی نشست به API وضعیت سریال اضافه شده")

# --- لایه‌ی گیرنده (ESP8266) ---
check("void handleServerLine(" in esp8266, "[ESP8266] خطوط سریال سرور پردازش می‌شوند")
check("void serviceServerLink(" in esp8266, "[ESP8266] سرویس دوره‌ای لینک سریال")
check('Serial.println("SRV_HELLO")' in esp8266,
      "[ESP8266] موقع بوت خودش را به سرور معرفی می‌کند")
check('strncmp(line, "SRV_READY", 9)' in esp8266,
      "[ESP8266] اعلام آمادگی سرور (SRV_READY) خوانده می‌شود")
check('currentClient.printf("ACK %d\\n", num)' in esp8266,
      "[ESP8266] تایید واقعی سرور عیناً به ESP32 فوروارد می‌شود")
check('currentClient.println("OK")' not in esp8266,
      "[ESP8266] پاسخ OK قبلی حذف شده (تاییدِ قبل از ثبت = منبع گم شدن داده)")
_rx_fwd = esp8266.split("else if (parseData(rxBuffer))")[1][:1400]
check("if (!serverReady)" in _rx_fwd and "WAIT" in _rx_fwd and "sendDataToComputer()" in _rx_fwd,
      "[ESP8266] رکورد فقط با سرورِ آماده به کامپیوتر می‌رود؛ وگرنه WAIT")
check('currentClient.printf("READY %s\\n", srvSid)' in esp8266,
      "[ESP8266] آمادگی با همان شناسه‌ی نشست سرور به ESP32 اعلام می‌شود")
check('currentClient.println("SRV_LOST")' in esp8266,
      "[ESP8266] قطع شدن ضربان سرور به ESP32 اعلام می‌شود (SRV_LOST)")
check('Serial.println("SRV_PING")' in esp8266,
      "[ESP8266] وقتی سرور آماده نیست خودش یادآوری می‌کند (SRV_PING)")

# --- لایه‌ی فرستنده (ESP32) ---
check("const uint8_t  ACK_WINDOW      = 8;" in esp32,
      "[ESP32] پنجره‌ی ارسال ۸ رکوردی تعریف شده (سرعت کامل)")
check("int serviceIncomingLink(bool windowActive)" in esp32,
      "[ESP32] خطوط ورودی گیرنده (ACK/READY/WAIT/SRV_LOST) پردازش می‌شوند")
check('uploadClient.println("HELLO")' in esp32,
      "[ESP32] قبل از هر پنجره، آمادگی سرور با HELLO پرسیده می‌شود")
check('strncmp(line, "ACK ", 4)' in esp32,
      "[ESP32] تایید واقعی سرور (ACK <num>) تشخیص داده می‌شود")
check('indexOf("OK")' not in esp32,
      '[ESP32] چک قدیمی indexOf("OK") حذف شده — ACK واقعی جایش را گرفت')
check("void processLinkLine(" in esp32,
      "[ESP32] هر خط لینک جداگانه پردازش می‌شود (بدون پارس نیمه‌خط)")
_up = esp32.split("case MODE_CLIENT_UPLOAD:")[1].split("case MODE_HOTSPOT_VIEW:")[0]
check("winAcked" in _up and "writeUploadPos(dayFile, pos)" in _up,
      "[ESP32] آفست فقط به اندازه‌ی پیشوند پیاپیِ ACK شده جلو می‌رود")
check("linkServerReady = false;" in _up and "HELLO_WAIT_MS" in _up,
      "[ESP32] هر دور آپلود با گیت تازه‌ی READY شروع می‌شود")
check("uploadClient.stop()" in _up,
      "[ESP32] اگر هیچ ACK نیامد سوکت مشکوک بسته و پنجره دوباره می‌رود")
check("WAIT_BACKOFF_MS" in esp32,
      "[ESP32] عقب‌نشینی کنترل‌شده وقتی سرور آماده نیست")

# --- تست داینامیک: رفتار واقعی سرور بدون سخت‌افزار ---
# send_serial_line قلاب می‌شود تا دقیقاً ببینیم چه خطوطی به گیرنده می‌رود
with flask_app.app.app_context():
    flask_app.db.create_all()
    flask_app._ensure_master_unique_index()

_sid = flask_app.make_session_id()
check(re.fullmatch(r"S[0-9A-F]{6}", _sid) is not None,
      f"[app.py/داینامیک] قالب شناسه‌ی نشست درست است ({_sid})")

_sent = []
_orig_send = flask_app.send_serial_line
_orig_unsaved = flask_app.UNSAVED_LOG
flask_app.send_serial_line = lambda line: (_sent.append(line), True)[1]
flask_app.UNSAVED_LOG = os.path.join(_TEST_DB_DIR, "unsaved_test.log")
flask_app.serial_session_id = "STEST01"
try:
    # ۱) رکورد تازه: باید دقیقاً «ACK <num>» برگردد
    _rec = ("NUM=99001,BCM1_OPEN=OK,BCM1_CLOSE=NOK,BCM2_OPEN=OK,BCM2_CLOSE=NOK,"
            "Temp=23.45,Humidity=51.20,Date=2026-09-30,Time=10:11:12")
    _sent.clear()
    _ok_new = flask_app.handle_serial_line(_rec)
    check(_ok_new is True and _sent == ["ACK 99001"],
          f"[app.py/داینامیک] رکورد تازه باید «ACK 99001» بگیرد (نتیجه={_ok_new}، ارسالی={_sent})")

    # ۲) ارسال مجدد همان رکورد (ACK گم شده): تکراری است ولی باز ACK می‌گیرد
    #    تا retry فرستنده تمام شود و داده دوبله هم ثبت نشود
    _sent.clear()
    _ok_dup = flask_app.handle_serial_line(_rec)
    check(_ok_dup is True and _sent == ["ACK 99001"],
          f"[app.py/داینامیک] رکورد تکراری هم ACK می‌گیرد (نتیجه={_ok_dup}، ارسالی={_sent})")

    # ۳) سلام و پینگ گیرنده با شناسه‌ی نشست فعلی
    _sent.clear()
    flask_app.handle_serial_line("SRV_HELLO")
    check(_sent == ["SRV_READY STEST01"],
          f"[app.py/داینامیک] SRV_HELLO باید «SRV_READY STEST01» بگیرد ({_sent})")
    _sent.clear()
    flask_app.handle_serial_line("SRV_PING")
    check(_sent == ["SRV_PONG STEST01"],
          f"[app.py/داینامیک] SRV_PING باید «SRV_PONG STEST01» بگیرد ({_sent})")

    # ۴) خط خراب: ACK نمی‌گیرد تا فرستنده دوباره بفرستد
    _sent.clear()
    _ok_bad = flask_app.handle_serial_line("NUM=99002,Temp=xx,Humidity=yy,Date=2026-09-30,Time=10:11:12")
    check(_ok_bad is False and len(_sent) == 0,
          f"[app.py/داینامیک] خط خراب نباید ACK بگیرد (نتیجه={_ok_bad}، ارسالی={_sent})")
finally:
    flask_app.send_serial_line = _orig_send
    flask_app.UNSAVED_LOG = _orig_unsaved

import shutil  # noqa: E402
shutil.rmtree(_TEST_DB_DIR, ignore_errors=True)

# --------------------------------------------------------------- گزارش
for w in warnings:
    print(f"::warning::{w}")

if failures:
    for f in failures:
        print(f"::error::{f}")
    print(f"\n{len(failures)} مورد ناسازگاری پروتکل پیدا شد.")
    sys.exit(1)

print("همه‌ی تست‌های سازگاری پروتکل پاس شد.")
