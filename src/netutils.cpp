// netutils.cpp —— 局域网扫描工具的底层网络工具实现（ICMP / TCP / ARP / 本机信息 / 反向 DNS）
//
// 平台：Windows + MinGW 8.1 + Qt 5.15
// 注意：Win32 头包含顺序必须是 winsock2.h -> windows.h -> iphlpapi.h，
//       否则 GetAdaptersAddresses 不会被声明（iphlpapi.h 里有 _WINSOCK2API_ 保护）。

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7+，保证 IP_ADAPTER_ADDRESSES 使用带网关字段的 LH 版结构体
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QStringList>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include "netutils.h"

// 老版本头文件可能没有 POLLWRNORM 定义
#ifndef POLLWRNORM
#define POLLWRNORM 0x0010
#endif

namespace
{

/// 惰性初始化 Winsock（整个进程只执行一次，静态局部变量初始化线程安全）
void ensureWinsock()
{
    static const bool initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Q_UNUSED(initialized);
}

/// 严格校验 IPv4 字符串（4 段十进制）并取主机字节序数值
bool parseIpv4(const QString &text, quint32 *value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})$"));

    const QString trimmed = text.trimmed();
    if (!pattern.match(trimmed).hasMatch())
        return false;

    const QHostAddress address(trimmed);
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;

    if (value)
        *value = static_cast<quint32>(address.toIPv4Address());
    return true;
}

/// 把 IPv4 字符串转成网络字节序数值（IcmpSendEcho / sockaddr_in 需要）
bool ipv4ToNetworkOrder(const QString &ip, quint32 *networkOrder)
{
    quint32 hostOrder = 0;
    if (!parseIpv4(ip, &hostOrder))
        return false;
    if (networkOrder)
        *networkOrder = htonl(hostOrder);
    return true;
}

/// 填充 IPv4 的 sockaddr_in
bool buildSockAddr(const QString &ip, sockaddr_in *addr)
{
    quint32 networkOrder = 0;
    if (!ipv4ToNetworkOrder(ip, &networkOrder))
        return false;

    ZeroMemory(addr, sizeof(sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = networkOrder;
    return true;
}

/// 把网卡物理地址格式化为 AA-BB-CC-DD-EE-FF（大写），非法长度返回空
QString formatMacAddress(const QString &raw)
{
    QString hex = raw;
    hex.remove(QLatin1Char(':'));
    hex.remove(QLatin1Char('-'));
    hex.remove(QLatin1Char('.'));
    hex = hex.toUpper();
    if (hex.size() != 12)
        return QString();

    QStringList pairs;
    pairs.reserve(6);
    for (int i = 0; i < 12; i += 2)
        pairs.append(hex.mid(i, 2));
    return pairs.join(QLatin1Char('-'));
}

/// 执行外部命令并返回标准输出（按系统本地编码解码）；超时或失败返回空
QString runSystemCommand(const QString &program, const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(500))
        return QString();

    const int budget = qMax(1, timeoutMs);
    QByteArray output;
    QElapsedTimer timer;
    timer.start();

    while (process.state() != QProcess::NotRunning && timer.elapsed() < budget)
    {
        const int remaining = static_cast<int>(budget - timer.elapsed());
        if (!process.waitForReadyRead(qMax(1, remaining)))
            break; // 超时或出错
        output += process.readAllStandardOutput();
    }

    if (process.state() != QProcess::NotRunning)
    {
        // 与 C# 的 WaitForExit(1500) 返回 false 一致：超时视为失败
        process.kill();
        process.waitForFinished(200);
        return QString();
    }

    output += process.readAll();
    return QString::fromLocal8Bit(output);
}

/// 执行 arp 命令并返回标准输出；超时或失败返回空
QString runArpCommand(const QStringList &arguments, int timeoutMs)
{
    return runSystemCommand(QStringLiteral("arp"), arguments, timeoutMs);
}

/// arp -a / arp -a <ip> 输出的行分隔（含 "Interface: xxx --- 0x5" 之类的头部行，后续解析会过滤）
QStringList splitArpOutput(const QString &output)
{
    return output.split(QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);
}

/// arp 输出的列分隔（空格 / Tab）
QStringList splitArpFields(const QString &line)
{
    return line.split(QRegularExpression(QStringLiteral("[ \\t]+")), Qt::SkipEmptyParts);
}

/// 专用 DNS 解析线程。
/// QHostInfo 依赖事件循环，而扫描工作线程里没有事件循环，因此在独立线程里跑查找，
/// 调用方通过条件变量阻塞等待结果。每次调用持有独立的 CallState，
/// 因此可以有任意多个线程并发解析（超时后调用方放弃，回调仍会安全地填充自己的状态对象）。
class DnsResolver
{
public:
    static DnsResolver &instance()
    {
        // 堆上分配且永不析构，避免进程退出时静态对象析构顺序带来的线程清理问题
        static DnsResolver *resolver = new DnsResolver;
        return *resolver;
    }

    QString resolve(const QString &ip, int timeoutMs)
    {
        QObject *context = ensureThread();

        QSharedPointer<CallState> state(new CallState);

        QMetaObject::invokeMethod(
            context,
            [context, ip, state]() {
                QHostInfo::lookupHost(ip, context, [ip, state](const QHostInfo &info) {
                    QString result;
                    bool ok = false;
                    if (info.error() == QHostInfo::NoError)
                    {
                        const QString hostName = info.hostName();
                        // Qt 会把 IP 字面量反向解析为它自身；.NET 无 PTR 记录时会失败，
                        // 这里保持与 .NET 一致，视作解析失败。
                        if (!hostName.isEmpty() && hostName.compare(ip, Qt::CaseInsensitive) != 0)
                        {
                            result = hostName;
                            ok = true;
                        }
                    }

                    QMutexLocker resultLocker(&state->mutex);
                    state->result = result;
                    state->ok = ok;
                    state->done = true;
                    state->cond.wakeAll();
                });
            },
            Qt::QueuedConnection);

        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&state->mutex);
        while (!state->done)
        {
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            if (remaining <= 0)
                break;
            state->cond.wait(&state->mutex, static_cast<unsigned long>(remaining));
        }

        return state->ok ? state->result : QString();
    }

private:
    struct CallState
    {
        QMutex mutex;
        QWaitCondition cond;
        bool done = false;
        bool ok = false;
        QString result;
    };

    DnsResolver() = default;
    ~DnsResolver() = default;
    DnsResolver(const DnsResolver &) = delete;
    DnsResolver &operator=(const DnsResolver &) = delete;

    QObject *ensureThread()
    {
        QMutexLocker locker(&m_mutex);
        if (!m_thread)
        {
            m_thread = new QThread;
            m_thread->setObjectName(QStringLiteral("DnsResolver"));
            m_context = new QObject; // 无父对象，随后移入解析线程
            m_context->moveToThread(m_thread);
            m_thread->start();
        }
        return m_context;
    }

    QThread *m_thread = nullptr;
    QObject *m_context = nullptr;
    QMutex m_mutex;
};

} // namespace

