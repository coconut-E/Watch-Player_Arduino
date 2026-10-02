#include "SDScan.h"
#include <LovyanGFX.hpp>

// ==================== 文件夹配置 ====================
const char* SCANLIST_FOLDER = "/ScanList";
const char* TRIGGER_FILE = "/修改请删除此文件以触发重新扫描.txt";

// ==================== 扩展名数组定义 ====================
const char* IMAGE_EXTENSIONS[] = {".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG"};
const char* VIDEO_EXTENSIONS[] = {".mjpeg", ".MJPEG"};
const char* MUSIC_EXTENSIONS[] = {".mp3", ".MP3"};
const char* NOVEL_EXTENSIONS[] = {".txt", ".TXT"};
const char* COMIC_EXTENSIONS[] = {".cmj", ".CMJ"}; 

// ==================== 扫描配置定义 ====================
FileCategory categories[] = {
    {"/图片", "/ScanList/picture.txt", IMAGE_EXTENSIONS, 6, true},
    {"/视频", "/ScanList/video.txt", VIDEO_EXTENSIONS, 2, true},
    {"/音乐", "/ScanList/music.txt", MUSIC_EXTENSIONS, 2, true},
    {"/小说", "/ScanList/novel.txt", NOVEL_EXTENSIONS, 2, true},
    {"/漫画", "/ScanList/comic.txt", COMIC_EXTENSIONS, 2, true} 
};
const int CATEGORY_COUNT = sizeof(categories) / sizeof(categories[0]);

// ==================== 全局对象定义 ====================
SdFs sd;
//SPIClass sd_spi(HSPI);
SPIClass sd_spi(SPI1_HOST);

// ==================== 回调函数指针 ====================
static ScanProgressCallback scanProgressCallback = nullptr;
static ScanErrorCallback scanErrorCallback = nullptr;
static ScanCompleteCallback scanCompleteCallback = nullptr;

// ==================== 全局状态变量定义 ====================
SDScanStatus scanStatus = {
    false,   // sdInitialized
    0,       // totalSizeMB
    0,       // freeSizeMB
    false,   // rescanRequired
    false,   // rescanPerformed
    0,       // scanTimeMs
    0,       // totalFilesFound
    {0, 0, 0, 0, 0},  // categoryFiles 初始化为5个0
    "",      // lastError
    -1,      // currentCategoryIndex
    0        // currentFileCount
};

// ==================== 回调函数设置 ====================
void setScanProgressCallback(ScanProgressCallback callback) {
    scanProgressCallback = callback;
}

void setScanErrorCallback(ScanErrorCallback callback) {
    scanErrorCallback = callback;
}

void setScanCompleteCallback(ScanCompleteCallback callback) {
    scanCompleteCallback = callback;
}

// ==================== 函数实现 ====================

// 初始化SD卡
bool initializeSDCard() {
    // 初始化SPI
    sd_spi.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    SdSpiConfig spiConfig(SD_CS, SHARED_SPI, SPI_SPEED, &sd_spi);
    
    // 初始化SD卡
    if (!sd.begin(spiConfig)) {
        scanStatus.lastError = "SD卡初始化失败";
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError.c_str());
        }
        scanStatus.sdInitialized = false;
        return false;
    }
    
    // 获取SD卡信息
    scanStatus.sdInitialized = true;
    scanStatus.totalSizeMB = sd.card()->sectorCount() / 2048;
    
    // 计算可用空间
    uint32_t freeSectors = sd.dataStartSector() + sd.clusterCount() * sd.sectorsPerCluster();
    scanStatus.freeSizeMB = freeSectors * 512 / (1024 * 1024);
    
    return true;
}

// 检查是否需要重新扫描
bool checkNeedRescan() {
    scanStatus.rescanRequired = !sd.exists(TRIGGER_FILE);
    return scanStatus.rescanRequired;
}

// 确保目录和文件存在
void ensureDirectoriesAndFiles() {
    // 创建ScanList目录
    if (!sd.exists(SCANLIST_FOLDER)) {
        if (!sd.mkdir(SCANLIST_FOLDER)) {
            if (scanErrorCallback) {
                scanErrorCallback("创建ScanList目录失败");
            }
        }
    }

    // 创建目标扫描目录
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        const char* folder = categories[i].folder;
        if (!sd.exists(folder)) {
            if (!sd.mkdir(folder)) {
                if (scanErrorCallback) {
                    scanErrorCallback(("创建目录失败: " + String(folder)).c_str());
                }
            }
        }
    }

    if (!sd.exists("/壁纸")) {
        sd.mkdir("/壁纸");
    }

    // 检查并初始化列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        const char* listFile = categories[i].listFile;
        if (!sd.exists(listFile)) {
            initEmptyListFile(listFile);
        }
    }
}

