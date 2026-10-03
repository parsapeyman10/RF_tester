@echo off
REM ==================================================================
REM  اجرای سرور RF_tester به‌صورت اپ دسکتاپ (پنجره‌ی مستقل، بدون مرورگر)
REM  بار اول محیط مجازی می‌سازد و پیش‌نیازها + pywebview را نصب می‌کند.
REM
REM  برای ساخت فایل مستقل exe (بدون نیاز به پایتون):  build_desktop.bat
REM ==================================================================
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] ساخت محیط مجازی...
    python -m venv .venv || goto :error
)

echo [2/3] نصب/بررسی پیش‌نیازها...
".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
".venv\Scripts\python.exe" -m pip install -r requirements.txt pywebview || goto :error

echo [3/3] اجرای اپ دسکتاپ...
".venv\Scripts\python.exe" desktop_app.py %*
goto :eof

:error
echo.
echo خطا در راه‌اندازی. خروجی بالا را بررسی کنید.
pause
