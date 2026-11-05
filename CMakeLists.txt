cmake_minimum_required(VERSION 3.24)

# ---- 版本号只有一个来源：ltool.iss 里那条 #define MyAppVersion ----
# 上一版它散在 CMakeLists / iss 的 #define 和注释 / README / 五个交付脚本里，升一次要改七处，
# 漏一处就是"脚本装着昨天的包还报绿"。现在 CMake 从 iss 解析，解析不到直接 FATAL：
# 宁可构建失败，也不要编出一个显示旧版本号的界面。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ltool.iss" _iss_text)
string(REGEX MATCH "#define MyAppVersion \"([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?)\"" _iss_ver "${_iss_text}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "ltool.iss 里没读到 #define MyAppVersion \"x.y.z\"（预发可以写 x.y.z-beta.N）")
endif()
set(LTOOL_VERSION_STRING "${CMAKE_MATCH_1}")
# `project(VERSION ...)` 只吃纯数字段：喂 "0.8.1-beta.1" 进去 CMake 当场报
# "Invalid number in project version"。所以数字部分给它、**完整那一串**给 LTOOL_VERSION 宏，
# 界面显示哪个数由宏说了算（预发要让用户看得见 -beta.N）。
string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+).*$" "\\1" LTOOL_VERSION_CORE "${LTOOL_VERSION_STRING}")
# 让构建系统盯着 ltool.iss：改了版本号，下一次 cmake --build 会自己重新 configure。
# 不写这条的话 iss 改了、宏还是上次那个数 —— 实测把 0.6.0 改成 0.7.0 之后
# 界面仍然显示 0.6.0，"只改一处"这句话就是空的。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ltool.iss)
message(STATUS "ltool 版本 ${LTOOL_VERSION_STRING}（来自 ltool.iss）")

project(ltool VERSION ${LTOOL_VERSION_CORE} LANGUAGES CXX)

# ---- MSVC：让一个目标里的所有 .cpp 并行编译（/MP）----
# VS 生成器默认把一个目标里的编译任务排队串行跑：本项目 53 个 TU，改一次 src/Utils.h
# （47 个 TU 包含它）串行要 114.7 秒，并行之后 16.4 秒；全量重建 18.3 秒。
# 不设线程上限，cl.exe 自己按逻辑核数开（本机 20 核 / 47GB 内存，峰值远吃不满）。
# 注：CMake 会把 /MP 翻成 vcxproj 里的 <MultiProcessorCompilation>true</...>，
#     所以在 AdditionalOptions 里 grep "/MP" 是找不到的。
if(MSVC)
    add_compile_options(/MP)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)   # 本项目没有 .qrc（图标走 resources/ltool.rc）

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network)

qt_standard_project_setup()

# 图标生成工具（构建期产出 resources/ltool.ico，嵌入 exe 与安装包）
add_executable(genicon tools/genicon.cpp)
target_link_libraries(genicon PRIVATE Qt6::Gui)
set(ICON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/resources/ltool.ico)
if(NOT EXISTS "${ICON_FILE}")
    add_custom_command(
        OUTPUT ${ICON_FILE}
        COMMAND ${CMAKE_COMMAND} -E env
                "QT_QPA_PLATFORM=offscreen"
                "QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins"
                $<TARGET_FILE:genicon> ${ICON_FILE}
        DEPENDS genicon
        COMMENT "genicon: 生成 ltool.ico"
        VERBATIM
    )
    add_custom_target(appicon DEPENDS ${ICON_FILE})
else()
    add_custom_target(appicon
        COMMENT "ltool.ico 已在位，跳过重复生成"
    )
endif()

