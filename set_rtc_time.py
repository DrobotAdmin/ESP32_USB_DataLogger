#!/usr/bin/env python3
"""
RTC Time Setter - Встановлює системний час на RTC годинник ESP32
"""

import serial
import serial.tools.list_ports
import datetime
import time
import sys

def find_esp32_port():
    """Знаходить COM порт ESP32"""
    ports = serial.tools.list_ports.comports()
    
    # Шукаємо ESP32 за описом
    esp32_keywords = ['ESP32', 'Silicon Labs', 'CH340', 'CP210', 'USB Serial']
    
    for port in ports:
        for keyword in esp32_keywords:
            if keyword.lower() in port.description.lower():
                return port.device
    
    # Якщо не знайшли автоматично, показуємо всі доступні порти
    print("ESP32 не знайдено автоматично. Доступні порти:")
    for i, port in enumerate(ports):
        print(f"{i+1}. {port.device} - {port.description}")
    
    if ports:
        try:
            choice = int(input("Виберіть номер порту: ")) - 1
            if 0 <= choice < len(ports):
                return ports[choice].device
        except ValueError:
            pass
    
    return None

def set_rtc_time():
    """Встановлює поточний системний час на RTC"""
    print("=== RTC Time Setter ===")
    print("Встановлення системного часу на RTC годинник")
    
    # Знаходимо ESP32
    port = find_esp32_port()
    if not port:
        print("Не вдалося знайти ESP32. Переконайтеся що пристрій підключено.")
        return False
    
    print(f"Підключення до {port}...")
    
    try:
        # Підключаємося до ESP32
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(2)  # Чекаємо стабілізації з'єднання
        
        print("Підключено!")
        
        # Очищуємо буфер
        ser.flushInput()
        ser.flushOutput()
        
        # Отримуємо поточний системний час
        now = datetime.datetime.now()
        time_str = now.strftime("%Y-%m-%d %H:%M:%S")
        
        print(f"Системний час: {time_str}")
        print("Встановлюємо час на RTC...")
        
        # Формуємо команду для ESP32
        command = f"settime {time_str}\n"
        
        # Відправляємо команду
        ser.write(command.encode('utf-8'))
        ser.flush()
        
        # Чекаємо відповідь
        print("Очікуємо відповідь від ESP32...")
        start_time = time.time()
        
        while time.time() - start_time < 5:  # Чекаємо до 5 секунд
            if ser.in_waiting > 0:
                response = ser.readline().decode('utf-8').strip()
                print(f"ESP32: {response}")
                
                if "встановлено" in response.lower() or "час синхронізовано" in response.lower():
                    print("✅ Час успішно встановлено!")
                    
                    # Перевіряємо встановлений час
                    print("Перевіряємо встановлений час...")
                    ser.write(b"gettime\n")
                    ser.flush()
                    time.sleep(0.5)
                    
                    if ser.in_waiting > 0:
                        check_response = ser.readline().decode('utf-8').strip()
                        print(f"Час на RTC: {check_response}")
                    
                    return True
            
            time.sleep(0.1)
        
        print("⚠️ Не отримано підтвердження від ESP32")
        return False
        
    except serial.SerialException as e:
        print(f"❌ Помилка підключення до порту {port}: {e}")
        return False
    except Exception as e:
        print(f"❌ Неочікувана помилка: {e}")
        return False
    finally:
        if 'ser' in locals():
            ser.close()
            print("З'єднання закрито.")

def main():
    """Головна функція"""
    try:
        success = set_rtc_time()
        
        if success:
            print("\n🎉 Операція завершена успішно!")
        else:
            print("\n❌ Не вдалося встановити час на RTC")
            
    except KeyboardInterrupt:
        print("\n⏹️ Операція перервана користувачем")
    
    input("\nНатисніть Enter для виходу...")

if __name__ == "__main__":
    main()