// 初始化空列表文件
void initEmptyListFile(const char* listFile) {
    FsFile file;
    if (!file.open(listFile, O_WRITE | O_CREAT | O_TRUNC)) {
        if (scanErrorCallback) {
            scanErrorCallback(("初始化空列表文件失败: " + String(listFile)).c_str());
        }
        return;
    }
    unsigned long scanTime = millis();
    file.printf("ScanTime:%lu\n", scanTime);
    file.printf("FileCount:0\n");
    file.sync();
    file.close();
}

// 创建触发文件
void createTriggerFile() {
    FsFile triggerFile;
    if (triggerFile.open(TRIGGER_FILE, O_WRITE | O_CREAT | O_TRUNC)) {
        triggerFile.println("此文件用于控制SD卡扫描。");
        triggerFile.println("如需重新扫描所有文件夹，请删除此文件。");
        triggerFile.println("");
        triggerFile.println("最后扫描时间: " + String(millis()) + "ms");
        triggerFile.sync();
        triggerFile.close();
    } else {
        if (scanErrorCallback) {
            scanErrorCallback("无法创建触发文件");
        }
    }
}

// 删除触发文件
void deleteTriggerFile() {
    if (sd.exists(TRIGGER_FILE)) {
        if (!sd.remove(TRIGGER_FILE)) {
            if (scanErrorCallback) {
                scanErrorCallback("无法删除触发文件");
            }
        }
    }
}


// 扫描指定类别
bool scanCategory(FileCategory& category, int categoryIndex) {
    scanStatus.currentCategoryIndex = categoryIndex;
    scanStatus.currentFileCount = 0;
    
    // 通知开始扫描当前类别
    if (scanProgressCallback) {
        scanProgressCallback(categoryIndex, 0, category.folder);
    }
    
    // 前置检查
    if (!sd.exists(category.folder)) {
        scanStatus.lastError = String("目标文件夹不存在: ") + category.folder;
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError.c_str());
        }
        return false;
    }
    
    // 打开列表文件（覆盖写入）
    FsFile listFile;
    if (!listFile.open(category.listFile, O_WRITE | O_CREAT | O_TRUNC)) {
        scanStatus.lastError = String("无法创建/打开列表文件: ") + category.listFile;
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError.c_str());
        }
        return false;
    }


    // 打开目标文件夹
    FsFile rootFolder;
    if (!rootFolder.open(category.folder)) {
        scanStatus.lastError = String("无法打开目标文件夹: ") + category.folder;
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError.c_str());
        }
        listFile.close();
        return false;
    }

    FsFile subFile;
    int fileCount = 0;
    char** fileList = new char*[MAX_FILES_PER_CATEGORY];
    
    // 收集文件信息
    while (subFile.openNext(&rootFolder, O_RDONLY)) {
        char filename[MAX_PATH_LENGTH];
        subFile.getName(filename, sizeof(filename));

        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
            subFile.close();
            continue;
        }

        if (subFile.isFile() && isFileType(filename, category.extensions, category.extCount)) {
            
            // 仅存储文件名
            fileList[fileCount] = new char[strlen(filename) + 1];
            strcpy(fileList[fileCount], filename);
            
            fileCount++;
            scanStatus.currentFileCount = fileCount;

            // 更新状态信息
            scanStatus.totalFilesFound++;
            
            // 回调进度更新
            if (scanProgressCallback) {
                scanProgressCallback(categoryIndex, fileCount, category.folder);
            }

            if (fileCount >= MAX_FILES_PER_CATEGORY) {
                if (scanErrorCallback) {
                    scanErrorCallback("文件数量超过限制，停止扫描");
                }
                break;
            }
        }

        subFile.close();
    }
    rootFolder.close();

    // 写入文件列表：仅写文件名，每行一个
    for (int i = 0; i < fileCount; i++) {
        listFile.println(fileList[i]); // 直接打印文件名，换行分隔
    }

    // 文件数量放到最后一行，格式FileCount:数字
    listFile.printf("FileCount:%d\n", fileCount);

    listFile.sync();
    listFile.close();

    // 清理内存：仅释放文件名数组
    for (int i = 0; i < fileCount; i++) {
        delete[] fileList[i];
    }
    delete[] fileList;


    // 更新类别文件计数（边界改为 CATEGORY_COUNT）
    if (categoryIndex >= 0 && categoryIndex < CATEGORY_COUNT) {
        scanStatus.categoryFiles[categoryIndex] = fileCount;
    }
    
    // 重置当前扫描状态
    scanStatus.currentCategoryIndex = -1;
    scanStatus.currentFileCount = 0;
    
    return true;
}