# ============================================================================
# ltoolcore.dll —— 主程序和 DLL 插件共用的一份实现
#
# 为什么要有它：工具页搬进插件之后，"插件里也要用"和"主程序自己也要用"的那部分
# 代码只能有一份，否则同一个后台状态会出现两份（界面里关了广告拦截、主程序那份还在拦）。
# 边界是按引用查出来的：AdBlocker 主程序在后台跑、DevEnv/SilentInstallLog 被配置页/语言页/
# 软件页/设置窗用、RegClean 与 SysTools 被搜索框用、FileTools 被搜索框和广告页用、
# InfoRow 被卸载页用 —— 这些都进 core；那 10 个工具页 + 右键动作 + F2 录屏 + F1 截图都在插件里。
# （2026-10-15：Recorder/RecordUi/RecProbe 也从 core 搬进了 plugins/record —— 录屏只有
#  那一个插件用，主程序现在只认动作名 "record"，所以 core 里不再编它，
#  WindowPicker 上那个 LTOOL_CORE_API 也跟着删了。
#  同一天 CaptureOverlay/PinWindow/Anno/Ocr/OcrBundle/OcrPanel/OcrPp 七个搬进 plugins/capture，
#  但 core 里留下 src/CaptureHistory.{h,cpp} 两样东西：往期的那份列表（配置页改存放目录要
#  reload() 并显示"共 N 张"）和 CaptureState::isActive()（搜索框靠它决定浮层举着时先别收起）。
#  这两个都是 core→插件的反向依赖，不留在 core 就得让 core 去链插件 dll。）
#
# 导出方式用 WINDOWS_EXPORT_ALL_SYMBOLS（CMake 扫 obj 自动生成 .def），而不是给
# 每个函数贴 __declspec：本项目跨界的几乎全是自由函数和类成员，自动导出够用；
# 查过 src/*.h 里没有 extern 全局数据，所以"数据导不出来"这个已知限制踩不到。
# ============================================================================
# ---- 平台层：同一道缝，两边各一份实现，挑哪份由目标平台说了算 ----
# Windows 那两份就是原来 Utils.cpp 里的 SHGetKnownFolderPath / ShellExecuteExW 原样搬过来
# （行为零变化，判据不该因为搬个家就红）；POSIX 那两份走 XDG 目录 + xdg-open / open / pkexec。
# 缝的规格写在 src/platform/*.h 里，不在这里重复。
if(WIN32)
    set(LTOOL_PLATFORM_SRC src/platform/Paths_win.cpp src/platform/Launch_win.cpp
        src/platform/Autostart_win.cpp src/platform/FileMeta_win.cpp src/platform/ProcInfo_win.cpp
        src/platform/FileOps_win.cpp src/platform/SysConfig_win.cpp
        src/platform/AppsFolder_win.cpp)
else()
    set(LTOOL_PLATFORM_SRC src/platform/Paths_posix.cpp src/platform/Launch_posix.cpp
        src/platform/Autostart_posix.cpp src/platform/FileMeta_posix.cpp
        src/platform/ProcInfo_posix.cpp src/platform/FileOps_posix.cpp
        src/platform/SysConfig_posix.cpp
        src/platform/AppsFolder_posix.cpp
        src/platform/ShellMenu_posix.cpp
        src/platform/SevenZip_posix.cpp)
endif()

