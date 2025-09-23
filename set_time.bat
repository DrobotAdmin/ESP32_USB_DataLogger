@echo off
echo ===================================
echo RTC Time Setter
echo ===================================
echo.
echo Встановлюємо системний час на RTC...
echo Переконайтеся що ESP32 підключено до USB
echo.

python set_rtc_time.py

echo.
echo Для повторного запуску натисніть будь-яку клавішу...
pause > nul
