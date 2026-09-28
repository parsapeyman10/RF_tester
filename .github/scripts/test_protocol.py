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

KEYS = ["NUM", "NBCM1", "NBCM2", "NBCM3", "NBCM4",
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
sys.path.insert(0, ROOT)
import app as flask_app  # noqa: E402  (import بعد از تنظیم مسیر)

sample = ("NUM=42,NBCM1=OK,NBCM2=NOK,NBCM3=OK,NBCM4=NOK,"
          "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09")

payload = flask_app.parse_industrial_line(sample)
check(payload is not None, "parse_industrial_line خط استاندارد را پارس نکرد")
if payload:
    check(payload["num_value"] == "42", f"NUM اشتباه پارس شد: {payload['num_value']}")
    check(payload["nbcm"] == ["NBCM1", "NBCM3"], f"NBCM اشتباه: {payload['nbcm']}")
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
    check("phaseNeeded(got, phase)" in cycle_block,
          "در تلاش‌های بعدی فقط فازِ ناقص تکرار می‌شود")
    check("got[phase][d] = true" in cycle_block or "got[phase][d] = true" in phase_block,
          "نتیجه‌ی فیدبک بین تلاش‌ها حفظ می‌شود (تجمعی)")

check("const uint8_t RELAY_PINS[PHASE_COUNT] = { 2, 4 };" in esp32,
      "دو رله به‌عنوان دو فرمان تعریف شده‌اند")
check("{ 13, 16 }" in esp32 and "{ 15, 17 }" in esp32,
      "جدول فیدبک: هر فاز، یک پین برای هر BCM")
check("const uint8_t RELAY_MAX_ATTEMPTS = 3;" in esp32, "سقف تکرار سه بار است")

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
check("ESP.getFreeHeap()" in esp32, "[ESP32] پایش حافظه فعال است")

# --------------------------------- 8) هشدارهای آماده‌سازی برای Production
warn("#define DEBUG_ENABLE false" in esp8266,
     "[ESP8266] DEBUG_ENABLE روی true است: اکوی خام سریال باعث ثبت چندباره‌ی "
     "هر رکورد در دیتابیس Flask می‌شود. قبل از فلش نهایی false شود.")

warn("#define DEBUG_MODE 0" in esp32,
     "[ESP32] DEBUG_MODE روی 1 است (فقط برای دیباگ مناسب است).")

# --------------------------------------------------------------- گزارش
for w in warnings:
    print(f"::warning::{w}")

if failures:
    for f in failures:
        print(f"::error::{f}")
    print(f"\n{len(failures)} مورد ناسازگاری پروتکل پیدا شد.")
    sys.exit(1)

print("همه‌ی تست‌های سازگاری پروتکل پاس شد.")
