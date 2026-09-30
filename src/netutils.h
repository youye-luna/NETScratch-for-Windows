#pragma once

#include <QHash>
#include <QString>
#include <QVector>

/// 底层网络工具（ICMP / TCP / ARP / 本机信息 / 反向 DNS）
namespace NetUtils
{
/// IPv4 字符串转 32 位数值，失败返回 -1
qint64 ipToLong(const QString &ip);

/// 32 位数值转 IPv4 字符串
QString longToIp(qint64 value);

/// 是否为内网地址（10.x / 172.16-31.x / 192.168.x）
bool isPrivateIp(const QString &ip);

/// ICMP 探测：成功返回 true 并写入往返毫秒数
bool icmpPing(const QString &ip, int timeoutMs, qint64 *roundTripMs);

/// 端口探测的确定性结果
struct PortProbeResult
{
    int openPort = -1;    ///< 探测到的第一个开放端口，无则 -1
    bool hostAlive = false; ///< 主机在线（连接被拒绝即认为在线）
};

/// 在总时间预算内按顺序探测端口，返回第一个确定性结果
PortProbeResult probePorts(const QString &ip, const QVector<int> &ports, int totalBudgetMs);

/// arp -a 全表，返回 IP -> MAC（AA-BB-CC-DD-EE-FF 大写）
QHash<QString, QString> readArpTable(int timeoutMs = 1500);

/// arp -a <ip> 单条查询，失败返回空
QString queryArpEntry(const QString &ip, int timeoutMs = 1000);

/// MAC 归一化为 AA-BB-CC-DD-EE-FF（大写），非法返回空
QString formatMac(const QString &raw);

/// 本机网卡（名称 / IPv6 接口索引 / IPv4），用于定位扫描网段所在网卡
struct LocalInterface
{
    QString name;        ///< 网卡友好名称
    int index = -1;      ///< IPv6 接口索引（ping -6 ... %index 使用）
    QString ipv4;        ///< 该网卡第一个非回环 IPv4
    QString adapterName; ///< Windows 适配器 GUID（{...}），用于拼 Npcap 设备名
};

/// 枚举本机已启用且配置了 IPv4 的网卡
QVector<LocalInterface> localInterfaces();

/// 由适配器 GUID 拼出 Npcap 设备名（\Device\NPF_{GUID}）；adapterName 为空时返回空
QString npcapDeviceName(const QString &adapterName);

/// 本机是否装有 Npcap/WinPcap（未装时 nmap 退化到 connect 模式，不能用 -e）
bool isNpcapAvailable();

/// 向 ff02::1 发一次 ICMPv6 回显，促使系统补齐 NDP 邻居表
void primeIpv6Neighbors(int interfaceIndex);

/// IPv6 邻居表：MAC(AA-BB-CC-DD-EE-FF 大写) -> 链路本地 fe80:: 地址
QHash<QString, QString> readIpv6Neighbors(int timeoutMs = 3000);

/// 本机物理网卡 MAC（AA-BB-CC-DD-EE-FF 大写），失败返回空
QString localMacAddress();

/// 默认网关 IPv4，失败返回空
QString defaultGatewayIp();

/// 本机第一个非回环 IPv4，失败返回空
QString localIpv4Address();

/// 带超时的反向 DNS 解析，失败/超时返回空
QString resolveHostName(const QString &ip, int timeoutMs);
} // namespace NetUtils
