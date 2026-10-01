// scanner.cpp —— 局域网 IP 段 + DHCP 服务器扫描器实现
//
// 线程模型：
//   Scanner（主线程）→ ScanWorker（QThread）
//     ├─ 调用 nmap 子进程（-sn）完成主机发现
//     └─ 局部 QThreadPool 并发补齐主机名 / MAC / DHCP 服务器判定
//   取消用共享的 ScanCancelToken（原子标志），worker 信号一律 QueuedConnection 回到主线程。

#include "scanner.h"
#include "cameradetector.h"
#include "netutils.h"
#include "nmaprunner.h"

#include <QDebug>
#include <QHash>
#include <QRunnable>
#include <QSet>
#include <QThreadPool>

#include <algorithm>
#include <exception>

namespace
{
/// 主机名解析超时与 ARP 补齐时测量延迟的 ping 超时
const int kHostNameTimeoutMs = 800;
const int kLatencyPingTimeoutMs = 400;
/// 复核 nmap 上线判定时的 ICMP 回显超时
const int kVerifyPingTimeoutMs = 400;

/// 复核 nmap 上报的在线主机，剔除「幽灵主机」。
///
/// 无管理员权限时 nmap 退化为 TCP connect() 模式，「连接被拒绝」同样算主机在线。
/// 跨网段探测时上游路由器会替不存在的地址代答 RST（实测 192.168.32.0/24 整段
/// 253 个地址全部被判定在线），因此需要二次确认：
///   - 本机 ARP 邻居表命中 → 二层直连证据，直接采信；
///   - 本机自身地址 → 必然在线；
///   - 其余地址要求 ICMP 回显成功。
/// 上述伪造地址不响应 ICMP 回显（返回 Destination port unreachable），会被剔除。
QVector<NmapHost> confirmAliveHosts(const QVector<NmapHost> &hosts, int maxParallelism,
                                    const QSharedPointer<ScanCancelToken> &token)
{
    if (hosts.isEmpty())
        return hosts;

    const QHash<QString, QString> arpTable = NetUtils::readArpTable();
    const QString localIp = NetUtils::localIpv4Address();

    const int hostCount = hosts.size();
    QVector<bool> keep(hostCount, false);
    bool *keepData = keep.data();

    QThreadPool pool;
    pool.setMaxThreadCount(qMax(1, maxParallelism));

    for (int i = 0; i < hostCount; ++i)
    {
        if (!token.isNull() && token->isCancelled())
            break;

        const QString ip = hosts.at(i).ipAddress;
        if (arpTable.contains(ip) || (!localIp.isEmpty() && ip == localIp))
        {
            keepData[i] = true;
            continue;
        }

        pool.start(QRunnable::create([ip, i, keepData]() {
            keepData[i] = NetUtils::icmpPing(ip, kVerifyPingTimeoutMs, nullptr);
        }));
    }

    pool.waitForDone();

    QVector<NmapHost> confirmed;
    confirmed.reserve(hostCount);
    for (int i = 0; i < hostCount; ++i)
    {
        if (keepData[i])
            confirmed.append(hosts.at(i));
    }

    if (confirmed.size() != hostCount)
    {
        qWarning("scanner: nmap reported %d alive host(s), %d rejected by ICMP/ARP check",
                 hostCount, hostCount - confirmed.size());
    }

    return confirmed;
}
} // namespace

ScanWorker::ScanWorker(const QStringList &ipList, int maxParallelism, bool ipv6Enabled,
                       const QString &adapterName, const QSharedPointer<ScanCancelToken> &token,
                       QObject *parent)
    : QThread(parent)
    , m_ipList(ipList)
    , m_maxParallelism(qMax(1, maxParallelism))
    , m_ipv6Enabled(ipv6Enabled)
    , m_adapterName(adapterName)
    , m_token(token)
{
}

