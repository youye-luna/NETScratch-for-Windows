#pragma once

#include "scanner.h"

#include <QDateTime>
#include <QString>
#include <QVector>

/// 扫描历史中的设备记录（可序列化存储）
struct ScanHistoryDevice
{
    QString ipAddress;
    QString macAddress;
    QString hostName;
    qint64 pingMs = -1;
    bool isActive = false;
    bool isDhcpServer = false;
    QDateTime responseTime;

    /// 从扫描结果转换
    static ScanHistoryDevice from(const DhcpServerInfo &info);

    /// 还原为扫描结果对象
    DhcpServerInfo toServerInfo() const;
};

/// 一次扫描的历史记录
struct ScanHistoryRecord
{
    QString id;
    QDateTime scanTime;
    QString startIp;
    QString endIp;
    QVector<ScanHistoryDevice> devices;
    QString filePath;
};

/// 扫描历史持久化 —— 每次扫描保存为独立 CSV 文件，存储在程序目录下的 ScanHistory 文件夹中。
/// 文件名格式：2026年08月05日 192.168.1.1~192.168.1.254（IP）.csv
namespace ScanHistoryStore
{
/// 加载全部历史记录（按时间倒序，并按当前保存配置清理过期记录）
QVector<ScanHistoryRecord> load();

/// 新增一条历史记录（保存 CSV 文件并按当前保存配置清理），回写 record->filePath
void save(ScanHistoryRecord *record);

/// 按当前保存配置立即清理过期历史记录（保存配置变更后调用）
void prune();

/// 删除一条历史记录（删除 CSV 文件）
void remove(const ScanHistoryRecord &record);

/// 清空所有历史记录（删除 ScanHistory 文件夹下所有 CSV）
void clear();
} // namespace ScanHistoryStore
