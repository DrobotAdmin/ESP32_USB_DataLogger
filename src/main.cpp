/*
 * ESP32-S3 USB Host Logger - Правильна реалізація USB Host
 * 
 * Використовує ESP-IDF USB Host API для читання CDC пристроїв через USB Type-C
 */

#include <Arduino.h>
#include "esp_log.h"

// ESP-IDF includes для USB Host
extern "C" {
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "usb/usb_host.h"
    #include "driver/gpio.h"
    #include "class/cdc/cdc.h"
}

static const char* TAG = "USB_HOST";

bool host_lib_init = false;
bool device_connected = false;
usb_device_handle_t device_handle = NULL;

// USB Host Client Handle
usb_host_client_handle_t client_hdl;

// Буфер для читання даних
#define USB_BUFFER_SIZE 512
uint8_t usb_buffer[USB_BUFFER_SIZE];

// Буфер для накопичення даних до кінця рядка
#define LINE_BUFFER_SIZE 1024
String lineBuffer = "";

// Глобальна змінна для endpoint
uint8_t cdc_in_endpoint = 0;

// Transfer callback для читання даних - З БУФЕРИЗАЦІЄЮ
void usb_transfer_cb(usb_transfer_t *transfer) {
    // Перевіряємо чи це не transfer від активної задачі
    if (transfer->context != (void*)999) { 
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
            // Додаємо отримані дані до буфера рядка
            for (int i = 0; i < transfer->actual_num_bytes; i++) {
                char ch = (char)transfer->data_buffer[i];
                
                if (ch == '\n') {
                    // Знайдено кінець рядка - виводимо повний рядок
                    Serial.println(lineBuffer);
                    lineBuffer = ""; // Очищуємо буфер
                } else if (ch == '\r') {
                    // Ігноруємо \r
                    continue;
                } else {
                    // Додаємо символ до буфера
                    lineBuffer += ch;
                    
                    // Захист від переповнення буфера
                    if (lineBuffer.length() > LINE_BUFFER_SIZE - 10) {
                        Serial.println(lineBuffer + " [ОБРІЗАНО]");
                        lineBuffer = "";
                    }
                }
            }
        }
        
        // Перезапускаємо transfer для безперервного читання
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK) {
            Serial.printf("[CDC] Помилка перезапуску: %s\n", esp_err_to_name(err));
        }
    }
}

// Простий callback для синхронних transfer з активної задачі
void sync_transfer_cb(usb_transfer_t *transfer) {
    // Просто позначаємо що transfer завершено
    // Основна логіка в cdc_reader_task - тут нічого не робимо
}

// Активна задача для читання (ЄДИНИЙ спосіб читання) - З БУФЕРИЗАЦІЄЮ
void cdc_reader_task(void *arg) {
    Serial.println("[CDC READER] ЄДИНА активна задача запущена");
    
    while (true) {
        if (device_handle != NULL && cdc_in_endpoint != 0) {
            // Створюємо transfer для читання
            usb_transfer_t *read_transfer;
            esp_err_t err = usb_host_transfer_alloc(64, 0, &read_transfer);
            if (err == ESP_OK) {
                read_transfer->device_handle = device_handle;
                read_transfer->bEndpointAddress = cdc_in_endpoint;
                read_transfer->callback = sync_transfer_cb;
                read_transfer->context = (void*)999; // Маркер активної задачі
                read_transfer->num_bytes = 64;
                read_transfer->timeout_ms = 10; // ШВИДКИЙ timeout для реального часу
                
                // Запускаємо transfer
                err = usb_host_transfer_submit(read_transfer);
                if (err == ESP_OK) {
                    // Чекаємо завершення з мінімальним timeout для реального часу
                    usb_host_client_handle_events(client_hdl, 20);
                    
                    if (read_transfer->status == USB_TRANSFER_STATUS_COMPLETED && 
                        read_transfer->actual_num_bytes > 0) {
                        
                        // Обробляємо отримані дані з буферизацією
                        for (int i = 0; i < read_transfer->actual_num_bytes; i++) {
                            char ch = (char)read_transfer->data_buffer[i];
                            
                            if (ch == '\n') {
                                // Знайдено кінець рядка - виводимо повний рядок
                                Serial.println(lineBuffer);
                                lineBuffer = ""; // Очищуємо буфер
                            } else if (ch == '\r') {
                                // Ігноруємо \r
                                continue;
                            } else {
                                // Додаємо символ до буфера
                                lineBuffer += ch;
                                
                                // Захист від переповнення буфера
                                if (lineBuffer.length() > LINE_BUFFER_SIZE - 10) {
                                    Serial.println(lineBuffer + " [ОБРІЗАНО]");
                                    lineBuffer = "";
                                }
                            }
                        }
                    }
                } else {
                    Serial.printf("[READER] Transfer submit failed: %s\n", esp_err_to_name(err));
                }
                
                // Завжди звільняємо transfer
                usb_host_transfer_free(read_transfer);
            } else {
                Serial.printf("[READER] Transfer alloc failed: %s\n", esp_err_to_name(err));
            }
            
            // МІНІМАЛЬНА затримка для реального часу (1мс = 1000 Hz!)
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Коли немає пристрою - чекаємо довше
        }
    }
}

