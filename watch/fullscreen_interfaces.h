#ifndef FULLSCREEN_INTERFACES_H
#define FULLSCREEN_INTERFACES_H

#include <Arduino.h>
#include <Preferences.h> 
#include <LittleFS.h>
#include <lvgl.h>
#include "File_Selection.h"
#include "atomic_utils.h"
#include "lgfx_config.h"
#include "SDScan.h"
#include "RTCManager.h"  
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <math.h>
#include <time.h>  
#include <stdio.h>
#include <cstdio>
#include <vector>

#define BUTTON_1   6
#define BUTTON_2   7
#define BUTTON_3   8

#define SD_DETECT_PIN 15

#define BATTERY_ADC_PIN         1
#define ADC_MAX_VALUE       4096.0f
#define ADC_REF_VOLTAGE       3.0f
#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_FULL_V        4.15f
#define BATTERY_EMPTY_V       3.3f
#define FILTER_ALPHA          0.3f

#define CHARGE_DETECT_PIN 39 

// I2S 引脚定义
#define I2S_BCLK  47
#define I2S_LRCLK 40
#define I2S_DIN   48

LV_FONT_DECLARE(chinese_24);
LV_FONT_DECLARE(time_70);
LV_FONT_DECLARE(tip_display_35);


// 退出回调函数声明
void fs_do_exit();
void sleep();

extern SdFs sd;
extern Preferences preferences;
extern LGFX display; 

// SD卡状态声明
extern bool sd_card_inserted;      // SD卡是否已插入
extern bool sd_card_initialized;   // SD卡是否已完成初始化
extern int sd_card_insertion_event;// SD卡插拔事件
extern bool sd_card_init_event;    // SD卡初始化失败事件
extern bool sd_card_scan_event;    // SD卡扫描事件
extern bool sd_force_scan;         // 强制扫描标记
extern SemaphoreHandle_t sd_state_mutex;  // SD状态互斥锁

//电池全局变量
extern float battery_voltage;// 电池电压
extern int   battery_percentage;// 电量百分比
extern volatile bool is_charging;//充电
extern volatile float Calibration;//电压校准

extern volatile int slow_start_exit_flag;
extern volatile int slow_start_completed;
extern volatile int current_brightness;

// 倒计变量
extern volatile bool g_countdown_active;      // true: 倒计时任务正在运行
extern volatile int  g_countdown_remaining;   // 剩余毫秒数
extern volatile int g_countdown_total;   // 总毫秒数

extern lv_obj_t* sliding_container;
extern lv_obj_t* fullscreen_container;

extern void destroy_file_selection_list(); 
extern void set_mapped_brightness(uint8_t input);
#endif