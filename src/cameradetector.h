#pragma once

#include <QString>

#include "netdiscovery.h"

/// 摄像头识别结果
struct CameraDetection
{
    bool isCamera = false;
    /// 命中的证据摘要（如 "RTSP;HTTP-强指纹"），未命中为空
    QString evidence;
};

/// 网络摄像头（IPC / DVR / NVR 等视频监控设备）识别。
/// 综合 RTSP 应答、HTTP 指纹、厂商私有端口、MAC 厂商前缀、主机名与组播发现线索
/// 加权打分，累计分数达到阈值才判定为摄像头，尽量降低误报。
namespace CameraDetector
{
/// hint 为该 IP 在组播发现（SSDP / mDNS）阶段收到的线索，可为 nullptr。
/// 家用 P2P 摄像头常常一个端口都不开，只有 hint 能提供判据。
CameraDetection detect(const QString &ip, const QString &macAddress, const QString &hostName,
                       const DiscoveryHint *hint = nullptr);
} // namespace CameraDetector
