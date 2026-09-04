/**
 * @file    disk_reader.cpp
 * @brief   磁盘读取模块实现
 *
 * 使用 Windows API 直接访问物理卷，读取原始扇区数据。
 * 需要以管理员权限运行。
 */

#include "disk_reader.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cctype>

DiskReader::DiskReader()
    : hDrive(INVALID_HANDLE_VALUE)
    , bytesPerSector(512)        // 默认 512, 后续从 BPB 更新
    , deviceSize(0)
{
}

DiskReader::~DiskReader() {
    close();
}

bool DiskReader::open(const std::string& driveLetter) {
    // 确保之前的句柄已关闭
    close();

    // --- 提取盘符字母 (处理 "E:", "E", "\\\\.\\E:", "\\.\E:" 等格式) ---
    char letter = 0;
    for (char c : driveLetter) {
        if (isalpha(static_cast<unsigned char>(c))) {
            letter = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            break;
        }
    }

    if (letter < 'A' || letter > 'Z') {
        lastError = "错误: 无法从输入中提取有效盘符. 请输入如 E: 或 E 的格式.";
        return false;
    }

    // 构建物理卷路径: \\.\X:
    drivePath = "\\\\.\\";
    drivePath += letter;
    drivePath += ":";

    // --- 使用 CreateFile 打开物理卷 ---
    // FILE_SHARE_READ | FILE_SHARE_WRITE: 允许其他进程同时访问
    // OPEN_EXISTING: 设备必须已存在 (已插入并格式化)
    // 0: 不使用 FILE_FLAG_ 特殊标志
    hDrive = CreateFileA(
        drivePath.c_str(),
        GENERIC_READ,                       // 只读访问
        FILE_SHARE_READ | FILE_SHARE_WRITE, // 共享模式
        nullptr,                            // 安全属性
        OPEN_EXISTING,                       // 打开已存在的设备
        0,                                   // 属性标志
        nullptr                              // 模板文件句柄
    );

    if (hDrive == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "错误: 无法打开设备 " << drivePath << "\n";
        oss << "  Windows 错误码: " << err << "\n";
        switch (err) {
            case ERROR_ACCESS_DENIED:   // 5
                oss << "  原因: 访问被拒绝. 请以【管理员身份】运行本程序.\n";
                break;
            case ERROR_FILE_NOT_FOUND:  // 2
                oss << "  原因: 找不到指定设备. 请确认 U 盘已插入并分配了盘符.\n";
                break;
            case ERROR_SHARING_VIOLATION: // 32
                oss << "  原因: 设备被其他进程占用. 请关闭所有访问该磁盘的程序.\n";
                break;
            default:
                oss << "  请确认: 1) U盘已插入  2) 盘符正确  3) 以管理员身份运行\n";
        }
        lastError = oss.str();
        return false;
    }

    // --- 获取设备大小 ---
    GET_LENGTH_INFORMATION lengthInfo;
    DWORD bytesReturned;
    if (DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO,
                        nullptr, 0, &lengthInfo, sizeof(lengthInfo),
                        &bytesReturned, nullptr)) {
        deviceSize = static_cast<uint64_t>(lengthInfo.Length.QuadPart);
    } else {
        deviceSize = 0;  // 获取失败, 不影响后续读取
    }

    return true;
}

void DiskReader::close() {
    if (hDrive != INVALID_HANDLE_VALUE) {
        CloseHandle(hDrive);
        hDrive = INVALID_HANDLE_VALUE;
    }
    drivePath.clear();
    deviceSize = 0;
}

bool DiskReader::isOpen() const {
    return hDrive != INVALID_HANDLE_VALUE;
}

bool DiskReader::readBytes(uint64_t offset, uint32_t size, std::vector<uint8_t>& buffer) {
    if (!isOpen()) {
        lastError = "错误: 设备未打开.";
        return false;
    }

    buffer.resize(size);

    // --- 设置文件指针到指定偏移 ---
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = static_cast<LONGLONG>(offset);

    if (!SetFilePointerEx(hDrive, liOffset, nullptr, FILE_BEGIN)) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "错误: SetFilePointerEx 失败 (偏移=" << offset
            << "), 错误码=" << err;
        lastError = oss.str();
        return false;
    }

    // --- 读取数据 ---
    // 注意: 物理卷要求读写大小必须是扇区大小的整数倍
    // 此处 size 由调用者确保满足该要求
    DWORD bytesRead = 0;
    if (!ReadFile(hDrive, buffer.data(), size, &bytesRead, nullptr)) {
        DWORD err = GetLastError();
        std::ostringstream oss;
        oss << "错误: ReadFile 失败 (偏移=" << offset
            << ", 请求=" << size << "字节), 错误码=" << err;
        lastError = oss.str();
        return false;
    }

    if (bytesRead != size) {
        std::ostringstream oss;
        oss << "警告: ReadFile 读取不完整 (请求=" << size
            << ", 实际=" << bytesRead << "字节)";
        lastError = oss.str();
        buffer.resize(bytesRead);
        return false;
    }

    return true;
}

bool DiskReader::readSector(uint32_t sectorNumber, std::vector<uint8_t>& buffer) {
    return readSectors(sectorNumber, 1, buffer);
}

bool DiskReader::readSectors(uint32_t startSector, uint32_t count,
                              std::vector<uint8_t>& buffer) {
    uint64_t offset = static_cast<uint64_t>(startSector) * bytesPerSector;
    uint32_t size = count * bytesPerSector;
    return readBytes(offset, size, buffer);
}

bool DiskReader::readCluster(uint32_t clusterNumber, uint32_t sectorsPerCluster,
                              uint32_t dataStartSector, std::vector<uint8_t>& buffer) {
    if (clusterNumber < 2) {
        std::ostringstream oss;
        oss << "错误: 非法簇号 " << clusterNumber << " (有效簇号从 2 开始).";
        lastError = oss.str();
        return false;
    }

    // FAT32: 数据区第一个簇的簇号为 2
    // 扇区 = 数据区起始 + (簇号 - 2) * 每簇扇区数
    uint32_t sector = dataStartSector + (clusterNumber - 2) * sectorsPerCluster;
    return readSectors(sector, sectorsPerCluster, buffer);
}