void ScanWorker::run()
{
    try
    {
        const int total = m_ipList.size();
        if (total == 0)
        {
            emit completed(QVector<DhcpServerInfo>());
            return;
        }

        // 1) 交给 nmap 子进程做主机发现（进度占前 50%）
        QString nmapError;
        const QVector<NmapHost> aliveHosts = NmapRunner::scanHosts(
            m_ipList, m_adapterName, [this](int percent) { emit progressChanged(percent / 2); },
            m_token.data(), &nmapError);

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        if (!nmapError.isEmpty())
        {
            emit failed(nmapError);
            return;
        }

        // 2) 复核 nmap 的上线判定，剔除路由器代答 RST 造成的「幽灵主机」
        const QVector<NmapHost> verifiedHosts =
            confirmAliveHosts(aliveHosts, m_maxParallelism, m_token);

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        // 3) 组播发现（SSDP / mDNS）：为「一个端口都不开的家用 P2P 摄像头」收集线索。
        //    这一步只在这里跑一次，结果按 IP 存表，供后面的并发识别只读查询。
        //    进度停在 50%，不参与进度公式。
        {
            const QString localIp = NetUtils::localIpv4Address();
            if (!localIp.isEmpty())
            {
                m_hints = QSharedPointer<const QHash<QString, DiscoveryHint>>::create(
                    NetDiscovery::discover(localIp, 1500, m_token.data()));
            }
        }

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        // 4) 并发补齐 nmap 未提供的主机名 / MAC / DHCP 判定（进度占后 50%）
        QVector<DhcpServerInfo> collected;
        collected.reserve(verifiedHosts.size());
        for (const NmapHost &host : verifiedHosts)
        {
            DhcpServerInfo info;
            info.ipAddress = host.ipAddress;
            info.macAddress = host.macAddress;
            info.hostName = host.hostName;
            info.responseTime = QDateTime::currentDateTime();
            info.isActive = true;
            info.pingMs = host.rttMs;
            collected.append(info);
        }

        if (!collected.isEmpty())
        {
            const int hostCount = collected.size();
            // 提前取出裸指针：各任务只写自己的下标，避免 QVector 在并发访问时 detach
            DhcpServerInfo *hostData = collected.data();
            QSharedPointer<QAtomicInt> completedCount = QSharedPointer<QAtomicInt>::create();

            QThreadPool pool;
            pool.setMaxThreadCount(qMax(1, m_maxParallelism));

            for (int i = 0; i < hostCount; ++i)
            {
                if (!m_token.isNull() && m_token->isCancelled())
                    break;

                pool.start(QRunnable::create([this, i, hostCount, hostData, completedCount]() {
                    enrichHost(hostData[i]);
                    const int current = completedCount->fetchAndAddRelaxed(1) + 1;
                    emit progressChanged(50 + static_cast<int>(static_cast<double>(current) / hostCount * 50));
                }));
            }

            pool.waitForDone();
        }

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        applyArpResults(collected);

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        // 3.5) 提取 IPv6：先预热 NDP 邻居表（向 ff02::1 发回显），再按 MAC 回填
        // IPv6 提取：仅在设置开启时预热邻居表并按 MAC 回填
        if (m_ipv6Enabled)
        {
            prepareIpv6Neighbors();
            applyIpv6Results(collected);
        }

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        // 5) 为本次扫描过但未发现设备的地址补一条「无设备」记录。
        //    nmap 只返回在线主机，若不补齐，IP 分布图里这些地址会保持默认底色，
        //    与图例中「未扫描」的颜色相同，无法区分。
        {
            QSet<QString> aliveIps;
            aliveIps.reserve(collected.size());
            for (const DhcpServerInfo &info : collected)
                aliveIps.insert(info.ipAddress);

            for (const QString &ip : m_ipList)
            {
                if (aliveIps.contains(ip))
                    continue;

                DhcpServerInfo info;
                info.ipAddress = ip;
                info.responseTime = QDateTime::currentDateTime();
                collected.append(info);
            }
        }

        if (!m_token.isNull() && m_token->isCancelled())
        {
            emit cancelled();
            return;
        }

        // 按 IP 数值升序排序（与 C# 一致）
        std::sort(collected.begin(), collected.end(),
                  [](const DhcpServerInfo &left, const DhcpServerInfo &right) {
                      return NetUtils::ipToLong(left.ipAddress) < NetUtils::ipToLong(right.ipAddress);
                  });

        emit completed(collected);
    }
    catch (const std::exception &ex)
    {
        emit failed(QString::fromUtf8(ex.what()));
    }
    catch (...)
    {
        emit failed(QStringLiteral("扫描时发生未知错误"));
    }
}

