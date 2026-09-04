/**
 * @file    fat32_parser.cpp
 * @brief   FAT32 文件系统解析模块实现
 *
 * 实现 FAT32 BPB 解析、FAT 表读取、簇链构建等核心逻辑。
 */

#include "fat32_parser.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <unordered_set>
#include <iostream>

// 调试日志 (发布版关闭)
#define CHAIN_DBG(msg)  do {} while(0)

FAT32Parser::FAT32Parser()
    : diskReader(nullptr)
    , initialized(false)
    , totalSectors(0)
    , sectorsPerFAT(0)
    , fatStartSector(0)
    , dataStartSector(0)
    , totalClusters(0)
    , fatLoaded(false)
{
}

FAT32Parser::~FAT32Parser() {
}

bool FAT32Parser::initialize(DiskReader* reader) {
    if (!reader || !reader->isOpen()) {
        return false;
    }

    diskReader = reader;
    initialized = false;
    fatLoaded = false;

    // --- 第一步: 读取引导扇区 (扇区 0) ---
    // 使用最小扇区大小 512 字节 (对任何 FAT32 卷都足够)
    std::vector<uint8_t> bootSector;
    if (!diskReader->readBytes(0, 512, bootSector)) {
        return false;
    }

    // --- 第二步: 验证 FAT32 签名 ---
    if (!validateBootSector(bootSector)) {
        return false;
    }

    // --- 第三步: 复制 BPB 数据 ---
    std::memcpy(&bpb, bootSector.data(), sizeof(FAT32_BPB));

    // --- 第四步: 更新 DiskReader 的扇区大小 ---
    diskReader->setBytesPerSector(bpb.bytesPerSector);

    // --- 第五步: 计算关键参数 ---
    // 总扇区数: 优先使用 32 位值, 为 0 时使用 16 位值
    totalSectors = (bpb.totalSectors16 != 0)
                   ? static_cast<uint32_t>(bpb.totalSectors16)
                   : bpb.totalSectors32;

    // 每个 FAT 表占用的扇区数
    sectorsPerFAT = (bpb.sectorsPerFAT16 != 0)
                    ? static_cast<uint32_t>(bpb.sectorsPerFAT16)
                    : bpb.sectorsPerFAT32;

    // FAT 表起始扇区 = 保留扇区数
    fatStartSector = bpb.reservedSectors;

    // 数据区起始扇区 = 保留扇区数 + FAT表数量 × 每个FAT扇区数
    dataStartSector = bpb.reservedSectors + bpb.numFATs * sectorsPerFAT;

    // 总簇数 = (总扇区数 - 数据区起始扇区) / 每簇扇区数
    // 防止文件系统损坏导致 dataStartSector > totalSectors 时出现 uint32 回绕
    if (totalSectors <= dataStartSector) {
        return false;  // 无效的 FAT32 布局
    }
    totalClusters = (totalSectors - dataStartSector) / bpb.sectorsPerCluster;

    // --- 第六步: 加载 FAT 表到内存 ---
    if (!loadFAT()) {
        return false;
    }

    initialized = true;
    return true;
}

