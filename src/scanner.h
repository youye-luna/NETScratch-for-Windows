#pragma once

#include <QAtomicInt>
#include <QDateTime>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include "netdiscovery.h"

/// 单个扫描结果
struct DhcpServerInfo
{
    QString ipAddress;
    QString macAddress;
    QString hostName;
    QDateTime responseTime;
    bool isActive = false;
    bool isDhcpServer = false;
    /// 是否为网络摄像头（IPC / DVR / NVR 等视频监控设备）
    bool isCamera = false;
    /// 摄像头识别依据（未判定为摄像头时为空）
    QString cameraEvidence;
    /// 提取到的 IPv6 地址（链路本地 fe80::，未提取到为空）
    QString ipv6Address;
    qint64 pingMs = 0;
};

Q_DECLARE_METATYPE(DhcpServerInfo)
Q_DECLARE_METATYPE(QVector<DhcpServerInfo>)

/// 扫描取消标志（跨线程共享）
struct ScanCancelToken
{
    QAtomicInt flag;

    void cancel() { flag.storeRelease(1); }
    bool isCancelled() const { return flag.loadAcquire() != 0; }
};

/// 扫描工作线程：用 nmap 做主机发现，再用局部线程池并发补齐主机信息
class ScanWorker : public QThread
{
    Q_OBJECT
public:
    ScanWorker(const QStringList &ipList, int maxParallelism, bool ipv6Enabled,
               const QString &adapterName, const QSharedPointer<ScanCancelToken> &token,
               QObject *parent = nullptr);

    signals:
    void progressChanged(int percent);
    void completed(const QVector<DhcpServerInfo> &results);
    void failed(const QString &message);
    void cancelled();

protected:
    void run() override;

private:
    /// 补齐 nmap 未提供的信息（主机名 / MAC / 是否为 DHCP 服务器）
    void enrichHost(DhcpServerInfo &info) const;
    void applyArpResults(QVector<DhcpServerInfo> &results) const;
    /// 取某个 IP 的组播发现线索，没有返回 nullptr
    const DiscoveryHint *hintFor(const QString &ip) const;
    /// 预热扫描网段所在网卡的 IPv6 邻居表（向 ff02::1 发回显）
    void prepareIpv6Neighbors() const;
    /// 按 MAC 从 IPv6 邻居表回填 ipv6Address
    void applyIpv6Results(QVector<DhcpServerInfo> &results) const;
    bool isLikelyRouterOrDhcp(const QString &ip, const QString &hostName) const;
    QString queryMacAddress(const QString &ip) const;
    QString queryHostName(const QString &ip) const;

    QStringList m_ipList;
    int m_maxParallelism;
    /// 是否提取 IPv6（关闭时跳过邻居表预热与回填）
    bool m_ipv6Enabled;
    /// 指定扫描用的网卡（适配器 GUID）；为空表示交给 nmap 自动选卡
    QString m_adapterName;
    QSharedPointer<ScanCancelToken> m_token;
    /// 本次扫描收集到的组播发现线索（IP -> 线索）；并发识别时只读共享
    QSharedPointer<const QHash<QString, DiscoveryHint>> m_hints;
};

/// 局域网 IP 段 + DHCP 服务器扫描器（对外接口）
class Scanner : public QObject
{
    Q_OBJECT
public:
    explicit Scanner(QObject *parent = nullptr);
    ~Scanner() override;

    bool isScanning() const { return m_isScanning; }
    int maxParallelism() const { return m_maxParallelism; }
    void setMaxParallelism(int value) { m_maxParallelism = value; }

    /// 是否提取并显示 IPv6（默认开启）
    void setIpv6Enabled(bool enabled) { m_ipv6Enabled = enabled; }

    /// 停止当前扫描（立即置取消标志，不阻塞等待线程结束）
    void stopScan();

    /// 扫描指定 IP 范围；adapterName 为指定网卡的适配器 GUID（空串 = 自动选卡）。
    /// 失败时把错误信息写入 errorMessage 并返回 false。
    /// 错误信息可能是 "TOO_MANY_SUBNETS:<网段数>" 形式。
    bool startIpRangeScan(const QString &startIp, const QString &endIp, const QString &adapterName,
                          QString *errorMessage);

signals:
    void scanProgress(int progress);
    void scanCompleted(const QVector<DhcpServerInfo> &results);
    void scanError(const QString &message);
    /// 无论成功、失败还是被取消都会发出，用于恢复界面状态
    void scanFinished();

private slots:
    void handleWorkerFinished();

private:
    int m_maxParallelism = 30;
    bool m_isScanning = false;
    bool m_ipv6Enabled = true;
    QSharedPointer<ScanCancelToken> m_token;
    ScanWorker *m_worker = nullptr;
};
