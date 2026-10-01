// netdiscovery.cpp —— 局域网组播/广播发现（SSDP / UPnP + mDNS）
//
// 为什么需要它：家用 P2P 摄像头为了穿透 NAT，只主动出站连云端，局域网里往往
// 一个端口都不开，逐 IP 的端口扫描完全看不到。但这类设备开机后通常会在
// mDNS（_rtsp._tcp 等）或 SSDP 里播报自己，因此这里做一次「全网段一次性」的
// 组播查询与监听，把 IP -> 线索 的映射交给逐 IP 的识别逻辑去打分。
//
// 说明：只监听组播/广播（SSDP NOTIFY、mDNS announcement），用普通 UDP socket
//       即可收到，不需要 Npcap。别的设备发往外网的单播流量在交换式网络里本来
//       就收不到，那需要端口镜像或 ARP 欺骗，不在本工具职责内。
//
// 网络调用沿用 netutils.cpp 的风格：裸 Winsock + 非阻塞 socket + WSAPoll + 总预算。

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
#include <ws2tcpip.h>
#include <windows.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "netdiscovery.h"

#include "netutils.h"
#include "scanner.h" // ScanCancelToken（netdiscovery.h 里只有前置声明）

#ifndef POLLWRNORM
#define POLLWRNORM 0x0010
#endif
#ifndef POLLRDNORM
#define POLLRDNORM 0x0100
#endif

namespace
{
const char *const kSsdpGroup = "239.255.255.250";
const int kSsdpPort = 1900;
const char *const kMdnsGroup = "224.0.0.251";
const int kMdnsPort = 5353;

/// 单个 UPnP 设备描述的抓取预算
const int kUpnpFetchBudgetMs = 400;
/// 单轮最多抓几个设备描述，防止极端网络下把预算耗尽
const int kMaxUpnpFetch = 8;

/// DNS 资源记录类型
const quint16 kTypeA = 1;
const quint16 kTypePtr = 12;
const quint16 kTypeTxt = 16;
const quint16 kTypeSrv = 33;

/// 惰性初始化 Winsock（整个进程只执行一次）
void ensureWinsock()
{
    static const bool initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Q_UNUSED(initialized);
}

/// IPv4 字符串 -> in_addr，失败返回 false
bool resolveIpv4(const QString &ip, in_addr *out)
{
    if (ip.isEmpty())
        return false;

    const QByteArray raw = ip.trimmed().toLatin1();
    const unsigned long value = ::inet_addr(raw.constData());
    if (value == INADDR_NONE)
        return false;

    out->s_addr = value;
    return true;
}

QString ipv4ToString(const in_addr &address)
{
    const quint32 value = ntohl(address.s_addr);
    return QStringLiteral("%1.%2.%3.%4")
        .arg((value >> 24) & 0xFFu)
        .arg((value >> 16) & 0xFFu)
        .arg((value >> 8) & 0xFFu)
        .arg(value & 0xFFu);
}

/// 建一个用于组播收发的 UDP socket。
/// 尽量绑到 preferredPort（这样还能收到别的设备主动播报的包），绑不上就退到
/// 系统分配的临时端口——此时主动查询的单播应答依然能收到，只是收不到播报。
SOCKET createMulticastSocket(int preferredPort, const QString &localIpv4)
{
    const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return INVALID_SOCKET;

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));

    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(static_cast<u_short>(preferredPort));
    if (::bind(sock, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
    {
        local.sin_port = 0;
        if (::bind(sock, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
        {
            ::closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    // 指定出口网卡，避免多网卡机器把查询发到别的网段
    in_addr localAddress;
    if (resolveIpv4(localIpv4, &localAddress))
    {
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char *>(&localAddress), sizeof(localAddress));
    }

    return sock;
}

void joinMulticastGroup(SOCKET sock, const char *group, const QString &localIpv4)
{
    ip_mreq request;
    memset(&request, 0, sizeof(request));
    request.imr_multiaddr.s_addr = ::inet_addr(group);
    request.imr_interface.s_addr = INADDR_ANY;
    resolveIpv4(localIpv4, &request.imr_interface);

    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&request),
               sizeof(request));
}

void sendMulticast(SOCKET sock, const char *group, int port, const QByteArray &payload)
{
    sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<u_short>(port));
    target.sin_addr.s_addr = ::inet_addr(group);

    ::sendto(sock, payload.constData(), static_cast<int>(payload.size()), 0,
             reinterpret_cast<const sockaddr *>(&target), sizeof(target));
}

/// "a.b.local" -> DNS 报文里的标签序列；非法返回空
QByteArray encodeDnsName(const QString &name)
{
    QByteArray out;
    const QStringList labels = name.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.isEmpty())
        return out;

    for (const QString &label : labels)
    {
        const QByteArray raw = label.toUtf8();
        if (raw.isEmpty() || raw.size() > 63)
            return QByteArray();
        out.append(static_cast<char>(raw.size()));
        out.append(raw);
    }
    out.append('\0');
    return out;
}