void ScanWorker::enrichHost(DhcpServerInfo &info) const
{
    // 未安装 Npcap 时 nmap 拿不到 MAC，且 nmap 用了 -n 不解析主机名，这里按原有方式补齐
    if (info.macAddress.isEmpty())
        info.macAddress = queryMacAddress(info.ipAddress);
    if (info.hostName.isEmpty())
        info.hostName = queryHostName(info.ipAddress);

    info.isDhcpServer = isLikelyRouterOrDhcp(info.ipAddress, info.hostName);

    // 摄像头识别（RTSP / Web 指纹 / 厂商端口 / MAC 厂商 / 主机名 / 组播线索综合打分）
    const CameraDetection camera = CameraDetector::detect(
        info.ipAddress, info.macAddress, info.hostName, hintFor(info.ipAddress));
    info.isCamera = camera.isCamera;
    info.cameraEvidence = camera.evidence;
}

const DiscoveryHint *ScanWorker::hintFor(const QString &ip) const
{
    if (m_hints.isNull())
        return nullptr;

    const auto it = m_hints->constFind(ip);
    return it == m_hints->constEnd() ? nullptr : &it.value();
}

void ScanWorker::applyArpResults(QVector<DhcpServerInfo> &results) const
{
    const QHash<QString, QString> arpTable = NetUtils::readArpTable();
    if (arpTable.isEmpty())
        return;

    // nmap 走 connect() 模式时，对静默丢弃探测端口的主机可能漏报；
    // 只要该地址出现在 ARP 邻居表里，就说明本机最近和它通信过，补一条记录
    QSet<QString> knownIps;
    for (const DhcpServerInfo &info : results)
        knownIps.insert(info.ipAddress);

    QStringList pendingIps;
    for (const QString &ip : m_ipList)
    {
        if (!knownIps.contains(ip) && arpTable.contains(ip))
            pendingIps.append(ip);
    }
    if (pendingIps.isEmpty())
        return;

    struct ArpFix
    {
        QString hostName;
        bool isDhcpServer = false;
        bool isCamera = false;
        QString cameraEvidence;
        qint64 pingMs = -1;
    };

    QVector<ArpFix> fixes(pendingIps.size());
    // 提前取出裸指针：各任务只写自己的下标，避免 QVector 在并发访问时 detach
    ArpFix *fixData = fixes.data();

    {
        QThreadPool pool;
        pool.setMaxThreadCount(qBound(2, m_maxParallelism / 4, 8));

        for (int k = 0; k < pendingIps.size(); ++k)
        {
            const QString ip = pendingIps.at(k);
            const QString mac = arpTable.value(ip);
            pool.start(QRunnable::create([this, ip, mac, k, fixData]() {
                fixData[k].hostName = queryHostName(ip);
                fixData[k].isDhcpServer = isLikelyRouterOrDhcp(ip, fixData[k].hostName);

                const CameraDetection camera =
                    CameraDetector::detect(ip, mac, fixData[k].hostName, hintFor(ip));
                fixData[k].isCamera = camera.isCamera;
                fixData[k].cameraEvidence = camera.evidence;

                // 该地址已在邻居表中，快速 ping 一次拿真实延迟（失败则保持 -1）
                NetUtils::icmpPing(ip, kLatencyPingTimeoutMs, &fixData[k].pingMs);
            }));
        }

        pool.waitForDone();
    }

    for (int k = 0; k < pendingIps.size(); ++k)
    {
        const QString ip = pendingIps.at(k);
        const ArpFix &fix = fixes.at(k);

        DhcpServerInfo info;
        info.ipAddress = ip;
        info.macAddress = arpTable.value(ip);
        info.hostName = fix.hostName;
        info.isDhcpServer = fix.isDhcpServer;
        info.isCamera = fix.isCamera;
        info.cameraEvidence = fix.cameraEvidence;
        info.isActive = true;
        info.pingMs = fix.pingMs;
        info.responseTime = QDateTime::currentDateTime();
        results.append(info);
    }
}

