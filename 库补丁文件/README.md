# 手表项目 - 依赖库与补丁说明

本目录用于**配合源码分发**。发行时只需分发：
1. 项目源码（`watch` 文件夹，含 `lv_conf.h` 的说明）；
2. 本目录（被修改过的库文件，直接覆盖即可）。

**不需要分发完整库文件**，接收方自行安装官方库后，用本目录中的同名文件覆盖即可。

---

## 一、需要安装的 Arduino 库及版本

| 库 | 版本 | 说明 |
|---|---|---|
| **arduino-esp32 (ESP32 core)** | **3.2.0** | 必须此版本；含 WiFi / WiFiUdp / HTTPClient / LittleFS / FS / Preferences / Wire / SPI / ESP-DSP / esp_adc_cal / FreeRTOS |
| lvgl | **8.3.11** | UI 框架 |
| LovyanGFX | **1.2.7** | ST7789 屏幕 + CST816S 触摸驱动 |
| SdFat | **2.2.3** | SD 卡（`SdFs`） |
| JPEGDEC | **1.8.0** | JPG 解码 |
| PNGdec | **1.1.3** | PNG 解码 |
| ArduinoJson | **7.4.2** | 天气 JSON 解析 |
| NTPClient | **3.2.1** | 网络时间 |
| ESP8266Audio | **2.0.0** | MP3 播放（**必须用补丁**） |

> 安装来源：Arduino IDE「库管理器」安装以上**精确版本**；ESP32 core 用开发板管理器安装 **3.2.0**。
> 注意：ESP8266Audio 请安装 **2.0.0 发行版**，不要用 GitHub master（文件格式不同，补丁不通用）。

---

## 二、ESP32 开发板设置（Arduino IDE）

板卡：**ESP32S3 Dev Module**
- PSRAM：**Enabled（QSPI PSRAM）** —— 代码大量使用 `ps_malloc` / `MALLOC_CAP_SPIRAM`
- Flash Size：**16MB (128Mb)**
- Partition Scheme：**Custom**（需配合下面的 `custom.csv` 和 `boards.txt`）
- CPU Frequency：默认 240MHz
- 编译优化：**-O2**（本项目已改 `platform.txt`，见第四节）

---

## 三、目录结构与覆盖位置

安装好上述库后，按下表把本目录中的文件覆盖到对应位置：

| 本目录文件 | 覆盖到（相对库安装目录） |
|---|---|
| `ESP8266Audio/src/AudioGeneratorMP3.h` | `Arduino\libraries\ESP8266Audio\src\` |
| `ESP8266Audio/src/AudioGeneratorMP3.cpp` | `Arduino\libraries\ESP8266Audio\src\` |
| `ESP8266Audio/src/AudioOutputI2S.cpp` | `Arduino\libraries\ESP8266Audio\src\` |
| `lvgl/src/extra/widgets/calendar/lv_calendar_header_dropdown.c` | `Arduino\libraries\lvgl\src\extra\widgets\calendar\` |
| `PNGdec/src/PNGdec.h` | `Arduino\libraries\PNGdec\src\` |
| `SdFat/src/SdFatConfig.h` | `Arduino\libraries\SdFat\src\` |
| `lv_conf.h` | **`Arduino\libraries\`（库根目录，不是 lvgl 里面）** |
| `esp32-core/boards.txt` | `...\Arduino15\packages\esp32\hardware\esp32\3.2.0\` |
| `esp32-core/platform.txt` | `...\Arduino15\packages\esp32\hardware\esp32\3.2.0\` |
| `esp32-core/tools/partitions/custom.csv` | `...\Arduino15\packages\esp32\hardware\esp32\3.2.0\tools\partitions\` |

> `Arduino` 一般指 `C:\Users\<用户名>\Documents\Arduino` 或自定义库目录；
> `Arduino15` 一般指 `C:\Users\<用户名>\AppData\Local\Arduino15`。
> 替换 ESP32 core 文件前建议先备份原文件。

---

## 四、各文件改动摘要

### 1. ESP8266Audio 2.0.0（3 个文件）
- `AudioGeneratorMP3.h`：新增 `setSampleCallback()` 与 `sampleCallback` 回调成员；`buffLen` 改为 `12*1024`。
- `AudioGeneratorMP3.cpp`：`Input()` 增加 `unused > 4096` 提前返回；`loop()` 中在消费采样前调用 `sampleCallback`（项目 `fs_music.cpp` 使用该回调做音谱/音量联动）。
- `AudioOutputI2S.cpp`：仅一处空格排版差异，无功能变化（可覆盖也可不覆盖）。

### 2. lvgl 8.3.11（1 个文件）
- `lv_calendar_header_dropdown.c`：日历年份下拉列表上限由 2023 扩展到 **2080**，并同步 `year_event_cb` / `value_changed_event_cb` 中的基准年份。

### 3. PNGdec 1.1.3（1 个文件）
- `PNGdec.h`：`PNG_MAX_BUFFERED_PIXELS` 由 `(480*4+1)*2` 改为 **`(2048*4+1)*2`**，支持更大宽度图片。

### 4. SdFat 2.2.3（1 个文件）
- `SdFatConfig.h`：启用 `#define USE_UTF8_LONG_NAMES 1` 及 `#define USE_LONG_FILE_NAMES 1`，用于正确读取/显示**中文文件名**。

### 5. ESP32 core 3.2.0（3 处）
- `boards.txt`：`esp32s3.menu.PartitionScheme.custom.build.partitions=custom`（让“Custom”分区方案指向 `custom.csv`）。**整文件较大（约 2.5MB），改动仅此 1 行**；也可手动把该行等号后留空改为 `custom`。
- `platform.txt`：编译优化由 `-Os` 改为 **`-O2`**（`compiler.optimization_flags` 的 debug/release 两行）。
- `custom.csv`（**新增文件**）：自定义分区表，`app0` 5MB + `littlefs` ~11.4MB，总 16MB。

---

## 五、未改动的库（无需替换）
- LovyanGFX 1.2.7：核心代码与官方一致（仅一个官方示例文件差异，与本项目无关）。
- JPEGDEC 1.8.0：完全一致。
- ArduinoJson 7.4.2：完全一致。
- NTPClient 3.2.1：未发现改动。

---

## 六、lv_conf.h 说明
`lv_conf.h` 为深度定制版，关键项：
- `LV_COLOR_DEPTH 16`、`LV_COLOR_16_SWAP 0`
- `LV_USE_LOG 1`、`LV_USE_PERF_MONITOR 1`、`LV_USE_MEM_MONITOR 1`
- `LV_FONT_FMT_TXT_LARGE 1`（源码中的大字体 `chinese_24.c` 依赖此宏，否则编译报错）
- 启用全部 Montserrat 字体

放置位置：**`Arduino\libraries\lv_conf.h`**（lvgl 库会在该路径查找）。

---

## 七、快速校验
编译前确认：
1. ESP32 core 版本 = 3.2.0；
2. ESP8266Audio = 2.0.0 且已覆盖 3 个文件；
3. `Arduino\libraries\lv_conf.h` 存在；
4. 板卡 = ESP32S3 Dev Module，PSRAM 已启用，Flash 16MB，Partition = Custom；
5. core 的 `custom.csv` 与 `boards.txt`、`platform.txt` 已替换。
