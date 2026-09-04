/**
 * @file    directory_parser.cpp
 * @brief   目录项解析模块实现
 *
 * 实现 FAT32 目录项的解析，包括短文件名(8.3)、长文件名(LFN)、
 * 递归目录遍历和按路径查找文件。
 */

#include "directory_parser.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <unordered_set>
#include <stdexcept>

//=============================================================================
// 调试日志宏 (定位菜单3闪退问题)
//=============================================================================
#define MENU3_DEBUG 0
#if MENU3_DEBUG
#define DBG_LOG(msg)  do { std::cerr << "[DBG] " << msg << std::endl; } while(0)
#define DBG_LOG2(msg, val) do { std::cerr << "[DBG] " << msg << " " << (val) << std::endl; } while(0)
#define DBG_ERR(msg)  do { std::cerr << "[ERR] " << msg << std::endl; } while(0)
#else
#define DBG_LOG(msg)  do {} while(0)
#define DBG_LOG2(msg, val) do {} while(0)
#define DBG_ERR(msg)  do {} while(0)
#endif


//=============================================================================
// 构造函数与析构函数
//=============================================================================
DirectoryParser::DirectoryParser(FAT32Parser* parser)
    : fat32Parser(parser)
{
}

DirectoryParser::~DirectoryParser() {
}

