/**
 * @file    disk_stats.cpp
 * @brief   磁盘块使用统计模块实现
 *
 * 负责统计 FAT32 卷的簇使用情况，并以可视化方式展示。
 */

#include "disk_stats.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

DiskStats::DiskStats(FAT32Parser* parser)
    : fat32Parser(parser)
    , statsGathered(false)
{
}

DiskStats::~DiskStats() {
}

bool DiskStats::gatherStats(ClusterStats& stats) {
    if (!fat32Parser || !fat32Parser->isInitialized()) {
        return false;
    }

    // 扫描 FAT 表
    uint32_t freeCount, usedCount, badCount;
    if (!fat32Parser->scanFAT(freeCount, usedCount, badCount)) {
        return false;
    }

    stats.totalClusters = fat32Parser->getTotalClusters();
    stats.freeClusters  = freeCount;
    stats.usedClusters  = usedCount;
    stats.badClusters   = badCount;

    // 计算百分比
    if (stats.totalClusters > 0) {
        stats.freePercent = (static_cast<double>(stats.freeClusters) / stats.totalClusters) * 100.0;
        stats.usedPercent = (static_cast<double>(stats.usedClusters) / stats.totalClusters) * 100.0;
        stats.badPercent  = (static_cast<double>(stats.badClusters) / stats.totalClusters) * 100.0;
    } else {
        stats.freePercent = stats.usedPercent = stats.badPercent = 0.0;
    }

    // 构建簇状态向量
    uint32_t totalClusters = fat32Parser->getTotalClusters();
    clusterStatus.resize(totalClusters + 2, 0);  // +2 因为簇号从 2 开始

    for (uint32_t i = 2; i <= totalClusters + 1; i++) {
        uint32_t entry;
        if (!fat32Parser->readFATEntry(i, entry)) {
            clusterStatus[i] = 2;  // 无法读取, 标记为未知/坏
            continue;
        }

        if (FAT32::isFreeCluster(entry)) {
            clusterStatus[i] = 0;  // 空闲
        } else if (FAT32::isBadCluster(entry)) {
            clusterStatus[i] = 2;  // 坏簇
        } else if (FAT32::isReservedCluster(entry)) {
            clusterStatus[i] = 2;  // 保留簇 (与坏簇同色显示)
        } else {
            clusterStatus[i] = 1;  // 已使用
        }
    }

    statsGathered = true;
    return true;
}

void DiskStats::printStats() {
    if (!statsGathered) {
        if (!gatherStats(cachedStats)) {
            std::cout << "错误: 无法获取磁盘统计信息.\n";
            return;
        }
    }

    std::cout << cachedStats.toString();
}

void DiskStats::printClusterGrid(int columns, uint32_t maxClusters) {
    if (!statsGathered) {
        if (!gatherStats(cachedStats)) {
            std::cout << "错误: 无法获取磁盘统计信息.\n";
            return;
        }
    }

    uint32_t total = cachedStats.totalClusters;
    uint32_t displayCount = (maxClusters == 0 || maxClusters > total) ? total : maxClusters;

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║           簇使用状态网格 (Cluster Grid)             ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";
    std::cout << "║ 图例: ■ 已使用  □ 空闲  × 坏块/保留                ║\n";
    std::cout << "╠══════════════════════════════════════════════════════╣\n";

    if (displayCount == 0) {
        std::cout << "║  (无簇数据)                                         ║\n";
    } else {
        std::string gridLine;
        for (uint32_t i = 0; i < displayCount; i++) {
            uint32_t clusterNum = i + 2;  // 簇号从 2 开始
            if (clusterNum < clusterStatus.size()) {
                switch (clusterStatus[clusterNum]) {
                    case 0: gridLine += "□"; break;  // 空闲
                    case 1: gridLine += "■"; break;  // 已使用
                    case 2: gridLine += "×"; break;  // 坏/保留
                    default: gridLine += "?"; break;
                }
            } else {
                gridLine += "?";
            }

            // 每 columns 个换行
            if ((i + 1) % columns == 0) {
                std::cout << "║ " << gridLine << " ║\n";
                gridLine.clear();
            }
        }

        // 最后一行不足 columns 的部分
        if (!gridLine.empty()) {
            // 填充到 columns 宽度
            gridLine.append(columns - gridLine.size(), ' ');
            std::cout << "║ " << gridLine << " ║\n";
        }
    }

    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "  每格代表 1 个簇, 共显示 " << displayCount << " / " << total << " 个簇\n\n";
}

//=============================================================================
// ClusterStats::toString
//=============================================================================
std::string ClusterStats::toString() const {
    std::ostringstream oss;
    oss << "\n╔══════════════════════════════════════════════════╗\n";
    oss << "║           磁盘块 (簇) 使用统计报告              ║\n";
    oss << "╠══════════════════════════════════════════════════╣\n";

    // 格式化数字 (千位分隔)
    auto fmtNum = [](uint32_t n) -> std::string {
        std::string s = std::to_string(n);
        int len = static_cast<int>(s.length());
        for (int i = len - 3; i > 0; i -= 3) {
            s.insert(i, ",");
        }
        return s;
    };

    oss << "║ 总簇数:       " << std::setw(30) << std::right << fmtNum(totalClusters) << " ║\n";
    oss << "║ 空闲簇:       " << std::setw(30) << std::right << fmtNum(freeClusters)
        << " (" << std::fixed << std::setprecision(1) << std::setw(6) << freePercent << "%) ║\n";
    oss << "║ 已使用簇:     " << std::setw(30) << std::right << fmtNum(usedClusters)
        << " (" << std::fixed << std::setprecision(1) << std::setw(6) << usedPercent << "%) ║\n";
    oss << "║ 坏簇/保留:    " << std::setw(30) << std::right << fmtNum(badClusters)
        << " (" << std::fixed << std::setprecision(1) << std::setw(6) << badPercent << "%) ║\n";

    // 计算总容量 (假设每簇 4096 字节, 实际值应传进来)
    oss << "╠══════════════════════════════════════════════════╣\n";

    // 进度条
    auto bar = [](double pct, int width = 36) -> std::string {
        int filled = static_cast<int>(pct / 100.0 * width);
        std::string result;
        for (int i = 0; i < width; i++) {
            result += (i < filled) ? "█" : "░";
        }
        return result;
    };

    oss << "║ 已用: " << bar(usedPercent) << " ║\n";
    oss << "║ 空闲: " << bar(freePercent) << " ║\n";

    oss << "╚══════════════════════════════════════════════════╝\n";

    return oss.str();
}
