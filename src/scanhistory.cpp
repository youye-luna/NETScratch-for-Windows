// scanhistory.cpp —— 扫描历史持久化（对应 C# ScanHistory.cs）
//
// 每次扫描保存为独立 CSV 文件，存放在程序目录下的 ScanHistory 文件夹；
// 文件头为 "# " 注释形式的元信息行，正文为设备数据，格式与 C# 版完全一致。

#include "scanhistory.h"

#include "appsettings.h"
#include "lang.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QUuid>

#include <algorithm>

namespace
{
const QLatin1String kMetaPrefix("# ScanHistory v1");
const QLatin1String kMetaTimePrefix("# Time:");
const QLatin1String kMetaStartIpPrefix("# StartIp:");
const QLatin1String kMetaEndIpPrefix("# EndIp:");

QString historyDirPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ScanHistory"));
}

void ensureDir()
{
    QDir().mkpath(historyDirPath());
}

/// 文件名中不允许的字符替换为下划线（防御性处理）
QString sanitizeFileName(const QString &name)
{
    QString result = name;
    result.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]")), QStringLiteral("_"));
    return result;
}

QString buildFileName(const QDateTime &time, const QString &startIp, const QString &endIp)
{
    const QString dateStr = time.toString(Lang::get(QStringLiteral("HistoryDateFormat")));
    const QString name = QStringLiteral("%1 %2~%3（IP）").arg(dateStr, startIp, endIp);
    return sanitizeFileName(name) + QStringLiteral(".csv");
}

/// 从文件名解析扫描时间和 IP 范围（兼容简体中文与英文两种日期格式）
bool parseFileName(const QString &filePath, QDateTime *time, QString *startIp, QString *endIp)
{
    QString name = QFileInfo(filePath).completeBaseName();
    const QString suffix = QStringLiteral("（IP）");
    if (name.endsWith(suffix))
        name.chop(suffix.size());

    const int spaceIndex = name.indexOf(QLatin1Char(' '));
    if (spaceIndex < 0)
        return false;

    const QString datePart = name.left(spaceIndex);
    const QString ipPart = name.mid(spaceIndex + 1);

    QDateTime parsed;
    static const QStringList formats = {
        QStringLiteral("yyyy年MM月dd日"),
        QStringLiteral("yyyy-MM-dd"),
    };
    for (const QString &format : formats)
    {
        parsed = QDateTime::fromString(datePart, format);
        if (parsed.isValid())
            break;
    }
    if (!parsed.isValid())
        return false;

    const QStringList ips = ipPart.split(QLatin1Char('~'));
    if (ips.size() != 2)
        return false;

    *time = parsed;
    *startIp = ips.at(0);
    *endIp = ips.at(1);
    return true;
}

QString escapeCsvField(const QString &field)
{
    if (field.contains(QLatin1Char(',')) || field.contains(QLatin1Char('"')) || field.contains(QLatin1Char('\n')))
    {
        QString escaped = field;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(escaped);
    }
    return field;
}

QStringList parseCsvLine(const QString &line)
{
    QStringList fields;
    bool inQuotes = false;
    QString current;

    for (int i = 0; i < line.size(); ++i)
    {
        const QChar c = line.at(i);
        if (c == QLatin1Char('"'))
        {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"'))
            {
                current.append(QLatin1Char('"'));
                ++i;
            }
            else
            {
                inQuotes = !inQuotes;
            }
        }
        else if (c == QLatin1Char(',') && !inQuotes)
        {
            fields.append(current);
            current.clear();
        }
        else
        {
            current.append(c);
        }
    }
    fields.append(current);
    return fields;
}

