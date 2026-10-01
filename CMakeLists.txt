cmake_minimum_required(VERSION 3.16)

# 未显式指定时使用 Qt 自带 MinGW 的 windres：若 PATH 中混入其他工具链（如 MSYS2），
# 会出现 "windres: preprocessing failed"
if(NOT CMAKE_RC_COMPILER AND EXISTS "J:/Qt/Tools/mingw810_64/bin/windres.exe")
    set(CMAKE_RC_COMPILER "J:/Qt/Tools/mingw810_64/bin/windres.exe" CACHE FILEPATH "" FORCE)
endif()

project(NETScratch VERSION 2.0 LANGUAGES CXX RC)

# app.rc 为 UTF-8 编码且含中文版本信息，需告知 windres 输入代码页，否则中文会乱码
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --codepage=65001")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)

# 未显式指定 Qt 路径时，回退到本机已安装的 Qt 5.15.2 (MinGW 8.1 64-bit)
if(NOT CMAKE_PREFIX_PATH AND EXISTS "J:/Qt/5.15.2/mingw81_64")
    set(CMAKE_PREFIX_PATH "J:/Qt/5.15.2/mingw81_64")
endif()

find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Network WinExtras)

set(PROJECT_SOURCES
    src/main.cpp

    src/lang.h
    src/lang.cpp

    src/appsettings.h
    src/appsettings.cpp

    src/netutils.h
    src/netutils.cpp

    src/cameradetector.h
    src/cameradetector.cpp

    src/netdiscovery.h
    src/netdiscovery.cpp

    src/scanner.h
    src/scanner.cpp

    src/nmaprunner.h
    src/nmaprunner.cpp

    src/scanhistory.h
    src/scanhistory.cpp

    src/ipaddressedit.h
    src/ipaddressedit.cpp

    src/ipgridpanel.h
    src/ipgridpanel.cpp

    src/subnetresultpanel.h
    src/subnetresultpanel.cpp

    src/settingspanel.h
    src/settingspanel.cpp

    src/historypanel.h
    src/historypanel.cpp

    src/uistyle.h

    src/toggleswitch.h
    src/toggleswitch.cpp

    src/mainwindow.h
    src/mainwindow.cpp
)

add_executable(NETScratch WIN32
    ${PROJECT_SOURCES}
    app.rc
    resources.qrc
)

target_link_libraries(NETScratch PRIVATE
    Qt5::Widgets
    Qt5::Network
    Qt5::WinExtras
    iphlpapi
    ws2_32
)

# ---------------------------------------------------------------- nmap 子进程
# 主机发现由官方 nmap.exe 以子进程方式完成。nmap 使用 NPSL 许可（与 GPLv3 不兼容），
# 按其许可要求把 nmap 自带的许可文件一并放入 release/nmap 内随附分发。
set(NMAP_DIST_DIR "J:/nmap-dist/extracted" CACHE PATH "nmap 免安装发行目录")
# 必须是 Win7 可用的 32 位 VC++ 运行库（14.29 / VS2019 16.11 为最后一个支持 Win7 SP1 的版本）。
# 14.3x 及以上版本会导入 Win8+ 的 GetSystemTimePreciseAsFileTime，导致 Win7 报
# “无法定位程序输入点 ... 于动态链接库 KERNEL32.dll 上”。
# 注意：这里是 CACHE 变量，修改默认值后需清理构建目录内的 CMakeCache.txt 才会生效。
set(NMAP_VCRUNTIME_DIR "J:/nmap-dist/vcruntime32" CACHE PATH "nmap 依赖的 32 位 VC++ 运行库目录（Win7 兼容版 14.29）")
# Win7 不自带 UCRT，需 app-local 随 nmap.exe 一同部署 ucrtbase.dll 与 api-ms-win-*-l1-1-0.dll
# （取自 Windows SDK Redist\ucrt\DLLs\x86，10.0.10240，已确认不含 Win8+ 导入）
set(NMAP_UCRT_DIR "J:/nmap-dist/ucrt32" CACHE PATH "nmap 依赖的 32 位 UCRT 运行库目录")

