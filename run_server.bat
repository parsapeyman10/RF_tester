@echo off
REM ==================================================================
REM  راه‌اندازی سرور RF_tester روی ویندوز
REM  بار اول محیط مجازی می‌سازد و پیش‌نیازها را نصب می‌کند.
REM ==================================================================
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] ساخت محیط مجازی...
    python -m venv .venv || goto :error
)

echo [2/3] نصب/بررسی پیش‌نیازها...
".venv\Scripts\python.exe" -m pip install --upgrade pip >nul
".venv\Scripts\python.exe" -m pip install -r requirements.txt || goto :error

echo [3/3] اجرای سرور...
echo.
echo   روی همین کامپیوتر :  http://127.0.0.1:5000
echo   از گوشی / شبکه    :  http://[IP این کامپیوتر]:5000
echo   (برای دیدن IP دستور ipconfig را بزنید)
echo.
".venv\Scripts\python.exe" app.py %*
goto :eof

:error
echo.
echo خطا در راه‌اندازی. خروجی بالا را بررسی کنید.
pause
