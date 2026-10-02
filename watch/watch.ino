#include <lvgl.h>
#include <LovyanGFX.hpp>
#include "lgfx_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ui_manager.h"
#include "atomic_utils.h" 
#include "RTCManager.h"   
#include "esp_adc_cal.h"


//SD卡状态信号量 
bool sd_card_inserted = false;
bool sd_card_initialized = false;
int sd_card_insertion_event = 0;
bool sd_card_init_event = false;// SD卡初始化失败事件
bool sd_card_scan_event = false;
bool sd_force_scan = false;
bool sd_card_warming= false;// SD卡预热中（预写入）
SemaphoreHandle_t sd_state_mutex = NULL;
QueueHandle_t scan_queue = NULL;
SemaphoreHandle_t spi_bus_sem = NULL;

// 电池监测
float battery_voltage = 0.0f;
int   battery_percentage = 0;
volatile float Calibration = 100;
volatile bool is_charging = false; 

//看门狗
hw_timer_t *watchdog_timer = NULL;

TaskHandle_t slow_start_task_handle = NULL;
volatile int slow_start_exit_flag = 0;      // 立即退出启动任务
volatile int slow_start_completed = 0;      // 启动动画正常执行完毕
volatile int current_brightness = 0;        // 当前屏幕亮度

// SPI总线占用标志
static bool spi_busy_flag = false;

LGFX display;
Preferences preferences;

#define TFT_HOR_RES   240
#define TFT_VER_RES   280
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 5 * (LV_COLOR_DEPTH / 8))
static lv_color_t draw_buf[DRAW_BUF_SIZE];

static lv_disp_draw_buf_t draw_buf_dsc;
static lv_disp_drv_t disp_drv;
static lv_disp_t * disp;

static lv_indev_drv_t indev_drv;
static lv_indev_t * indev;


void ARDUINO_ISR_ATTR onWatchdogTimeout() {
    esp_restart();
}
void set_mapped_brightness(uint8_t input) {
    if (input == 0) {
        display.setBrightness(0);
        return;
    }
    const float k = 0.0175f;
    uint8_t output = (uint8_t)((expf(input * k) - 1.0f) / (expf(255 * k) - 1.0f) * 255.0f);

    if (output == 0 && input > 0) output = 1;
    
    display.setBrightness(output);
}
void slow_start_task(void* parameter) {
    preferences.begin("watch", true);
    int target_linear = preferences.getUChar("brightness", 128);
    preferences.end();
    int steps = 80; 
    display.setBrightness(0);
    atomic_store_int(&current_brightness, 0); 
    for (int i = 1; i <= steps; i++) {
        if (atomic_load_int(&slow_start_exit_flag)) {
            vTaskDelete(NULL);
            return;
        }
        int current_linear = (target_linear * i) / steps;
        set_mapped_brightness(current_linear);
        atomic_store_int(&current_brightness, current_linear); 
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    set_mapped_brightness(target_linear);
    atomic_store_int(&current_brightness, target_linear);
    atomic_store_int(&slow_start_completed, 1);
    vTaskDelete(NULL);
}
void my_print(const char* buf) {
    Serial.println(buf);
    Serial.flush();
}

void my_disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p) {
    while (atomic_load_bool(&spi_busy_flag)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    display.startWrite();
    display.setAddrWindow(area->x1, area->y1, w, h);
    display.writePixels((lgfx::rgb565_t*)color_p, w * h);
    display.endWrite();
    lv_disp_flush_ready(disp_drv);
}

void my_touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
    uint16_t x, y;
    bool touched = display.getTouch(&x, &y);
    data->state = touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (touched) {
        data->point.x = x;
        data->point.y = y;
    }
}