void writeCsv(const QString &filePath, const ScanHistoryRecord &record)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream.setGenerateByteOrderMark(true);

    // 元信息（注释行，供读取时还原 ScanHistoryRecord）
    stream << kMetaPrefix << "\r\n";
    stream << kMetaTimePrefix << record.scanTime.toString(Qt::ISODateWithMs) << "\r\n";
    stream << kMetaStartIpPrefix << record.startIp << "\r\n";
    stream << kMetaEndIpPrefix << record.endIp << "\r\n";

    // 表头
    stream << Lang::get(QStringLiteral("CsvHeader")) << "\r\n";

    // 设备数据
    const QString online = Lang::get(QStringLiteral("Online"));
    const QString noDevice = Lang::get(QStringLiteral("NoDevice"));
    const QString yes = Lang::get(QStringLiteral("Yes"));
    const QString no = Lang::get(QStringLiteral("No"));

    for (const ScanHistoryDevice &device : record.devices)
    {
        const QStringList ipParts = device.ipAddress.split(QLatin1Char('.'));
        const QString subnet = QStringList(ipParts.mid(0, 3)).join(QLatin1Char('.'));
        const QString status = device.isActive ? online : noDevice;
        const QString ping = (device.isActive && device.pingMs >= 0) ? QString::number(device.pingMs) : QStringLiteral("-");
        const QString dhcp = device.isDhcpServer ? yes : no;

        stream << escapeCsvField(subnet) << ','
               << escapeCsvField(device.ipAddress) << ','
               << escapeCsvField(device.macAddress) << ','
               << escapeCsvField(device.hostName) << ','
               << escapeCsvField(ping) << ','
               << escapeCsvField(dhcp) << ','
               << escapeCsvField(status) << "\r\n";
    }
}

/// 解析时间元信息：优先 ISODateWithMs，其次 ISODate
QDateTime parseMetaTime(const QString &value)
{
    QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!result.isValid())
        result = QDateTime::fromString(value, Qt::ISODate);
    return result;
}

bool readCsv(const QString &filePath, ScanHistoryRecord *record)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream.setAutoDetectUnicode(true);

    QStringList lines;
    while (!stream.atEnd())
        lines.append(stream.readLine());
    file.close();

    if (lines.isEmpty())
        return false;

    record->filePath = filePath;
    int dataStart = 0;

    // 解析元信息注释行
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString line = lines.at(i);
        if (line.startsWith(kMetaTimePrefix))
        {
            const QDateTime parsed = parseMetaTime(line.mid(kMetaTimePrefix.size()));
            if (parsed.isValid())
                record->scanTime = parsed;
            dataStart = i + 1;
        }
        else if (line.startsWith(kMetaStartIpPrefix))
        {
            record->startIp = line.mid(kMetaStartIpPrefix.size());
            dataStart = i + 1;
        }
        else if (line.startsWith(kMetaEndIpPrefix))
        {
            record->endIp = line.mid(kMetaEndIpPrefix.size());
            dataStart = i + 1;
        }
        else if (line.startsWith(QLatin1Char('#')))
        {
            dataStart = i + 1;
        }
        else
        {
            break;
        }
    }

    // 跳过表头行（如果有）
    if (dataStart < lines.size() && !lines.at(dataStart).trimmed().isEmpty())
        ++dataStart;

    // 旧版无元信息文件 → 从文件名解析
    if (record->startIp.isEmpty() || record->endIp.isEmpty())
    {
        QDateTime time;
        QString startIp;
        QString endIp;
        if (parseFileName(filePath, &time, &startIp, &endIp))
        {
            record->scanTime = time;
            record->startIp = startIp;
            record->endIp = endIp;
        }
    }

    // 解析设备数据行
    const QString online = Lang::get(QStringLiteral("Online"));
    const QString yes = Lang::get(QStringLiteral("Yes"));

    for (int i = dataStart; i < lines.size(); ++i)
    {
        const QString line = lines.at(i);
        if (line.trimmed().isEmpty())
            continue;

        const QStringList fields = parseCsvLine(line);
        if (fields.size() < 7)
            continue;

        // 字段顺序：网段,IP地址,MAC地址,主机名,延迟(ms),DHCP服务器,状态
        const QString statusField = fields.at(6);
        const QString dhcpField = fields.at(5);

        const bool isActive = statusField == online
                              || statusField == QStringLiteral("在线")
                              || statusField == QStringLiteral("Online")
                              || statusField == QStringLiteral("線上");
        const bool isDhcp = dhcpField == yes
                            || dhcpField == QStringLiteral("是")
                            || dhcpField == QStringLiteral("Yes");

        qint64 ping = -1;
        if (fields.at(4) != QStringLiteral("-"))
        {
            bool ok = false;
            const qint64 parsed = fields.at(4).toLongLong(&ok);
            if (ok)
                ping = parsed;
        }

        ScanHistoryDevice device;
        device.ipAddress = fields.at(1);
        device.macAddress = fields.at(2);
        device.hostName = fields.at(3);
        device.pingMs = ping;
        device.isActive = isActive;
        device.isDhcpServer = isDhcp;
        device.responseTime = record->scanTime;
        record->devices.append(device);
    }

    return true;
}