# ---- 非 Windows：台阶一，只编"量过确实可移植"的那些 TU，然后到此为止 ----
# 这份名单不是猜的：2026-10-16 在 WSL 上对 core 的 36 个 TU 逐个跑
# `g++ -std=c++20 -fsyntax-only -I src $(pkg-config --cflags Qt6*)`，13 过 23 不过。
# 不过的那 23 个全卡在同一类东西上：#include <windows.h> —— 注册表 / USN-MFT /
# WGC+Media Foundation / HKCU verb / COM 计划任务与防火墙这五族机制，还没抽成缝。
# 为什么用 OBJECT 而不是 .so：这批 TU 里有一堆还在调用 Utils:: 那些**还没搬进缝**的函数，
# 链接期要么报未定义符号、要么（Linux 默认允许 .so 留未定义符号）"链接成功"却根本跑不起来。
# 那两种形状都比现在老实说"编到这里为止"更像假绿。
if(NOT WIN32)
    set(LTOOL_CORE_PORTABLE_SRC
        src/Theme.cpp          # 只有 Qt，没碰 Win32：弹层那两份样式表要能从 Utils.cpp 里调它
        src/Utils.cpp           # 2026-10-16 夜把里面最后六个 Win32 入口抽成 FileMeta / ProcInfo
                                # 两道缝之后，这个 TU（所有人都在调它）终于是可移植的了
        src/Pinyin.cpp          # 搜索框的拼音匹配（表在 pinyin_data.h，只吃 QString）
        src/Archives.cpp        # 走系统 libarchive；bit7z 那颗后端只在 Windows 那边编
        src/CaptureHistory.cpp
        src/DownloadManager.cpp
        src/DevEnvConfig.cpp    # 2026-10-17 第九刀：DevEnv 里「代理怎么生效、装东西的根在哪」那四颗
                                # （动作两边共用，读的东西按平台走 platform/SysConfig.h）
        src/FileTools.cpp
        src/InfoRow.cpp
        src/PluginHost.cpp      # 2026-10-17 通读实测：装载走 QPluginLoader/QLibrary，本来就跨平台；
                                # 卡住的只有一处漏 include 的 QJsonDocument（MSVC 靠传递包含糊过去，
                                # GCC 当场报错）和写死的 .dll 文件名，两者都已补/抽进 platform/PluginNames.h
        src/PluginStore.cpp
        src/SilentInstallLog.cpp
        src/SysTools.cpp
        src/ToolsPage.cpp
        src/WebShortcuts.cpp
        src/WebsitesPage.cpp
        src/AboutPage.cpp       # 2026-10-17 第十刀：它自己一个 Win32 入口都没有，卡在"LTOOL_VERSION
                                # 没传进来"那条 #error 上 —— 宏给上就过了（版本号仍然只有 iss 一个来源）
        src/InstallLogDialog.cpp
        src/TaskScheduler.cpp)  # 这两棵以前挂着 #include <windows.h>，只为一句 CREATE_NO_WINDOW；
                                # 那句收进 platform/Launch.h::runWithoutConsole 之后就没别的地方用 Win32 了
                                # （计划任务其实是起 schtasks 进程，不是 COM）。
                                # ClipboardStore 留在名单外：它靠 GetClipboardSequenceNumber 判断
                                # "剪贴板到底变了没"，POSIX 没有对等的计数器 ⇒ 那是 #90 要拍形状的第五件。
    find_package(LibArchive QUIET)
    add_library(ltool_portable OBJECT ${LTOOL_PLATFORM_SRC} ${LTOOL_CORE_PORTABLE_SRC})
    target_include_directories(ltool_portable PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ltool_portable PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network)
    # 名单里有 AboutPage.cpp 了，它就要求拿到同一个版本串（没定义它自己 #error，
    # 绝不让界面显示一个"忘了改的旧版本"）。来源仍是 ltool.iss 那一处。
    target_compile_definitions(ltool_portable PRIVATE LTOOL_VERSION="${LTOOL_VERSION_STRING}")
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(ltool_portable PRIVATE LibArchive::LibArchive)
    endif()
    set_target_properties(ltool_portable PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # 数目从名单算，不写死：写死过两次都腐烂了（一度是 12、名单实际 13，再加 PluginHost 是 14）
    list(LENGTH LTOOL_CORE_PORTABLE_SRC LTOOL_PORTABLE_N)
    message(STATUS "非 Windows：只编平台层那几道缝（份数就是上面那个名单）+ 名单里那 ${LTOOL_PORTABLE_N} 个 TU；core/exe/插件要等注册表、USN、WGC、COM 和 bit7z 的对等实现")
    return()
endif()

qt_add_library(ltoolcore SHARED
    ${LTOOL_PLATFORM_SRC}
    src/Theme.cpp
    src/Utils.cpp
    src/EnvPath.cpp
    src/RegClean.cpp
    src/StartupAudit.cpp
    src/AppScanner.cpp
    src/Pinyin.cpp
    src/SysTools.cpp
    src/FileIndexer.cpp
    src/UsnIndex.cpp
    src/WebShortcuts.cpp
    src/DevEnv.cpp
    src/DevEnvConfig.cpp
    src/DownloadManager.cpp
    src/TaskScheduler.cpp
    src/WebsitesPage.cpp
    src/CaptureHistory.cpp
    src/InstallLogDialog.cpp
    src/SilentInstallLog.cpp
    src/FileTools.cpp
    src/AdBlocker.cpp
    src/DiskScan.cpp
    src/ClipboardStore.cpp
    src/ToolsPage.cpp
    src/PluginHost.cpp
    src/PluginStore.cpp
    src/Firewall.cpp
    src/InfoRow.cpp
    src/ShellMenu.cpp
    src/FileOps.cpp
    src/Archives.cpp
    src/SevenZip.cpp
    src/AboutPage.cpp
    src/ConfigPage.cpp
    src/SettingsDialog.cpp
    src/SearchWindow.cpp
)
set_target_properties(ltoolcore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
    # 导入库（1.7MB）挪出 Release 树：安装包按 build\Release\* 递归收走，躺在里面的
    # ltoolcore.lib/.exp 不是运行时文件，纯属被误打包
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib)
if(MSVC)
    target_link_options(ltoolcore PRIVATE "/IGNORE:4197")
endif()
# core-api.h 靠它决定是 dllexport 还是 dllimport（只有编 core 这一份目标定义）
target_compile_definitions(ltoolcore PRIVATE LTOOL_CORE_BUILD)
# 界面「关于」那一页显示的就是它（PUBLIC：exe 和插件都拿得到同一个串）。
# AboutPage.cpp 里没定义就直接 #error，绝不让界面显示一个"忘了改的旧版本"。
target_compile_definitions(ltoolcore PUBLIC LTOOL_VERSION="${LTOOL_VERSION_STRING}")

# ---- 预编译头 ----
# 53 个 TU 每个都要啃同一批 Qt 头（QApplication/QWidget/QtConcurrent 展开就是几万行），
# /MP 只能并行"各自啃一遍"，PCH 才是根本不啃。内容见 src/pch.h（故意不含 windows.h）。
# 挂在 INTERFACE 上：exe 和十二个插件都复用 core 编好的那一份，不用每个目标各啃一遍。
target_precompile_headers(ltoolcore INTERFACE src/pch.h)

find_package(LibArchive REQUIRED)

# ---- 7z 的写出后端：7-Zip 自己的编码器 ----
# vcpkg 的 7zip 端口打的是官方那个格式库（CPP/7zip/Bundles/Format7zF），只是文件名叫
# 7zip.dll；bit7z 是它的 C++ 封装（静态库）。换掉 libarchive 的理由只有一个：libarchive
# 的 7z 写出器 LZMA1/LZMA2 恒为单线程，而 7-Zip 把输入切块分给多个核。
# 代价：bit7z 里引用到的符号会拉进 7zip.lib（导入库）→ 7zip.dll 得跟 exe 待在一起。
# 它是按需加载的（实测藏掉它：解压/zip/热键全照常，只有「压缩为 .7z」失败并说明原因），
# 所以漏打包不会让程序起不来，但那条功能就是坏的 —— 下面 POST_BUILD 复制它，
# 打包靠 build\Release\* 一起带走，交付闭环里再点名确认一次。
# 许可证：7zip.dll 是 LGPL-2.1（其中 Rar* 那几个文件另带 unRAR 限制条款），bit7z 是 MPL-2.0。
find_package(unofficial-bit7z CONFIG REQUIRED)

# 每个 ltool 目标（core / exe / 插件）都要的这一套，收成一个函数免得三处漂移
function(ltool_target_setup tgt)
    target_include_directories(${tgt} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # ONNX Runtime 不链接进 core（它是首次用到时下载到 %APPDATA%\ltool\ocr 的那个 dll，
    # 运行时 LoadLibrary + C API 函数指针），编译期只要这一个头
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime)
    if(TARGET LibArchive::LibArchive)
        # 用导入目标：包含目录和 debug/release 的库都由它带
        target_link_libraries(${tgt} PRIVATE LibArchive::LibArchive)
    else()
        target_include_directories(${tgt} PRIVATE ${LIBARCHIVE_INCLUDE_DIR})
    endif()
    target_link_libraries(${tgt} PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::Network
    )
    # 这一串只有 Windows 有：Win32 导入库（外壳/注册表/任务栏/媒体 foundation/直接 3D…）
    # 加 7z 后端。Linux 端的对等物是 WP2 的事，别在这儿猜。
    if(WIN32)
        target_link_libraries(${tgt} PRIVATE
            user32
            shell32
            ole32
            advapi32
            uuid
            runtimeobject
            version
            dwmapi
            powrprof
            rstrtmgr
            iphlpapi
            crypt32
            wintrust
            ws2_32
            winhttp
            mfplat
            mf
            mfreadwrite
            mfuuid
            d3d11
            dxgi
            unofficial::bit7z::bit7z64
        )
    endif()
    target_compile_definitions(${tgt} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /utf-8 /permissive- /W3)
    endif()
    # ---- C++/WinRT：F2 录屏按窗口取帧用的 Windows.Graphics.Capture。
    #      投影头在 Windows SDK 的 cppwinrt 目录里，不在 MSVC 默认包含路径上，要自己加；
    #      RoInitialize / 激活工厂这些符号在 runtimeobject.lib（上面已经链了）----
    if(MSVC)
        foreach(_root "C:/Program Files (x86)/Windows Kits/10" "C:/Program Files/Windows Kits/10")
            set(_inc "${_root}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
            if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND EXISTS "${_inc}")
                target_include_directories(${tgt} PRIVATE "${_inc}")
                break()
            endif()
        endforeach()
    endif()
endfunction()

ltool_target_setup(ltoolcore)

if(MSVC)
    set(_winrt_found FALSE)
    foreach(_root "C:/Program Files (x86)/Windows Kits/10" "C:/Program Files/Windows Kits/10")
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION
           AND EXISTS "${_root}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
            set(_winrt_found TRUE)
            break()
        endif()
    endforeach()
    if(NOT _winrt_found)
        message(FATAL_ERROR
            "找不到 Windows SDK 的 C++/WinRT 头（Include/<版本>/cppwinrt/winrt/windows.graphics.capture.h），"
            "F2 录屏需要它。装一下 Windows 10/11 SDK。")
    endif()
endif()

# ---- 主程序：现在只剩 main.cpp（热键、托盘、单实例、命令行自测入口）----
qt_add_executable(ltool WIN32
    src/main.cpp
    resources/ltool.rc
)
add_dependencies(ltool appicon)
target_link_libraries(ltool PRIVATE ltoolcore)
ltool_target_setup(ltool)

# ---- 工具插件：十二个各一个 dll，产出后拷进 exe 旁边的 ltool-plugins\ ----
#
# 产物先落在 Release 树**外面**（plugin-build）：Qt 的插件部署会往目标的输出目录倒依赖，
# 放进 build\Release 里会被 ltool.iss 那条 `build\Release\*` 一起收进安装包（实测多 58MB）。
# 只有那一个 dll 进 ltool-plugins\ —— 目录名不能叫 plugins，Qt 自己占那个名字（windeployqt
# 会往里面放 imageformats/platforms 那些）。
#
# 每个插件的三件套：<id>plugin.cpp（实现 LToolPlugin 的那层壳）、页面本体（从 src 搬过来的）、
# plugin.json（Id/Name/Desc/Keywords/Api/Color，宿主只读元数据就能画卡片，不用实例化）。
#
# "怎么把一个插件编成 dll"这件事只有 cmake/ltool-plugin.cmake 那一份实现：
# 插件仓库（https://github.com/mateo-ki/ltool_plugin）include 的是**同一个文件**（SDK 快照里那份），
# 差别只在 core 从哪来 —— 这儿链的是同树里的 ltoolcore target，那边链导入库。
include(cmake/ltool-plugin.cmake)
set(LTOOL_CORE_TARGET ltoolcore)
set(LTOOL_PLUGIN_OUT ${CMAKE_BINARY_DIR}/plugin-build)
set(LTOOL_DEPLOY_DIR ${CMAKE_BINARY_DIR}/Release/ltool-plugins)

function(ltool_add_tool id cls)
    ltool_add_plugin(${id} ${cls} ${ARGN} plugins/${id}/plugin.json)
    # 主仓库这一侧还带上 PCH、LibArchive/bit7z 的包含目录与 cppwinrt 头（插件仓库那份由
    # ltool_add_plugin 的 SDK 分支自己补）—— 这一段不复制到第二处去。
    ltool_target_setup(ltool-${id})
endfunction()

ltool_add_tool(repair RepairPlugin      plugins/repair/RepairPage.cpp plugins/repair/repairplugin.cpp)
ltool_add_tool(ads AdsPlugin            plugins/ads/AdsPage.cpp plugins/ads/adsplugin.cpp)
ltool_add_tool(disk DiskPlugin          plugins/disk/DiskPage.cpp plugins/disk/diskplugin.cpp)
ltool_add_tool(network NetworkPlugin    plugins/network/NetworkPage.cpp plugins/network/networkplugin.cpp)
ltool_add_tool(registry RegistryPlugin  plugins/registry/RegistryPage.cpp plugins/registry/registryplugin.cpp)
ltool_add_tool(startup StartupPlugin    plugins/startup/StartupPage.cpp plugins/startup/startupplugin.cpp
                                         plugins/startup/startupselftest.cpp)
ltool_add_tool(user UserPlugin          plugins/user/UserAccount.cpp plugins/user/userplugin.cpp
                                                      plugins/user/userselftest.cpp)
ltool_add_tool(json JsonPlugin          plugins/json/JsonTool.cpp plugins/json/jsonplugin.cpp)
ltool_add_tool(base64 Base64Plugin      plugins/base64/Base64Tool.cpp plugins/base64/base64plugin.cpp)
# 第一个「动作型」插件：右键那几件事（占用分析/粉碎/解压/压缩）。verb 表还在主程序的
# ShellMenu 里 —— 入口留在主程序，按下之后转成动作名交给插件，这是用户拍的板。
ltool_add_tool(shell ShellPlugin         plugins/shell/shellaction.cpp plugins/shell/shellplugin.cpp)
# 第二个「动作型」插件：F2 录屏。Recorder（WGC 取帧 + MF 编码）/ RecordUi（选择窗 + 悬浮条）/
# RecProbe（可行性探针）三样从 src 搬了进来，五套判据（--recprobe/--recq/--rectest/--recordui/
# --memloop）也跟着搬成 extern "C" 导出符号 —— 主程序按名字点名要（见 src/main.cpp 的
# runPluginSelftest）。
ltool_add_tool(record RecordPlugin       plugins/record/Recorder.cpp plugins/record/RecordUi.cpp
                                           plugins/record/WindowPreview.cpp
                                           plugins/record/RecProbe.cpp plugins/record/recordplugin.cpp)
# 第三个「动作型」插件：F1 截图。截图浮层 / 置顶窗 / 标注 / OCR 那一族七个文件从 src 搬了进来，
# 十二套判据（--snaprect / --ocrtest / --ocrlayout / --pinpick / --ocrchain / --pintool / --pindel /
# --ocrget / --ocrpanel / --pinshot / --capgeo / --capflash）跟着搬成 captureselftest.cpp 里的
# extern "C" 导出符号。**只有 CaptureHistory（往期列表）和 CaptureState（浮层开没开）留在 core**：
# 配置页要改存放目录并 reload()、搜索框要在浮层举着的时候先别收起，而 core 不能反向依赖插件 dll。
ltool_add_tool(capture CapturePlugin     plugins/capture/CaptureOverlay.cpp plugins/capture/PinWindow.cpp
                                           plugins/capture/Anno.cpp plugins/capture/Ocr.cpp
                                           plugins/capture/OcrBundle.cpp plugins/capture/OcrPanel.cpp
                                           plugins/capture/OcrPp.cpp
                                           plugins/capture/captureplugin.cpp
                                           plugins/capture/captureselftest.cpp)

# 这三张原来是设置窗菜单里的行（软件 / 下载器 / 定时任务）。菜单收成四行之后它们是普通插件：
# 卡片在插件商店那一面，装了才出现、改名 .off 就没了 —— 和别的插件同一条路，没有特判。
# 调度和下载的实现（TaskScheduler / DownloadManager）留在 core：主程序起来就要用，
# 搬走的只是那张页。
ltool_add_tool(software SoftwarePlugin   plugins/software/SoftwarePage.cpp plugins/software/UninstallPage.cpp
                                         plugins/software/softwareplugin.cpp)
ltool_add_tool(downloads DownloadsPlugin plugins/downloads/DownloadsPage.cpp
                                         plugins/downloads/downloadsplugin.cpp)
ltool_add_tool(tasks TasksPlugin         plugins/tasks/TasksPage.cpp
                                         plugins/tasks/tasksplugin.cpp)
ltool_add_tool(grid GridPlugin           plugins/grid/GridBoard.cpp plugins/grid/GridPage.cpp
                                         plugins/grid/gridplugin.cpp plugins/grid/gridselftest.cpp)
ltool_add_tool(language LanguagePlugin   plugins/language/LanguagePage.cpp
                                         plugins/language/languageplugin.cpp)
ltool_add_tool(clipboard ClipboardPlugin plugins/clipboard/ClipboardPage.cpp
                                         plugins/clipboard/clipboardplugin.cpp
                                         plugins/clipboard/clipselftest.cpp)
# 随机数生成：区间取数 / 不重复抽取 / 随机字符串 / UUID。
# 生成那一层是 Rand::generate() 一颗纯函数，页面和 randselftest 读同一份（判据不许另抄一遍算法）。
ltool_add_tool(random RandomPlugin       plugins/random/RandomTool.cpp
                                         plugins/random/randomplugin.cpp
                                         plugins/random/randselftest.cpp)
# 第一个"后台自己会跑"的插件：plugin.json 里挂了 "Activate":true，启动时由
# PluginHost::startBackground() 按名字叫它（LinuxdoWatch 那个定时器不靠页面活着）。
ltool_add_tool(linuxdo LinuxdoPlugin     plugins/linuxdo/LinuxdoWatch.cpp
                                         plugins/linuxdo/LinuxdoPage.cpp
                                         plugins/linuxdo/linuxdoplugin.cpp
                                         plugins/linuxdo/linuxdoselftest.cpp)

# 交付闭环是 `cmake --build --target ltool`（只编主程序那一棵）。插件目标只依赖 core，
# 不在 ltool 的依赖图上 —— 不点名挂上去，改完页面不重编，ISCC 照旧把**上一个 dll**
# 打进安装包，装出来的还是旧的（构建那一段照样报绿）。
add_dependencies(ltool ltool-repair ltool-ads ltool-disk ltool-network ltool-registry
                        ltool-startup ltool-user ltool-json ltool-base64 ltool-shell ltool-record
                        ltool-capture ltool-software ltool-downloads ltool-tasks ltool-grid
                        ltool-language ltool-linuxdo ltool-clipboard ltool-random)

# ---- Qt 运行库部署：把 exe 所需的 Qt DLL / platform 插件复制到输出目录，
#      避免 "no Qt platform plugin could be initialized" ----
find_program(QT_WINDEPLOYQT_EXE windeployqt
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin"
)
if(QT_WINDEPLOYQT_EXE)
    # --no-system-d*-compiler：D3Dcompiler_47 / dxcompiler / dxil 一共约 20MB，只有
    #   ANGLE/Qt Quick 编译着色器时才用得到，本项目是 Widgets + 光栅绘制，没人导入它们
    # --exclude-plugins：svg 图标插件（Qt6Svg+qsvg+qsvgicon）和 IoT 触摸插件用不到
    add_custom_command(TARGET ltool POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXE}"
                "$<IF:$<CONFIG:Debug>,--debug,--release>"
                --no-translations
                --no-system-d3d-compiler
                --no-system-dxc-compiler
                --no-opengl-sw
                --exclude-plugins qsvg,qsvgicon,qtuiotouchplugin
                "$<TARGET_FILE:ltool>"
        COMMENT "windeployqt: 部署 Qt 运行库到输出目录"
        VERBATIM
    )
