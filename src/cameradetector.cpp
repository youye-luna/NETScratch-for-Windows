// cameradetector.cpp —— 网络摄像头（IPC / DVR / NVR 等视频监控设备）识别实现
//
// 判定思路：局域网内的摄像头通常会暴露 RTSP 服务、厂商私有端口或带明显指纹的
// Web 管理页，因此按证据加权打分，累计分数达到阈值才判定，尽量降低误报。
//
// 家用 P2P 摄像头（萤石 / 米家 / V380 方案等）为了穿透 NAT 只出站连云端，
// 局域网里往往一个端口都不开，光靠端口扫描永远看不到。这类设备只能靠
// 组播发现（mDNS / SSDP）与 MAC 厂商前缀来认，所以 detect() 还接收一份
// 由 NetDiscovery 预先收集好的 DiscoveryHint。
//
// 网络探测用裸 winsock（非阻塞 connect + WSAPoll），与 netutils.cpp 保持一致：
// 不依赖 Qt 事件循环，可安全地在扫描线程池中被并发调用。

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "cameradetector.h"

#include "netutils.h"

// 老版本头文件可能没有这两个定义
#ifndef POLLWRNORM
#define POLLWRNORM 0x0010
#endif
#ifndef POLLRDNORM
#define POLLRDNORM 0x0100
#endif

namespace
{
/// 得分达到该值即判定为摄像头（单项强证据即可命中，弱证据需叠加）
const int kScoreThreshold = 5;

/// 各阶段时间预算
/// 端口数翻倍（16 个），预算同步放宽，仍远小于一次 ping 的等待
const int kPortScanBudgetMs = 800;
const int kExchangeBudgetMs = 400;

/// 参与探测的端口：RTSP / HTTP / 厂商私有端口
const QVector<int> &candidatePorts()
{
    static const QVector<int> ports{554,  8554, 10554, 80,   8000,  8080,  88,    34567,
                                    37777, 8081, 8001,  2020, 5000,  6666,  8899,  9000};
    return ports;
}

/// 被当作 RTSP 服务来握手的端口
const QVector<int> &rtspPorts()
{
    static const QVector<int> ports{554, 8554, 10554};
    return ports;
}

/// 可能提供 Web 管理页的端口
const QVector<int> &httpPorts()
{
    static const QVector<int> ports{80, 8000, 8080, 8001, 8081, 88, 5000, 9000};
    return ports;
}

/// 惰性初始化 Winsock（整个进程只执行一次，静态局部变量初始化线程安全）
void ensureWinsock()
{
    static const bool initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Q_UNUSED(initialized);
}

/// 严格校验 IPv4 字符串并填充 sockaddr_in
bool buildSockAddr(const QString &ip, sockaddr_in *addr)
{
    const QStringList parts = ip.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;

    quint32 value = 0;
    for (const QString &part : parts)
    {
        bool ok = false;
        const int octet = part.toInt(&ok);
        if (!ok || octet < 0 || octet > 255)
            return false;
        value = (value << 8) | static_cast<quint32>(octet);
    }

    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(value);
    addr->sin_port = 0;
    return true;
}

/// 并发探测端口：返回所有在预算内完成三次握手的端口
QVector<int> scanPorts(const QString &ip, const QVector<int> &ports, int budgetMs)
{
    QVector<int> openPorts;
    if (ports.isEmpty())
        return openPorts;

    sockaddr_in target;
    if (!buildSockAddr(ip, &target))
        return openPorts;

    ensureWinsock();

    QVector<SOCKET> socks;
    QVector<int> pendingPorts;
    QVector<WSAPOLLFD> pollFds;
    socks.reserve(ports.size());
    pendingPorts.reserve(ports.size());
    pollFds.reserve(ports.size());

    for (int port : ports)
    {
        const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
            continue;

        u_long nonBlocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0)
        {
            ::closesocket(sock);
            continue;
        }

        target.sin_port = htons(static_cast<u_short>(port));
        if (::connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0)
        {
            openPorts.append(port); // 立即连接成功
            ::closesocket(sock);
            continue;
        }
        if (WSAGetLastError() == WSAECONNREFUSED)
        {
            ::closesocket(sock); // 收到 RST：端口关闭
            continue;
        }

        socks.append(sock);
        pendingPorts.append(port);

        WSAPOLLFD pollFd;
        pollFd.fd = sock;
        pollFd.events = POLLWRNORM;
        pollFd.revents = 0;
        pollFds.append(pollFd);
    }

