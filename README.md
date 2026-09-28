# RF_tester

سیستم تست خودکار درب‌های BCM با ESP32 + ESP8266 و سرور ثبت داده.

```
ESP32 (تست رله‌ها، SD، RTC) ──WiFi──► ESP8266 (گیرنده) ──USB/Serial──► app.py ──► SQLite
        │
        └──WiFi (حالت دیتا)──► گوشی / کلاینت دسکتاپ ──HTTP──► app.py
```

---

## کدام فایل‌ها را استفاده کنم؟

### ۱) روی برد ESP32 فلش می‌شود

```
esp32_controller_FIXED/esp32_controller_FIXED.ino
```

بردی که رله‌ها، سنسور SHT31، RTC و کارت SD به آن وصل است.
قبل از فلش، بلاک `USER CONFIG` بالای فایل را ببینید (پین‌ها، زمان‌بندی، شبکه).

### ۲) روی برد ESP8266 فلش می‌شود

```
esp8266_receiver_FIXED/esp8266_receiver_FIXED.ino
```

بردی که با USB به کامپیوتر وصل است و نقش اکسس‌پوینت گیرنده را دارد.

### ۳) روی کامپیوتر (سرور)

| فایل | نقش |
|---|---|
| `app.py` | خود سرور |
| `requirements.txt` | پیش‌نیازها |
| `run_server.bat` | **ویندوز: همین را دوبار کلیک کنید** |
| `run_server.sh` | لینوکس / مک |
| `templates/` | ۴ فایل HTML — بدون این پوشه سرور بالا نمی‌آید |

راهنما: [`docs/SERVER_SETUP.md`](docs/SERVER_SETUP.md)

### ۴) کلاینت کامپیوتر (اختیاری — برای حالت دیتا)

| فایل | نقش |
|---|---|
| `dist/RFTester.pyz` | بسته‌ی آماده، تک‌فایل: `python RFTester.pyz` |
| `desktop/` | سورس همان برنامه |

### ۵) اپ اندروید (اختیاری)

```
android/            ← پروژه‌ی Android Studio
```

برای ساخت APK بدون نصب Android Studio: ورک‌فلوی `ci/workflows/release.yml` را
فعال کنید (یک دستور، در [`docs/CI_AND_APP.md`](docs/CI_AND_APP.md)).

---

## چیزهایی که لازم ندارید

| مسیر | توضیح |
|---|---|
| `test/` | نسخه‌های قدیمی و دیتابیس‌های نمونه |
| `.github/scripts/` | تست‌های خودکار CI |
| `ci/workflows/` | فقط اگر می‌خواهید CI/ساخت APK فعال شود |
| `docs/` | مستندات |

---

## شروع سریع

1. `esp8266_receiver_FIXED.ino` را روی ESP8266 فلش کنید و با USB به کامپیوتر بزنید.
2. `esp32_controller_FIXED.ino` را روی ESP32 فلش کنید.
3. با گوشی به وای‌فای `SetClock` (رمز `12345678`) وصل شوید، `http://192.168.1.1`
   را باز کنید؛ ساعت خودکار ست می‌شود. «شروع کار عادی» را بزنید.
4. روی کامپیوتر `run_server.bat` را اجرا کنید و در داشبورد پورت COM را انتخاب کنید.
5. تمام — رکوردها هر ۲ دقیقه می‌آیند.

چک‌لیست کامل تست واقعی: [`docs/FIELD_TEST.md`](docs/FIELD_TEST.md)

---

## مستندات

| فایل | محتوا |
|---|---|
| [`docs/SERVER_SETUP.md`](docs/SERVER_SETUP.md) | راه‌اندازی سرور روی کامپیوتر |
| [`docs/FIELD_TEST.md`](docs/FIELD_TEST.md) | چک‌لیست تست روی سخت‌افزار |
| [`docs/DATA_MODE.md`](docs/DATA_MODE.md) | حالت دیتا، ترتیب رله‌ها، پایداری وای‌فای، هسته‌ها |
| [`docs/DATABASE_METHOD.md`](docs/DATABASE_METHOD.md) | ذخیره‌سازی SD و متد دیتابیس |
| [`docs/FIRMWARE_V2.md`](docs/FIRMWARE_V2.md) | تغییرات فریمور |
| [`docs/CI_AND_APP.md`](docs/CI_AND_APP.md) | فعال‌سازی CI و ساخت APK |