namespace NetUtils
{

qint64 ipToLong(const QString &ip)
{
    quint32 value = 0;
    if (!parseIpv4(ip, &value))
        return -1;
    return static_cast<qint64>(value);
}

QString longToIp(qint64 value)
{
    const quint32 v = static_cast<quint32>(value);
    return QStringLiteral("%1.%2.%3.%4")
        .arg((v >> 24) & 0xFF)
        .arg((v >> 16) & 0xFF)
        .arg((v >> 8) & 0xFF)
        .arg(v & 0xFF);
}

bool isPrivateIp(const QString &ip)
{
    quint32 value = 0;
    if (!parseIpv4(ip, &value))
        return false;

    const quint32 a = (value >> 24) & 0xFF;
    const quint32 b = (value >> 16) & 0xFF;

    if (a == 10) // 10.0.0.0/8
        return true;
    if (a == 172 && b >= 16 && b <= 31) // 172.16.0.0/12
        return true;
    if (a == 192 && b == 168) // 192.168.0.0/16
        return true;
    return false;
}

bool icmpPing(const QString &ip, int timeoutMs, qint64 *roundTripMs)
{
    if (roundTripMs)
        *roundTripMs = -1;

    quint32 address = 0;
    if (!ipv4ToNetworkOrder(ip, &address))
        return false;

    ensureWinsock();

    HANDLE handle = IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    const QByteArray payload(32, '\0');
    // 回复缓冲区需要容纳 ICMP_ECHO_REPLY 结构体 + 回显数据 + 8 字节 ICMP 头
    QByteArray replyBuffer(static_cast<int>(sizeof(ICMP_ECHO_REPLY)) + payload.size() + 8, '\0');
    const DWORD timeout = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 1);

    bool ok = false;
    const DWORD replyCount = IcmpSendEcho(handle, static_cast<IPAddr>(address),
                                          const_cast<char *>(payload.constData()),
                                          static_cast<WORD>(payload.size()), nullptr,
                                          replyBuffer.data(),
                                          static_cast<DWORD>(replyBuffer.size()), timeout);
    if (replyCount > 0)
    {
        const ICMP_ECHO_REPLY *reply = reinterpret_cast<const ICMP_ECHO_REPLY *>(replyBuffer.constData());
        if (reply->Status == IP_SUCCESS)
        {
            if (roundTripMs)
                *roundTripMs = static_cast<qint64>(reply->RoundTripTime);
            ok = true;
        }
    }