    if (!pollFds.isEmpty() && WSAPoll(pollFds.data(), pollFds.size(), budgetMs) > 0)
    {
        for (int i = 0; i < pollFds.size(); ++i)
        {
            if (pollFds.at(i).revents == 0)
                continue;

            int socketError = 0;
            int socketErrorSize = static_cast<int>(sizeof(socketError));
            if (getsockopt(pollFds.at(i).fd, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char *>(&socketError), &socketErrorSize)
                    == 0
                && socketError == 0)
            {
                openPorts.append(pendingPorts.at(i));
            }
        }
    }

    for (SOCKET sock : socks)
        ::closesocket(sock);

    return openPorts;
}

bool containsAny(const QString &text, const QStringList &keywords)
{
    if (text.isEmpty())
        return false;
    for (const QString &keyword : keywords)
    {
        if (text.contains(keyword))
            return true;
    }
    return false;
}

/// 抓取 Web 管理页内容（小写），用于指纹匹配
QString fetchHttpFingerprint(const QString &ip, const QVector<int> &openPorts)
{
    for (int port : httpPorts())
    {
        if (!openPorts.contains(port))
            continue;

        const QByteArray request = "GET / HTTP/1.0\r\nHost: " + ip.toUtf8()
                                   + "\r\nUser-Agent: Mozilla/5.0\r\nConnection: close\r\n\r\n";
        const QByteArray response = NetUtils::tcpExchange(ip, port, request, kExchangeBudgetMs);
        if (response.isEmpty())
            continue;

        // TLS 握手记录（0x16 0x03）说明该端口是 HTTPS，无法在此解析，跳过
        if (static_cast<unsigned char>(response.at(0)) == 0x16)
            continue;

        return QString::fromLatin1(response).toLower();
    }
    return QString();
}

/// RTSP 应答文本（小写），非 RTSP 服务返回空
QString probeRtsp(const QString &ip, const QVector<int> &openPorts)
{
    for (int port : rtspPorts())
    {
        if (!openPorts.contains(port))
            continue;

        const QByteArray request = "OPTIONS rtsp://" + ip.toUtf8() + ":"
                                   + QByteArray::number(port)
                                   + " RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: NETScratch\r\n\r\n";
        const QByteArray response = NetUtils::tcpExchange(ip, port, request, kExchangeBudgetMs);
        if (response.startsWith("RTSP/"))
            return QString::fromLatin1(response).toLower();
    }
    return QString();
}

/// 已知的摄像头管理页路径：命中即说明这是台视频设备。
/// 仅在首页指纹没能给出强证据时才跑，且与指纹抓取共用一份时间预算。
QString probeKnownPaths(const QString &ip, const QVector<int> &openPorts)
{
    struct PathProbe
    {
        const char *path;
        const char *evidence;
    };
    static const QVector<PathProbe> probes{
        {"/onvif/device_service", "HTTP-ONVIF"},   // ONVIF 设备服务：几乎只出现在视频设备上
        {"/doc/page/login.asp", "HTTP-HIK-Login"}, // 海康 / 萤石的登录页
    };
    static const QStringList keywords{
        QStringLiteral("onvif"), QStringLiteral("networkvideotransmitter"),
        QStringLiteral("getdeviceinformation"), QStringLiteral("isapi"),
        QStringLiteral("dvrdvs"), QStringLiteral("hikvision")};

    QElapsedTimer timer;
    timer.start();

    for (int port : httpPorts())
    {
        if (!openPorts.contains(port))
            continue;

        for (const PathProbe &probe : probes)
        {
            const int remaining = kExchangeBudgetMs - static_cast<int>(timer.elapsed());
            if (remaining <= 0)
                return QString();

            const QByteArray request = QByteArray("GET ") + probe.path
                                       + " HTTP/1.0\r\nHost: " + ip.toUtf8()
                                       + "\r\nUser-Agent: NETScratch\r\nConnection: close\r\n\r\n";
            const QByteArray response = NetUtils::tcpExchange(ip, port, request, remaining);
            if (response.isEmpty())
                continue;
            if (static_cast<unsigned char>(response.at(0)) == 0x16)
                continue; // TLS，解析不了

            const QString text = QString::fromLatin1(response).toLower();
            if (containsAny(text, keywords))
                return QString::fromLatin1(probe.evidence);
        }

        break; // 只试第一个开放的 Web 端口
    }

    return QString();
}