// 写入文件列表
void writeFileList(FileCategory& category) {
    // 查找类别索引
    int categoryIndex = -1;
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (strcmp(category.folder, categories[i].folder) == 0) {
            categoryIndex = i;
            break;
        }
    }
    scanCategory(category, categoryIndex);
}

// 检查文件类型
bool isFileType(const char* filename, const char** extensions, int extCount) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;

    for (int i = 0; i < extCount; i++) {
        if (strcasecmp(dot, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

// 执行扫描流程
void performScan() {    
    if (scanProgressCallback) {
        scanProgressCallback(-1, 0, "开始执行扫描流程...");
    }
    
    // 删除旧列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (sd.exists(categories[i].listFile)) {
            sd.remove(categories[i].listFile);
        }
    }
    
    // 重新创建列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        initEmptyListFile(categories[i].listFile);
    }
    
    // 重置扫描状态（使用 CATEGORY_COUNT）
    scanStatus.totalFilesFound = 0;
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        scanStatus.categoryFiles[i] = 0;
    }
    
    // 执行扫描
    unsigned long totalStartTime = millis();
    
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (categories[i].shouldScan) {
            scanCategory(categories[i], i);
        }
    }
    
    scanStatus.scanTimeMs = millis() - totalStartTime;
    scanStatus.rescanPerformed = true;
    
    // 创建标记文件
    createTriggerFile();
    
    // 回调扫描完成
    if (scanCompleteCallback) {
        scanCompleteCallback();
    }
}

// ==================== 状态信息获取函数 ====================

// 获取扫描状态
SDScanStatus getScanStatus() {
    return scanStatus;
}

// 打印扫描状态
void printScanStatus() {
    Serial.println("\n========== SD卡扫描状态 ==========");
    Serial.printf("SD卡初始化: %s\n", scanStatus.sdInitialized ? "成功" : "失败");
    Serial.printf("SD卡总容量: %u MB\n", scanStatus.totalSizeMB);
    Serial.printf("SD卡剩余空间: %u MB\n", scanStatus.freeSizeMB);
    Serial.printf("重新扫描需要: %s\n", scanStatus.rescanRequired ? "是" : "否");
    Serial.printf("重新扫描已执行: %s\n", scanStatus.rescanPerformed ? "是" : "否");
    Serial.printf("扫描总耗时: %lu ms\n", scanStatus.scanTimeMs);
    Serial.printf("总共找到文件: %d 个\n", scanStatus.totalFilesFound);
    
    if (scanStatus.rescanPerformed) {
        Serial.println("各类别文件数量:");
        Serial.printf("  图片: %d 个\n", scanStatus.categoryFiles[0]);
        Serial.printf("  视频: %d 个\n", scanStatus.categoryFiles[1]);
        Serial.printf("  音乐: %d 个\n", scanStatus.categoryFiles[2]);
        Serial.printf("  小说: %d 个\n", scanStatus.categoryFiles[3]);
        Serial.printf("  漫画: %d 个\n", scanStatus.categoryFiles[4]); 
    }
    
    if (scanStatus.lastError.length() > 0) {
        Serial.printf("最后错误: %s\n", scanStatus.lastError.c_str());
    }
    Serial.println("=================================\n");
}

// 检查SD卡是否就绪
bool isSDCardReady() {
    return scanStatus.sdInitialized;
}

// 获取SD卡总容量
uint32_t getSDCardSizeMB() {
    return scanStatus.totalSizeMB;
}

// 获取SD卡剩余空间
uint32_t getSDCardFreeSpaceMB() {
    return scanStatus.freeSizeMB;
}

// 获取总共找到的文件数
int getTotalFilesFound() {
    return scanStatus.totalFilesFound;
}

// 获取指定类别的文件数
int getCategoryFileCount(int categoryIndex) {
    if (categoryIndex >= 0 && categoryIndex < CATEGORY_COUNT) {
        return scanStatus.categoryFiles[categoryIndex];
    }
    return 0;
}

// 获取当前正在扫描的类别索引
int getCurrentCategoryIndex() {
    return scanStatus.currentCategoryIndex;
}

// 获取当前类别已扫描的文件数
int getCurrentFileCount() {
    return scanStatus.currentFileCount;
}

// 重置扫描状态
void resetScanStatus() {
    scanStatus = {
        scanStatus.sdInitialized,  // 保持SD卡初始化状态
        scanStatus.totalSizeMB,    // 保持容量信息
        scanStatus.freeSizeMB,     // 保持剩余空间
        false,                     // 重置重新扫描需要
        false,                     // 重置重新扫描已执行
        0,                         // 重置扫描时间
        0,                         // 重置总文件数
        {0, 0, 0, 0, 0},           // 重置各分类文件数（5个0）
        "",                        // 清空错误信息
        -1,                        // 重置当前类别索引
        0                          // 重置当前文件数
    };
}