/// 组装一个 mDNS PTR 查询报文。QCLASS 带 QU 位，请求单播应答，
/// 免得和系统自带的 mDNS 服务抢 5353 端口上的应答。
QByteArray buildMdnsQuery(const QString &service, quint16 transactionId)
{
    const QByteArray qname = encodeDnsName(service);
    if (qname.isEmpty())
        return QByteArray();

    QByteArray packet;
    const auto append16 = [&packet](quint16 value) {
        packet.append(static_cast<char>((value >> 8) & 0xFF));
        packet.append(static_cast<char>(value & 0xFF));
    };

    append16(transactionId); // 事务 ID
    append16(0x0000);        // 标准查询
    append16(1);             // QDCOUNT
    append16(0);             // ANCOUNT
    append16(0);             // NSCOUNT
    append16(0);             // ARCOUNT
    packet.append(qname);
    append16(kTypePtr);
    append16(0x8001); // IN + QU（请求单播应答）

    return packet;
}

/// 读取 offset 处的一个 DNS 名字（支持 0xC0 压缩指针），并把 offset 推进到
/// 名字首次出现位置的末尾（正常情况即原始 offset 之后）。
QString readDnsName(const QByteArray &data, int *offset)
{
    QStringList labels;
    int cursor = *offset;
    int afterFirst = -1;
    bool jumped = false;
    int guard = 0;

    while (cursor >= 0 && cursor < data.size() && guard++ < 128)
    {
        const int length = static_cast<quint8>(data.at(cursor));
        if (length == 0)
        {
            if (!jumped)
                afterFirst = cursor + 1;
            break;
        }

        if ((length & 0xC0) == 0xC0)
        {
            if (cursor + 1 >= data.size())
                break;
            if (!jumped)
            {
                afterFirst = cursor + 2;
                jumped = true;
            }
            const int pointer =
                ((length & 0x3F) << 8) | static_cast<quint8>(data.at(cursor + 1));
            if (pointer <= 0 || pointer >= data.size())
                break;
            cursor = pointer;
            continue;
        }

        if (cursor + 1 + length > data.size())
            break;

        labels.append(QString::fromUtf8(data.mid(cursor + 1, length)));
        cursor += 1 + length;
        if (!jumped)
            afterFirst = cursor;
    }

    *offset = afterFirst > 0 ? afterFirst : data.size();
    return labels.join(QLatin1Char('.'));
}

/// 从 "_rtsp._tcp.local" 或 "IPC-1234._rtsp._tcp.local" 这类名字里
/// 抽出服务类型 "_rtsp._tcp"；不像服务名则返回空
QString serviceTypeOf(const QString &name)
{
    const QStringList labels = name.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (int i = 0; i + 1 < labels.size(); ++i)
    {
        // 服务类型由两个下划线开头的标签组成，如 _rtsp._tcp
        if (labels.at(i).startsWith(QLatin1Char('_'))
            && labels.at(i + 1).startsWith(QLatin1Char('_')))
        {
            return labels.at(i) + QLatin1Char('.') + labels.at(i + 1);
        }
    }
    return QString();
}

/// 一条 mDNS 报文解析出来的原始素材
struct MdnsPacketInfo
{
    QSet<QString> instanceNames; ///< PTR / SRV / TXT 提到的实例名与主机名
    QSet<QString> services;      ///< 报文里出现的服务类型
    QVector<QString> addresses;  ///< A 记录里的 IPv4
    QString txt;                 ///< TXT 记录文本（拼接）
};