//=============================================================================
// 读取目录内容
//=============================================================================
bool DirectoryParser::readDirectory(uint32_t cluster,
                                     std::vector<FileInfo>& entries,
                                     const std::wstring& basePath) {
    entries.clear();

    // 防御: 整个函数包裹 try-catch
    try {
        DBG_LOG("=== readDirectory 入口 ===");
        DBG_LOG2("  cluster =", cluster);
        DBG_LOG2("  basePath =", ws2s(basePath));

        if (!fat32Parser) {
            DBG_ERR("readDirectory: fat32Parser 为 nullptr");
            return false;
        }
        if (!fat32Parser->isInitialized()) {
            DBG_ERR("readDirectory: fat32Parser 未初始化");
            return false;
        }
        if (cluster < 2) {
            DBG_LOG2("readDirectory: 簇号 < 2, 拒绝. cluster =", cluster);
            return false;
        }

        uint32_t totalClusters = fat32Parser->getTotalClusters();
        if (cluster > totalClusters + 1) {
            DBG_LOG2("readDirectory: 簇号超出范围. cluster =", cluster);
            DBG_LOG2("  totalClusters + 1 =", totalClusters + 1);
            return false;
        }

        DiskReader* reader = fat32Parser->getDiskReader();
        if (!reader) { DBG_ERR("readDirectory: reader nullptr"); return false; }
        if (!reader->isOpen()) { DBG_ERR("readDirectory: reader not open"); return false; }

        uint32_t spc        = fat32Parser->getSectorsPerCluster();
        uint32_t dataStart  = fat32Parser->getDataStartSector();
        DBG_LOG2("  spc =", spc);
        DBG_LOG2("  dataStart =", dataStart);

        // 读取目录的所有簇
        std::vector<uint8_t> dirData;
        uint32_t currentCluster = cluster;
        std::unordered_set<uint32_t> visitedClusters;
        constexpr uint32_t MAX_DIR_CLUSTERS = 4096;
        int chainStep = 0;

        while (visitedClusters.size() < MAX_DIR_CLUSTERS) {
            chainStep++;
            DBG_LOG("  -------- FAT链 step " << chainStep << " --------");
            DBG_LOG2("    currentCluster =", currentCluster);

            // 循环检测
            if (visitedClusters.find(currentCluster) != visitedClusters.end()) {
                DBG_ERR("    FAT链循环, 终止");
                return false;
            }
            visitedClusters.insert(currentCluster);

            // 范围检查
            if (currentCluster < 2 || currentCluster > totalClusters + 1) {
                DBG_LOG2("    簇号非法, 终止. val=", currentCluster);
                return false;
            }

            // 读取簇数据
            std::vector<uint8_t> clusterData;
            DBG_LOG("    调用 readCluster ...");
            try {
                if (!reader->readCluster(currentCluster, spc, dataStart, clusterData)) {
                    DBG_LOG2("    readCluster 失败! cluster=", currentCluster);
                    DBG_LOG2("    error:", reader->getLastError());
                    return false;
                }
            } catch (...) {
                DBG_ERR("    readCluster 抛出未知异常!");
                return false;
            }
            DBG_LOG2("    readCluster size =", clusterData.size());

            // 追加数据 (单独 try-catch)
            try {
                dirData.insert(dirData.end(), clusterData.begin(), clusterData.end());
                DBG_LOG2("    dirData 大小 =", dirData.size());
            } catch (const std::exception& e) {
                DBG_LOG("    insert 异常: " << e.what());
                return false;
            } catch (...) {
                DBG_ERR("    insert 未知异常!");
                return false;
            }

            // 读取 FAT 条目
            uint32_t nextCluster = 0;
            try {
                if (!fat32Parser->readFATEntry(currentCluster, nextCluster)) {
                    DBG_LOG2("    readFATEntry 失败, cluster=", currentCluster);
                    break;
                }
            } catch (...) {
                DBG_ERR("    readFATEntry 抛出异常!");
                break;
            }
            DBG_LOG2("    nextCluster =", nextCluster);

            // 检查终止条件
            if (FAT32::isEndOfChain(nextCluster)) {
                DBG_LOG("    -> EndOfChain");
                break;
            }
            if (FAT32::isBadCluster(nextCluster)) {
                DBG_LOG("    -> BadCluster");
                break;
            }
            if (FAT32::isFreeCluster(nextCluster)) {
                DBG_LOG("    -> FreeCluster");
                break;
            }
            if (nextCluster < 2 || nextCluster > totalClusters + 1) {
                DBG_LOG2("    -> nextCluster 超出范围:", nextCluster);
                break;
            }

            currentCluster = nextCluster;
        }  // end while

        DBG_LOG2("  簇链遍历完毕, dirData.size =", dirData.size());

        // ★ 解析目录项 (单独 try-catch)
        DBG_LOG("  调用 parseDirectoryEntries ...");
        try {
            parseDirectoryEntries(dirData, entries);
        } catch (const std::exception& e) {
            DBG_LOG("  parseDirectoryEntries 异常: " << e.what());
            return false;
        } catch (...) {
            DBG_ERR("  parseDirectoryEntries 未知异常!");
            return false;
        }
        DBG_LOG2("  parseDirectoryEntries 返回, entries =", entries.size());

        // ★ 填充路径 (单独 try-catch)
        DBG_LOG("  填充 fullPath ...");
        try {
            for (size_t i = 0; i < entries.size(); i++) {
                auto& entry = entries[i];
                entry.fullPath = basePath + L"/" + entry.getDisplayName();
            }
        } catch (const std::exception& e) {
            DBG_LOG("  fullPath 填充异常: " << e.what());
            return false;
        } catch (...) {
            DBG_ERR("  fullPath 填充未知异常!");
            return false;
        }

        DBG_LOG("=== readDirectory 正常退出, entries=" << entries.size() << " ===");
        return true;

    } catch (const std::exception& e) {
        DBG_LOG("!!! readDirectory 顶层异常: " << e.what());
        return false;
    } catch (...) {
        DBG_ERR("!!! readDirectory 顶层未知异常");
        return false;
    }
}