bool FAT32Parser::validateBootSector(const std::vector<uint8_t>& data) {
    if (data.size() < 512) {
        return false;
    }

    // 检查结束签名 0x55 0xAA (位于扇区末尾两字节)
    if (data[510] != 0x55 || data[511] != 0xAA) {
        return false;
    }

    // 检查扇区大小: 必须是 512 的整数倍且在有效范围内
    uint16_t bytesPerSec = *reinterpret_cast<const uint16_t*>(&data[11]);
    if (bytesPerSec < 512 || bytesPerSec > 4096 || (bytesPerSec & (bytesPerSec - 1)) != 0) {
        return false;  // 不是 2 的幂
    }

    // 检查每簇扇区数: 必须是 2^n (n≥0)
    uint8_t secPerClus = data[13];
    if (secPerClus == 0 || (secPerClus & (secPerClus - 1)) != 0) {
        return false;
    }

    // 检查 FAT 数量
    uint8_t numFATs = data[16];
    if (numFATs == 0 || numFATs > 4) {
        return false;
    }

    // 判断是否为 FAT32:
    // 1) rootEntryCount (偏移 17-18) 应为 0
    // 2) sectorsPerFAT16 (偏移 22-23) 应为 0
    // 3) sectorsPerFAT32 (偏移 36-39) 应非 0
    // 4) 总簇数 >= 65525
    uint16_t rootEntCnt = *reinterpret_cast<const uint16_t*>(&data[17]);
    uint16_t secPerFAT16 = *reinterpret_cast<const uint16_t*>(&data[22]);
    uint32_t secPerFAT32 = *reinterpret_cast<const uint32_t*>(&data[36]);

    // FAT12/16 的 rootEntryCount 非 0
    if (rootEntCnt != 0) {
        return false;  // 不是 FAT32
    }

    if (secPerFAT16 != 0) {
        return false;  // 使用 16 位 FAT 大小, 不是 FAT32
    }

    if (secPerFAT32 == 0) {
        return false;  // FAT32 的 sectorsPerFAT32 不应为 0
    }

    // 检查文件系统类型字符串 (偏移 82, 8 字节)
    const char* fsType = reinterpret_cast<const char*>(&data[82]);
    // 宽松检查: 允许非标准实现 (有些不会写 "FAT32   ")
    // 仅当有字符串时检查前 5 个字符
    if (fsType[0] != 0x20 && fsType[0] != 0x00) {
        if (std::strncmp(fsType, "FAT32", 5) != 0) {
            // 警告但不阻止 — 有些格式化工具会写不同的字符串
        }
    }

    return true;
}

bool FAT32Parser::loadFAT() {
    if (!diskReader) {
        CHAIN_DBG("loadFAT: diskReader 为 nullptr");
        return false;
    }

    uint32_t fatSizeBytes = sectorsPerFAT * bpb.bytesPerSector;
    CHAIN_DBG("loadFAT: sectorsPerFAT=" << sectorsPerFAT
              << " bps=" << bpb.bytesPerSector
              << " fatSizeBytes=" << fatSizeBytes
              << " fatStartSector=" << fatStartSector);

    fatCache.resize(fatSizeBytes);

    if (!diskReader->readSectors(fatStartSector, sectorsPerFAT, fatCache)) {
        CHAIN_DBG("loadFAT: readSectors 失败!");
        fatCache.clear();
        return false;
    }

    CHAIN_DBG("loadFAT: 成功, fatCache.size=" << fatCache.size());
    fatLoaded = true;
    return true;
}

uint32_t FAT32Parser::clusterToSector(uint32_t cluster) const {
    if (cluster < 2) {
        return dataStartSector;  // 簇 0 和 1 不存在, 返回数据区起始
    }
    return dataStartSector + (cluster - 2) * bpb.sectorsPerCluster;
}

bool FAT32Parser::readFATEntry(uint32_t cluster, uint32_t& nextCluster) {
    if (!fatLoaded) {
        CHAIN_DBG("readFATEntry: FAT 未加载!");
        return false;
    }

    uint32_t offset = cluster * FAT32::FAT_ENTRY_SIZE;
    if (offset + FAT32::FAT_ENTRY_SIZE > fatCache.size()) {
        CHAIN_DBG("readFATEntry: offset 越界! cluster=" << cluster
                  << " offset=" << offset << " cacheSize=" << fatCache.size());
        return false;
    }

    uint32_t rawEntry = *reinterpret_cast<const uint32_t*>(&fatCache[offset]);
    nextCluster = rawEntry & FAT32::FAT_MASK;

    // ★ 仅对 cluster <= 100 打印 (避免日志爆炸)
    if (cluster <= 100) {
        CHAIN_DBG("FAT[" << cluster << "] offset=" << offset
                  << " raw=0x" << std::hex << rawEntry << std::dec
                  << " masked=0x" << std::hex << nextCluster << std::dec
                  << " isEOF=" << (FAT32::isEndOfChain(nextCluster) ? "Y" : "N")
                  << " isBad=" << (FAT32::isBadCluster(nextCluster) ? "Y" : "N")
                  << " isFree=" << (FAT32::isFreeCluster(nextCluster) ? "Y" : "N"));
    }

    return true;
}