/// 解析一条 mDNS 报文，把素材塞进 info
void parseMdnsPacket(const QByteArray &data, MdnsPacketInfo *info)
{
    if (data.size() < 12)
        return;

    const auto read16 = [&data](int offset) -> quint16 {
        return static_cast<quint16>((static_cast<quint8>(data.at(offset)) << 8)
                                    | static_cast<quint8>(data.at(offset + 1)));
    };

    const int questionCount = read16(4);
    const int answerCount = read16(6);
    const int authorityCount = read16(8);
    const int additionalCount = read16(10);

    int cursor = 12;

    // 跳过问题段
    for (int i = 0; i < questionCount; ++i)
    {
        readDnsName(data, &cursor);
        cursor += 4; // QTYPE + QCLASS
        if (cursor > data.size())
            return;
    }

    const int recordCount = answerCount + authorityCount + additionalCount;
    for (int i = 0; i < recordCount; ++i)
    {
        if (cursor + 10 > data.size())
            return;

        const QString owner = readDnsName(data, &cursor);
        if (cursor + 10 > data.size())
            return;

        const quint16 type = read16(cursor);
        const int dataLength = read16(cursor + 8);
        cursor += 10;
        if (cursor + dataLength > data.size())
            return;

        const int dataStart = cursor;
        switch (type)
        {
        case kTypePtr:
        case kTypeSrv:
        {
            int inner = cursor;
            if (type == kTypeSrv)
                inner += 6; // priority + weight + port
            const QString target = readDnsName(data, &inner);
            if (!target.isEmpty())
            {
                info->instanceNames.insert(target);
                const QString service = serviceTypeOf(target);
                if (!service.isEmpty())
                    info->services.insert(service);
            }
            const QString ownerService = serviceTypeOf(owner);
            if (!ownerService.isEmpty())
                info->services.insert(ownerService);
            break;
        }
        case kTypeA:
            if (dataLength == 4)
            {
                const quint32 value = (static_cast<quint32>(static_cast<quint8>(data.at(cursor)))
                                       << 24)
                                      | (static_cast<quint32>(static_cast<quint8>(data.at(cursor + 1)))
                                         << 16)
                                      | (static_cast<quint32>(static_cast<quint8>(data.at(cursor + 2)))
                                         << 8)
                                      | static_cast<quint8>(data.at(cursor + 3));
                info->addresses.append(QStringLiteral("%1.%2.%3.%4")
                                           .arg((value >> 24) & 0xFFu)
                                           .arg((value >> 16) & 0xFFu)
                                           .arg((value >> 8) & 0xFFu)
                                           .arg(value & 0xFFu));
            }
            break;
        case kTypeTxt:
        {
            int inner = cursor;
            while (inner < dataStart + dataLength)
            {
                const int length = static_cast<quint8>(data.at(inner));
                ++inner;
                if (length <= 0 || inner + length > dataStart + dataLength)
                    break;
                info->txt += QString::fromUtf8(data.mid(inner, length));
                info->txt += QLatin1Char(' ');
                inner += length;
            }
            break;
        }
        default:
            break;
        }

        cursor = dataStart + dataLength;
    }
}

/// 把一条 mDNS 报文解析出的素材归到对应 IP 上。
/// 同一台设备的全部记录通常在同一条报文里，因此报文内的 A 记录 IP 就是线索的归属。
void applyMdnsPacket(const QByteArray &data, const QString &senderIp,
                     QHash<QString, DiscoveryHint> *hints)
{
    MdnsPacketInfo info;
    parseMdnsPacket(data, &info);

    if (info.instanceNames.isEmpty() && info.services.isEmpty() && info.txt.isEmpty())
        return;

    // 优先用 A 记录里的地址；没有 A 记录（如对 _services._dns-sd 的应答）就退回发送方地址
    QVector<QString> targets = info.addresses;
    if (targets.isEmpty())
        targets.append(senderIp);

    for (const QString &ip : targets)
    {
        if (ip.isEmpty())
            continue;

        DiscoveryHint &hint = (*hints)[ip];
        if (hint.mdnsName.isEmpty() && !info.instanceNames.isEmpty())
            hint.mdnsName = *info.instanceNames.constBegin();
        for (const QString &service : info.services)
        {
            if (!hint.mdnsServices.contains(service))
                hint.mdnsServices.append(service);
        }
        if (hint.mdnsTxt.isEmpty())
            hint.mdnsTxt = info.txt.trimmed();
    }
}

