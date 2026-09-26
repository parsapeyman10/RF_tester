@echo off
REM RF Tester - نسخه مرورگری (خودش مرورگر را باز می‌کند)
cd /d "%~dp0"
python server.py %*
if errorlevel 1 pause