QVector<ScanHistoryRecord> readAll()
{
    QVector<ScanHistoryRecord> records;

    const QDir dir(historyDirPath());
    if (!dir.exists())
        return records;

    const QStringList files = dir.entryList({QStringLiteral("*.csv")}, QDir::Files);
    for (const QString &name : files)
    {
        ScanHistoryRecord record;
        if (readCsv(dir.filePath(name), &record))
            records.append(record);
    }

    std::sort(records.begin(), records.end(), [](const ScanHistoryRecord &a, const ScanHistoryRecord &b) {
        return a.scanTime > b.scanTime;
    });
    return records;
}

/// 根据设置的保存策略筛选应保留的记录列表（不删除文件）
QVector<ScanHistoryRecord> applyRetention(const QVector<ScanHistoryRecord> &list)
{
    if (list.isEmpty())
        return list;

    const AppSettings settings = AppSettings::load();
    if (settings.historySaveMode == HistorySaveMode::None)
        return list; // 不保存模式：不清理已有文件
    if (settings.historySaveMode == HistorySaveMode::ByCount)
    {
        if (list.size() <= settings.historySaveMaxRecords)
            return list;
        return list.mid(0, settings.historySaveMaxRecords);
    }
    if (settings.historySaveDays <= 0)
        return list; // 不清除

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-settings.historySaveDays);
    QVector<ScanHistoryRecord> retained;
    for (const ScanHistoryRecord &record : list)
    {
        if (record.scanTime >= cutoff)
            retained.append(record);
    }
    return retained;
}
} // namespace

ScanHistoryDevice ScanHistoryDevice::from(const DhcpServerInfo &info)
{
    ScanHistoryDevice device;
    device.ipAddress = info.ipAddress;
    device.macAddress = info.macAddress;
    device.hostName = info.hostName;
    device.pingMs = info.pingMs;
    device.isActive = info.isActive;
    device.isDhcpServer = info.isDhcpServer;
    device.responseTime = info.responseTime;
    return device;
}

DhcpServerInfo ScanHistoryDevice::toServerInfo() const
{
    DhcpServerInfo info;
    info.ipAddress = QHostAddress(ipAddress).isNull() ? QStringLiteral("0.0.0.0") : ipAddress;
    info.macAddress = macAddress;
    info.hostName = hostName;
    info.pingMs = pingMs;
    info.isActive = isActive;
    info.isDhcpServer = isDhcpServer;
    info.responseTime = responseTime;
    return info;
}

namespace ScanHistoryStore
{

QVector<ScanHistoryRecord> load()
{
    if (AppSettings::load().historySaveMode == HistorySaveMode::None)
        return {}; // 不保存模式：不展示任何历史记录
    return applyRetention(readAll());
}

void save(ScanHistoryRecord *record)
{
    if (!record)
        return;

    if (AppSettings::load().historySaveMode == HistorySaveMode::None)
        return; // 不保存模式：扫描结果不写入历史记录

    ensureDir();

    const QString fileName = buildFileName(record->scanTime, record->startIp, record->endIp);
    QString filePath = QDir(historyDirPath()).filePath(fileName);

    // 同名文件处理（同秒多次扫描）
    if (QFileInfo::exists(filePath))
    {
        const QString baseName = QFileInfo(fileName).completeBaseName();
        int seq = 2;
        while (QFileInfo::exists(filePath))
        {
            filePath = QDir(historyDirPath()).filePath(QStringLiteral("%1(%2).csv").arg(baseName).arg(seq));
            ++seq;
        }
    }

    writeCsv(filePath, *record);
    record->filePath = filePath;
    prune();
}

void prune()
{
    const QVector<ScanHistoryRecord> records = readAll();
    const QVector<ScanHistoryRecord> retained = applyRetention(records);

    QSet<QString> retainedPaths;
    for (const ScanHistoryRecord &record : retained)
    {
        if (!record.filePath.isEmpty())
            retainedPaths.insert(record.filePath);
    }

    for (const ScanHistoryRecord &record : records)
    {
        if (!record.filePath.isEmpty() && !retainedPaths.contains(record.filePath))
            QFile::remove(record.filePath);
    }
}

void remove(const ScanHistoryRecord &record)
{
    if (!record.filePath.isEmpty())
        QFile::remove(record.filePath);
}

void clear()
{
    QDir dir(historyDirPath());
    if (dir.exists())
        dir.removeRecursively();
}

} // namespace ScanHistoryStore