/// 定位 nmap 自带的 MAC 前缀库（随 nmap 一起分发在程序目录的 nmap 子目录下）
QString findMacPrefixesFile()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/nmap/nmap-mac-prefixes"),
        QDir::cleanPath(appDir + QStringLiteral("/../nmap/nmap-mac-prefixes")),
        QDir::cleanPath(appDir + QStringLiteral("/../release/nmap/nmap-mac-prefixes"))};

    for (const QString &candidate : candidates)
    {
        const QFileInfo info(candidate);
        if (info.isFile())
            return info.absoluteFilePath();
    }
    return QString();
}

/// MAC 前缀（6 位十六进制）-> 厂商名。数据来自 nmap-mac-prefixes，进程内只加载一次；
/// 读不到文件时表为空，识别会退回下面那份硬编码的监控厂商前缀。
const QHash<QString, QString> &macPrefixTable()
{
    static const QHash<QString, QString> table = []() {
        QHash<QString, QString> result;

        const QString path = findMacPrefixesFile();
        if (path.isEmpty())
            return result;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return result;

        while (!file.atEnd())
        {
            const QString line = QString::fromLatin1(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            // 格式固定为「6 位十六进制前缀 + 空格 + 厂商名」
            const int separator = line.indexOf(QLatin1Char(' '));
            if (separator != 6)
                continue;

            result.insert(line.left(6).toUpper(), line.mid(separator + 1).trimmed());
        }

        return result;
    }();
    return table;
}

/// 按 MAC 查厂商名（如 "Hangzhou Ezviz Software"），查不到返回空
QString lookupVendorByMac(const QString &macAddress)
{
    if (macAddress.size() < 8)
        return QString();

    QString key = macAddress.left(8);
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char(':'));
    if (key.size() != 6)
        return QString();

    return macPrefixTable().value(key.toUpper());
}

/// 监控设备厂商：自有品牌或 P2P 方案商，这类网卡基本只出现在摄像头 / 录像机上
const QStringList &surveillanceVendorKeywords()
{
    static const QStringList keywords{
        QStringLiteral("hikvision"),  QStringLiteral("ezviz"),      QStringLiteral("dahua"),
        QStringLiteral("uniview"),    QStringLiteral("xiongmai"),   QStringLiteral("gwelltimes"),
        QStringLiteral("zmodo"),      QStringLiteral("foscam"),     QStringLiteral("reolink"),
        QStringLiteral("amcrest"),    QStringLiteral("wansview"),   QStringLiteral("vstarcam"),
        QStringLiteral("sricam"),     QStringLiteral("yoosee"),     QStringLiteral("tvt"),
        QStringLiteral("juan optical"), QStringLiteral("juan intelligent"),
        QStringLiteral("axis communications"), QStringLiteral("vivotek"),
        QStringLiteral("bosch security"), QStringLiteral("geovision")};
    return keywords;
}

