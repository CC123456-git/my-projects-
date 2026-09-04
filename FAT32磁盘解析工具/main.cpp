/**
 * @file    main.cpp
 * @brief   FAT32 文件浏览器 - 控制台主程序
 *
 * 本程序直接读取 FAT32 U盘的物理扇区，解析文件系统数据结构，
 * 提供 BPB 信息查看、目录浏览、文件簇链分析和磁盘统计功能。
 *
 * 运行要求:
 *   - Windows 操作系统
 *   - 以【管理员身份】运行
 *   - 已插入并格式化为 FAT32 的 U 盘
 *
 * 编译方法 (MSVC):
 *   cl /EHsc /std:c++17 /FAT32Explorer.exe src\*.cpp /I include
 *
 * 或使用 CMake:
 *   mkdir build && cd build && cmake .. && cmake --build .
 */

#include "disk_reader.h"
#include "fat32_parser.h"
#include "directory_parser.h"
#include "disk_stats.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <limits>
#include <windows.h>

//=============================================================================
// 全局变量
//=============================================================================
static DiskReader      g_reader;        // 磁盘读取器
static FAT32Parser     g_parser;        // FAT32 解析器
static DirectoryParser* g_dirParser = nullptr;  // 目录解析器
static DiskStats*      g_stats = nullptr;       // 磁盘统计
static std::string     g_driveLetter;   // 当前盘符

//=============================================================================
// 辅助函数
//=============================================================================

/** 清屏 */
void clearScreen() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    DWORD cellCount;
    COORD homeCoords = {0, 0};

    if (hConsole == INVALID_HANDLE_VALUE) return;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;

    cellCount = csbi.dwSize.X * csbi.dwSize.Y;
    if (!FillConsoleOutputCharacter(hConsole, ' ', cellCount, homeCoords, &count)) return;
    if (!FillConsoleOutputAttribute(hConsole, csbi.wAttributes, cellCount, homeCoords, &count)) return;

    SetConsoleCursorPosition(hConsole, homeCoords);
}