set(NMAP_DEPLOY_COMMANDS)
if(EXISTS "${NMAP_DIST_DIR}/nmap.exe")
    set(_NMAP_DST_DIR "${CMAKE_SOURCE_DIR}/release/nmap")

    # nmap.exe 为 32 位程序，其运行库与许可文件必须放在 nmap.exe 所在目录
    set(_NMAP_RUNTIME_FILES
        "${NMAP_DIST_DIR}/nmap.exe"
        "${NMAP_DIST_DIR}/libcrypto-3.dll"
        "${NMAP_DIST_DIR}/libssh2.dll"
        "${NMAP_DIST_DIR}/libssl-3.dll"
        "${NMAP_DIST_DIR}/zlibwapi.dll"
        "${NMAP_VCRUNTIME_DIR}/MSVCP140.dll"
        "${NMAP_VCRUNTIME_DIR}/VCRUNTIME140.dll"
        "${NMAP_DIST_DIR}/nmap-services"
        "${NMAP_DIST_DIR}/nmap-protocols"
        "${NMAP_DIST_DIR}/nmap-mac-prefixes"
        "${NMAP_DIST_DIR}/LICENSE"
        "${NMAP_DIST_DIR}/3rd-party-licenses.txt"
    )
    # 不随包分发 nmap-os-db / nmap-service-probes / nse_main.lua / nmap-rpc：
    # 这些只在 -O（系统识别）/ -sV（版本探测）/ -sC（NSE 脚本）/ -sR（RPC）时才会读取，
    # 本程序固定用 `nmap -sn` 做主机发现（实测去掉后扫描、ARP、MAC 厂商均正常），
    # 去掉可省约 8 MB。

    list(APPEND NMAP_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_NMAP_DST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_NMAP_RUNTIME_FILES} "${_NMAP_DST_DIR}/")

    # UCRT（ucrtbase.dll + api-ms-win-*-l1-1-0.dll，32 位）：Win7 无此组件，随 nmap.exe 同目录部署
    if(EXISTS "${NMAP_UCRT_DIR}")
        list(APPEND NMAP_DEPLOY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${NMAP_UCRT_DIR}" "${_NMAP_DST_DIR}")
    else()
        message(WARNING "未找到 ${NMAP_UCRT_DIR}，Win7 上 nmap 可能因缺少 UCRT 而无法运行")
    endif()

    foreach(_nmap_dir licenses)
        if(EXISTS "${NMAP_DIST_DIR}/${_nmap_dir}")
            list(APPEND NMAP_DEPLOY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${NMAP_DIST_DIR}/${_nmap_dir}" "${_NMAP_DST_DIR}/${_nmap_dir}")
        endif()
    endforeach()
else()
    message(WARNING "未找到 ${NMAP_DIST_DIR}/nmap.exe，release 将不包含 nmap")
endif()

# ---------------------------------------------------------------- 部署到 release
# 用户要求：只产出包含全部运行库 + 可执行文件的 release 目录（不打包 zip/exe）
# 注：windeployqt 在 MinGW 构建下会误判插件为 debug 版本并报
#     "Unable to find the platform plugin."，故此处直接拷贝所需运行库。
get_target_property(_QMAKE_EXE Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_QT_BIN_DIR "${_QMAKE_EXE}" DIRECTORY)
get_filename_component(_QT_PREFIX_DIR "${_QT_BIN_DIR}" DIRECTORY)
set(_QT_PLUGINS_DIR "${_QT_PREFIX_DIR}/plugins")
get_filename_component(_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)

set(QT_RUNTIME_DLLS
    "${_QT_BIN_DIR}/Qt5Core.dll"
    "${_QT_BIN_DIR}/Qt5Gui.dll"
    "${_QT_BIN_DIR}/Qt5Widgets.dll"
    "${_QT_BIN_DIR}/Qt5Network.dll"
    "${_QT_BIN_DIR}/Qt5WinExtras.dll"
)

set(MINGW_RUNTIME_DLLS
    "${_MINGW_BIN_DIR}/libgcc_s_seh-1.dll"
    "${_MINGW_BIN_DIR}/libstdc++-6.dll"
    "${_MINGW_BIN_DIR}/libwinpthread-1.dll"
)

add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NETScratch>" "${CMAKE_SOURCE_DIR}/release/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${QT_RUNTIME_DLLS} ${MINGW_RUNTIME_DLLS} "${CMAKE_SOURCE_DIR}/release/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_QT_PLUGINS_DIR}/platforms/qwindows.dll"
            "${CMAKE_SOURCE_DIR}/release/platforms/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release/styles"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_QT_PLUGINS_DIR}/styles/qwindowsvistastyle.dll"
            "${CMAKE_SOURCE_DIR}/release/styles/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/release/imageformats"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_QT_PLUGINS_DIR}/imageformats/qico.dll"
            "${CMAKE_SOURCE_DIR}/release/imageformats/"
    ${NMAP_DEPLOY_COMMANDS}
    DEPENDS NETScratch
    VERBATIM
    COMMENT "部署到 release 目录（含 Qt 运行库、编译器运行库与 nmap）"
)