/// 消费电子大厂：路由器、手机、智能家居与摄像头共用同一批 OUI，
/// 只能当弱证据（单独永远不够判定），必须与其它证据叠加。
/// 天津华来科技（Tianjin HuaLai）是米家摄像头的 ODM，但它同时也做门铃等
/// 非视频 IoT，所以同样只给弱证据。
const QStringList &consumerVendorKeywords()
{
    static const QStringList keywords{
        QStringLiteral("tp-link"), QStringLiteral("xiaomi"),   QStringLiteral("tuya"),
        QStringLiteral("espressif"), QStringLiteral("itead"),  QStringLiteral("sonoff"),
        QStringLiteral("broadlink"), QStringLiteral("huawei"), QStringLiteral("hualai")};
    return keywords;
}

/// 设备名 / 型号里出现这些词，说明它自报家门是台视频设备。
/// 中文词用于匹配用户自己起的名字（如 mDNS 实例名、SSDP friendlyName 里的「米家智能摄像机」）；
/// chuangmi 是创米（小米摄像头子品牌），其 miIO 型号形如 chuangmi.camera.ipc009。
const QStringList &cameraNameKeywords()
{
    static const QStringList keywords{
        QStringLiteral("camera"), QStringLiteral("ipcam"),  QStringLiteral("ipc"),
        QStringLiteral("webcam"), QStringLiteral("nvr"),    QStringLiteral("dvr"),
        QStringLiteral("onvif"),  QStringLiteral("chuangmi"),
        QStringLiteral("摄像头"), QStringLiteral("摄像机"), QStringLiteral("监控"),
        QStringLiteral("录像机")};
    return keywords;
}

/// SSDP 应答 SERVER 头里出现这些词，基本可以确认是台视频设备
const QStringList &ssdpServerKeywords()
{
    static const QStringList keywords{
        QStringLiteral("hikvision"), QStringLiteral("dahua"),    QStringLiteral("uniview"),
        QStringLiteral("goahead"),   QStringLiteral("app-webs"), QStringLiteral("netwave"),
        QStringLiteral("ipcam"),     QStringLiteral("camera"),   QStringLiteral("vstarcam"),
        QStringLiteral("sricam"),    QStringLiteral("bosch"),    QStringLiteral("axis")};
    return keywords;
}

/// MAC 前缀是否属于监控设备厂商（取前 3 段，形如 AA-BB-CC）
bool isSurveillanceVendorMac(const QString &macAddress)
{
    static const QSet<QString> vendorPrefixes{
        // Hikvision 海康威视
        QStringLiteral("44-19-B6"), QStringLiteral("4C-BD-8F"), QStringLiteral("BC-AD-28"),
        QStringLiteral("C0-56-E3"), QStringLiteral("28-57-BE"), QStringLiteral("54-C4-15"),
        QStringLiteral("8C-E7-48"), QStringLiteral("A4-14-37"), QStringLiteral("18-68-CB"),
        QStringLiteral("24-28-FD"), QStringLiteral("58-03-FB"), QStringLiteral("6C-F1-7E"),
        QStringLiteral("84-9D-C5"), QStringLiteral("C4-2F-90"),
        // Dahua 大华
        QStringLiteral("3C-EF-8C"), QStringLiteral("90-02-A9"), QStringLiteral("4C-11-BF"),
        QStringLiteral("E0-50-8B"), QStringLiteral("08-ED-ED"),
        // Uniview 宇视
        QStringLiteral("48-EA-63"),
        // Axis
        QStringLiteral("00-40-8C"), QStringLiteral("AC-CC-8E"), QStringLiteral("B8-A4-4F"),
        // Vivotek
        QStringLiteral("00-02-D1"),
        // Bosch Security
        QStringLiteral("00-07-5F"),
    };

    if (macAddress.size() < 8)
        return false;
    return vendorPrefixes.contains(macAddress.left(8).toUpper());
}
} // namespace