//=============================================================================
// 解析原始目录数据
//=============================================================================
bool DirectoryParser::parseDirectoryEntries(const std::vector<uint8_t>& data,
                                              std::vector<FileInfo>& entries) {
    entries.clear();
    DBG_LOG2("  [PE] data.size =", data.size());

    if (data.empty()) {
        DBG_LOG("  [PE] 空数据, 返回");
        return true;
    }

    const uint8_t* ptr = data.data();
    const uint8_t* end = ptr + data.size();
    std::vector<const LFNEntry*> lfnStack;
    int entryIdx = 0;

    while (ptr + 32 <= end) {
        entryIdx++;
        uint8_t firstByte = ptr[0];
        uint8_t attr      = ptr[11];

        // --- 0x00 终止 ---
        if (firstByte == FAT32::UNUSED_MARKER) {
            break;
        }

        // --- 0xE5 已删除 ---
        if (firstByte == FAT32::DELETED_MARKER) {
            lfnStack.clear();
            ptr += 32;
            continue;
        }

        // --- LFN 项 ---
        if (attr == FAT32::ATTR_LFN) {
            const LFNEntry* lfn = reinterpret_cast<const LFNEntry*>(ptr);
            uint8_t seq = lfn->sequence;
            if (seq == 0x00 || seq == 0xFF) {
                lfnStack.clear();
            } else {
                lfnStack.push_back(lfn);
            }
            ptr += 32;
            continue;
        }

        // --- 短文件名项 ---
        {
            const DirEntry* de = reinterpret_cast<const DirEntry*>(ptr);

            FileInfo info;
            try {
                combineLFN(lfnStack, de, info);
            } catch (...) {
                lfnStack.clear();
                ptr += 32;
                continue;
            }

            // 跳过卷标 / . / ..
            if (!info.isVolumeLabel && info.shortName != L"." && info.shortName != L"..") {
                entries.push_back(std::move(info));
            }
        }

        lfnStack.clear();
        ptr += 32;
    }

    DBG_LOG2("  [PE] 完成, entries =", entries.size());
    return true;
}

//=============================================================================
// 合并 LFN 和短文件名
//=============================================================================
void DirectoryParser::combineLFN(const std::vector<const LFNEntry*>& lfnEntries,
                                  const DirEntry* shortEntry,
                                  FileInfo& info) {
    // 空指针保护
    if (!shortEntry) {
        DBG_ERR("combineLFN: shortEntry 为 nullptr");
        return;
    }

    // --- 解析属性 ---
    info.attributes = shortEntry->attributes;
    info.isDirectory = (shortEntry->attributes & FAT32::ATTR_DIRECTORY) != 0;
    info.isVolumeLabel = (shortEntry->attributes & FAT32::ATTR_VOLUME_ID) != 0;

    // --- 解析起始簇号 ---
    info.firstCluster = (static_cast<uint32_t>(shortEntry->firstClusterHigh) << 16)
                       | shortEntry->firstClusterLow;

    // --- 解析文件大小 ---
    info.fileSize = shortEntry->fileSize;

    // --- 解析写入日期时间 ---
    info.writeDate = shortEntry->writeDate;
    info.writeTime = shortEntry->writeTime;

    // --- 解析短文件名 ---
    char namePart[9] = {0};
    char extPart[4] = {0};
    std::memcpy(namePart, shortEntry->name, 8);
    std::memcpy(extPart, shortEntry->name + 8, 3);

    // 去除尾部空格 (安全循环: i 不会负数)
    for (int i = 7; i >= 0; i--) {
        if (namePart[i] == ' ') namePart[i] = 0; else break;
    }
    for (int i = 2; i >= 0; i--) {
        if (extPart[i] == ' ') extPart[i] = 0; else break;
    }

    // 构建短文件名 (宽字符)
    std::wstring shortName;
    shortName.reserve(13);
    for (int i = 0; i < 8 && namePart[i] != 0; i++) {
        shortName += static_cast<wchar_t>(static_cast<unsigned char>(namePart[i]));
    }
    if (extPart[0] != 0) {
        shortName += L'.';
        for (int i = 0; i < 3 && extPart[i] != 0; i++) {
            shortName += static_cast<wchar_t>(static_cast<unsigned char>(extPart[i]));
        }
    }
    info.shortName = shortName;

    // --- 解析长文件名 ---
    if (!lfnEntries.empty()) {
        std::wstring longName;
        longName.reserve(256);  // FAT32 LFN 最多 255 字符

        // 验证校验和
        uint8_t expectedChecksum = calculateChecksum(shortEntry->name);

        // LFN 项在磁盘上是倒序存储的 (最后一项在前)
        for (auto it = lfnEntries.rbegin(); it != lfnEntries.rend(); ++it) {
            const LFNEntry* lfn = *it;
            if (!lfn) continue;  // 空指针保护

            // 校验和验证 (跳过校验和不匹配的项)
            if (lfn->checksum != expectedChecksum) {
                continue;
            }

            // 辅助 lambda: 安全读取 UTF-16LE 字符数组
            auto readNameChars = [&longName](const uint16_t* src, int maxCount) {
                for (int i = 0; i < maxCount; i++) {
                    uint16_t ch = src[i];
                    if (ch == 0x0000 || ch == 0xFFFF) return;  // 终止
                    // 过滤非法 surrogate (0xD800-0xDFFF 必须成对出现, 单独出现非法)
                    if (ch >= 0xD800 && ch <= 0xDFFF) {
                        // 跳过孤立 surrogate, 避免 wcout 崩溃
                        continue;
                    }
                    longName += static_cast<wchar_t>(ch);
                }
            };

            // 读取 name1 (5 个字符), name2 (6 个), name3 (2 个)
            readNameChars(lfn->name1, 5);
            readNameChars(lfn->name2, 6);
            readNameChars(lfn->name3, 2);
        }

        // 安全去除尾部字符
        while (!longName.empty() && (longName.back() == L'\0' || longName.back() == L' ')) {
            longName.pop_back();
        }

        info.longName = longName.empty() ? shortName : longName;
    } else {
        info.longName = shortName;
    }

    // 防御: 如果长短文件名都为空, 给一个占位名
    if (info.shortName.empty() && info.longName.empty()) {
        info.shortName = L"???";
        info.longName = L"???";
    }
}

