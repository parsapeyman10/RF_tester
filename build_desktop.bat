@echo off
REM ==================================================================
REM  ساخت فایل اجرایی مستقل RF_Tester_HMI.exe (اپ دسکتاپ تک‌فایلی)
REM
REM  خروجی:  dist\RF_Tester_HMI.exe
REM  - بدون نیاز به نصب پایتون روی کامپیوتر مقصد
REM  - دیتابیس‌ها، لاگ‌ها و تنظیمات کنار خود exe ساخته می‌شوند
REM    (master_industrial.db داخل پوشه‌ی instance کنار exe)
REM
REM  نکته: pywebview روی ویندوز از WebView2 (اجینگ) استفاده می‌کند که
REM  روی ویندوز 10/11 به‌صورت پیش‌فرض نصب است.
REM ==================================================================
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] ساخت محیط مجازی...
    python -m venv .venv || goto :error
)

echo [2/3] نصب/بررسی ابزار ساخت (pyinstaller)...
".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
".venv\Scripts\python.exe" -m pip install -r requirements.txt pywebview pyinstaller || goto :error

echo [3/3] ساخت فایل اجرایی...
".venv\Scripts\pyinstaller.exe" --noconfirm --clean --onefile --windowed ^
    --name RF_Tester_HMI ^
    --add-data "templates;templates" ^
    --hidden-import serial.tools.list_ports ^
    --collect-submodules serial ^
    desktop_app.py || goto :error

echo.
echo [OK] ساخته شد: dist\RF_Tester_HMI.exe
echo      دیتابیس‌ها کنار خود exe ساخته می‌شوند.
pause
goto :eof

:error
echo.
echo خطا در ساخت. خروجی بالا را بررسی کنید.
pause