CameraDetection CameraDetector::detect(const QString &ip, const QString &macAddress,
                                       const QString &hostName, const DiscoveryHint *hint)
{
    CameraDetection result;
    if (ip.isEmpty())
        return result;

    const bool hasHint = hint && (!hint->ssdpModel.isEmpty() || !hint->ssdpManufacturer.isEmpty()
                                  || !hint->ssdpFriendlyName.isEmpty() || !hint->ssdpServer.isEmpty()
                                  || !hint->mdnsName.isEmpty() || !hint->mdnsTxt.isEmpty()
                                  || !hint->mdnsServices.isEmpty());

    const QVector<int> openPorts = scanPorts(ip, candidatePorts(), kPortScanBudgetMs);
    // 家用 P2P 摄像头常常一个端口都不开，此时只能靠组播线索与 MAC 厂商判定
    if (openPorts.isEmpty() && !hasHint && macAddress.isEmpty())
        return result;

    int score = 0;
    QStringList evidence;

    // 1) RTSP 服务：局域网内开放 554/8554/10554 基本可确定是视频设备
    bool rtspOpen = false;
    for (int port : rtspPorts())
        rtspOpen = rtspOpen || openPorts.contains(port);
    if (rtspOpen)
    {
        score += 5;
        evidence.append(QStringLiteral("RTSP"));

        static const QStringList rtspVendorKeywords{
            QStringLiteral("hikvision"), QStringLiteral("dahua"), QStringLiteral("uniview"),
            QStringLiteral("axis"),     QStringLiteral("goahead"), QStringLiteral("v380"),
            QStringLiteral("live555"),  QStringLiteral("rtsp server")};
        if (containsAny(probeRtsp(ip, openPorts), rtspVendorKeywords))
        {
            score += 2;
            evidence.append(QStringLiteral("RTSP-Vendor"));
        }
    }

    // 2) 厂商私有端口：大华 37777、通用 DVR 34567、海康 SDK 8000、TP-LINK IPC 2020
    if (openPorts.contains(37777))
    {
        score += 5;
        evidence.append(QStringLiteral("P37777"));
    }
    if (openPorts.contains(34567))
    {
        score += 4;
        evidence.append(QStringLiteral("P34567"));
    }
    if (openPorts.contains(8000))
    {
        score += 4;
        evidence.append(QStringLiteral("P8000"));
    }
    if (openPorts.contains(2020))
    {
        score += 3;
        evidence.append(QStringLiteral("P2020"));
    }

    // 3) Web 管理页指纹
    const QString http = fetchHttpFingerprint(ip, openPorts);
    bool httpStrongHit = false;
    if (!http.isEmpty())
    {
        static const QStringList strongKeywords{
            QStringLiteral("hikvision"),  QStringLiteral("dvrdvs"),
            QStringLiteral("netsurveillance"), QStringLiteral("surveillance"),
            QStringLiteral("ipcamera"),   QStringLiteral("ip camera"),
            QStringLiteral("network camera"), QStringLiteral("ipcam"),
            QStringLiteral("webcam"),     QStringLiteral("onvif"),
            QStringLiteral("rtsp://"),    QStringLiteral("video server"),
            QStringLiteral("yoosee"),     QStringLiteral("v380"),
            QStringLiteral("dahua"),      QStringLiteral("xiongmai"),
            QStringLiteral("xmeye")};
        static const QStringList mediumKeywords{
            QStringLiteral("goahead-webs"), QStringLiteral("app-webs"),
            QStringLiteral("camera"),       QStringLiteral("nvr"),
            QStringLiteral("dvr")};

        if (containsAny(http, strongKeywords))
        {
            score += 5;
            evidence.append(QStringLiteral("HTTP-Fingerprint"));
            httpStrongHit = true;
        }
        else if (containsAny(http, mediumKeywords))
        {
            score += 3;
            evidence.append(QStringLiteral("HTTP-Keyword"));
        }
    }

    // 3b) 已知摄像头管理页路径（ONVIF 设备服务 / 海康登录页）：
    //    首页指纹没给出强证据时才跑，避免白白占满预算
    if (!httpStrongHit && !openPorts.isEmpty())
    {
        const QString pathEvidence = probeKnownPaths(ip, openPorts);
        if (!pathEvidence.isEmpty())
        {
            score += 4;
            evidence.append(pathEvidence);
        }
    }

    // 4) MAC 厂商：优先查 nmap 前缀库拿厂商全名，分「监控专属 / 消费电子」两档
    const QString vendor = lookupVendorByMac(macAddress);
    if (!vendor.isEmpty() && containsAny(vendor.toLower(), surveillanceVendorKeywords()))
    {
        score += 5;
        evidence.append(QStringLiteral("OUI-Vendor"));
    }
    else if (!vendor.isEmpty() && containsAny(vendor.toLower(), consumerVendorKeywords()))
    {
        // 路由器 / 手机 / 智能家居与摄像头共用 OUI，只能当弱证据
        score += 2;
        evidence.append(QStringLiteral("OUI-Consumer"));
    }
    else if (isSurveillanceVendorMac(macAddress))
    {
        // 查不到厂商名时退回硬编码的监控厂商前缀表
        score += 3;
        evidence.append(QStringLiteral("MAC-Vendor"));
    }

    // 5) 主机名关键字
    static const QStringList hostKeywords{
        QStringLiteral("ipcam"), QStringLiteral("ip-cam"), QStringLiteral("ipc"),
        QStringLiteral("camera"), QStringLiteral("webcam"), QStringLiteral("dvr"),
        QStringLiteral("nvr"),    QStringLiteral("hikvision"), QStringLiteral("dahua")};
    if (containsAny(hostName.toLower(), hostKeywords))
    {
        score += 3;
        evidence.append(QStringLiteral("Hostname"));
    }

    // 6) 组播发现线索（SSDP / mDNS）——P2P 摄像头的最后一道判据
    if (hasHint)
    {
        // 6a) mDNS 服务类型：_rtsp._tcp 是视频设备的直接自述
        for (const QString &service : hint->mdnsServices)
        {
            const QString lower = service.toLower();
            if (lower.contains(QStringLiteral("_rtsp._tcp"))
                || lower.contains(QStringLiteral("_onvif._tcp")))
            {
                score += 5;
                evidence.append(QStringLiteral("mDNS-RTSP"));
                break;
            }
        }
        // _hap._tcp 是 HomeKit，摄像头与灯泡都有，只能当弱证据
        for (const QString &service : hint->mdnsServices)
        {
            if (service.toLower().contains(QStringLiteral("_hap._tcp")))
            {
                score += 2;
                evidence.append(QStringLiteral("mDNS-HAP"));
                break;
            }
        }

        // 6b) mDNS 实例名 / TXT / SSDP friendlyName 里自报「camera / ipc / nvr」之类
        const QString selfName = (hint->mdnsName + QLatin1Char(' ') + hint->mdnsTxt
                                  + QLatin1Char(' ') + hint->ssdpFriendlyName)
                                     .toLower();
        if (containsAny(selfName, cameraNameKeywords()))
        {
            score += 3;
            evidence.append(QStringLiteral("mDNS-Name"));
        }

        // 6c) UPnP 设备描述里的型号 / 厂商：几乎只在视频设备上写明
        const QString ssdpInfo = (hint->ssdpModel + QLatin1Char(' ') + hint->ssdpManufacturer)
                                     .toLower();
        if (containsAny(ssdpInfo, cameraNameKeywords())
            || containsAny(ssdpInfo, surveillanceVendorKeywords()))
        {
            score += 5;
            evidence.append(QStringLiteral("SSDP-Model"));
        }

        // 6d) SSDP SERVER 头里的嵌入式 Web 服务器名（GoAhead / App-Webs / NETSurveillance）
        if (containsAny(hint->ssdpServer.toLower(), ssdpServerKeywords()))
        {
            score += 4;
            evidence.append(QStringLiteral("SSDP-Server"));
        }
    }

    result.isCamera = score >= kScoreThreshold;
    if (result.isCamera)
        result.evidence = evidence.join(QLatin1Char(';'));
    return result;
}