    IcmpCloseHandle(handle);
    return ok;
}

PortProbeResult probePorts(const QString &ip, const QVector<int> &ports, int totalBudgetMs)
{
    PortProbeResult result;
    if (ports.isEmpty())
        return result;

    sockaddr_in target;
    if (!buildSockAddr(ip, &target))
        return result;

    ensureWinsock();

    // 每个端口分配一个时间片，整体不超过 totalBudgetMs
    const int budgetPerPort = qMax(1, totalBudgetMs / ports.size());
    QElapsedTimer timer;
    timer.start();

    for (int port : ports)
    {
        const int remaining = totalBudgetMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0)
            break;
        const int portBudget = qMax(1, qMin(budgetPerPort, remaining));

        const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
            break;

        target.sin_port = htons(static_cast<u_short>(port));

        // 非阻塞 connect：连接成功与被拒绝都能给出确定性结论
        u_long nonBlocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0)
        {
            ::closesocket(sock);
            continue;
        }

        // 0 = 不确定；>0 = 端口开放（值为端口号）；<0 = 主机在线（收到 RST，值为 -端口号）
        int status = 0;
        if (::connect(sock, reinterpret_cast<const sockaddr *>(&target), sizeof(target)) == 0)
        {
            status = port;
        }
        else
        {
            const int lastError = WSAGetLastError();
            if (lastError == WSAECONNREFUSED)
            {
                status = -port;
            }
            else if (lastError == WSAEWOULDBLOCK)
            {
                WSAPOLLFD pollFd;
                pollFd.fd = sock;
                pollFd.events = POLLWRNORM;
                pollFd.revents = 0;

                if (WSAPoll(&pollFd, 1, portBudget) > 0)
                {
                    int socketError = 0;
                    int socketErrorSize = static_cast<int>(sizeof(socketError));
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char *>(&socketError), &socketErrorSize) == 0)
                    {
                        if (socketError == 0)
                            status = port; // 三次握手完成 → 端口开放
                        else if (socketError == WSAECONNREFUSED)
                            status = -port; // RST → 主机明确在线
                    }
                }
            }
        }

        ::closesocket(sock);

        if (status > 0)
        {
            result.openPort = status;
            result.hostAlive = true;
            return result;
        }
        if (status < 0)
        {
            result.hostAlive = true;
            return result;
        }
    }

    return result;
}

QHash<QString, QString> readArpTable(int timeoutMs)
{
    QHash<QString, QString> table;

    const QString output = runArpCommand(QStringList() << QStringLiteral("-a"), timeoutMs);
    if (output.isEmpty())
        return table;

    static const QRegularExpression macPattern(
        QStringLiteral("^[0-9A-F]{2}(-[0-9A-F]{2}){5}$"));

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        const QStringList fields = splitArpFields(line);
        if (fields.size() < 2)
            continue;

        quint32 addressValue = 0;
        if (!parseIpv4(fields.at(0), &addressValue))
            continue;

        // 跳过静态项（第 3 列是 "static"）
        if (fields.size() >= 3 && fields.at(2).compare(QStringLiteral("static"), Qt::CaseInsensitive) == 0)
            continue;

        QString mac = fields.at(1);
        mac.replace(QLatin1Char(':'), QLatin1Char('-'));
        mac = mac.toUpper();

        if (macPattern.match(mac).hasMatch()
            && mac.compare(QStringLiteral("FF-FF-FF-FF-FF-FF"), Qt::CaseInsensitive) != 0)
        {
            table.insert(longToIp(static_cast<qint64>(addressValue)), mac);
        }
    }

    return table;
}

QString queryArpEntry(const QString &ip, int timeoutMs)
{
    if (ip.isEmpty())
        return QString();

    const QString output = runArpCommand(QStringList() << QStringLiteral("-a") << ip, timeoutMs);
    if (output.isEmpty())
        return QString();

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        if (!line.contains(ip))
            continue;

        const QStringList fields = splitArpFields(line);
        if (fields.size() < 2)
            continue;

        const QString mac = fields.at(1).trimmed();
        if (mac.contains(QLatin1Char('-')) && mac.size() == 17)
            return mac.toUpper();
    }

    return QString();
}

QString formatMac(const QString &raw)
{
    return formatMacAddress(raw);
}

