#include "fullscreen_interfaces.h"
//#include <SD.h>
#include <SPI.h>
#include "AudioFileSourceSdFat.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <vector>
#include "esp_dsp.h"   

extern void fs_do_exit();
extern FileSelectionInstance* g_file_selection_instance;
extern SdFs sd;
extern bool sd_force_scan;
extern SemaphoreHandle_t sd_state_mutex;

LV_FONT_DECLARE(chinese_24);

#define DEBOUNCE_THRESHOLD 20
#define STATE_CHANGE_COOLDOWN 500
#define TIMER_PERIOD 10
#define MUSIC_PATH_PREFIX "/音乐/"
#define SCAN_LIST_PATH "/ScanList/music.txt"

// FFT 常量
#define REF_MAX_MAGNITUDE 400000.0f
#define FFT_SAMPLES 2048              // 2048 点处理
#define FFT_BINS 50

// 前向声明
static void audioTask(void *pvParameters);

// 结构体与枚举
enum PlayMode {
    MODE_SEQUENCE = 0,
    MODE_RANDOM,
    MODE_LOOP
};

struct MusicEntry {
    char name[128];
    char full_path[256];
    bool played;
};

// GPIO 状态
static bool last_BUTTON2_level = false;
static uint32_t stable_start_BUTTON2 = 0;
static bool last_sd_card_state = true;
static lv_timer_t* gpio_timer = NULL;
static lv_obj_t* g_container = NULL;

// 播放列表状态
static std::vector<MusicEntry> playlist;
static int current_song_index = -1;
static PlayMode current_mode = MODE_SEQUENCE;

// UI 对象
static lv_obj_t *volume_slider = NULL;
static lv_obj_t *progress_slider = NULL;
static lv_obj_t *play_pause_btn = NULL;
static lv_obj_t *play_pause_label = NULL;
static lv_timer_t *progress_timer = NULL;
static lv_obj_t *prev_btn = NULL;
static lv_obj_t *next_btn = NULL;
static lv_obj_t *mode_btn = NULL;
static lv_obj_t *mode_label = NULL;
static lv_obj_t *file_name_label = NULL;
static lv_obj_t *volume_icon = NULL;

// 消息框对象
static lv_obj_t* music_msgbox_bg = NULL;
static lv_obj_t* music_msgbox = NULL;

// 音频对象
static AudioGeneratorMP3 *mp3 = nullptr;
static AudioFileSourceSdFat *audio_file = nullptr;
static AudioOutputI2S *out = nullptr;
static TaskHandle_t audioTaskHandle = NULL;
static QueueHandle_t cmdQueue = NULL;

// 共享状态
typedef struct {
    volatile bool fileOpened;
    volatile uint32_t fileSizeBytes;
    volatile uint32_t currentPosBytes;
    volatile bool isPlaying;
    volatile bool isPaused;
    volatile float volume;
    volatile bool taskQuitFlag;
    volatile bool songFinished;
    volatile bool openFailed;
    char lastFailedPath[256];
} PlayerStatus;

static PlayerStatus playerStatus = {0};
static bool manual_request_pending = false;

enum CommandType {
    CMD_NONE = 0,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_TOGGLE_PAUSE,
    CMD_SEEK_BYTES,
    CMD_SET_VOLUME,
    CMD_QUIT_TASK,
    CMD_OPEN_FILE
};

typedef struct {
    CommandType cmd;
    union {
        int32_t seekPos;
        float volume;
        char filePath[256];
    } param;
} QueuedCommand;

// FFT 变量（使用 ESP-DSP，float 类型）
static volatile float fft_magnitudes[FFT_BINS] = {0};
static volatile bool fft_new_data = false;
static float fft_peak_magnitudes[FFT_BINS] = {0};
static const float PEAK_FALL_SPEED = 0.8f;

// ESP-DSP 所需的 16 字节对齐工作数组
__attribute__((aligned(16))) static float fft_input[FFT_SAMPLES * 2]; // 复数输入/输出
__attribute__((aligned(16))) static float hamming_window[FFT_SAMPLES]; // 窗系数
static bool dsp_initialized = false;

//FFT 频率 bin 映射表
static int bin_map[50] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    21,  23,  25,  27,  29,  31,  33,  35,  37,  39,
    42,  45,  48,  52,  56,  60,  65,  70,  76,  83,
    91, 100, 111, 124, 140, 160, 185, 220, 280, 350
};

// 环形缓冲区与 FFT 本地缓冲区
#define FFT_RING_BUF_SIZE 4096  
static int16_t fft_ring_buf[FFT_RING_BUF_SIZE];
static volatile uint32_t fft_write_idx = 0;
static int16_t fft_ui_buf[FFT_SAMPLES];