bool FAT32Parser::buildClusterChain(uint32_t startCluster, std::vector<uint32_t>& chain) {
    chain.clear();

    CHAIN_DBG("========== buildClusterChain 入口 ==========");
    CHAIN_DBG("  startCluster = " << startCluster);
    CHAIN_DBG("  totalClusters = " << totalClusters);
    CHAIN_DBG("  totalClusters+1 = " << (totalClusters + 1));

    if (startCluster < 2) {
        CHAIN_DBG("  FAIL: startCluster < 2");
        return false;
    }

    // ★ 验证 startCluster 在有效范围内
    if (startCluster > totalClusters + 1) {
        CHAIN_DBG("  FAIL: startCluster " << startCluster
                  << " > totalClusters+1 " << (totalClusters + 1));
        return false;
    }

    uint32_t current = startCluster;
    constexpr uint32_t MAX_CHAIN_LENGTH = 1000000;
    uint32_t iterations = 0;
    std::unordered_set<uint32_t> visited;

    chain.push_back(current);
    visited.insert(current);

    CHAIN_DBG("  初始簇 " << current << " 已加入链, 开始遍历 FAT ...");

    while (iterations < MAX_CHAIN_LENGTH) {
        uint32_t nextCluster;
        if (!readFATEntry(current, nextCluster)) {
            CHAIN_DBG("  FAIL: readFATEntry(" << current << ") 返回 false");
            return false;
        }

        // ★ readFATEntry 内部已打印 FAT[current] 详情

        if (FAT32::isEndOfChain(nextCluster)) {
            CHAIN_DBG("  -> isEndOfChain=Y, 簇链结束. 链长度=" << chain.size());
            break;
        }

        if (FAT32::isBadCluster(nextCluster)) {
            CHAIN_DBG("  -> isBadCluster=Y, 记录坏簇标记");
            chain.push_back(0xFFFFFFFF);
            break;
        }

        // ★ 范围检查
        if (nextCluster < 2) {
            CHAIN_DBG("  FAIL: nextCluster=" << nextCluster << " < 2, FAT 表可能损坏");
            return false;
        }
        if (nextCluster > totalClusters + 1) {
            CHAIN_DBG("  FAIL: nextCluster=" << nextCluster
                      << " > totalClusters+1=" << (totalClusters + 1));
            return false;
        }

        // 循环检测
        if (visited.find(nextCluster) != visited.end()) {
            CHAIN_DBG("  FAIL: 检测到循环引用, cluster=" << nextCluster);
            return false;
        }

        CHAIN_DBG("  -> nextCluster=" << nextCluster << " 有效, 加入链");
        visited.insert(nextCluster);
        chain.push_back(nextCluster);
        current = nextCluster;
        iterations++;
    }

    if (iterations >= MAX_CHAIN_LENGTH) {
        CHAIN_DBG("  FAIL: 达到最大链长 " << MAX_CHAIN_LENGTH);
        chain.clear();
        return false;
    }

    CHAIN_DBG("========== buildClusterChain 成功, 链长度=" << chain.size() << " ==========");
    return true;
}

bool FAT32Parser::scanFAT(uint32_t& totalFree, uint32_t& totalUsed, uint32_t& totalBad) {
    totalFree = 0;
    totalUsed = 0;
    totalBad = 0;

    if (!fatLoaded) return false;

    // 从簇 2 开始扫描 (簇 0 和 1 是保留的)
    uint32_t entryCount = fatCache.size() / FAT32::FAT_ENTRY_SIZE;
    uint32_t scanEnd = std::min(totalClusters + 2, entryCount);

    for (uint32_t i = 2; i < scanEnd; i++) {
        uint32_t entry;
        if (!readFATEntry(i, entry)) continue;

        if (FAT32::isFreeCluster(entry)) {
            totalFree++;
        } else if (FAT32::isBadCluster(entry)) {
            totalBad++;
        } else {
            totalUsed++;
        }
    }

    return true;
}