// Функція для налаштування CDC читання
void setup_cdc_reading(usb_device_handle_t dev_hdl) {
    // Отримуємо дескриптор конфігурації
    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    
    Serial.println("[CDC] Налаштовуємо CDC інтерфейс...");
    
    // Шукаємо CDC інтерфейси
    const usb_intf_desc_t *data_intf_desc = NULL;
    const usb_ep_desc_t *in_ep_desc = NULL;
    int offset = 0;
    int data_intf_num = -1;
    
    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)((uint8_t *)config_desc + offset);
        
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf_desc = (const usb_intf_desc_t *)desc;
            
            Serial.printf("[CDC] Інтерфейс %d, клас: 0x%02X, підклас: 0x%02X\n", 
                         intf_desc->bInterfaceNumber, 
                         intf_desc->bInterfaceClass,
                         intf_desc->bInterfaceSubClass);
            
            // CDC Data інтерфейс (клас 0x0A) або шукаємо будь-який з endpoints
            if (intf_desc->bInterfaceClass == 0x0A || intf_desc->bNumEndpoints > 0) {
                data_intf_desc = intf_desc;
                data_intf_num = intf_desc->bInterfaceNumber;
                Serial.printf("[CDC] Використовуємо інтерфейс %d як CDC Data\n", data_intf_num);
            }
        } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && data_intf_desc != NULL) {
            const usb_ep_desc_t *ep_desc = (const usb_ep_desc_t *)desc;
            
            Serial.printf("[CDC] Endpoint: 0x%02X, тип: 0x%02X, напрямок: %s\n", 
                         ep_desc->bEndpointAddress,
                         ep_desc->bmAttributes & 0x03,
                         (ep_desc->bEndpointAddress & 0x80) ? "IN" : "OUT");
            
            // Шукаємо Bulk IN endpoint
            if ((ep_desc->bEndpointAddress & 0x80) == 0x80 && 
                (ep_desc->bmAttributes & 0x03) == 0x02) { // Bulk transfer
                in_ep_desc = ep_desc;
                cdc_in_endpoint = ep_desc->bEndpointAddress; // Зберігаємо для задачі
                Serial.printf("[CDC] Знайдено Bulk IN endpoint: 0x%02X\n", ep_desc->bEndpointAddress);
                break;
            }
        }
        
        offset += desc->bLength;
    }
    
        if (data_intf_num >= 0 && in_ep_desc != NULL) {
        // Відкриваємо інтерфейс
        esp_err_t err = usb_host_interface_claim(client_hdl, dev_hdl, data_intf_num, 0);
        if (err != ESP_OK) {
            Serial.printf("[CDC] Помилка відкриття інтерфейсу: %s\n", esp_err_to_name(err));
            return;
        }
        Serial.printf("[CDC] Інтерфейс %d успішно відкрито\n", data_intf_num);
        
        // Створюємо ШВИДКИЙ асинхронний transfer для реального часу
        usb_transfer_t *transfer;
        err = usb_host_transfer_alloc(USB_BUFFER_SIZE, 0, &transfer);
        if (err != ESP_OK) {
            Serial.printf("[CDC] Помилка створення transfer: %s\n", esp_err_to_name(err));
            return;
        }
        
        // АКТИВУЄМО основний transfer для максимальної швидкості
        Serial.println("[CDC] Основний ШВИДКИЙ transfer АКТИВОВАНО");
        
        transfer->device_handle = dev_hdl;
        transfer->bEndpointAddress = in_ep_desc->bEndpointAddress;
        transfer->callback = usb_transfer_cb;
        transfer->context = (void*)(uintptr_t)0; // Основний transfer
        transfer->num_bytes = USB_BUFFER_SIZE;
        transfer->timeout_ms = 10; // Швидкий timeout
        
        // Запускаємо transfer
        err = usb_host_transfer_submit(transfer);
        if (err == ESP_OK) {
            Serial.printf("[CDC] Основний ШВИДКИЙ transfer запущено\n");
        } else {
            Serial.printf("[CDC] Помилка запуску основного transfer: %s\n", esp_err_to_name(err));
            usb_host_transfer_free(transfer);
            return;
        }
        
        Serial.println("[CDC] Система готова до читання в РЕАЛЬНОМУ ЧАСІ!");
        
        // ВІДКЛЮЧАЄМО додаткову задачу - використовуємо тільки швидкий callback
        // xTaskCreate(cdc_reader_task, "cdc_reader", 4096, NULL, 4, NULL);
    } else {
        Serial.println("[CDC] Не знайдено підходящий інтерфейс або endpoint!");
    }
}