// 画布相关
#define BAR_WIDTH 3
#define BAR_SPACING 1
#define MAX_BAR_HEIGHT 30
#define CANVAS_WIDTH (FFT_BINS * (BAR_WIDTH + BAR_SPACING) - BAR_SPACING)
#define CANVAS_HEIGHT (MAX_BAR_HEIGHT + 2)
static uint8_t canvas_buf[CANVAS_WIDTH * CANVAS_HEIGHT * 2];
static lv_obj_t* fft_canvas = NULL;

// 内部标志
static bool music_state_machine = false;
static bool need_switch_to_music = false;
static uint32_t last_state_change_ms = 0;
static String initial_file_name = "";
static volatile bool is_dragging_progress = false;

// ==================== 函数声明 ====================
static void clean_music_interface(void);
static void quitAudioTaskSafely();
static void stopAndReleaseResources();
static void play_song_by_index(int index, bool manual);
static void play_next_auto();
static void play_next_manual();
static void play_prev_manual();
static void load_playlist_to_ram(const char* initial_filename);
static void update_mode_ui();
static void update_filename_ui(const char* filename);
static void show_file_not_found_msgbox();
static void msgbox_btn_cb(lv_event_t * e);

static float linear_to_log_volume(int slider_value);
static int log_to_linear_slider(float log_volume);

// ==================== 播放列表与逻辑函数 ====================
static void load_playlist_to_ram(const char* initial_filename) {
    playlist.clear();
    current_song_index = 0;
    FsFile file;
    char line[256];
    if (file.open(SCAN_LIST_PATH, O_RDONLY)) {
        while (file.fgets(line, sizeof(line))) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0) continue;
            if (strncmp(line, "FileCount:", 10) == 0) continue;
            MusicEntry entry;
            strncpy(entry.name, line, sizeof(entry.name) - 1);
            snprintf(entry.full_path, sizeof(entry.full_path), "%s%s", MUSIC_PATH_PREFIX, line);
            entry.played = false;
            playlist.push_back(entry);
            if (initial_filename && strcmp(line, initial_filename) == 0) {
                current_song_index = playlist.size() - 1;
            }
        }
        file.close();
        Serial.printf("播放列表已加载: %d 首歌曲。当前索引: %d\n", playlist.size(), current_song_index);
    } else {
        Serial.println("无法打开音乐扫描列表！");
        if (initial_filename) {
            MusicEntry entry;
            strncpy(entry.name, initial_filename, sizeof(entry.name));
            snprintf(entry.full_path, sizeof(entry.full_path), "%s%s", MUSIC_PATH_PREFIX, initial_filename);
            entry.played = false;
            playlist.push_back(entry);
        }
    }
}

static void reset_random_flags() {
    for (auto &entry : playlist) {
        entry.played = false;
    }
    if (current_song_index >= 0 && current_song_index < playlist.size()) {
        playlist[current_song_index].played = true;
    }
}

static void play_song_by_index(int index, bool manual) {
    if (playlist.empty()) return;
    if (index < 0) index = playlist.size() - 1;
    if (index >= playlist.size()) index = 0;
    current_song_index = index;
    MusicEntry* entry = &playlist[current_song_index];
    entry->played = true;
    update_filename_ui(entry->name);
    manual_request_pending = manual;
    QueuedCommand qcmd;
    qcmd.cmd = CMD_OPEN_FILE;
    strncpy(qcmd.param.filePath, entry->full_path, sizeof(qcmd.param.filePath) - 1);
    qcmd.param.filePath[sizeof(qcmd.param.filePath) - 1] = '\0';
    xQueueSend(cmdQueue, &qcmd, portMAX_DELAY);
}

static void play_next_auto() {
    if (playlist.empty()) return;
    int next_index = current_song_index;
    switch (current_mode) {
        case MODE_SEQUENCE:
            next_index++;
            if (next_index >= playlist.size()) next_index = 0;
            break;
        case MODE_LOOP:
            next_index = current_song_index;
            break;
        case MODE_RANDOM: {
            std::vector<int> unplayed_indices;
            for (int i = 0; i < playlist.size(); i++) {
                if (!playlist[i].played) unplayed_indices.push_back(i);
            }
            if (unplayed_indices.empty()) {
                reset_random_flags();
                if (playlist.size() > 1) {
                    do { next_index = random(0, playlist.size()); } while(next_index == current_song_index);
                } else { next_index = 0; }
            } else {
                int r = random(0, unplayed_indices.size());
                next_index = unplayed_indices[r];
            }
        } break;
    }
    play_song_by_index(next_index, false);
}