/// 从 SSDP 报文里取某个头部的值（头部名大小写不敏感），没有返回空
QString ssdpHeader(const QString &text, const QString &header)
{
    const QString prefix = header.toLower() + QLatin1Char(':');
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines)
    {
        const QString trimmed = line.trimmed();
        if (trimmed.toLower().startsWith(prefix))
            return trimmed.mid(prefix.size()).trimmed();
    }
    return QString();
}

/// 解析 SSDP 应答 / NOTIFY：记下 SERVER 头与 LOCATION，供后续抓设备描述
void parseSsdpPacket(const QByteArray &data, const QString &senderIp,
                     QHash<QString, DiscoveryHint> *hints,
                     QHash<QString, QString> *locations)
{
    const QString text = QString::fromLatin1(data);
    if (!text.contains(QStringLiteral("HTTP/1.1"), Qt::CaseInsensitive)
        && !text.contains(QStringLiteral("NOTIFY"), Qt::CaseInsensitive))
        return;

    DiscoveryHint &hint = (*hints)[senderIp];

    const QString server = ssdpHeader(text, QStringLiteral("SERVER"));
    if (!server.isEmpty() && hint.ssdpServer.isEmpty())
        hint.ssdpServer = server;

    const QString location = ssdpHeader(text, QStringLiteral("LOCATION"));
    if (!location.isEmpty() && !locations->contains(senderIp))
        locations->insert(senderIp, location);
}