/** 暂停等待用户按键 */
void pauseAndWait() {
    std::cout << "\n按 Enter 键继续...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

/** 显示程序横幅 */
void printBanner() {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║                                                              ║
║     FAT32 文件系统浏览器 v1.0                                ║
║     FAT32 File System Explorer                               ║
║                                                              ║
║     基于直接扇区读取的文件系统解析器                          ║
║     课程设计项目 - 操作系统原理                               ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << std::endl;
}

/** 显示主菜单 */
void printMenu() {
    std::cout << "\n";
    std::cout << "┌────────────────────────────────────────────────────┐\n";
    std::cout << "│  当前磁盘: " << std::setw(10) << std::left << (g_driveLetter + ":")
              << "  卷标: " << std::setw(15) << std::left << g_parser.getVolumeLabel() << "│\n";
    std::cout << "│  总容量:   " << std::setw(40) << std::left
              << FileInfo::formatFileSize(
                  static_cast<uint64_t>(g_parser.getTotalSectors()) * g_parser.getBytesPerSector())
              << "│\n";
    std::cout << "├────────────────────────────────────────────────────┤\n";
    std::cout << "│  1. 显示 BPB (BIOS参数块) 详细信息                 │\n";
    std::cout << "│  2. 显示根目录文件列表                              │\n";
    std::cout << "│  3. 显示完整目录树                                  │\n";
    std::cout << "│  4. 查看文件簇链 (按路径查找)                       │\n";
    std::cout << "│  5. 显示磁盘簇使用统计                              │\n";
    std::cout << "│  6. 显示磁盘簇使用网格图                            │\n";
    std::cout << "│  7. 切换磁盘                                        │\n";
    std::cout << "│  0. 退出程序                                        │\n";
    std::cout << "└────────────────────────────────────────────────────┘\n";
    std::cout << "\n请选择 [0-7]: ";
}

/** 获取用户输入的盘符 */
std::string promptDriveLetter() {
    std::string input;
    while (true) {
        std::cout << "请输入 FAT32 U盘的盘符 (例如 E 或 E:): ";
        std::getline(std::cin, input);

        // 提取字母
        char letter = 0;
        for (char c : input) {
            if (isalpha(static_cast<unsigned char>(c))) {
                letter = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                break;
            }
        }

        if (letter >= 'A' && letter <= 'Z') {
            return std::string(1, letter);
        }

        std::cout << "错误: 无效的盘符, 请输入单个字母 (A-Z).\n";
    }
}

/** 尝试打开磁盘 */
bool tryOpenDrive(const std::string& letter) {
    std::cout << "正在打开设备 \\\\.\\" << letter << ": ... ";

    if (!g_reader.open(letter)) {
        std::cout << "失败!\n";
        std::cout << g_reader.getLastError() << "\n";
        return false;
    }

    std::cout << "成功!\n";
    std::cout << "设备大小: " << FileInfo::formatFileSize(g_reader.getDeviceSize()) << "\n";

    std::cout << "正在解析 FAT32 文件系统... ";
    if (!g_parser.initialize(&g_reader)) {
        std::cout << "失败!\n";
        std::cout << "请确认磁盘已格式化为 FAT32 文件系统.\n";
        g_reader.close();
        return false;
    }

    std::cout << "成功!\n";
    std::cout << "文件系统: " << g_parser.getOEMName() << "\n";
    std::cout << "卷标: " << g_parser.getVolumeLabel() << "\n";

    // 创建目录解析器和统计对象
    delete g_dirParser;
    delete g_stats;
    g_dirParser = new DirectoryParser(&g_parser);
    g_stats = new DiskStats(&g_parser);

    g_driveLetter = letter;
    return true;
}

//=============================================================================
// 菜单功能
//=============================================================================

/** 功能 1: 显示 BPB 信息 */
void showBPB() {
    clearScreen();
    std::cout << g_parser.getBPBInfo() << std::endl;
    pauseAndWait();
}

/** 功能 2: 显示根目录文件列表 */
void showRootDirectory() {
    clearScreen();
    std::cout << "\n╔══════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                        根目录文件列表                                   ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════╣\n";

    std::vector<FileInfo> entries;
    if (!g_dirParser->readDirectory(g_parser.getRootDirCluster(), entries, L"")) {
        std::cout << "║ 错误: 无法读取根目录                                                   ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n";
        pauseAndWait();
        return;
    }

    if (entries.empty()) {
        std::cout << "║  (根目录为空)                                                           ║\n";
    } else {
        // 表头
        std::cout << "║ " << std::setw(24) << std::left << "文件名"
                  << std::setw(10) << "类型"
                  << std::setw(8) << "大小"
                  << std::setw(8) << "起始簇"
                  << std::setw(19) << "修改时间"
                  << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════╣\n";

        for (const auto& entry : entries) {
            std::string name = DirectoryParser::ws2s(entry.getDisplayName());
            // 截断过长的文件名
            if (name.length() > 22) {
                name = name.substr(0, 19) + "...";
            }

            // 格式化日期
            std::string dateStr = "-";
            if (entry.writeDate != 0 || entry.writeTime != 0) {
                dateStr = DirectoryParser::formatDateTime(entry.writeDate, entry.writeTime);
            }

            std::cout << "║ " << std::setw(24) << std::left << name
                      << std::setw(10) << entry.getTypeString()
                      << std::setw(8) << (entry.isDirectory ? "-" : entry.getSizeString())
                      << std::setw(8) << (entry.firstCluster >= 2
                                           ? std::to_string(entry.firstCluster)
                                           : "-")
                      << std::setw(19) << dateStr
                      << "║\n";
        }
    }

    std::cout << "╠══════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ 共 " << std::setw(2) << entries.size() << " 个项目"
              << "                                                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n";

    pauseAndWait();
}

/** 功能 3: 显示完整目录树 */
void showDirectoryTree() {
    clearScreen();
    std::cout << "\n正在遍历目录树 (最大深度: 8)...\n\n";

    try {
        g_dirParser->printDirectoryTree(g_parser.getRootDirCluster(), L"", 8);
    } catch (const std::exception& e) {
        std::cout << "\n[错误] 目录树遍历异常: " << e.what() << "\n";
    } catch (...) {
        std::cout << "\n[错误] 目录树遍历发生未知异常, 已安全终止.\n";
    }

    pauseAndWait();
}

/** 功能 4: 查看文件簇链 */
void showClusterChain() {
    clearScreen();
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║              文件簇链查看器                          ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << "║  请输入文件路径 (例如 \\Documents\\file.txt)          ║\n";
    std::cout << "║  或直接输入簇号查看 (例如 cluster:5)                 ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "\n路径: ";

    std::string input;
    std::getline(std::cin, input);

    if (input.empty()) {
        std::cout << "已取消.\n";
        pauseAndWait();
        return;
    }

    uint32_t startCluster = 0;
    std::string displayName;

    if (input.find("cluster:") == 0 || input.find("CLUSTER:") == 0) {
        // 直接输入簇号
        startCluster = static_cast<uint32_t>(std::stoul(input.substr(8)));
        displayName = "簇 " + std::to_string(startCluster);
    } else {
        // 按路径查找
        std::wstring wPath(input.begin(), input.end());
        FileInfo info;
        if (!g_dirParser->findFile(wPath, info)) {
            std::cout << "\n错误: 找不到文件 \"" << input << "\"\n";
            std::cout << "提示: 路径请以 \\ 开头, 使用反斜杠分隔, 如 \\folder\\file.txt\n";
            pauseAndWait();
            return;
        }

        startCluster = info.firstCluster;
        displayName = DirectoryParser::ws2s(info.getDisplayName());

        std::cout << "\n═══════════════════════════════════════════════════════\n";
        std::cout << "文件信息:\n";
        std::cout << "  名称:     " << displayName << "\n";
        std::cout << "  类型:     " << info.getTypeString() << "\n";
        std::cout << "  大小:     " << (info.isDirectory ? "<目录>" : info.getSizeString()) << "\n";
        std::cout << "  起始簇:   " << info.firstCluster << "\n";
    }

    if (startCluster < 2) {
        std::cout << "\n该文件没有分配数据簇 (可能是空文件或目录项无数据).\n";
        pauseAndWait();
        return;
    }

    // 构建簇链
    std::vector<uint32_t> chain;
    if (!g_parser.buildClusterChain(startCluster, chain)) {
        std::cout << "\n错误: 无法构建簇链 (文件系统可能已损坏).\n";
        pauseAndWait();
        return;
    }

    // 显示簇链
    std::cout << "\n═══════════════════════════════════════════════════════\n";
    std::cout << "簇链 (Cluster Chain):\n";
    std::cout << "  共 " << chain.size() << " 个簇\n";
    std::cout << "  每簇 " << g_parser.getClusterSize() << " 字节 ("
              << g_parser.getSectorsPerCluster() << " 扇区 × "
              << g_parser.getBytesPerSector() << " 字节)\n";
    std::cout << "  占用空间: " << FileInfo::formatFileSize(
                  static_cast<uint64_t>(chain.size()) * g_parser.getClusterSize()) << "\n\n";

    // 链式显示
    constexpr uint32_t MAX_DISPLAY = 50;  // 最多显示 50 个簇, 超出用省略号
    std::cout << "  ";
    for (size_t i = 0; i < chain.size() && i < MAX_DISPLAY; i++) {
        if (chain[i] == 0xFFFFFFFF) {
            std::cout << "[坏簇]";
        } else {
            std::cout << chain[i];
        }
        if (i < chain.size() - 1) {
            std::cout << " → ";
        }
        // 每行最多 8 个簇
        if ((i + 1) % 8 == 0 && i < chain.size() - 1) {
            std::cout << "\n  → ";
        }
    }

    if (chain.size() > MAX_DISPLAY) {
        std::cout << " → ... → " << chain.back()
                  << " (共 " << chain.size() << " 个簇)";
    }

    std::cout << " → [结束]\n";

    // 扇区映射
    std::cout << "\n═══════════════════════════════════════════════════════\n";
    std::cout << "扇区映射 (展开前 " << std::min(static_cast<size_t>(5), chain.size()) << " 个簇):\n";
    for (size_t i = 0; i < chain.size() && i < 5; i++) {
        if (chain[i] != 0xFFFFFFFF) {
            uint32_t startSec = g_parser.clusterToSector(chain[i]);
            uint32_t endSec = startSec + g_parser.getSectorsPerCluster() - 1;
            std::cout << "  簇 " << chain[i] << " → 扇区 " << startSec << " ~ " << endSec << "\n";
        }
    }

    pauseAndWait();
}

/** 功能 5: 显示磁盘统计 */
void showStats() {
    clearScreen();
    g_stats->printStats();
    pauseAndWait();
}

/** 功能 6: 显示簇网格 */
void showClusterGrid() {
    clearScreen();
    g_stats->printClusterGrid(80, 3200);  // 80列 × 40行 = 3200簇
    pauseAndWait();
}

/** 功能 7: 切换磁盘 */
void switchDrive() {
    clearScreen();

    // 清理旧资源
    delete g_dirParser;
    delete g_stats;
    g_dirParser = nullptr;
    g_stats = nullptr;
    g_reader.close();
    g_driveLetter.clear();

    std::string letter = promptDriveLetter();
    tryOpenDrive(letter);
    pauseAndWait();
}

//=============================================================================
// 主函数
//=============================================================================
int main() {
    // --- 设置控制台为 UTF-8 编码 (支持中文和长文件名) ---
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 启用 ANSI 转义序列支持 (Windows 10+)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);

    printBanner();

    // --- 检查管理员权限 ---
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }

    if (!isElevated) {
        std::cout << "⚠ 警告: 未检测到管理员权限!\n";
        std::cout << "  本程序需要管理员权限才能直接读取物理磁盘.\n";
        std::cout << "  请以【管理员身份】重新运行本程序.\n\n";
        std::cout << "  是否继续尝试? (y/n): ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            return 0;
        }
        std::cout << "\n";
    }

    // --- 初始连接磁盘 ---
    std::string letter = promptDriveLetter();
    if (!tryOpenDrive(letter)) {
        std::cout << "按 Enter 键退出...";
        std::cin.get();
        return 1;
    }

    // --- 主循环 ---
    bool running = true;
    while (running) {
        clearScreen();
        printMenu();

        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        switch (input[0]) {
            case '1': showBPB();               break;
            case '2': showRootDirectory();      break;
            case '3': showDirectoryTree();      break;
            case '4': showClusterChain();       break;
            case '5': showStats();              break;
            case '6': showClusterGrid();        break;
            case '7': switchDrive();            break;
            case '0':
                running = false;
                std::cout << "\n正在退出...\n";
                break;
            default:
                std::cout << "无效选项! 请输入 0-7 之间的数字.\n";
                pauseAndWait();
                break;
        }
    }

    // --- 清理 ---
    delete g_dirParser;
    delete g_stats;
    g_reader.close();

    std::cout << "感谢使用 FAT32 文件系统浏览器!\n";
    return 0;
}
