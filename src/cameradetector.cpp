// cameradetector.cpp —— 网络摄像头（IPC / DVR / NVR 等视频监控设备）识别实现
//
// 判定思路：局域网内的摄像头通常会暴露 RTSP 服务、厂商私有端口或带明显指纹的
// Web 管理页，因此按证据加权打分，累计分数达到阈值才判定，尽量降低误报。
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
#include <QElapsedTimer>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "cameradetector.h"

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
const int kPortScanBudgetMs = 600;
const int kExchangeBudgetMs = 400;

/// 参与探测的端口：RTSP / HTTP / 厂商私有端口
const QVector<int> &candidatePorts()
{
    static const QVector<int> ports{554, 8554, 80, 8000, 8080, 88, 34567, 37777};
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

/// 与指定端口做一次短文本交互（发请求、读到无数据或超时为止），失败返回空
QByteArray tcpExchange(const QString &ip, int port, const QByteArray &request, int budgetMs)
{
    QByteArray response;
    sockaddr_in target;
    if (!buildSockAddr(ip, &target))
        return response;

    ensureWinsock();

    const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        return response;

    u_long nonBlocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0)
    {
        ::closesocket(sock);
        return response;
    }

    target.sin_port = htons(static_cast<u_short>(port));

    bool connected = ::connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0;
    if (!connected)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            ::closesocket(sock);
            return response;
        }

        WSAPOLLFD pollFd;
        pollFd.fd = sock;
        pollFd.events = POLLWRNORM;
        pollFd.revents = 0;
        if (WSAPoll(&pollFd, 1, budgetMs) <= 0)
        {
            ::closesocket(sock);
            return response;
        }

        int socketError = 0;
        int socketErrorSize = static_cast<int>(sizeof(socketError));
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socketError),
                       &socketErrorSize)
                != 0
            || socketError != 0)
        {
            ::closesocket(sock);
            return response;
        }
    }

    if (::send(sock, request.constData(), static_cast<int>(request.size()), 0) <= 0)
    {
        ::closesocket(sock);
        return response;
    }

    QElapsedTimer timer;
    timer.start();
    char buffer[2048];
    while (response.size() < 8192)
    {
        const int remaining = budgetMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0)
            break;

        WSAPOLLFD pollFd;
        pollFd.fd = sock;
        pollFd.events = POLLRDNORM;
        pollFd.revents = 0;
        if (WSAPoll(&pollFd, 1, remaining) <= 0)
            break;

        const int received = ::recv(sock, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0)
            break;
        response.append(buffer, received);
    }

    ::closesocket(sock);
    return response;
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
    static const QVector<int> httpPorts{80, 8000, 8080, 88};
    for (int port : httpPorts)
    {
        if (!openPorts.contains(port))
            continue;

        const QByteArray request = "GET / HTTP/1.0\r\nHost: " + ip.toUtf8()
                                   + "\r\nUser-Agent: Mozilla/5.0\r\nConnection: close\r\n\r\n";
        const QByteArray response = tcpExchange(ip, port, request, kExchangeBudgetMs);
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
    static const QVector<int> rtspPorts{554, 8554};
    for (int port : rtspPorts)
    {
        if (!openPorts.contains(port))
            continue;

        const QByteArray request = "OPTIONS rtsp://" + ip.toUtf8() + ":"
                                   + QByteArray::number(port)
                                   + " RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: NETScratch\r\n\r\n";
        const QByteArray response = tcpExchange(ip, port, request, kExchangeBudgetMs);
        if (response.startsWith("RTSP/"))
            return QString::fromLatin1(response).toLower();
    }
    return QString();
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
                                       const QString &hostName)
{
    CameraDetection result;
    if (ip.isEmpty())
        return result;

    const QVector<int> openPorts = scanPorts(ip, candidatePorts(), kPortScanBudgetMs);
    if (openPorts.isEmpty())
        return result;

    int score = 0;
    QStringList evidence;

    // 1) RTSP 服务：局域网内开放 554/8554 基本可确定是视频设备
    if (openPorts.contains(554) || openPorts.contains(8554))
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

    // 2) 厂商私有端口：大华 37777、通用 DVR 34567、海康 SDK 8000
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

    // 3) Web 管理页指纹
    const QString http = fetchHttpFingerprint(ip, openPorts);
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
        }
        else if (containsAny(http, mediumKeywords))
        {
            score += 3;
            evidence.append(QStringLiteral("HTTP-Keyword"));
        }
    }

    // 4) MAC 厂商前缀
    if (isSurveillanceVendorMac(macAddress))
    {
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

    result.isCamera = score >= kScoreThreshold;
    if (result.isCamera)
        result.evidence = evidence.join(QLatin1Char(';'));
    return result;
}