static void play_next_manual() {
    if (playlist.empty()) return;
    int next_index = current_song_index;
    if (current_mode == MODE_RANDOM) {
        std::vector<int> unplayed_indices;
        for (int i = 0; i < playlist.size(); i++) {
            if (!playlist[i].played) unplayed_indices.push_back(i);
        }
        if (unplayed_indices.empty()) {
            reset_random_flags();
            next_index = random(0, playlist.size());
        } else {
            int r = random(0, unplayed_indices.size());
            next_index = unplayed_indices[r];
        }
        play_song_by_index(next_index, true);
    } else {
        next_index++;
        if (next_index >= playlist.size()) next_index = 0;
        play_song_by_index(next_index, true);
    }
}

static void play_prev_manual() {
    if (playlist.empty()) return;
    int next_index = current_song_index - 1;
    if (next_index < 0) next_index = playlist.size() - 1;
    play_song_by_index(next_index, true);
}

static void show_file_not_found_msgbox(void) {
    if (music_msgbox_bg) return;
    music_msgbox_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(music_msgbox_bg, 240, 280);
    lv_obj_align(music_msgbox_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(music_msgbox_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_msgbox_bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(music_msgbox_bg, 0, 0);
    lv_obj_clear_flag(music_msgbox_bg, LV_OBJ_FLAG_SCROLLABLE);

    music_msgbox = lv_obj_create(music_msgbox_bg);
    lv_obj_set_size(music_msgbox, 200, 150);
    lv_obj_align(music_msgbox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(music_msgbox, lv_color_make(50, 50, 50), 0);
    lv_obj_set_style_border_color(music_msgbox, lv_color_white(), 0);
    lv_obj_set_style_border_width(music_msgbox, 2, 0);
    lv_obj_set_style_radius(music_msgbox, 10, 0);
    lv_obj_clear_flag(music_msgbox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(music_msgbox);
    lv_label_set_text(label, "找不到该文件\n是否重新扫描");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &chinese_24, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -25);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_cont = lv_obj_create(music_msgbox);
    lv_obj_set_size(btn_cont, 180, 40);
    lv_obj_align(btn_cont, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_yes = lv_btn_create(btn_cont);
    lv_obj_set_size(btn_yes, 70, 30);
    lv_obj_set_style_bg_color(btn_yes, lv_color_make(0, 100, 0), 0);
    lv_obj_add_event_cb(btn_yes, msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_yes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * label_yes = lv_label_create(btn_yes);
    lv_label_set_text(label_yes, "是");
    lv_obj_set_style_text_color(label_yes, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_yes, &chinese_24, 0);
    lv_obj_center(label_yes);
    lv_obj_clear_flag(label_yes, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_no = lv_btn_create(btn_cont);
    lv_obj_set_size(btn_no, 70, 30);
    lv_obj_set_style_bg_color(btn_no, lv_color_make(100, 0, 0), 0);
    lv_obj_add_event_cb(btn_no, msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_no, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * label_no = lv_label_create(btn_no);
    lv_label_set_text(label_no, "否");
    lv_obj_set_style_text_color(label_no, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_no, &chinese_24, 0);
    lv_obj_center(label_no);
    lv_obj_clear_flag(label_no, LV_OBJ_FLAG_SCROLLABLE);
}

static void msgbox_btn_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    const char * txt = lv_label_get_text(label);
    if (strcmp(txt, "是") == 0) {
        fs_do_exit();
        clean_music_interface();
        destroy_file_selection_list();
        if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_force_scan = true;
            xSemaphoreGive(sd_state_mutex);
        }
    } else {
        if (music_msgbox_bg) { lv_obj_del(music_msgbox_bg); music_msgbox_bg = NULL; music_msgbox = NULL; }
        if (current_song_index >= 0 && current_song_index < playlist.size()) {
            playlist[current_song_index].played = true;
        }
        play_next_auto();
    }
}

// ==================== UI 辅助函数 ====================
static void update_filename_ui(const char* full_filename) {
    if (!file_name_label) return;
    char name_buf[128];
    strncpy(name_buf, full_filename, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf)-1] = '\0';
    char* dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';
    lv_label_set_text(file_name_label, name_buf);
    lv_obj_set_style_text_color(file_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(file_name_label, LV_OPA_COVER, 0);
    lv_label_set_long_mode(file_name_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(file_name_label, LV_SIZE_CONTENT);
    lv_obj_update_layout(file_name_label);
    int width = lv_obj_get_width(file_name_label);
    if (width > 240) {
        lv_obj_set_width(file_name_label, 240);
        lv_label_set_long_mode(file_name_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else {
        lv_obj_set_width(file_name_label, width);
        lv_label_set_long_mode(file_name_label, LV_LABEL_LONG_CLIP);
    }
    lv_obj_align(file_name_label, LV_ALIGN_CENTER, -5, -30);
}

static void update_mode_ui() {
    if (!mode_label) return;
    const char* icon = "";
    switch(current_mode) {
        case MODE_SEQUENCE: icon = LV_SYMBOL_RIGHT; break;
        case MODE_RANDOM:   icon = LV_SYMBOL_SHUFFLE; break;
        case MODE_LOOP:     icon = LV_SYMBOL_LOOP; break;
    }
    lv_label_set_text(mode_label, icon);
}

static int log_to_linear_slider(float log_volume) {
    if (log_volume <= 0.009f) return 0;
    if (log_volume >= 1.0f) return 100;
    float normalized = (log_volume - 0.009f) / (1.0f - 0.009f);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;
    return (int)(100.0f * sqrtf(normalized));
}

static float linear_to_log_volume(int slider_value) {
    if (slider_value <= 0) return 0.009f;
    if (slider_value >= 100) return 1.0f;
    float normalized = slider_value / 100.0f;
    return 0.009f + (1.0f - 0.009f) * (normalized * normalized);
}

// ==================== UI 回调函数 ====================
static void mode_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (current_mode == MODE_SEQUENCE) current_mode = MODE_RANDOM;
        else if (current_mode == MODE_RANDOM) current_mode = MODE_LOOP;
        else current_mode = MODE_SEQUENCE;
        if (current_mode == MODE_RANDOM) reset_random_flags();
        update_mode_ui();
        preferences.begin("watch", false);
        preferences.putInt("Playback_Mode", (int)current_mode);
        preferences.end();
    }
}

static void prev_btn_event_cb(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) play_prev_manual(); }
static void next_btn_event_cb(lv_event_t *e) { if (lv_event_get_code(e) == LV_EVENT_CLICKED) play_next_manual(); }

static void play_pause_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        QueuedCommand qcmd = {CMD_TOGGLE_PAUSE, {0}};
        xQueueSend(cmdQueue, &qcmd, 0);
    }
}

static void volume_slider_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        int32_t slider_val = lv_slider_get_value(volume_slider);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        int32_t slider_val = lv_slider_get_value(volume_slider);
        float log_volume = linear_to_log_volume(slider_val);
        preferences.begin("watch", false);
        preferences.putFloat("Volume", log_volume);
        preferences.end();
        QueuedCommand qcmd = {CMD_SET_VOLUME, {.volume = log_volume}};
        xQueueSend(cmdQueue, &qcmd, 0);
    }
}

static void progress_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) { is_dragging_progress = true; }
    if (code == LV_EVENT_RELEASED && playerStatus.fileOpened) {
        is_dragging_progress = false;
        int32_t prog_val = lv_slider_get_value(progress_slider);
        uint32_t seek_pos = (uint32_t)((float)prog_val / 1000.0f * playerStatus.fileSizeBytes);
        QueuedCommand qcmd = {CMD_SEEK_BYTES, {.seekPos = seek_pos}};
        xQueueSend(cmdQueue, &qcmd, 0);
    }
}