else()
    message(WARNING "windeployqt 未找到，exe 目录不会自动部署 Qt 运行库")
endif()

# imageformats/qjpeg.dll 硬导入 jpeg62.dll，但 windeployqt 只管 Qt 自己的产物、
# vcpkg 的 applocal 只跟着 exe 的导入表走 —— 不手动补上，qjpeg 就是个加载不了的死插件
add_custom_command(TARGET ltool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/jpeg62.dll"
            "$<TARGET_FILE_DIR:ltool>/jpeg62.dll"
    COMMENT "copy jpeg62.dll（imageformats/qjpeg 的依赖）"
    VERBATIM
)

# 7z 压缩的后端：exe 硬导入 7zip.dll，它不在 Qt/vcpkg 的 applocal 名单里会漏出去，
# 复制过来（安装包按 build\Release\* 全盘收，所以这一条同时管打包）
add_custom_command(TARGET ltool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/7zip.dll"
            "$<TARGET_FILE_DIR:ltool>/7zip.dll"
    COMMENT "copy 7zip.dll（7z 压缩后端）"
    VERBATIM
)

# 7zip.dll 是 LGPL-2.1（里面 Rar* 那几个文件另带 unRAR 限制条款）、bit7z 是 MPL-2.0，
# 动态链接/静态链接都要把许可证文本跟着软件一起给。vcpkg 已经把原文落到
# share/<port>/copyright 里了，直接搬 —— 别自己抄一份（抄的总会和上游不一致）。
add_custom_command(TARGET ltool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/7zip/copyright"
            "$<TARGET_FILE_DIR:ltool>/licenses/7zip-LGPL-2.1-with-unRAR.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/bit7z/copyright"
            "$<TARGET_FILE_DIR:ltool>/licenses/bit7z-MPL-2.0.txt"
    COMMENT "copy 7zip / bit7z 许可证原文"
    VERBATIM
)