void ScanWorker::prepareIpv6Neighbors() const
{
    if (m_ipList.isEmpty())
        return;

    // 只预热本次扫描实际使用的网卡，避免逐张网卡发组播造成额外耗时。
    // 用户指定了网卡时按 GUID 精确定位；未指定时退回按网段前缀匹配。
    const QStringList parts = m_ipList.first().split(QLatin1Char('.'));
    if (parts.size() != 4)
        return;
    const QString prefix = QStringLiteral("%1.%2.%3.").arg(parts.at(0), parts.at(1), parts.at(2));

    const QVector<NetUtils::LocalInterface> interfaces = NetUtils::localInterfaces();
    for (const NetUtils::LocalInterface &iface : interfaces)
    {
        if (iface.index <= 0)
            continue;
        if (!m_adapterName.isEmpty())
        {
            if (iface.adapterName == m_adapterName)
                NetUtils::primeIpv6Neighbors(iface.index);
            continue;
        }
        if (iface.ipv4.startsWith(prefix))
            NetUtils::primeIpv6Neighbors(iface.index);
    }
}

void ScanWorker::applyIpv6Results(QVector<DhcpServerInfo> &results) const
{
    const QHash<QString, QString> neighbors = NetUtils::readIpv6Neighbors();
    if (neighbors.isEmpty())
        return;

    for (DhcpServerInfo &info : results)
    {
        if (!info.isActive)
            continue;

        const QString mac = NetUtils::formatMac(info.macAddress);
        if (mac.isEmpty())
            continue;

        const QString ipv6 = neighbors.value(mac);
        if (!ipv6.isEmpty())
            info.ipv6Address = ipv6;
    }
}

bool ScanWorker::isLikelyRouterOrDhcp(const QString &ip, const QString &hostName) const
{
    static const QStringList routerKeywords = {
        QStringLiteral("miwifi"), QStringLiteral("xiaoqiang"), QStringLiteral("openwrt"),
        QStringLiteral("asus"), QStringLiteral("tp-link"), QStringLiteral("tplink"),
        QStringLiteral("netgear"), QStringLiteral("huawei"), QStringLiteral("h3c"),
        QStringLiteral("小米路由"), QStringLiteral("华硕"), QStringLiteral("路由器"),
        QStringLiteral("网关"), QStringLiteral("router"), QStringLiteral("gateway"),
        QStringLiteral("路由")};

    const QString name = hostName.toLower();
    for (const QString &keyword : routerKeywords)
    {
        if (name.contains(keyword))
            return true;
    }

    const QString gatewayIp = NetUtils::defaultGatewayIp();
    return !gatewayIp.isEmpty() && ip.compare(gatewayIp, Qt::CaseInsensitive) == 0;
}

QString ScanWorker::queryMacAddress(const QString &ip) const
{
    // 本机自身地址不在 ARP 邻居表里，优先从网卡读取
    const QString localIp = NetUtils::localIpv4Address();
    if (!localIp.isEmpty() && ip.compare(localIp, Qt::CaseInsensitive) == 0)
    {
        const QString localMac = NetUtils::localMacAddress();
        if (!localMac.isEmpty())
            return localMac;
    }

    const QString mac = NetUtils::queryArpEntry(ip);
    if (!mac.isEmpty())
        return mac;

    // 触发一次通信让系统补齐 ARP 缓存后再查一次
    NetUtils::icmpPing(ip, 200, nullptr);
    const QString macAfterPing = NetUtils::queryArpEntry(ip);
    return macAfterPing.isEmpty() ? QStringLiteral("未知") : macAfterPing;
}

QString ScanWorker::queryHostName(const QString &ip) const
{
    const QString hostName = NetUtils::resolveHostName(ip, kHostNameTimeoutMs);
    return hostName.isEmpty() ? QStringLiteral("未知") : hostName;
}