// ==================== 核心 UI 构建 ====================
void music_player_ui(lv_obj_t* parent) {
    memset((void*)&playerStatus, 0, sizeof(PlayerStatus));
    manual_request_pending = false;
    
    //初始化 ESP-DSP
    if (!dsp_initialized) {
        esp_err_t ret = dsps_fft2r_init_fc32(NULL, FFT_SAMPLES);
        if (ret != ESP_OK) {
            Serial.println("ESP-DSP FFT 初始化失败！");
        } else {
            dsps_wind_hann_f32(hamming_window, FFT_SAMPLES);
            dsp_initialized = true;
            Serial.println("ESP-DSP FFT 初始化成功。");
        }
    }

    preferences.begin("watch", false);
    float saved_volume = preferences.getFloat("Volume", 0.5f);
    int saved_mode = preferences.getInt("Playback_Mode", MODE_SEQUENCE);
    preferences.end();
    playerStatus.volume = saved_volume;
    current_mode = (PlayMode)saved_mode;

    load_playlist_to_ram(initial_file_name.c_str());

    lv_obj_set_size(parent, 240, 280);
    
    file_name_label = lv_label_create(parent);
    lv_obj_set_style_text_font(file_name_label, &chinese_24, 0);
    lv_obj_set_style_text_align(file_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(file_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(file_name_label, LV_OPA_COVER, 0);
    if (!playlist.empty()) { update_filename_ui(playlist[current_song_index].name); }
    else { lv_label_set_text(file_name_label, "无音乐文件"); }

    volume_slider = lv_slider_create(parent);
    lv_obj_set_size(volume_slider, 140, 10);
    lv_obj_set_pos(volume_slider, 70, 25);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, log_to_linear_slider(saved_volume), LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_RELEASED, NULL);

    mode_btn = lv_btn_create(parent);
    lv_obj_set_size(mode_btn, 40, 40);
    lv_obj_set_pos(mode_btn, 10, 5);
    lv_obj_set_style_radius(mode_btn, 10, 0);
    lv_obj_set_style_bg_color(mode_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_add_event_cb(mode_btn, mode_btn_event_cb, LV_EVENT_CLICKED, NULL);
    mode_label = lv_label_create(mode_btn);
    update_mode_ui();
    lv_obj_center(mode_label);

    volume_icon = lv_label_create(parent);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_pos(volume_icon, 135, 0);
    lv_obj_set_style_text_color(volume_icon, lv_color_white(), 0);
    lv_obj_set_style_text_opa(volume_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_zoom(volume_icon, 350, 0);

    progress_slider = lv_slider_create(parent);
    lv_obj_set_size(progress_slider, 210, 10);
    lv_obj_align(progress_slider, LV_ALIGN_CENTER, 0, 45);
    lv_slider_set_range(progress_slider, 0, 1000);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_PRESSING, NULL);

    int start_x = (240 - CANVAS_WIDTH) / 2 - 13;
    int base_y = 160;
    fft_canvas = lv_canvas_create(parent);
    lv_obj_set_size(fft_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_set_pos(fft_canvas, start_x, base_y - CANVAS_HEIGHT);
    lv_canvas_set_buffer(fft_canvas, canvas_buf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(fft_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);

    int btn_diameter = 50, spacing = 10;
    int total_width = 3 * btn_diameter + 2 * spacing;
    int start_x_btns = (240 - total_width - 32) / 2;

    prev_btn = lv_btn_create(parent);
    lv_obj_set_size(prev_btn, btn_diameter, btn_diameter);
    lv_obj_set_style_radius(prev_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0xC0A000), 0);
    lv_obj_set_pos(prev_btn, start_x_btns, 280 - btn_diameter - 30);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_label_icon = lv_label_create(prev_btn);
    lv_label_set_text(prev_label_icon, LV_SYMBOL_PREV);
    lv_obj_center(prev_label_icon);

    play_pause_btn = lv_btn_create(parent);
    lv_obj_set_size(play_pause_btn, btn_diameter, btn_diameter);
    lv_obj_set_style_radius(play_pause_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play_pause_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_pos(play_pause_btn, start_x_btns + btn_diameter + spacing, 280 - btn_diameter - 30);
    lv_obj_add_event_cb(play_pause_btn, play_pause_btn_event_cb, LV_EVENT_CLICKED, NULL);
    play_pause_label = lv_label_create(play_pause_btn);
    lv_label_set_text(play_pause_label, LV_SYMBOL_PLAY);
    lv_obj_center(play_pause_label);

    next_btn = lv_btn_create(parent);
    lv_obj_set_size(next_btn, btn_diameter, btn_diameter);
    lv_obj_set_style_radius(next_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0xC0A000), 0);
    lv_obj_set_pos(next_btn, start_x_btns + 2*(btn_diameter + spacing), 280 - btn_diameter - 30);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label_icon = lv_label_create(next_btn);
    lv_label_set_text(next_label_icon, LV_SYMBOL_NEXT);
    lv_obj_center(next_label_icon);

    // UI 定时器
    progress_timer = lv_timer_create([](lv_timer_t *timer){
        if (is_dragging_progress) return;

        if (playerStatus.songFinished) {
            playerStatus.songFinished = false;
            play_next_auto();
            return;
        }

        if (playerStatus.openFailed) {
            playerStatus.openFailed = false;
            String failedPath = String(playerStatus.lastFailedPath);
            int failedIndex = -1;
            for (size_t i = 0; i < playlist.size(); i++) {
                if (failedPath.equals(playlist[i].full_path)) { failedIndex = i; break; }
            }
            if (manual_request_pending) {
                manual_request_pending = false;
                show_file_not_found_msgbox();
            } else {
                if (failedIndex != -1) playlist[failedIndex].played = true;
                play_next_auto();
            }
            return;
        }

        if (playerStatus.fileOpened && playerStatus.fileSizeBytes > 0) {
            float progress = (float)playerStatus.currentPosBytes / playerStatus.fileSizeBytes;
            int32_t prog_val = (int32_t)(progress * 1000.0f);
            lv_slider_set_value(progress_slider, prog_val, LV_ANIM_OFF);
            if (playerStatus.isPlaying) {
                lv_label_set_text(play_pause_label, playerStatus.isPaused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
            } else {
                lv_label_set_text(play_pause_label, LV_SYMBOL_PLAY);
            }
        }

        //FFT 处理
        if (dsp_initialized) {
            // 从环形缓冲区提取最新的 1024 个采样
            uint32_t read_start = fft_write_idx;
            for (int i = 0; i < FFT_SAMPLES; i++) {
                fft_ui_buf[i] = fft_ring_buf[(read_start + i) % FFT_RING_BUF_SIZE];
            }

            // 构建复数输入
            for (int i = 0; i < FFT_SAMPLES; i++) {
                fft_input[i * 2 + 0] = (float)fft_ui_buf[i] * hamming_window[i];
                fft_input[i * 2 + 1] = 0.0f;
            }

            // 执行 FFT
            dsps_fft2r_fc32(fft_input, FFT_SAMPLES);
            dsps_bit_rev_fc32(fft_input, FFT_SAMPLES);

            // 计算每个频率点的幅值
            for (int i = 0; i < FFT_BINS; i++) {
                int bin_idx = bin_map[i];
                if (bin_idx >= FFT_SAMPLES / 2) bin_idx = FFT_SAMPLES / 2 - 1;
                float real = fft_input[bin_idx * 2 + 0];
                float imag = fft_input[bin_idx * 2 + 1];
                fft_magnitudes[i] = sqrtf(real * real + imag * imag);
            }
            fft_new_data = true;
        }

        // 绘制频谱
        if (fft_new_data) {
            fft_new_data = false;
            float maxVal = 0.001f;
            for (int i = 0; i < FFT_BINS; i++) if (fft_magnitudes[i] > maxVal) maxVal = fft_magnitudes[i];
            float normFactor = (maxVal > REF_MAX_MAGNITUDE) ? maxVal : REF_MAX_MAGNITUDE;
            
            lv_canvas_fill_bg(fft_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
            lv_draw_rect_dsc_t rect_dsc;
            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.radius = 2;
            rect_dsc.bg_opa = LV_OPA_COVER;
            rect_dsc.border_width = 0;
            
            int bar_x = 0;
            for (int i = 0; i < FFT_BINS; i++) {
                float ratio = fft_magnitudes[i] / normFactor;
                int h = (int)(ratio * MAX_BAR_HEIGHT);
                if (h < 2) h = 2;
                if (h > MAX_BAR_HEIGHT) h = MAX_BAR_HEIGHT;

                if (h > fft_peak_magnitudes[i]) {
                    fft_peak_magnitudes[i] = h;
                } else {
                    fft_peak_magnitudes[i] *= PEAK_FALL_SPEED;
                    if (fft_peak_magnitudes[i] < 2) fft_peak_magnitudes[i] = 2;
                }

                int display_h = (h > (int)fft_peak_magnitudes[i]) ? h : (int)fft_peak_magnitudes[i];
                uint16_t hue = (i * 360) / FFT_BINS;
                rect_dsc.bg_color = lv_color_hsv_to_rgb(hue, 100, 100);
                
                lv_canvas_draw_rect(fft_canvas, bar_x, CANVAS_HEIGHT - display_h, BAR_WIDTH, display_h, &rect_dsc);
                bar_x += BAR_WIDTH + BAR_SPACING;
            }
            lv_obj_invalidate(fft_canvas);
        }
    }, 20, NULL);

    if (cmdQueue == NULL) {
        cmdQueue = xQueueCreate(5, sizeof(QueuedCommand));
        xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 2, &audioTaskHandle, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    QueuedCommand volCmd = {CMD_SET_VOLUME, {.volume = saved_volume}};
    xQueueSend(cmdQueue, &volCmd, portMAX_DELAY);

    if (!playlist.empty()) {
        play_song_by_index(current_song_index, false);
    }
}

//清理
static void clean_music_interface(void) {
    quitAudioTaskSafely();
    if (progress_timer) { lv_timer_del(progress_timer); progress_timer = NULL; }
    stopAndReleaseResources();
    playlist.clear();
    
    if (volume_slider) { lv_obj_del(volume_slider); volume_slider = NULL; }
    if (progress_slider) { lv_obj_del(progress_slider); progress_slider = NULL; }
    if (play_pause_btn) { lv_obj_del(play_pause_btn); play_pause_btn = NULL; }
    if (fft_canvas) { lv_obj_del(fft_canvas); fft_canvas = NULL; }
    if (prev_btn) { lv_obj_del(prev_btn); prev_btn = NULL; }
    if (next_btn) { lv_obj_del(next_btn); next_btn = NULL; }
    if (mode_btn) { lv_obj_del(mode_btn); mode_btn = NULL; }
    if (file_name_label) { lv_obj_del(file_name_label); file_name_label = NULL; }
    if (volume_icon) { lv_obj_del(volume_icon); volume_icon = NULL; }
    if (music_msgbox_bg) { lv_obj_del(music_msgbox_bg); music_msgbox_bg = NULL; music_msgbox = NULL; }
    Serial.println("音乐界面已清理，内存列表已清除。");
}

static void music_file_selected_cb(const char* filename, void* user_data) {
    if (strcmp(filename, SCAN_SPECIAL_FILENAME) == 0) {
        fs_do_exit();
        clean_music_interface();
        destroy_file_selection_list();
        if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) { sd_force_scan = true; xSemaphoreGive(sd_state_mutex); }
        return;
    }
    Serial.printf("已选择音乐: %s\n", filename);
    initial_file_name = String(filename);
    need_switch_to_music = true;
}

static void gpio_check_cb(lv_timer_t* timer) {
    uint32_t now = lv_tick_get();
    static bool current_sd_state = true;
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) { current_sd_state = sd_card_inserted; xSemaphoreGive(sd_state_mutex); }
    
    if (last_sd_card_state == true && current_sd_state == false) {
        Serial.println("SD卡拔出");
        fs_do_exit();
        if (out) { out->stop(); }
        clean_music_interface();
        destroy_file_selection_list();
        if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
        music_state_machine = false;
        need_switch_to_music = false;
        last_sd_card_state = current_sd_state;
        return;
    }
    last_sd_card_state = current_sd_state;

    bool BUTTON2 = digitalRead(BUTTON_2);
    if (BUTTON2 != last_BUTTON2_level) { stable_start_BUTTON2 = now; last_BUTTON2_level = BUTTON2; }
    else if (BUTTON2 == 1 && (now - stable_start_BUTTON2) >= DEBOUNCE_THRESHOLD) {
        if (now - last_state_change_ms > STATE_CHANGE_COOLDOWN) {
            last_state_change_ms = now;
            if (music_state_machine) {
                if (fullscreen_container) lv_obj_move_foreground(fullscreen_container);
                clean_music_interface();
                file_selection_create(g_container, FILE_TYPE_MUSIC, music_file_selected_cb, NULL);
                music_state_machine = false;
            } else {
                fs_do_exit();
                destroy_file_selection_list();
                if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
                return;
            }
        }
    }
    bool BUTTON1 = digitalRead(BUTTON_1);
    bool BUTTON3 = digitalRead(BUTTON_3);
    
    // 仅在音乐播放界面生效
    if (music_state_machine) {
        if(BUTTON1){
            // 音量加 1
            int32_t current_val = lv_slider_get_value(volume_slider);
            if (current_val < 100) {
                current_val += 1;
                lv_slider_set_value(volume_slider, current_val, LV_ANIM_OFF);
                
                // 同步到音频输出和保存设置
                float log_volume = linear_to_log_volume(current_val);
                preferences.begin("watch", false);
                preferences.putFloat("Volume", log_volume);
                preferences.end();
                
                QueuedCommand qcmd = {CMD_SET_VOLUME, {.volume = log_volume}};
                xQueueSend(cmdQueue, &qcmd, 0);
            }
        } else if(BUTTON3){
            // 音量减 1
            int32_t current_val = lv_slider_get_value(volume_slider);
            if (current_val > 0) {
                current_val -= 1;
                lv_slider_set_value(volume_slider, current_val, LV_ANIM_OFF);
                
                // 同步到音频输出和保存设置
                float log_volume = linear_to_log_volume(current_val);
                preferences.begin("watch", false);
                preferences.putFloat("Volume", log_volume);
                preferences.end();
                
                QueuedCommand qcmd = {CMD_SET_VOLUME, {.volume = log_volume}};
                xQueueSend(cmdQueue, &qcmd, 0);
            }
        }
    }

    if (need_switch_to_music) {
        destroy_file_selection_list();
        music_player_ui(g_container);
        music_state_machine = true;
        need_switch_to_music = false;
    }
}

// ==================== 音频任务 ====================
static void onSample(int16_t left, int16_t right, int rate) {
    (void)rate;
    int16_t mono = (left + right) / 2;
    fft_ring_buf[fft_write_idx] = mono;
    fft_write_idx = (fft_write_idx + 1) % FFT_RING_BUF_SIZE;
}

static void audioTask(void *pvParameters) {
    (void)pvParameters;
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK, I2S_LRCLK, I2S_DIN);
    out->SetGain(0.1f);

    while (1) {
        if (playerStatus.taskQuitFlag) {
            if (mp3 && mp3->isRunning()) mp3->stop();
            if (mp3) { delete mp3; mp3 = nullptr; }
            if (audio_file) { delete audio_file; audio_file = nullptr; }
            if (out) { delete out; out = nullptr; }
            if (cmdQueue) xQueueReset(cmdQueue);
            vTaskDelete(NULL);
            break;
        }

        QueuedCommand qcmd;
        if (xQueueReceive(cmdQueue, &qcmd, 0) == pdTRUE) {
            switch (qcmd.cmd) {
                case CMD_OPEN_FILE:
                    if (mp3) { mp3->stop(); delete mp3; mp3 = nullptr; }
                    if (audio_file) { delete audio_file; audio_file = nullptr; }
                    audio_file = new AudioFileSourceSdFat(qcmd.param.filePath);
                    if (audio_file->isOpen()) {
                        playerStatus.fileOpened = true;
                        playerStatus.fileSizeBytes = audio_file->getSize();
                        playerStatus.currentPosBytes = 0;
                        mp3 = new AudioGeneratorMP3();
                        mp3->setSampleCallback(onSample);
                        mp3->begin(audio_file, out);
                        playerStatus.isPlaying = true;
                        playerStatus.isPaused = false;
                        playerStatus.openFailed = false;
                    } else {
                        playerStatus.fileOpened = false;
                        playerStatus.openFailed = true;
                        strncpy(playerStatus.lastFailedPath, qcmd.param.filePath, sizeof(playerStatus.lastFailedPath) - 1);
                        playerStatus.lastFailedPath[sizeof(playerStatus.lastFailedPath) - 1] = '\0';
                    }
                    break;
                case CMD_PLAY:
                    if (audio_file && audio_file->isOpen()) { playerStatus.isPaused = false; playerStatus.isPlaying = true; }
                    break;
                case CMD_PAUSE:
                    if (playerStatus.isPlaying) playerStatus.isPaused = true;
                    break;
                case CMD_TOGGLE_PAUSE:
                    if (audio_file && audio_file->isOpen()) {
                        playerStatus.isPaused = !playerStatus.isPaused;
                        if (!playerStatus.isPaused && !mp3->isRunning()) { mp3->begin(audio_file, out); playerStatus.isPlaying = true; }
                    }
                    break;
                case CMD_SEEK_BYTES:
                    if (audio_file && audio_file->isOpen()) {
                        if (qcmd.param.seekPos < playerStatus.fileSizeBytes) { audio_file->seek(qcmd.param.seekPos, SEEK_SET); playerStatus.currentPosBytes = qcmd.param.seekPos; }
                    }
                    break;
                case CMD_SET_VOLUME:
                    if (out != nullptr) { out->SetGain(qcmd.param.volume); }
                    break;
                case CMD_QUIT_TASK:
                    playerStatus.taskQuitFlag = true;
                    break;
                default: break;
            }
        }

        if (playerStatus.isPlaying && !playerStatus.isPaused) {
            if (mp3 && mp3->isRunning()) {
                if (!mp3->loop()) {
                    playerStatus.isPlaying = false;
                    playerStatus.isPaused = false;
                    playerStatus.songFinished = true;
                }
            }
        }

        if (audio_file && audio_file->isOpen()) {
            static uint32_t last_ui_update = 0;
            if (millis() - last_ui_update >= 100) {
                last_ui_update = millis();
                playerStatus.currentPosBytes = audio_file->getPos();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 导出函数
void fs_create_music(lv_obj_t* container) {
    g_container = container;
    if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
    last_BUTTON2_level = digitalRead(BUTTON_2);
    stable_start_BUTTON2 = lv_tick_get();
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) { last_sd_card_state = sd_card_inserted; xSemaphoreGive(sd_state_mutex); }
    else { last_sd_card_state = true; }
    gpio_timer = lv_timer_create(gpio_check_cb, TIMER_PERIOD, NULL);
    g_file_selection_instance = file_selection_create(container, FILE_TYPE_MUSIC, music_file_selected_cb, NULL);
}

void fs_cleanup_music(void) {
    destroy_file_selection_list();
    if (gpio_timer) { lv_timer_del(gpio_timer); gpio_timer = NULL; }
}

void quitAudioTaskSafely() {
    if (audioTaskHandle != NULL) {
        playerStatus.taskQuitFlag = true;
        if (cmdQueue != NULL) {
            QueuedCommand qcmd = {CMD_QUIT_TASK, {0}};
            xQueueSend(cmdQueue, &qcmd, 0);
        }
        uint32_t waitStart = millis();
        while (eTaskGetState(audioTaskHandle) != eDeleted && (millis() - waitStart) < 300) { vTaskDelay(pdMS_TO_TICKS(10)); }
        audioTaskHandle = NULL;
    }
    if (cmdQueue != NULL) { vQueueDelete(cmdQueue); cmdQueue = NULL; }
}

void stopAndReleaseResources() {
    if (out) { out->stop(); delete out; out = nullptr; }
    memset((void*)&playerStatus, 0, sizeof(PlayerStatus));
    fft_new_data = false;
    
    fft_write_idx = 0;
    memset((void*)fft_ring_buf, 0, sizeof(fft_ring_buf));
    
    is_dragging_progress = false;
    memset(fft_peak_magnitudes, 0, sizeof(fft_peak_magnitudes));
}