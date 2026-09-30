#pragma once

#include "lang.h"

#include <QString>

/// 扫描历史保存方式
enum class HistorySaveMode
{
    ByTime = 0, ///< 按时间保存（保留最近 N 天）
    ByCount = 1 ///< 按数量保存（保留最近 N 条）
};

/// 应用设置（持久化到程序目录下的 settings.json）
struct AppSettings
{
    AppLanguage language = AppLanguage::Chinese;
    int scanThreads = 30;
    /// 是否启用 IPv6 地址显示：开启才扫描 IPv6 并在结果表格中显示该列
    bool ipv6Enabled = true;
    HistorySaveMode historySaveMode = HistorySaveMode::ByCount;
    int historySaveDays = 30;
    int historySaveMaxRecords = 100;

    /// 日期显示格式；空串表示跟随界面语言
    QString dateFormat;
    /// 时间显示格式；空串表示跟随界面语言
    QString timeFormat;

    /// 逐字段比较（设置页用于检测未保存的更改）
    bool operator==(const AppSettings &other) const
    {
        return language == other.language && scanThreads == other.scanThreads
            && ipv6Enabled == other.ipv6Enabled && historySaveMode == other.historySaveMode
            && historySaveDays == other.historySaveDays
            && historySaveMaxRecords == other.historySaveMaxRecords
            && dateFormat == other.dateFormat && timeFormat == other.timeFormat;
    }
    bool operator!=(const AppSettings &other) const { return !(*this == other); }

    /// 可选的日期格式（唯一数据源，设置页与校验共用）
    static QStringList supportedDateFormats();

    /// 可选的时间格式
    static QStringList supportedTimeFormats();

    /// 实际生效的日期格式：未设置时回退到当前语言的内置格式
    QString effectiveDateFormat() const;

    /// 实际生效的时间格式：未设置时回退到当前语言的内置格式
    QString effectiveTimeFormat() const;

    /// 从配置文件加载设置，首次运行时自动检测系统语言并保存
    static AppSettings load();

    /// 根据系统区域设置自动检测语言
    static AppLanguage detectSystemLanguage();

    /// 保存设置到配置文件
    void save() const;

    /// 配置文件所在目录（程序所在目录）
    static QString configDir();

    /// 配置文件完整路径
    static QString filePath();
};