static uint32_t my_tick(void) {
    return millis();
}
// 预写入函数
void pre_write_warm_sd_card() {
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_card_warming = true;
        xSemaphoreGive(sd_state_mutex);
    }
    // 预写文件
    const char* warm_file = "/warm_sd.tmp";
    FsFile warmFile = sd.open(warm_file, O_WRONLY | O_CREAT | O_TRUNC);
    Serial.printf("%lu\n", millis());
    if (warmFile) { 
        warmFile.write('0');
        warmFile.close();
        //删除临时文件
        sd.remove(warm_file);
        Serial.printf("%lu\n", millis());
    } else {
        Serial.println("SD卡预写入失败");
    }
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_card_warming = false;
        xSemaphoreGive(sd_state_mutex);
    }
}
// ==================== SD卡初始化 ====================
bool initialize_sd_card() {
    bool init_result = false;   
    setScanProgressCallback(onScanProgress);    

    //SD卡初始化期间禁止屏幕刷新使用SPI
    atomic_store_bool(&spi_busy_flag, true);   // 设置标志
    vTaskDelay(pdMS_TO_TICKS(10));
    bool sd_init_ok = initializeSDCard();      // 执行初始化
    atomic_store_bool(&spi_busy_flag, false);  // 清除标志


    if (!sd_init_ok) {
        Serial.println("SD卡初始化失败");    
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_initialized = false;
            sd_card_init_event = true;
            xSemaphoreGive(sd_state_mutex);
        }
        init_result = false;
    } else {
        Serial.println("SD卡初始化成功");

        // 预写入
        //pre_write_warm_sd_card();

        init_result = true;
        ensureDirectoriesAndFiles();
        if (checkNeedRescan()) {
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = true;
                xSemaphoreGive(sd_state_mutex);
            }
            if (scan_queue != NULL) xQueueReset(scan_queue);
            performScan();
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = false;
                xSemaphoreGive(sd_state_mutex);
            }
        }
        // 设置SD卡初始化成功状态
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_initialized = true;
            xSemaphoreGive(sd_state_mutex);
        }
    }
    return init_result;
}

void onScanProgress(int categoryIndex, int currentCount, const char* categoryName) {
    ScanData scan_data;
    scan_data.category_index = categoryIndex;
    scan_data.current_count = currentCount;
    if (categoryName) {
        strncpy(scan_data.category_name, categoryName, sizeof(scan_data.category_name)-1);
        scan_data.category_name[sizeof(scan_data.category_name)-1] = '\0';
    }
    if (scan_queue) xQueueSend(scan_queue, &scan_data, 0);
}

