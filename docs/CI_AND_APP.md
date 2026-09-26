# CI و اپلیکیشن اندروید — RF_tester

## ۱) ورک‌فلوها

| فایل | چه کار می‌کند | چه وقت اجرا می‌شود |
|---|---|---|
| `.github/workflows/firmware-ci.yml` | کامپایل فریمور ESP32 و ESP8266 با `arduino-cli`، چک سینتکس و لینت `app.py`، و تست سازگاری پروتکل | هر push/PR به‌جز تغییرات `android/**` و `test/**` |
| `.github/workflows/android.yml` | بیلد، لینت و unit test اپ اندروید و آپلود APK دیباگ | هر push/PR روی `android/**` |

### firmware-ci
- ماتریس دو برد: `esp32:esp32:esp32` و `esp8266:esp8266:nodemcuv2`.
- کتابخانه‌های لازم ESP32 به‌صورت خودکار نصب می‌شوند: `Adafruit SHT31 Library`, `Adafruit BusIO`, `Adafruit Unified Sensor`.
- خروجی `.bin/.elf` به‌عنوان artifact با نام `firmware-<sketch>` آپلود می‌شود؛ می‌توانید همان را مستقیم فلش کنید.
- `.github/scripts/test_protocol.py` این زنجیره را قفل می‌کند:

```
ESP32 --snprintf--> ESP8266 --sscanf/printf--> app.py (parse_industrial_line)
```

اگر کلیدها (`NUM=,NBCM1..4=,Temp=,Humidity=,Date=,Time=`)، تعداد ۱۳ فیلد `sscanf`،
یا اندازه‌ی ۲۵ بایتی struct فایل‌های `.dat` (`<iff????iBBBBB`) به‌هم بخورد، CI قرمز می‌شود.
همچنین اگر `DEBUG_ENABLE` در ESP8266 روی `true` باشد، به‌عنوان **warning** در خلاصه‌ی اکشن نشان داده می‌شود
(چون اکوی خام سریال باعث ثبت چندباره‌ی رکورد در دیتابیس Flask می‌شود).

### android
از `gradlew` استفاده نمی‌کنیم تا `gradle-wrapper.jar` (فایل باینری) داخل ریپو نباشد؛
اکشن `gradle/actions/setup-gradle` نسخه‌ی Gradle 8.11.1 را نصب می‌کند و مستقیم `gradle` صدا زده می‌شود.
اگر خواستید wrapper داشته باشید: یک‌بار در لوکال `gradle wrapper --gradle-version 8.11.1` را داخل `android/` بزنید
و در ورک‌فلو `gradle` را با `./gradlew` عوض کنید.

## ۲) اپلیکیشن اندروید (`android/`)

اپ مینیمالی که با **حالت دیباگ/هات‌اسپات ESP32** حرف می‌زند:

1. گوشی را به اکسس‌پوینت ESP32 وصل کنید.
2. آی‌پی (پیش‌فرض `192.168.1.1`) و پورت (`80`) را بدهید.
3. دکمه‌ها دستور متنی `sync` یا `sync10` را روی TCP می‌فرستند.
4. پاسخ JSON دستگاه پارس و خوانا نمایش داده می‌شود.

فرمت رکورد دریافتی (خروجی `sendDataFile()` در فریمور):

```json
{"ID":42,"T":23.45,"H":51.20,"N1":1,"N2":0,"Time":"2026-01-05 13:04:09"}
```

ساختار کد:

- `EspProtocol.kt` — پارسر خالص Kotlin (بدون وابستگی اندرویدی) → در unit test تست می‌شود.
- `EspClient.kt` — سوکت TCP؛ چون فریمور سوکت را نمی‌بندد، با `SO_TIMEOUT` و تشخیص پایان آرایه (`]`) خواندن تمام می‌شود.
- `MainActivity.kt` — UI ساده با ViewBinding + coroutine روی `Dispatchers.IO`.
- `network_security_config.xml` — cleartext فقط برای `192.168.1.1` و `192.168.4.1` مجاز است، نه کل اینترنت.

بیلد لوکال:

```bash
cd android
gradle assembleDebug      # یا ./gradlew assembleDebug اگر wrapper ساختید
```

---

## ۳) فعال‌سازی CI (یک دستور)

توکن این سشن اجازه‌ی نوشتن در `.github/workflows/` را ندارد، بنابراین ورک‌فلوها
فعلاً در `ci/workflows/` قرار دارند. برای فعال کردنشان:

```bash
mkdir -p .github/workflows
git mv ci/workflows/*.yml .github/workflows/
git commit -m "Enable CI workflows" && git push
```

بعد از اولین اجرا، APK دیباگ در تب **Actions → Android CI → Artifacts → rftester-debug-apk**
قابل دانلود است و فایل‌های `.bin` فریمور در `firmware-*`.

## ۴) کلاینت دسکتاپ (`desktop/`)

بدون هیچ وابستگی بیرونی، فقط Python 3.8+ (کتابخانه‌ی استاندارد):

| فایل | توضیح |
|---|---|
| `rf_tester_gui.py` | اپ بومی Tkinter (جدول رکوردها، خروجی CSV، نمایش پاسخ خام) |
| `server.py` | همان قابلیت‌ها ولی با UI مرورگری؛ خودِ پایتون TCP می‌زند |
| `esp_protocol.py` | پارسر/کلاینت مشترک، هم‌ارز `EspProtocol.kt` |
| `simulator.py` | شبیه‌ساز ESP32 برای تست بدون سخت‌افزار |
| `run_desktop.bat` / `run_web.bat` / `run.sh` | اجرای سریع |

```bash
python3 desktop/rf_tester_gui.py             # اپ بومی
python3 desktop/server.py                    # نسخه‌ی مرورگری
python3 desktop/rf_tester_gui.py --simulate  # دمو بدون سخت‌افزار
```
