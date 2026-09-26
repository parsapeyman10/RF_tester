@echo off
REM RF Tester - اجرای کلاینت دسکتاپ روی ویندوز (نیاز: Python 3.8+)
cd /d "%~dp0"
python rf_tester_gui.py %*
if errorlevel 1 pause