void sd_init_task(void* parameter) {  
    uint8_t last_sd_pin_state = HIGH;
    while (1) {
        if (!atomic_load_bool(&is_fullscreen_container_active)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            if (digitalRead(SD_DETECT_PIN) == LOW) {
                vTaskDelay(pdMS_TO_TICKS(200));
                Serial.println("SD卡插入");
                if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    sd_card_inserted = true;
                    xSemaphoreGive(sd_state_mutex);
                }
                initialize_sd_card();
            } else {
                Serial.println("未检测到SD卡");
                if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    sd_card_inserted = false;
                    sd_card_initialized = false;
                    xSemaphoreGive(sd_state_mutex);
                }
            }
            last_sd_pin_state = digitalRead(SD_DETECT_PIN);
            while(1) {
                bool need_force_scan = false;
                if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    need_force_scan = sd_force_scan;
                    if (need_force_scan) sd_force_scan = false;
                    xSemaphoreGive(sd_state_mutex);
                }
                if (need_force_scan) {
                    Serial.println("触发SD卡重新扫描");
                    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        sd_card_scan_event = true;
                        xSemaphoreGive(sd_state_mutex);
                    }
                    if (scan_queue) xQueueReset(scan_queue);
                    Serial.println(1);
                    Serial.println(millis());
                    performScan();
                    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        sd_card_scan_event = false;
                        xSemaphoreGive(sd_state_mutex);
                    }
                }
                uint8_t curr_sd_pin_state = digitalRead(SD_DETECT_PIN);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (last_sd_pin_state == HIGH && curr_sd_pin_state == LOW) {
                    Serial.println("SD卡插入");
                    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        sd_card_insertion_event = 1;
                        sd_card_inserted = true;
                        xSemaphoreGive(sd_state_mutex);
                    }
                    initialize_sd_card();
                } else if (last_sd_pin_state == LOW && curr_sd_pin_state == HIGH) {
                    Serial.println("[SD检测] 上升沿触发,SD卡拔出");
                    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        sd_card_insertion_event = -1;
                        sd_card_inserted = false;
                        sd_card_initialized = false;
                        xSemaphoreGive(sd_state_mutex);
                    }
                }    
                last_sd_pin_state = curr_sd_pin_state;
            }
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
void battery_monitor_task(void* parameter) {
  gpio_reset_pin((gpio_num_t)BATTERY_ADC_PIN);
  pinMode(BATTERY_ADC_PIN, ANALOG);
  gpio_pullup_dis((gpio_num_t)BATTERY_ADC_PIN);
  gpio_pulldown_dis((gpio_num_t)BATTERY_ADC_PIN);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  
  // --- 状态变量 ---
  float last_raw_voltage = 0.0f;        
  float voltage_before_charge = 0.0f;   
  float fixed_ir_drop = 0.0f;           
  float filtered_percentage = -1.0f;   
  
  bool last_charging_state = false;    
  int lasttime = -20000;
  
  const int SAMPLES_PER_CYCLE = 20;
  TickType_t last_wake_time = xTaskGetTickCount();

  bool last_charging = digitalRead(CHARGE_DETECT_PIN);
  bool initial_charging = (last_charging == 0); // 0表示正在充电
  
  if(initial_charging){
    atomic_store_bool(&is_charging, true);  
    Serial.println("启动时充电器已插入");
    //赋予经验压升差值
    fixed_ir_drop = 0.15f; 
    // 同步充电状态
    last_charging_state = true; 
    // 标记需要初始化 voltage_before_charge，设为负数作为标志位
    voltage_before_charge = -1.0f; 
  }

  while (1) {
    bool charging = digitalRead(CHARGE_DETECT_PIN);
    if (charging == 0 && last_charging == 1 ) {
        atomic_store_bool(&is_charging, true);  
        Serial.println("充电器插入");
    } else if(charging == 1 && last_charging == 0){
        atomic_store_bool(&is_charging, false); 
        Serial.println("充电器拔出");
        fixed_ir_drop = 0;
    }
    last_charging = charging;
    
    if(millis() - lasttime > 20000){
        lasttime = millis();
        
        // 采样并计算
        uint32_t mv_sum = 0;
        for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
            mv_sum += analogReadMilliVolts(BATTERY_ADC_PIN);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        float avg_pin_mv = (float)mv_sum / SAMPLES_PER_CYCLE;
        float raw_voltage = (avg_pin_mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;
        
        bool current_charging_state = atomic_load_bool(&is_charging);

        // --- 启动时已充电的首次推算 ---
        // 如果 voltage_before_charge 被标记为 -1.0f，说明是启动时已插充电器，需要用当前电压反推
        if (voltage_before_charge < 0.0f && current_charging_state) {
            voltage_before_charge = raw_voltage - fixed_ir_drop; // 反推拔出前的大致电压
            if (voltage_before_charge < 0.0f) voltage_before_charge = 0.0f; // 安全保护
            Serial.print("启动反推插入前电压: "); Serial.println(voltage_before_charge);
        }
        
        //处理充电插入瞬间的状态
        if (current_charging_state && !last_charging_state) {
            if (last_raw_voltage > 0.1f) {
                voltage_before_charge = last_raw_voltage;
            }
            fixed_ir_drop = 0.0f; 
        }
        
        // 在插入充电后的时，锁定固定的内阻压降
        if (current_charging_state && fixed_ir_drop == 0.0f && voltage_before_charge > 0.1f) {
            float current_rise = raw_voltage - voltage_before_charge;
            if (current_rise > 0.0f) {
                fixed_ir_drop = current_rise; 
            }
        }
        
        // 计算用于电量百分比的补偿电压
        float voltage_for_calc = raw_voltage;
        
        if (current_charging_state && fixed_ir_drop > 0.0f) {
            if (raw_voltage < 4.15f) { // 恒流阶段
                voltage_for_calc = raw_voltage - fixed_ir_drop;
                
                // 防止因为测量波动导致算出来的值低于插入前电压
                if (voltage_for_calc < voltage_before_charge) {
                    voltage_for_calc = voltage_before_charge;
                }
            } else {
                voltage_for_calc = raw_voltage;
            }
        }
        
        //校准并计算
        float cal = atomic_load_float(&Calibration);
        float calibrated_voltage = voltage_for_calc * (cal / 100.0f);
        
        int raw_percent = (int)((calibrated_voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0f);
        if (raw_percent < 0) raw_percent = 0;
        if (raw_percent > 100) raw_percent = 100;
        
        if (filtered_percentage < 0.0f) {
            filtered_percentage = (float)raw_percent;
        } else {
            filtered_percentage = (FILTER_ALPHA * raw_percent) + ((1.0f - FILTER_ALPHA) * filtered_percentage);
        }
        int final_percent = (int)round(filtered_percentage);
        
        // 存储结果
        atomic_store_float(&battery_voltage, raw_voltage); 
        atomic_store_int(&battery_percentage, final_percent); 
        
        last_raw_voltage = raw_voltage;
        last_charging_state = current_charging_state;
    }
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(200));
  }
}

void sleep(){
    display.setBrightness(0);
    gpio_deep_sleep_hold_en();
    digitalWrite(5, HIGH);
    gpio_deep_sleep_hold_en();
    gpio_hold_en((gpio_num_t)5);
    rtc_gpio_deinit((gpio_num_t)BUTTON_2);
    rtc_gpio_set_direction((gpio_num_t)BUTTON_2, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)BUTTON_2); 
    rtc_gpio_pullup_dis((gpio_num_t)BUTTON_2);  
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_2, 1); 
    Serial.flush(); 
    esp_deep_sleep_start();
}
void setup() {
    gpio_hold_dis((gpio_num_t)5);
    gpio_deep_sleep_hold_dis();
    pinMode(5, OUTPUT);
    digitalWrite(5, LOW);
    Serial.begin(115200);
    sd_state_mutex = xSemaphoreCreateMutex();
    scan_queue = xQueueCreate(10, sizeof(ScanData));

    pinMode(SD_DETECT_PIN, INPUT_PULLUP);
    pinMode(CHARGE_DETECT_PIN, INPUT);
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << (gpio_num_t)BUTTON_1) | (1ULL << (gpio_num_t)BUTTON_2) | (1ULL << (gpio_num_t)BUTTON_3);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;  
    gpio_config(&io_conf);

    // 从 Preferences 读取校准值
    preferences.begin("watch", true);
    float saved_cal = preferences.getFloat("batt_cal", 100);
    preferences.end();
    atomic_store_float(&Calibration, saved_cal);  

    xTaskCreatePinnedToCore(battery_monitor_task, "BatteryMon", 3072, NULL, 1, NULL, 1);
     
    display.init();
    display.setRotation(0);
    display.setBrightness(0);
    display.setColorDepth(16);
    display.fillScreen(TFT_BLACK);
    
    if (rtcManager.begin()) {
        // 将 RTC 时间同步到系统
        if (rtcManager.syncToSystem()) {
            Serial.println("系统时间已从外部 RTC 同步");    
        } else {
            Serial.println("RTC 同步失败");
        }
    } else {
        Serial.println("外部 RTC 不存在");
    }

    lv_init();
    