/// 取 SSDP 的 LOCATION 指向的 UPnP 设备描述，抠出型号 / 厂商 / 友好名
void fetchUpnpDescription(const QString &location, DiscoveryHint *hint, int budgetMs)
{
    // http://192.168.1.50:8080/description.xml
    static const QRegularExpression urlPattern(
        QStringLiteral("^http://([^/:]+)(?::(\\d+))?(/.*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = urlPattern.match(location.trimmed());
    if (!match.hasMatch())
        return;

    const QString host = match.captured(1);
    const int port = match.captured(2).isEmpty() ? 80 : match.captured(2).toInt();
    const QString path = match.captured(3).isEmpty() ? QStringLiteral("/") : match.captured(3);

    const QByteArray request = "GET " + path.toUtf8() + " HTTP/1.0\r\nHost: "
                               + host.toUtf8() + "\r\nUser-Agent: NETScratch\r\nConnection: close\r\n\r\n";
    const QByteArray response = NetUtils::tcpExchange(host, port, request, budgetMs);
    if (response.isEmpty())
        return;

    const QString body = QString::fromLatin1(response);

    static const QRegularExpression modelPattern(
        QStringLiteral("<modelName>(.*?)</modelName>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression manufacturerPattern(
        QStringLiteral("<manufacturer>(.*?)</manufacturer>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression friendlyPattern(
        QStringLiteral("<friendlyName>(.*?)</friendlyName>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    const QString model = modelPattern.match(body).captured(1).trimmed();
    if (!model.isEmpty() && hint->ssdpModel.isEmpty())
        hint->ssdpModel = model;

    const QString manufacturer = manufacturerPattern.match(body).captured(1).trimmed();
    if (!manufacturer.isEmpty() && hint->ssdpManufacturer.isEmpty())
        hint->ssdpManufacturer = manufacturer;

    const QString friendly = friendlyPattern.match(body).captured(1).trimmed();
    if (!friendly.isEmpty() && hint->ssdpFriendlyName.isEmpty())
        hint->ssdpFriendlyName = friendly;
}
} // namespace

QHash<QString, DiscoveryHint> NetDiscovery::discover(const QString &localIpv4, int budgetMs,
                                                     const ScanCancelToken *token)
{
    QHash<QString, DiscoveryHint> hints;
    if (budgetMs <= 0)
        return hints;

    ensureWinsock();

    const SOCKET ssdpSock = createMulticastSocket(kSsdpPort, localIpv4);
    const SOCKET mdnsSock = createMulticastSocket(kMdnsPort, localIpv4);
    if (ssdpSock == INVALID_SOCKET && mdnsSock == INVALID_SOCKET)
        return hints;

    if (ssdpSock != INVALID_SOCKET)
        joinMulticastGroup(ssdpSock, kSsdpGroup, localIpv4);
    if (mdnsSock != INVALID_SOCKET)
        joinMulticastGroup(mdnsSock, kMdnsGroup, localIpv4);

    // 主动查询：SSDP 问三类目标，mDNS 问几类与摄像头相关的服务
    if (ssdpSock != INVALID_SOCKET)
    {
        const QVector<QByteArray> searchTargets{
            QByteArrayLiteral("ssdp:all"), QByteArrayLiteral("upnp:rootdevice"),
            QByteArrayLiteral("urn:schemas-upnp-org:device:Basic:1")};
        for (const QByteArray &searchTarget : searchTargets)
        {
            const QByteArray request = QByteArrayLiteral("M-SEARCH * HTTP/1.1\r\n")
                                       + QByteArrayLiteral("HOST: 239.255.255.250:1900\r\n")
                                       + QByteArrayLiteral("MAN: \"ssdp:discover\"\r\n")
                                       + QByteArrayLiteral("MX: 2\r\nST: ") + searchTarget
                                       + QByteArrayLiteral("\r\n\r\n");
            sendMulticast(ssdpSock, kSsdpGroup, kSsdpPort, request);
        }
    }

    if (mdnsSock != INVALID_SOCKET)
    {
        const QVector<QString> services{
            QStringLiteral("_services._dns-sd._udp.local"), QStringLiteral("_rtsp._tcp.local"),
            QStringLiteral("_http._tcp.local"), QStringLiteral("_hap._tcp.local"),
            QStringLiteral("_onvif._tcp.local"),
            // 小米 / 米家生态设备（含摄像头）会用这个服务名自报型号，如 chuangmi.camera.ipc009
            QStringLiteral("_miio._udp.local")};
        quint16 transactionId = 0x4E53; // "NS"
        for (const QString &service : services)
        {
            const QByteArray query = buildMdnsQuery(service, transactionId++);
            if (!query.isEmpty())
                sendMulticast(mdnsSock, kMdnsGroup, kMdnsPort, query);
        }
    }

    // 收包阶段占总预算的 3/5，剩下的留给抓 UPnP 设备描述
    const int listenBudgetMs = budgetMs * 3 / 5;

    QHash<QString, QString> locations; // IP -> LOCATION
    WSAPOLLFD pollFds[2];
    int socketCount = 0;
    if (ssdpSock != INVALID_SOCKET)
    {
        pollFds[socketCount].fd = ssdpSock;
        pollFds[socketCount].events = POLLRDNORM;
        pollFds[socketCount].revents = 0;
        ++socketCount;
    }
    if (mdnsSock != INVALID_SOCKET)
    {
        pollFds[socketCount].fd = mdnsSock;
        pollFds[socketCount].events = POLLRDNORM;
        pollFds[socketCount].revents = 0;
        ++socketCount;
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < listenBudgetMs)
    {
        if (token && token->isCancelled())
            break;

        const int remaining = listenBudgetMs - static_cast<int>(timer.elapsed());
        const int ready = WSAPoll(pollFds, socketCount, qMin(remaining, 200));
        if (ready <= 0)
            continue;

        for (int i = 0; i < socketCount; ++i)
        {
            if (pollFds[i].revents == 0)
                continue;

            for (;;)
            {
                char buffer[4096];
                sockaddr_in from;
                memset(&from, 0, sizeof(from));
                int fromLength = static_cast<int>(sizeof(from));

                const int received =
                    ::recvfrom(pollFds[i].fd, buffer, static_cast<int>(sizeof(buffer)), 0,
                               reinterpret_cast<sockaddr *>(&from), &fromLength);
                if (received <= 0)
                    break;

                const QString senderIp = ipv4ToString(from.sin_addr);
                const QByteArray packet(buffer, received);
                if (senderIp.isEmpty())
                    continue;

                if (pollFds[i].fd == ssdpSock)
                    parseSsdpPacket(packet, senderIp, &hints, &locations);
                else
                    applyMdnsPacket(packet, senderIp, &hints);
            }
        }
    }

    // 抓取 UPnP 设备描述：这是拿到「型号 / 厂商」这类强指纹的唯一途径
    int fetched = 0;
    for (auto it = locations.constBegin(); it != locations.constEnd(); ++it)
    {
        if (fetched >= kMaxUpnpFetch)
            break;
        if (token && token->isCancelled())
            break;

        const int remaining = budgetMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0)
            break;

        fetchUpnpDescription(it.value(), &hints[it.key()],
                             qMin(kUpnpFetchBudgetMs, remaining));
        ++fetched;
    }

    if (ssdpSock != INVALID_SOCKET)
        ::closesocket(ssdpSock);
    if (mdnsSock != INVALID_SOCKET)
        ::closesocket(mdnsSock);

    return hints;
}