Scanner::Scanner(QObject *parent)
    : QObject(parent)
{
    // 跨线程信号需要元类型（main.cpp 也会注册，这里保证单独使用时不缺）
    qRegisterMetaType<QVector<DhcpServerInfo>>("QVector<DhcpServerInfo>");
}

Scanner::~Scanner()
{
    stopScan();

    if (!m_worker)
        return;

    // 断开信号：析构过程中不再回调本对象
    disconnect(m_worker, nullptr, this, nullptr);

    if (!m_worker->wait(5000))
    {
        // 线程未能及时结束：解除父子关系，交给 QThread::finished -> deleteLater 自行回收，
        // 不使用 terminate（避免线程持锁时被强杀导致死锁）
        m_worker->setParent(nullptr);
        m_worker = nullptr;
        return;
    }

    delete m_worker;
    m_worker = nullptr;
}

void Scanner::stopScan()
{
    if (m_isScanning && !m_token.isNull())
        m_token->cancel();
}

bool Scanner::startIpRangeScan(const QString &startIp, const QString &endIp,
                               const QString &adapterName, QString *errorMessage)
{
    if (m_isScanning)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("扫描已在进行中");
        return false;
    }

    const qint64 startNum = NetUtils::ipToLong(startIp);
    const qint64 endNum = NetUtils::ipToLong(endIp);
    if (startNum < 0 || endNum < 0)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("IP格式不正确: %1").arg(startNum < 0 ? startIp : endIp);
        return false;
    }
    if (startNum > endNum)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("起始IP不能大于结束IP");
        return false;
    }

    // 构建有效 IP 列表：跳过最后一段为 0 的地址，且只保留内网地址
    QStringList ipList;
    QSet<qint64> subnets;
    bool tooManySubnets = false;

    for (qint64 value = startNum; value <= endNum; ++value)
    {
        if ((value & 0xFF) == 0)
            continue;

        const QString ip = NetUtils::longToIp(value);
        if (!NetUtils::isPrivateIp(ip))
            continue;

        subnets.insert(value >> 8); // 前三段相同视为同一网段
        if (subnets.size() > 100)
        {
            // 网段过多时只统计网段数，不再累积 IP，避免超大范围占用大量内存
            tooManySubnets = true;
            continue;
        }
        ipList.append(ip);
    }

    if (ipList.isEmpty() && !tooManySubnets)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("扫描范围内不包含内网地址");
        return false;
    }

    if (tooManySubnets)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("TOO_MANY_SUBNETS:%1").arg(subnets.size());
        return false;
    }

    m_isScanning = true;
    m_token = QSharedPointer<ScanCancelToken>::create();
    m_worker = new ScanWorker(ipList, m_maxParallelism, m_ipv6Enabled, adapterName, m_token, this);

    connect(m_worker, &ScanWorker::progressChanged, this, &Scanner::scanProgress, Qt::QueuedConnection);
    connect(m_worker, &ScanWorker::completed, this, &Scanner::scanCompleted, Qt::QueuedConnection);
    connect(m_worker, &ScanWorker::failed, this, &Scanner::scanError, Qt::QueuedConnection);
    // scanCompleted 必须先于 scanFinished 到达，因此都用队列连接，保证投递顺序
    connect(m_worker, &ScanWorker::completed, this, &Scanner::handleWorkerFinished, Qt::QueuedConnection);
    connect(m_worker, &ScanWorker::failed, this, &Scanner::handleWorkerFinished, Qt::QueuedConnection);
    connect(m_worker, &ScanWorker::cancelled, this, &Scanner::handleWorkerFinished, Qt::QueuedConnection);
    connect(m_worker, &QThread::finished, m_worker, &QObject::deleteLater);

    m_worker->start();
    return true;
}

void Scanner::handleWorkerFinished()
{
    // 线程对象由 QThread::finished -> deleteLater 回收，这里只更新状态
    m_isScanning = false;
    m_worker = nullptr;
    emit scanFinished();
}