std::string FAT32Parser::getBPBInfo() const {
    std::ostringstream oss;
    oss << "╔══════════════════════════════════════════════════╗\n";
    oss << "║           FAT32 BPB (BIOS参数块) 信息            ║\n";
    oss << "╠══════════════════════════════════════════════════╣\n";
    oss << "║ OEM 名称:           " << std::setw(28) << std::left << getOEMName() << "║\n";
    oss << "║ 每扇区字节数:       " << std::setw(28) << std::left
        << std::to_string(bpb.bytesPerSector) + " 字节" << "║\n";
    oss << "║ 每簇扇区数:         " << std::setw(28) << std::left
        << std::to_string(bpb.sectorsPerCluster) + " (" + std::to_string(getClusterSize()) + " 字节/簇)" << "║\n";
    oss << "║ 保留扇区数:         " << std::setw(28) << std::left
        << std::to_string(bpb.reservedSectors) << "║\n";
    oss << "║ FAT 表数量:         " << std::setw(28) << std::left
        << std::to_string(bpb.numFATs) << "║\n";
    oss << "║ 每个FAT大小:        " << std::setw(28) << std::left
        << std::to_string(sectorsPerFAT) + " 扇区 (" + FileInfo::formatFileSize(sectorsPerFAT * bpb.bytesPerSector) + ")" << "║\n";
    oss << "║ 隐藏扇区数:         " << std::setw(28) << std::left
        << std::to_string(bpb.hiddenSectors) << "║\n";
    oss << "║ 总扇区数:           " << std::setw(28) << std::left
        << std::to_string(totalSectors) + " (" + FileInfo::formatFileSize(static_cast<uint64_t>(totalSectors) * bpb.bytesPerSector) + ")" << "║\n";
    oss << "║ 总簇数:             " << std::setw(28) << std::left
        << std::to_string(totalClusters) << "║\n";
    oss << "║ FAT 起始扇区:       " << std::setw(28) << std::left
        << std::to_string(fatStartSector) << "║\n";
    oss << "║ 数据区起始扇区:     " << std::setw(28) << std::left
        << std::to_string(dataStartSector) << "║\n";
    oss << "║ 根目录起始簇:       " << std::setw(28) << std::left
        << std::to_string(bpb.rootCluster) << "║\n";
    oss << "║ 卷序列号:           " << std::setw(28) << std::left
        << std::hex << std::uppercase << bpb.volumeID << std::dec << "║\n";
    oss << "║ 文件系统类型:       " << std::setw(28) << std::left
        << std::string(reinterpret_cast<const char*>(bpb.filesystemType), 8) << "║\n";
    oss << "║ FSInfo 扇区:        " << std::setw(28) << std::left
        << std::to_string(bpb.fsInfo) << "║\n";
    oss << "║ 备份引导扇区:       " << std::setw(28) << std::left
        << std::to_string(bpb.backupBootSector) << "║\n";
    oss << "╚══════════════════════════════════════════════════╝\n";

    return oss.str();
}

std::string FAT32Parser::getVolumeLabel() const {
    // 卷标在 BPB 偏移 0x47, 11 字节, 不足用空格填充
    char label[12] = {0};
    std::memcpy(label, bpb.volumeLabel, 11);
    // 去除尾部空格
    for (int i = 10; i >= 0; i--) {
        if (label[i] == ' ' || label[i] == 0) {
            label[i] = 0;
        } else {
            break;
        }
    }
    return std::string(label);
}

std::string FAT32Parser::getOEMName() const {
    char name[9] = {0};
    std::memcpy(name, bpb.OEMName, 8);
    // 去除尾部空格
    for (int i = 7; i >= 0; i--) {
        if (name[i] == ' ' || name[i] == 0) {
            name[i] = 0;
        } else {
            break;
        }
    }
    return std::string(name);
}