// USB Host подія callback
void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: {
            Serial.println("[USB] Новий пристрій підключено!");
            device_connected = true;
            
            // Отримуємо список адрес пристроїв
            uint8_t dev_addr_list[10];
            int num_dev = 10;
            usb_host_device_addr_list_fill(10, dev_addr_list, &num_dev);
            
            if (num_dev > 0) {
                // Отримуємо handle для першого пристрою
                esp_err_t err = usb_host_device_open(client_hdl, dev_addr_list[0], &device_handle);
                if (err == ESP_OK && device_handle != NULL) {
                    Serial.printf("[USB] Отримано handle пристрою (addr: %d)\n", dev_addr_list[0]);
                    setup_cdc_reading(device_handle);
                } else {
                    Serial.printf("[USB] Помилка відкриття пристрою: %s\n", esp_err_to_name(err));
                }
            }
            break;
        }
        case USB_HOST_CLIENT_EVENT_DEV_GONE: {
            Serial.println("[USB] Пристрій відключено!");
            device_connected = false;
            if (device_handle != NULL) {
                usb_host_device_close(client_hdl, device_handle);
                device_handle = NULL;
            }
            break;
        }
        default:
            break;
    }
}

// Задача USB Host
void usb_host_task(void *arg) {
    Serial.println("[TASK] USB Host задача запущена");
    
    // Ініціалізація USB Host библиотеки
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] USB Host install failed: %s\n", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    Serial.println("[USB] USB Host успішно встановлено");
    
    // Створення клієнта
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        }
    };
    
    err = usb_host_client_register(&client_config, &client_hdl);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] Client register failed: %s\n", esp_err_to_name(err));
        usb_host_uninstall();
        vTaskDelete(NULL);
        return;
    }
    Serial.println("[USB] USB Host клієнт створено");
    
    host_lib_init = true;
    
    // Основний цикл обробки подій - ШВИДКИЙ для реального часу
    while (true) {
        // Обробка подій библиотеки з мінімальним timeout
        uint32_t event_flags;
        usb_host_lib_handle_events(10, &event_flags); // 10мс замість portMAX_DELAY
        
        // Обробка подій клієнта з мінімальним timeout
        usb_host_client_handle_events(client_hdl, 5); // 5мс замість 0
        
        vTaskDelay(pdMS_TO_TICKS(1)); // Мінімальна затримка 1мс
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("===============================================");
    Serial.println("    ESP32-S3 USB Host Logger - TRUE HOST");
    Serial.println("===============================================");
    Serial.println("Ініціалізація USB Host...");
    
    // Налаштовуємо GPIO для USB-OTG (Host mode)
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT_OUTPUT);  // USB D-
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_INPUT_OUTPUT);  // USB D+
    
    // Створюємо задачу USB Host
    xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 5, NULL);
    
    // Чекаємо ініціалізації
    Serial.print("Чекаємо ініціалізації USB Host");
    while (!host_lib_init) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println();
    Serial.println("USB Host ініціалізовано!");
    Serial.println("Підключіть ESP32 через USB Type-C кабель");
    Serial.println("ESP32-S3 тепер працює як USB HOST");
    Serial.println("===============================================");
    
    pinMode(2, OUTPUT);
    
    // Тест LED
    for(int i = 0; i < 5; i++) {
        digitalWrite(2, HIGH);
        delay(100);
        digitalWrite(2, LOW);
        delay(100);
    }
}

void loop() {
    
}