QVector<LocalInterface> localInterfaces()
{
    QVector<LocalInterface> interfaces;

    ensureWinsock();

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG bufferSize = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &bufferSize) != ERROR_BUFFER_OVERFLOW)
        return interfaces;
    if (bufferSize == 0)
        return interfaces;

    QByteArray buffer(static_cast<int>(bufferSize), '\0');
    IP_ADAPTER_ADDRESSES *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize) != NO_ERROR)
        return interfaces;

    for (IP_ADAPTER_ADDRESSES *adapter = addresses; adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        QString ipv4;
        for (IP_ADAPTER_UNICAST_ADDRESS_LH *unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next)
        {
            const LPSOCKADDR sockaddr = unicast->Address.lpSockaddr;
            if (sockaddr == nullptr || sockaddr->sa_family != AF_INET)
                continue;

            const sockaddr_in *addr = reinterpret_cast<const sockaddr_in *>(sockaddr);
            const QHostAddress address(ntohl(addr->sin_addr.s_addr));
            if (!address.isNull() && !address.isLoopback())
            {
                ipv4 = address.toString();
                break;
            }
        }

        if (ipv4.isEmpty())
            continue;

        LocalInterface entry;
        entry.name = QString::fromWCharArray(adapter->FriendlyName);
        entry.index = static_cast<int>(adapter->Ipv6IfIndex);
        entry.ipv4 = ipv4;
        interfaces.append(entry);
    }

    return interfaces;
}

void primeIpv6Neighbors(int interfaceIndex)
{
    if (interfaceIndex <= 0)
        return;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    const QString target = QStringLiteral("ff02::1%") + QString::number(interfaceIndex);
    const QStringList arguments = QStringList()
        << QStringLiteral("-6") << QStringLiteral("-n") << QStringLiteral("1")
        << QStringLiteral("-w") << QStringLiteral("400") << target;
    process.start(QStringLiteral("ping"), arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(500))
        return;

    if (process.state() != QProcess::NotRunning)
    {
        if (!process.waitForFinished(1500))
        {
            process.kill();
            process.waitForFinished(200);
        }
    }
}

QHash<QString, QString> readIpv6Neighbors(int timeoutMs)
{
    QHash<QString, QString> table;

    const QStringList arguments = QStringList()
        << QStringLiteral("interface") << QStringLiteral("ipv6") << QStringLiteral("show")
        << QStringLiteral("neighbors");
    const QString output = runSystemCommand(QStringLiteral("netsh"), arguments, timeoutMs);
    if (output.isEmpty())
        return table;

    const QStringList lines = splitArpOutput(output);
    for (const QString &line : lines)
    {
        const QStringList fields = splitArpFields(line);
        if (fields.size() < 3)
            continue;

        // 只保留链路本地地址（自动排除 ff02:: 组播与全局地址）
        const QString address = fields.at(0);
        if (!address.startsWith(QStringLiteral("fe80"), Qt::CaseInsensitive))
            continue;

        const QString mac = formatMacAddress(fields.at(1));
        if (mac.isEmpty() || mac == QStringLiteral("00-00-00-00-00-00"))
            continue;

        if (!table.contains(mac))
            table.insert(mac, address);
    }

    return table;
}

QString localMacAddress()
{
    QString fallback;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces)
    {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;

        const QString mac = formatMacAddress(iface.hardwareAddress());
        if (mac.isEmpty())
            continue;

        bool hasIpv4 = false;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries)
        {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            {
                hasIpv4 = true;
                break;
            }
        }

        if (hasIpv4)
            return mac; // 优先返回持有 IPv4 地址的网卡
        if (fallback.isEmpty())
            fallback = mac;
    }

    return fallback;
}

QString defaultGatewayIp()
{
    ensureWinsock();

    ULONG bufferSize = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &bufferSize)
        != ERROR_BUFFER_OVERFLOW)
    {
        return QString();
    }
    if (bufferSize == 0)
        return QString();

    QByteArray buffer(static_cast<int>(bufferSize), '\0');
    IP_ADAPTER_ADDRESSES *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    const ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                              addresses, &bufferSize);
    if (result != NO_ERROR)
        return QString();

    for (IP_ADAPTER_ADDRESSES *adapter = addresses; adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        for (IP_ADAPTER_GATEWAY_ADDRESS_LH *gateway = adapter->FirstGatewayAddress;
             gateway != nullptr; gateway = gateway->Next)
        {
            const LPSOCKADDR sockaddr = gateway->Address.lpSockaddr;
            if (sockaddr == nullptr || sockaddr->sa_family != AF_INET)
                continue;

            const sockaddr_in *ipv4 = reinterpret_cast<const sockaddr_in *>(sockaddr);
            const QHostAddress address(ntohl(ipv4->sin_addr.s_addr));
            if (!address.isNull())
                return address.toString();
        }
    }

    return QString();
}

QString localIpv4Address()
{
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses)
    {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            return address.toString();
    }
    return QString();
}

QString resolveHostName(const QString &ip, int timeoutMs)
{
    if (ip.isEmpty())
        return QString();
    return DnsResolver::instance().resolve(ip, timeoutMs > 0 ? timeoutMs : 1);
}

} // namespace NetUtils