#if LV_USE_LOG
    lv_log_register_print_cb(my_print);
#endif

    // 显示驱动
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf, NULL, DRAW_BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TFT_HOR_RES;
    disp_drv.ver_res = TFT_VER_RES;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc;
    disp_drv.sw_rotate = 1;
    disp_drv.rotated = LV_DISP_ROT_NONE;
    disp = lv_disp_drv_register(&disp_drv);

    // 触摸驱动
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    indev = lv_indev_drv_register(&indev_drv);

    // 屏幕背景
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_STATE_DEFAULT);

    ui_init_all();

    // 创建二值信号量
    spi_bus_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(spi_bus_sem);


    xTaskCreatePinnedToCore(sd_init_task, "SD Init", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(slow_start_task,"SlowStart",2048,NULL,1,&slow_start_task_handle,1);

    watchdog_timer = timerBegin(1000000);               
    timerAttachInterrupt(watchdog_timer, &onWatchdogTimeout); 
    timerAlarm(watchdog_timer, 10 * 1000000, false, 0);

    Serial.println("系统初始化完成");
} 
void loop() {
    if (watchdog_timer) {
        timerWrite(watchdog_timer, 0);
    }
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(2));


/*
    static uint32_t last_ram_print_ms = 0;
    if (millis() - last_ram_print_ms >= 10000) {
        last_ram_print_ms = millis();
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t free_kb = free_heap / 1024;
        uint32_t max_kb = max_block / 1024;
        Serial.printf("[INTERNAL RAM] 剩余: %u KB | 最大连续: %u KB\n", free_kb, max_kb);
    }*/
}