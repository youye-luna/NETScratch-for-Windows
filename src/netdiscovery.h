#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

struct ScanCancelToken;

/// 组播/广播发现得到的单台设备线索——逐 IP 探测之外的第二条信息来源。
/// 家用 P2P 摄像头（萤石 / 米家 / V380 方案等）常常不开放任何端口，逐 IP 敲门
/// 完全看不到它，但它开机后往往会在 mDNS / SSDP 里主动播报自己，这里就是收这些播报。
struct DiscoveryHint
{
    QString ssdpModel;        ///< UPnP 设备描述里的 modelName
    QString ssdpManufacturer; ///< UPnP 设备描述里的 manufacturer
    QString ssdpFriendlyName; ///< UPnP 设备描述里的 friendlyName
    QString ssdpServer;       ///< SSDP 应答的 SERVER 头（如 "GoAhead-Webs/2.5.0"）
    QString mdnsName;         ///< mDNS 实例名（如 "IPC-1234._rtsp._tcp.local"）
    QString mdnsTxt;          ///< mDNS TXT 记录文本（常含型号，如 "model=IPC"）
    QStringList mdnsServices; ///< 该设备声明的服务类型，如 _rtsp._tcp / _hap._tcp
};

namespace NetDiscovery
{
/// 做一轮全网段发现：主动发 SSDP M-SEARCH 与 mDNS 查询，并在预算内接收
/// 组播应答（同时也顺手收别的设备周期性播报的 NOTIFY / announcement）。
/// 返回 IP -> 线索；没有任何线索的设备不会出现在表里。
/// localIpv4 指定组播出口网卡，为空时走系统默认路由。
/// token 非空时会响应「停止扫描」，可随时中断。
QHash<QString, DiscoveryHint> discover(const QString &localIpv4, int budgetMs,
                                       const ScanCancelToken *token);
} // namespace NetDiscovery