//=============================================================================
// 计算 LFN 校验和
//=============================================================================
uint8_t DirectoryParser::calculateChecksum(const uint8_t shortName[11]) {
    uint8_t checksum = 0;
    for (int i = 0; i < 11; i++) {
        checksum = ((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + shortName[i];
    }
    return checksum;
}

//=============================================================================
// 递归遍历目录树 (公开接口, 带循环检测包装)
//=============================================================================
bool DirectoryParser::traverseTree(uint32_t cluster, const std::wstring& path,
                                    std::function<void(const FileInfo&, const std::wstring&, int)> callback,
                                    int maxDepth, int currentDepth) {
    std::unordered_set<uint32_t> visited;
    return traverseTreeImpl(cluster, path, callback, visited, maxDepth, currentDepth);
}

bool DirectoryParser::traverseTreeImpl(uint32_t cluster, const std::wstring& path,
                                        std::function<void(const FileInfo&, const std::wstring&, int)> callback,
                                        std::unordered_set<uint32_t>& visited,
                                        int maxDepth, int currentDepth) {
    if (currentDepth > maxDepth || !fat32Parser) {
        return false;
    }

    // 循环引用检测: 防止损坏的文件系统导致无限递归
    if (visited.find(cluster) != visited.end()) {
        return false;  // 检测到目录循环
    }
    visited.insert(cluster);

    std::vector<FileInfo> entries;
    if (!readDirectory(cluster, entries, path)) {
        return false;
    }

    for (const auto& entry : entries) {
        // 回调: 通知调用者发现了文件/目录
        callback(entry, path, currentDepth);

        // 如果是子目录, 递归进入
        if (entry.isDirectory && entry.firstCluster >= 2) {
            std::wstring subPath = path + L"\\" + entry.getDisplayName();
            traverseTreeImpl(entry.firstCluster, subPath, callback, visited, maxDepth, currentDepth + 1);
        }
    }

    return true;
}

//=============================================================================
// 打印目录树 (内部递归辅助函数, 纯 C++ 安全实现)
//=============================================================================
static void printTreeRecursive(DirectoryParser* parser, uint32_t cluster,
                                const std::wstring& path, const std::string& prefix,
                                std::unordered_set<uint32_t>& visited,
                                int maxDepth, int currentDepth) try {
    DBG_LOG(">>> printTreeRecursive 入口");
    DBG_LOG2("  cluster =", cluster);
    DBG_LOG2("  depth =", currentDepth);
    DBG_LOG2("  path =", DirectoryParser::ws2s(path));

    // 深度保护
    if (currentDepth > maxDepth) {
        DBG_LOG("  -> 超过最大深度, 返回");
        return;
    }
    if (currentDepth < 0) {
        DBG_LOG("  -> 深度为负, 返回");
        return;
    }

    // 循环引用检测
    if (visited.find(cluster) != visited.end()) {
        DBG_LOG2("  -> 簇已访问过, 跳过 (防止循环). cluster =", cluster);
        return;
    }
    visited.insert(cluster);

    // 簇号合法性
    if (cluster < 2) {
        DBG_LOG2("  -> 非法簇号, 跳过. cluster =", cluster);
        return;
    }

    // 读取目录
    std::vector<FileInfo> entries;
    DBG_LOG("  调用 readDirectory ...");
    if (!parser->readDirectory(cluster, entries, path)) {
        DBG_LOG("  readDirectory 返回 false, 跳过此目录");
        return;
    }
    DBG_LOG2("  readDirectory 成功, entries =", entries.size());

    // 遍历条目
    for (size_t idx = 0; idx < entries.size(); idx++) {
        const auto& entry = entries[idx];
        bool isLast = (idx == entries.size() - 1);

        // 获取显示名称 (UTF-8)
        std::string displayName;
        try {
            displayName = DirectoryParser::ws2s(entry.getDisplayName());
        } catch (...) {
            displayName = "???";
        }

        DBG_LOG2("  条目[" << idx << "]: ", displayName);
        DBG_LOG2("    isDir =", entry.isDirectory);
        DBG_LOG2("    firstCluster =", entry.firstCluster);

        // 树形连接线 (纯 ASCII)
        std::string connector = isLast ? "`-- " : "|-- ";
        std::string extension = isLast ? "    " : "|   ";

        // 日期
        std::string dateStr;
        if (entry.writeDate != 0 || entry.writeTime != 0) {
            dateStr = DirectoryParser::formatDateTime(entry.writeDate, entry.writeTime);
        }

        // 输出
        std::cout << prefix << connector;
        if (entry.isDirectory) {
            std::cout << "[DIR]  " << displayName;
        } else {
            std::cout << "[FILE] " << displayName
                      << "  [" << entry.getSizeString() << "]";
        }
        if (!dateStr.empty()) {
            std::cout << "  [" << dateStr << "]";
        }
        std::cout << "\n";
        std::cout.flush();

        // 递归进入子目录 (严格保护)
        if (entry.isDirectory && entry.firstCluster >= 2) {
            DBG_LOG2("    -> 递归进入子目录, cluster =", entry.firstCluster);

            // 保护1: 不能指向自身
            if (entry.firstCluster == cluster) {
                DBG_LOG("      [警告] 子目录指向自身, 跳过");
                continue;
            }

            // 保护2: 不能指向已访问的簇 (防止 A→B→A 循环)
            if (visited.find(entry.firstCluster) != visited.end()) {
                DBG_LOG("      [警告] 子目录指向已访问簇, 跳过");
                continue;
            }

            // 保护3: 检查总簇数范围
            // (readDirectory 内部会再次检查, 这里提前过滤避免无效递归)

            std::wstring subPath = path + L"\\" + entry.getDisplayName();
            std::string newPrefix = prefix + extension;
            printTreeRecursive(parser, entry.firstCluster, subPath, newPrefix,
                              visited, maxDepth, currentDepth + 1);
        }
    }

    DBG_LOG2("<<< printTreeRecursive 退出, cluster =", cluster);

} catch (const std::exception& e) {
    DBG_LOG("!!! printTreeRecursive 异常: " << e.what());
    // 不重新抛出, 安全返回
} catch (...) {
    DBG_ERR("!!! printTreeRecursive 未知异常, 安全返回");
}

void DirectoryParser::printDirectoryTree(uint32_t cluster, const std::wstring& path, int maxDepth) try {
    DBG_LOG("===== printDirectoryTree 入口 =====");
    DBG_LOG2("  cluster =", cluster);
    DBG_LOG2("  maxDepth =", maxDepth);
    DBG_LOG2("  path =", ws2s(path));

    std::string rootName = ws2s(path.empty() ? L"根目录 ( / )" : path);
    std::cout << "\n[DIR]  " << rootName << "\n";
    std::cout.flush();

    if (cluster < 2) {
        std::cout << "(错误: 无效的起始簇号 " << cluster << ")\n";
        DBG_ERR("printDirectoryTree: 起始簇号 < 2, 终止");
        return;
    }

    // 验证 cluster 在有效范围内
    if (fat32Parser) {
        uint32_t tc = fat32Parser->getTotalClusters();
        if (cluster > tc + 1) {
            std::cout << "(错误: 起始簇号 " << cluster << " 超出总簇数 " << tc << ")\n";
            DBG_ERR("printDirectoryTree: 起始簇号超出范围");
            return;
        }
    }

    std::unordered_set<uint32_t> visited;
    printTreeRecursive(this, cluster, path, "", visited, maxDepth, 0);

    DBG_LOG("===== printDirectoryTree 正常退出 =====");

} catch (const std::exception& e) {
    std::cout << "\n[ERROR] 目录树遍历异常: " << e.what() << "\n";
    DBG_LOG("!!! printDirectoryTree 异常: " << e.what());
} catch (...) {
    std::cout << "\n[ERROR] 目录树遍历发生未知异常, 已安全终止.\n";
    DBG_ERR("!!! printDirectoryTree 未知异常");
}

//=============================================================================
// 按路径查找文件
//=============================================================================
bool DirectoryParser::findFile(const std::wstring& path, FileInfo& result) {
    if (!fat32Parser || path.empty()) {
        return false;
    }

    // 分割路径
    std::vector<std::wstring> components;
    std::wstring current;
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        components.push_back(current);
    }

    if (components.empty()) {
        return false;
    }

    // 从根目录开始搜索
    uint32_t currentCluster = fat32Parser->getRootDirCluster();
    std::wstring currentPath;

    for (size_t i = 0; i < components.size(); i++) {
        std::vector<FileInfo> entries;
        if (!readDirectory(currentCluster, entries, currentPath)) {
            return false;
        }

        bool found = false;
        for (const auto& entry : entries) {
            // 比较文件名 (大小写不敏感)
            std::wstring target = components[i];
            std::wstring entryName = entry.getDisplayName();

            // 转为小写进行比较
            std::wstring targetLower = target;
            std::wstring entryLower = entryName;
            std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(), ::towlower);
            std::transform(entryLower.begin(), entryLower.end(), entryLower.begin(), ::towlower);

            if (targetLower == entryLower) {
                if (i == components.size() - 1) {
                    // 找到目标文件/目录
                    result = entry;
                    result.fullPath = currentPath + L"\\" + entryName;

                    // 构建簇链
                    if (result.firstCluster >= 2) {
                        fat32Parser->buildClusterChain(result.firstCluster, result.clusterChain);
                    }
                    return true;
                } else if (entry.isDirectory) {
                    // 进入子目录继续搜索
                    currentCluster = entry.firstCluster;
                    currentPath = currentPath + L"\\" + entryName;
                    found = true;
                    break;
                } else {
                    // 路径中间组件不是目录, 搜索失败
                    return false;
                }
            }
        }

        if (!found && i < components.size() - 1) {
            return false;  // 中间路径未找到
        }
        // 如果是最后一个组件且未找到, 循环自然结束
    }

    return false;
}

//=============================================================================
// 辅助函数
//=============================================================================
std::string DirectoryParser::formatDateTime(uint16_t date, uint16_t time) {
    int year  = ((date >> 9) & 0x7F) + 1980;
    int month = (date >> 5) & 0x0F;
    int day   = date & 0x1F;
    int hour  = (time >> 11) & 0x1F;
    int min   = (time >> 5) & 0x3F;
    int sec   = (time & 0x1F) * 2;

    std::ostringstream oss;
    oss << std::setfill('0')
        << year << "-" << std::setw(2) << month << "-" << std::setw(2) << day
        << " " << std::setw(2) << hour << ":" << std::setw(2) << min << ":" << std::setw(2) << sec;
    return oss.str();
}

std::wstring DirectoryParser::trimTrailing(const std::wstring& s) {
    size_t end = s.find_last_not_of(L' ');
    return (end == std::wstring::npos) ? L"" : s.substr(0, end + 1);
}

std::string DirectoryParser::ws2s(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}
