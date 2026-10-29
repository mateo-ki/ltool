# ltool 详细记录

[README](../README.md) 只回答"有什么功能、怎么编、怎么装、插件怎么写"。
这一份放**为什么这么做、实测是多少、踩过什么坑** —— 全部是从 README 里**整段搬过来**的原文，
一个字没删（搬的脚本 `build/verify/readme_slim.py`，跑完会自己验"原文每段还在不在"）。

## 文件索引怎么保持新鲜（原文）

## 文件索引如何保持新鲜

- **快索引（常用目录）**：启动全量扫一遍后，用 `ReadDirectoryChangesW` 实时监听目录，文件新建/删除/改名秒级反映到搜索结果。
- **深索引（全盘）**：
  - 启动时加载上次的全量快照（`deepindex.dat`），搜索立即可用；
  - **管理员权限运行**：读取 NTFS USN 变更日志做增量更新（只处理变化，实测约 0.5 秒），日志失效时才重新枚举 MFT 全量重建（约 8 秒）；
  - **普通权限**：无法读 USN 日志，保留快照使用；快照超过 `refreshHours`（默认 6 小时）后，下次启动/定时刷新时用目录遍历重建（约 17 秒，后台进行）。
- 手动 `rescan` 命令或托盘"重新扫描"会强制全量重建。

## 构建与依赖的细节（vcpkg / 许可证 / 并行编译与 PCH 的实测）

清单依赖是 `qtbase` + `libarchive` + `bit7z`（见 `vcpkg.json`；bit7z 会带进 `7zip` 和 `ghc-filesystem`，`7zip` 打出来的就是 7-Zip 官方那个格式库，只是名字叫 `7zip.dll`）。

- 走 **vcpkg manifest 模式**（根目录 `vcpkg.json`，baseline 与本机 vcpkg 对齐）；依赖会装进 `build\vcpkg_installed`，本机有二进制缓存时 configure 阶段直接恢复，不需要重编 Qt。首次拉 libarchive 约 1~2 分钟。
- **加新依赖就 `cmake -S . -B build`，别在仓库根上裸跑 `vcpkg install`**：命令行那次不读 `build\CMakeCache.txt`，会把整棵树按默认位置重装一份到 `<项目>\vcpkg_installed`（实测 2.2GB 的 Qt6 副本，构建根本不读它，只会让人以为依赖换了地方）。
- 7z 那条后端的许可证：**`7zip.dll` 是 LGPL-2.1**（其中 `CPP/7zip/Compress/Rar*` 与 `Archive/Rar/*` 另带 **unRAR 限制条款** —— vcpkg 的端口把 Rar 解码器也编进去了，所以读 rar 的那条路受这一条约束），**bit7z 是 MPL-2.0**。两份**原文**由 CMake POST_BUILD 从 vcpkg 树的 `share/<端口>/copyright` 复制成 `licenses\7zip-LGPL-2.1-with-unRAR.txt` / `licenses\bit7z-MPL-2.0.txt` 跟着 exe 一起装（不自己抄，抄的迟早和上游不一致）。LGPL 的"库形式使用"这一条是满足的：7zip 是独立的 DLL，换掉它就能重新链接。
- **别手动往 `D:\vcpkg\installed` 装**：那是 classic 树，本项目读的是 manifest 树，装了也链不上。
- **一个目标里的 53 个 .cpp 是并行编的（`/MP`，见 CMakeLists 顶部），并且有预编译头（`src/pch.h`）**。VS 生成器默认把同一目标内的编译排队串行跑，实测改一次 `src/Utils.h`（47 个 TU 包含它）串行要 **114.7 秒**，加 `/MP` 后 **16.4 秒**；再加 PCH 之后（2026-10-12 同一台机器重测）：**改 `Utils.h` 19.9 → 13.7 秒**、**改 `main.cpp` 12.0 → 10.2 秒**、**全量重建 20.2 → 17.9 秒**、**什么都没改 2.5 秒**（这 2.5 秒是 msbuild 启动 + CMake 检查 + AUTOMOC 扫 53 个 TU 的固定税，改一个 5KB 的小 TU 也要 3.1 秒 —— 也就是说日常这一档基本是固定开销，不是编译）。PCH 里故意不放 `windows.h`：33 个文件要它，但录屏那几个要 C++/WinRT 的头、USN 那几个要 `winioctl.h`，放进 PCH 等于把这些顺序约束强加给所有 TU，而真正的大头是 Qt 那几个模块头。
- 构建后自动执行 windeployqt，把 Qt DLL / platform 插件部署到 `build\Release`；但 **windeployqt 不管 vcpkg 的依赖**，`archive.dll` 及其传递依赖（zlib1 / liblzma / zstd / bz2 / lz4 / brotlicommon / brotlidec）是靠 CMake 的 applocal 复制落到 `build\Release` 的，安装包再按 `build\Release\*` 一起收走。（windeployqt 单跑只有 115ms，不是耗时瓶颈，所以留在每次构建里。）

## 打包与交付闭环（原文：一条命令、逐段报耗时、版本号、档位与体积实测）

一条命令跑完整条链，并**逐段报耗时**（哪一步慢直接看数字，不用猜）：

```powershell
powershell -File build\verify\deliver.ps1              :: 构建 → 打包(normal，6.7 秒/22.2MB) → 停旧实例 → 静默装 → 比 SHA256 → 重新起
powershell -File build\verify\deliver.ps1 -Final       :: 打包换成 lzma2/max（9.9 秒，最小的 21.5MB）—— 出正式包
powershell -File build\verify\deliver.ps1 -Quick       :: 打包换成 lzma2/fast（2.4 秒，26.5MB）—— 只想快点拿到一个能装的包
powershell -File build\verify\deliver.ps1 -NoBuild     :: 跳过构建
```

2026-10-11 实测（本机 20 核）：`build 2.5s / package 9.9s / stop 0.3s / install 2.6s / relaunch 0.2s`，**整条闭环 15.6 秒**。

等价的裸命令（脚本内部就是这几步）：

```bat
cmake --build build --config Release
"D:\software\InnoSetup6\ISCC.exe" ltool.iss
installer\ltool-<版本>-setup.exe /VERYSILENT /NORESTART /SUPPRESSMSGBOXES
```

- **版本号只有一个来源：`ltool.iss` 里那条 `#define MyAppVersion`**。CMake 在 `project()` 之前
  用它解析出 `LTOOL_VERSION_STRING`，再编成 `LTOOL_VERSION` 宏传给 core ——
  安装包文件名、卸载表里的 `DisplayVersion`、界面「关于」那一页显示的版本，是同一个数。
  读不到就 `message(FATAL_ERROR)`，宏没传进来就 `#error`：**宁可构建失败，也不要编出一个
  显示旧版本号的界面**（0.5.0 之前它散在 iss 的 #define 和注释、CMakeLists、README、
  五个交付脚本里共七处，升一次全要手动改）。现在升版本只动那一条。
- **`deliver.ps1` 装完按 SHA256 逐个点名比**：`ltool.exe`、`ltoolcore.dll`、
  `ltool-plugins\*.dll`（零插件包：插件目录只报装着几颗，不再逐颗比哈希）。上一版只比 exe —— 而自 2026-10-13 起
  代码主要在那两个 dll 里，"只比 exe"就是假绿；另外插件目标不在 `ltool` 的依赖图上，
  `cmake --build --target ltool` 根本不重编它们，所以要 `add_dependencies` 点名挂上。
- **压缩档位**（2026-10-12 重测，payload 71.1MB / 34 个二进制；括号里是整条 `deliver.ps1` 闭环）：
  `lzma2/normal`+solid **6.7 秒 / 22.22MB（12.9 秒）← 现在的默认**、
  `lzma2/max`+solid 9.9 秒 / 21.52MB（16.5 秒，出最小正式包用 `deliver.ps1 -Final`）、
  `lzma2/max` 非 solid 7.9 秒 / 22.02MB、`lzma2/normal` 非 solid 6.5 秒 / 22.43MB、
  `lzma2/fast`+solid 2.4 秒 / 26.54MB（`deliver.ps1 -Quick`）。选 normal 的理由只有一条：
  安装包是本地装的，多 0.7MB 看不见，而每轮闭环少 3.6 秒天天摸得到。
  **`CompressionThreads` 不用再试**：这次是用耗时量的（上次判"无效"只看产物 md5 相同，那只能证明输出确定），
  不设 10.7 秒、1 线程 20.9 秒、2/4/8/16 全在 10.4~10.5 秒 —— 默认已经吃满多线程。
  **payload 也剪不动**：按 PE 导入表从 `ltool.exe` 走闭包，71.1MB 里只有 4.0MB 不在闭包上，
  而那 4.0MB 全是运行时按目录加载的插件（`platforms/qwindows`、`styles`、`imageformats`、`tls`）
  和按需加载的 `7zip.dll`；最肥的 `icudt78.dll`（31.58MB，占整包 44%）是 Qt6Core 硬导入的 ICU 数据，
  实测从安装目录藏掉它 `ltool.exe` 直接起不来。
  走 `ltool.iss` 里的嵌套 `#ifdef FINAL / #ifdef QUICK`（**别写 `#elifdef`，ISPP 不认，整个编译当场 abort**，
  而 `deliver.ps1` 现在会查 ISCC 退出码 + 安装包时间戳，不会再当成"打包成功、体积没变"）。
  裸写 `Compression=lzma2` 等于 `lzma2/max`。
- 装完确认装的就是刚构建的那份（`deliver.ps1` 会直接把两边 SHA256 比了并打 `MATCH`/`DIFF`）：

```powershell
(Get-FileHash build\Release\ltool.exe).Hash
(Get-FileHash D:\software\ltool\ltool.exe).Hash
```

- 产物：`installer\ltool-<版本>-setup.exe`，**22.5 MB**（走 FINAL 是 21.8 MB）；安装目录 **约 73 MB**（ICU 三个 DLL 就占 37 MB，动不了）。
- 安装行为：默认按用户装到 `%LOCALAPPDATA%\Programs\ltool`（`PrivilegesRequired=lowest`，无需管理员）；本机实际装在 `D:\software\ltool` —— 同一个 `AppId` 第二次安装会**沿用上次的目录**，所以闭环命令里不写 `/DIR` 也会覆盖到原处；全新机器要指定就加 `/DIR="D:\software\ltool"`（与 `config.ini` 里 `[install] path=D:\software` 一致）。含简体中文向导；可选任务"创建桌面快捷方式""开机自动启动"（HKCU Run，卸载时清除）。
- `CloseApplications=yes`：安装会等正在运行的 ltool 退出，**静默安装可能卡几十秒**，不是失败。
- 卸载不会删 `%APPDATA%\ltool` 用户数据（索引/插件/任务配置/日志保留）；卸载器 `unins000.exe /VERYSILENT`。
- 构建时会先生成 `resources\ltool.ico`（由 `tools\genicon.cpp` 编译的小工具绘制，7 个尺寸）并嵌入 exe 资源，exe/快捷方式/安装包共用同一图标。
- Inno 不会自动删掉"已经不在包里"的旧文件，剪依赖时要同时往 `[InstallDelete]` 里点名，否则升级完安装目录还是老的体积。
- 打包前先跑回归（无界面，一行结论）：`build\Release\ltool.exe --archivetest extract <包> <目录>`、`--archivetest make <输入> <输出> [7z] [档位]`、`--archivetest probe <文件>`、`--archivetest progress <输入> <输出> [7z] [第几次按取消]`；整套 16 项在 `build\verify\archtest.ps1`（zip / 7z / tar.gz 解压、GBK 名字的老 zip、自产包两个方向往返、单文件 `.gz`、"是不是压缩包"的三种判定、硬解纯文本，加上进度/取消三项：7z 全程、7z 第 3 次回调按取消、zip 全程当对照；最后用 `7z -t` 验我们自产的两个包外部工具也认）。脚本和临时产物都放 `build\verify\`，`build\` 已在 `.gitignore` 内。

## 打包的等价裸命令（deliver.ps1 内部就是这几步）

等价的裸命令（脚本内部就是这几步）：

```bat
cmake --build build --config Release
"D:\software\InnoSetup6\ISCC.exe" ltool.iss
installer\ltool-<版本>-setup.exe /VERYSILENT /NORESTART /SUPPRESSMSGBOXES
```

## CI 的细节与踩过的坑

### GitHub Actions：推标签就出安装包、发 Release（`.github/workflows/build.yml`）

- **CI 里从 `vcpkg list` 取版本号：按列取，别拿正则抠数字。** 插件仓库那条 CI 第一次红就红在这：
  `grep -oE '[0-9][0-9.]*'` 对整行找数字，第一列 `qtbase:x64-windows` 里的 **64** 先命中，
  `head -1` 取走它，于是把一次正常装好的 6.10.2 判成"版本不对"。正解是
  `vcpkg list | tr -d '
' | awk '/^qtbase:/{print $2; exit}'`（再把 `#3` 那种端口修订号从 `#` 截掉）。
  `tr -d '
'` 不是装饰：vcpkg.exe 在 git-bash 下输出的是 Windows 行尾，带着它比较永远不等。
- **"取不到那一行"和"版本不对"要分两条报错**：前者是上一步的装依赖没真装上，后者是钉的 commit 和本机树不一致，
  混成一句话下次就得重新查一遍。
- **本地能把这一段单独验掉**：把那个 step 的脚本体导出成 .sh，`PLUGINS_VCPKG=D:/vcpkg QT_MUST_BE=6.10.2 bash …`
  跑三种情况（对上 / 对不上 / 根本没有 vcpkg），比推上去等一小时快得多，也才知道它真的会红。

一条 job（`windows-2022`）从头到尾就是本机那条链的翻版：**装依赖 → 编 Release → 跑无界面回归 → ISCC 打包 → 传 artifact →（只有标签推的 run）发 GitHub Release**。

- **触发**：推 `v*` 标签 = 打包并发布；推 `main` 或手动 `workflow_dispatch` = 只打包不发（所以"这版能不能出包"随时可以验，不必先承诺发版）。
- **预发和稳定就靠标签本身分**：`v0.8.1-beta.1` 建的 Release 自动带 `--prerelease`，`v0.8.1` 是稳定版。界面「检查更新」打的是 `/releases/latest`，而 **GitHub 的 `latest` 实测会跳过 draft 和 pre-release**（拿 `microsoft/PowerToys` 对的：列表里最新是 `v0.102.2862.0 pre=True`，`latest` 回的是 `v0.101.2362.0`）—— 所以预发不会推给走稳定通道的人，客户端一行代码都不用改。配套的两条硬规则：`compareVersions` 认后缀且**同数字时带后缀的那个更旧**（`0.8.1-beta.1 < 0.8.1`，装预发的人因此看得见正式版可升；以前整串按 `.` 拆段，`1-beta` 读成 1、尾巴 `.1` 还多一位，预发被当成比稳定新）；`project(VERSION ...)` 只吃纯数字段，所以 CMake 把 `0.8.1` 交给 `project`、把完整的 `0.8.1-beta.1` 交给 `LTOOL_VERSION` 宏（界面显示哪个由宏说了算）。
- **标签必须和包里的版本号对上**：`v<尾巴>` 不等于 `ltool.iss` 里的 `MyAppVersion` 就当场失败，杜绝"发出去的安装包写着上一个号"。
- **依赖用清单模式**（和本机一样）：仓库根有 `vcpkg.json`，装 `qtbase`/`libarchive`/`bit7z`；那棵 vcpkg 树先 `fetch` 到 **baseline 那个 commit**（`--depth 1` 只 clone 分支 tip 会找不到 baseline，那样清单模式直接拒绝）。baseline 从 `vcpkg.json` 里 `grep` 出来，**不在 yml 里抄第二份**，免得两处漂移。
- **装依赖和配 CMake 是两步**（2026-10-15 拆开，为了"慢在哪一眼看得见"）：`vcpkg install --x-install-root=build/vcpkg_installed --clean-after-build` 单独一步，日志就是 `vcpkg-install.log`；配 CMake 那一步因此只对着已经摆好的树，几秒过。vcpkg 树本身改成 `git init` + `git fetch --depth 1 origin <baseline>`（原来那份全量 clone 要拉 300MB+），缓存里已经有 `vcpkg.exe` 就跳过 bootstrap。`--clean-after-build` 清的是几十 GB 的**源码/中间树**，不是 `build/vcpkg_installed`，所以它和下面那条缓存不打架。
- **缓存四样，全在工作区里**（`.ci/vcpkg`、`build/vcpkg_installed`、`.ci/vcpkg_downloads`、`.ci/vcpkg_archives`）：`actions/cache` 对 `GITHUB_WORKSPACE` 之外的路径**静默不保存**，放外面等于每次重编 Qt（约一小时）；只缓存 vcpkg 树而**不缓存安装产物**也一样白干 —— 干净检出后 `build/` 是空的，那棵装了 qtbase 的树还得重解甚至重编。键带 `vcpkg.json` 的哈希，`restore-keys` 回退到旧键，加一个依赖时没动的 qtbase 从二进制缓存里秒级复用。二进制缓存双份：`VCPKG_BINARY_SOURCES=clear;x-gha,readwrite;files,<工作区目录>,readwrite`（云端那份要 `actions: write`；本地文件那份跟着缓存一起存，x-gha 挂了还有它）。
- **`timeout-minutes` 从 360 降到 180**：360 那份余量是把"冷缓存 + 从源码重编 qtbase"当成每天要跑的常态。量到的两个数：老形状那条「配置（vcpkg 在这一步按清单装…）」被下一次推送顶掉时**已经跑了 22.7 分钟还没结束**；新形状第一次跑（缓存 miss、14 秒就撞上面那条二进制缓存报错）整条 job 34 秒。180 是给"冷缓存真要从源码编一遍 qtbase"留的预算，不是量出来的常态。
- **生成器是 Visual Studio 17 2022，不是 Ninja**：CMakeLists 把插件拷进 `${CMAKE_BINARY_DIR}/Release/ltool-plugins`、`ltool.iss` 按 `build\Release\*` 收包 —— 换成 Ninja 就没有那个 `Release` 子目录，配置能过、包是空的。也因此**不用** `ilammy/msvc-dev-cmd`（它会把 `VCPKG_ROOT` 指到 VS 自带那份 vcpkg，后面静默换掉缓存好的树）。
- **本地并发和 CI 并发不是一回事，别互相抄**（2026-10-15 本机 20 逻辑核全量重编实测，四种形状各跑一遍）：默认（无 `--parallel`，`/MP` 吃满）94.0s → `--parallel 8` **38.4s** → 12 = 37.5s → 16 = 36.5s（8 就到平台期）；而 `--parallel 8 + -p:CL_MPcount=2` 反而 68.2s、`16 + CL_MPcount=2` 68.4s —— **限每颗工程的编译线程数在这台机器上是净损失**。所以 `deliver.ps1` 用 `--parallel 8`，CI 那台 2 核的才压到 `--parallel 4 + CL_MPcount=2`。
- **`VCPKG_DEFAULT_BINARY_CACHE` 指着不存在的目录时 vcpkg 不是"当没缓存"，是当场拒绝干活**：`error: <defaults>: error: Environment variable VCPKG_DEFAULT_BINARY_CACHE must be a directory`（run 38648353323 就这么红在 14 秒，因为缓存 miss 时 `actions/cache` 什么都不还原，那棵树根本不在）。修法就是装依赖那一步 `mkdir -p` 把归档目录和下载目录都建出来。本机拿一份只依赖 zlib 的清单**双向验过**：目录不存在 → 一模一样那句 error；先 `mkdir` → 2/2 装完。注意 `--dry-run` 量不出来（它不走二进制缓存那条初始化），要真装。
- **CI 编 Release 的并发压到 `--parallel 4` + `-p:CL_MPcount=2`**：`windows-2022` 只有 2 核 / 7GB，原来 `--parallel 8` 乘上 CMakeLists 里那条全局 `/MP`，同时能起来十几个 `cl.exe` 和好几个 `link.exe` 抢那点 CPU 和内存 —— 抢出来的不是快，是换页和 I/O 堵塞。总并发压在 4~8 之间。
- **MSBuild 那个开关只能写 `-p:`，不能写 `/p:`**：这一步是 `shell: bash`，MSYS 会把 `/p:CL_MPcount=2` 当 Windows 路径处理掉，MSBuild 收到的是 `p:CL_MPcount=2`，当场 `MSB1008 只能指定一个项目`（本机 2026-10-15 实测撞的，和 `/DFINAL` 那条同一个坑）。同理，`tee` 之前都要 `set -o pipefail`，否则最后那段管道把退出码洗成 0，编坏了也不会红。
- **自测跑在 offscreen 平台插件上**：runner 上能不能拿到窗口站不作保证，而这条判据要的是"控件建得出来、布局算得对、点了有反应"。`windeployqt` 不带 `qoffscreen.dll`，所以那一步从 vcpkg 树里**借一颗**进 `build/Release/platforms`；`ltool.iss` 的 `Excludes` 点名排除它，**安装包收不到这颗测试专用的插件**（本机验过：装完 `platforms\` 里只有 `qwindows.dll`）。
- **每个旗标都必须有输出，空输出算失败**：这套自测里 `--smoke` 曾以"0 字节 + 退出码 0"的形状哑过（命令行被单实例守卫转交给已常驻的实例），CI 这边直接把它判成红。CI 跑的是 `--smoke --plugintest --rowmenu --username --about --cfgcheck --appdirs --pinyin --diskbranch --archivetest`；**跳过的点名写进日志**：`--capgeo --capflash --recq --rectest --recordui`（要真屏幕/DWM 会话）、`--searchtime`（整盘扫描太慢），别让"CI 绿"看着像"这些也过了"。（`--mfttest` 原来也在"要管理员所以跳过"那一串里 —— 2026-10-17 查明 runner 是提权的，已挪进真跑那一串。）
- **发出去的包走 `-DFINAL`**（本机同一棵树实测：normal 22.51MiB / 6.8 秒，FINAL 21.79MiB / 10.3 秒 —— 省 0.72MB 多花 3.5 秒；本机的 `deliver.ps1` 不带它，因为每轮闭环少 3.5 秒是天天摸得到的）。开关用**短横不用斜杠**：那一步是 bash，MSYS 会把 `/DFINAL` 当路径转换掉，ISCC 于是报 `You may not specify more than one script filename`（本机实测撞过）。
- **发一版就三步**：① 改 `ltool.iss` 的 `MyAppVersion`（要预发就写 `x.y.z-beta.N`）；② 提交并推 `main`（这一步之后 workflow 文件才在默认分支上，**标签早于 workflow 推上去是不会触发的**）；③ `git tag v<和号一模一样的串>` 再 `git push origin v<那个串>`。第 ③ 步是对外的，由人按下。

## 自测旗标逐条（每面旗量到的数、期望值为什么改过）

## 自测旗标逐条

上面那张表只写"这面旗标验什么"。下面这段是前一版 README 里「命令行参数」那**一整条**，
原样搬过来的 —— 每一面旗标量到的数、期望值为什么改过、反向验怎么做到的，都在里面。

`--show` 启动即显示搜索框；`--smoke` 无界面自检；`--mfttest` 需管理员，诊断 MFT 枚举；`--searchtime 词1 词2` 无界面，加载索引后逐词打印深索引搜索耗时与前 3 条结果（**272 万条下现在 9~38ms/次**：两个字的词 9ms、`chrome` 38ms。改之前是 540~680ms —— 每命中一条就 `fromUtf8` 造一条整路径 QString 收进一个大数组，而绝大多数命中根本活不到前 20 名。现在按 (来源, 分数, 名字长度, 路径) 分 8 个桶、每桶只留 2×limit 条候选，比大小时不造 QString（UTF-8 的字节序就是 BMP 字符的 UTF-16 序，只有含 emoji 那类 4 字节序列才退回 QString 比），首字节用 memchr 跳，每个卷的条目区间切成段丢给最多 8 个线程各扫各的再并桶）；`--searchdump 词1 词2` 无界面，把每个词的前 20 条结果**按顺序整条打出来** —— 改搜索实现最怕「快了但顺序变了」，所以这条就是改前改后各跑一遍、拿 diff 当判据的入口（2026-10-12 那次重做：同一份实现连跑三遍互相比对一致、和老实现逐行一致，只有索引条数那一行随磁盘变化）；`--disktime` 无界面，A/B 对比两种磁盘遍历（`DiskScan` 一次建树 vs `QDirIterator`）在 C 盘上的耗时并列出前 5 大目录，还会把 `DiskScan` 自己那三段（枚举 / 合并 / 排序）连同文件数、目录数一起打出来 —— 加这三段是因为第一次拿它得出了「新遍历比老的慢 0.7×」这个**假结论**：两趟共用一个进程，先跑的那趟把目录项喂进了系统缓存，比的其实是缓存冷热不是实现高低；改成冷/冷、热/热各一轮之后实测 **DiskScan 14.5s（热）/ 20.2s（冷）vs QDirIterator 26.3s（热）/ 29.4s（冷），热态 1.8×**（这台机器 48.1 万个目录 / 142 万个文件 / 254.5GB；三段拆分里"扫盘"占 20198/20244 ms，合并建树 31ms、排序 5ms —— 时间全在枚举上，建树和排序根本不是瓶颈，所以优化方向不在那儿）；`--diskscale` 无界面，拿环境变量 `LTOOL_DISKTHREADS` 把同一棵树按 1/2/4/8/16 线程各扫一遍量扩展性（**连跑两遍**：1 线程 22.0 / 22.9 秒，2~16 线程落在 12.9~17.6 秒之间，第二遍里 4/8/16 的顺序还会互换 —— 也就是加线程最多换到 1.5× 上下、且不稳，目录枚举在这台机器上早就不吃 CPU 了；上一版这里写的「1→4 线程 3.2×」拿第二遍复测**没复现**，已按实测改掉）。结论：**加线程救不了首扫**，所以「磁盘分析太慢」能做的两件事是别重复扫、边扫边出数）；`--diskbranch` 无界面，验「边扫边报的一级分支大小」和最后那棵树是不是同一回事（4 条：分支数齐、每个分支报的数和定稿树逐个对上、一级分支之和 = 全盘减去卷根直属、第一个分支在整趟结束之前就出数 —— 实测 20 个分支 206ms 就出第一个、整趟 13.1s）：这条必须能红，孩子算漏或算两遍都会被第一条抓到，而界面上先给用户看的那几个数就是它报的；`--diskui` 无界面（慢，要真扫一整盘，所以不进 `--plugintest`），把磁盘页那条链整条走一遍（7 条，判据写在插件 `DiskPage.cpp` 里 —— 它要点真按钮、看真行卡片，主程序链接不到那些类型）：控件都在 → 点「整理」→ **途中状态行报了「一级分支已扫完 N / 20」且列表已经摆了行卡片**（实测 1.56s 摆出 13 行，不是空白干等到 16s）→ 定稿后状态换成树上的数、并写下这次扫描的时间 → 换到没扫过的盘列表让位（不拿上一个盘的数当结论）→ 换回来**直接用缓存**把行摆出来（没重扫）；`--username` 无界面验「Windows 用户名」那一页（30 条，**一次都不真的改账户/建账户** —— 那是不可逆级别的动作）：`psQuote` 把单引号翻倍（拿一条注入串试：`x'; Add-LocalGroupMember ...` 拼出来必须首尾各一个引号、里面没有裸单引号）；名字合法性（空/带斜杠/带引号/带换行/账户名 21 字 全被拒，20 字和中文显示名通过）；拼出去的脚本长什么样 —— ADSI 那串路径必须是**拼出来的** `"WinNT://" + '机' + '/' + '号' + ',user'`（整串塞进双引号里再套单引号，单引号就成了名字的一部分，实测错过一次）、新建用户按 SID 找管理员组而不是写死组名、不勾管理员就**整段不加**；真实读取（只读）：当前账户名非空、SID 以 `S-1-` 开头、用户目录真在盘上、查一个不存在的账户得回 `exists=false`；页面三颗按钮**从布局可达**（不是 `findChild` 数得到就算）；再加工具页结构判据：`embedded>=0` 的槽位互不重复且是 0..N-1 连着（`ToolsPage` 的占位数正好盖住，卡片指向一个没建的页就会被这条抓到）。脚本里那 15 个 cmdlet 参数名（`-PasswordNeverExpires`/`-AccountNeverExpires`/`-NewName`…）是拿 `build/verify/psparam.ps1` 在本机 `Get-Command` 逐个验过的，不是凭记忆写的；`--rowmenu` 无界面，验磁盘页两栏列表（「可清理位置」和「占用分析 · 本层目录」共用同一行卡片）右键该弹什么：存在的目录只有一项、叫「打开文件夹」、点得动，且**这项的 data 就是这一行自己的目录**（点开之后界面拿的就是它，所以这条等于验“开的是它、不是别的”）；目录已经不在了就只剩一项灰的、写「这个目录已经不在了」；再加两条几何/结构断言（行卡片的 contextMenuPolicy 是 CustomContextMenu，下面那行路径是它自己的子控件 —— 右键点在路径那行上也要冒得到）。判据写在 DiskPage.cpp 里（行卡片藏在那个文件的匿名 namespace），菜单构建拆成了独立的 `makePathMenu`，因为真按一下就会拉起资源管理器，那一下不能放进自测；`--archivetest extract|make|probe|progress` 无界面跑压缩包读写（回归用，见上；`make` 的第 5 个参数是 7z 档位数字 0-9，zip 那条不吃；`progress <输入> <输出> [7z] [第几次按取消]` 是专门验进度与取消这两条只有界面在用的线的：分母准不准、done 单调不减、回调返回 false 之后有没有当场停下、半成品 `.part` 有没有被删干净）；`--gridtest` 无界面验「图片排版」那张板（34 条，判据在 plugins/grid/gridselftest.cpp）：格数只截断不重排（九宫摆满退回四宫还是那四张）、四格收四张其余算拒收、后缀不是图或盘上没有就一个都不收、拖到空格=挪过去、拖到有图的格=交换、空格拖不动、✕ 只空那一格；合成验到像素（150x150 = 60*2+10*3、四边和缝是背景色、每格中心就是那张图的颜色、空格子是背景色不是黑的）；**板上预览和出图必须同一块构图** —— 拿一张 120x40 的红|绿|蓝横条，中间那条正好 40 见方，整张压扁就会漏出红和蓝（compose 两条 + 离屏 grab() 两条），反向验过：把居中裁正方形换成整张映射，这四条当场点名；再加两条几何（板子再扁格子也是正方形、整块居中而且真留了白 —— 只断言左右对称的话两种写法都绿，所以量的是留白有多大）和一条探针自检（✕ 角标必须比图本身暗，防止离屏 grab 画出空图把全部像素判据一起骗绿）；坏文件那三条验的是界面说实话：后缀对的坏图先进格子（人得看到自己拖错了）、合成时那一格留背景色、画得和空格子一样素（不是一块黑）；`--gridtest` 这一轮又加 5 条盯「生成宫图先弹框展示效果图」：弹框里「保存这张 / 取消（不存）」两颗都在布局里、那行字 == `previewCaption`（写的是**存出去的全尺寸**和落点目录，不是屏上这块缩图）、缩图真有像素、按「取消」这个框回绝；预览**只缩不放大**（图本来就小就按原尺寸摆，放大出来的糊图会让人以为存出去的也是糊的）。顺序那条和它的反向验见上面「自测旗标逐条」的 `--gridtest`；`--linuxdotest` 无界面验「linux.do 盯帖」离线那半条（33 条，判据在 plugins/linuxdo/linuxdoselftest.cpp，跑在沙箱 APPDATA 里因为它要写 linuxdo/* 那一节）：解析（id 乱序不重排、没标题那条丢掉、URL 按 id 算、Cloudflare 那张 HTML 挑战页解出 0 条、空字节不崩）、命中（标题撞 / 只有标签撞 / 撞两个词只算一条 / 上一轮报过的不再报 / 空词不把全部帖子都算命中）、前缀拼法（带不带尾斜杠、两边带空格、留空=只用直连）、要不要弹那一句的决定（第一次只播种不弹、没命中不弹）、配置（默认开着 / 10 分钟 / 三个词 / 中转前缀就是实测那条；0 分钟夹到 3、9999 夹到 240、关键词去空去重 trim、三样都真落盘、再建一张页回填的就是盘上那份）、页面九件控件**从布局可达**（数 findChild 不等于摆在页面上）。反向验过：把 plugin.json 的 `Activate` 摘掉，`--plugintest` 那条「牌和导出的符号一致」当场红成 `got=[0/1]`。`--linuxdonet` 是另一面旗（6 条，要联网、不进 CI）：真去取一趟，实测 4954ms 拿到 30 条、状态行说出走的是中转、第一趟只播种。**这条路是量出来的不是猜的**：直连 linux.do 在这台机器上 DNS 被污染（解到 Facebook 的 IP），走系统代理能到 Cloudflare 但非浏览器客户端一律吃挑战页 —— **带着浏览器导出的 cf_clearance + 完整 Chrome 请求头也一样**（它绑 TLS 指纹），`latest.json` / `session/current.json` / 单个帖子三条实测全 403，所以给 cookie 也换不来登录态；只有 `https://r.jina.ai/<原地址>` + `X-Return-Format: text` 能拿到真 JSON（全站最新 30 条、categories 12 个分类、单个分类流 30 条，4.5~7.3 秒）。发帖速率实测 0.64 帖/分钟（一页 30 条跨 47 分钟），所以 5~30 分钟一轮漏不掉，间隔下限卡 3 分钟。**「盯帖没效果」的根因就是这个**（2026-10-16 实测）：那 90 条新帖里标题含「福利/女装/公益」的 **0 条**，但在分类 36「福利羊毛」里的有 **4 条** —— 「福利」是**分类名**，不在标题里，只比标题和标签就永远 0 命中。现在 `matchNew` 也比分类名（`categories.json` 缓存进 config.ini，一天一趟，拉不到就自己补拿，页面上那行「分类表：已拿到 N 个 / 还没拿到」把降级状态写出来）；`--linuxdonet` 里那条端到端最硬：拿真那一页里**帖子最多的分类**的名字当关键词，必须撞到、且撞到的确实在那个分类里（实测 30 条里「搞七捻三」25 条，撞到 25 条）。**两个自己造的坑也一并修了**：① 启动时帖子和分类表各抓一次、表为空时两条路径又重复触发 = 三个并发请求，把免费中转打到 25 秒全超时（日志里三条一模一样的「直连不通」）—— 现在**一次只飞一趟**，帖子这趟回来了才轮到表（`m_wantCats` 排队）；② 表是这轮**之后**才到的话，那 30 条就永远没被分类名比过 —— `advanceSeen()` 因此规定：没表的时候**不推进 lastSeen**，下一轮拿到表再把同一页比一遍（宁可晚报不要漏）。真机第一发命中：16:25:19「新命中 1 条 · 分类表 12 个」，撞的是「福利」在分类「福利羊毛」，标题是 workbuddy补偿已到位（id 3033129）—— 正是只比标题时永远抓不到的那一类；`--linuxdotest` 另有一条反向断言：分类表传空 → 就是 0 命中（老行为）；`--pinyin` 无界面验拼音匹配（52 条断言）：表和码点必须对齐（错一位就会把"垃圾"当"任务"）、多音字（应用那一档 chongqing/zhongqing 都算，直达那一档只认表里第一条 —— `An` 被「广告」的生僻读音抢页那次收窄的）、全拼允许中段而首字母只认开头、纯英文名不掺和这档、单字母不启用、耗时（810 个名字首次展开 979us、缓存后扫全表 79us/次），**最后真建一个不显示的启动器窗口往输入框里打字**，验 `mumumoniqi` 能搜到本机的"MuMu模拟器"、`cipanqingli`/`cpql` 能直达"磁盘清理"，再加 `renwu` 那两条（**应用节必须在**、**工具直达那一节不能把应用顶掉**）和 `qingli` 一条（取整不伤直达：它不是「磁盘清理」的开头，但是关键词「清理」的开头），再加 2026-10-16 那四条盯"直达不清场"（`cpql` 一条盯"工具还在第一行"、一条盯"末尾那条「用搜索引擎搜索」加得回来"；`disk` 两条是 env-gated，索引里没有 `Disk*` 应用就点名跳过 —— **别拿 `python` 当这一类探针**：他 `webs.json` 里显式配了 `python`/`rust`/`cpp`/`npm`/`pypi`/`qt`/`docker` 这些网站关键字，那几条走的是 `webKeywordWins`「整节让路」，根本到不了直达这一站，用它测等于测错东西）。**这四条里真正盯得住"有人把 return 加回来"的只有两条**（「用搜索引擎搜索」那条 + `disk` 的应用节那条）—— 反向验就是这么定的：把 `rebuildList(); return;` 塞回去重编，`小节` 立刻塌成 `[工具]` 单节、`disk` 应用节=0，红的正是这两条，而"排第一"那两条照绿（独占时工具本来也排第一，它们不区分这件事，别当清场探针）。另：**反向验之前必须先确认补丁真进了二进制** —— 我第一次拿 bash heredoc 里的 python 改 `SearchWindow.cpp`，脚本自己打印了 "patched"、`assert` 也过了，但文件 mtime 没动、`grep` 拿不到那行字，那一轮"红了几条"全是拿旧 dll 测出来的（作废）。再加 `qq` 两条 —— 半角 `qq` 必须出应用节（这台机器上 `apps.json` 里 `QQ` 排第 1，复算过排序不是问题）、**全角 `ＱＱ` 必须出同一个应用节**（这条第一次跑是红的：偏移写成 `0xFEE` 把 `Ｑ` 变成了 `3`，端到端断言当场抓到，所以没再单加一条纯函数断言）；顺带给 harness 加了一个把结果里出现的小节原样打出来的探针，查「某个词为什么搜不到 X」时看它比看断言红不红有用；`--archspeed [每文件KB] [文件数]` 无界面量压缩包读写的吞吐（默认 4096×15=60MB 伪日志，另可 `--archspeed 2 2000` 量海量小文件）：我们 vs 系统里的 7-Zip，zip/7z 两个方向各打一行 MB/s 和产物大小，并把 7z 的 1/3/5/7/9 档整条曲线跑一遍，**每档都验"我们自己的读取器解得回来 + `7z t` 退出码 0"**（换档位最怕快是快了、别人打不开），夹具全在 `%TEMP%\ltool-archspeed`；`--ocrtest <图片>` 无界面打印图里认出的文字与耗时；`--ocrlayout <图片>` 无界面打印每行每词的包围盒和切出来的字格（划词的坐标就靠它验）；`--pinpick <图片>` 无界面，把鼠标事件直接送给一个不显示的置顶窗，跑「图上划词」的 9 条断言（拖半行/单击整词/跨行/词内半个字/跨词空格/Enter 全部/Esc 退出），跑完归还剪贴板；`--recprobe [ms]` 无界面量录屏的地基：系统里有哪些视频/音频编码器、MP4/H.264 写 30 帧再读回来要多久、`Windows.Graphics.Capture` 按窗口取帧能不能用（会弹一张自己的小窗 1 秒多）；`--recq` 无界面验录屏那两个新选择真的进了编码器（13 条）：`Recorder::encodedSize` 的五种画幅（不缩 / 等比缩 / 奇数先取偶再缩 / **只缩不放大** / 结果永远偶数）、码率随画幅下降，然后**拿真编码器录四局自制卡**并 `inspectMp4` 读回来比：不缩那局文件画幅 = 640x400，选 ≤320 那局文件画幅真是 320x200 且**亮度网格仍然上亮下暗**（缩小不许把画面翻过来），同内容同长度缩小那局文件必须更小；画质两局用**逐像素随机噪声**（第一版用的是 24x12 大块，放大后太平滑，流畅和超清只差 7% —— 那条断言测的是内容不是档位，所以改成噪声并要求"至少大 10%"，实测 +26%~37% 两遍稳定）；`--rectest [秒]` 无界面录一个自造的会动的卡片，然后 `inspectMp4` 把文件解回来数帧（帧数/时长/有没有音轨/画幅），**末尾再录 1.5 秒整块屏幕**（`CreateForMonitor` 那一路：能开起来 / 采到帧 / 全程无错 / 收尾后 `running()` 回 false / **解出来的画幅正好等于这块屏幕的物理尺寸取偶** / 时长对得上），画面变化靠的就是那张每 30ms 换数字的卡，不合成鼠标；`--recordui [秒]` 把 F2 那条链整条走一遍（45 条断言，2026-10-14 加了「画质/画面」两颗下拉框那 7 条，2026-10-15 又加 12 条盯选择窗这三件事：「开始录制」那颗存在且摆在布局里、**单击和双击条目都不再开录**（合成一对鼠标事件送给列表的 viewport，数 `picked` 发过几次）、按那颗才 emit 且 emit 的就是当前选中那条、**按住标题那一行能把窗拖走**（sendEvent 给这个窗自己，pos 必须正好挪过 (60,40)）、**选中一条就真拿到 WGC 帧**（指着那张每 30ms 换数字的自造卡测，它一直在重绘所以这条不飘）、预览尺寸 == `Recorder::encodedSize(采集, 当前档)`、换「画面」档之后预览跟着换成 ≤1280 的画幅（拿整块屏幕量，那张卡只有 360 宽任何档都不缩）、窗一收起就把取帧那一路放掉。选择窗自己用 `WA_DontShowOnScreen` 摆成"可见"——不往他桌面上贴像素，但布局和 isVisible 全走真流程。反向验过：把三处实现逐个掐掉（重新接上 itemActivated / 拖动的 press 短路 / refreshPreview 直接 return），红的正是那 6 条，一条不多一条不少）：在不在布局里、四档齐不齐、**当前档 == 配置里存的档**、程序化改档之后立刻落盘、**再建一个选择窗回填成刚选那两档**、跑完把用户原来的档还回去）：列表第一条必须是「整个屏幕」且带着“这是屏幕不是窗口”的标志位、第二条是普通窗口 → 悬浮条在计数 → 用 `click()` 程序化按「停止」→ 报已保存 → 文件能解出帧 → **再开一局按「不要了」：条上写“已取消，没保存（丢掉 N 帧）”，并且断言 `QFile::exists` 说那个 MP4 真的不在磁盘上了**（判的是文件，不是它自己写的那句话）→ **选择窗的四条退路**（「✕」那条是 `findChild` + **布局可达性** + `click()` 三步：
    按钮存在、真的摆在布局里、按下去窗就收起 —— 数得到控件不等于摆在页面上，见「配置页」那条教训）、往列表里 sendEvent 一个 Esc 键能收起（`QLineEdit` 自己会吃掉 Esc，所以产品里是给输入框和列表两个子控件都装了事件过滤器）、给它派一个 `WindowDeactivate` 能收起（=“点到别处就自己收起”那条，不合成鼠标）、`RecordUi::startViaPicker()` 连按两次能收起（第二次就是 F2 的取消分支，走的是全局那个 picker 而不是测试里这个局部对象）；后面还是改大小会收尾、一次失败只写一条日志、自己收尾之后 F2 还能再开一局那几条；`--pindel <图片>` 无界面，跑「右键往期缩略图删除」的 10 条断言（往期计数 / `captures` 里的文件真没了 / 删的正是当前那张时自动挪到邻居 / 越界不炸），它把 `APPDATA` 临时改到 `%TEMP%\ltool-pindel`，不会碰你真实的往期截图；`--ocrpanel <图片>` 真开一次"提取文字"结果窗；`--ocrget` 无界面，只把 OCR 引擎那三个文件下到 `%APPDATA%\ltool\ocr`（每 0.5MB 报一次进度）；`--ocrchain <图片>` 把「首次使用」那条整链跑一遍（11 条断言：沙箱里没有引擎 → `available` 说要下载 → 下载 → 进度只朝前且到 100% → 下载中再要一次不会重开 → 文件齐 → 当场认得出那三行字），它把 `APPDATA` 临时改到 `%TEMP%\ltool-ocrchain`，会真下 26MB、但碰不到你真实的那份；`--memloop [轮数] [宽x高]` 无界面，用来回答"做完某事内存没回落"：每轮先解压一个自造的 32MB 包（随机字节，压不动）再录一段指定尺寸的窗口，逐步打印私有字节 / 工作集 / 句柄数的增量（判读：私有随轮数线性涨=真泄漏；涨两轮封顶=分配器留着页复用，不是泄漏）；`--toggle` 冷启动跑，6 条断言核对"按 Alt+Space 到底出不出来"（冷状态呼出且必须在某个显示器内 → 最小化后再按必须拉回来 → 失焦自动收起 → 收起后再按能再呼出 → 在前台时再按才收起），它会真的把启动器弹出来一两秒；`--pathcfg` 无界面，验配置页那两行存放目录真的管用（沙箱 `APPDATA`，9 条断言：默认值 → 写 `paths/captures` → 新截图落进新目录且不碰老目录 → `CaptureHistory::reload()` 后往期还是那一张 → 录像目录能建能写。其中"光问一句不该把 `captures` 建出来"那条**判的是这一问之前之后有没有变化**，不是"它在不在"：装在软件目录那份的 `captures` 本来就有用户真实截图，2026-10-15 对着安装包跑就红了一项（build 那份绿、装出来的那份红），改成比较开跑前后的状态之后两边都绿、而"光是问一句就 mkdir"照样能红）；`--appdirs` 无界面，验配置页「应用搜索的额外项目」那条路（沙箱 `APPDATA`，12 条断言：层数 0/1/2/3 各收到哪些 exe、中文目录名要进得去、卸载器不算、直接指到一个 exe 上也收、目录不存在只是跳过，**再加过滤那两条**：一棵造好的夹具树里 `gui.exe`（真 PE 头、Subsystem=2）该收，`tool.exe`（Subsystem=3 控制台）、`FooUpdater.exe`（助手名）、`7.4.1/Inside.exe`（版本号目录）、`locales/X.exe`（运行时目录）都不该收；而**点名指到 `tool.exe` 时必须收到它**；再加一条「同目录已有主程序」：夹具里点名 `gui.exe` 之后，同目录的 `gui2.exe`/`notepad.exe` 都不该再被自动收（期望值我第一次写错了 —— 写成还留着 notepad，红了之后确认是**规则本来就该这样**、不是代码错，改的是期望值）。`--pinyin` 那边还有一条端到端：**搜 `qq` 的第一行必须是 QQ 本人**（判的是 `resultAt()` 里的标题，不是「有没有应用节」—— 空键那个 bug 的形状正是有节、顺序错）。PE 头是测试自己按布局拼的 512 字节镜像。**偏移是实测出来的：Subsystem 在可选头 +68**（本机拿 74 个真 exe 验的，+70 紧跟着就是合法的 DllCharacteristics 掩码）—— 第一版按记忆写成 +44，**实现和自造夹具一起错，那条测试当场白过**；所以夹具之外又拷了两个系统里现成的：**这两条第一次跑是白过的**：那两个目录夹具被写到了另一棵树下、根本没建出来，断言照样绿 —— 补真夹具后反向验过：把 `looksLikeHelper` 整个摘掉，断言立刻红成 `got=[FooUpdater,Inside,X,gui]`，夹具建在沙箱数据目录里且删之前先断言那是不是算出来的绝对路径；`--capgeo` 无界面，量截图那条几何链（真屏抓取 + 一张自造的 DPR=1.5 画布 + `drawPixmap` 的 sourceRect 按哪种坐标解释），这是"150% 缩放截不全全屏"的判据；`--cfgcheck` 离屏建一次配置页，数「下载代理」是不是只有一行、那行没生效过的「扫描根目录」有没有被加回来、应用那一栏（项目列表 + 往里几层）在不在、「添加应用…」和「添加目录…」是不是**各只有一颗** —— 配置页是 GUI，我不动他桌面，就只能把「某行出现几次」变成可断言的数字（2026-10-12 那次「下载代理」出现两遍就是这么抓的）。**同一天还补了一条结构断言：每个分组框都必须能从 `page.layout()` 走到**（滚动区里那层不是布局项，要专门进 `QScrollArea::widget()` 再走一层）。因为「应用搜索的额外目录」那一块 new 出来时认了 `this` 当父件、却没 `addWidget` 进那一列，于是标签全在、`--cfgcheck` 一片绿，**界面上却一行都没有**（他报的就是这个）；反向验过：把那行 `addWidget` 删掉，新断言就点名「应用搜索的额外目录」，而旧的三条照样全绿 —— 数标签这条路天生看不见这类问题；`--pintool <图片>` 无界面，跑置顶窗那两个收起开关的 19 条几何断言（沙箱 `APPDATA`）：初始有往期带和工具栏 → 「历史」按钮切往期带的宽 → 隐藏工具栏时窗口抱住图（不保留那条 42px 空白）→ 放回来时**长回原来那么大** → 两个开关叠用窗口正好等于图片本身 → **叠用过之后再依次放开必须回到最初尺寸**（这条抓到过"来回点几下窗口越走越歪"的 bug）→ 图区/缩略图命中坐标跟着新几何走；`--capflash [次]` 同理会真把截图浮层开一次（每下不到半秒）再走和 Esc 一样的取消路径，量的是"浮层在不在、有没有铺满整屏、是不是压在所有窗口最上"——**不判"有没有抢到前台"**，因为脚本进程拿不到前台是 Windows 的规则（前台锁看最后一次真实输入），真实按 F1 的这一支由产品日志 `capture: shown ... 前台=/在最上=/试了N次` 观测。**自测旗标的派发位置本身也是个坑**：凡在事件循环里跑的（`--capgeo`/`--capflash`）必须绕开单实例守卫，否则第二个进程会把命令行转交给已常驻的实例然后当场 `return 0` —— 形状是「0 字节输出 + 退出码 0」，看着全绿其实一行没跑（`--smoke` 就以这个形状哑了很久，2026-10-14 才发现并提到守卫之前；判据：跑任何自测旗标都得拿到输出，拿到空输出就先怀疑派发点，不能当通过）。

## 插件的三份产物（旧表）+ 插件机制、动作型路由与三次搬家记

| 目录 | 是什么 |
| --- | --- |
| `ltool.exe`（264 KB） | 只剩热键、托盘、单实例、命令行自测入口 |
| `ltoolcore.dll`（1.8 MB） | 主程序和插件**共用的一份实现**：`Utils` / `DevEnv` / `SilentInstallLog` / `AdBlocker` / `RegClean` / `InfoRow` / `FileTools` / `SysTools` / `DiskScan` / `StartupAudit` / `CaptureHistory` + 启动器/设置窗那一整套界面 |
| `ltool-plugins\ltool-<id>.dll` | 一个工具一个：页面本体 + 一层 `LToolPlugin` 壳 + 嵌在 dll 里的 `plugin.json` |

`src/ltool-plugin.h` 是两边唯一的契约：`apiVersion / id / name / desc / keywords / createPage / feedText / actions / invoke / save / load`。

**API 3（2026-10-15）加的是「动作型插件」**：截图、录屏、解压/压缩、单文件占用分析这些东西**没有卡片页可开** ——
用户按的是热键或资源管理器右键。拍板的切法是「**入口留在主程序，按下之后转发**」：
`ShellMenu` 照旧往 HKCU 写那几条 verb（它决定"有哪些入口"），点进来起的一次性进程把旗标去掉两个短横当动作名，
交给 `PluginHost::invoke(action, args)`；主程序**不再链接**那些窗口本体。
**「该不该在」= 用户那个勾（落盘 `shell/intent`）× 提供者在不在了（`plugins/shell` 装着且没禁用）**,
四个变更点都调 `syncWithPlugin()`（启动 / 装 / 卸 / 禁用启用），一条通道不分叉。
**这里踩过一个单向闩锁（2026-10-16 他报"装了右键动作，文件夹右键里没有 ltool"）**:
缺键时 `intent()` 的默认值读的是**注册表现状**,而现状正是这条规则自己会改的东西 ——
卸载插件 → `remove()` 删掉注册表 → 装回来时 `syncWithPlugin()` 读不到那个键、回落到"注册表里有没有"
→ 刚被自己删掉 = false → 判定"不写"，于是**一旦撤掉就再也装不回来**。根因一句话:**默认值不能读自己刚写过的东西**。
改法:算"该不该写"之前先**认领意图**（纯函数 `shouldAdoptIntent(键存在过?, 插件在?) = !键存在 && 插件在`）——
开关从没被人为动过且插件在，就把"装了这把插件就是为了右键"落成显式 `intent=true` 并落盘;
键存在（人明确勾掉过）就永远尊重那个 false，绝不替他打开。`--plugintest` 207 → 212 钉这张真值表
（反向验过:把认领整个关掉，红的正好是那两条）。
**写完/删完注册表立刻广播一次 `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, …)`**（和 `path add`
广播 `WM_SETTINGCHANGE` 是同一套路，只不过冲的是外壳的关联缓存）：`SHCNF_FLUSH` 逼它同步发出去，否则消息还压在
队列里界面就报"已通知资源管理器重读"，那句话成了假的。这样"撤掉右键菜单"就不用再重开窗口、注销、更不用动 explorer.exe。
**这句的效果在这台机器上还没实测**（要拿证据只有两条路，都不该我擅自动：去点他桌面上的资源管理器右键，
或者真把 ltool 的右键集成关掉再打开 —— 那是他正在用的设置，而 `HKCU\Software\Classes` 没法沙箱），
所以文案按标准机制的分寸说，不说满：新开的窗口一定跟着变；那种"改之前就已经开着"的老窗口每次弹菜单才现搭，
多半也当场就变，不变就按一下 F5。`install()`/`remove()` 返回给人看的那句就是这个口径（"一般马上就没；
个别早就开着的窗口要按一下 F5"）。**没有**用 `taskkill explorer` 这种重手段 —— 那会把他开着的窗口全冲掉，
为一张右键菜单不值当，这也是文案留 F5 那一档兜底的原因。
`actions()` 报这个插件接哪些动作，plugin.json 里那份 `Actions` 只是"门口挂的牌"（不装载就能查，商店和日志用它）。

- 路由最终以**实例自己报的** `actions()` 为准：牌上写了、代码没实现 → 记一行「元数据说接 X，实例自己不认」再找下一家。
  `--plugintest` 把「主程序那张 verb 表 / 插件的分发表 / plugin.json 那块牌」三处对一遍，
  所以「右键点下去什么都不发生」这条最难查的形状有判据。
- **实现只有一张表**：`plugins/shell/shellaction.cpp` 里那个 `kVerbs[]` 既是分发也是 `implemented()`，
  `actions()` 直接拿它，不再第二处抄一份。
- **返回值只能有一个"失败"**: `ShellAction::kNotHandled`（这条动作我根本没实现）。
  压缩包打不开、用户按了取消、窗口正常关闭 —— 那些**已经由弹窗/托盘/日志跟人说清了**，
  主程序再盖一层「这个右键动作做不了」就是第二次报错。这条是我自己踩出来的：
  第一版把 `invoke` 判成 `run(...) == 0`，而那条链的返回值本来就是窗口结果和错误码混在一起，
  于是一次**成功的压缩**（包都生成了）被判成失败，还弹了个警告框到他屏幕上（2026-10-15 实测）。
  修完之后同一条命令 `exit=0`、包正常生成、不弹任何东西。
- **F2 录屏已经整条搬进 `plugins/record`**（2026-10-15）：`Recorder`(758) + `RecordUi`(742，含选窗那一张 `WindowPicker` 和悬浮条) + `WindowPreview`(251，选择窗右边那块实时预览) + `RecProbe`(361) 都不再是 `ltoolcore.dll` 里的东西，`WindowPicker` 上那个 `LTOOL_CORE_API` 也跟着去掉了（它当初只是因为主程序的 `--recordui` 要 `qobject_cast` 它）。热键照旧由主程序注册（`RegisterHotKey(VK_F2)`），按下之后 `PluginHost::invoke("record")`；没人接的时候**只写日志 + 托盘**，不弹模态框（弹框那一次是上一轮的错误，已经改掉了）。跟着实现搬走的还有那五面旗标的判据本体：`--recprobe [ms]` `--recq` `--rectest [秒] [宽x高]` `--recordui [秒]` `--memloop [轮数] [宽x高]` —— 主程序只解析命令行、点名要导出符号 （`ltoolRecprobeRun` / `ltoolRecqSelftest` / `ltoolRectestRun` / `ltoolRecorduiRun` / `ltoolMemloopRun`），**符号找不到就算失败**（`--plugintest` 里有一条专门验 `ltoolRecqSelftest` 解析得到）。「宽x高」那条实测还在：同一秒数下 320x200 录出 7840 字节、500x301 → 9267、900x600 → 12178。画幅/奇偶那几条算法判据本来就在 `--recq` 里，一条没少。
- 没人接住动作时主程序**必须说实话**：打印 `FAIL 右键动作 X：…` + 写日志 + 弹一句「做不了：没装接这个动作的插件」+ 退出码 1，
  绝不静默。
- **F1 截图已经整条搬进 `plugins/capture`**（2026-10-15，第三个动作型插件）：`CaptureOverlay`(854，全屏浮层：框选 / 悬停吸附 / 标注 / 撤销)
  + `PinWindow`(1280，置顶窗：往期带、图上划词取字、标注对象还能改) + `Anno`(233，上面两家共用的那一套工具图标与形状几何)
  + OCR 那一族四个文件（`Ocr` 119 / `OcrPp` 740 / `OcrBundle` 305 / `OcrPanel` 140）都不再是 `ltoolcore.dll` 里的东西。
  **OCR 必须和截图一起走**：`OcrBundle.cpp` 要拿 `Archive::extract` 解引擎包，只搬截图就会多出一条 core→插件的反向依赖。
  热键照旧由主程序注册（`RegisterHotKey(VK_F1)`），按下之后 `PluginHost::invoke("capture")`，和 F2 共用同一句"没人接"的写法
  （日志 + 托盘气泡 + stderr，**一次都不弹模态框**）。
  跟着搬走的还有那十二面旗标的判据本体（`plugins/capture/captureselftest.cpp`，739 行，一个字没改）：
  `--snaprect` `--ocrtest <图>` `--ocrlayout <图>` `--pinpick <图>` `--ocrchain <图>` `--pintool` `--pindel <图>` `--ocrget`
  `--ocrpanel <图>` `--pinshot <图>` `--capgeo` `--capflash [次]` —— 符号名 `ltoolSnaprectRun` … `ltoolCapflashRun`，
  `--plugintest` 把这十二个符号逐个 resolve 一遍（点不到就算红，"插件没加载起来"伪装不成绿）。
  搬的时候踩到两处形状问题：① `--ocrpanel` / `--pinshot` 原来靠主程序的 `app` 局部跑嵌套事件循环，
  在插件里没有那个局部，改成 `qApp` + `QCoreApplication::exec()`（lambda 不能捕获 `qApp`，它是宏，得捕获 nothing）；
  ② `--capgeo` / `--capflash` 的失败数原来直接当退出码，所以 `runPluginSelftest` 加了一个"把原始失败数带回去"的出参，
  这两条仍然留在**单实例守卫之后**（浮层要在事件循环里真开出来才算数）。
- **截图族留在 core 的两样，不是忘了搬**：`src/CaptureHistory.{h,cpp}`（往期列表，配置页改存放目录要 `reload()`
  还要显示"共 N 张"）和 `CaptureState::isActive()`（搜索框靠它决定"浮层举着的时候先别收起"）。
  两个都是 core→插件的反向依赖，只能留在共享的那一份里；`CaptureOverlay::isActive()` 现在就是读这个状态位，
  不存在第二份真相。实测：`--pathcfg`（往期和存放目录那 8 条）0 失败，`--capflash 1` 一次开合 1/1、取消干净。
要点：

- **画卡片只读元数据**（`QPluginLoader::metaData()`，`plugin.json` 的内容嵌在 `MetaData` 那一层下面），
  点开才 `LoadLibrary` + `createPage`；页建过一次就缓存复用。
- **只装不卸**：Qt 插件 unload 之后里面活着的 `QWidget`/定时器就是悬空指针。要禁用某个工具就把 dll 改名或删掉，下次启动生效。
- 版本闸两层：IID 不符 `QPluginLoader` 自己拒；`Api` 不等主程序拒，并在卡片墙那行**说人话**
  （"这个插件是给 API 1 编的，ltool 现在是 API 2"），不让人对着"加载失败"猜。
- 元数据不齐（缺 `Id`/`Name`）或者塞进来一个 Qt 自己的图片格式插件，同样点名说清楚，
  而不是默默少一张卡。目录整个空了 = 这个安装是坏的，界面上也会这么写。
- 搜索框里工具那一节按名字/关键词/拼音匹配，直达那一路现在两条规矩：**命中只认开头**
  （否则搜 `renwu` 想要任务管理器会被「开机启动分析」的关键词"计划任务"顶掉）、
  **只认每个字的第一条读音**（见下一条）。松散命中（查询串里"正好含有"工具名/关键词）并进**同一节**、
  排在强命中后面 —— 不再另起一个"工具"标题。
- **2026-10-16：直达"独占整页"这个特权整个取消了**（原来命中就 `rebuildList(); return;`，
  后面「网站 / 网址直开 / 应用 / 文件 / 系统 / 剪贴板历史」五节整个没跑到）。
  现在工具那一节照样摆最前面（**第一行还是它，回车还是它**），只是不再铲平别人。
  为什么非改不可：拿他真实的 `apps.json`（267 条 / 258 个不同名字）复算旧行为，
  **48 个查询串会独占整页、44 个应用永远搜不出来**，按"这个独占该不该给"分四类：
  只是前缀 24 条（最刺眼的是 `windows` —— 「Windows 用户名」那把工具的名字本身就是 windows 开头，
  于是所有 Windows* 应用没了；还有 `wa`→War3、`go`→Google Chrome、`ding`→DingTalk、`ru`→RustRover、
  `lt`→ltool 自己）、"关键词+空格参数"15 条（`registry edit` 的人有一半是想要 regedit 那个应用）、
  字面撞名 5 条（`ad`/`disk`/`node`/`reg`/`registry`）、两字母整串首字母 4 条（`cl`→CLion、`lx`→LX Music、
  `xf`→Xftp 8、`lt`→ltool）。**注意别把 `python`/`rust` 算进这条**：他 `webs.json` 里显式配了这些网站关键字，
  那几条走的是 `webKeywordWins`（网站赢、工具整节不出），到不了直达这一站。
- 第二条（只认第一条读音）是同一天前因「搜 `An` 搜不到 Antigravity」加上的：`广` 在表里是 `guang,yan,an`，
  于是 `an` 被当成「广告」的全拼开头 → 直达 → 应用那一节整个没了（探针打出来就是 `小节=[工具] 应用行=0`）。
  代价扫过一遍：18 个自带工具的名字 + 关键词在主读音下全部照旧直达
  （`ggdc` / `qingli` / `paiban` / `gongge` / `cipanqingli` / `cpql` 一条没伤），丢的只有"次读音"那种
  （`chongqing` 不算「重庆」的直达，表里第一条是 `zhong`），而应用那一档（`Pinyin::match`）两个读音照收。
- **`webKeywordWins`（显式配在网站上的关键字 > 工具）留着不动**（2026-10-15 他报的
  `gg` 搜不到 Google 那次立的规矩）：那是"这一节整节让路"，和"让路之后还不清场"是两件事，
  各管各的 —— `gg` 看到的还是干净的 Google 一行，不掺广告弹窗。
- 这条 `gg` 规矩的来历（2026-10-15 他报的「搜 `gg` 搜不到 Google」）：那时候「广告弹窗」的首字母 `gg`
  先命中直达、整个函数提前 `return`，后面「网站关键字 / 网址直开」根本没机会跑。当时治法是
  `webKeywordWins`（工具那一节让路，不出现）；2026-10-16 把"提前 return 清场"整个取消之后，
  让路这一条**照旧保留** —— 它管的是"这一节出不出现"，不是"清不清场"。
  结果：`gg` 出 Google、`gg ltool` 多出「在 Google 搜索」那一行，而广告弹窗还有 `ggdc`（整串首字母）
  和 `广告`（关键词松散命中）两条路 —— 三条都钉在 `--pinyin` 里，
  并且**先检查 `webs.json` 里配没配 `gg`，没配就点名跳过**（CI 那台是干净配置，判红就是环境依赖的假故障）。
- `feedText`（API 2）：`json 这串…` / `b64 这串…` 把一段文本递给那个工具的页。

加一个工具（或把现有的拆出去自己维护）只要三样，都在 `plugins/<id>/` 下：

1. `plugin.json`：`{"Id":"...","Name":"...","Desc":"...","Api":2,"Keywords":[...],"Color":"r,g,b"}`
2. 页面本体：一个 `QWidget` 子类（怎么排版、读什么数据都随意，用 `ltoolcore` 的现成件就行）
3. 壳：`QObject` + `LToolPlugin` 两个基类，`Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")`
   + `Q_INTERFACES(LToolPlugin)`，照 `plugins/network/networkplugin.cpp` 抄一份改五个函数

然后在 `CMakeLists.txt` 里 `ltool_add_tool(network NetworkPlugin plugins/network/NetworkPage.cpp plugins/network/networkplugin.cpp)`
加一行（它负责把产物拷进 `build\Release\ltool-plugins\`，安装包按 `build\Release\*` 递归收走）。
`build/verify/gen_plugins.py` 是那九个页面型壳的生成器 —— 名字/描述/关键词/颜色只有一个来源，
`--plugintest` 会把 `plugin.json` 那份和壳里那五个 `override` 对一遍，漂移当场变红。

编译要求（不是建议，是"凑不齐就崩在两边的堆上"）：**同一个 MSVC 工具集、同一份按 /MD 运行的
Qt6\*.dll、同为 Release**；跨边界只递 Qt 类型，不递 `std::string`/`std::vector`。
自测入口 `ltool.exe --plugintest`（221 条，全程离屏、一次都不 `show()`）：十九个插件逐个验
"元数据通过 → 真加载 → 建页 → 没显示 → 缓存命中 → 两边元数据一致"，外加递文本、
卡片墙张数与几何（`WA_DontShowOnScreen` 走完整布局但不往屏幕上贴像素）。
判据反证做过：把 `ltool-disk.dll` 藏掉 → 4 条红（含"九个工具都在卡片墙上 got=[8]"）；
把 `ltoolcore.dll` 藏掉 → `ltool.exe` 连进程都起不来（实测 error while loading shared libraries），
所以交付闭环按 SHA256 比 exe 和 core 那两颗，装错装漏都硬停（插件目录只报数）。

## 数据目录那一页表格的原文说明

| 文件/目录 | 说明 |
| --- | --- |
| `config.ini` | 配置（搜索框齿轮修改，也可手改） |
| `apps.json` | 应用索引 |
| `usage.json` | 使用频率 |
| `icons\` | 应用图标缓存 |
| `plugins\` | 开发环境插件 |
| `webs.json` | 网站关键字 |
| `deepindex.dat` | 深索引全量快照（全盘索引，数百 MB） |
| `usnstate.dat` | USN 增量游标（几十字节） |
| `backups\` | 注册表删除前的 .reg 备份 |
| `captures\`（**默认在软件目录下**，不在 C 盘） | 截图产物：`cap-*.png` 是置顶窗的往期，`capture-*.png` 是手动保存的那张，`ocr-*.txt` 是提取文字存下来的。**存放位置可在配置页改** |
| `recordings\`（**默认在软件目录下**，不在 C 盘） | F2 录屏的 MP4（`rec-时间戳.mp4`）。**存放位置可在配置页改** |
| `clipimg\` | 剪贴板历史里的图片（开了剪贴板历史才有） |
| `ocr\` | 提取文字用的 PaddleOCR 模型 + `onnxruntime.dll`（首次用到时自动下载，之后离线） |
| `log.txt` | 运行日志（热键注册结果、扫描/增量统计） |

## 配置与数据目录（原文：config.ini 的完整注释）

`config.ini` 可配置项：

```ini
[paths]
captures=             ; 截图（含往期）存放到哪；留空 = 软件目录下的 captures（exe 旁边）
recordings=           ; 录屏产物放到哪；留空 = 软件目录下的 recordings（exe 旁边）
                      ; 配置页「数据」分组里可以直接浏览选择，保存时先验"能建、能写"，
                      ; 不行就回退到原值并说明；改完立刻生效（往期列表当场从新目录重读），
                      ; 但只影响以后存的文件 —— 已有文件一律不搬

[web]
engine=baidu          ; 默认搜索引擎：baidu / google / bing / duckduckgo

[index]
deep=true             ; 关闭后只搜常用目录
maxEntries=5000000    ; 配置页那行能改，但 FileIndexer 里的上限是写死的（20 万 / 500 万），
                      ; 也就是说这一行现在**没人读** —— 要么以后接上，要么连控件一起删掉
refreshHours=6        ; 深索引自动刷新间隔（0=不自动）；管理员增量模式无感知
; 原来还有一行 roots=（自定义扫描根目录）：配置页那行从来只有它自己在读写、FileIndexer 根本不读它，
; 深索引一直是「所有固定盘」。已经连控件一起删掉；要「指定文件夹 + 层数」请用下面的 [apps]。

[apps]
; 配置页「应用搜索的额外项目」：一行一个目录（往里找 exe，先过一道「像不像主程序」，
; 命令行工具和子进程不收）或者一行一个 exe 的完整路径（点名要的，无条件收）。
; 这条是真在用的（AppScanner::scan 读它），默认留空 = 只扫桌面/开始菜单那几处。
roots=D:\tools;D:\software\便携软件
depth=3               ; 往里找几层（0=只看目录本身，最多 6）
```

## 已知限制原文（一条一段的那种写法）

- **「压缩为 .7z」依赖 exe 旁边那个 `7zip.dll`（1.8MB，7-Zip 官方的格式库）**。它是运行期按路径加载的（objdump 查过：不在 ltool.exe 的导入表里），所以漏打包**不会**让程序起不来 —— 解压、zip、热键那些照跑，只有这一条给出"7zip.dll 不在软件目录里"并写进行为日志。`build\verify\deliver.ps1` 装完会点名确认它在不在，别只看 exe 的哈希。
- **深索引增量（USN）需要管理员权限运行**；普通权限下自动回退为"快照 + 定时目录遍历重建"，功能不缺失只是新鲜度略降。
- 本机 Windows 版本（26200）的 `FSCTL_ENUM_USN_DATA` 必须把 `MFT_ENUM_DATA` 的 `HighUsn` 设为当前日志最大 USN 才能正常枚举（已适配）。
- `reg clean` 里 HKLM 的项需要管理员权限，无权限时该项会失败并提示（HKCU 项不受影响）。
- `Alt+Space` 被其他程序占用时自动降级到备选热键（日志与托盘提示）。
- 截图"提取文字"的引擎是**自带的 PaddleOCR PP-OCRv4**（det 4.5MB + rec 10.4MB + `onnxruntime.dll` 17.6MB）。这三样**不打进安装包**：第一次用到时从 PyPI 现下（两个固定版本的 wheel，SHA256 钉死，解包只抽这三个成员），落在 `%APPDATA%\ltool\ocr`，之后完全离线；下不到就明说"没装上"。以前用的系统自带 `Windows.Media.Ocr` 已经**完全移除** —— 它取决于对方在 设置→应用→可选功能 里装没装「中文(简体) 光学字符识别」，很多机器上没有。运行时用 `LoadLibrary` + ONNX Runtime 的 C API（`third_party/onnxruntime/*.h` 只要头，不链接），det 找行 → 每行 crop 后 rec 认字 → CTC 贪心解码，字典从 `rec.onnx` 的元数据 `character` 里读（6625 类：0 是 blank，尾巴一个是空格）。**只做正放的文字**：det 的连通域取轴对齐外接矩形，不做 minAreaRect/透视矫正（对外的 `Ocr::Line` 本来就是 QRect）。实测：640×180 三行字 0.5s；2560×1440 整屏 2.1s、219 行；一个字一个框（划词因此不用等宽切）。手工补下：`ltool.exe --ocrget`。

## 功能细节与实测（每个功能为什么这么做、实测多少、拿哪面旗标验）

## 功能细节与实测

上面那张表只回答"有什么功能"。这一节按功能记着**为什么这么做、实测是多少、拿哪面旗标验**，
是原来那张功能表里的字，整段搬下来的，一条没删。

### 应用快速启动

扫描桌面（含一层子目录）、公共桌面、用户/公共开始菜单里的 `.lnk`，解析目标与图标，同目标自动去重。配置页「应用搜索的额外目录」（在**配置那一栏往下滚，「定时扫描」和「剪贴板历史」中间**那一块）还能自己加，**两种条目**：一行一个**目录**（往里若干层找 exe，层数 0-6 可配，默认 3 层，`apps/roots` / `apps/depth`）或一行一个 **exe 的完整路径**（点名要它）—— 界面上是「添加目录…」/「添加应用…」/「移除选中」三颗按钮，那一栏的标题也从「应用搜索的额外目录」改成了**「应用搜索的额外项目」**（现在装的那版就是新标题）；扫出来的和快捷方式走同一套去重，卸载器一律不算应用。**填目录时先过一道「像不像主程序」**：这台机器实测把 `D:/software` 配进去（往里两层）一下捞回 596 个 exe，全是 `QQPCTray / BugReport / pg_ctl / 7z / cmake / git-bash / fsnotifier / restarter / ShellFolder64` 这种子进程和命令行工具，把搜索框彻底淹了，所以两道判据命中任一条就不算应用：① **PE 头是控制台子系统**（Subsystem=3，命令行工具一律排除；头读不出来按「不是命令行」处理，不能让解析失败变成什么都不收）；② **名字或路径像助手**（updater/crash/helper/elevator/service/agent/patch/cli/bash…，路径里有版本号目录如 `18.1.30302.212`，或是 `locales`/`swiftshader`/`plugins`/`redist` 这类运行时目录）。**误杀的出路就是点名加**：那一栏里直接写那个 exe 的完整路径，无条件收。**第三条规则是「同目录已有主程序就不再多收一个」**：桌面/开始菜单的快捷方式指向哪个 exe，那个目录就算被占（点名加进来的 exe 同理），同目录其余 exe 不再自动收 —— 这条杀的是 `QQEX.exe`（QQ 的协议启动器，双击没反应，却和 QQ.exe 同目录，上一版它还排在 QQ 前面；他截图指的就是这个）。本机实测两道 + 这一道之后：`应用目录 2 个（往里 2 层）收了 49 个应用，跳过 398 个不像主程序的、149 个同目录已有主程序的`，索引从 753 → 295。加/改目录要按保存才会重扫（扫盘是重活，不随输入跑），无界面断言在 `ltool.exe --appdirs`

### 全局呼出

`Alt+Space`（被占用时自动降级 Win+Space → Ctrl+Alt+Space）在屏幕中上方弹出搜索框，再按一次隐藏

### 搜索

输入即搜应用（前缀 > 词首 > **拼音全拼** > **拼音首字母** > 包含，按使用频率排序；**使用次数的键**：有快捷方式记 `lnkPath`、没有的记 `target` —— 以前直接用 `lnkPath`，而目录扫来的那些没有 lnk，**全共用同一个空键**（这台机器涨到 43 次），于是 QQEX 靠别人家的 43 次排在 QQ 前面；那个空键读的时候直接丢掉，它没法归属到具体应用）；**输入先归一：全角折半角**（`ＱＱ` = `qq`，偏移 `0xFEE0`，全角空格 U+3000 也一起收）—— 中文输入法切到全角时打出来的根本不是 ASCII，索引里明明有 QQ 却一行都搜不出来（2026-10-12 他报的）；同一条链上还验 `renwu` 那种直达抢页的情况，判据见下面 `--pinyin`；**`renwu` 出"任务/人物"，`rw` 也出**（多音字每个读音都算：`chongqing` 和 `zhongqing` 都能搜到"重庆"），工具直达同样认拼音（`cipanqingli` / `cpql` → 磁盘清理），**但直达那一档必须从开头命中**（`Pinyin::matchStart`）—— 中段命中只是“名字里正好含这几个字母”，拿它独占整页结果会把人要的那个顶掉：实测踩过搜 `renwu` 想要任务管理器，结果被「开机启动分析」的关键词“计划任务”（jihua+renwu）当成直达，而直达是 `return`，应用那一节整个没了。应用那一档仍然允许中段（打 `guanli` 要能出任务管理器）；**同一档还只认每个字的第一条读音**（2026-10-16 他报的 `An` 搜不到 Antigravity：`广` 在表里是 `guang,yan,an`，`an` 于是被当成「广告」的全拼开头，又是直达 `return` 把应用那一节顶掉 —— 生僻读音没有独占整页的权力；代价只有"次读音"那种，`chongqing` 不再直达「重庆」，而应用那一档两个读音照收）；表由 `tools/genpinyin.py` 从 pypinyin 的字典生成（2.6 万字，140KB，编进 exe）；文件索引不做拼音（270 万条现算会把 20ms 拖成秒级，要做得预建索引）；同时搜文件（见下）；网址/网站关键字直达浏览器

### 文件搜索

**快索引**（桌面/文档/下载/图片/音乐/视频，实时监听秒级更新）+ **深索引**（全盘快照 + USN 增量，管理员权限下增量仅需约 0.5 秒）

### 网站直达

`ld`/`linuxdo` → linux.do，`gh` → GitHub，`gg` → Google…；输入网址直接打开；关键字后带参数可站内搜索（`gh qt6`）；无匹配时提供搜索引擎兜底

### 开发环境初始化

`init` 列出插件，`init java` / `init c++` / `init node` / `init python` / `init rust` 检测组件，缺失的按 Enter 用 winget 安装

### 管理 PATH

`path` / `path list` 查看用户 PATH；`path add <目录>` 添加；`path del <目录>` 移除；写入 `HKCU\Environment\Path` 并广播 `WM_SETTINGCHANGE`

### 注册表整理

`reg` 体检：无效卸载残留 / 失效开机启动项 / MuiCache 残留；Enter 删除单项，`reg clean` 全部清理，删除前自动备份 `.reg`

### 配置页面

- **菜单四行**（2026-10-15 收的）：网站 / 插件商店 / 配置 / 关于。`--settingspage <行> [tab] [插件id]` 的行号也跟着换：0 网站、1 插件商店、2 配置、3 关于。
  `openAt` 现在是**先展开卡、再挑 tab** —— 软件那两张页搬进插件之后，那个 `QTabWidget` 在卡片里面，反过来写就永远挑不到。
- **搬走的三张页**：`plugins/software`（推荐软件 + 软件列表两张 tab）、`plugins/downloads`（下载器）、`plugins/tasks`（定时任务）。
  实现里留在 core 的两件是刻意的：`TaskScheduler`（主程序一起来就每 30 秒看一次的时钟）和 `DownloadManager`（「关于」页下载安装包也走它，两处共用同一份任务表）——
  把卡片禁用掉不会让"到点真的执行"跟着消失。
- **`--uptime` 那两条耗时现在走插件符号**（`ltoolSoftwarePage` / `ltoolUninstallPage`，主程序按名字点名要）。
  拿不到符号会打一行"没有导出 …"而不是静默变成 0 ms —— 装出来的 dll 太旧时这条就是证据。
- **`TaskScheduler` / `DownloadManager` 两个类贴了 `LTOOL_CORE_API`**：搬页的时候链接器报的是 `staticMetaObject` 找不到，
  而 `WINDOWS_EXPORT_ALL_SYMBOLS` 只导函数不导数据 —— QObject 子类的 metaobject 是数据，跨模块 `new` 它就得显式导出。
- **商店只有一面列表**（2026-10-15：原本"卡片墙 + 下面一栏清单"是两页并排，他要求合成一面）。
  `ToolsPage::shopRows()` 是这一面唯一的数据入口：没拉清单 → `PluginStore::localRows()`（只报目录里有什么，不猜仓库），
  拉过清单 → `rows(entries)` 后面接 `localOnly(entries)`。一张卡可以有**三颗**按钮（「更新」+「禁用」+「卸载」——
  装了的东西不能因为版本旧就禁不掉、卸不掉），
  它们排在**卡片最底下一行**（2026-10-15：以前和状态词挤同一行，状态那句长一点就把按钮挤得看不见）：
  `ShopCard` 的正文列是 名字 → 说明 → 状态 → stretch → 按钮排，`addButton()` 只管往那颗前置 stretch 后面追加。
  大小一律写成 **MB**（`PluginStore::humanSize()`，两位小数）：卡片上任何一行都不许留"N B"那种裸字节，
  包括下载中途按钮上那行字（总大小不知道时报的就是已下到的 MB）。
  `localOnly` 现在服务的就是 software / downloads / tasks 这三张（`dist/` 还没收它们，#78 做完就没了）。
- **`QFile::rename` 在 Windows 上不覆盖已存在的目标**（2026-10-15 他一点「更新」就露馅）：Qt 那边直接返回失败，
  于是 `finishDownload` 撞在 `renameStaged` 上，报的还是那句"被别的程序挡住（多半是杀毒）"—— 冤枉了杀软。
  「更新」这一条从写出来就没成功过一次（自测只走过"目录里本来没有"那条，`--storetest` 里现在补了"能盖掉已存在的那颗"）。
  正解：落盘之前先 `QFile::remove(dest)`；删不掉（这个进程正用它）就明说"重启之后再点一次更新"，并把 .part 拿掉别留垃圾。
- **「装/更新」现在排队，一次只飞一颗**（2026-10-15）：他连点九颗那次崩在 `scan()` 里 —— 事件日志写着 `ltool.exe` 挂 `Qt6Widgets.dll` / 0xc0000005，程序日志最后九行是九颗「装好」，紧接着重扫的「plugin: 看到 …」数到第 9 条就断了。九条下载 + 九次改名 + 中途 `reload()`/重排叠在一起是根，不是哪一颗的错。`ToolsPage::installEntry` 现在只放行一颗，其余进 `m_queue`（卡片上写"排在第几位"），队列空了才 `finishInstalls()`。**队列里也存 `QPointer`**：第一版存裸指针，判据点九颗那一下当场崩给我看（`rebuildGrid()` 之后 `startNextInstall()` 往没了的按钮上写字）。
- **卸载答复那段拆成了 `ToolsPage::applyUninstall`**：离屏里点不到"是" —— 无窗口平台上 `QMessageBox::question()` 会**立刻返回默认值**（第一版判据因此拿到"没动。"，红了个假的）。拆出来让测试直接调同一段，不抄第二遍。
- **判据的终止条件别拿"字节数不变"当落地**：刚点下去那几秒 `.part` 就是 0KB，三次采样相同不等于装完了（上一版就是这么把一次正常排队读成"留了个 .part 没清"）。要"不变 **且已经有字节**"。
- **离屏自测里 `processEvents()` 不结算 `DeferredDelete`**：`rebuildGrid()` 之后旧卡片其实还活着，
  "回调打到已删控件"这种形状根本量不出来（第一版 `--storemulti` 就是这么绿着放过了访问违例）。
  要 `processEvents()` 之后补一句 `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`，
  旧控件真死了，判据才有牙。
- **拉插件那三级顺序只在 `PluginStore::fetch` 里写一遍**（清单和 dll 两段都用它）：镜像前缀 + 原件走**直连**，
  然后原件走配置里那条代理（留空 = 系统代理），最后原件直连；每条候选原地试两次。
  镜像只给 `raw.githubusercontent.com` 用 —— 指到自己的服务器还套镜像只会拿回 404，让他以为自己的源坏了。
  2026-10-15 实测的形状：`gh-proxy.com` 对这个文件 **403**（10-11 量到 8.3MB/s），代码重试两次后换
  `ghfast.top` 一次成功，状态行就写"走的是 镜像 ghfast.top，一共试了 3 次"。
  内置候选只有这两条：`mirror.ghproxy.com` 21s 超时已死、`raw.gitmirror.com`/`raw.kkgithub.com` 解不到 DNS、
  `cdn.jsdelivr.net` 缓存约 7 天 —— 拿它装插件会一直"哈希对不上"，所以不收。
- **卸载是真删文件**：`<file>` 和它的 `.off` 一起删；本进程已经加载过的那颗 Windows 不放行（我们从不 unload 插件），
  于是记进 `store/pendingRemove`，**下次启动第一件事**（`applyPendingRemoves`，在任何插件被加载之前）删掉并销账。
  `--storetest` 验的是"记上账 → 下次启动真删 → 销账"，**没有**假装验过"删不动"那个分支（那要一颗真加载过的 dll 才进得去）。
- **`定时任务` 那张卡的关键词是 定时/计划/cron，没有单字的「任务」**：`renwu` 是"任务管理器"的全拼，
  加了「任务」这条直达就把应用整节顶掉了（#60 就是为了这个把「计划任务」摘掉的）。`--plugintest` 里那两条 renwu 判据当场红过，改的是关键词不是判据。

语言那一页是**整个搬进**配置页那个「语言与翻译」分组框的（控件一个没抄，所以也不会有两份内存状态、两个保存路径），入口只剩「配置」这一个。

### F1 截图

全屏遮罩框选，鼠标悬停自动吸附窗口（**吸附出来的就是看到的那个边界**：拿 DWM 的 `EXTENDED_FRAME_BOUNDS`，不是 `GetWindowRect` —— 后者在本机实测比可见帧**左/右/下各多 7px、上边多 0px**（那三边是看不见的 resize 边框），旧写法还按左边的余量给上边补了 7px，等于四周各多圈一层桌面；无界面断言在 `ltool.exe --snaprect`）；标注支持矩形/圆/箭头/文字/马赛克/撤销；**提取文字**（自带的 PaddleOCR，第一次用下载一次模型、之后离线）把选区里的字认出来，自动进剪贴板并可编辑/存 txt；完成后自动复制到剪贴板并存到截图目录（**默认在软件目录下：`D:\software\ltool\captures`**，不往 C 盘的 `%APPDATA%` 堆；配置页「数据」里可改，装到写不了的只读位置时自动退回数据目录）。置顶截图窗里画的矩形/圆/箭头/打码块都是**可编辑对象**：切到「拖动/选择」点中就选中，拖内部整体移动、拖 8 个手柄改大小、Delete 删除（打码块挪走后原位置恢复成原图，因为底图和标注分开存）；按住 Alt 拖是自由涂抹，烙进图里改不了。工具栏还有一把**「划词」**：认完字后直接在图上拖拉选字（按词的包围盒切到单个字），松手就进剪贴板、窗不关，单击选整词、`Enter` 复制全部、`Esc` 退出；要改字/存 txt 用图上右键「识别全文」。左侧往期缩略图**右键 = 删除那一张**（内存和 `captures` 里的文件一起删；删的正好是当前显示的那张就自动挪到相邻一张）。**缩放不是 100% 也能截全整块屏**（他这台机器 150%）：`QPixmap` 的 `copy()` 和 `drawPixmap` 的 sourceRect 都按**设备像素**解释，而选区是逻辑坐标，旧写法直接把逻辑矩形递进去，150% 下就只截到 4/9 那块屏；现在统一过一层 `deviceRectFor`（dpr=1 时原样返回，100% 的行为一点没动），裁完再把 DPR 贴回去。无界面断言在 `ltool.exe --capgeo`（含一张自造的 1.5 倍画布，100% 的机器上照样能抓到这个错）；**置顶窗默认就是"只有一张干净的图"**：工具栏和左边那条往期带建窗时都收着（窗口正好等于图片本身），要工具栏就在图上右键「显示工具栏」，换上一张/下一张用图左右两侧的箭头（往期带收着也能用），放出来之后工具栏的「历史」按钮再切往期带。右键「隐藏工具栏」可以随时收回去 —— 收起时窗口抱住图片本身（不留左右黑边、也不留那条 42px 的空白），且图上右键只剩「显示工具栏」和「关闭」两项

### F2 录屏

按 F2 弹出选择列表（输入关键字过滤，回车开录），**不想录了有四条退路**：右上角那颗**「✕」**（看得见的那个，给眼睛用）、`Esc` 收起、点到别的窗口（一失焦就自己收起，它原本会一直压在你要录的那个窗上面）、**再按一次 F2 也收起**（手就放在热键上，这条最自然）；后三条都能从输入框和列表里收到，因为 `QLineEdit` 会自己吃掉 Esc，所以给两个子控件都装了事件过滤器。这窗是 `Qt::Tool` + 无边框，本来没有标题栏，所以「✕」是画在标题那一行的右上角（hover 变红），点它 = `hide()`，和 Esc 一条路。**第一条就是「整个屏幕 · 主显示器 · 2560x1440」**，往下是桌面上看得见的窗口，选谁录谁：录窗口用的是系统自带的 `Windows.Graphics.Capture` 按窗口取帧，别的程序盖在它上面也不会入镜；被虚拟桌面收起 / 没标题 / 小于 80x60 的窗口不列。**录整块屏幕走 `CreateForMonitor`，并且把鼠标指针也画进画面**（`IsCursorCaptureEnabled`；录窗口那一路仍然不含指针，保持合成帧确定性）——注意那条小悬浮条本身在屏幕里，录全屏时它会出现在画面右下角。右下角一条小悬浮条显示 ⏺ 计时 + 帧数，点「停止」收尾，产物是普通 MP4（H.264，Media Foundation 系统编码器，不拉任何外部 DLL），存到录像目录（**默认在软件目录下：`D:\software\ltool\recordings\rec-时间戳.mp4`**，不在 C 盘；配置页「数据」里可改），条上变成"已保存 N 帧 / X KB → 文件名" + 「打开所在目录」。**旁边还有一个「不要了」**：停下这一局并且把那个半成品 MP4 就地删掉（Recorder 没有"只停不写盘"这条路，所以照常收尾再原地删），条上写"已取消，没保存（丢掉 N 帧）"，8 秒后自己消失 —— 录到一半的东西留在录像目录里就是垃圾。**当前只录画面，不录声音**（要加的是 WASAPI 环路采集 + 音画对齐，没做就不放开关）。参数在 `config.ini`：`ui/recFps`（默认 15；画面静止时 WGC 不给帧，不凑帧）、**`ui/recQuality`（画质档，默认 1=标准）**、**`ui/recMaxWidth`（编码画幅长边上限，默认 0=原始）**。 **2026-10-14 起，选择窗列表下面多了两颗下拉框**（改了就记住，下次按 F2 还是那两档，不用每回重选）： 「画质」= 流畅 / 标准 / 高清 / 超清，本质是**每像素每帧给多少 bit**（0.060 / 0.145 / 0.250 / 0.400）， 码率按**编码后的画幅**算：`标准` 在 2560x1440@15 上正好是以前那个固定 **8000kbps**（等效值反推来的，不是随手取的）， 所以全屏录制行为没变；小窗口以前不管多大都按 8Mbps 写，现在按面积给，**小窗录出来的文件会明显比以前小**。 「画面」= 原始 / 长边≤1920 / ≤1280 / ≤854，等比缩小**用面积平均，不是隔行取样**（隔行取样一缩放文字就闪、就出摩尔纹）。 `--recq` 实测：同一张 640x400 的卡，不缩那局文件画幅 640x400 / 13337 字节，选 ≤320 那局文件画幅真是 **320x200**、5853 字节， 并且解回亮度网格仍然"上亮下暗"（缩放没把画面翻过来）；噪声画面两局 271534 vs 371749 字节。 **一个实测事实值得记下**：这台机器的 MF 编码器把 `MF_MT_AVG_BITRATE` 当**参考**而不是硬上限 （低档目标 400kbps，实测 2.5 秒写出约 900kbps），所以自测的判据写成"档位调高之后同内容文件明显不变小"， 而不是"文件大一倍"—— 我第一版按后者写，当场红，量清楚才发现是我的期望不对。 中途改窗口大小 / 把目标窗口最小化或关掉，这一局会自己收尾（已录下的部分照样在文件里，条上会写清是"没录成：原因"），收尾后 F2 立刻可以再用。 H.264 只吃偶数画幅，所以奇数大小的窗口会自动往下取偶数再编

### 修复

检测 .NET Runtime / VC++ 2005~2022 / VSTO / DirectX / UCRT，未安装的一键 winget 安装

### 广告弹窗

按窗口标题/进程名扫描弹窗，可关闭窗口、结束进程、加入拦截名单；勾选自动拦截后每 60 秒自动关闭

### C盘清理

常见可清理位置（临时/更新缓存/DX缓存/pip/npm/浏览器缓存等）扫描大小、一键清理、回收站清空；C 盘顶层目录占用分析。**两个 tab 的列表都支持右键「打开文件夹」**（右键落在整行哪都行 —— 名字、下面那行完整路径、占比条都会冒到行卡片上）：「可清理位置」开的是那个缓存/临时目录本身，「占用分析 · 本层目录」开的是那一行对应的目录；左键在占用分析里是下钻，所以开文件夹只走右键。目录已经不在了（比如清理完之后的 Temp）那一项就摆成灰的、写「这个目录已经不在了」，不会点了没反应。这条只有界面在用，且真按一下就会拉起资源管理器，所以判据在离屏自测 `ltool.exe --rowmenu`（9 条：菜单就一项、文案、灰没灰、**这项的 data 就是这一行自己的目录** —— 点开之后拿的就是它，所以这条等于验"开的是它不是别的"）。**占用分析首扫在这台机器上要 14~19 秒**（48.1 万个目录、142 万个文件，全量枚举的地板，多线程也顶不住：8 线程只比 1 线程快 1.3~1.7×、而且 4/8/16 的顺序在两遍之间还会互换，见 `--diskscale`），所以改成了两件事：**边扫边出数**（一个一级分支定稿就摆一行上来，实测 1.6 秒就有 13 行，不用干等最后一秒）和**每个盘的结果留着**（切走再切回来直接吃缓存、不重扫；状态行写明上次扫描的时间，按「整理」才重算，清理完那个盘会作废该盘的缓存）。判据 `ltool.exe --diskui`（7 条）+ `--diskbranch`（4 条，专门盯"先报给用户的那个数和最后树上的数是不是同一个"）

### Windows 用户名

工具页那张卡片（2026-10-13 起它是 `ltool-user.dll` 这个插件）：先只读地把现状摆出来（账户名 / 机器名 / 显示名 / 本地还是微软账户 / 用户目录 / SID），然后三件事：**改显示名**（SAM 的 FullName，登录界面和锁屏上那个名字，最安全）、**改账户名**（`Rename-LocalUser`；界面上写明白 **C:\Users 那个目录不会跟着改名**、写死旧路径的程序不会自动跟上、要注销重登）、**新建用户**（显示名 + 密码 + 勾选「加到 Administrators 组」= 提权，不勾就是标准用户）。三个动作都是**提权跑 PowerShell**（`runas`），而且**一律不自动执行**：要人在界面上按下，界面上也写清楚后果。判据搬进了插件本身（`--username` 现在跑的是插件导出的 `ltoolPluginSelftest`，27 条：`psQuote` 把单引号翻倍 / 名字合法性 / 拼出去的脚本长什么样 / 页面上三颗按钮真的摆在布局里 / 读回来的现场含真实机器名和 SID）

### 关于（菜单第 4 行 + 托盘「关于」）

显示**版本号（唯一来源是 `ltool.iss` 的 `#define MyAppVersion`，CMake 解析后编进 `LTOOL_VERSION`）**、Qt 运行时版本、程序/数据目录、工具插件数（含没通过检查的个数）和日志路径；下面是**检查更新 → 下载新版 → 装并重启**三颗按钮，一件都不自动：检查走 `https://github.com/<仓库>/releases/latest` 那条 **302**（跟完重定向从 `/releases/tag/<tag>` 里读版本号，带 User-Agent，沿用「配置 → 下载代理」那条），失败按 404/连不上传区别说一句人话；下载交给内置下载器（进度看台在「下载器」那一页，这里不假装有进度）；「装并重启」会**先退出 ltool**，所以要点两下（弹窗默认停在“不”），它写一段临时 PowerShell：等我们这个 pid 退场 → 静默装回**同一个 /DIR** → 再把 ltool 拉起来，日志在 `%TEMP%\ltool-update.log`。服务器给的资产名一律过 `safeFileName`（只留 `[A-Za-z0-9._-]`、去掉开头的点），脚本里每个路径都走单引号翻倍，带单引号直接整段拒绝。无界面断言 `ltool.exe --about`（24 条，含版本比较 `1.10 > 1.9`、`../evil.exe` 变 `evil.exe`、逃逸口、四颗按钮在布局里、页面显示的版本 == 编译期那个），真联网探针 `ltool.exe --updatecheck` 点的是界面同一颗按钮

- **检查更新以前打的是 `api.github.com/.../releases/latest`，那是全局限流的一条**：匿名额度按**出口 IP** 算 60 次/小时，同一台机器上任何脚本（我这轮就拿它反复查 CI 作业）都能把它吃光 —— 他点「检查更新」看到 `HTTP 403` 就是这么来的。1.0.3 起改走网页那条 302：`/releases/latest` → `/releases/tag/v1.0.2`，读最终地址就行，**没有限流**；安装包直链按我们自己的命名推（`releases/download/<tag>/ltool-<版本>-setup.exe`），两条都实测过：curl 拿 302 落点、Range 拿直链回 206。代价是 api 才给的字段没了 —— 发布**日期**和资产**大小**不再预报（宁可少说一项，也不为一行字把限流那条请回来），`--storenet` 里加了一条真跑这条链的判据。

### 网络

网卡 DNS 一键切换预设（阿里/腾讯/114/Google），UAC 提权 netsh 执行，含刷新 DNS 缓存与恢复 DHCP

### 文件占用 / 粉碎

文件与文件夹右键 ltool 子菜单：**占用分析**一个窗口两件事 —— 上面是大小 / 实际占簇 / 簇大小 / 文件系统 / 硬链接（文件夹会递归求和），下面是哪个进程正开着它（Restart Manager，可结束该进程）；**粉碎**为三轮覆写后永久删除，不进回收站

### 压缩包读写

右键解压到当前目录 / 解压到同名文件夹、压缩为 .zip / .7z。读是 libarchive：zip / 7z / tar / gz / bz2 / xz / zstd / iso / cab / cpio，rar 只读且不完全支持；写两条分家 —— zip 用 libarchive 的 deflate，**.7z 用 7-Zip 自己的多线程编码器**（bit7z + 随包的 `7zip.dll`，见下）。**要快就用 zip**：同一个 32MB 的文件夹实测 —— 我们的 .zip 0.73 秒 / 27.1MB（比 7-Zip 自己压 zip 的 0.96 秒还快）。**.7z 那条已经不再走 libarchive**，改成直接请 7-Zip 自己的多线程编码器干活（`src/SevenZip.cpp` + vcpkg 的 `bit7z`/`7zip`，运行时加载随包带的 `7zip.dll`）：同一个文件夹 **10.1 秒 → 1.8 秒**（改前那 10.14 秒是他自己现场日志里的一行），产物 23.46MB 和 7-Zip 命令行逐字节同尺寸（它 `-mx5` 是 1.58 秒，差的三百毫秒是我们启动自己的开销）。以前追不上的原因量清楚了，就是线程数：libarchive 的 7z 写出器 LZMA1/LZMA2 恒为单线程，7-Zip 靠 `-mmt` 把 LZMA2 分块丢给多个核。试过又否掉的两条：把 zstd 塞进 7z（`threads` 唯一支持的编码器）能到 0.3 秒，但产出的包连本机 7-Zip 25.01 都报 `Method = 04F71101 / Unsupported Method`；自己包一层 .lzx（清单 + 标准 .xz 流，用 liblzma 的 `lzma_stream_encoder_mt`）实测 0.48 秒 / 25.7MB 最快，但**别人拿到 .lzx 根本不知道是什么文件**，所以删了。zip 里指定 zstd 也不行：7-Zip 会静默退回 Deflate。中文条目名按格式喂 `hdrcharset`（tar/cpio=UTF-8，zip/rar/lha=本机码页），中文路径走 `_w` 接口。默认后台静默完成 + 托盘提示，配置页可打开进度窗口（带取消）；**进度窗口跑完自己就关**（成功或取消都关，那句话交给托盘去举），只有失败才留着说原因，这时「打开位置」指向那个没改名的半成品；半成品写 `.part`，成功才改名。**吞吐实测（60MB 伪日志，`--archspeed`）**：解压 zip 857MB/s、解压 7z 343MB/s（都比 7-Zip 自己快，解压这条路还是 libarchive）；压缩 zip 118MB/s / 5.9MB；**压缩 7z（新后端）4.17 秒 / 3.6MB，同一个夹具 7-Zip 命令行要 6.30 秒 / 3.4MB —— 我们快 34%**。档位曲线（只认 7-Zip 定义的 0/1/3/5/7/9）：1 档 0.20 秒 / 4.7MB、3 档 0.53 秒 / 4.5MB、5 档 4.43 秒 / 3.6MB、7 档 7.79 秒 / 4.4MB、9 档 8.00 秒 / 4.4MB —— **7 档往上体积反而回涨**（多线程把输入切的块多了，每块各自从零开始建字典，上下文短了），所以默认停在 5 档：它既是 7-Zip 自己的默认档，也是这条曲线上"每秒换多少字节"最划算那一点。每一档都验过两件事：我们自己的读取器解得回来、`7z t` 退出码 0。**右键那一次性进程干完活把托盘气泡交给常驻实例去举**（走单实例那条本地套接字，消息格式 `note\t标题\t正文`）：以前它为了"让气泡显示满 5 秒"自己多活 5 秒，实测解压一个包端到端 5.4 秒而活儿本身只有 0.2 秒；改完端到端 0.36 秒

### 其他

托盘图标、单实例、使用频率统计（空查询显示常用应用）、运行日志

## 自测旗标一览（44 面，全表）

| 旗标 | 一句话 |
| --- | --- |
| `--show` | 启动即把搜索框显示出来，不是只进托盘。 |
| `--smoke` | 无界面把索引、目录、插件都点一遍。 |
| `--mfttest` | MFT 直读那一路的判据（提权机器上会真跑枚举与 $MFT 大小解析，非提权点名跳过）。 |
| `--searchtime` | `--searchtime 词1 词2`：深索引搜索的耗时 + 前 3 条结果。 |
| `--searchdump` | `--searchdump 词1 词2`：每个词前 20 条**按顺序整条**打出来，改搜索实现就拿它做 diff。 |
| `--disktime` | C 盘上 A/B 两种磁盘遍历（DiskScan vs QDirIterator）的耗时。 |
| `--diskscale` | 拿环境变量 `LTOOL_DISKTHREADS` 把同一棵树按 1/2/4/8/16 线程各扫一遍，量扩展性。 |
| `--diskbranch` | 4 条：边扫边报的那几个分支数，和最后定稿的树是不是同一回事。 |
| `--diskui` | 7 条：磁盘页整条链（点「整理」→ 途中就出行 → 定稿 → 换盘让位 → 换回来吃缓存）。慢，要真扫一整盘。 |
| `--plugintest` | 十九个插件逐个验「元数据 → 真加载 → 建页 → 没显示 → 缓存命中 → 两边一致」+ 右键菜单该不该在的四组合 + 意图认领那张真值表 + 卡片墙张数与几何 + 动作路由（现在 221 条）。 |
| `--cliptest` | 「剪贴板」那一页 离屏 56 条 + 要真剪贴板的 5 条（`QT_QPA_PLATFORM=windows` 才跑，离屏点名跳过：offscreen 的 clipboard 是内存桩，跑在那儿只会假绿）（沙箱 APPDATA，一次都不碰你真实的 `clipboard.json`）：过滤那条纯函数 13 格（空关键字=全部、大小写、trim、中文、搜整段而非首行、按日期、图片按「图片」和尺寸、命不中给空、一条命中多处不重复列），剩下盯**行号 ↔ 存储下标**（过滤后第 0 行必须映射回 items[1]，每一行显示的字必须等于它 UserRole 那条的摘要）、状态行说实话（没在记录写「没在记录」并指路开关）、双击复用 / Delete 删这条 / 清空 三条动作真落到数据上。反向验过：把 UserRole 存成行号，当场红 4 条。**两个自己踩的坑也记在这里**：① 幻觉代码 `QListWidgetKeyEater` 和两参 `installEventFilter`（编译当场抓住）；② 第一版把"状态行 setEnabled"那两条排在 Delete **前面**，一打开收集 `capture()` 就往夹具里补一条、把固定 4 条的下标冲乱，于是两条判据假红 —— 夹具型判据里**任何会改全局状态的开关都要排在依赖固定数据的判据之后**。 |
| `--username` | 27 条：「Windows 用户名」那一页，**一次都不真的改账户 / 建账户**。 |
| `--rowmenu` | 9 条：磁盘页两栏列表的右键该弹什么、点开的是不是这一行自己的目录。 |
| `--archivetest` | `--archivetest extract / make / probe / progress`：压缩包读写回归；`make` 第 5 个参数是 7z 档位 0-9。 |
| `--pinyin` | 70 条（2026-10-20 本机实测 PASS 行数；这一串从前写的是手工点的 52，口径不同，往后按行数记）：拼音表和码点必须对齐、多音字、全拼与首字母，直达那一档只认第一条读音（`An` 被「广告」的生僻读音抢页那次加的），另有 4 条盯"直达不清场"（反向验过：把 `return` 塞回去，红的正是其中 2 条），最后真建一个不显示的窗口往输入框里打字。电源那一族占 13 条：路由 5 + 去抖抢拍 2 + 命令行形状 6（`shutdown.exe` / `shutdown /s` / `shut` / `shut /f` 算，`shutdown 怎么用` / `shutter` / `shut down 怎么用` 不算，中文「关机后怎么恢复文件」不算）。；打包应用那一族占 4 条（现扫 + 那道缝本身，见文末那节）。 |
| `--linuxdotest` | 44 条：「linux.do 盯帖」离线那半条 —— 解析 / 命中与去重 / 前缀拼法 / 配置夹紧与回读 / 要不要弹那一句 / 页面九件控件可达。一次网都不上、一个气泡都不弹（跑在沙箱 APPDATA 里）。 |
| `--linuxdonet [秒]` | 10 条：真去取一趟（默认等 60 秒），状态行说出这次走的是直连还是中转、拿到几条、第一趟只播种不弹框。**要联网，不进 CI**。实测一趟 4954ms、30 条。 |
| `--gridtest` | 64 条：「图片排版」的格数与列行、收图与拒收、拖动是挪还是换、✕ 只空那一格、合成尺寸与像素、板上预览与出图同一块构图、坏文件那一格说实话、**效果图弹框先看过才落盘**（`generate()` 里那道 `m_gate` 就是为这条留的门：判据把它换成回绝 → 断言没写文件、状态行说「没存」、「打开这张」没亮；换成同意 → 才真落盘并数到目录里多一个 `grid-*.png`。反向验过：把弹框从 `generate()` 里摘掉，那 2 条当场红成 `got=[0/0/1]`。离屏判据都不 `exec()` 模态框 —— 那里面的返回值不作数，这条在卸载那一步栽过）（离屏，判据在 \`plugins/grid/gridselftest.cpp`）。**背景色的选择挪进了效果图弹框**（顶栏那颗是盲选）：框里那颗写着当前挑的色，换一下 → `compose` 重算 → 那块缩图和那行字当场跟着换；**没按保存之前页面认的还是老色**（`commitPreview` 才把它定下来），按取消连色都不改。8 条判据盯这个，其中一条是反向断言「顶栏不再有那颗」—— 两处都留迟早对不上。**点空格子 = 往里放一张**这一组 13 条：`pickInto(那一格)` 只在「空格子 + 原位松开」时发（点有图的格子不发 —— 那是拖动起点；按下去拖出那一格再松也不发；点 ✕ 只拿掉不发选图），`putAt` 只动指定那一格、拒非图片后缀、拒越界格号、能盖掉已有那张；端到端那两条走页面：先给 `setPickGate` 装一道门（默认是 `QFileDialog`，判据换成给一个固定路径），点第 4 格 → 断言 `paths()[3]` 就是那张、状态行说「第 4 格放了」，门回空串（= 选图框按取消）→ 那一格还是空的。反向验过：把页面那条 `connect` 注释掉，红的正是端到端那 2 条。**踩过的坑**：板子层的信号判据一开始走的是页面那块板子，一点就弹真选图框 —— 离屏看不见窗口但进程卡在模态框里，形状是「0 字节输出 + 一直不返回」；改成用一块独立的 `GridBoard` 验信号，页面只留给端到端那两条。 |
| `--archspeed` | `--archspeed [每文件KB] [文件数]`：量压缩包读写吞吐，并把 7z 的 1/3/5/7/9 档整条曲线跑一遍，每档都验「我们能解回来 + `7z t` 退出码 0」。 |
| `--ocrtest` | `--ocrtest <图片>`：打印图里认出的文字和耗时。 |
| `--ocrlayout` | `--ocrlayout <图片>`：打印每行每词的包围盒和切出来的字格（划词的坐标就靠它验）。 |
| `--pinpick` | `--pinpick <图片>`：图上划词 9 条（把鼠标事件直接送进一个不显示的置顶窗，**不合成鼠标**），跑完归还剪贴板。 |
| `--recprobe` | `--recprobe [毫秒]`：录屏之前先量地基 —— 系统里有哪些编码器、MP4 写 30 帧再读回来要多久、按窗口取帧能不能用。 |
| `--recq` | 13 条：录屏那两档（画质 / 画面）真的进了编码器，缩完不许把画面翻过来。 |
| `--rectest` | `--rectest [秒] [宽x高]`：录一张自造的会动的卡 → 把 mp4 解回来数帧，末尾再录 1.5 秒整块屏幕。 |
| `--recordui` | `--recordui [秒]`：45 条，F2 那条链整条走一遍（列表 → 悬浮条计数 → 程序化按「停止」→ 解出帧 → 「不要了」→ 选择窗四条退路）。 |
| `--pindel` | `--pindel <图片>`：10 条，右键往期缩略图删那一张（`APPDATA` 挪进临时目录，不碰你真实的往期）。 |
| `--ocrpanel` | `--ocrpanel <图片>`：真开一次「提取文字」结果窗（要人关掉才算跑完）。 |
| `--ocrget` | 只把 OCR 引擎那三个文件下载好并校验 / 解包（新机器补下载用的入口）。 |
| `--ocrchain` | `--ocrchain <图片>`：11 条，从「一个文件都没下」跑到当场认得出那三行字；会真下 26MB，但落在临时沙箱里。 |
| `--memloop` | `--memloop [轮数] [宽x高]`：每轮解压一个压不动的 32MB 包 + 录一段指定尺寸的窗，逐步打印私有字节 / 工作集 / 句柄数。 |
| `--toggle` | 6 条：按 `Alt+Space` 到底出不出来（冷启动跑，会把启动器真弹出来一两秒）。 |
| `--pathcfg` | 9 条：配置页那两行「存放目录」真的管用（沙箱 `APPDATA`）。 |
| `--appdirs` | 12 条：应用目录那条过滤（PE 子系统、助手名、版本号/运行时目录、同目录已有主程序）。 |
| `--capgeo` | 7 条：缩放不是 100% 时截不全全屏那条几何链（真屏 + 一张自造的 DPR=1.5 画布，100% 的机器上照样能抓到这个错）。 |
| `--cfgcheck` | 11 条：离屏建一次配置页，数每一行在不在、那两颗按钮是不是**各只有一颗**、每个分组框能不能从布局走到；另数设置窗菜单那四行（顺序、每行点开有内容、**只有「插件商店」那颗换成了画出来的袋子**），并把四颗图标拼成一条 PNG 存到 `%TEMP%\ltool-cfgshots\menu-icons.png` 供肉眼复核。 |
| `--storetest` | 67 条（2026-10-16 数的；文档原来写 60，加"排队那句写进卡片"三条之前实测已经是 64）：插件商店离线那半条链 —— 清单解析（收两条、拒三条，拒的每条说一句）、`localRows`（目录里 `*.dll` 算已装、`*.dll.off` 算已禁用、旁边的 `*.bin` 不算）、和本地目录比（能装/已装/可更新/装不了）、`.part` 验哈希再改名（哈希或字节数不符就删掉半成品）、`<file>`↔`<file>.off` 改名禁用/恢复、索引地址留空回内置。**一次都不联网**。 |
| `--storenet` | 18 条：真拉一次 `dist/index.json` 并离屏点「刷新清单」，**逐张卡**核对「那句状态词 ↔ 那颗按钮」——没装的只给「装」、装了的只给「禁用/启用」、可更新的两颗都要，再对状态行里的卡片数 / 能装数 / 拒收数 / 清单外数；三条新的盯这次改版：卡片上不许留裸字节数、本机装着的那几张都得写出 MB、**每颗按钮都在卡片最底下一行**（离屏量几何）。**要联网，没网就是红，所以不进 CI**。 |
| `--autoupdatenow` | 立刻跑那一趟每天自动更新（推开"今天跑过"那颗闸），把结果那句话打到 stdout 然后退出。要联网、会真往 `ltool-plugins\` 写 dll ⇒ 不进 CI。2026-10-21 本机第一次：清单 404（那版还没发）→ 退回本地缓存 19 条 → 挑出 0 颗 → 那句话照实说「没有要换的（网络没成…）」，一趟 8.2s / 第二次 24.2s。 |
| `--seamtest` | 平台层那三道缝自己（`src/platform/Paths.h`、`Launch.h`、`Autostart.h`、`ProcInfo.h`、`FileMeta.h`）20 条：四个已知目录问得到且**两两不同**、Windows 上各自落点、占位符换成系统里那个真值、夹在句子里的也换、认不出的原样留着、空路径不许启动/不许提权。Linux 侧 43 条走 `wsl -e bash tools/seam_probe.sh`（拼音 10 + 缝 14 + 自启 9 条真往返：HOME 指临时目录，勾上→文件真在→关掉→文件没了）。细节与反向验见文末 **跨平台移植：平台层那道缝**。 |
| `--storemulti [颗数]` | 5 条：连着点几颗「装/更新」（他那次是九颗），然后**当场把卡片墙拆掉重排** —— 回调连的是页面（还活着）、卡片是 deleteLater 掉的，用裸控件指针就是往已删的 QWidget 上写。回退版跑出来 `rc=-1073741819`（0xC0000005 访问违例），修复版全绿。要联网、会往运行目录真写 dll，所以 `build/verify/storemulti.ps1` 先拷一份临时副本；不进 CI。 |
| `--uninst [id]` | 11 条：卸载那条路整个走到底 —— 先 `PluginHost::page()` 把那张页开出来（dll 就此映射进进程）、回商店、`applyUninstall`、重排、再切回那张页；另一颗没打开过的直接卸。两种都得活着，且"当场删掉"与"排到下次启动"必有一条成立。跑在临时副本里（它会真删文件），不进 CI。 |
| `--pintool` | 19 条几何断言：置顶窗那两个「收起」开关（离屏，一次都不 `show()`）。 |
| `--capflash` | `--capflash [次]`：把 F1 浮层真开一次（每下不到半秒）再走和 Esc 一样的取消路径，量「在不在、铺没铺满、压没压在最上」。 |

每一面旗标量到的数字、期望值为什么改过、反向验是怎么做到的，原样记在文末 **自测旗标逐条**。

## 数据目录与配置项（全表）

| 文件/目录 | 说明 |
| --- | --- |
| `config.ini` | 配置（搜索框齿轮修改，也可手改） |
| `apps.json` | 应用索引 |
| `usage.json` | 使用频率 |
| `icons\` | 应用图标缓存 |
| `plugins\` | 开发环境插件 |
| `webs.json` | 网站关键字 |
| `deepindex.dat` | 深索引全量快照（全盘索引，数百 MB） |
| `usnstate.dat` | USN 增量游标（几十字节） |
| `backups\` | 注册表删除前的 .reg 备份 |
| `captures\`（**默认在软件目录下**，不在 C 盘） | 截图产物：`cap-*.png` 是置顶窗的往期，`capture-*.png` 是手动保存的那张，`ocr-*.txt` 是提取文字存下来的。**存放位置可在配置页改** |
| `recordings\`（**默认在软件目录下**，不在 C 盘） | F2 录屏的 MP4（`rec-时间戳.mp4`）。**存放位置可在配置页改** |
| `clipimg\` | 剪贴板历史里的图片（开了剪贴板历史才有） |
| `ocr\` | 提取文字用的 PaddleOCR 模型 + `onnxruntime.dll`（首次用到时自动下载，之后离线） |
| `log.txt` | 运行日志（热键注册结果、扫描/增量统计） |

## 跨平台移植：平台层那道缝（WP1，2026-10-16）

- 他拍的是**全量移植三个平台**，所以顺序不是选出来的、是编译器定的：core 的 30 个 TU 直接 `#include <windows.h>`，Linux 上连 `ltoolcore` 都编不出来 —— 必须先抽缝，Windows 这边行为零变化，再往缝的另一头接 POSIX 实现。
- 第一轮只动两道最小的缝：**已知目录 + 环境变量展开**（`src/platform/Paths.h` → `Paths_win.cpp` / `Paths_posix.cpp`）、**打开 / 提权打开 / 在文件管理器里定位**（`src/platform/Launch.h` → `Launch_win.cpp` / `Launch_posix.cpp`）。Windows 那两份就是把 `SHGetKnownFolderPath` / `ShellExecuteExW` / `ExpandEnvironmentStringsW` 原样搬过去，一行语义没改；`revealInExplorer` 改名 `reveal`（那三个字在 mac 上是句假话）。
- `Utils::` 里那五个入口**删干净了**，18 个文件（src 8 个 + 插件源 10 个）全换成 `Platform::`：`expandEnv` 18 处、`launch` 11 处、`launchAsAdmin` 3 处、`reveal` 11 处、`knownFolderPath` 10 处、枚举项 13 处（外加 `Utils::KnownFolder` 这个类型名 1 处），另有 Utils.cpp 里 3 处不带前缀的内部调用；脚本按字节替换，跑完断言残留数为 0。
- 枚举项换了名字：`Desktop/PublicDesktop/UserStartMenuPrograms/CommonStartMenuPrograms` → `UserDesktop/SharedDesktop/UserApps/SystemApps`。Linux 对得上的是 XDG 桌面 + `~/.local/share/applications` + `/usr/share/applications`，macOS 是 `~/Desktop` + `~/Applications` + `/Applications`；`SharedDesktop` 在这两个平台上**天生是空的**，扫应用那几处本来就会跳过空根。
- **Wayland / macOS 上的降级是能力缺失，不是 bug**：`reveal` 在 Linux 只能打开父目录（xdg-open 没有「选中这一项」这个语义），带参数又不是可执行文件的目标在 POSIX 起不来（那是 Windows 文件关联才有的语义），这两条都写在头文件里，界面文案要跟着改口径。
- 判据：**`--seamtest` Windows 14 条、`seam_probe.sh` Linux 14 条，两边全绿**（这是第一刀当时数的；到 22:20 复跑已经是 16 / 23，见下面「第三道缝」）。原有那三面对照数一条没动：`--plugintest` 221 / `--pinyin` 52 / `--cfgcheck` 11（`--cliptest` 从 36 涨到 56 是同一天另一件事，见 **剪贴板历史的保留天数**）。
- **这条缝为什么必须新加判据**（反向验的结果）：把 `FOLDERID_PublicDesktop` 换成 `FOLDERID_CommonPrograms` → 红 2 条（「两两不同」、「Public 那一棵」），`INFO` 那行当场把共用桌面打印成 ProgramData 那棵；把 `expandEnv` 改成 `return s` → 红 2 条。但第一版那两条占位符判据是**拿 expandEnv 自己的输出和自己比**，改成直通照样绿 —— 这就是"自己比自己"的判据，等于没判据。换成跟 `qEnvironmentVariable("SystemRoot")` / `QDir::homePath()` 这两个**外部真值**比之后，直通才被抓得住。
- 顺手抓出两个连带问题：① `tools/make_sdk.py` 抄头文件时不建子目录，插件一旦 `#include "platform/Paths.h"`（现在有了）下次发 SDK 会当场抛 `FileNotFoundError`，已加建目录那一步，实跑到 `include/platform/` 两颗头齐；② CI 的判据清单是写死的那串旗标，新旗标不进去就等于只在开发机上跑过，`--seamtest` 已加进 `.github/workflows/build.yml` 那个循环。
- Linux 端**还没到能编整个工程**：configure 停在 `find_package(unofficial-bit7z)`（vcpkg 那个端口不出 Linux  triplet），`ltool_target_setup` 里那 18 个 Win32 导入库已经先收进 `if(WIN32)`。下一刀（WP2）要处理的是依赖分档 + `Archives`/`SevenZip` 那条压缩链，而不是假装缝抽完就能编。

## 剪贴板历史的保留天数（2026-10-16）

- 他要的是「给个 7 天，可设置天数」：配置页「剪贴板历史」那一栏多一行 **保留天数**（`clipboard/keepDays`，默认 7，0=「不限」，范围 0-3650），改一下当场生效、不用点保存。
- **两条上限取交集**：超过 N 天的一律清，没超龄的仍受「最多 200 条」兜底；清掉的条目连 `clipimg\` 里那张 png 一起删（`prune()` 那趟孤儿图清理本来就干这事）。裁剪时机三处：`load()` 读完立刻裁（并把 json 重写一次）、改了天数立刻裁、2 秒轮询里加一道"今天扫过没有"的闸门 —— 最后这道是给**收集开关关着**的那条路留的：`capture()` 第一行就 return，永远走不到 `prune()`，不补这一刀的话超龄条目会一直看得见。磁盘成本是一天一次，不是两秒一次。
- 边界说清楚：7 天 = 整 7 天那天就清（`secsTo(now) >= keepDays*86400`），"保留 7 天"不该留到第 8 天；时间戳读不出来的条目按超龄处理（界面本来也画不出它是哪天复制的）。裁掉就是真删了，改回「不限」不会把它们找回来 —— 这句写在 tooltip 里，界面上不说假话。
- 他真实的历史量过才动：`%APPDATA%\ltool\clipboard.json` 现在 200 条、最老 **1.6 天**，所以 7 天这条规则上线**一条都不会裁掉**（30/90 天同样是 0 条）。
- 判据：`--cliptest` 36 → **47 条**。新增 11 条是 4 条纯谓词（差 1 秒不超龄 / 整 7 天超龄 / 天数 0 永不超龄 / 无效时间戳算超龄）+ 4 条数据层（改成 7 天当场少一条、活下来的是哪两条按 hash 点名、留下的那张图文件还在、天数进了 config.ini）+ 2 条往返（改回「不限」不找回、config 跟着回 0）+ 1 条状态行（天数=0 时必须写「天数不限」，不许只报条数）。
- **夹具里的时间戳全部改成"从现在往前算"**：原来写死 `2026-05-04T23:01:02`，这种日期在按条数封顶的年代没事，一加天数规则就会在某天自己变成超龄条目、把那 36 条踩在一份被动过的数据上。沙箱 config.ini 现在把 `keepDays` 钉成 0，天数自己的行为单独测。
- **我这一轮自己写出来又自己删掉的一次真数据丢失（必须原样记着）**：上面那条"归一化"我只修了一半 —— keep 那一侧转成了正斜杠、`full` 那一侧还是拼出来的混合写法，两边永远不相等，于是**所有还在用的图都被判成孤儿**。更糟的是我为天数规则加了 `load() → prune()`，把这段低频的清理第一次挪到了**启动路径**上：21:05:09 装上的那版一启动就把他 `%APPDATA%\ltool\clipimg` 里 **49 张剪贴板图全删了**（条目里声明的合计 9.9MB）。证据链：clipimg 目录 mtime = `21:05:09.413`，和 log.txt 里 `clipboard: started enabled=1 items=200` 同一秒；json 里那 49 条的 `path` 现在全部 `exists=False`；`QFile::remove` 不进回收站。下一次 `load()` 会撞上"图没了就不留空条目"那条，把这 49 条历史一并丢掉。
- 修法三件，缺一不可：① **两侧**都过 `keepKey()`（正斜杠/反斜杠/连续斜杠/大小写全折叠）再比；② 加一道闸门 `sweepAllowed(itemCount)` —— 一条历史都没 load 到的时候**不许**启动批量删（真要全删走的是 `clear()` 自己那段）；③ 删了几张必须写日志（`clipboard: swept N orphan image file(s), items=… keepDays=…`），静默的批量删除等于没有发生。
- 判据：`--cliptest` 36 → **56 条**。新增 20 条里，11 条是保留天数（4 条纯谓词 + 4 条数据层 + 2 条往返 + 1 条状态行），9 条是这个孤儿判定（`keepKey` 三种写法同判 / 两个不同文件不许被归成一个 / 闸门在 0 条时拦住 / 现场造一张没人引用的 stray → 裁一次它必须走、历史里那张必须留、条目数不许被牵连）。**反向验**：把"只对一侧归一化"这个我真犯过的错还原回去 → 红 2 条；把闸门改成永不拦 → 红 1 条。
- 教训（比上面那条更通用）：**把一段"清理/删除"代码从低频路径挪到启动路径之前，要先说清它删的是谁**。我加 `load() → prune()` 是为了让天数规则在关着收集时也生效，结果顺手把一次字符串比较的错误放大成了一次全量删除。
- 顺手修的一个输出形状：`--cliptest` 以前打两行"失败 N 项"（判据自己打 5、宿主按退出码又打 1），grep 的人拿到的是那个被压成 0/1 的数。按仓库既有规矩给 `--cliptest` 传 `flag="clipboard"`，宿主就不再补那一行。
- 反向验三处故意改动都在**同一轮里一起红**，各归各的：边界符号 `>=`→`>` 红 1 条（整 7 天那条）；按天裁剪整块关掉红 3 条（少一条 / 活下来的名单 / 改回不限不找回）；天数不落盘红 1 条。有一条本来以为会红的没红 —— 「配置文件跟着回到 0」在"不落盘"那一版照样绿，因为夹具开头就写死了 0。教训同上一轮：**没红的那条要能解释**，别当成"这条也验到了"。

## 21:20 那次闪退：点剪贴板那张页 → 卡住 → 0xC0000005（2026-10-16 夜）

- 他报的现象：「在插件点击剪贴板卡，卡死了闪退了」。日志形状：`21:18:48 plugin: 建页 clipboard` → 70 秒空档 → `21:19:58 hotkey 呼出` → `21:20:03 FATAL CRASH: code=0xC0000005 addr=7ffeeae161f4`，转储 `%LOCALAPPDATA%\CrashDumps\ltool.exe.29012.dmp`。
- 转储能确定的：**异常地址在 Qt6Core.dll + 0x61f4**（`0xC0000005`）。这台机器没装 cdb/WinDbg、Release 又没有 PDB，我用 python 手写了一遍 minidump 解析（模块表 + 异常流 + 线程表 + AMD64 CONTEXT），拿到 Rip/Rsp 之后只能做"栈里哪些 qword 落在 ltool 自己模块内"这种残留扫描 —— 那 46 个命中里大部分是残留数据（`最近导出` 的偏移大到 +0xFF000 那种就不是函数边界），**不足以定罪**。归因结论只能到这里：崩在 Qt 内部的一次坏指针解引用，而栈顶残留里有 `qt_static_metacall@ToolsPage@`（商店那张页）→ 两个方向都在。
- **能定罪的是代码本身**：`ClipboardStore::restore()` 原来是 `const Item& it = m_items[index];` 然后往剪贴板写。写这一步在真实 Windows 上会**同步**回发 `dataChanged`（外壳连写入方自己一起通知），我们那个槽当时直接 `capture()` —— 它把这一条先 `removeAt` 再 `prepend`（去重置顶），`m_items` 就此搬家，而 `restore()` 手里那个引用指向已经释放的内存，最后那行日志还在读 `it.isImage` / `it.text`。这个形状完全对得上"异常地址在 Qt6Core 里"（QString 的字段被读在已释放的对象上）。**这条不是天数那轮新写的代码，是剪贴板功能自带的一条老 bug**，只是他现在才走到"复用中间那一行"。
- 修了两处，都在 [src/ClipboardStore.cpp](file:///D:/project/cpp/ltool/src/ClipboardStore.cpp)：① `restore()` 拿**副本**（`const Item it = ...`），引用不再跨过一次剪贴板写入；② `dataChanged` 槽改成 `QTimer::singleShot(0, ...)` 推到下一个事件循环回合，进去先比 `GetClipboardSequenceNumber()` 再决定收不收 —— 这样自己那次写既不重入、也不再白重编码一遍（图片那条实测是几百毫秒，就是他看到的"点一下卡住"）。
- **一条关于离屏测试的硬事实（下次别再踩）**：`QT_QPA_PLATFORM=offscreen` 下 `QApplication::clipboard()` 是**内存桩** —— `setText()` 不发 `dataChanged`、也不碰真剪贴板。所以"自写不重入"这 5 条判据跑在离屏里是必然绿的假绿。现在按平台点名跳过（同 `--recordui` 要真屏幕那条规矩），要真验就 `QT_QPA_PLATFORM=windows ./ltool.exe --cliptest`：那一档 61 条全绿，且反向验（把槽改回同步直收）当场红 1 条 —— 红的是「自我那次写不把历史顺序搅动」。
- 离屏 `--cliptest` 56 条 + 真剪贴板那 5 条 = 61；`--plugintest` 221 照旧。装的那份同步到本次构建后离屏跑 56/0 并打出那行 SKIP。
- **没查完的那一半**：栈残留指向 ToolsPage，而并行那个会话有 2 个未提交的文件（`src/ToolsPage.cpp`、`src/AboutPage.cpp`）**已经随这两个包一起装上了** —— 它的改动里有"卡片自己 mouseReleaseEvent 里先调基类""卸载/启用改成 singleShot 推迟""rebuildGrid 里 `setCurrentIndex(0)`"这几处，形状上都可能在点击卡片时踩到已删控件。我没有证据说就是它，也没有证据排除它。要定罪只能拿"再崩一次的转储"或"在真桌面上点一遍卡片"（后者要动他的屏幕，我没做）。

## 卸载排队那句要写在卡片上（2026-10-16 夜）

- 他报「右键动作卸载不了」。查到的事实：`store/pendingRemove` 记上了 `ltool-shell.dll`，21:47:59 那次启动由 `applyPendingRemoves` **真删掉了 1 个**（现在插件目录里既没有 `.dll` 也没有 `.dll.off`，待删表是空的），`HKCU\Software\Classes` 下 `*\shell\ltool`、`Directory\shell\ltool`、`Directory\Background\shell\ltool` 三颗 `Test-Path` 全是 False，日志里 `shellmenu: remove, 现在干净了吗=yes`。**东西其实卸掉了。**
- 让他说"卸载不了"的是界面：插件被进程加载着的时候点卸载，只会记进待删表、等下次启动才落地，而卡片上那句话原先只写给 `applyUninstall` 传进去的那个 `QLabel` —— 点完马上 `rebuildGrid()` 换整格，那句话跟着旧标签一起没了。他连点四下，屏幕上一条解释都没有。
- 改在 core 侧（[src/PluginStore.cpp:420](file:///D:/project/cpp/ltool/src/PluginStore.cpp#L420) 那一段）：`localRows()` 扫到排过队的文件名，就把它的 `note` 写成「xxx 已安排卸载 · 这个进程还加载着它，下次启动真删掉（138 KB）」—— 不靠任何一次点击的临时标签，重建多少次都在。**这是数据层说话，不是文案补丁**：卡片状态本来就该由"目录 + 待删表"两个事实决定。
- 判据：`--storetest` 64 → **67 条**（新加 3 条：排队那颗写明「下次启动真删掉」、没排队的对照不许被说成排队中、对照那颗必须在列表里——最后这条是防止上一条拿"空列表"假绿）。反向验：把那段 `if` 短路 → 当场红 1 条，且只有那一条红。
- 文档里的旗标数我重新数了：`--storetest` 原来两处写 60，实测本轮之前就已经是 64。

## 第三道缝：开机自启（2026-10-16 夜，跨平台 WP1 续）

- 选它当第三道的理由：那五个"机制级缺口"里**只有它有真 POSIX 对等物** —— 不是降级，是三个平台各有各的机制：Windows `HKCU\...\CurrentVersion\Run` 里那个值（不需要管理员）、Linux `~/.config/autostart/ltool.desktop`（XDG 自动启动规范）、macOS `~/Library/LaunchAgents/com.ltool.autostart.plist` + `RunAtLoad`。`Utils::setAutostart/autostartEnabled` 删干净，4 个调用点（配置页那颗 + 托盘菜单那颗）换成 `Platform::`。
- macOS 那一支**只写文件、不 `launchctl bootstrap`**：文件写上只是"注册"，真装载要么等下次登录、要么另外调 launchctl。所以那句"已开启开机自启"在 macOS 上只能说"注册上了"，界面上别说"已经生效"。这条限制写在 [src/platform/Autostart.h](file:///D:/project/cpp/ltool/src/platform/Autostart.h) 的注释里，不是埋在代码里。
- 两个纯函数是**为了能被问才抽出来的**：`autostartTarget()`（落点）和 `autostartEntry(exe)`（内容）。这条缝的失败模式只有"勾了没反应"，不会报错、不会崩，路径拼错就等于功能不存在。
- `.desktop` 的 `Exec=` 走 unix 引号规则：空格、制表、换行、双引号、反引号都要转义（`xdgQuote`），否则装到带空格的目录里那一行会被切成两个参数；plist 那一支要过 XML 转义（`xmlEscape`）。
- 判据：Windows 侧 `--seamtest` 14 → **16 条**，**只问落点和内容、绝不调 `setAutostart`** —— 它会真去动他机器的 HKCU Run 键（顺手 INFO 打一句这台机器当前是"已开/没开"，让他看见自测没替他改）。Linux 侧 `tools/seam_probe.sh` 14 → **33 条**（2026-10-16 夜又并进拼音那 10 条），其中自启那 9 条是**真往返**：`HOME` 指到一个临时目录 → 勾上 → 文件真在 `autostartTarget()` 说的那个路径上、`Exec=` 里是当前 exe → 关掉 → 文件没了、`autostartEnabled()` 跟着说没开。
- 反向验（三处故意改动，两边各自红自己那几条）：Windows 值名 `ltool`→`ltool-disabled` → 红「落点是 Run 键下的 ltool 值」；条目不转原生分隔符 → 红「条目写的就是当前这个 exe」；Linux `xdgQuote` 直接 return → 红「空格被转义」；文件名 `ltool.desktop`→`ltoolX.desktop` → 红「文件名就是 ltool.desktop」。**最后这条判据是这一轮新加的**：我先只做"写和读都用同一个函数"，那样改文件名是量不出来的（写读一起错就自洽）—— 又是一次"自洽的判据等于没判据"。
- 对照数一条没动：`--plugintest` 221 / `--pinyin` 52 / `--cfgcheck` 11 / `--storetest` 67 / `--cliptest` 56。装的那份同步到本次构建后 `--plugintest` 那 12 红还是"右键动作被他卸掉了"那一族（不是这轮带的）。
- 还欠着的：`Utils` 里剩下的 Windows 依赖（`extractIconToPng` 的 `PrivateExtractIconsW`、`fileTargetMissing` 的 `SearchPathW`、`killProcessTree`、`workingSetMB`、`trimWorkingSet`）＋ `src/pch.h` 之外那 38 个 `#include <windows.h>` 的 TU ＋ WP2 那个 bit7z configure 坎。

## 发版前那道 ABI 闸：`tools/abi_check.py`（2026-10-16 夜）

- 起因就是这一轮那一刀：core 少了一个导出符号名 = 已发布的插件 dll 当场 `LoadLibrary` 失败，而现场只报"加载失败"、不报缺哪个符号。我不想再靠事故回忆，所以把"这份新 core 能不能喂饱商店那批 dll"写成一个能跑的数。
- 做法是纯 PE 解析（python，不需要 dumpbin/WinDbg）：读插件的 `IMAGE_DIRECTORY_ENTRY_IMPORT`，挑出模块名是 `ltoolcore.dll` 的那些符号，再和本机那份 core 的导出表比。**退出码就是缺的符号数**（0 = 都能加载）。
- 拿真东西量了两头：对本机这批（19 颗，都是同一棵树编的）缺 **0**；对商店已经发布的那批（`C:\work\ltool_plugin\dist`，14 颗）缺 **7 个引用槽 = 2 个不同符号**，坏 **6 颗**（capture / language / record / registry / repair / startup）：`Utils::revealInExplorer` 被 5 颗 import，`Utils::launch` 被 2 颗。
- 比我先前口头说的"9 颗 / 3 个名字"更准：他机器上当场坏 9 颗，是因为里面还有 3 颗（downloads / grid / software）是本机构建那份、**商店清单里还没有**（#78 那五颗没进去的坑）；而 `expandEnv` 其实没有已发布 dll 用到它（那三个调用点都在 core 内部）。**口头数字再次因为没实测而偏**，这次是拿导入表实测的。
- 已经挂进交付闭环（[build/verify/deliver.ps1](file:///D:/project/cpp/ltool/build/verify/deliver.ps1) 的 `abi` 那一栏，52-83ms）：目录不在就 `SKIP ... 没查，不是通过`，绝不静默放行。**不 throw** —— 这一栏说的是"发版前要不要重发插件"，装到他自己机器上这件事不受它管。
- 顺带一条 PowerShell 的坑（我第一次就把这栏写成了假数字）：`python` 打出来的中文进了 PS 会被 GBK 码页搅成乱码，用中文行去 `Select-String` 数就数出 **0**，于是那一栏打印"缺 0 个符号"、而它上面刚列出 7 个。改成让 python 额外打一行纯 ASCII 的 `MISSING_SYMBOLS=<n>` / `BAD <dll> <n>`，PS 只数那个。**要跨进程传数就别用中文当键。**
- #91 现在有了数字可以拍：① 重发那 6 颗（要走插件仓库 CI，顺带把 #78 欠的五颗补齐）；② core 里留 `Utils::launch` / `revealInExplorer` 两个转发名（商店里那批永远不断，代价是两个没人调的壳）。我的倾向是 ①，因为 1.1.0 还没发、这一版正好把 #78 一起收掉。

## 台阶一：Linux 上 configure 过了，编出来的第一批 TU 有名单（2026-10-16 夜）

- 做法是把"哪些 TU 现在就可移植"变成**量出来的名单**，不是我印象里的数：在 WSL 上对 core 的 36 个 TU 逐个 `g++ -std=c++20 -fsyntax-only -I src $(pkg-config --cflags Qt6*)` → **13 过 / 23 不过**。过的：`Pinyin` `Archives` `CaptureHistory` `DownloadManager` `FileTools` `InfoRow` `PluginStore` `SilentInstallLog` `SysTools` `ToolsPage` `WebShortcuts` `WebsitesPage`（+ 平台层那三道缝）。不过的 23 个全卡在同一类：`#include <windows.h>`，也就是注册表 / USN-MFT / WGC+MF / HKCU verb / COM 这五族机制还没抽缝。
- `Archives.cpp` 居然过了：它吃的是**系统 libarchive**（WSL 里 `/usr/include/archive.h` 在），bit7z 那颗后端在 `SevenZip.cpp` 里，那一颗照样 FAIL —— 所以"7z 压缩"这一路在 Linux 上还没有后端，别当成已经能用了。
- CMake 的形状：非 Windows 走到 `add_library(ltool_portable OBJECT 缝 + 名单)` 然后 **`return()`**，core / exe / 插件那几段一行都不执行。为什么用 OBJECT：这批名单里还有一堆调用 `Utils::` 那些**还没进缝**的函数，做成 `.so` 要么链接报未定义符号，要么（Linux 默认允许 .so 留未定义符号）**"链接成功"却根本跑不起来** —— 后者比现在老老实实说"编到这里为止"更像假绿。
- 实测（WSL Ubuntu 24.04 / gcc 13.3 / cmake 3.28）：`cmake -S . -B /tmp/lp2` **CONFIGURE OK**（那条 status 打的是"只编平台层那三道缝 + 名单里那 12 个 TU"），`cmake --build --target ltool_portable` **BUILD OK**。Windows 那一整条路径一个字没动：`--seamtest` 16 / `--plugintest` 221 / `--pinyin` 52 / `--cfgcheck` 11 / `--storetest` 67 / `--cliptest` 56 全绿、退出码 0，闭环 20.9s、包 22.09MB、SHA `MATCH 3A5404E9F16D`。
- 顺手让 Linux 那侧第一次**跑起来**验了行为而不是只编译：`tools/seam_probe.sh` 从 23 涨到 **33 条**，新并进去的 10 条是拼音引擎（`usableQuery` 三条闸门、全拼/首字母/中段、`An` 不抢「广告弹窗」的直达、`renwu` 不抢「开机启动分析」、表和码点对齐的"垃圾/任务"抽查、多音字两个读音）—— 全绿。**这十条是有分量的**：搜索框那一整路是可移植的，Linux 上不用重写。

## 第五道缝：进程与文件元信息（2026-10-16 深夜，WP2 第二块砖）

- 动机不是"再抽一道缝好看"，是 **`Utils.cpp` 自己还不能在 Linux 上编** —— 所有人都在调它，它就是一堵墙。里面最后六个 Win32 入口切成分给了两处：`platform/ProcInfo.{h}`（`killProcessTree` / `workingSetMB` / `trimWorkingSet`）和 `platform/FileMeta.{h}`（`extractIconToPng` / `fileVersion`），另外把"只给文件名时系统找不找得到"（`SearchPathW`）归进 `Paths.h` 叫 `findExecutable`。`fileTargetMissing` 那个判断逻辑留在 `Utils`（本来就跨平台），只把平台那一步调出去。
- 三个真对等物 + 两件如实做不到：**`findExecutable`**（POSIX 用 `$PATH`）、**`workingSetMB`**（读 `/proc/self/statm` 第二列乘页大小）、**`killProcessTree`**（`/proc/<pid>/task/<tid>/children` 递归，读不到就扫 `/proc` 比 `PPid`，最后 `SIGKILL`）—— 探针里真的起了一个 `sleep 30` 再杀掉，验 `/proc/<pid>` 从在到没了。做不到的两件：`trimWorkingSet` 在 POSIX 没有对等物（`malloc_trim` 只还堆顶，动不了内核记的常驻页）→ **返回 false**，调用方要说"这平台做不到"；`fileVersion` 普通文件没有版本资源 → **空串**，不许编一个。
- 判据：Windows `--seamtest` 16 → **20 条**，Linux 探针 23 → **43 条**。Windows 那 20 条里有一条顺带量出来的事实：**我们自己的 exe 没有 VERSIONINFO 资源**（`fileVersion(自己)` 是空，`Get-Item .VersionInfo.FileVersion` 也是空）—— 关于页那个版本号是编译期宏，PE 里读不到，所以任务管理器和"属性→详细信息"的版本列是空的。我没有为了让判据变绿去改期望值，而是把判据换成拿 `cmd.exe` 量（真读得到 `10.0.26100.x`），自己那份作为 `INFO` 打出来 + 记成已知缺口。
- **Linux 侧的 TU 名单从 12 涨到 13**：`Utils.cpp` 进来了（WSL 复测 `g++ -fsyntax-only` 逐个 TU 量：还是那 12 个 + Utils，其余 22 个照旧卡在 `windows.h`）。CMake 的 `LTOOL_CORE_PORTABLE_SRC` 跟着加了一行。
- **ABI 闸当场涨了价**：`deliver` 的 `abi` 栏从"缺 7 个符号"变成**缺 8 个**（capture 从缺 1 变缺 2）。这轮又删了六个 `Utils::` 名字，已发布的那批里 import 到的多了一个。#91 现在更贵了 —— 越往后抽缝，要重发的插件越多。这不是停止的理由，但它是"什么时候停手重发一轮产物"的定价信号。
- 对照数与交付：Windows 221 / 52 / 11 / 67 / 56 + `--seamtest` 20 全绿；闭环 18.9s、22.09MB、SHA `MATCH 5B7CA22BD43D`，装的那份同步到本次构建（`--plugintest` 那 12 红还是"右键动作已卸载"那一族，不是新伤）。

## 链接缺口地图：`tools/link_gap.sh`（2026-10-16 深夜）

- 只编不链（CMake 那个 OBJECT 库）回答不了"离能链出 core 还差几步"：编得过的 TU 会引用编不过的那批，链接期才暴露。这个脚本把可移植那 18 个 TU 真去 `-shared -Wl,--no-undefined` 链接一次，让**链接器自己列缺口**。
- 实测结果：**59 个未定义符号**，按归属数下来主要是 —— `PluginHost::*`（`page` / `meta` / `feed` / `reload` / `pluginsDir` …，外加它的 vtable/staticMetaObject）、`FileOps::*`（`whoHas` / `shred` / `killProcess`）、`ShellMenu::syncWithPlugin`、`DevEnv::installLocation` / `applyProxy`、`DownloadManager::changed()` 与 `InfoRow::checkToggled(bool)` 这类 **moc 生成物**（对应 TU 没进来就会缺）。剩下 8 个是 vtable/typeinfo/staticMetaObject —— 那不是函数缺，是"这个 QObject 类的 moc 编译单元没进这批"。
- 所以下一级该做的顺序也清楚了：**先 `PluginHost`**（它是插件能不能加载的核心，而 `QPluginLoader`/`dlopen` 在 Linux 上有真对等物，是最有回报的一道缝），再 `FileOps`（`whoHas` 在 Linux 是 `/proc/*/fd` 扫、`killProcess` 是 `kill(2)`、`shred` 有对等物），`ShellMenu`/`DevEnv` 那两处要么给空实现要么让调用方在别的平台上不引用 —— 那是能力问题，不是工作量问题。
- 用法：`wsl -e bash tools/link_gap.sh`。名单跟 `CMakeLists.txt` 的 `LTOOL_CORE_PORTABLE_SRC` 同源，**改那边要改这边**（脚本里那行注释就是提醒这件事的）。

## 第六道缝：插件产物的命名（2026-10-17 零点）

- 这一刀的规格写错了前提，动手前通读 `src/PluginHost.cpp` 288 行推翻了它：**Win32 装载调用一处都没有**（`LoadLibrary/GetProcAddress` 全仓只剩 `plugins/capture/OcrPp.cpp` 里 onnxruntime 那一颗句柄）。宿主走的是 `QPluginLoader` + `QLibrary`，Qt 自己就跨平台。所以不存在"第六道装载缝"，再包一层 `Platform::PluginLoad` 就是把 Qt 的实现平行做一遍。
- 真正的平台差异只有**文件名**：Windows `ltool-<id>.dll`、Linux `libltool-<id>.so`、macOS `libltool-<id>.dylib`（POSIX 的加载器认 `lib` 前缀）。缝落在 `src/platform/PluginNames.h`，`PluginHost` 那三处写死 `.dll` 的（扫描匹配式 + 两种 `.dll.off` 历史形状）改吃它。做成**头文件内联**而不是 `_win/_posix` 两份 .cpp：每条 OS 就一个常量，拆两份要多两个 TU 还会进 core 导出面 —— 于是 `abi` 栏这轮**还是缺 8 个**，没被这一刀推贵。
- GCC 又当场逮到两处 MSVC 悄悄惯着的：① `QJsonDocument` 用了没 include（靠传递包含编过）；② `QLibrary::resolve` 返回 `QFunctionPointer`，隐式转 `void*` MSVC 当扩展收、GCC 直接 `-fpermissive` 报错，改成显式 `reinterpret_cast`。
- `PluginHost.cpp` 进可移植名单（core 侧 13 → 14 个，连平台五缝共 **19 个 TU，19/19 在 WSL 编过**）。链接缺口 **59 → 54**，掉的那几个正是 `PluginHost::page/meta/feed/reload/pluginsDir` —— 规格里"PluginHost 是最大的一族"这件事本身是对的，只是缺口成因是"这个 TU 没进名单"，不是"它不可移植"。**54 里有 12 个是 vtable/typeinfo/staticMetaObject**：这个探针直接 g++ 不跑 moc，那部分是 AUTOMOC 的活儿，数缺口时先减掉（已在脚本头点名，免得下一轮又把它当实现缺口）。
- `tools/link_gap.sh` 的名单不再手抄，改成从 `CMakeLists.txt` 现读，并加一条**独立取法核对条数**（`grep -oE`）。这条核对第一轮就抓到 awk 解析漏了 2 个 —— 右括号粘在最后一个文件名上（`src/WebsitesPage.cpp)`），漏掉的恰好是 `ProcInfo_posix.cpp`，所以 `Platform::killProcessTree` 还挂在缺口里；而核对用的那条 grep 自己也是错的（字符类里没 `/`，带子目录的路径整条匹配不上）。**两个独立的错互相抓出来才叫证据**。
- 装机复跑抓到一件不在代码里的事：`D:\software\ltool\ltool-plugins` 只有 **17 颗**，比本机构建的 19 颗少了 `network` 和 `shell`。所以那 12 条红不是回归，是"装的这棵缺插件"（其中 7 条是右键七个动作没人认领 + 「工具插件数=19 实到 17」）。同步成 19 颗后：装的那份 `--plugintest 221 / --storetest 67 / --cliptest 56(+1 点名 SKIP) / --seamtest 20`，全 0 红；在跑的实例重启后日志数到 **19 条 `plugin: 看到`**，右键那一路有人接了。这正是 #78 那五颗没进插件仓库的代价 —— 从商店装的新用户同样拿不到这两棵。
- 交付数：`build 7869ms errors=0` / `abi 缺 8 个符号` / 包 **22.09MB** / `install MATCH 0C36B9599360` / 闭环 18.65s。判据（本机构建那棵）：221 / 20 / 67 / 11 / 52 / 56+SKIP / 64 / 44，全 0 红。
- 一条手滑：`powershell -Command "…$_…"` 里的 `$_` 被 bash 吃掉 → **整段解析错误，一条命令都没执行**（我那条"重启实例"就这么静默没跑，回头查 `StartTime` 还是 00:19:58 才发现）；PowerShell 的语法错不是"这一段没跑"，是"全部没跑"。另：一次 `Copy-Item` 目标写成了目录形状，在他插件目录里套出一个空 `ltool-plugins\ltool-plugins\`（确认空之后 `rmdir` 收掉，rmdir 只对空目录生效）。这类事以后一律落成 ASCII `.ps1` 用 `-File` 跑，新增 `build/verify/restart_ltool.ps1`。

## 商店协议去 `.dll` 硬编码（2026-10-17，#78 的第一半）

- 协议侧三处假设一次改齐：`parseIndex` 的文件名校验、`localRows` 的目录扫描匹配式、文件名**反推 id**（原来 `startsWith("ltool-") → mid(6)` 再 `chop(4)`）。命名缝补了 `pluginFilePrefix()` 与 `pluginIdOf()` 两个方向；Windows 侧算出来的字符串逐字等价（判据没动就跑得出等价）。
- 新增一条能红的判据：清单里混进一颗**另一个平台后缀**的 `ltool-foreign.so`，必须被拒，而且拒的理由要点出「这里要 .dll」—— 不是只说"不是 .dll"（那句在 Linux 上等于没说）。`--storetest` 67 → **68**。
- Windows 上"后缀规则改了"这件事本身测不出来（这台机器的后缀就是 dll）。所以去 POSIX 那侧补 **6 条**：`so` / `*.so` / `libltool-shell.so` / 反推回 `shell` / **`ltool-shell.dll` 在这边反推不出 id** / 裸 `.so` 不算插件。`tools/seam_probe.sh` 43 → **49 条，0 红**。
- **真清单实测**：`--storenet` 拉真 index（14 条），新校验下 **拒收 0** —— 这是"协议改了但 Windows 行为没变"最硬的一条证据（假夹具只能证明我自己编的那几条）。
- `--storenet` 另有 **4 条红**，做了真 A/B 才敢定责：把这三个文件退回 HEAD、重编、同一条判据跑出来 **13 过 4 红，红的名字逐字相同** ⇒ 这 4 条在 `25a1e0d` 上就已经红了，CI 不含 storenet 所以一直没被看见（另开任务）。A/B 期间用 `git diff > 补丁` + `git checkout --` + `git apply` 走，不碰 stash。
- 自测夹具里 **57 处** `.dll` 字面量换成从缝里现算。批量替换用的 python 脚本在 raw 字符串里写了 `\'`，**37 处非法转义原样进了源码**，MSVC 当场 `C2017/C2001` 红 —— 幸好报错了：那一轮我差点把**旧产物**跑出来的 536 全绿当成本轮的数（`build rc=1` 之后 exe 的 mtime 才是唯一可信的判据）。
- 装机那条 `其中 0 颗和本机构建同一份` 不是坏信号：core 一重链，19 颗插件的哈希必然全变。新增 `build/verify/sync_plugins.ps1`（停实例 → 拷 → 数哈希 → 重启），装的那棵 19 颗与构建**逐颗一致**，这条用 python 独立核过一遍，没信脚本自报的数。
- 对照数：本机 221 / **68** / 20 / 11 / 52 / 56+SKIP / 64 / 44 = **536 条 0 红**；装的那份 221 / 68 / 20 / 11 / 56+SKIP，`--storenet` 13/4（同 HEAD）；Linux 19/19 TU 编过、链接缺口仍 54。闭环 19.0s、22.1MB、`install MATCH DDA93318A2AB`。

## #94：`--storenet` 那 4 条红（2026-10-17 凌晨定案，红的不是产品，是判据认字法）

- 现象：`有状态 5 / 卡片 19`、`14 张不对（一张没有状态词的卡）`、`可更新 0 张，缺箭头 0 张`、`禁用/启用 19 颗 / 装着 5 张`。
- 取证先加再解释：把"没有状态词的卡"改成**打印那张卡的全部标签**。一跑就定死了 —— 那 14 张写的是 `↑ 可更新 v1.0.0 · 0.10 MB`，界面说实话，**是判据拿 `startsWith("可更新")` 认不出带箭头前缀的写法**。`可更新 0 张、缺箭头 0 张` 那条更是空转：`upCards=0` 让它永远"通过"。
- 修法是回到数据层，不是放宽阈值：`ToolsPage::makeCard` 给卡片挂 `ltoolState` 属性（状态那个枚举），判据**先认属性、再要求界面那句里确实出现了对应的词**（允许 `↑` 前缀与 `(v…)` 后缀）。箭头那条同样改法。两边都要对上：状态有、字没画，照样红。
- 数字：`--storenet` 13 过 4 红 → **17 过 0 红**（`可更新 14 张，缺箭头 0 张` 第一次是真数出来的）；其余判据一字没动（221 / 68 / 20 / 11 / 52 / 56+SKIP / 64 / 44 = 536，0 红）。
- 为什么一直没被发现：`--storenet` 要联网，**不在 CI 那串旗标里**；而且它红的时候退出码非 0、没人跑。教训一条：**"这条判据上次跑是什么时候"要说得出**，否则它是不是在测东西没人知道（这次 `upCards=0` 就是装饰了很久的绿）。

## 链接缺口从 54 掉到 8：#93 的真相是探针少给了两样依赖（2026-10-17 凌晨）

- 起因是 #93 那条"54 个里 12 个是 moc 产物，先脑内减掉"。**减掉不如补上**：脚本现在自己跑 moc（名单里每个 TU 的同名头，带 `Q_OBJECT` 就过一次，实测 5 个），再把 `pkg-config --libs libarchive` 链上 —— CMake 那边 portable 库本来就 `find_package(LibArchive)` 链着，探针不链就会多算十来个 `archive_*`"缺口"。
- 结果：**54 → 8**（元对象族 1 个 = `SettingsDialog::staticMetaObject`，那是那个类的 TU 还没进名单；真缺口 7 个）。剩下就是四个类族：`DevEnv::applyProxy` / `installLocation`、`FileOps::killProcess` / `shred` / `whoHas`、`SevenZip::compress`、`ShellMenu::syncWithPlugin`。
- **我上两轮报出去的 59 和 54，大头是测量方法自己造的**，不是实现缺那么多。归因顺序记一条：报"还差 N 个"之前，先核对**探针的依赖集和真实构建一致不一致**（moc、第三方库、编译旗标），差一样就虚高一截；能补进探针就别写在注释里让人脑内减。
- 顺手核实掉一条我以为的真缺口：`Archives.cpp` 调的 `archive_write_open_filename_w` 这类宽字符入口，**Linux 的 libarchive 也导出**（`nm -D /usr/lib/x86_64-linux-gnu/libarchive.so` 里命中 1 个），所以它不是 #90 的活。写这句前先查过，别再拿"Windows 才有的 API"当直觉结论。
- 这一改**只动 `tools/link_gap.sh`**：没碰任何 C++、没有产品代码变化，所以没跑 deliver（产物与上一轮逐字节相同，跑了只会白杀一次他的实例）。

## 商店「可更新」的语义改成版本优先（2026-10-17，他点了方案 2）

- 实测起因：`--storenet` 修好后第一次对上真实界面 —— 19 张卡里 14 张报「可更新」。查清单：14 条 `version` 全是 `1.0.0`，本机 14 颗哈希与清单 **0 颗一致** ⇒ 旧的 `compare()` **只比哈希**，版本号一次都没参与。后果不是"我机器特殊"：商店 CI 每次跑都重编，MSVC 产物哈希必变，**所有人每次发版都会看到一片「可更新」**。
- 新语义：`Update` 只留给「清单版本号 > 本机版本号」；哈希不同但版本不比本机新 → 新增一档 `Different`，状态词「与商店不同」，按钮「换成商店那份」（不是「更新」），提示分别说"版本号都是 v… 但内容不同"和"本机 v… 比商店还新，按它会降级"。一侧读不到版本号也走 `Different`（说不清就不许当新版卖）。
- 本机版本号从哪来：`PluginHost::metaVersionOf(path)` —— `QPluginLoader::metaData()` 只读嵌在产物里那份 plugin.json，**不装载**。
- 卡片文案顺手改掉一处自相矛盾：`↑ 可更新 v1.0.0` 里那个版本号印的是**本机自己**那份（`localRows` 填的就是本地元数据），读起来像"要更新到 1.0.0"。两档有两个版本号的（可更新/与商店不同）不再拼 `(v…)`，版本对照进悬停。
- 版本号比较从「关于」页挪进 `Utils::compareVersions`（商店也要同一套规矩，抄第二遍迟早分叉；`AboutPage::compareVersions` 与那份 `segNum` 删掉，8 条判据改指 Utils）。
- 判据：`--storetest` 68 → **73**（新增"借一颗真插件读自己版本号" + 清单更高/一样/更旧/读不到 四档）。第一版把借来的真 dll 放进了别人要数插件颗数的那个目录，**红了两条**（`6 应 4`、`5/1 应 3/1`）—— 新夹具改放自己的子目录。`--storenet` 17/0，且那条「可更新有绿箭头」不再假设世界上一定有可更新的卡，改成把**本次量的张数**写进名字与实值（现在是 `可更新 0 张`，诚实的 0）。
- 真清单实测（装好的那份）：状态行 `19 张卡 · 能装或可更新 0 · 拒收 0 · 清单外 5`，14 张走「与商店不同」——**这就是他现在界面会看到的样子**。
- 欠着的一条（#78 落地时要一起拍）：19 颗 `plugin.json` 的 `Version` 全写死 `1.0.0`，没人负责递增；发版纪律不加，这套版本判定就永远是 `Different`。

## 插件版本号改成「每颗自己递增」，并给它立一道闸（2026-10-17，他拍的形状）

- 形状：号不跟 core 走，改了哪颗那颗自己 +1（末位；接口或行为大改自己进位）。core 那边改了不动插件号 —— 插件 dll 是导入 `ltoolcore.dll` 的，core 变了它字节也没变，那不在这条纪律的范围里。
- 闸：`tools/plugin_version_gate.sh [基线ref]`，默认基线 = 最近一个 tag（发出去过、用户按那批号装过的那个时刻）。只管 `plugins/<id>/` 里除 `plugin.json` 之外的文件：源码改了而 `Version` 一模一样 → `NEEDS_BUMP <id>`；号往回走 → `VERSION_WENT_BACK <id>`；基线上没这颗（tag 之后新建）→ `NEW_SINCE_BASE`，1.0.0 就是它第一号，不要求递增。计数行一律 ASCII（`GATE_PLUGINS=` 等），这台机器 PowerShell 码页 GBK，拿中文行 grep 计数会读出 0。
- 标签那次 run 的空转防线：基线 tag 正好指着 HEAD 时（CI 里 tag push 就长这样）自动往前退一个 tag，否则差集永远是空集 —— 那是装饰性的绿。实测 `bash tools/plugin_version_gate.sh $(git rev-parse HEAD)` 打印「基线退到 v1.0.3」，量的还是那 19 颗。
- 一次性基线：v1.0.3 之后源码动过的 12 颗里，9 颗（capture / downloads / language / record / registry / repair / shell / software / startup）1.0.0 → 1.0.1；另 3 颗（clipboard / grid / linuxdo）是 v1.0.3 之后新建的，留在 1.0.0。bump 前闸报 `NEEDS_BUMP=9`，bump 后报 0。
- 反向验证：shell 改回 1.0.0 → 闸点名 shell、`NEEDS_BUMP=1`、退出码 1；capture 改成 0.9.0 → `VERSION_WENT_BACK`、退出码 1；还原后回到 0。两个分支都在真对象上红过，不是写着好看。
- 判据：`--plugintest` 加一条「plugin.json 的版本号是 x.y.z 形状 [实值]」，在 19 颗上量的（221 → **240**）。拿一颗真插件（json）把 Version 写成 `1.0` 重编，那条当场红且把 `[1.0]` 印在名字里；还原重编回到 240/0。为什么要判形状：`PluginStore::compare` 里一侧版本号空或垃圾就走「说不清有没有新版」，那颗永远等不到「可更新」。
- CI：`.github/workflows/build.yml` 的 checkout 改 `fetch-depth: 0` + `fetch-tags: true`（默认那次浅克隆里 `git describe --tags` 直接失败，闸会报 `GATE_BASELINE=none` 而不是假装通过），闸放在装依赖之前，秒级失败。**插件仓库那份 workflow 没装这道闸**：它 `plugins/` 是从本仓库拷过去的快照，基线该跟谁比不归这条纪律管 —— 要装就连 #78 那批对外动作一起定。
- 后果说清：商店清单 14 条版本号还是 1.0.0（现拉的那份 index.json 逐条看过：ads base64 capture clipboard disk json language network record registry repair shell startup user）⇒「与商店不同」那 14 张里 **7 张**现在写的是"本机 v1.0.1 比商店 v1.0.0 还新…按它会降级"：capture / language / record / registry / repair / shell / startup。bump 了的另两颗（software、downloads）清单里根本没有，走「清单外」——这一面 5 张就是 downloads grid linuxdo software tasks。这两个数是 `--storenet` 新加的 INFO 行现量的（各档分布 + 那个 7），不是我拿 9 减出来猜的（猜的那一遍真错了：software 其实不在清单里，startup 在）。等他把插件产物重发一批（#78 对外的下一步），两边同号才回到「已安装」。`Version` 是嵌在 dll 里的元数据 ⇒ 那 9 颗产物字节确实变了，装的那份要重新同步（本轮 sync 后 19/19 与构建逐颗一致）。
- 数字：`--plugintest` 240/0、`--storetest` 73/0、seamtest 20/0、cfgcheck 11/0、gridtest 64/0、pinyin 52/0、linuxdotest 44/0、cliptest 56/0+1 SKIP；`--storenet` 16 过 1 红，红的是一条判据之外的东西 —— `检查更新走网页 302` 拿到 HTTP 0。同一条 curl 分测两条路：直连 8.0s 超时（code=000），经本机 9980 代理 0.83s 拿到 302。同一轮 40 分钟前那条还是 200，所以红的是**这台的出口**，不是这轮的刀；`--storenet` 要联网又不在 CI 里，这条以后还得靠"上次跑是什么时候"盯着。

## 第八刀：FileOps 抽成 platform 那道缝（2026-10-17，链接缺口 8 → 5）

- 为什么开在这里：`src/FileTools.cpp` 早在可移植名单里，而它调的三颗（`shred` / `whoHas` / `killProcess`）实现全在 `src/FileOps.cpp` —— 那个 TU 整个卡在 Win32 上，链接期就缺这三个符号。
- 切法：只抽真的分叉的两个动作 `shredOne` / `removeEmptyDir`；「先走文件、目录从最深往上删、最后删自己」那套顺序两边一模一样，所以写成 `src/platform/FileOpsShred.h` 里一份 `inline shredWalk` —— 抄第二遍迟早分叉这条已经量过代价了。各平台一份的只有 `whoHas`（Windows 问 Restart Manager，POSIX 扫 `/proc/<pid>/fd` 按 st_dev+st_ino 比对）和 `killProcess`。
- `src/FileOps.cpp` 只剩 `info` / `usage` / `human`：那三件要的是盘符、簇、NTFS 压缩大小，POSIX 没有对等概念，所以它不进可移植名单 —— 不是没做完，是不该在那边做。
- POSIX 那三份是真实现不是空壳：覆写走 `open(O_WRONLY)` + `write` + `fsync` + `ftruncate`，删除走 `unlink`，thorough 走 `rename` + `unlink`；占用扫描认得出 `tail -F` 那颗 pid；结束进程先 SIGTERM 等 2 秒再 SIGKILL，僵尸算已经没了（`kill(pid,0)` 对别人的进程返回 EPERM，不能当成"不在了"）。
- 两条平台边界要说清（不是降级）：① 不是 root 只看得到同一用户的进程，读不到别人的 fd 目录就跳过 ⇒ 空表不等于"没人占用"；② Windows 那句 `SetFileAttributesW(NORMAL)` 这边没有对等物也不需要 —— POSIX 删文件看的是**所在目录**的写权限，不是文件属性。还有一条别混：`FileOps::killProcess` 只杀点名的那颗（和 Windows 一致），要连子孙的是 `Platform::killProcessTree`（取消安装那一路）。
- Windows 行为零变化的证据：搬家前后各跑一遍同一批夹具（`--shredtest` 3MB 浅/3MB thorough/整棵目录树/不存在 + `--whohastest` 无主/有人开着并 kill），把 pid 和路径归一之后**逐行相同** —— `erased=3145728`、`erased=12288`、`erased=-1 文件不存在`、`holders=1 → kill ok → 结束后 0`。为了这条能量出来，顺手给 `--whohastest` 加了 `kill` 那个参数（结束逐个占用者再数一遍）。
- 判据：`--seamtest` 20 → **28**（两边共用同一组期望：粉碎报的字节数、真没了、乱名 tmp 不留、目录树 12288、不存在的返回 -1 且有原因、无主是空表）。「自己开着的文件认不认得出自己」只打 INFO 不当判据 —— Restart Manager 和 `/proc` 对"看得见查询者本身"本来就不是同一套约定，本机实测 Windows=1 个。POSIX 探针 49 → **63**（同一批断言 + 拿 `tail -F` 当占用者的端到端：认出 pid、杀得掉、杀完空表、再杀老实报"没有那个进程"）。
- 数字：链接缺口 8 → **5**（元对象族 1 = `SettingsDialog::staticMetaObject`，那个 TU 还没进名单；真缺口 4：`DevEnv::applyProxy`、`DevEnv::installLocation`、`SevenZip::compress`、`ShellMenu::syncWithPlugin`）。离线判据 560 → **568** 条 0 红；装的那份 `--seamtest` 28/0、`--plugintest` 240/0。`abi` 仍缺 8 个符号（这轮只加名字没删名字，商店那批导入的符号一个没少）。闭环 24.7s、22.1MB、`install MATCH 678B269C76CA`，sync 后 19/19 与构建逐颗一致。
- 这轮踩的一条工具链坑（`edit-anchor-newline-hazard` 的新变体）：用 bash heredoc 喂 python 往脚本里插代码时，源码里的「反斜杠 + n」被吃成"真换行"，于是 `seam_probe.sh` 里出现 C 字符串断成两行的 `std::printf` 和 g++ 命令行末尾一个裸的反斜杠 n（`ld: cannot find n`）。两处都是拿 Edit 落的才修好 —— 以后往脚本/源码里插带转义的代码，别走 heredoc-python 这条路。
- 版本判断：**不升**。Windows 这侧是搬家（六个场景逐行相同、abi 没变），POSIX 那侧还没产出用户拿得到的东西。

## 第九刀：DevEnv 的代理与安装根抽成第八道缝（2026-10-17，链接缺口 5 → 3）

- 缝开在哪：`DevEnv::applyProxy` / `installLocation` 的**动作两边共用**，不一样的只有两件事 —— 系统代理从哪读、装东西的根在这台机器上长什么样。所以新增 `src/platform/SysConfig.h`：`systemProxyUrl()` + `defaultInstallRoot()` + `usableInstallRoot()` 各平台一份，`normalizeProxy()`（把 host:port 补成 URL 那条规矩）两边共用一份 inline，不抄第二遍。
- 另外新建一个可移植 TU `src/DevEnvConfig.cpp`（Windows 的 core 名单和 Linux 的可移植名单里都有它）：`proxyUrl` / `applyProxy` / `installLocation` 住这儿。为什么非要新 TU：`src/DownloadManager.cpp` 和 `src/PluginStore.cpp` 早就在可移植名单里，链接期要找得到这几个符号，而 `src/DevEnv.cpp` 整个还卡在注册表那套 Win32 上。
- `DevEnv::systemProxyUrl` 只剩一层转接，为什么不删：配置页那句「系统代理是 X」还在读这个名字，而且 **core 的导出面 = 已发布插件的 ABI，名字可以加不可以缩**（#91 那条）。这轮的 abi 实测仍是缺 8 个符号 —— 没删任何名字的直接证据。
- 两条语义在这轮被明确钉住（都是以前只写在注释里的）：① **Windows 上「安装目录留空」= 不指定 --location**，不是"用默认根"；「没配过」才给默认根。搬家时把这两种情况分开处理（`cfg.contains` 而不是 `cfg.value(带默认)`），就是为了不改这条语义。② POSIX 上没有"不指定装哪"这回事，所以空 / Windows 盘符写法 / 相对路径一律退到 `~/software` —— 装的根必须真的能用，不能跟着工作目录跑。
- POSIX 的代理是真对等不是降级：读 `http_proxy` / `HTTP_PROXY` / `https_proxy` / `HTTPS_PROXY`（curl、wget、pip、git 全认这套）。一条**宁缺不假**：值是 `socks5://…` 时返回空，因为下游 `applyProxy` 只会摆 `QNetworkProxy::HttpProxy`，把它报成 http 代理是骗人。PAC/自动发现两边都解不了（Windows 那半本来也解不了）。
- Windows 行为零变化的证据：`--seamtest` 搬家前后各跑一遍，**逐行相同**（31 条时），包括新加的三条真值断言打出来的 `INFO 生效代理=[] 系统那半=[]` —— 这台机器 `net/proxy` 留空、注册表那个 9980 又没被设成系统代理，所以那三条走的是"没代理"那一支。**这就是为什么还要用夹具量规则本身**：`normalizeProxy` 三条 + `usableInstallRoot` 三条（Windows 的 `Z:` 退 `C:`、留空还是留空、C 盘和网络路径不动），否则"有代理"那一支永远没被人碰过 = 装饰性的绿。
- 判据与工具：`--seamtest` 28 → **37**；POSIX 探针 `tools/seam_probe.sh` 63 → **69**（代理那四条用 `qputenv` 摆现场，不依赖跑的人机器上有没有代理）。链接缺口 **5 → 3**：只剩 `SevenZip::compress`、`ShellMenu::syncWithPlugin`（就是要他拍形状的那两个，#90）+ `SettingsDialog::staticMetaObject`（那个 TU 还没进名单，不算实现欠账）。可移植 TU **19 → 22**（+FileOps_posix、+SysConfig_posix、+DevEnvConfig）。
- 顺手用真 CMake 在 WSL 复核了一遍（不只我那两个探针脚本）：`cmake -S . -B /tmp/... && cmake --build --target ltool_portable` 过了，configure 打出「名单里那 15 个 TU」。
- 数字：离线判据 568 → **577** 条 0 红；装的那份 `--seamtest` 37/0、`--plugintest` 240/0、`--storenet` 16 过 1 红（又是「检查更新走网页 302」那条：GitHub 直连这台机器现在不通，走 9980 代理 0.83 秒就 302 —— 而商店清单那条走镜像是通的，`状态行` 里写着「走的是 镜像 gh-proxy.com」）。闭环 20.0s、22.11MB、`install MATCH A76D3BD0E3CB`，sync 后 19/19 与构建逐颗一致。
- 版本判断：**不升**（Windows 侧行为逐行相同，POSIX 侧还没有用户拿得到的产物；插件号也没动 —— 这轮改的是 core）。

## 第十刀：能进的先进来（可移植 TU 15 → 18，链接缺口仍是 3）

- 先量地图而不是猜：对 core 的每个 TU 跑 `g++ -fsyntax-only`，结果是"能编的 15 个 == 名单里的 15 个"，也就是说没有白捡的；然后按 Win32 触点给剩下 20 个排了个厚度（RegOpenKeyEx/HKEY 一堆的：RegClean 60 处、ShellMenu 12、StartupAudit 20、ConfigPage/EnvPath；只有 HANDLE/DWORD 几个的：DiskScan、AdBlocker）。
- **我第一版结论是错的，被 MSVC 打回来的**：三个 TU（ClipboardStore / InstallLogDialog / TaskScheduler）挂着 `#include <windows.h>` 却"grep 不到任何 Win32 入口"，我就写下"这是早期留下的空壳"。删掉 include 之后 GCC 那边过了、MSVC 当场报 `GetClipboardSequenceNumber 找不到标识符` 和 `CREATE_NO_WINDOW 未声明` —— 我的 token 名单是自己编的，不在名单里的 API 就等于"不存在"，这不算测量。真正的测法是**把 include 删掉、两个编译器各跑一遍**，只有一个编译器说话不算。
- 收的一道新缝：`Platform::runWithoutConsole(QProcess*)`（`platform/Launch.h`）。原来临时起的进程要藏控制台窗口，全项目写了 **8 遍**同一个三行 lambda（DevEnv 6、InstallLogDialog 1、TaskScheduler 1，外加 TaskScheduler 自己那个 `noWindow` 小别名）；现在 8 处都走缝，Windows 那份挂 CREATE_NO_WINDOW，POSIX 那份是**空动作**（这里没有控制台窗口可藏，不是降级）。黑窗弹没弹在无界面下量不到，所以判据只盯能量的一半：过了这道缝的进程照样起得来、输出读得到（`--seamtest` 37 → **38**）。
- 三颗 TU 真进名单：`AboutPage.cpp`（自己一个 Win32 入口都没有，卡的只是 `LTOOL_VERSION` 那条 `#error` —— CMake 那边这个宏本来就 PUBLIC 挂在 ltoolcore 上，portable 目标也得给同一个串，来源仍是 ltool.iss 一处）、`InstallLogDialog.cpp`、`TaskScheduler.cpp`（后两颗要的"不弹控制台"进了缝之后就没别的 Win32 了；计划任务其实是起 schtasks 进程，不是 COM）。
- `ClipboardStore.cpp` **留在名单外**：它靠 `GetClipboardSequenceNumber()` 判"剪贴板到底变了没有"（界面上那条"自己写的不算"就靠它，注释里写得很细 —— restore() 会同步发 dataChanged，序号是写完才变的）。POSIX 没有对等的计数器，硬做等于换一套机制、改行为 ⇒ 记成 #90 要拍形状的第五件。
- 探针跟着修了两处自己的失真：`tools/link_gap.sh` 以前不认名单里的**行尾注释**（我加 TU 时带了说明，awk 的"这行以 ) 结尾"判断被注释带偏，最后一颗整条漏掉，awk 25 / grep 26 —— 幸好那条独立核对还在，把解析漏洞当场抓出来了）；两边现在都先剥 `#` 后面的内容再数。另一处是 AboutPage 进名单后探针不给 `LTOOL_VERSION`，那 1 个 TU 的 `#error` 会被念成实现缺口 —— 和当初 moc / libarchive 是同一类错。
- 数字：可移植 TU core 15 → **18**（+7 道缝文件 = 探针里 25 个），`g++ -fsyntax-only` 全过、**真 CMake 在 WSL 上 `--target ltool_portable` 构建过了**；链接缺口仍是 **3**（`SevenZip::compress`、`ShellMenu::syncWithPlugin` 这两个要拍形状 + `SettingsDialog::staticMetaObject` 那个 TU 还没进名单）。POSIX 探针 69/0。Windows 侧离线判据 **578 条 0 红**（`--plugintest` 240、`--storetest` 73、`--seamtest` 38、cfgcheck 11、gridtest 64、pinyin 52、linuxdotest 44、cliptest 56+1 SKIP）。abi 仍缺 8（没删名字）。闭环 19.0s、22.11MB、`install MATCH FDF38B0E1BD6`，sync 后 19/19 与构建逐颗一致。
- 版本判断：**不升**。Windows 侧 8 处调用是同一句旗标应用搬进缝（进程照常起、输出照常读，578 条判据一字没变），POSIX 侧还没有用户拿得到的产物。

## 跨平台地图更新（2026-10-17 夜，第十刀之后量的，不是印象）

- 现在能编的 core TU 是 18 个，**正好等于名单里的 18 个** —— 也就是说"白捡的"已经捡完了，剩下 20 个每个都要动一次结构。
- 剩下 20 个按卡住的东西分类（每个都是 grep + 两个编译器实测出来的）：注册表那一族 5 个（EnvPath 最薄：只有 RegOpenKeyExW / RegSetValueExW / RegGetValueW；RegClean 60 处 Reg 系列最厚）；机制级要拍形状的 6 个（SevenZip 卡 bit7z、UsnIndex/FileIndexer/DiskScan 卡 USN-MFT 和 FindFirstFileExW 那套快速枚举、AdBlocker 卡 EnumWindows + DWM 取标题、ClipboardStore 卡 GetClipboardSequenceNumber）；纯界面的那几个（ConfigPage / SearchWindow / SettingsDialog / StartupAudit / Firewall / AppScanner / ShellMenu）各带注册表或 COM。
- 一处我读错的规格顺手改掉：`SettingsDialog` 的 showEvent 里除了 `SetWindowPos` 还有 `DwmSetWindowAttribute`（深色标题栏）和 `SetForegroundWindow` —— 只把 SetWindowPos 抽进缝**并不能**让这颗 TU 进名单，那条"消掉 staticMetaObject 缺口"的说法作废。
- 干净的 on/off 置顶其实只剩一处（`PinWindow::applyTopmost`，旗标是 NOMOVE|NOSIZE|NOACTIVATE）；`CaptureOverlay` 那颗和抢焦点那一整套（按 Alt、接前台线程输入队列、重试、写日志）缠在一起，不能跟着一起搬 —— 所以"setTopMost 那道缝"这一刀的价值只是少抄一遍，不推名单，先排在 EnvPath 后面。
- 下一刀定在 **EnvPath**：Windows 那份逐字保住行为，POSIX 那份有真对等做法（`~/.config/environment.d` 那套 + shell rc 追加），做完 core 可移植 TU 18 → 19。它不欠任何人拍板。

## 「Windows 用户名」这页：模态框被隐藏的启动器吞了（2026-10-17 下午，他报"改名失败 + 页面太丑"）

- 他报的两件事里，"失败"这条是有证据的：**整份 log.txt 里 `username:` 一条都没有**（同期 `linuxdo:` 307 行、`clipboard:` 534 行 ⇒ 插件写日志的通路正常），而他答问卷时说的是「没弹授权窗就失败」⇒ 三颗按钮每次都在**提权之前**就 return 了。
- 根因是那颗**模态确认框**：这页是从启动器里叫出来的，而启动器一失焦就把自己藏起来（Alt+Space 那套宽限/最小化判据改过几次）——挂在它里面的 `QMessageBox::question` 跟着看不见，Esc/超时都算「不是 Yes」，于是哪儿也没去，界面还停在原来那句说明文字上（他读成"显示修改成功"）。
- 形状按他挑的**方案 B**重做：单列到底、动作一律在行尾（摘要行 + 重读、显示名 + 改、账户名 + 改、一条分隔、新账户三个输入 + 建、管理员勾选），**说明/结果那一行挪到所有动作行下面并常驻** —— 它会换文字、可能从一行变两行，摆在上面一按就把输入框往下推（他否过"点一下突然多出一块"），摆在底下上面那颗都不动。
- 确认改成**就地两段**：第一下把同一颗按钮变成「确认改成这个」并亮 6 秒，第二下才动手；点另一颗会把前一颗收回（同时只有一颗在武装）；校验没过也写在同一行，不再弹任何框（`QMessageBox` 的 include 已从这两个文件消失）。
- 判据 27 → **36**：四个动作都在行尾（数每行 QHBoxLayout 的最后一项）、摘要行一行摆完、点第一下按钮换了字、`activeModalWidget()` 仍然是空的、**第一下不许把上面任何一颗挪走**（比三行的 y 坐标）、只留一颗武装、收尾那条写明"安全靠结构不靠这条断言"。几何断言两个坑都踩过才修对：① 页面没排开时三个 y 全是 0 ⇒ 加了一条「三行的 y 必须是互不相同的正数」（实测 39/109/139）；② 点完没重排就读坐标 ⇒ 等于拿旧几何比，改成 `invalidate()+activate()+processEvents()` 十轮。
- **我自己造成的两件事，都记在这**：① 上面那条几何判据我第一版是"永远相等"的空转绿，是拿"点一下插一块进去"的破坏试验照出来的（第一次破坏试验还因为我的 sed 没接上 `addWidget(m_hint)`，变成"说明行根本不在布局里"这种假验证）；② **判据里我对同一颗按钮点了第二下 —— 第二下就是"确认"，于是真的对他账户发起了一次「改显示名」**（16:00:28 与 16:01:08 两条 `username: 改显示名 ok=0 ... 退出码不是 0（1）` 都是我）。他的账户**没有被改动**（`Get-LocalUser` 的 `FullName` 现在仍是空，和之前一致），但那是一次不该发生的提权尝试。已把收尾改成"点另一颗来收回"，并在代码里写死这条规矩：**测试里绝不对同一颗两段确认按钮点第二下**。
- 还欠一条（没做，已开任务）：提权失败现在只能说「退出码不是 0（1）」——真实原因在提权那个进程的 stderr 里，被 `ShellExecuteEx` 吃掉了。要把它带回界面得让脚本把 `$_.Exception.Message` 写进一个临时文件（路径走环境变量传，别拼进脚本文本），跑完读回来。
- 数字：`--username` 36/0，离线判据 578 → **614 条 0 红**（578 那半不含 username；622 是我加错过的一次，已按各旗标实测数重算：240+73+38+11+64+52+44+56+36）（plugintest 240 / storetest 73 / seamtest 38 / cfgcheck 11 / gridtest 64 / pinyin 52 / linuxdotest 44 / cliptest 56 / username 36）；这颗插件按新规矩 1.0.1 → **1.0.2**，闸绿；`install MATCH FDF38B0E1BD6`（exe 与上一轮同哈希，改的只有插件），sync 后 19/19 与构建逐颗一致。core 的号不升（1.1.0 仍未发布），这轮是**用户可感知的新形状**，发版时要写进 release note。

## 磁盘清理·第一刀：把 light-c 的地基搬进来（MFT 直读），顺手抓到深索引一条从没生效的分支（2026-10-17 晚）

- 来源是他给的 `Chunyu33/light-c`（Rust+Tauri 的"高性能 C 盘清理"）。它的 `engine_selector.rs` 三条决定照抄了：`提权 && NTFS` 才走 MFT、MFT 失败**自动**降级遍历、**并且把这次实际用了哪条路显示给界面看**。
- 我们没有平行实现第二套枚举：`UsnIndex::enumVolumeMft`（深索引在用）已经给了"谁在谁下面 + 叫什么"。**缺的只有大小** —— USN 记录里不带大小，它在每条 FILE 记录的 `$STANDARD_INFORMATION` 里，所以新增 `UsnIndex::mftSizes()`：把 `$MFT` 顺序读一遍，按记录号 head/next 链表一次填完（硬链接多条名字各拿一份，和遍历口径一致）。
- **抓到一条真 bug**：`openVolume` 拼的是 `\.\C:\`（结尾多一个反斜杠）。实测四种写法：`\.\C:` → err **5 ACCESS_DENIED**（形式对，只差提权）；`\.\C:\` → err **3 PATH_NOT_FOUND**。也就是说**深索引那条「MFT 优先」从来没生效过**，一直在走遍历回退 —— 它日志里那句 `volume open failed ... 回退目录遍历` 就是这个。判据里钉了一条"打不开卷的原因不能是错误码 3"。
- `$MFT` 的打开形式也量过：`\.\C:$MFT` → err 2（文件不存在），`C:\$MFT` → err 5（只差提权）。别拿裸设备路径去拼文件名。
- 记录长度不靠猜 `$Boot` 的偏移（那格有版本差异）：拿 `FILE` 签名在 256/512/1024/2048/4096 里找周期，连续四格都对上才算数。
- 两条扫描路共用同一颗尾巴 `finishTree()`（自下而上求和 → 孩子表 → 每层按大小排序剪枝）。分开的理由写在函数头上：树的形状一旦按引擎不同，界面上的排行和下钻就会表现不同。
- 提权的形状：ltool 是启动器，平时**不该**提权跑。所以「直读 MFT 重扫」起一个提权的自己执行 `ltool.exe --mftree <盘> <文件>` —— 它只做一件事：读、把树写成二进制快照、退出（0=好了 / 2=这个卷走不了 MFT / 1=别的失败）；界面读回来之后把那个临时文件删掉（只删算出来的那一个绝对路径）。快照带魔数 + 版本号，对不上整份丢掉不猜。
- 界面说实话：定稿状态行点名"这次走的是 MFT 直读还是常规遍历"，回退还把**为什么没走快的**一起带上；MFT 途中的进度句子换成"已枚举多少条记录"，不拿"已统计多少个目录"冒充另一条路的数字。
- `--mfttest` 从"一行断言都没有的诊断输出"改成真判据：本机（非提权）**11 条 = 10 绿 + 1 条点名 SKIP**；提权机器上再多跑 7 条（枚举拿到多少记录、$MFT 大小读不读得出来、合计像不像真盘、这次走的是哪条路、整盘树、与文件系统已用量的量级对账、System32 逐目录 ±10% 对账）。
- CI 里 `--mfttest` 从"要管理员，跳过"挪进真跑那一串：runner 是提权的（`--username` 早就读到过 `runneradmin@…`），所以那两条对账判据在 CI 里有真对象。
- 数字：`--mfttest` 本机 11 条（10 绿 + 1 SKIP）、`--diskui` 8/0、`--plugintest` 240/0（改 Desc 时它当场判红了一次：插件自报描述和 plugin.json 必须一字不差，这条闸是有用的）；那九串离线判据 614 → **625**。本机 `mftUsable(C:/)=0 why=[没有管理员权限，打不开卷（错误码 5）]` ⇒ **MFT 那条路本机的实测耗时还欠着**，要么他给一次 UAC，要么看 CI。
- 连带账：`Tree` 多了 engine/engineNote、`Progress` 多了 engine、core 多导出 `mftUsable/writeTree/readTree/engineMft/engineWalk` ⇒ 已发布的那颗 `ltool-disk.dll` 和这版 core **ABI 对不上**（#91/#78 那笔"商店产物要重推"的老账又多一处）。disk 这颗 1.0.0 → **1.0.1**；core 的号不升（1.1.0 还没发布，同批功能吸进同一个号）。
- 下一刀（#103）：清理位补齐 + 按类别分组（回收站现在根本没进清单，而他树上 `C:\$Recycle.Bin` 是 5.19 GB）。
- **CI 第一次跑就红了，红的不是判据是预算**（run 39026813012，`--mfttest` exit **124** = timeout）：runner 确实是提权的（日志第一行 `INFO mftUsable(C:/)=1`），于是我给的那条"整盘再走一遍遍历对账"在 VM 的盘上一分多钟，把 60 秒的步长预算吃穿了。改法两处：① 逐目录对账**只走一个子树**（`C:\Windows\System32`），整盘那遍遍历砍掉 —— 它证明的是同一件事（$MFT 的大小解析对不对），代价从一分多钟降到几秒；② 补一条不花时间的**第三条独立量法**：MFT 整盘合计 vs 文件系统自己报的"已用"，只抓"差一个量级"（口径天生不同：簇分配量 vs 逻辑大小，compact OS 的压缩文件会让逻辑明显更大，所以容差给的是 /2 ~ ×3，不是 ±10%）；③ `--mfttest` 单独给 240 秒。教训记一条：**判据里凡是要真扫整盘的，先量它在最慢的那台机器（CI 的 VM）上要多久**，本机几百毫秒不等于 CI 几百毫秒。
- 装上心跳之后第三次 CI 自己交代了（run 39029141013，日志原文照抄）：`STAGE 枚举到 1669141 条记录（累计 7250ms）` → `STAGE 1398724 个文件要去 $MFT 里问大小` → `STAGE MFT 直读没走通（$MFT 大小没读出来（错误码 5）），回退常规遍历` → 然后 240 秒到死。**三件事一次分清**：① `enumVolumeMft` 是好的（166 万条 / 7.25 秒，这条函数在提权下第一次真跑）；② 卡的是 `C:\$MFT` 这个写法 —— 提权的 runner 上枚举卷能过、开 `$MFT` 却给 ACCESS_DENIED，说明它走的是普通路径名查找，得换 `\.\C:\$MFT`（卷设备那颗，取证工具普遍用的写法；本机非提权下两种都是 5，形式都对，分不出来）；③ **真正烧掉 240 秒的是回退之后那趟遍历** —— CI 那台 VM 上整盘遍历比我们这台 NVMe 上的 14~19 秒慢一个数量级还不止。
- 于是判据的形状也跟着改了：**先把 MFT 那两步单独量、单独判**（枚举出多少条 / 大小读不读得出来 / 合计像不像真盘），两步有一步没成就**不去跑整盘**（点名 SKIP 那三条），因为那个跑法必然掉进四分钟的遍历。教训：**判据不许在"实现失败"这条分支上顺手触发一个更慢的兜底** —— 那等于把一条本该三秒判红的红，变成一次四分钟的超时。
- **追到第九轮才看清的墙**：`$MFT` 在现代 Windows 上**连提权的 Administrators 都不给按名字开**。CI 那台 runner 实测：`\.\C:\$MFT` = 5、`C:\$MFT` = 5、`\.\C:$MFT` = 2，加上 `FILE_FLAG_BACKUP_SEMANTICS`（`SeBackupPrivilege` 确实开上了）再跑一遍**还是同样的三个数**；按记录号开（`NtCreateFile` + `FILE_OPEN_BY_FILE_ID`）拿不到序列号 —— 因为 `FSCTL_ENUM_USN_DATA` **根本不返回 `$MFT` 这条元数据记录**（实测 `mftFrn=0`）。于是加了第三条路：卷句柄按簇裸读 —— 引导扇区里 `$MFT` 起始簇那格偏移各家文档不一致（读 0x30 得到"记录长度 1007616"这种鬼话），改成多候选偏移 + 只信 FILE 签名周期之后**起点找到了（簇 786432，周期验过）**、记录 1 里的非驻留 `$DATA` 也找到了，**卡在盘区段（run list）解出来的首簇对不上起点** —— 下一轮的失败原因里会带那 128 字节属性头，看一眼就能手算出正确的 run-list 偏移，不必再拿 CI 去猜。
- 判据跟着定的规矩：**环境不让读不等于产品坏了**。所以"读不到 `$MFT`"这一档改成**点名 SKIP + 每轮把整条原因印出来**，而"枚举拿得到记录"（真红过就完蛋的那条）和"回退出数 + 界面说出为什么慢"仍然是硬判据。反过来也不许它静默腐烂：SKIP 那行明确写了跳的是哪四条。
- **修好卷路径的连带后果**：`--smoke` 会重建深索引，而深索引一直是"MFT 优先、开不了卷就回退遍历" —— 那句 `\.\C:\` 修对之后**这条优先路径第一次真的跑了**。在 CI 那台 VM 上光 `FSCTL_ENUM_USN_DATA` 枚举 167 万条就要 25~45 秒（同一台机器同一份代码，三轮分别量到 4.5 / 22.7 / 25 / 44.7 秒 —— 波动极大），60 秒的步长预算当场被吃穿（run 39038920886：`--mfttest` 这次 0 红，红的变成 `--smoke` exit 124）。给它和 `--mfttest` 一样 240 秒。**对他本机没有影响**：ltool 平时不提权，开不了卷，走的还是原来那条回退遍历；只有提权跑的时候深索引才会换到 MFT 这一路。
- 盘区段那一格也量到手了：记录 1 里 0x108 那颗属性头 128 字节原样印出来，看到的是"非驻留 $DATA、NameOffset=0x40、名字长 0、值长 0、属性大小 0x1000"，后面 0x40 那格是 `ffffffff...` —— 也就是说**那颗不是 $MFT 的主 $DATA**（值长为 0，而且 0x150 还有一颗一模一样的 $DATA），记录 1 装的是 `$ATTRIBUTE_LIST` 那套扩展结构。要按簇读通 $MFT，得先认 $ATTRIBUTE_LIST、再去扩展记录里找真正带盘区段的那颗 $DATA —— 那是另一件活，不欠这一刀。

## 置顶截图：拖边缩放时图会漂（2026-10-17 深夜，他一句"缩放大小拖动的时候位置不要变"）

- 先分清是哪一层在动。合成一次"按住右下角拖 +60/+40"：**窗口左上角一字没动**（那条判据当场绿），**图区左上角从 184,0 漂到 182,0** —— 漂的是图，不是窗。
- 根因就一行：`PinWindow::imageRect()` 把图**居中**在客户区里。于是拖任意一条边，长宽各分掉一点余量，图就往反方向跳 —— 屏幕上看着就是"一缩放就跑位"。改成**钉在客户区左上**：拖哪条边都只改大小，图的左上角一动不动。
- 顺带把当年那个"半格"坑一起变简单了：居中要用整除自己算，因为 `area.center()` 会往前偏半像素，四舍五入后图的左沿可能落到历史条那一列（点在图上却换成了图）；现在左沿恒等于 `stripW()`，那个条件不可能成立。
- 代价说清楚：往期带 + 工具栏都展开、而图比窗口窄的时候，空白全堆在**右边和下边**（以前是两边各一半）。默认状态（两样都收起、窗口抱住图）一点没变 —— 那条"两样都收起：窗口正好等于图本身"照绿。
- 判据 +6 条（`--pintool` 实测 23 → **29 条 0 红**）：拖右下角之后窗口左上角不变、**图左上角不变**、图确实变大了（防"没生效所以不动"的假绿）、窗口正好 +60/+40；拖左边那条是反向验 —— 窗口左上角**该**跟着走（应有行为），但图必须仍然贴着客户区左上。
- 复测过的邻居：`--capgeo` 0 红、`--plugintest` 240/0、`--pinpick` 拿真夹具（`build/verify/picktest/text.png`）0 红 —— 中途我差点拿一张旧截图当夹具跑它，红了 6 条，那是**我喂错了图**不是回归（判据认的是那三行写在已知坐标上的字）。

## 置顶窗右键菜单白底浅灰字（2026-10-17 深夜，他截图问"为什么、怎么解决"）

- 原因：**深色弹出菜单那套样式表从来只写在两个地方** —— 设置窗那一大张 QSS 里、以及搜索框托盘菜单自己 `setStyleSheet` 的那一份。置顶窗是**独立的顶层窗口**，两处都盖不到它；而 Qt6 的 windows11 风格画弹出菜单用的是**系统**的浅色底（`QPalette::Window` 那格深色它不认），字色却仍按我们的调色板给浅灰 ⇒ 白底压浅灰字，看着像"整排菜单都灰掉了"。
- 修法：`Utils::darkMenuStyle()` —— 那份 QMenu 样式收到 core 里**只有这一份**，设置窗、搜索框、置顶窗两处菜单都从它拿（原来是三份字面量，抄来抄去就漏了置顶窗这一份）。置顶窗另外加了一颗 `PinWindow::makeStyledMenu()`，两处右键菜单都经它建。
- 为什么拆这颗函数：**真弹一下要占屏幕，自测没法验**。第一版判据在这里就错过一次 —— 探针自己 `new QMenu`，我把样式表改了它照样报"底色是深的"（离屏不挂样式表也走调色板，量出来 `#191a1e`），白绿。改成量 `makeStyledMenu()` 这个**真对象**之后，认的是只有样式表能给出来的两个数：底色 `== #232429`（CSS 里那格）和 `padding: 4px` 推出来的首项 `y=5`（不挂样式表时 y=0、高 20）。
- 反向验过：把 `m->setStyleSheet(...)` 那一行掐掉重编，红的正是这两条（`#191a1e` / `no`），第三条"字比底色亮"照绿 —— 说明它管的是"别把字和底弄成一个色"，不管样式表。恢复时踩了老坑：`mv` 回来的文件带旧 mtime，构建直接跳过，判据还停在红的那版；`touch` 之后重编才回到绿。
- 数字：`--pintool` 29 → **32 条 0 红**；`--smoke` / `--plugintest` 240 / `--capgeo` / `--cfgcheck` / `--rowmenu` / `--pinpick`（真夹具）/ `--pindel` 全 0 红。core 多一颗导出 `Utils::darkMenuStyle()`（只加不减，ABI 规矩没破）。capture 1.0.2 → **1.0.3**。

## 点「显示工具栏」右边多一块黑边（2026-10-17 深夜，他挑了「工具栏改成图标优先」）

- 根因不在我上一条改的锚定，在 `setPixmap` 里那句 `setMinimumSize(工具栏 sizeHint 宽, 140)`：**这一排原来 612px 宽**，比大多数区域截图还宽，所以一放工具栏，窗口被撑到 612，多出来的宽度只能填近黑色 `#18191d`。原来图居中，空档两边各一半不显眼；改成钉左上之后全堆右边，就扎眼了。留这个最小宽度的原始理由是"再窄右边的 📌/ 就被挤没"。
- 他挑的形状：**动作按钮收成图标**（撤销/复制/划词/历史/保存/拖动选择/📌/ 全换 30px 图标，画法照 `Anno::toolIcon` 那一套：18×18、`#e8ebf2`、1.7 圆头线）。一排 612 → **434**，于是**图宽 ≥358 就完全没有黑边**；📌 那种"两种状态"的按钮改由选中态（蓝底）+ 可访问名 + 悬停说明来说，撤销的可撤销步数从按钮文字挪进可访问名和 tooltip。
- 文字没了，**可访问名必须补齐**（读屏和 UIA 就靠它）——判据里有一条就是数这个：15 颗按钮里"既没文字又没可访问名"的必须为 0。
- 判据 +4 条（`--pintool` 32 → **36 条 0 红**）：一排 ≤460、按钮都认得出、图够宽时"窗口宽 ≤ 往期带 + 图宽"（就是黑边本身）、图够宽时"图按原尺寸摆"。
- 夹具撞过一次屏幕：第一条写成"窗口宽 == 带 + 图宽"拿 520 宽的图测，红了 —— 查出来是离屏平台报的虚拟屏只有 800×800，`setPixmap` 那条"显示尺寸不超过屏宽 60%"把图夹成 478。**那是夹具撞屏幕，不是产品**，换成 380 宽的图并把屏幕尺寸打进 INFO 才对。

## 商店：卸完插件自己跳到那张页 + 悬停说明灰字白底（2026-10-18，他一次报了三条）

- 他的复现口径是原话："先打开插件 A，返回插件页面，卸载插件 B，插件页面自动显示插件 A 内容，然后内存爆炸卡死"。
- 根因在 `rebuildGrid()` 的**摘-插顺序**：`removeWidget(第 0 格)` 之后、`insertWidget(0, 新格)` 之前，`QStackedLayout` 会把后面的格子顶成当前 —— 于是**每次重排都把上一张点开的插件页 Show 出来一次**。量法：给那张页挂 event filter 数 `QEvent::Show`。旧顺序 **1 次**，新顺序 **0 次**（同一份判据、同一台机器、只换那三行顺序）。
- 卸载那一下界面不说真话：`applyUninstall` 收到的 `state` 是 `nullptr`，答复写在一张**当场就被重排删掉**的标签上；而 dll 被进程映射着删不掉 → 只是排进待删表，文件还在盘上，所以重排之后那张卡又回来说"已装 (v1.0.1)"。他 11:17:24 / 11:17:35 连点两次「卸载」就是这么来的（日志两条都是"还加载着，删不掉"）。
- 修法改数据层不改文案：`PluginStore::Row::queuedRemove` 一格，`shopRows()` 从配置里那张待删表补上，卡片的状态词整条换成「已安排卸载 · 这个进程还加载着它 · 下次启动真删掉」。这样它活得过任何一次重排。
- 出口那颗：`‹ 返回商店` 原来是 `autoRaise` 的淡箭头（他的原话是"找不到退出按钮"），现在和「刷新清单」同款实心；另加一条 **Esc 在那张页上 = 回卡片墙** —— 必须把这个事件吃掉，不然它冒到 `QDialog` 会把整个设置窗关掉。三处出口（按钮 / Esc / 重排之后）共用 `backToGrid()` 一颗。
- `showTool()` 加了一条日志：`store: 展开 <id>（stack 第 N 格 / 共 M 格，之前在第 K 格）`。三条路（点卡片正文 / 搜索框直达 / 设置窗 `openAt`）原来在日志里长得一模一样，他这次报的"自己跳过去"根本没法定位是谁跳的。
- 悬停说明和 2026-10-17 那个右键菜单**同一个病**：Qt6 的 windows11 风格画 tooltip 用的是系统的浅色底，`applyDarkPalette` 里设的 `ToolTipBase/ToolTipText` 它根本不读 ⇒ 灰字压白底。`Utils::darkTipStyle()` 挂在 **qApp** 上（tooltip 是独立顶层窗口，挂在某个窗口的样式表管不到它）。反向验过：把 `app.setStyleSheet(...)` 那一行撤掉，两条判据立刻红（`qApp 样式表长度=0`、`底=#000000 字=#000000`）。
- 离屏平台的一个硬限制，记下来别再上当：**`grab()` 出来的中文全是豆腐块 `□`**（qoffscreen 那套字体没有 CJK 字形）。所以"字看不看得见"这类判据在离屏只能量**样式表里写死的两个数**，不能拿像素亮度当证据 —— 拿像素测会假绿（豆腐块也是亮像素）。
- 没复现的那一半要说清楚：内存爆炸这条离屏量到 **58→63 MB**（他那一轮真连点了 15 下装/卸，日志 11:18:07~20 连着 14 条"装好"）。Show 闪一次解释得了"自动显示"，解释不了"卡死"；这一轮的日志里也没有任何一条 `--mfttest` 那种超时形状。所以判据先钉在原地，日志那颗留给下一次。
- 数字：新增 `--storeexit` **14 条 0 红**（离屏、不联网、一颗 dll 都不真删 —— B 那颗先点开一次，让它走"排进待删表"那条）；`--cfgcheck` 11 → **13 条 0 红**。邻居复测：`--plugintest` 240、`--storetest` 73、`--username` 36、`--about` 27、`--pinyin` 52、`--seamtest` 38、`--gridtest` 64、`--cliptest` 57、`--linuxdotest` 44、`--rowmenu` 9、`--appdirs` 12、`--diskbranch` 4、`--mfttest` 11、`--smoke` 全 0 红。CI 的判据循环加了 `--storeexit`。
- 版本：**不升号**。工作树是 1.1.0 且从没打过 tag（最新 tag 还是 `v1.0.3`），这一轮修的就是同一批未发布产物；改的全在 core（`src/ToolsPage.*`、`src/PluginStore.h`、`src/Utils.*`、`src/main.cpp`），十九颗插件一行没动，所以 `plugin.json` 一颗都不必递增。

## F1 报「是不是缺 ltool-capture.dll」，可 dll 就在目录里（2026-10-19 凌晨，他装机后第一次按 F1）

- 现象：托盘气泡「F1 截图 / 没装接 capture 这个动作的插件（ltool-plugins 里是不是缺 ltool-capture.dll）」，F2 同形状。
- 实情：那颗 dll **在**，`LoadLibrary` 起不来 —— `Cannot load library …: 找不到指定的程序`（缺导入函数 `Utils::revealInExplorer` / `Utils::launch`，跨平台那十刀把它们挪进 `Platform::` 了）。这就是挂着的 #91：商店发的那批产物不是按这版 core 的导出面编的。
- 现场量的：拿**装好的那份**跑 `--plugintest`（安装包 Excludes 掉了 qoffscreen，得借 `QT_QPA_PLATFORM_PLUGIN_PATH=build\Release\platforms` 才起得来）→ 19 颗元数据全过，**6 颗装载失败**：capture / record / registry / repair / startup / language，判据红 19 条。同样的报错在他日志里最早是 **10-16 20:27**，不是这次装机带来的；触发点是他 11:17~11:18 在商店里连点「更新」，把本来能跑的本地构建换成了仓库那份。
- 先救功能再改文案：只把那 6 颗换成同树构建的本机 dll（旧的那份备份在 `%TEMP%\store-dll-backup\`），**没杀他实例** —— 那 6 颗本来就没映射进进程，拷得动；他的实例下次按 F1 会重新 `LoadLibrary`。复跑装好的那份：`--plugintest` **19/19 通过检查、0 红**。
- 文案这条是真 bug，改在数据层：`PluginHost::loadErrorOf(id)`（每次装载失败把原因按 id 记下来，成功就销掉）+ `whyNoOwner(action)` 三分支 —— 没挂牌 = 「是不是缺这个文件」；挂了牌但起不来 = 「它就在目录里，可它起不来：<原因>」；起来了但不认这个动作 = 「元数据和实例对不上」。两处调用点（F1/F2 气泡、右键那一路）都改成拿这一颗。
- 判据 +2 条（`--plugintest` 240 → **242**）：装了的不许甩锅成「是不是缺文件」；真没人接的必须点名缺哪个文件。离线合计 694 → **696 条 0 红**。
- 版本：core **不升号**（1.1.0 从未打 tag，这全是同一批未发布产物里的修文案 + 修谎报）；插件一行没动，闸绿。
- 还没治的那半：#91 的根 —— 要么把 `ltool_plugin` 仓库那批用这版 core 重编重发（动公开仓库，等他拍），要么让商店在装之前先比一次"这颗在这台机器上起不起得来"（起不来就别换掉能用的那份）。

## 电源直达：输 sleep/休眠 就休眠，输 shutdown/关机 就关机（2026-10-19 他点名要的功能）

- 形状：搜索框里**整条相等**才算（`休眠`/`sleep`/`hibernate`、`关机`/`shutdown`）→ 第一节「系统动作」那一行排第一，回车立刻动手、不问第二遍（他原话"直接回车关机"）。松散命中一律不算 —— 判据里有一条反向的：搜「关机后怎么恢复文件」不许把关机那条排第一。
- 不清场：这条走的是 2026-10-16 立下的规矩（直达不许铲平别的节），应用/文件/网站那几节照常在下面列出来。回车取的是 `m_list->setCurrentRow(firstActionable)` 那一行，节标题不会被当成当前行。
- 动手走 `shutdown.exe`（`/h` 休眠、`/s /t 0` 关机）而不是 `SetSuspendState`/`ExitWindowsEx`：那条要新链 powrprof 还要自己拼特权，而 shutdown.exe 的报错是给人看的；`QProcess` + `Platform::runWithoutConsole` = 不闪黑窗，拿得到退出码和 stderr，**失败就把原因写在启动器下面那行、窗口不收**（成功才 `hide()`）。
- 休眠开没开：`CallNtPowerInformation(SystemPowerCapabilities)` 问两格 —— **`SystemS4` 只是"固件支持"，`HiberFilePresent` 才是"休眠当前启用"**。第一版只查了 `SystemS4`，量出来"能休眠"，可 `powercfg /a` 当场说「休眠：尚未启用休眠」—— 复测抓出来的，改成两格都看之后两边对上了（这台机器现在报"没开"，那一行直接写 `powercfg /h on`）。
- 判据 +6 条（`--pinyin` 52 → **58**）：四个词各自的路由、反向那条"整条相等才算"、以及"没开休眠的那台必须点名 powercfg / 开了的那台不许出现这句"（按机器实际状态分支，两种机器上都成立）。**判据一次都不真按回车** —— 只读 `resultAt(i).target`，跑判据的机器不能睡过去。
- 可移植那份没破：`src/SysTools.cpp` 在 `LTOOL_CORE_PORTABLE_SRC` 名单里，Win32 那截用 `#ifdef _WIN32` 圈住了；WSL 里 `g++ -std=c++20 -fsyntax-only` 复跑过 = `POSIX_SYNTAX_OK`。core 多链一颗 `powrprof`（只在 `if(WIN32)` 那一串里）。
- 数字：离线合计 696 → **702 条 0 红**。版本：可感知新功能本该 minor，但工作树 **1.1.0 从没打过 tag**（最新 tag 还是 v1.0.3），按"未发布的号吸收同批功能"并进 1.1.0，不另起号；插件一行没动，闸绿。
- 他装完立刻报"搜 shutdown 什么都没有搜出来" —— 根因不在匹配规则，在**回车和 120ms 去抖抢拍**：`textChanged → m_debounce(120ms) → runSearch`，回车走的是 `activateCurrent()`，读的是**上一个查询那一版**列表（少一个字母那一版里没有电源那条，只剩"用搜索引擎搜索"）。修法：`SearchWindow::flushSearch()`（去抖还压着就立刻补一拍），`activateCurrent()` 第一件事就是它。这条不只关电源 —— 任何"打字快 + 立刻回车"都在抢这一拍。
- 判据踩到一次测量陷阱：第一次写这条判据红了，因为"没补那一拍"时读到的列表是**上一个查询留下的**（里面正好有 `power:shutdown`）。改成先 `setText("zzz-什么都搜不到-qqq") + settle()` 把列表清干净，再 `setText("shutdown")` **不 settle** —— 读到的才是"少一拍"那一版（= 0 条），`flushSearch()` 之后 = 1 条。`--pinyin` 58 → **59 条 0 红**，离线合计 **703 条 0 红**。
- 他接着问"为什么搜索框没有 shutdown 的下拉选项" —— 原来那条规则是**整条相等**才算，`shutdown.exe`、`shutdown /s` 这类都落不进来。改成：**中文词仍只认整条**（"关机后怎么恢复文件"是一句查询，前缀匹配会误关机），**英文词按"命令行形状"认**：尾巴为空、`.exe`、或"空格 + `/`/`-` 开关"才算，`shutdown 怎么用` 不算。判据 +4 条（`--pinyin` 59 → **63**，离线 **707 条 0 红**）：`shutdown.exe` 有、`shutdown /s` 有、`shutdown 怎么用` 没有、中文那条没放宽。
- 他补的第四句：`shut` 也要出关机。没另起一条规则 —— 把 `shut` 当英文词喂给同一个 `powerWord()`，尾巴那三道（空 / `.exe` / `空格 + /`或`-`）照旧生效，所以 `shut`、`shut.exe`、`shut /f` 算，`shutter`、`shut down 怎么用` 落不进来（尾巴是普通词 = 在查资料）。判据 +3 条（`--pinyin` 63 → **66 条 0 红**）：`shut` 有、`shut /f` 有、`shutter` 与 `shut down 怎么用` 没有。反向那条特意一次钉两个词，只放宽前缀不钉尾巴的话，`shutter` 会被顺手关机器。
- 版本：`v1.1.0` 已经打了 tag 并在 CI 发版流水线上跑（指向 `9fd13fa`，不含这一句），所以这刀升 **1.1.1**（patch：同一档功能加一个关键词，不动数据格式、不动插件 ABI）。改的全在 core（`src/SearchWindow.cpp`、`src/main.cpp`），十九颗插件一行没动。

## 下载全走本地代理：八处新建管理器，漏的那处是 OCR 引擎包（2026-10-19 深夜，他一句"如果有代理的情况下使用本地代理下载"）
- 先量这台机器的真值：注册表 `ProxyEnable=1`、`ProxyServer=127.0.0.1:9980`，`%APPDATA%\ltool\config.ini` 里 `net/proxy` 留空 ⇒ `--seamtest` 打的是 `INFO 生效代理=[http://127.0.0.1:9980]`（留空 = 跟系统代理这条规矩本身是好的）。
- 逐处数管理器（新闸 `tools/proxy_gate.sh`：每处新建 `QNetworkAccessManager` 之后 15 行内必须有 `applyProxy(`/`setProxy(`，结论行按纯 ASCII 打 `PROXY_GATE_SITES=` / `PROXY_GATE_BAD=`）：**8 处，漏 1 处** —— 漏的是 `plugins/capture/OcrBundle.cpp` 那个**栈上的** nam。其余七处（DownloadManager 每次取用都重读、PluginStore 镜像→代理→直连那条链、AboutPage、LinuxdoWatch、SoftwarePage 的 favicon、两处判据）本来就走代理。
- 这条漏要命的地方在于它是**境外源**：OCR 引擎包 27.9MB 从 `files.pythonhosted.org` 下（两个 PyPI wheel + 钉死 SHA256），这台机器直连那域名是不通的 ⇒ 表现就是"提取文字"第一次开下载永远转圈，而失败那句话里连"走的哪条路"都没有。
- 修法两步：`DevEnv::applyProxy(&nam)`（用现成那颗，不另起一套），失败那句话点名路线 —— 有代理时 `这趟走的是本地代理 http://…，想换一个就在配置页「下载代理」那格填新的地址`，没检测到代理时 `这趟是直连`。
- 端到端实测（沙箱 APPDATA 里塞一条 `net/proxy=127.0.0.1:1` 这个必然被拒的地址，跑 `--ocrget`，一行真网络都没上）：`err=下载失败：Proxy connection refused（这趟走的是本地代理 http://127.0.0.1:1…）` —— "连代理都被拒"就是"这条请求确实经过代理"的正面证据；改之前同一条探针是直连 pythonhosted，报的是连接超时那一族。
- 已知没解决、也没偷偷发明的第三态：配置页那格**留空只能退回系统代理**，没有"这台机器有系统代理，但我这次要强制直连"那一档。要加就是 `net/proxy` 认一个 `direct` 关键字，等他拍。
- 闸接进 CI：`.github/workflows/build.yml` 里排在插件版本闸后面（同一步形状，`shell: bash`）。反向验过：改之前跑同一道闸，红的正是 `plugins/capture/OcrBundle.cpp:70` 那一行。
- 数字：`--seamtest` 38、`--cfgcheck` 13、`--plugintest` **242**、`--storenet` **17/0**（这次实测走的是 `镜像 gh-proxy.com`，1 次成功）、`--pinyin` 66 全 0 红。README 那张表里 `--plugintest` 还写着 221，按实测改成 242。
- 交付分层要说清：改的是**源码 + 本机构建那份 `ltool-capture.dll`**（它自己版本号 1.0.4 → **1.0.5**，过版本闸）。安装包是「开箱零插件」，所以这一条**不随主程序安装包出去**；商店里那颗（`ltool_plugin` 仓库 dist 那批）还带着 #91 那三个符号的坑，等重发。

## 剪贴板那张页打开卡：每一行的图标都在整图解码（2026-10-19 深夜，他一句「打开剪贴板很卡」）
- 先量他那份真数据：`%APPDATA%\ltool\clipboard.json` 是 **200 条**，其中 **51 条是图片**，`clipimg` 里 51 张 PNG 合计 **11.72 MB / 51.6 百万像素**（最大一张 2.6 MB，常见是 1264×871 和 1024×680）。
- 卡在哪：`ClipboardStore::iconFor()` 每一行都走 `QPixmap(整图)` + `SmoothTransformation` 缩到 40px，而那张页**每次被看见、每敲一个过滤字**都要 `m_list->clear()` 重铺一遍列表（`ClipboardPage::buildRows`）⇒ 几百毫秒挂在 GUI 线程上。
- 一趟的代价（拿同一批 51 张真图测的两条趟，PowerShell System.Drawing 作代理，Qt 那侧另有实测见下）：**整图解码+缩图 632ms**，**只读 40px 缩略图 10ms**，差 **63×**。
- 改法就一条：缩略图落盘 + 内存缓存。`clipimg/<sha1>.png` 旁边多一颗 `clipimg/<sha1>.t40.png`；`iconFor` 先读小的，没有才解整图并补一张；图片**被收进来的那一刻**就顺手落小的（`capture()` 里），所以正常用下去那张页永远碰不到整图。
- 为什么不另开一棵目录：`prune()` 的孤儿清理只扫 `clipimg` 那一层，开第二棵就得写第二遍"孤儿怎么撤"，两处迟早对不上。于是 keep 表里现在**原图和它的缩略图都记一颗**（`src/ClipboardStore.cpp` 那三行）—— 原图还在用，小的不算孤儿；原图被撤了，小的跟着走。
- 判据 +9 条（`--cliptest` 离屏 56 → **65**，`QT_QPA_PLATFORM=windows` 真剪贴板那一趟 **70 条 0 红**）：
  - 「第一趟会落一张小的」「落盘那张真的是 40px 以内」（实测 40x40）「thumbPath 只换后缀、和原图同目录」；
  - 「落盘后再重铺 40 行不再整图解 40 遍（<120ms）」—— Qt 侧实测：**第一趟 15ms（1600×1600 解码+缩放+存盘），之后 40 次合计 0ms**；
  - 「原图坏了、缩略图在：还给得出图标」—— 这条是"读的是小的那张"的**正面证据**（比量时间硬：把原图写成一份坏 PNG，整图那条路必然给空图标）；
  - 「原图坏、缩略图也没了：给空图标就行（不许崩）」；
  - 清理那侧两条：「还在用的那张，它的缩略图没被当孤儿删掉」「没人要的那张缩略图照样算孤儿」。
- 踩到一次测量自造的红：那条"活着的缩略图不许被删"第一版写成"删掉小的 → 调 `iconFor` 补回来" —— 同进程里**内存缓存早就有这颗**，一命中就再也不落盘，于是量到的是缓存而不是清理。改成判据**自己写**那颗小的，再走一次 prune。
- 他那份的推算成本（说清是推算）：按 Qt 实测 2.56 百万像素一趟 15ms 折算，他这 51.6 百万像素是**升级后第一次打开那张页补一次约 0.3~0.6 秒**，之后就常年 10ms 那一档；新收进来的图当场就落小的，不会再有这一次。
- 版本：**1.1.1 → 1.1.2**（patch：修的是"打开卡"这个缺陷，不是新页面/新插件；`v1.1.1` 已经发出去了，同一颗号不能被后续产物复用）。这颗号改的是 **core**（`ClipboardStore.{h,cpp}`）+ clipboard 插件里那份判据 TU，所以 `clipboard` 自己 1.0.0 → **1.0.1**（版本闸绿：19 颗、NEEDS_BUMP=0）。
- ABI：`thumbPath` 是**新增**的导出名（名字可以加不可以缩那条规矩），`ClipboardStore` 的成员一个都没挪（缓存做成了函数内的 static，没往类里加字段）—— 已发布那批插件 dll 的导入表和偏移都不受影响。

## 剪贴板那张列表改成卡片：时间 + 文案 + 一块缩略图（2026-10-19 深夜，他连发两句「剪贴板列表做成卡片」）
- 拍定的形状（他连发同一句、跳过我摆的对照页 = 按默认做完再点名残留）：一张卡 **208×176**，里面三块 —— 顶部一行时间（8.5pt `#8b93a7`，画不下就省略号）、中间一块 **190×84** 的缩略图（只有图片那条有）、下面文案（9.5pt `#e8ebf2`，按词折行、超出裁掉）。底色 `rgba(255,255,255,0.08)`、描边 `rgba(255,255,255,0.12)`、选中描边 `#3f6ae0` —— 全是「插件商店」那张卡片墙在用的同一套写法，没在这页发明第四种深色。
- **仍然是一个 QListWidget + 一颗自绘 delegate**（`IconMode` + `QStyledItemDelegate`），不是每行 `setItemWidget`：那个一行 3-4ms，200 条就是 0.6 秒 —— 正是上一轮"打开卡"要消掉的东西，别刚修完又装回去。
- 缩略图从 40px 涨到 **120×84**（`clipimg/<sha1>.t120.png`）：卡片上 40px 太小。旧的 `.t40.` 不用管 —— `prune()` 只认 keep 表里那两颗路径（原图 + `.t120.`），老的自动当孤儿收掉，不留两套缓存。
- 卡片上的三块矩形从 `cardSize()` 推出来，抽成 `timeRect/thumbRect/bodyRect` 三颗静态函数，判据直接问几何（自绘最容易字压字，量不到就只能靠肉眼）。
- 判据 73 条 0 红（`--cliptest` 离屏 65 → **73**）：卡片墙成立（`IconMode` + delegate 两个条件都要有）、每格尺寸 = `cardSize()`、三块都在卡内、时间不许压图也不许压字、图和文案不许互压、有图的卡文案确实被压矮（119 vs 27）、缩略图块 ≥120×84。
  - **反向验过**：把 `IconMode` 换回 `ListMode` → "现在是卡片墙"红成 `got=[行列表]`；把时间块往下挪 20px → "不许压到缩略图"红。两条都是活线，不是装饰性的绿。
- 拿离屏渲的 PNG 复核抓到一条**判据量不到的真错**：文本卡也空着上半截图位。根因是"这张卡有没有图"我原来问的是 `!index.data(Qt::DecorationRole).isNull()` —— 文本条的图标是**空 QIcon**，而 `QVariant(QIcon())` **不是 null**，于是每张文本卡都以为自己有图。改成显式的 `ImageRole`（bool），并补一条 `bodyTopAt(row)` 判据把这类错钉住（实测 `[119,27,27,27]`）。这条是那张 PNG 看出来的，不是断言抓出来的 —— 记下来：自绘版面渲一张出来看，值回它的时间。
- 双击放回 / Delete 删这条 / 关键字过滤 / 状态行 / 清空 五个动作的语义一个都没动（判据读的 `rowCount()/rowTextAt()/storeIndexAt()` 保持原样，那 56 条老判据一条没改就继续跑）。
- 版本：**1.1.2 → 1.2.0**（可感知的界面改版 = minor；1.1.2 从没打 tag、没发过 Release，按"未发布的号吸收同批功能"并进 1.2.0 —— 里面装着上一轮的缩略图缓存 + 这一轮的卡片）。clipboard 自己 1.0.1 → **1.1.0**（版本闸绿：19 颗、`NEEDS_BUMP=0`、基线已退到 `v1.1.1`）。

## 搜索框搜不到 ChatGPT：开始菜单里那 43 颗打包应用，索引三个来源一个都不覆盖（2026-10-20 凌晨）
- 先分两层取证：`apps.json` 267 条里含 chat/gpt 的 **0 条**；注册表三处 `Uninstall` 键全扫也**零命中**；`winget list` 报的是 `MSIX\OpenAI.Codex_...`；`Get-StartApps` 里它是 `ChatGPT :: OpenAI.Codex_2p2nqsd0c76g0!App`。这台开始菜单 312 条里有 **43 条是这种打包应用**（还有 Claude / Dev Home / 设置 / Teams / Kazumi…），磁盘上没有 `.lnk`，`C:\Program Files\WindowsApps` 的 ACL 连提权的 Administrators 都不给列（glob 它静默返回空）。而 `AppScanner::scan()` 只有三个来源：用户/公共桌面 + 开始菜单 `.lnk` + 配置页应用目录 ⇒ 一颗都覆盖不到。
- 接法：新道缝 `src/platform/AppsFolder.{h,_win.cpp,_posix.cpp}` —— `SHGetKnownFolderItem(FOLDERID_AppsFolder)` + `BindToHandler(BHID_EnumItems)` 枚举，`SIGDN_NORMALDISPLAY` 当名字、`SIGDN_DESKTOPABSOLUTEPARSING` 当身份，拼成 `shell:AppsFolder\<AUMID>` 作启动入口（`Platform::launch` 就是 `ShellExecute("open")`，这个写法开始菜单自己也在用）。POSIX 那一份返回空 = 这个平台没有这一类，不是没做完。
- 三处实测踩到的形状，都写进注释了：
  - `SIGDN_DESKTOPABSOLUTEPARSING` 回来的是**裸 AUMID**，不带 `shell:AppsFolder\` 前缀 —— 前缀要自己拼。
  - 别拿"有没有 `!`"当判据：`Anysphere.Cursor` / `ai.opencode.desktop` / `ai.verdent.deck` 的 AppID 就不带（`Get-StartApps` 报的一模一样），要求带 `!` 会挡掉三颗能启动的。
  - 但 `Microsoft.AutoGenerated.{GUID}` 那一族**必须拒**：那是普通 `.lnk` 在 AppsFolder 里的镜像，这台混进来的正是"Uninstall CMake""任务管理器""Hyper-V 管理器"。加上"名字含 uninstall/卸载 的一律不收"（和 `.lnk` 那条规矩一致），这台这道缝列出 **94 条**、进索引 **48 颗**。
- 图标：`IExtractIconW` 这条路 `hwndOwner` 给 `nullptr` 时一颗都拿不到，换成 `GetDesktopWindow()` 后 48 颗里 **5 颗**拿到；剩下的走 `Utils::letterIcon` 兜底（`SearchWindow` 那两处 `category=="system"` 的判断加上了 `appsfolder`），不留空白行。
- 撞名：AppsFolder 是"整个开始菜单"的合并视图（这台列出 312 条），所以和 `.lnk` 必然撞 —— 撞了让位给快捷方式（这台 94 条里 46 条同名让位，最后进索引 48 颗），索引里同一名字不许有两条来自这一族。
- 判据 +4 条（`--pinyin` 66 → **70 条 0 红**）：缝列出东西时索引必须收到这一族 / 入口写法全是 `shell:AppsFolder\` / 零垃圾（AutoGenerated 与卸载器）/ 这一族不和别的来源同名。**量的是现扫的 `scan()` 和那道缝本身** —— 第一版我拿 `loadIndex()` 量，反向把拒收规则撤掉判据照样绿，量的其实是上次 `--smoke` 写好的那份文件；改成现扫之后同一撤法红的就是那条（实测 `垃圾 24 颗` → FAIL）。
- 端到端那条（搜 `chatgpt` 出应用行）在**盘上索引还是旧 core 写的那份**时会点名跳过 —— 他机器上常驻的 1.1.2 实例一重扫就把 `apps.json` 写回 267 条，这是环境不是代码，装上 1.3.0 之后它自己会跑起来。
- 版本：**1.2.0 → 1.3.0**（新能力：搜索框多覆盖一类应用 = minor）。改的全在 core（`AppScanner.cpp`、`SearchWindow.cpp`、新道缝、`main.cpp` 判据），十九颗插件一行没动，`plugin_version_gate` 绿。

## 配置页加「夜间 / 日间」：颜色从 380 处字面量搬进一张角色表（2026-10-20 他点名要这颗切换）
- 先量的规模（改之前那一版上 `git grep -oE '#[0-9a-fA-F]{6}|rgba\('` 数的）：全仓 **271 处、散在 28 个文件**，其中 `src/` **135 处 / 9 个文件**、`plugins/` **136 处**。改完 `src/` 只剩 Theme.cpp 那张表本体（表外 0 处），`plugins/` 一处没动。所以这一步不是"加一个开关"，是把颜色的**出处**换掉 —— 新文件 `src/Theme.{h,cpp}` 一张 `角色 → {夜间值, 日间值}` 表，别处只写名字。
- 日间那一份是**算出来的不是挑出来的**（现算 WCAG 相对亮度）：正文 `#1b1c20` 压 `#f2f3f6` = **15.34:1**、灰字 `#5b616e` = **5.60:1**、输入框底上正文 = **17.02:1**、白字压主色 `#2f56c9` = **6.37:1**，四条全过 AA（4.5:1）。主色两个模式下不同值（夜 `#3f6ae0` / 日 `#2f56c9`）就是为了白字压上去两头都够 —— 夜间那颗只有 4.83:1，本来就在及格线上。
- 「当场生效」要两件事同时成立，缺一半就是"别处白了、这块还黑"：① 重刷 qApp 调色板 + qApp 那份悬停说明样式表；② 每个控件自己那份 QSS 重算。②是**几十个调用点**，所以新加 `Theme::bind(w, qssWithTokens)` —— 挂上去的同时装一颗 parented 事件过滤器接 `QEvent::ThemeChange`，切换时原样重算。`setMode()` 给顶层窗口**和它们的子控件**各发一次事件（只发顶层的话，配置页里的控件一个都不动 —— 这条反向验过：撤掉子控件那一圈，判据当场红成"日间=夜里的色号"）。
- 样式表挂在**子控件**上而不是窗口上，这一条是这次最容易想错的地方：`setStyleSheet` 改 qApp 传导不下去，而 tooltip 反过来是**独立顶层窗口**、挂在某个窗口上管不到它 —— 两头各治一处，不能合并成一句。
- `expand()` 里埋着一颗静默错：表里有 5 对"一个是另一个的前缀"（`card`/`cardLine`、`accent`/`accentUp`/`accentDown`、`group`/`groupText`、`disabled`/`disabledLine`、`border`/`borderHi`/`borderHiUp`）。按表顺序挨个 `replace` 会把 `@cardLine` 变成 `<card 的值>Line` —— 界面上是"边框色不对"，日志一个字不印。改成正则整段吃名字再查表；判据拿这 8 个名字拼一条串钉住（同一撤法实测红：`rgba(255,255,255,0.08)|rgba(255,255,255,0.08)Line|...`）。
- 自绘的那几处不走样式表（搜索框整扇窗的底、结果行的选中块、行标题/副标题字色、图标兜底圆），改读 `Theme::color(角色[, alpha])`。为此把 `panel`/`hairline` 存成 `#rrggbb` 而不是 rgba —— QColor 解析不了 CSS 的 `rgba()`，`color()` 会静默给一个无效色；rgba 那几颗（`card`/`cardLine`/`scrollHandle`/`tintCyan`）只准在 QSS 里用，这条写在表的注释里。
- 一道新闸 `tools/theme_gate.sh`（和代理那道同一形状）：数 `src/` 里每一处 `setStyleSheet(` / `Theme::bind`，往后 12 行内出现 `#rrggbb` 或 `rgba(` 就算漏。当前 **33 处、漏 0**。它抓出来的头两颗不是我改的那批，是两扇日志窗（`SilentInstallLog`、`InstallLogDialog`）—— 启动时建一次、之后反复 show/hide 复用的窗，正是"改了字符串但没人重挂"的那一类。已进 CI，排在代理闸后面。
- 判据 `--cfgcheck` **13 → 24 条 0 红**（+11）。三条值得单说：
  - 「拨回夜间一字不差回到原来那一份」—— 单向棘轮（切得过去切不回来）只有这一条抓得到。
  - 「挂在子控件上的那份样式表跟着切换重挂」—— 探针 new 一颗**装在窗口里的** QLabel 走 `Theme::bind`，量的是切换前后那串样式表文本，不是控件在不在。
  - **最后一条量像素**：两种模式各 `grab()` 一张配置页存成 PNG，再取整张平均灰度 —— 日间 **234** vs 夜间 **35**，差 **199**。前面那些判据量的是"字符串里写的哪个色号"，样式表写得对但画出来还是黑的完全可能（那份 QSS 没挂到这个控件上、或这个部件走调色板）。两张 PNG 落在 `%TEMP%\ltool-cfgshots\theme-{night,day}.png`，肉眼复核用的就是它们。
- 残留（界面上没装任何东西，配置页那行的悬停说明里写明了）：**19 颗插件 dll 自己那批颜色没动** —— 录屏、置顶窗、宫图、剪贴板…那 136 处写死深色。改它们 = 重发全部产物，而"商店里那份 dll 和这版 core 的导出面对不上"本来就是另一件待拍的事（#91），不该为了配色去加重它。所以这一轮切到日间时，插件自己的页仍是深色；core 这边（搜索框、菜单墙、配置、关于、网站、托盘菜单、两扇日志窗）全跟着换。
- ABI：`Utils::darkMenuStyle()` / `darkTipStyle()` **符号名一个没动**（已发布插件的导入表写死了它们），里面转调 `Theme::menuStyle()` / `tipStyle()` —— "样式表只有一份"这个事实不能因为改名裂成两份。`Theme::*` 是 core 新增导出，只加不减。
- 版本：**1.3.0 不升**（`v1.1.1` 是最新已发布号，1.2.0 / 1.3.0 从没打 tag、没有任何产物以这两个号存在过 ⇒ 按"未发布的号吸收同批功能"并进来）。十九颗插件一行没动，`plugin_version_gate` 绿。

## 日间那份白看不清搜索框的边框：无边框窗只靠一圈描边定位（2026-10-20 他第一句反馈）
- 他的原话：「这个白色不好，完全看不清搜索框的边框」。根因不是"白得太亮"，是**启动器那扇是 `WA_TranslucentBackground` + `FramelessWindowHint`** —— 没有系统标题栏、没有阴影框，"搜索框在哪儿"这件事**只由那一圈 1px 描边**说给他看。而日间那份底是**纯白 `#ffffff`**、描边是黑压 26/255，两个模式共用同一颗 `hairline`：夜里白压深色底看得见，日间黑压白底就没了。
- 先把它变成能数的东西再动手。新加一条**量像素**的判据（`--cfgcheck` 24 → **25 条**）：离屏 `grab()` 那扇启动器，沿**左边缘第 0 列、竖着中间那一行**取描边的颜色、往里第 8 列取窗口底的颜色，按 WCAG 相对亮度算对比度。改之前实测 **夜间 1.86:1 / 日间 1.51:1** —— 他那句"完全看不清"是 1.51。
  - 取点位置踩了两次：① 先从顶行往里扫"第一条不透明的像素"，结果扫过了整扇窗（`panel` 自己带 247 的半透明，`>=250` 不满足），量到的是输入框的占位字（读出来 `#78808c` 才发现）；② 改成按 alpha 分档之后，日间底一变成不透明就"描边和底同一行"了。**最后走左边缘那一列**：路径是 `QRectF(0.5, 0.5, …)` 配 1px 画笔 → 描边正好盖满第 0 列，而顶行有 13px 圆角、不能用。
- 修法：`panel` 日间从纯白挪到 `#f7f8fa`（白桌面/白文档上还分得出一块浮起来的东西），描边单独立一颗 `panelEdge`（夜 `#1affffff` = 原来那颗白压 26/255，**值原样搬进来所以夜里一个字没变**；日 `#848c9a`），选中行的底同理立一颗 `rowSel`。`#AARRGGBB` 是 QColor 和 QSS 都认的写法，半透明因此能存进表里，不用在调用点补 alpha —— 这也就是 `color(角色, alpha)` 那颗重载存在的原因。
- 判据两个模式各判一次，但**门槛不一样**：日间按 1.4.11 要 ≥3:1（现在 **3.19:1**），夜间那颗 1.86:1 只钉"别再变糊"（`>= 1.8`）。理由：他点的是"这个白色不好"，没说要改他用了很久的默认外观 —— 拿 3:1 去量夜里，会顺手改掉他没投诉过的东西。
- 两张 PNG 落在 `%TEMP%\ltool-cfgshots\launcher-{night,day}.png`，我 Read 过才报绿（数字对上不等于看着对：`rowSel` 日间那颗就是靠眼睛挑的，选中行没有结果可量，这条如实说是**没量过的**）。
- 版本：**1.3.0 不升**（1.3.0 从没打 tag、没有产物以这个号存在过，按"未发布的号吸收同批功能"并进来）。

## 日间换成「A 冷灰纸 + 青碧」：病因不是白得刺眼，是三层同一个白（2026-10-20 他回「太白了换一个白色，给几个方案」）
- 先出对照页（现状 1:1 复刻 + A/B/C/D 四个方向，数字在浏览器里现算、强调色可点着换），没有让他对着 hex 猜。他回「开dev A 冷灰纸 青碧」= 按 A 那套 + `#0a6c86` 落地。
- **诊断改变了修法**：原来那份日间的窗口 `#f2f3f6`、面板 `#f7f8fa`、输入框 `#ffffff` —— 面板压窗口只有 **1.00:1**、输入框压面板 **1.06:1**，也就是"三层几乎是同一个白"。所以"太白了"不是亮度问题（正文对比度 15.34:1 好得很），是**没有层次**。A 做的事就是把最外那层压到 `#e9edf3`、中间那层留 `#f4f6fa`、输入框留纯白，让卡片和输入框浮起来。
- 落地后的四位数（现算，写进配置页那行的悬停说明，别只留在 notes）：正文 `#191a1e` 压窗口 `#e9edf3` = **14.76:1**、灰字 `#525863` = **6.07:1**、白字压主色 `#0a6c86` = **6.00:1**、启动器那圈描边 `#6f7783` 压窗口 = **3.85:1**（判据实测读数就是 3.85，和对照页算的 3.84 对得上）。
- 跟着主色一起换的还有 `infoText` / `neutralBadge`（原来都写死 `#1d5fbf`/`#2f56c9` 那族蓝）和 `selBg`/`rowSel`（选中那档从蓝灰 `#4d7f96c8` 换成青碧 `#330a6c86`）—— 漏一颗的表现就是"主色青了、那颗徽章还是蓝的"。夜里那一列一个字符没动。
- **踩到一次静默重复**：搬这张表时把 `good/warn/bad/mutedBadge/neutralBadge/cyan` 那六颗写进了两遍。`findRole()` 取第一条命中，所以第二遍是死值、界面上什么都看不出来，编译器也不报 —— 修完之后把「角色名不许重复」加进 `tools/theme_gate.sh` 当第二件事（现在 `THEME_GATE_ROLES=48（无重复）`）。
- 判据跟着动的地方：那条**量像素**的描边判据门槛不变（日间 ≥3:1、夜间只钉"别再比 1.86:1 更糊"），换色之后实测 3.85:1 过；配置页那张渲染图整张平均灰度从 234 落到 **228**（判据口径是"日间比夜间亮 >60"，193 照过）。`--cfgcheck` 25 条 0 红、离线合计 689 条 0 红。
- 没量过的那一颗还是没量过：`rowSel` 在离屏里没有结果行可测，这次只是跟着主色重算了一档透明度，**眼睛挑的**。
- 版本：**1.3.0 不升**（同上一刀的理由：从没打 tag、没有产物以这个号存在过）。

## 搜索框来点透明：透多少不是挑的，是拿两种极端桌面算出来的（2026-10-20 他回「太丑了太难受了」→「搜索框来点透明」「在dev改呀」）
- 那扇本来就是 `WA_TranslucentBackground` + 无边框，但 `panel` 的 alpha 是 247/255 —— 也就是**名字叫半透明、实际几乎不透明**。他要的"透"就是把这个真的透下去。
- **透多少不能凭眼睛挑**：底下是壁纸，字读不读得清取决于桌面多亮多暗。所以拿两个极端当最坏情况 —— 整块纯白、整块纯黑 —— 把面板按 alpha 合成到这两种底上再量对比度。新判据就是把这张表钉在 `--cfgcheck` 里（26 条）：`夜间 透 85%（正文 8.69:1 / 灰字 3.35:1）；日间 透 80%（正文 9.30:1 / 灰字 4.61:1）`，门槛是"两个模式**正文**在最坏桌面下都过 AA 4.5:1"、"灰字 ≥3:1"。
- 判据当场抓到我自己算错一次：node 那一版我拿**提亮后的**夜间灰字试的（80% → 3.43:1），真表里那颗还是 `#8b93a1` —— 落地一量是 **2.82:1**，红。要么把夜间灰字提亮（动他用了很久的默认外观，他没点头），要么退一档透明度。**选了退档**：夜间 85%、日间 80%。
- 日间灰字顺手从 `#525863` 压到 `#474c56`：它在不透明的设置页上本来就 7.34:1（原来 6.07），压深只是为了让透出来那块也还读得动 —— 这一档**没有牺牲任何东西**，是白捡的。
- **这是没有毛玻璃的透明**：底下是壁纸原样。要 Windows 11 那种磨砂得接 DWM `DwmEnableBlurBehindWindow`，而它配 `WA_TranslucentBackground` 在个别驱动上会把整扇窗画成**全黑** —— 那个我离屏看不见（`grab()` 后面没有桌面），所以没擅自装。要就下一刀接，第一版得他亲眼过。
- 顺带：那条量描边的判据读数从 1.86:1 变成 **1.98:1**（夜间），不是描边改了，是**底透下去之后合成出来的底变了** —— 这条判据量的是真画出来的像素，所以它会跟着底动，符合预期。
- 渲染改成渲**整扇设置窗**（左边菜单 + 右边那张页），不再只渲单页：他说"太难受了"看的是整体，只渲 ConfigPage 会漏掉菜单和页眉跟内容之间的配色关系。整张平均灰度 夜间 39 / 日间 238。
- 离线合计 **690 条 0 红**；`theme_gate` 33 处漏 0、48 颗角色无重复。版本 **1.3.0 不升**（同前：从没打 tag、没有产物以这个号存在过）。

## 撤掉透明、上苹果那套白（P1 iOS 分组）：连打两轮自造配色之后改摆有出处的体系（2026-10-20 他「先把透明干掉,试试p1」）
- **方向是减法**：透明那一刀他看完直接撤 —— `panel` 的 alpha 回到不透（夜 247/255 是这一版一直以来的值、不是残留档；日 255）。撤显示层要连**只测那层的判据**一起撤：那条"压到纯白/纯黑两种桌面上正文还过不过 AA"整块 48 行删掉，不留空开关。
- 第三版不再自己调色，改成摆**有出处的体系**（Apple HIG / iOS 设置页 / macOS 窗口 / apple.com 四选一）。他挑了 P1。
- P1 的要点不是"把底压深"，是**中性 + 分组层次**：页面 `#f2f2f7`（蓝减红 +5，A 那颗是 +10，带蓝味多半就是上一版难受的原因）→ 卡片纯白（1.12:1）→ 发丝线 `#d1d1d6`。日间 `card`/`cardLine` 两颗因此从 rgba 蒙白改成 `#ffffff` + `#d1d1d6`。
- **苹果自己的浅色规范不过 WCAG AA，这条先摊开再抄**：iOS 的 `secondaryLabel` = `rgba(0,0,0,0.5)` 压白底 **3.95:1**（AA 要 4.5）、白字压 iOS 蓝 `#007AFF` = **4.02:1**。所以这两颗按 AA 调深（次级 `#6e6e73` = 5.07、主色用官网那颗链接蓝 `#0066CC` = 白字 5.57），"照抄苹果"和"过 AA"不能同时要。
- 主色家族是**按"白字压上去 ≥4.5"筛的**，不是按好看：`#0066CC` 5.57 / hover `#0072ce` 4.89 / pressed `#0058b0` 6.95 —— 中间试过 `#1a7ad4`（4.39）和 `#0a84ff`（3.65，那颗才是 iOS 现行蓝），都不够所以没要。
- 日间三档次级文字（`muted`/`groupText`/`sectionText`）**并成一个值** `#6e6e73`：苹果那套里本来就只有一档 secondaryLabel，留着三个名字各给一个近似值是自造层次。夜里那三档差得开，名字和值都不动。
- `dim` 是唯一保留苹果原味的低对比档（`#a1a1a6` = 压白卡 2.57:1），因为它画的是**按不动的非可执行行** —— 禁用态按 WCAG 1.4.3 免检。这条写在表的注释里，不写成"看着淡一点"。
- 门槛 near-miss 自己先补过再交：P1/P3 的描边第一版算出来 2.98 / 2.99:1（差 0.02 没过 3.0），挪深一档到 `#868a90` / `#898a8f` 才 3.11 / 3.16。
- 对照页自己踩了一次静默失败：`<script>` 里一个括号不配对 ⇒ 整段不执行、页面只剩静态文字和标题，**截图字节数从 194KB 掉到 54KB 是唯一的哨兵**。之后交之前先把 `<script>` 抽出来跑 `node --check`。
- 落地后实测：`--cfgcheck` **25 条 0 红**（26 减掉撤掉的那条）、整扇设置窗平均灰度 夜间 39 / 日间 240、描边判据 夜 1.86:1（回到原值）/ 日 3.11:1。两张 PNG 我 Read 过才报绿。
- 版本：**1.3.0 不升**（同前几刀：从没打 tag、没有产物以这个号存在过）。

## 配置页拆成「通用 / 外观」两个 tab（2026-10-20 他点名，且强调"其他的不动"）
- 形状：`QTabWidget#cfgTabs` 挂在 outer 布局里，第一页**直接就是**原来那颗 `QScrollArea`（不额外包一层），第二页是一张只放「外观」分组框的页 + `addStretch(1)`（东西少，别让分组框被拉到整页高）。「保存」那颗留在 tab 外面 —— 两页都要能存，挪进任何一页都是坑。
- 只搬了「夜间 / 日间」那一行，其余 7 颗分组框一个没动（他的原话）。
- **判据先红了一次，红的是探针不是代码**：那条"每个分组框都真的摆在布局里"的走布局探针只认 `QScrollArea` 的特例（"滚动区里那层不是布局项"），不认 `QTabWidget` —— 拆完当场把 7 颗分组框全报成"没摆进布局"。修法是把"访问一个控件"抽成 `visit()` 只写一遍（自己的布局 + 滚动区 viewport + tab 的每一页），而不是再叠一个特例。
  - 踩到的第二层：tab 第一页**本身就是**那颗 `QScrollArea`，所以 `visit(tab 页)` 必须走同一条递归，光 `walk(page->layout())` 拿不到里面的东西 —— 这就是为什么原来那个滚动区特例在 `visit` 里而不是在调用点。
- 新加 3 条判据（`--cfgcheck` 25 → **28 条**）：正好两个 tab 且顺序是 通用/外观；那两颗单选**在外观这一页上**；其余分组框全在通用页、外观页只有「外观」自己那一颗（这条防的是以后有人往外观页随手塞下载代理）。
  - 反向验过第二条：把那行 `addRow` 从 `appearanceLay` 换回 `generalLay`（= 搬一半），判据当场红成 `在外观页 0 颗 · 别处 2 颗`。
- 渲染判据补了一张 `theme-look-tab.png`：拆 tab 之后只截第一页等于没截到新东西，"这一页是不是空得只剩一行"得用眼睛过。我 Read 过了 —— 是空的，只有一行，这是他要的形状。
- 一处冗余我**没自作主张改**：外观那一页上分组框标题是「外观」、里面那行的标签也是「外观：」，看着重复；通用那一页同理（tab 叫通用、第一颗分组框也叫通用）。他说"其他的不动"，所以留着，改法是把行标签去掉 / 把分组框标题去掉，一句话的事。
- 版本：**1.3.0 不升**（同前几刀）。

## 「软件在磁盘清理闪退了」：名字齐 ≠ 能一起跑 —— 结构体布局漂移，`abi_check` 那一栏是假绿（2026-10-20 04:00 起查）
- 两层取证先分清"崩没崩"：事件日志 `10-20 03:52:31 Application Error id=1000` —— 故障模块 **`ltool-disk.dll`**、异常码 **0xc0000005**、偏移 **0x706a**；WER 有 `ltool.exe.33708.dump`（03:52:36）。**不是我 03:57 那次 `deliver.ps1` 停的实例**（时间对不上，差 5 分钟）。它自己的日志留了现场：`03:51:40 建页 disk` → `03:52:31 FATAL CRASH: code=0xC0000005 addr=7ffc44b3706a`。
- 反汇编那颗 dll 的 `0x18000706a`（image base `0x180000000` + RVA `0x706a`）：`mov (%rcx),%rax` → `call *0x20(%rax)`，前后夹着 `lock incl 0x8(%rdi)` / `lock xadd %eax,0xc(%rdi)` —— **拿垃圾指针做引用计数和虚调用**。错位读出来的 `QString` 就是这个形状。
- 根因：`src/DiskScan.h` 的 `struct Tree` 在 `d42eb64`（**10-17 18:56**，磁盘清理 MFT 第一刀）里加了 `QString engine; QString engineNote;`（`Progress`/`Timing` 也各加了）—— 大小和字段偏移全变了。而**装着的 `ltool-disk.dll` 的 PE 时间戳是 10-16 11:27**，比那次改动早一天多。它按老偏移读新 core 返回的 `Tree`。
- **为什么 `abi_check.py` 没抓到**：它只比**导出名字**。disk 导入 core 的 5 颗名字**一个不缺**，`LoadLibrary` 也真成功 —— 于是那一栏报"缺 0"、一片绿，然后运行期崩。#91 之前被理解成"6 张卡打不开"，这次证明它的另一半是**"名字齐的这批会闪退"**。
- 更要紧的是：**`d42eb64` 已经在发布出去的 v1.1.1 里**（`git merge-base --is-ancestor d42eb64 v1.1.1` = 是）。所以这颗雷从 10-19 装上 v1.1.1 那会儿就在，不是今天这几刀带进来的。
- 新增 `tools/abi_stale.py`（第二道 ABI 闸，接进 `deliver.ps1` 的 `stale` 栏）：数 **dll 的 PE 编译时刻 vs 它 include 的那些 core 头最后一次"动布局"的提交时刻**。判定"动布局"= diff 里加/删了数据成员（那行**既无 `(` 也无 `)`**）或 `virtual`；`static`/`constexpr`/`inline` 开头的排掉（不占对象空间）。当前：装的那棵 **只有 `disk` 陈**（= 崩的那颗）、商店 dist 里 `disk` + `clipboard` 两颗陈。
- 写这道闸自己踩了三个坑，全都会造成假绿，都记下来：
  - `git diff <base>.. HEAD` 是**非法用法**（rc=129），我第一版没查退出码 ⇒ 直接报"33 颗全 OK"，而 disk 明明该报陈。**git 非 0 退出必须抛。**
  - `subprocess(text=True)` 在这台机器上按 **GBK** 解 diff 里的中文注释 ⇒ 抛异常被吞、又是假绿。要 `encoding='utf-8'`。
  - 只按"这头被改过就算陈" ⇒ 33 颗里报 31 颗（全是 `Utils.h` 那种只加函数声明的改动），等于没信号；而 `bool async = false);` 这种**函数签名续行**会被当成数据成员。
- 影响面（量化）：10-16 以来**动了布局**的 core 头有 7 颗（`DiskScan.h` +10、`PluginStore.h` +19、`AboutPage.h` +3、`ToolsPage.h` +3、`PluginHost.h` +2、`CaptureHistory.h` +2、`ClipboardStore.h` +1），其中真被装着的插件吃到的只有三对：**disk←DiskScan（已崩）**、clipboard←ClipboardStore、capture←CaptureHistory。后两颗装的是 10-19 新编版，所以现在没事 —— 但**在商店点「更新」会把它们换回 10-16 那批陈的**。
- 结论对 #91 的影响：**"在 core 里补几颗转接壳"这个选项治不了这次的问题** —— 壳只能补名字，补不了结构体大小和字段偏移。要么把 19 颗插件全部对这份 core 重编重发，要么每次发 core 都同时发插件。等他在这一层重拍。

## 「注册表分析和开机分析为什么点不开」：两层原因，第二层是界面在装没事（2026-10-20 04:0x）
- 第一层（为什么打不开）：`ltool-registry.dll` 和 `ltool-startup.dll` 各自 `LoadLibrary` 失败，缺的是**同一颗** core 导出 —— `Utils::revealInExplorer(QString const&)`。这名字在跨平台那几刀里搬进 `Platform::` 之后从 core 的导出面缩掉了，而装着的这两颗是 10-16/10-18 编的。`abi_check` 数出来：registry 导入 16 缺 1、startup 导入 9 缺 1。
- 第二层（**这才是我改的**）：点了之后**日志有、界面上一个字都没有**。日志里明明白白：`plugin: 打不开 registry：插件 registry 没能加载（看日志 plugin: 那几行）`（他 04:02:43/44/45 连点三对）。屏幕上只是回到卡片墙。
- 而且卡片上那行状态**本来就是准备说实话的**，结果是个半成品：`"%1 · 本机有这个 dll 但没通过检查：%2"` 的 `%2` 走的是 `meta(id) ? QString() : "目录里认不出它"` —— **`meta` 存在时取的是空串**，界面上就是一行以冒号结尾的话。registry/startup 正是这种：plugin.json 解析得好好的（`meta` 有），只是装载失败。所以"没通过检查："后面永远什么都没有。
- 修法：① 那句话改成走 `PluginHost::loadErrorOf(id)`（启动扫完就留下了真原因）→ `md->error` → 兜底"目录里认不出这个文件"，三级回退，颜色换成 `@bad`；② `showTool()` 失败那一支不再只写日志，把原因写进**页眉**（`插件商店 · 注册表分析 打不开：<原因>`）—— 页眉本来就是"现在在哪个插件"的位置。
- 判据 3 条（`--storetest` 73 → **76**）。测的是**拼法**不是卡片，因为这一支只有装载真失败过才走得到、判据造不出一颗坏 dll（造出来就得往真插件目录里写东西）。所以把 `ToolsPage::cannotOpenText(stateWord, loadErr, metaErr)` 抽成静态纯函数，三条精确断言：有原因时原样带出来、两个原因都在时说装载那个、**三个都空时不许以冒号结尾**。反向验过：把兜底那句拿掉，第三条当场红。
- **这 3 条第一次插错了地方**：我插进 `--storenet` 那一支 —— 它要联网、不进 CI，等于没人跑。跑 `--storetest` 数出来还是 73 条才发现。教训：新判据必须看它落在哪个旗标下，"写了"不等于"会被跑到"。
- 和挂着的 #101 是同一类（"提权脚本失败原因现在只写退出码"）：**失败要能在界面上读出来**，日志只算半条。
- 版本：**1.3.0 不升**（未发布号吸收；这批改的是 core 的界面文案 + 判据）。

## 「根治」#91：商店改由发版那次构建供货 + 接口号推到 4 + 一道"该升号就升号"的闸（2026-10-20 12:3x 起）
- 他一个字「根治」，拍的是我摆的那条：19 颗对这份 core 重编重发。动手前先把**为什么会漂**量清楚，不然只是再拷一次 dll：
  - 商店读的是 `mateo-ki/ltool_plugin` 仓库 main 上**手工提交进 git 的 `dist/`**：最后一次 10-16 19:32，core 之后一直没停。
  - 那个仓库的 `plugins/` 是主仓库源码的**分叉**：`diff -rq` 实测 14 颗里 **8 颗不同**，`record` 少了整个 `WindowPreview.cpp/.h`。
  - 于是"发一次商店"和"发一次 core"物理上就是两件事 —— **漂是结构性的，不漂是运气**。补转接壳治不了（上一节说过：壳补名字，补不了大小和偏移）。
- **第一刀（渠道）**：商店那份清单和 19 颗 dll 改由**主仓库发版那一次构建**发出去。
  - 新增 `tools/make_plugin_index.py`：sha256/字节数从 `build/Release/ltool-plugins` 现算，名字/描述/关键词/Api/Version 从各 `plugins/<id>/plugin.json` 读，一条都不手抄；产 `plugins-index.json`。
  - 本机构建那份实测：**19 条全进、INDEX_BAD=0**，`core` 字段自动写成 `来自 ltool 1.3.0（插件接口 API 4）`。
  - CI 加两步：「打插件商店清单」（先数产物颗数 == `plugins/` 目录数，再用商店自己的解析读一遍这份 JSON，缺字段就红）+「发版前两道 ABI 闸」（`abi_check` 和 `abi_stale` 对**要发出去的这批产物**跑，红就不许挂 Release）；`gh release create` 的资产从 2 个变成 21 个（安装包 + SDK + 清单 + 19 颗 dll）。
  - `PluginStore::defaultIndexUrl()` 不再指插件仓库，改成 `releases/download/v<我自己这版版本号>/plugins-index.json` —— **按版本号点名，不用 `latest`**：latest 有两个坑（带 `-beta.N` 的预发它不回，而且老 core 会拿到新 dll，那正是这次要根治的事）。标签 = `v` + `ltool.iss` 那个号，CI 本来就有一步硬验这件事。
  - 直链的式子搬进 `AboutPage::releaseAssetUrl(tag, file)`：安装包那条和商店这条必须是同一个式子，仓库地址不写第二处。
  - `attemptsFor()` 原来只给 `raw.githubusercontent.com` 套镜像；`github.com/…/releases/download/…` 现在也套（这条线路 github.com 直连间歇挂死比 raw 更常遇到，少排那两条腿就是"商店在国内装不上插件"）。
  - **老用户不受这次改动影响**：商店地址是**编译进 core 的**，≤1.1.1 那几版读的还是插件仓库那条 raw 地址、那批 API 3 的 dll —— 那套自己是对得上的。
- **第二刀（号）**：`LTOOL_PLUGIN_API_VERSION` 3 → **4**，iid 跟着 `/3.0` → `/4.0`。
  - 接口一个字没加。升号的是 **core 那一侧**：`DiskScan::Tree` 多了两个 QString、`ClipboardStore` 多了 keepDays、`CaptureHistory` 的行多了 key/pixmap、`Utils::launch/revealInExplorer/trimWorkingSet` 搬进 platform 缝。
  - 效果：`PluginStore::compare()` 里**本来就有的** `State::ApiMismatch` 那条从此真能命中 —— 商店卡片写"清单里的 Api 是 3，这台机器上的宿主只认 4"，`QPluginLoader` 的 iid 再自己拒一遍。**从"进来崩"变成"拒之门外并说清"。**
  - 19 份 `plugins/*/plugin.json` 里手抄的 `"Api": 3` 全部跟着改 4（那个数是插件自报家门的来源，写错了宿主就按错的放行）。
  - 每颗自己的 `Version` 都 +1（18 颗 patch，`clipboard` 那颗本来就在 1.1.0 → 1.1.1）：**不动号的话商店那张卡显示"已装 · 太旧"却没有「更新」可点** —— 商店判"可更新"比的是版本号，不是哈希。`plugin_version_gate.sh` 绿（GATE_PLUGINS=19 NEEDS_BUMP=0 WENT_BACK=0）。
- **第三刀（闸）**：新增 `tools/core_abi_gate.py` —— **公开头动了布局却没升号 = 红**。
  - 判据不重写：`last = git log -1 -S'#define LTOOL_PLUGIN_API_VERSION <当前值>'`（**必须连着值一起搜**，`-S` 找的是"这串出现次数变了"的提交，只搜前缀就永远找不到、闸静默假绿），从那一笔比到**工作树**，用 `abi_stale.py` 那条"什么才算动布局"的判据扫 `make_sdk.py` 那份头闭包（25 个）。
  - 顺带盯 19 份 plugin.json 的 `Api` 和头里的数一致。
  - **先红后绿**：升号之前跑 = `ABI_API_GATE_HITS=3`（`CaptureHistory.h` / `ClipboardStore.h` / `DiskScan.h`），正是这次的三个真凶；升到 4 之后再跑 = 0。
  - 第一版误报一行 `platform/Launch.h: class QProcess;` —— **前向声明一个字节都不进对象**。修的时候顺手把判据抽成 `abi_stale.layout_lines(diff)` 一份，两道闸共用（原来我在 core_abi_gate 里抄了一份，注释还写着"同一份判据"，那就是下一-次漂）。
- `deliver.ps1` 补两栏（**把以前手工做的事变成闭环里的事**）：
  - `plugins` 栏：装完之后**逐颗对齐**到刚编出来那 19 颗，报"本来就是这个哈希 / 换成新的 / 原来缺 / 多出 / 被占用没换成"，并点名目录里的 `.old.*`。以前这步是我手工拷的 —— 手工的那次就是漏的那次（10-19 那 6 颗、10-20 这颗 disk）。
  - `installed` 栏：拿**装好的** `ltool.exe` 再跑一遍 `--plugintest`（`QT_QPA_PLATFORM` + `QT_QPA_PLATFORM_PLUGIN_PATH` 两个变量一起给，安装包 Excludes 掉 qoffscreen，只给一个就"没有可用平台插件"）—— 10-19 立的那条不变量进脚本了。
  - `abi`/`stale` 两栏量的目录改了：从"商店 dist"改成"本机构建 + 装着的那两棵"。**量错对象会得出反向结论**（第 48 条），dist 已经不是渠道，继续量它等于每轮喊一次没人能修的狼。
- 代价，如实说：**号一升，装着的旧插件全都不再被装载**（F1/F2 和所有卡片一起没声，不是某个功能坏）。所以"换 core 那一轮必须同时换插件"是硬要求，这就是上面那栏存在的原因；别的老用户则按卡片上那颗「更新」一颗颗换（商店版本号和清单都跟着 core 发，点了就配上）。
- 判据：`--storetest` 76 → **80**（内置地址按版本号点名、这条地址要能吃到两条镜像、`releases/download/...dll` 也排四个候选、套镜像不改路径、`github.com` 上**不是**发版资产的路径不套镜像 —— 最后那条是反向钉住"只认 host"的实现）。`--storenet` 加了第一行打印**实际用的清单地址**：这一版还没发商店资产就是 404，后面"卡片张数"会跟着一起红，那不印这行下次没人分得清"商店坏了"和"还没发版"。
- 全量离线 17 个旗标 **0 红**（口径：`grep -c ^PASS + ^ok`，比文档里手工点的口径宽）：`--plugintest 242`、`--storetest 80`、`--cfgcheck 28`、`--cliptest 73`、`--gridtest 64`、`--pinyin 71`、`--username 36`、`--about 27`、`--appdirs 12`、`--linuxdotest 44`、`--storeexit 14`、`--seamtest 38`、`--rowmenu 9`、`--diskui 8`、`--mfttest 10`、`--diskbranch 4`、`--smoke` 只报文件数。
- 还挂着三件：**① `v1.3.0` 的 tag 没推** —— 商店那条地址要等这次发版才有资产，他点头我才动公开 Release；**② 插件仓库那头**的 `dist/` 和它 CI 里那句 `api != 3`（现在该是 4）—— 那是别人的仓库，要不要清/要不要顺手把渠道关掉，等他；**③** 商店从此没有"清单外"那 5 张卡了（19 颗全在同一份清单里），`#95` 里"缺哪几颗"这半已被 `plugins` 栏做掉。
- 版本：**1.3.0 不升**（未发布的号吸收同批功能；这轮里没有新的用户可感知功能，是把 #91 那条根拆掉）。但**必须发**：这一版的商店要等它进 Release 才有东西可拉 —— 这是"发版"而不是"改代码"的一半，等他拍。

## 「注册表分析和开机分析还是点不开」的第二层：商店那条装不了 ≠ 本机那颗打不开（2026-10-20 13:3x，v1.3.1）
- 他报的还是同一句。这次先取证再动手，形状和上午那次**不一样**：
  - 常驻实例（pid 12300）日志里 19 颗 `plugin: 看到 …` 全在，registry / startup 都在，**一条 `打不开` 都没有**；
    另起一份进程拿 `D:\software\ltool\ltool.exe` 跑 `--plugintest` 是 242 过 0 红，`plugin: 建页 registry`、`建页 startup` 都成功
    ⇒ **dll 是好的、起得来**。所以不是装载失败。
  - 而界面上点卡片没反应、日志里连一次 `showTool` 都没有 ⇒ 拦在卡片那一层。
- 根因是我上一轮升 API 号时留在 `PluginStore::compare()` 第一行的那句 `if (e.api != 宿主) return ApiMismatch`：
  它把**两件不同的事**并成了一件事 —— "清单里那一条能不能从商店拿下来装" 和 "本机这一颗能不能用"。
  他机器上装着的是本机构建那份（Api 4、起得来），而缓存里那份清单还是商店旧那份（Api 3），
  于是每张卡都走 `ApiMismatch` 那一支：**一颗按钮都不给、`setBodyClick` 给了空函数** ⇒ 点什么都没反应，
  连"禁用/卸载"都没了。这条正是他自己 10-20 上午问的那个问题的**新形状**，症状一样、原因不同。
- 改法（改谓词，不是改文案）：
  - `compare()` 拆成 `localCompare()`（只管本机这颗：装了/可更新/与商店不同/被禁用/没装）+ 外面那一层再叠 Api 那一半：
    **只有"本机没这颗"时才叫 `装不了`(ApiMismatch)**；装着的时候返回原本那一档，note 换成
    `清单里的 Api 是 3，这台机器上的宿主只认 4：换不了商店那份 · 本机这颗照旧（…）`。
  - 新增一个谓词 `PluginStore::installable(entry)`（= 那条的 Api 和宿主对得上）+ `Row::entryUsable`，
    界面**只问它**决定「装 / 更新 / 换成商店那份」给不给，不再自己比 api（比第二处就会和这里说法不一）。
    `禁用` / `卸载` / 点正文开页这三条出路不受它管。
  - `ToolsPage::makeCard`：`Missing`/`Update`/`Different` 那三颗"从商店拿一份"的按钮都挂在 `r.entryUsable` 上；
    `Update`/`Different` 的正文点击仍按 `openable`（宿主认不认得出本机那颗）。
- 判据 `--storetest` 80 → **84**（+5 −1）：撤掉「就算文件在，Api 不对也先拒」——那条钉的就是这个错形状；
  换成 ①没装着+Api 不对=装不了 ②装着+Api 不对=**与商店不同**（不再锁死） ③那句原因要把两半说开
  ④⑤ `installable()` 两个方向各一条。**新加的 ① 当场红过一次**（`got=[已装] want=[装不了]`）——
  我那条"先删 good 文件"的清理写在了检查后面，判据读到的是上一条留下的文件；把 `QFile::remove` 挪到这一段最前面。
  老实说一句没量到的边界：`entryUsable=false` 时"按钮真的少给没给"这一层只在数据层钉了谓词，
  卡片那一层要造一颗坏 dll 才量得到，没造（上一轮 `cannotOpenText` 也是同样原因抽成纯函数测的）。
- 版本：**1.3.0 已发布 ⇒ 这个号不能被后面产物复用**，所以这刀走 **1.3.1**（修 bug = patch）。
  顺带记录 v1.3.0 的发版实况：tag `v1.3.0` = `e2545e2`，run 39374556199 completed success，
  Release 资产 **22 个**（安装包 22,520,066 B + SDK + `plugins-index.json` + 19 颗 dll）。

## 把「点了没反应」变成能测的东西（2026-10-20 13:5x，1.3.2）
- v1.3.1 发完之后（run 39375940844 success，Release 资产 **22 个**，`plugins-index.json` 10,943 B，
  `releases/latest` 的 302 已指到 v1.3.1），拿 `--storenet` 复跑商店：**清单 19 条 · 19 张卡 · 能装 0 · 拒收 0 · 清单外 0**，
  走的是镜像 gh-proxy.com、试了 1 次 —— 上一轮那条唯一红的"状态行把数说清"自己转绿了。
- 但"点不开"这一类**当时还没有判据**：`--storenet` 只核对状态词 ↔ 按钮，正文给了空函数它看不出来。
  所以补上：`ShopCard::hasBodyClick()`（读 `m_onClick` 那个决策本身）+ 卡片挂 `ltoolClickable` / `ltoolOpenable` / `ltoolId`，
  逐张核对 `clickable == (没装那张 || (装了的四档 且 宿主认得出那颗))`。
- **反向验**（把 Different 那张的正文点击摘掉重编）：当场红并把 19 张逐一点名到 id
  ——「与商店不同」(ads) 那张 正文可点=没（该=有，宿主认得出=是）…；改回来 19/19 全绿。
- 写这条判据自己踩的两发，都是"判据假象"那一族：
  - 第一版把 `setProperty("ltoolClickable", card->hasBodyClick())` 写在 **makeCard 开头** ——
    那时 `switch` 还没跑、`m_onClick` 还是空 ⇒ 19 张卡全体红 `正文可点=没`。
    **红的是判据自己**，不是产品：这类"读决策的时机比决策早"的错误只会以全量红的形式出现，先怀疑探针。
  - 第二版用状态档推 `wantClickable`，会漏掉"装着但宿主认不出"那种诚实的不可点 ⇒ 改成把 `openable` 也挂出来，
    判据拿**两个决策输入**算期望，而不是拿状态词猜。
- 顺手补一条 INFO：清单没落地时 `--storenet` 里"逐张卡片核对"整段挂在 `hasMatch()` 里、一条都不会跑，
  以前只红那一条状态行、看着像"卡片验过了"。现在明写 `WARN 逐张卡片的核对这次没跑…别当成验过`。
- 版本：**1.3.1 → 1.3.2**。这轮只有判据 + 三颗诊断属性（无用户可感知变化），按"测试/文档不升号"本来可以不升；
  但**"已发布的号不能被后续产物复用"优先** —— 装到他机器上的这颗 exe 和已发出去的 v1.3.1 资产不是同一份，
  再挂 1.3.1 就变成"关于页/卸载表说自己是 1.3.1、哈希却不是那个 1.3.1"。1.3.2 现在是未发布号，吸收后续同批工作。

## v1.3.2 发出去之后的商店实链路（2026-10-20 14:2x）
- tag `v1.3.2` = `02d1911`（tag object `954b6127`），第 4 轮重试才推上去（`github.com:443` 那阵整段超时）。
  run 39378840228 作业 26 步全 success，其中第 23 步「发 GitHub Release」真跑了（标签推的 run 才发）。
- Release `v1.3.2` 资产 **22 个**：`ltool-1.3.2-setup.exe` 22,525,536 B `sha256 4b1e78427e5be8ed…78d2396`、
  `ltool-sdk-1.3.2.zip` 266,522 B `bf945406adb42c47`、`plugins-index.json` 10,943 B `f9189da0eb66bf5d` + 19 颗 dll。
  `releases/latest` 实测已指到 **v1.3.2**（这一条是拿 api 读的：网页那条 302 当时正好连不上，`curl` 也超时）。
- 拿**装好的** `D:\software\ltool\ltool.exe --storenet` 复跑：状态行
  「清单 19 条（… v1.3.2/plugins-index.json · 走的是 镜像 gh-proxy.com，一共试了 1 次）· 这一面 19 张卡 ·
  能装或可更新 0 · 拒收 0 · 清单外 0」，各档 `与商店不同=19`（其中"本机号比商店高、等商店重发产物"的是 **0 张**）——
  这就是"插件 dll 和 core 同一次构建"应有的形状：版本号一致，只有 PE 时间戳带来的哈希差。
  **逐张卡片的核对（状态词 ↔ 按钮 ↔ 正文可点）16 条全过**，新加那条点击判据在真数据上是绿的。
- 那一趟唯一的红是「检查更新走网页 302」：`got=[HTTP 0 · 最终 …/releases/latest]`。
  单独复跑 `--updatecheck` 同一分钟也是 `Operation timed out`（`curl -I` 同一条 15 秒超时），
  而界面给的是一句实话：「检查更新没走通：Operation timed out。可以直接点 打开发布页 看最新版本」——
  **是出口那一分钟不通，不是代码**；同一轮里清单那条走镜像 1.6 秒就 200 了。
- 他自己那一头的实证（不是我造的点击）：14:07:53 他机器上那份 1.3.2 实例打出
  `plugin: 建页 registry` + `store: 展开 registry（stack 第 1 格 / 共 2 格，之前在第 0 格）`，
  14:08:09 `registry page: deleted 0/2` —— 那张页**真被他自己点开并跑了一次清理**。
  `deleted 0/2` 的另一半也已经是实话：状态行写「已删除 0/2 项 · 备份在 backups\registry-backup-*.reg ·
  失败 2 项（<第一条原因>）」，而 `RegClean` 对 `ERROR_ACCESS_DENIED` 给的正是「需要管理员权限」。

## PCH 藏住漏 include：只有"独立编"那一侧会红（2026-10-20 14:3x，1.3.3）
- 把插件仓库的 `plugins/` 按主仓库源码同步过来、SDK 重生成（API 4 / 25 个头 / ltool 1.3.2）之后独立编 19 颗：
  **16 颗过、3 颗红** —— `capture/captureselftest.cpp` 少 `<QPushButton>`、`grid/gridselftest.cpp` 少 `<QLabel>`
  （还有一行重复的 `<QApplication>`）、`linuxdo/linuxdoselftest.cpp` 少 `<QThread>` 和 `<QTimer>`。
- 为什么主仓库从来没红过：`target_precompile_headers(ltoolcore INTERFACE src/pch.h)` 挂在 **INTERFACE** 上，
  整棵树（含 19 个插件目标）都白拿一份预编译头，里面正好有那些 Qt 类 ⇒ **漏写 include 在被链接的那棵树里看不见**。
  插件仓库那份是独立构建（链导入库、不吃这棵树的 PCH），于是它才是这道题的考官。
  这正是 [[feedback-verify-before-concluding]] (40) 那条的 Qt 版本：grep 自造的名单不算测量，**换一种编译形状才算**。
- 修法在主仓库改源码（补三行 include），不在插件仓库改 —— 那棵树只是镜像。
  插件版本号跟着走：`capture 1.0.6→1.0.7`、`grid 1.0.1→1.0.2`、`linuxdo 1.0.1→1.0.2`
  （`plugin_version_gate.sh` 报 `NEEDS_BUMP=3` 时我是先看见红才去 bump 的，闸是对的）。
- **插件仓库这一轮顺手根治了它的"第二份源码"**：`plugins/` 由脚本从主仓库同步（19 个目录、每目录一份
  4 行 `CMakeLists.txt` 模板：GLOB 自己目录的 `*.cpp` + `plugin.json`，类名从主仓库 `CMakeLists.txt` 现读，
  不手抄），`sdk/` 由主仓库 `tools/make_sdk.py` 重生成。它的独立构建 + `abi_check`/`abi_stale` 两道闸都过了
  （19 颗、缺 0 个符号、布局陈旧 0 颗，dll 里个人路径 0 命中）。

## 日间模式剪贴板一整墙看不清：自绘那张页抄了一份深色真值，因为它读不了 `rgba()`（2026-10-21，1.3.4）
- 他这句是「日间模式剪贴板每个卡片很难看，那个背景完全看不清」。病因不在剪贴板：
  `ClipCardDelegate::paint` 里那三颗颜色是**手抄的深色字面量**（`255,255,255,20` / `255,255,255,31` / `#3f6ae0`），
  而它抄的不是懒 —— 表里 `card`/`cardLine` 那两颗当时写的是 CSS 的 `rgba(...)`，**QColor 解析不了**，
  自绘代码拿不到，只能自己抄一份。于是切到日间=白卡上画白字白线。
- 修法是把那两颗角色改成 `#AARRGGBB`（QColor 和 QSS 都认，`rowSel`/`hairline` 那一族早就是这么办的），
  剪贴板改成只问表：`ClipboardPage::cardInks(bool selected)` 一颗函数当 delegate 的唯一取色口，
  再给这张页接 `QEvent::ThemeChange` → `viewport()->update()`（不然切了模式卡片要等鼠标划过才换色）。
  夜间那两份值按 alpha 换算（0.08→`0x14`、0.12→`0x1f`），**换算不是改外观**：夜里量到的三个数一字没动。
- 半透明必须**先合成再比**：`Theme::over(卡片底, 列表底)` 出来那颗不透明的"纸"，正文/小字/描边都压它算。
  拿 `rgba` 的原始 rgb 直接算会得出假的高对比（白卡 8% 白的 rgb 就是 255,255,255，压黑字能算出 20:1）。
  这套公式原来在 `src/main.cpp` 的 `--cfgcheck` 里是一颗局部 lambda，这次搬进 `Theme`（`relativeLuminance` /
  `contrast` / `over`），cfgcheck 那两条改读它 —— 同一个算式不许有第二份。
- 实测（`--cliptest` 打的 INFO 行，正文/小字/描边）：夜间 `12.01 / 4.63 / 1.47` · 日间 `16.83 / 5.07 / 3.47`。
  **第一版量到的日间描边是 1.52** —— 那条照抄了 iOS separator 的 `#d1d1d6`，白卡压白页等于没有线，
  他说的"背景完全看不清"有这一半；换成启动器那圈描边同一支 `#868a90` 才到 3.47（WCAG 1.4.11 非文本要 ≥3:1）。
- 四条新判据钉的是**决策的输入**（`cardInks()` 那个返回值），不是截图：日正文 ≥4.5、夜正文 ≥4.5（改颜色不许弄坏老模式）、
  日描边 ≥3、夜描边 ≥1.4（夜里那条一直是 1.47，他这次点的是日间，拿 3:1 去动他用了很久的默认外观得他点头）。
  两个模式各渲一张 PNG 到 `%TEMP%\ltool-clip-cards-{night,day}.png` 供肉眼复核 —— 离屏没中文字形，字是豆腐块，那就核色块和线。
- **连带读到这两颗角色的地方**（改共享角色必须报的账）：`src/ToolsPage.cpp:103` 商店卡片墙 `border: 1px solid @cardLine`
  和 `src/SearchWindow.cpp:260` 的 `background:@card`。日间商店卡片那道线一起变深（同一族问题，一次改到位），
  启动器 hover 底没动（日/夜换算前后同值）。`--cfgcheck` 那 11 条（含描边 3.11:1 那条）跑完一字没动。
- 为什么这道事故能发生：`tools/theme_gate.sh` 只扫 `src/`，**`plugins/**` 是明确写在闸外的**（19 颗 dll 各有一份写死的深色，
  改它们=重发全部产物）。所以"漏一处没人响"在插件侧连数的人都没有。这次只把剪贴板这一颗接进表，
  README 那一行原来把「剪贴板」列在"切不动"名单里，现在按实际改掉（录屏、置顶窗、宫图仍在名单里）。
- CI 那圈离线判据原来**没有** `--cliptest` 和 `--storetest`（12 面里不含这两面）—— 那意味着这 4 条对比度判据
  只有我本机跑过。补进同一循环（14 面），两面都在本机离屏 `失败 0 项`（`--cliptest` 77 绿 + 1 行点名 SKIP，`--storetest` 84 绿）。
- 版本：core `1.3.3→1.3.4`（可感知的显示 bug 修复=patch；数据格式和插件接口都没动，`API` 保持 4，
  `core_abi_gate.py` 在改完的工作树上 `GATE_HITS=0`），剪贴板插件 `1.1.1→1.1.2`（每颗自己递增那道闸）。
- **SDK 闭包因此涨了一个头**：`tools/make_sdk.py` 量到 `25 → 26`，多出来的正是 `Theme.h`
  （`src/` 里只有 `.cpp` include 它，所以它以前不在任何插件的 include 闭包里；这次剪贴板第一个 include 它）。
  插件仓库那头要重生成 `sdk/` 才编得过这颗 —— 那是下一次同步的事。
- 拿**装好的那份**跑离屏判据时我又踩了一次弹框：安装包只打 `qwindows.dll`（`D:\software\ltool\platforms\` 实测就这一个），
  所以对着它设 `QT_QPA_PLATFORM=offscreen` 不是"安静地不跑"，是**Qt 自己弹一扇系统模态错误框到他桌面上**
  （"Available platform plugins are: windows."）。`deliver.ps1` 的 `installed` 那栏本来就是拿
  `build\Release\platforms`（那里有 `qoffscreen.dll`）喂给装好的 exe —— 我绕过了那道守卫。
  规矩：这两个变量是一颗原子的，`QT_QPA_PLATFORM=offscreen` 只能配一份**真有 qoffscreen.dll** 的 plugin path。

## 插件商店每天自动更新一次（2026-10-21 他点名，1.4.0）
- 他这句是「插件商店增加一个自动更新功能，一天自动更新一次，如果有新插件」。两句问答拍定：
  **范围 = 只换已装的那几颗**（清单里新出现的插件仍要点「安装」，不往机器上塞他没选过的 dll）；
  **时机 = 程序启动后后台跑**（不进商店也跑）。默认**开** —— 他这句是"自动做 X"，不是"给我个按钮"。
- 现状对了一下才动手：`store/lastFetchDate` 那套**每天拉一次清单**早就有（`shouldAutoRefresh`），
  缺的只是"拉回来之后自动装"那一步 —— 所以这一轮不是新造一条链，是把最后一段接上。
- **不平行实现**：「下到 `.part` → 验哈希和字节 → 落地」原来长在 `ToolsPage::runInstall` 里，和卡片上的
  按钮/状态行缠在一起。自动更新那趟没有界面，要么抄一条下载路（两条路早晚说法不一），要么搬出来。
  搬出来了：`PluginStore::installEntry(entry, parent, onProgress, done)` + `installInFlight(file)`；
  商店页那侧只剩"把进度写成字"（`src/ToolsPage.cpp:680` 起，回调签名换了，排队/`QPointer` 那套原样留着）。
- 那道 `installInFlight` 闸不是装饰：手动那路和自动那路确实能同时点中同一颗，两趟写同一个 `.part`
  必坏，而且第二趟的哈希一定对不上 —— 那在界面上会被说成"仓库被人动过"（`finishDownload` 那句原话）。
- 这一趟挑谁 = 纯函数 `autoUpdateTargets(rows)`：**只认 `State::Update`**。
  `Missing`（新插件）故意不要；`Different`（哈希不同但清单版本不比本机新）也不要 —— 那多半是本机
  手工同步的更新的一份，自动拿商店那份盖上去就是把他的东西换掉；`Disabled` 更不要（他亲手禁用的，
  不许顺手启用回来）；`entryUsable=false`（Api 对不上）的即使有新版也不换。反向验过：把 `Missing`
  放进谓词 → 当场红 2 条（`got=[upd1,brandnew]`）。
- 那句结果的话 = 纯函数 `autoUpdateSummary(ok, bad, src)`。**第一版三种形状都带"已落盘，下次启动用新的"** ——
  一颗都没动的时候那是假话（真跑第一次就撞上了：404 → 用缓存 → 挑出 0 颗，界面却说"已落盘"）。
  改成只有 `ok` 非空才带那句；判据跟着钉"没动过文件的那句不许说已落盘"。
- 商店那一页多一行**常驻**说明（`ToolsPage::autoUpdateLine()`，static 是为了判据不造一整扇页就能问它）：
  关着说"没开"并指路配置页、开着没跑过说"今天这一趟还没跑"、跑过就把结果带出来（含时间）。
  空白一行会被读成"这功能坏了"，所以三种情况都得有话说。
- 一天一次的闸是 `store/autoUpdateDate`，而且**先记日期再动手**：跑挂了不至于每次启动都刷网络
  （那的表现是"开机就卡"）。启动后延迟 15 秒才跑，冷启动那阵子在扫应用 + 预热窗口，别让慢网络把"呼出很卡"带上；
  带 `--` 旗标的自测进程一律不跑（`inLoopSelftest` 那颗谓词是现成的），否则就是边测边往插件目录写 dll。
- 实测（`--autoupdatenow`，本机）：`没有要换的（网络没成（… v1.4.0/plugins-index.json 404 …），
  拿本地缓存那 19 条清单试） · 这一趟 8169 ms`，第二次 24195 ms（那条链上"连上但不吐字节"，每条候选试两次）。
  **404 是版本钉死的必然结果**：清单地址是 `releases/download/v<我这版>/…`，1.4.0 没发之前那一版就没有清单 ——
  所以 `--storenet` 在发版之前是**预期会红**的（它第一条就是"清单拉到了"），别把它当回归。
- 判据：`--storetest` 84 → **99 条**（新增 15 条：默认开 / 关掉跟着关 / 今天跑过不再跑 / 日期过去就该跑 /
  从没跑过也该跑 / 只挑 Update 那一档 / 满墙新插件不自动装 / Api 对不上的不换 / 那句话三种形状 /
  没动文件不许说已落盘 / 三种说法都带"清单从哪条拿到的" / 商店那一行三种情况）。
  `--storenet` 17 → 18 条（拿真清单 + 真目录量"会挑谁"，只挑不装；挑了 0 颗时那条自己写明"在 0 个对象上，不算量到"）。
- 版本：`1.3.4 → 1.4.0`（可感知的新功能 = minor）。**1.3.4 没发布过，被这一版吸收**（号跟着发布走，不跟着 commit 走）；
  这轮只动 `src/`，19 颗插件一行没动 ⇒ 没有插件要 bump，`core_abi_gate` 在改完的工作树上 `GATE_HITS=0`（公开头加了
  几颗非虚函数，没动布局）。

## 这轮补测：内存和 Win10（2026-10-21 他问「测试了吗 → 测内存泄露和 win10 兼容性」）
### 内存：量出来一个真 bug，但不是泄漏
- 给搬出来的那颗 `PluginStore::installEntry` 加了一节离屏判据（`--storetest` 99 → **104 条**）：
  ① 起一趟、当场看"在飞"标记挂上了（判据量的是被测代码真造的那趟，不是我自己 new 的）；
  ② 把 parent 删掉（= 商店页在下载中途被拆）之后标记必须跟着放掉；
  ③ 连跑 200 趟真下载（空地址，一次网都不上）：每趟都要回话、跑完不许有残留标记、工作集涨幅有上限。
- **② 第一次跑是红的**（`got=[yes] want=[no]`）：标记只在 `done` 里放，而 `fetch` 的所有连接都挂在
  `parent` 上 —— 页没了 `done` 永远不会回。搬进 core 之前那个计数是页面成员（跟着页一起没，不出事），
  搬出来之后它是**进程级**的，于是那颗 dll 从此永远卡在「已经有一趟在飞」，他每次点都是同一句话。
  这就是 [[feedback-gui-delivery-visible-and-hands-off]] 那条"点了没反应被固化成永久失灵"的形状。
- 修法：标记的生命周期挂到下载本身 —— `new QObject(parent)` + `destroyed` 里放标记，
  正常收尾也走同一颗（`deleteLater`）。放的时候**对号**（`QHash<文件, 趟号>`）：
  不对号的话"上一趟的收尾"能把"下一趟刚挂上的标记"错放掉，那道闸就白装了。
- ③ 的量法顺带被 ② 救了：修之前那 200 趟全在闸口被"已经有一趟在飞"挡回去（`回话 200 次` 是 200 次**拒绝**），
  数字看着漂亮其实什么都没跑 —— 修完之后才是 200 趟真走完 `fetch`+`QFile`+chain。
  实测 **工作集 25 → 26 MB**（200 趟，约 5 KB/趟，是分配器留页不是线性涨），整面 `--storetest` 9.8 秒。
- 没测到的：真下成一趟（成功分支）的内存 —— 现在没有可用的更新产物，那条要等 v1.4.0 发出去之后拿真数据跑。
### Win10：这轮新代码一行 Win32 都没碰，但整套东西的下限就是 Win10，而且安装包以前没写
- `git diff` 扫过：新增的 81 行里 `windows.h`/`HWND`/`DwmGet*`/winrt **0 命中** —— 只有 QSettings/QFile/QNetwork/QObject。
- 拿 node 解了一遍 PE 导入表（`%TEMP%\peimports.js`，54 个依赖去重）：
  `api-ms-win-crt-*`（UCRT）、`api-ms-win-core-winrt-*`、`mfplat.dll`+`mfreadwrite.dll`（Media Foundation）、
  `d3d11.dll`、`dwmapi.dll`。**没有 Win11 独有的导入** —— 所以不是"只能在 Win11 跑"。
- 但依赖面里三处是 Win10 内部有档次的：
  ① `plugins/record` 用的是 `Windows.Graphics.Capture`（WGC）——**Win10 1809 及以上**才有，老 Win10 上 F2 录屏起不来（有 `winrt::hresult_error` 兜着，不会崩，但那件功能没）；
  ② `mfplat` 在 **Windows 10 N / KN** 版里默认没有（要装 Media Feature Pack），同一个形状；
  ③ `SettingsDialog.cpp:341` 那颗 `DWMWA_USE_IMMERSIVE_DARK_MODE` 也是 1809+，早的版本只是标题栏不跟着变暗，返回值没当判据。
- **安装包以前没写 `MinVersion`**：Qt 6.10 本身只支持 Win10+，而 UCRT 在 Win7/8.1 上还得单独装 ——
  那两种系统上的表现是"装是装上了，双击没反应"（Qt6Core 起不来），最难查的一种坏。
  `ltool.iss` 现在补了 `MinVersion=10.0`，ISCC 收了、包照打（22.22 MB）。
  **没证的**：Win7/8.1 上真会弹那句拒装 —— 这台是 Win11（10.0.26300），没法本机证，别当已验。
- 一档旁证（不算 Win10 实测）：CI 那台 runner 是 `windows-2022` = 内核 10.0.20348，和 Win10 同一内核系列，
  离线判据 + 打包在那上面一直绿 ⇒ 这套东西不是"只在 Win11 内核上才起得来"。
  真要判 Win10 桌面行为（DWM 暗色、WGC 录屏、置顶窗缩放），得有一台 Win10 机器 —— 没有的话这条就是空的。

## 「搜索框为什么会莫名其妙出现」：三条路只有一条留过日志（2026-10-21 他问的）
- 先把能把搜索框弹出来的入口数出来（`grep showLauncher`）：**五处调用、三条真实路径** ——
  热键（`SearchWindow::toggleVisible`）、托盘图标左键单击 / 右键菜单那颗、第二个实例转交过来的 `"show"`；
  另外 `--show` 那一支是启动即显示，不算"莫名"。
- **只有热键那条留日志**（`hotkey: 呼出（visible=… 前台=…）`）。托盘单击和"第二个实例转交"这两条
  在现场**一个字都不写** —— 所以"它自己出来了"这件事本来就查不到。现在 `showLauncher(const char* why)`
  必须报上名字，收口那一条打 `show: 呼出（要求者=…）`，五个调用点各自填自己那一句。
- 日常最可能的两个（按这条日志下次就能分辨）：
  ① **托盘图标左键单击**（`Trigger` = 单击，设计如此，手滑就是一次呼出）；
  ② **任何不带 `--` 参数再启动一次 `ltool.exe`** —— 双击 exe、快捷方式、任务栏那颗、开机自启重复拉起：
  第二个实例把 `"show"` 写进本地套接字就自己退出，看起来就是"它自己弹出来"。
  右键那几项**不走这条路**：注册表里实测是 `"D:\software\ltool\ltool.exe" --compress-7z "%1"`，
  带杠 ⇒ 绕开单实例守卫（那是 10-19 为了"自测不许被转交吞掉"加的），不会呼出窗口。
- **排除了一个嫌疑**：启动 1.2 秒那次预热。它 `move(-32000,-32000)` + 透明度 0 映射一遍再收起，
  而那条日志以前排在 `move(saved)` **之后**才读 `x()/y()` —— 打出来的是恢复完的落点，
  本机一直显示 `(0,0 660x100)`，看着像"Qt 把它夹回屏幕里了"。改成量"停在预热位置那一刻"的矩形
  并判它压不压在显示器上之后，实测 `(-32000,-32000 660x100) · 在所有显示器之外` ⇒ **不是它**。
  这条日志自己就是个"判据读的是上一步留下的状态"的形状（[[feedback-verify-before-concluding]] (46)）。
- 顺手把 `--toggle` 那面扶正：`确实造出了「最小化」这个状态` 在 offscreen 上是**必然红**的
  （那个平台没有真 HWND，`SW_MINIMIZE` 是空操作）。拿装好的那份 A/B 过 —— HEAD 上就红，不是这轮带的；
  而 `--toggle` 不在 CI 那 14 面里，所以没人看见（同 #94 那一族）。按仓库规矩改成点名 SKIP，
  现在这面 `失败 0 项 + 2 条 SKIP`。
- 版本：不升号 —— 加的是日志与判据形状，行为没变（`showLauncher` 多个参数是内部签名，不在插件公开面上）；
  跟着还没发的 1.4.0 同一批。

## 「搜索框隐藏的时候会卡一下」：宽限期把"真点走了"当成"激活竞争"吞了（2026-10-21）
- 现场证据先拿到：`log.txt` 里 `show: 失焦落在宽限里，复查后收起` **今天 35 次**，最近两次就在 14:07:23 和 14:07:26，
  夹在他两次按热键中间（14:07:22 呼出 → 14:07:23 才收）。这条日志打的是**复查定时器触发**那一刻，
  所以那一下的等待是 600ms 起步 —— 加上"呼出后 500ms 内的失焦一律忽略"，最长能挂 ~1.1 秒。
- 根因：`event()` 里那条判据只问了一件事 ——「离上次呼出够不够 500ms」。它想防的是
  #40 那一支（我们 `SetForegroundWindow` 抢来的前台被 Windows 撤回去，那一下失焦不是用户点走了，
  当场 hide 就变成"按了没出来"）。但它把**两种完全不同的失焦**当成了一种：
  ① 激活被撤回（前台回到**这次呼出之前**那个窗口）；② 用户点了别的东西（前台去了**别的**窗口）。
- 修法是把判据拆成那颗纯函数 `SearchWindow::blurAction(拿到过前台, 回到原主, 距呼出ms, 菜单开着, 截图进行中)`，
  四支各有名字：`立刻收 / 当竞争稍后复查 / 等截图结束 / 菜单开着不动`。
  新读的两颗输入都是本来就有的状态：`m_hadForeground`（这一次呼出有没有真的拿到过前台，
  `WindowActivate` 置真、每次呼出复位）和 `m_prevFg`（呼出之前那个前台窗口，Ctrl+Enter 粘贴早就在用它）。
  **宽限期内**只有「没拿到过前台」或「前台回到了原主」才继续等，其余一律立刻收。
- 判据：`--toggle` 6 → **13 条 PASS**（+7）。离屏**造不出**真失焦那两支（那个平台抢不到前台，
  所以"在前台时再按=收起"一直是 SKIP），所以这七条问的是谓词本身，不是模拟出来的事件；
  其中一条专门钉"这两支必须给不同答案，否则『拿到过前台』这个输入等于没读"。
- 顺带：`hide:` 那条日志现在每次失焦都写一行（走哪一支 + 拿到过前台没有 + 距呼出多少毫秒），
  下次他觉得"还是慢"，`grep 'hide: 失焦' log.txt` 就能分清是没收到事件、还是走了复查那一支。
- 版本：不升号 —— 这是修一个可感知的迟钝，跟着还没发的 1.4.0 同一批（号跟着发布走）。

## Release 说明改成自己生成（2026-10-21 他给了 vpn-parser v1.8.9 当样板）
- 以前 CI 用 `gh release create --generate-notes`，GitHub 拼出来的是**裸提交标题列表** ——
  而这个仓库的提交信息本来就写着原因和实测数字（一笔动辄几十行），全被丢了。
- 新增 `tools/make_release_notes.py`（一次网都不上，读 git log + installer/ + build 产物）：
  概述 → **一笔提交一节**（图标按标题关键词挑：🐛 修 / 🧩 插件商店 / 🎨 界面 / ⚡ 性能 / ✅ 判据 / 📝 文档 / ✨ 新增）
  → 资产表（文件名 / 大小 / sha256 前 8 位 / 这颗是干什么的，19 颗 dll 逐颗带自己的版本号）
  → `Full Changelog` 的 compare 链接。本机对 v1.0.3 跑出来 **23 个资产 · 3276 字符**（样板那份 3285 字符，同一个量级）。
- 顺带产出 `installer/sha256.txt`：说明里写了"下 sha256.txt 对整批"，那它必须真在资产里 ——
  CI 收尾多了一步：核对 `sha256.txt` 和 `plugins-index.json` 这两个名字**确实在 Release 资产里**，不在就红。
- 三个自己踩的坑都写进注释了：
  ① "上一个标签"必须用 `git describe <标签>^` 问，不是拿 `tag --sort=-creatordate` 取第一个不等于自己的 ——
  后者给 v1.0.3 找出 v1.3.3 当上一个，区间反了 ⇒ 0 笔提交（脚本会因此非 0 退出，不发空说明）；
  ② 资产表要按**这一版的版本号**过滤文件名：本机 `installer\` 里躺着十几个历史 setup，全收就成"46 个资产"，
  而 CI 那棵目录是干净的 ⇒ 这类错只会在本机复跑时露出来；
  ③ 提交正文里的续行（缩进行）要并回上一条 bullet，否则一行被拆成两条、读起来断。
- 版本：不升号 —— 只动 CI 与工具，不进任何产物字节。


## 搜索结果改序：应用第一、网站第二（2026-10-21 他点名）
- 他这句是「搜索框把应用放在第一个，网站放在第二个候选」。先问清了一件事才动手：**工具直达要不要让位** ——
  他选"命中直达时直达仍第一"（回车还是它，那是 10-19 定的）。所以新规矩是一句判据：
  `toolFirst = 命中工具直达 && 网站关键字没赢` → 直达第 0 行；否则**应用节在最前、网站组第二**。
- 原来那一段的发射顺序是「电源直达 → 工具(精确+松散) → 网站命中 → 应用 → 松散工具 → 文件 → 引擎兜底」，
  改完是「电源直达 → [工具] → 应用 → 网站(命中 / 网址直开 / **引擎兜底**) → [松散工具] → 文件」。
  **引擎兜底那条从列表最末尾搬进了网站这一组** —— 它本来就是"网站"性质，甩在最后等于网站排到了最底。
- 顺手补了一颗表头：网站那一组**以前没有表头**，那几行直接跟在上一节的表头底下（搜 `b64` 时就挂在"工具"下面），
  看着像工具的结果。现在叫「── 网站 ──」。
- 结构上动了一处：工具命中**只算不发**（`toolExact`），应用匹配（`ms`）搬到发射点之前算 ——
  因为"谁排第一"要同时知道直达命中没有和应用命中没有，在发射点之后算就来不及了。
  直达命中而 `webKeywordWins` 时那几条精确工具**依旧整节不发**（这是原来就有的行为，没顺手改）。
- 判据：`--plugintest` 242 → **247 条**，数的是**行号**不是"在不在"（两条都在但顺序反了，正是这次要改的东西）。
  反向验过：把 `toolFirst` 硬写成 `false` → 当场红 5 条（含两条老判据）。
  实测行号：搜 `renwu` 应用第 0 / 网站第 3；搜 `b64` 工具第 0（本机没有名字含 b64 的应用 ⇒ 应用节 -1 行，
  所以那条判据钉的是"直达在第 0 行"，不是"直达在应用之前"—— 后者在 0 个应用上会空转）；
  搜 `windows` 工具第 0 / 应用第 2 / 网站第 11（这一条同时钉住"直达拿第一行"和"不许铲平应用"两件事）。
- 没测到的：**常用面板（空查询）没动**，它一直是 应用 → 网站 → 工具 → 文件 → 系统；
  还有"电源直达永远在最前"这一条我保留了（`shutdown` 让位给应用的话，回车可能变成启动一个名字像 shutdown 的应用）。
- 版本：不升号 —— 这是 1.4.0 那一批里的一处排序改动，1.4.0 本身还没发。

## 数据目录搬到软件目录下（2026-10-21 他点名"所有的日志和数据都放软件目录下，不要放在 C 盘"）
- 改的是**一颗函数**：`Utils::dataDir()`（88 处调用点都问它）。三档优先抽成纯函数 `resolveDataDir(软件目录, 环境变量, APPDATA, 写得进去吗)`：
  ① `LTOOL_DATA_DIR`（自测沙箱 / 便携模式）→ ② `<exe>\data` → ③ 只读位置才退回 `%APPDATA%\ltool` 并留一条日志。
- 搬家是一次性的：老 `%APPDATA%\ltool` 还在 + 新目录里没有真配置 → 整棵树复制过去，**每个文件都按 sha256 对过**，
  对不上的删掉半成品；**旧目录一个字都不动**（那是 378 MB 真实数据，删错了没法复）。搬完留一颗 `.migrated-from-appdata` 标记。
- **本机实测**：`983 个文件 / 360 MB`，旧目录 `%USERPROFILE%\AppData\Roaming\ltool` 还是 378 MB 原样在那儿。
  耗时 **约 9 秒**（16:12:49 → 16:12:58），而且它排在**托盘和窗口之前** —— 升级后第一次启动会慢这 9 秒，
  只有一次（第二次起跑就 0 秒，标记在）。这个数如实写在这儿，别当成"免费的一次搬家"。
- 判据：`--pathcfg` 11 → **18 条**（三档优先各一条 + "这一趟 dataDir() 真等于沙箱"一条 + 搬家五条：
  全复制并逐个对哈希 / 子目录真建出来 / **源目录一个字都不动** / 内容真对得上 / **目标里已有同名文件不覆盖**）。
- 自己踩的三个坑（都是判据的坑，不是代码的）：
  ① 守卫一开始写成"新目录非空就别搬" —— `dataDir()` 一被问到就 mkpath，`Utils::log()` 立刻写下 `log.txt`，
  而 deliver 的 `installed` 那一栏会先拿装好的 exe 跑一次 `--plugintest` ⇒ 新目录里已经有一条日志 ⇒
  **relaunch 那次判定"搬过了"，378 MB 一个字节都没动，而且界面和日志都没说**。改成只认真信号（标记 / `config.ini`），
  并且**每个提前返回都要把"为什么没搬"写进 note**（`data: 没搬：…`），不然这种"看起来做了其实没做"抓不住。
  ② 那条"不覆盖"的判据第一版**白绿**：夹具里 `from` 根本没有 `log.txt`，跳过分支一次都没走到。
  ③ 同一批里另一条红是因为我连着 `new` 了两个 `QFile`，第二个没 `open` 就 `readAll`。
- 打包那侧：`ltool.iss` 的 `Excludes` 加了 `data`（否则构建目录里那份 `data\` 会跟着安装包出去 —— 和当年 `captures\` 同一族）。
- 自测沙箱从"挪 `APPDATA`"改成"挪 `LTOOL_DATA_DIR`"：1.4.0 之后数据默认在软件目录下，挪 APPDATA 已经管不着它了。
- 版本：不升号 —— 跟着还没发的 1.4.0 同一批（这是它里面的一处可感知改动，`core_abi_gate` 仍 `GATE_HITS=0`）。


## JSON 格式化那页改成「左边输入、右边看效果」（2026-10-21 他点名）
- 以前 `QSplitter` 里是 **树在左、文本框在右**（`split->addWidget(m_tree)` 先），所以要粘 JSON 得先把光标
  挪到右半边，而左边那块先被解析结果占住。改成文本框在左、树在右，两边各 1:1（实测 900 宽的页：
  输入框 x=10 宽=467 · 结果树 x=481 宽=409）。
- 判据在 `--plugintest` 里加 3 条（量**真实控件的几何**，不是"splitter 第 0 个是谁"那种自己造的断言）：
  输入框 x 小于树 / 输入框 `!isReadOnly()` / 两边各拿到不少于页宽 1/4 的宽度。
- 两个自己踩的坑：① 隐藏着的控件不跑布局，第一次量到"输入框 48px、树 48px"（那是 minimumSizeHint，
  不是布局）—— 判据必须先 `show()` + `resize()` + 跑一轮事件循环再量；② 阈值别写死像素：
  离屏那个虚拟屏比真机窄，写死 `>=300` 会被环境先夹掉，改成按 `页宽/4` 算。
- 版本号：`json` 插件 `1.0.1 → 1.0.2`（插件源码改了，`plugin_version_gate.sh` 那道闸要它自己递增）。
  core 不升号 —— 跟着还没发的 1.4.0 同一批。

## 开机启动分析：能关掉应用的自启 + 系统服务默认不列（2026-10-21 他点名）
- 这一页从「只读」变成**能动一件**：把某一条**应用**启动项开/关。走的就是任务管理器 · 启动 那同一个存储
  （`HKCU|HKLM\...\Explorer\StartupApproved\{Run|StartupFolder}` 下的一个 REG_BINARY），**不动 Run 值本身**，所以随时反得回来。
  服务 / 计划任务 / 加载项一律拒绝，并给出那句人能看懂的原因（`这条不是应用启动项…这一页不动它`）。
- **原来那个开关是读反的**：注释写着「0x03=启用」，代码就照着 `enabled = (blob[0] == 0x03)` 写。
  本机拿真注册表对出来（`reg query` 抄回）：`ltool` 和 `org.yuanli.utools` 是 `020000000000000000000000`（它们确实在自启），
  `Steam` / `NeatDM` / `CiscoMeetingDaemon` / `MicrosoftEdgeAutoLaunch_…` 是 `03` + 一个 FILETIME（那四条才是被关掉的形状）。
  ⇒ **首字节 0x02=开着、0x03=被关掉；压根没有这个值=从没碰过=开着**。判据钉的就是这两组真字节。
- 写回去只改首字节，**长度照原来那份**（UWP 那种 24 字节截成 12 就是写坏；原来没有才给 12），
  尾巴上那个 FILETIME 原样留着（那是 Explorer 记「什么时候关的」，不是我该改的东西）。
- 写完**读回来核对**，对不上就说是没改成（`写了，但读回来还是原来那个状态`）—— 界面不许在自己没做成的事上说「已禁用」。
- 系统服务默认不列（他点名）：一台机器上自动起来的服务几十上百条，全在 `%SystemRoot%` 下面，混进来只会把要看的那几条挤掉。
  但**不许悄悄少一批**：那颗「显示系统服务」开关 + 汇总那句「另有 N 条系统服务没列出」+ 分组标题「（…，系统服务除外）」
  三处都跟着状态走；只点「后台服务」那一类而它们又被藏着时，状态行要接一句为什么这片是空的。
  「没给出绝对路径的那些（svchost 那种裸名字）」也归进这一档 —— 这页对它唯一能说的话就是「系统开机时把它拉起来了」，那是所有服务的共同点，不是信息。
- 「就地两段」确认，**不弹模态框**：第一下把按钮换成「再点一次确认禁用 X」并把话写在状态行，第二下才真写注册表。
  换一行选中 = 收回那句确认（不然是「看着没关掉、按下去关了别的」）。
- 真写的判据走**自己造的临时键** `HKCU\Software\ltool-startuptest-<pid>`，跑完 `RegDeleteTreeW` 删掉、并自己验它没了；
  他机器上真实那六条启动项**一条都不改** —— 页面那半只点第一下（第二下会真改这台机器），那条路径由临时键那条覆盖同一个函数。
- 判据：新建 `--startuptest`（`plugins/startup/startupselftest.cpp`）**55 条 0 红**。里面有 4 条盯「那把尺子自己得能量出东西」：
  `first_click_writes_nothing` 是靠**改前/改后各取一次三个 StartupApproved 键的整棵快照**逐字节对比，
  所以另加一条 `snapshot_tool_detects_a_real_write` —— 在临时键上量一次真写入，证明这快照不是永远为空（不然那条必白绿）。
- 反向验：把 `approvedState` 的 02/03 认反（就是原来那个 bug）→ 当场红 **9 条**，其中 5 条是真写那一段
  （写完读回来核对不上，`setStartupEnabled` 直接拒绝报成功 —— 这条护栏是真起作用的）。
- 自己踩的四个坑（都是判据/工具的坑）：
  ① `QDir::fromNativeSeparators` 是把 `\` 换成 `/`（**不是反过来**），我拿它归一之后又按 `\system32\` 匹配 ⇒ 三条判据红；
     改成两边都过它、只用 `/` 比，顺手把 `%SystemRoot%\…` 没展开和正斜杠写法一起吃进来。
  ② `QEventLoop::isRunning()` 在没 `exec()` 之前是 false ⇒ 那个「等扫描回来」的循环**一次都没等**，
     于是打成「120 秒没出结果」，实际耗时 0 ms —— 超时话写得像实测，差点把我引去查扫描性能。
  ③ 两条判据自己红：分组标题是在**勾上之后**才读的（那时候本来就不该有「除外」）、want 写成 `"0"` 而 got 是 `"0/72行"`。
  ④ Chinese 串里嵌 ASCII 双引号又炸一次编译（这轮的第三次），改用「」了事。
- 实测数字：本机扫一次出 **72 行 / 699 ms**、藏起来 **25** 条系统服务；那颗按钮在 900 宽的页里 x=366 宽=80（后排不下是看得见的）。
  全量离线 **848 条 PASS / 0 FAIL / 2 SKIP**（`--plugintest` 250、`--startuptest` 55、`--pathcfg` 18+1 SKIP、`--storetest` 104、`--cliptest` 77+1 SKIP…）。
- 三道闸都绿：`plugin_version_gate` `BUMPED startup v1.0.2 → v1.1.0`（行为大改自己进位）、`core_abi_gate` `GATE_HITS=0`、`theme_gate` `BAD=0`。
- 版本：startup 插件 **1.0.2 → 1.1.0**；core 不升号 —— 跟着还没发的 1.4.0 同一批（这批已经有 8 件事了，见上面那几节）。

## 冷启动必崩：搬家那句守卫用了两个临时 QStringList（2026-10-21，我自己 16:17 那笔带的）
- 现象：**双击图标 / 开机自启那次启动必崩**（`c0000005`，`Qt6Core.dll + 0x1489c9`），而手上所有离线判据全绿。
  今天日志里 6 条 FATAL 全在这一条上（16:26、16:40×2、16:47、20:21、20:22、20:44 —— 20:44 那次就是 deliver 的 relaunch）。
  取证分两层看过：`Get-WinEvent` 里 P4/P8 全是同一句指令（`Qt6Core.dll` / `00000000001489c9`），
  `%LOCALAPPDATA%\CrashDumps` 里 9 颗 `ltool.exe.<pid>.dmp` —— **进程真的崩了**，不是"界面卡住"那一族。
- 根因（16:17 那笔 `9313288` 带的）：`std::none_of(app.arguments().cbegin() + 1, app.arguments().cend(), …)`。
  `QCoreApplication::arguments()` 在 Qt 6.10 头里写的是 `static QStringList arguments();` —— **按值返回**，
  所以 `.cbegin()` 和 `.cend()` 来自**两个不同的临时 QStringList**，区间的头和尾不在同一份数据上。
  没有旗标时（只有 argv[0]）头指针正好越过末尾，解引用别人家的堆 ⇒ 当场访问违例。
- **为什么离线判据一条都抓不到**：那条分支的准入条件是「命令行里一个 `--` 都没有」，而**每一条自测都带旗标** ——
  带旗标反而不炸（predicate 第一个元素就命中，根本没走这个坏区间）。这是"判据跑在被测分支的对面"的形状，
  绿灯数再高也不覆盖这条路（`--plugintest` 250 条那阵子它照样必崩）。
- 定性用的是同一个二进制、只动一个变量：`--noprofile` → rc=124（活满 12 秒，日志一路到预热）；
  不带旗标 → rc=139 + 日志里只有一行 FATAL。**第一次 A/B 不算数** —— 那次比的是「装的那份+旗标」对「构建目录那份+无旗标」，
  两个变量一起动，结论不成立；同一颗 exe 重跑才定性（自己的旧结论也要复测）。
- 另一条差点把结论带偏的：`std::fprintf(stderr, …)` 那批探针对这份 exe **一个字都不落**（GUI 子系统那份 stderr 我拿不到），
  0 字节不等于"没走到"。换成能落地的证据（进程退出码 + `Utils::log`）才算。
- 定位工具（新，留着）：`build/verify/pe_rva.py <dll> <rva>` —— 这台机器没有 dumpbin/cdb，WER 只给"模块+偏移"，
  这一颗把它问成"落在哪个导出符号之前"。本机答：`0x1489c9` 的下一个导出是
  `QString::QString(const QChar*, qsizetype)`（在 0x1494a0，往前 2775 字节）⇒ 和「QString 那一带」对得上。
  第一版试写 `dump_stack.py`（想从 minidump 里捞候选栈）没验通，**没留着** —— 别把没跑通的工具当依据。
- 修：`const QStringList args = app.arguments();` 一份局部拷贝，首尾都从它上面取；顺手把下面那行重复的
  `app.arguments()` 删掉（同一个数组不必取两次）。
- 新判据（`--pathcfg` 最后一条）：**起一个「一个参数都不给」的子进程，6 秒内不许崩**。
  只能起子进程，因为这个进程自己是带着旗标跑起来的；本机有常驻实例时点名 SKIP
  （那条子进程会把手势递给他的实例、把启动器叫到屏幕上，不打扰）。
  反向验过：把坏形状改回去 → 这条当场红 `crashed-0xc0000005`，26 ms；改回来 → `alive`。
- 装机复验：deliver 九栏全过（`install MATCH ED85AEBF9818`、19 颗逐颗对齐、装的那份 `--plugintest` 250/0），
  relaunch 起来的 pid 16052 活到 21:08 之后，日志正常走到「扫了 349 个应用 / 预热 / linuxdo 改走中转」，**没有新的 FATAL**。
- 还没归因的：**15:57 和 16:06 那两次崩在同一句指令上**，而那台机器上装的是 15:34 那笔（搬家守卫那时候还没进代码）。
  所以别把这条读成"今天所有闪退都是它" —— 那两次我没能解释，先记着。
- **我自己造的一次打扰（要认）**：这条判据第一版是拿 `QLocalSocket` 连一下那根管道来问"有没有常驻实例"，
  而 server 那边**只连不写也算"第二个实例来交接"** ⇒ 21:15:02 他那份启动器被我叫出来了
  （日志里留着 `show: 呼出（要求者=第二个实例转交过来的 show）` + `置前失败`）。
  改了两处：① 判据换成 `WaitNamedPipeW`（只看有没有人在听，不建立连接、不写一个字节，改完复跑他日志里不再多一行）；
  ② 服务端那条 handler 现在**把空消息当没这回事**（`show
` / `note	…` 才动手），
  并加两条判据钉住这两侧：只连不写的连接不许叫出窗口（`show:` 计数差必须为 0）、真写 `show` 的照常叫出来（不许把门焊死）。
  这两条和冷启动那条同一族：本机有常驻实例时一起 SKIP，CI 里跑。
- 装机复验（第二轮，改了 handler 之后）：`install MATCH 40BF9A313CB1`、19 颗逐颗对齐、
  relaunch 起来的 pid 23208 活到 21:22+（**跑过了启动后 15 秒那颗自动更新**），日志正常走到「中转拿到 30 条」，没有新 FATAL。
- 版本：core 不升号（这是**未发布的** 1.4.0 那一批里的一处崩溃修复，不是发布过的号）。

## 剪贴板：点一张卡就复制（2026-10-21 他点名「点击剪贴板卡片就是复制」）
- 原来是**双击**才复制（`itemDoubleClicked`），现在接 `itemClicked`：按一下那张卡就放回剪贴板。
- **双击那根信号同时删掉了**（不留着当"第二条通道"）：单击已经是复制，双击的第一下就完成这件事，
  再挂一条只是让同一次操作写两遍剪贴板、多两行 `clipboard: restored` 日志。
- 那张页下面那行说明跟着改成「点一张卡 = 放回剪贴板 · 选中按 Delete = 从记录里去掉那一条」，
  并加一条判据钉住它（写着"双击"而按一下就成了 = 骗用户多按一下）。
- 判据这边换了量法：原来是**自己调 `cs.restore(...)`** —— 那量的只是 core 那颗函数，
  页面把信号接回双击照样绿。现在送**一对真鼠标按下 + 抬到那张卡的矩形上**（`visualItemRect` 取中心，
  发给列表的 viewport），走的才是它自己那根 `itemClicked`。
- 两个必须有的护栏，都是这次加的：
  ① 点之前先把剪贴板清成一个别的值（`ZZZclip 复制前的占位`）—— 不清的话，上一段判据留下的同一个字符串
    会让这条**白绿**（反向验时正是靠这条占位，才看见红出来的是"占位还在"）。
  ② 一条"那块矩形量得到"的前置判据（`visualItemRect` 空 = 根本没点到卡上）——
    没有它，第一条单击判据可能在"事件谁都没收到"的情况下绿。
- 还加了一条"连点两张不同的卡"：第一下之后整墙会 `refresh()` 重建，重建之后信号接不回来就只复制得动第一次。
  这条挑的是**文本**卡（第 3 行「普通一条 ZZZ」）—— 第 0 行是图卡，走 `setImage`，`text()` 比不了。
- 一个夹具型副作用是复测时发现的：那段为了点第二张卡临时把过滤清空了，而**下一条 Delete 判据钉的是
  "过滤状态下删掉选中的那一条"** —— 我把过滤设回 `alpha` 之后补了一条「又只剩一张卡」，
  让"夹具被上一段改过"这件事下次会当场红，而不是悄悄把那条判据变成删掉全部四张的第一张。
- 反向验过：把 `itemClicked` 改回 `itemDoubleClicked` 重编 → 单击那两条当场红
  （`got=[ZZZclip 复制前的占位]`，就是"什么都没发生"的形状），其余全绿 —— 红的是该红的那两条。
- 真剪贴板那一档顺手复测了一遍"自我那次写不把历史顺序搅动"（2026-10-16 那次闪退的形状）：仍然绿。
- 数字：`--cliptest` 离屏 **82 条 PASS + 1 条点名跳过**（offscreen 的剪贴板是内存桩，那 4 条测不到），
  `QT_QPA_PLATFORM=windows` 那一档 **87 条 PASS / 0 红**。
- **CI 那一圈原来没有 `--pathcfg`**：拿 run 39607595058 的日志复核才发现（`--startuptest` 在里面，`--pathcfg` 整段找不到）。
  后果是这次那三条只在"没有常驻实例"的环境里跑得起来的判据（冷启动子进程 + 两条交接）**本机全部 SKIP、CI 又根本不跑**
  = 一次都没执行过就进了仓库。已把 `--pathcfg` 加进那条循环（那一步现在 16 面旗标）。
  顺带从那份日志读到的：`--startuptest` 在 runner 上 54 过 / 0 红 / 1 跳，跳的是 `hklm_refusal_reason`
  —— runner 是**提权**跑的，HKLM 真写成功了，判据按设计把那个键删掉并点名"正常权限下才有得测"；
  扫描那边是 51 行 / 577 ms（本机是 72 行 / 699 ms）。
- 交付：`install MATCH 40BF9A313CB1`、19 颗里只换 `clipboard` 这一颗、装的那份 `--cliptest` 0 红（那条单击判据在装好的那份上也是绿的）。
- 版本：`clipboard` 插件 **1.1.2 → 1.1.3**（`plugin_version_gate` `BUMPED clipboard v1.1.1 → v1.1.3` —— 基线 v1.3.3 上还是 1.1.1，
  中间那次日间配色改的 1.1.2 也还没发出去）；core 不升号 —— 还是跟着未发布的 1.4.0 那一批。

## 「网站关键字直达只有这么点数据了」：搬家那条"同名不覆盖"被程序自己写的默认值满足了（2026-10-22 他报的）

- 现象与量出来的因：界面里只剩默认那 2 条。`D:\software\ltool\data\webs.json` = **345 B / 2 条**，
  `%APPDATA%\ltool\webs.json` = **4286 B / 24 条**。老目录那份才是他的。
- 根因不是"没搬"，是**搬的时候那一条"目标已有同名就不覆盖"被程序自己刚写下的默认表满足了**：
  deliver 的 `installed` 那一栏会先拿装好的 exe 跑 `--plugintest`，那一趟在**还没搬**的数据目录里
  写下了默认 `webs.json`（2 条）→ 下一次正常启动真搬家时，目标里已经有同名文件 → 跳过 →
  你那 24 条永远留在老目录。`.migrated-from-appdata` 那颗标记照样写了（它只看"复制到的个数 > 0"），
  所以第二次启动也不会再搬 —— 修不好就是永久性的。
- 先救数据再动代码：手工把老目录那份 24 条放回新目录（默认那 2 条另存到
  `%TEMP%\webs.json.default-20261021-234244`），重启后界面 24 条。
- 代码改法：搬家**复制之前**先让路 —— `Utils::clearStockDefaultsForMigration(dir)` 删掉目标里那些
  「内容逐字节等于内置默认」的文件（只认这一种；判"什么算默认"只有一份答案：拿写出默认值的
  `WebShortcuts::stockWebsJson()` 再算一遍字节，不第二处抄）。
  `note` 里跟着报「%1 份程序自带的默认表给用户数据让了路」。
- 判据：`--pathcfg` 加 8 条搬家判据 —— 3 条问 helper（默认表会让路 / 是真删掉不是留个空文件 /
  内容不是默认表的一份一个字节都不许动），5 条拿两个临时目录跑**整条链**
  （默认表没挡住真数据 / 搬成的个数 / note 说了让路几份 / 源目录一个字不动 / 跑第二遍不再搬）。
- **第一版那 8 条之外我还写过一条"按源码顺序闸住调用处"的判据，写的是 `SRC_ROOT "/src/Utils.cpp"`，
  而这个宏整个仓库根本不存在** —— 没编译过就先写了，属于"脑内减掉"那一类。
  换成真的那条：`Utils::migrateBetween(legacy, now, note)`（`migrateLegacyDataDir` 只负责算出这两个目录
  再交给它），判据驱动的就是这一颗，量的是被测代码真造的对象。
- 反向验过（把这一步删掉重编）：整条链那 3 条当场红，报的是
  `got=[345 字节 vs 89 字节]`、`got=[1] want=[2]`、note 里没有"让了路"那半句 ——
  而**问 helper 那 3 条全绿**。这正是这次事故的形状：helper 都在、也测得绿，只是那条链没叫它。
- 复测他这份真数据时又挖出**同一家的第二半**（不修就等于把他历史里几十条图条目判了死刑）：
  `clipboard.json` 里记的是**绝对路径**，搬家之后老条目仍然指向 `%APPDATA%\ltool\clipimg`。
  本机实测：新目录 200 条历史里 39 条是图，其中 **37 条**的 `path` 前缀还是老位置；
  而 load() 那句「图不在了就不留空条目」会把它们**连条目一起**丢掉，随后一次 save 就再也找不回。
  更糟的是老目录里那 78 个 clipimg 副本已经被这一版的孤儿清理删过两轮
  （日志里两条 `clipboard: swept 78 orphan image file(s), items=200`，16:12:59 和 23:43:03 ——
  后一轮删的就是我 23:42:44 手工补回去的那一份，19 秒之后）：历史记的是老路径，
  新目录里那些副本在 keep 表里算"没人引用"。
- 改法：`ClipboardStore::resolveImagePath(recorded)` —— 每一格先按**文件名**在当前 `clipimg\` 认一次，
  认得到就用它（那颗文件名本来就是图内容的 sha1，同名即同内容），认不到才原样返回。
  这样下一次 `save()` 顺手把路径带回新目录，孤儿清理也不会再把副本当没用的删。
  `ClipboardStore::load()` 为了能重跑一遍改成 public（判据要量的就是"读"那一步）。
- 判据：`--cliptest` 加 6 条 —— 4 条问纯函数（两边都有同名 → 认现在这个 / 认回来的那个真能打开 /
  新目录没有同名而老路径还打得开 → 一个字都不改 / 哪儿都没有 → 老实原样返回），
  2 条**真改写 `clipboard.json` 再 `load()` 一遍**（条目不许被连条目一起丢掉 / 那一格指向现在这个 clipimg）。
  反向验过：把 load() 里那一格换回原样抄路径 → 红的正是那 2 条（`got=[0] want=[1]`、`got=[(历史是空的)]`），
  4 条纯函数全绿。
- 他这份数据的修复：`build/verify/fix_clipimg.py` 把老目录那 78 个文件按 sha256 对过复制进新目录
  （`copied=78 / 同名且同内容=0 / 同名但不同字节=0`，一个都没覆盖），
  装上新版重启后 `D:\software\ltool\data\clipboard.json` 里 **39 条图条目的 path 全变成了
  `D:/software/ltool/data/clipimg`**（`build/verify/clip_check.py` 量的），
  老目录那 78 个文件也再没被孤儿清理收走。现在他删 `%APPDATA%\ltool` 不会丢历史。
- **顺手发现自己那条交付链也没说实话**：deliver 的 `relaunch` 那一栏一直读
  `%APPDATA%\ltool\log.txt` —— 1.4.0 之后日志写在 `<exe>\data\log.txt`，那份老文件早就不更新了，
  所以它打出来的是"上一版留下的那条 hide（15:56）"，而真日志那时已经是 00:07 那几行。
  改成先读现在这个、读不到才退回老位置，并把**读的是哪一份**打进那一栏（拿错文件当证据比没证据更坏）。
- 全套判据（本机、离屏）：`--smoke` 有输出且退出码 0、`--plugintest 250`、`--rowmenu 9`、`--username 36`、
  `--about 27`、`--cfgcheck 28`、`--appdirs 12`、`--pinyin 68`、`--seamtest 38`、`--diskbranch 4`、
  `--storeexit 14`、`--storetest 104`、`--cliptest 88 + 1 跳`、`--startuptest 55`、`--pathcfg 26 + 1 跳`，
  全部 0 红。四道闸：theme 34 处/漏 0、proxy 8 处/漏 0、plugin_version `NEEDS_BUMP=0`、
  core_abi `GATE_HITS=0 / META_BAD=0`（新加的都是自由函数和静态纯函数，不动布局 ⇒ 接口号 4 不用升）。
- 交付：`build 884 KB errors=0`、`abi 缺 0`、`stale 0`、`apigate 号配得上`、
  `install MATCH 8EBFF90E8EBD`、`plugins 19 颗全对齐（缺 0 / 陈 0）`、`installed --plugintest 250 过 / 0 红`、
  `relaunch pid=18016`、TOTAL 35.1 秒。
- 版本：**core 不升号** —— 这是未发布的 1.4.0 那一批里的一处数据丢失修复，1.4.0 还没打 tag、
  没发出去，按"未发布的号可以吸收同批功能"跟着这批走；`clipboard` 插件号不动 —— 这轮只改了它的
  判据文件（`clipselftest.cpp`），插件自身源码没动，`plugin_version_gate` 也报 `NEEDS_BUMP=0`。

## 「新版本不能让用户少数据」：把播默认值那一手连根拔掉 + 一次性迁移脚本（2026-10-22 他点名）

- 上一条只是**接住了后果**，这一条找的是"谁会往还没搬的目录里写默认值"。答案在代码里，不是推测：
  沙箱名单 [src/main.cpp:609](../src/main.cpp) 里**没有 `--plugintest`**，而 [src/main.cpp:672](../src/main.cpp)
  的 `WebShortcuts::ensureDefaultWebs()` 对**所有**路径都要跑 —— deliver 的 `installed` 那一栏跑的正是
  `D:\software\ltool\ltool.exe --plugintest` ⇒ 那趟把 2 条的默认 `webs.json` 播进还没搬的数据目录。
- 两道守卫：① `--plugintest` 加进沙箱名单（先实测：`LTOOL_DATA_DIR` 指到临时目录时它仍然 250 过 / 0 红，
  插件那 19 颗是从 `exe 目录\ltool-plugins` 装的，和数据目录不是一条线）；
  ② `ensureDefaultWebs()` / `DevEnv::ensureDefaultPlugins()` 先问 `Utils::legacyMigrationPending()`，
  **还没接住真数据的目录一个字节默认值都不写**。判断只有一份：拆成纯函数 `migrationPendingFor(legacy, now)`
  （`--pathcfg` 用两个临时目录直接问它，5 条）。
- 一次性脚本 `build/verify/migrate_ltool_data.ps1`：只按两条规矩 —— 老目录有而新目录**缺**的按 sha256 补进去
  （对不上就回滚那一份），两边都有但字节不同的**一个字都不碰**、只列出来。默认是 dry run，`-Apply` 才写盘；
  落点从 Inno 卸载键的 `InstallLocation` 推（推不到才用 `D:\software\ltool\data`）。
  写的是纯 ASCII：`.ps1` 里放中文又没 BOM 在这仓库里炸过五种样子。
  本机实跑（dry）：扫 992 个、缺 4 个（0.2 MB，全是 `clipimg\`）、两边都有 988（字节相同 981 / 不同 7）。
  第一版踩了两个 PS 的坑：`Split-Path -LiteralPath … -Parent` 在这台机的版本上是 ambiguous parameter set（换
  `[IO.Path]::GetDirectoryName`）、`Write-Output 'a' + $n + 'b'` 会被当三个参数各打一行（整个式子先括起来）。
- `-Apply` 之后本机补齐了那 4 个（`copied=4 / 失败 0`）；剩下的 7 条"字节不同"看一眼就知道都不该动：
  新目录那一份全是今天的 mtime（程序现在写的就是这里），老目录全是昨天及更早的冻结快照。
- **有一条我本来要加、最后决定不加**：把"目标里那份等于内置默认就拿老目录那份换掉"也放进*搬过之后*那趟。
  不加的依据就是上面那 7 条里的 `plugins\java.json`（老 538 / 新 379 字节）：逐字节 diff 之后两边差的
  **只有程序自带默认文字本身的改版措辞**（key 顺序 + 一条 note 的写法），没有一行是你写的。
  "等于**这一版**的默认"推不出"老目录那份是用户数据"——它可能是**上一版**的默认，换过去就是把新版文字退回旧的。
  加上 1.4.0 还没打 tag、没发出去 ⇒ 没有任何用户装过带这个 bug 的版本 ⇒ 没有需要救的现场，要救的现场已经在
  **搬家之前**那一步堵住了。所以 `backfillFromLegacy` 只做一件有把握的事：把**现在这份 `clipboard.json`
  引用着、而本地缺**的剪贴板图从老目录补回来（按文件名认，不按记着的那串路径 —— 记的那串可能还打得开，
  可你一删旧目录就断了）；没引用的一张都不补，不然你删掉的往期图每次启动都复活。
- 判据：`--pathcfg` 26 → **37 过 + 1 跳**（新增 5 条问 `migrationPendingFor` + 1 条钉"沙箱设着
  `LTOOL_DATA_DIR` 时 pending 必须是 false"（少这一条，任何离屏夹具都会被自己的守卫挡住、默认表播不进去）
  + 2 条盯补图 + 2 条盯它不许复活东西/幂等）。反向验过：摘掉 `migrateBetween` 里那颗调用，
  红的正是 4 条（`got=[0] want=[2]`、`got=[n,n]`、note 那半句、幂等那条变 `changed`），前面 33 条全绿。
- **如实点名一处量不到**：`ensureDefaultWebs()` / `ensureDefaultPlugins()` 里那句守卫**本身**没有判据 ——
  每一条离屏判据都设着 `LTOOL_DATA_DIR`，而那正是 pending 必须为 false 的档位，所以"pending=true 时真的不写"
  在这套沙箱里永远量不到；要量就得跑一个不带 `LTOOL_DATA_DIR` 而把 `APPDATA` 指到临时目录的进程，
  那种进程会往 `build\Release\data` 里落文件、被安装包收走。兜住它的是另外三层：
  判断本身 5 条 + `--plugintest` 进沙箱 + 搬家前的"让路"那 3 条。
- 全套（本机、离屏）0 红：`--plugintest 250`、`--cfgcheck 28`、`--cliptest 88+1 跳`、`--startuptest 55`、
  `--pathcfg 37+1 跳`、其余同上一轮；四道闸绿（`core_abi_gate` 动了布局的头 0 个 ⇒ 新加的仍是自由/静态纯函数、
  接口号 4 不升）。
- 交付：`build 903 KB errors=0`、`install MATCH 9F2B7ACB7854`、19 颗插件全对齐、
  装好的那份 `--plugintest` 250 过 / 0 红（**这一栏现在跑在沙箱里了**）、`relaunch pid=25060`，
  而且 `relaunch` 那一栏读的是 `D:\software\ltool\data\log.txt`（上一条修的那处，这次是它第一次报对文件）。
- 版本：**core 不升号** —— 还是未发布的 1.4.0 那一批（这批的定性是"把搬家这件事做对"，不是新功能）；
  插件号不动（这轮没碰任何 `plugins/*` 源码），`plugin_version_gate` `NEEDS_BUMP=0`。

- 这轮又跑了一遍那个脚本（dry），把"为什么还能缺 4 个"钉死：那 4 个（两张图 + 它们的缩略图）
  **现在这份历史一条都没引用** —— 老目录里那份历史早就被更新的历史顶掉了，程序按天清理把它们当孤儿收走
  （日志两条 `swept 4 orphan`，时间正好在 `-Apply` 之后）。所以脚本现在自己把这个归类打出来
  （`of those, referenced by the history now: 0 ; not referenced: 4`）—— 不说清的话，下一轮看
  "还缺 4 个"就像"没搬干净"。程序那侧的 `backfillFromLegacy` 从一开始就只补被引用的，不会来回拉锯。

### 这一次 runner 上每条旗标量到几个（和这台机器不一样，别说成一个数）

`python build/verify/ci_log_pathcfg.py <日志 zip>` 数的（run 39636325299 = `bbd6265`，终态 success）：

    --plugintest 250 · --storetest 104 · --cliptest 88(+1 跳) · --pathcfg 40 · --startuptest 54(+1 跳)
    --cfgcheck 28 · --username 36 · --seamtest 38 · --pinyin 60 · --appdirs 12 · --about 27
    --rowmenu 9 · --mfttest 10(+1 跳) · --diskbranch 4 · --storeexit 14 · --smoke 0 条判据（它是诊断，CI 那道闸判的是"有输出且退出码 0"）

- `--pathcfg` 在 runner 上是 **40**，这台机器是 37 过 + 1 跳 —— 差的正是那三条（冷启动子进程 + 两条交接）：
  本机有常驻实例，那种子进程会把启动器叫到屏幕上所以点名跳过；runner 上它们真跑了，
  日志里那行 `INFO 冷启动子进程 verdict=alive ms=6005` 就是它执行过的凭据。
- `--pinyin` 在这台机器 68、runner 60：少的 8 条**按环境点名跳过**（`INFO 跳过 gg 那三条：这台机器的 webs.json 里没配 gg`
  · `跳过 An 那两条：索引里没有 Antigravity` · `没有 QQ` · `没有中文名应用` · `ChatGPT 那条` 等），
  而"跳过"这一形状在 pinyin 那一节是 `INFO` 开头不是 `SKIP` 开头 —— 只 grep `^SKIP` 会把"少了几条"读成"莫名少了"。
  数判据的工具自己先把这点补上了（`ok` 开头的 --cfgcheck、`INFO 跳过` 都归类打出来）。

## 推 v1.4.0 之前先把发版说明脱敏（2026-10-22，他拍的"先加脱敏再推"）

- 为什么这一步值得停下来问一句：CI 的发布步骤跑 `tools/make_release_notes.py`，它把 v1.3.3..v1.4.0
  **每笔提交的正文原样**拼进公开 Release 页，而真历史里那句
  `- 本机实测：983 个文件 / 360 MB，旧目录 \`C:\Users\<用户名>\AppData\Roaming\ltool\` 还是 378 MB 原样在那儿`
  正好在这个区间里（`9313288`）。同一串字符在 `git log` 里是一行历史，印在发版封面上是另一回事。
  数过：这一区间标题+正文里带用户名的就 **1 处**。
- 改法：`scrub()` 只吃"家目录 + 用户名"那一段（反斜杠和正斜杠两种写法都认，后者是 Qt 归一化之后的形状），
  换成 `%USERPROFILE%`；`D:\software\ltool`、`%LOCALAPPDATA%\Programs\ltool` 这类**文档要的路径一个字都不动**
  （判据里就有这两条"不许动"）。接在 `commits()` 的标题和 `body_lines()` 的每一项上。
- 两道闸，都放在**每推都跑**那一栏而不是只在发版那一次（只在推 tag 才量的话，漏出去的那一版已经发出去了）：
  `python tools/make_release_notes.py --check`（6 条），
  `python tools/notes_history_leak_check.py [区间 | --file release-notes.md]`（真区间 17 笔 / 171 行 → 命中 0；
  发布步骤里再拿**将要发出去的那份文件**量一遍）。反向验过：把 `scrub` 改成原样返回 → `--check` 当场红 4 条；
  往一个临时文件里塞一句 `C:\Users\someone\AppData` → leak check 退出码 1 并点名那一行。
- 判据脚本里**不写死任何一个用户名**（只按形状判）：那个文件本身要进公开仓库。
- 校验 yml 用 `build/verify/yaml_check.js`（`$TEMP\jsy` 那份 js-yaml；这台机的 msys python 没有 pip）。
  这份 yml 现在 1 个 job / 24 步，新那一步排在配色闸后面。
闸自己踩红过一次：判据的图案要和脱敏的图案是同一颗

刚加的那道「发版说明脱敏闸」在 main 上就红了（run 39641227629 第 6 步）：
`NOTES_CHECK` 6 条全过，而 leak check 报 `带家目录的行=1，残留 C:\Users\<用户名>\AppData\Roaming\ltool`。

红的原因不是漏了真用户名，是我自己这两颗图案不一致：
- `scrub()` 认的是"家目录 + 一段 `[A-Za-z0-9._-]+` 的用户名"；
- leak check 里另写了一条更宽的（`C:\Users\` 后面接任意非空白）。
于是 e6128e8 那笔提交正文里**为了说明这件事而写的占位符** `C:\Users\<用户名>\...`
脱敏不动它（本来就不该动），却被闸当命中拦下 —— 一条合法的说明文字把每推都跑的闸踩红。

改法：leak check 直接 import `make_release_notes.USER_HOME_RE`，判据和脱敏共用一颗图案
（这仓库的规矩：同一个判断不许有第二份答案）。闸判的应该是"有一个真用户名活着出来了"，
不是"出现了 C:\Users 这几个字"。

两边都验过：
- 真区间 `v1.3.3..HEAD` 180 行 → 命中 0（占位符那条不再红）；
- 拿一份临时文件塞三行（合法路径 / 占位符 / 真用户名 `zzrealname`）→ 只点名为 1 条、退出码 1。

## v1.4.0 发出去了（2026-10-22 01:2x，run 39641826096 = success）

- Release id 419844618、**23 个资产**：安装包 22,572,344 B + 19 颗 dll + `ltool-sdk-1.4.0.zip` +
  `plugins-index.json` + `sha256.txt`。tag `v1.4.0` 是 annotated，指向 `bf53b66`（脱敏那把图案统一之后那一笔）。
- **对着公开那一面复验**（`python tools/postrelease_check.py`，退出码 0）：
  公开说明 21,792 字里带家目录的行 **0**；商店那条链 `releases/download/v1.4.0/plugins-index.json`
  **HTTP 200、19 条**（装 1.4.0 之后那条一直 404 的链这次随发版活了）；
  `sha256.txt` 里安装包那一行 == 资产 API 的 digest（`1895560fd733…`）。
- tag 走过一次弯路：第一次推的 `v1.4.0` 指在 `e6128e8`（带的是那颗写宽的判据），CI 在我自己那道闸上红了、
  Release 没建成（当时查是 404）。按你拍的"把 tag 移到修复那一笔"：删远端那个同名 tag → 在 `bf53b66` 重打 → 推。
  删的时候没有 Release 挂在它下面，所以没有"删 tag 连带删掉发布物"那种副作用。
- `--autoupdatenow` **这一轮没有再跑**：跑了就会把装着的 19 颗换成商店那批（MSVC 每次字节不同），
  而"要不要以商店那份为准"（A/B/C）你还没拍。README 那一行跟着改成了这个状态。
- 网络工具那条坑（值得记）：这台机的 msys python 走 `urllib` 拿 GitHub 是
  `CERTIFICATE_VERIFY_FAILED: unable to get local issuer certificate`（没有可信 CA 那份），
  而 curl 自带 —— 所以 `postrelease_check.py` 里对外一律 `subprocess curl` + 重试，
  一次抖动不算"那条链没活"。
- 插件仓库同步（另一半）：`tools/sync_plugins.py` 报 **新放 36 / 改动 36 / 已一致 34**，
  6 颗整个缺的（clipboard、downloads、grid、linuxdo、software、tasks）连各自那份 `CMakeLists.txt` 一起补上，
  类名是从主仓库 `CMakeLists.txt` 里现问的（`ltool_add_tool(<id> <Class> …)`，不第二处抄）；
  `sdk/` 用主仓库重新铺：**26 个头 + 4 个 onnxruntime 头 + ltoolcore.lib（API 4，1.4.0）**。
  推之前要先在本地把 19 颗独立编一遍 —— 那边没有 PCH，历史上正是这条路才暴露"少 include"。

## 插件仓库同步完了（`461a394` + `b4ba2e1`，那边 CI 终态 success）

- 本地那份 clone **落后 `origin/main` 三笔**，而 `191fb80` 早就把 19 颗源码同步过（SDK 到 API 4 / ltool 1.3.3）。
  我第一遍在旧 base 上做，报的是「新放 36 / 改动 36」；真实增量只有「新放 1 / 改动 10」。
  做法：旧 base 那版先留在一条本地分支（内容全可复现，所以留不留都不影响）→ `reset --hard origin/main` → 重跑 `tools/sync_plugins.py --apply`。
  教训：动另一台仓库之前先 `git fetch` + 看 `status -sb` 的 behind，别拿本地旧状态当现状。
- 增量三颗：`clipboard` 1.1.2→1.1.3、`startup` 1.0.2→1.1.0、`json` 1.0.1→1.0.2（源码 + `plugin.json` 一起）。
- `sdk/` 重铺：**26 个头** + 4 个 onnxruntime 头 + `ltoolcore.lib`（863,986 B，API 4 / ltool 1.4.0）。
  比 1.3.3 那份多的那颗是 **`Theme.h`** —— 我先顺手写成 `ClipboardStore.h`，拿 `git show 191fb80:sdk/INFO.txt`
  和现在这份一比才发现不对（只有 `plugins/clipboard/` 那两棵 include `Theme.h`，上一版任何插件都没 include）。
  **别按"应该是哪颗"往文档里写**，一比就知道。
- 独立编这一遍才是凭据（那边没有主仓库那层 PCH）：新建 `build-sync` 这个 binaryDir（没碰 `build-ci` 那份缓存），
  `cmake --build build-sync --config Release` → **19 颗 dll、0 error**。`dist/` 的 dll 和 `index.json` 一起重算，
  按他们 CI 的量法自洽：19 条、api 全 4、sha256 与字节数逐个对上。
- 那边 CI 的终态（run `39645316369` = success，逐步点名）：验 SDK 快照 ✓、`dist/ 自洽：19 个插件的 sha256 与字节数逐个对上` ✓、
  `一个 dll 都不许缺` ✓（日志里列出的产物正好 19 行）、验 Qt 就是宿主那一份版本 ✓。
- 顺手把他们两处**写死的数字**改了（`b4ba2e1`）：CI 步骤名「配置并编出 12 个 dll」和 README 里「全部 12 个」——
  这一栏现在编 19 颗，数字写死第二次同步就过期（这次就是）。改成按 `plugins/*/` 现枚举的说法。
- 网络那条老毛病复现：`git push` 到插件仓库连着两次 `Recv failure / Failed to connect ... port 443`，
  第三次（直连重试）就过了 ⇒ 推完一定要 `git ls-remote` 对上 sha 再算成功（这次 `b4ba2e1` 已对上）。
- 版本：插件仓库只是主仓库的镜像，**不升任何号**（三颗的版本号就是主仓库发出去的那三个号）；
  core 侧这轮只有文档和工具（`tools/sync_plugins.py`、`tools/postrelease_check.py`）⇒ 不升号。

## 「b64 会闪退」：插件页的命不在宿主手里，可两边都存了裸指针（2026-10-22 12:00 他报的）

- 先分两层取证（进程崩没崩 vs 只是界面没反应）：事件日志 `Id=1001` 有 **2026-10-22 12:00:17 ltool.exe
  崩在 `Qt6Core.dll` 6.10.2.0 + RVA `0x3fc2b8`，异常 `c0000005`、访问地址 `0x8`**；
  `%LOCALAPPDATA%\CrashDumps\ltool.exe.17412.dmp` 同一分钟。**是真崩**，不是"点了没反应"。
- 签名和昨天那一批不是一个：10-21 那 10 次全是 `0x1489c9`（= 我已经修掉的 `app.arguments()` 那个），
  今天这个是**新的一个偏移**。用 `build/verify/crashsig.ps1 -Days 7` 一列就分得清（这台机的 PowerShell
  从 bash 里传 `$_` 会被吃掉，所以走 .ps1 + 纯 ASCII）。
- 现场那一趟在日志里是：`12:00:01 clipboard: swept 2 orphan` → `12:00:03 plugin: 建页 base64` →
  `store: 展开 base64（stack 第 1 格 / 共 2 格）` → `12:00:04 store: 重排卡片墙（卡片 38 张 / stack 2 格）` →
  `12:00:09 呼出` → `12:00:12 hide` → 崩。
- **代码上确实站得住的一处**（这轮修了）：插件页是 `createPage(parent)` 时交给调用方那棵树拥有的，
  而 `ToolsPage::rebuildGrid()` 会把 stack 摘掉的那一格 `deleteLater()`；宿主这边 `PluginHost::g_pages`
  和各壳里那份 `m_page` 存的都是**裸指针**，谁都不知道对方已经没了 → 下一次 `page(id)` 或 `feedText()`
  就是 use-after-free，形状正好是"在 Qt 里读一个成员（偏移 8）"。
  改：`g_pages` 换成 `QHash<QString, QPointer<QWidget>>`（缓存里那颗跟着页一起死，`page()` 遇到死的就重造），
  `base64` / `json` 那两份 `m_page` 换成 `QPointer`，`feedText` 在页没了的时候**老实返回 false**（不假装收下）；
  `grid` 那份 `m_page` 从头到尾没人读过 —— 那是个只写不读的字段，连着删掉（不留空开关）。
- 判据：`--plugintest` 加 3 条 —— 拿 base64 那张页**真删一次**，再问宿主要点什么。
  修完是 `dead/new` + `yes`；反向验（把 `page()` 换回"缓存里有就发出去"那份写法）当场红 2 条
  （`got=[dead/same]`、重造那条 `got=[no]`），其余 251 条照绿。总数 250 → **253**。
- 插件号：`base64` 1.0.1→**1.0.2**、`json` 1.0.2→**1.0.3**、`grid` 1.0.2→**1.0.3**（都是行为修复，走补丁位）。
  core：接口没动布局（`core_abi_gate` `GATE_HITS=0`），但 `ltool.iss` 的号 **1.4.0 → 1.4.1** ——
  **1.4.0 已经发出去了，发出去的号不能被后来的产物复用**（这条规矩上次是拿商店那条链当尺子量的：
  1.4.1 的产物会去找 `releases/download/v1.4.1/plugins-index.json`，**在你推 `v1.4.1` 之前它是 404**，
  应用会退回本地缓存那 19 条并照实说 —— 这是设计里的行为，不是回归）。
- 老实说一句：**这处补丁我没能证明就是他这一次崩的那一处**。上面那条链是"代码上真有、且一崩就是这个形状"，
  但偏移 `0x3fc2b8` 在 Qt6Core 内部、附近没有可读的导出（`pe_rva.py` 量到的是 `QBluetoothPermission` 那个
  staticMetaObject 之后 23 KB 的一处**未导出代码**），单凭这一个数定不到调用方。
  所以下次真崩要能自己说清 —— 崩溃处理里补了**栈回溯**：`SetUnhandledExceptionFilter` 现在拿
  `CaptureStackBackTrace`（kernel32 那一版，不用链 ntdll.lib）打最多 12 帧，
  每帧一行 `CRASH FRAME ## 模块名+0xRVA` 进 `log.txt`，行号可以喂回 `pe_rva.py` 去问旁边是谁；
  只用固定缓冲区 + `fromLatin1`，异常处理里再多分配一次，第二次崩会把线索一起吃掉。
  顺手给 `store: 重排卡片墙` 那行加了「摘掉的那格=<类名>」：正常永远该是卡片墙那一类，
  哪天摘到一张插件页，那行会先替这次崩溃说话。

### 这一处后来被**证死**了（同一轮 12:2x，他答"每次都会 · 从搜索框直达进去"）

- 他那两条一给，路径就收窄到 `SearchWindow.cpp:1474` 那一行：`openTool(id, extra)`
  → `SettingsWindow::showTool` → `ToolsPage::showTool` → `PluginHost::page()` + `PluginHost::feed()`。
  "每次都会"排除了"偶发时序"，所以那条路必须能进判据 —— 加了 4 条走**真那条链**（拿 `ToolsPage wall`
  直接调 `showTool("base64", "aGVsbG8=")`，判输入框/结果框/stack 当前格）。
- **反向验 = 决定性的一步**：把三处缓存（`g_pages` 和 base64/json 两份 `m_page`）退回旧的裸指针写法、
  判据一字不动重编 → `--plugintest` **段错误退出（rc=139）**，死在"宿主不会把已释放的页再发一次"
  那条报红之后的下一步（也就是拿那块页去 `m_stack->indexOf/addWidget`）。
  和他事件日志那个 `崩在 Qt6Core + 读地址 0x8` 同一个形状。改回来 → **257 条 0 红**。
  所以这次不是"我修了一处看着像的"，是"同一条路在旧写法上必崩、在新写法上不崩"。
- 判据自己也被修过一次：`findChildren<QPlainTextEdit*>()` 的下标不是界面顺序（第 0 个其实是只读的结果框），
  第一版按"0 是输入"写，当场红两条 —— 红的是判据。现在按**角色**认（只读 = 结果框）。
- 总数：`--plugintest` 250 → 253（页缓存那三条）→ **257**（直达那四条）；离线全套仍然 0 红
  （`--cliptest 88+1 跳`、`--pathcfg 37+1 跳`、`--storetest 104`、`--startuptest 55`、`--pinyin 68`…），
  `plugin_version_gate` `NEEDS_BUMP=0`（这一小步只动了 core 的判据侧）。

### 「打包安装我试试」这一趟里，我自己写坏的两个东西（都当场修了）

- `build/verify/migrate_ltool_data.ps1` 找卸载键时写死 `ltool_is1` —— **真名是 Inno 那串 GUID
  `{B7E6F3A1-...}_is1`**，所以它一直查不到、又**不出声地**退回写死的 `D:\software\ltool\data`，
  结果碰巧对。改成按 `DisplayName` 现搜两棵 hive，并把「target 是从哪儿来的」打进那一行
  （查不到就明说是 HARDCODED FALLBACK）。
- 同一个文件被我加了中文注释 ⇒ **无 BOM 的 .ps1 直接 ParserError**（这仓库第五次踩）。
  全文件回到纯 ASCII。它的头部注释本来就写着"ASCII only on purpose"，是我自己没照着做。
- 还有一条**别再用错证据**：`ltool.exe` 的字节里 grep 不到 `1.4.1` 也 grep 不到 `1.4.0`
  （ANSI 和 UTF-16 两种编码各数一遍都是 0）—— 短字面量在 release 产物里会被拆开，
  所以"装了哪一版"要看**注册表 `DisplayVersion`** 和**安装包文件名**，不是看 exe 里那串字。

## 「测试全部插件保证没有闪退」这一趟：把那三条只钉 base64 的判据按颗铺开（2026-10-22）

- 原来只钉一颗是不够的：`g_pages`、壳里那颗 `m_page`、卡片墙那一次 `deleteLater` —— 这三样对 19 颗插件
  是同一套代码，形状和"是哪一颗"无关。现在 `--plugintest` 里三趟都按颗数着过：
  ① 逐颗「页被单删后再拿」**跑三趟**（第一趟逐颗出声，后两趟只在坏的时候点名 + 每趟一条汇总）；
  ② 逐颗直达 → `rebuildGrid()` 拆墙 → 再逐颗直达（两轮）；
  ③ 另开一扇 `ToolsPage`、逐颗直达、把**整扇窗**删掉（他真实的形状：设置窗一关，点开过的页一起没），
  然后逐颗「先递文本、再要页」。整趟 `--plugintest` 实测 5964 ms（跑三遍删造也不慢）。
- 为什么要跑三遍而不是一遍：这类生命周期问题和**堆怎么复用**有关。第一趟的 INFO 就抓到两颗
  （`grid`、`language`）"新造的那张正好落在刚释放的地址上" —— 只跑一遍 + 按地址判，这两颗会被判成
  "缓存把旧的又发了一次"，那是假红；而假红会让人去改本来对的那半句代码。
- 第③趟里那个顺序是有名字的：**先 `feed()` 再 `page()`**。反过来写的话 `page()` 会把壳里那颗 `m_page`
  顶成新造的那张，"往已经没掉的页上写字"这一支就永远量不到了 —— 量不到不等于没有。
- 新判据自己红过一次，红的是判据：`again != first` 拿**地址**当"是不是重造的"证据 ——
  `delete` 之后新造那张经常落在同一块回收内存上（grid 就被判成过 `same`）。地址相等说明不了"发的是旧的"。
  改成：`QPointer` 跟着死（缓存不裸）+ 当场 `feed` 不许接 + 拿回来的那张用得动（给结果也挂一颗 `QPointer`，
  挂在已释放的对象上它自己说自己是 null）。地址被回收时打一行 INFO 说明"不是同一对象"。
- 反向验（这一轮又跑了一次，因为判据换了）：只把 `src/PluginHost.cpp:23` 那颗 `g_pages` 退回
  `QHash<QString, QWidget*>`、判据一字不动重编 ⇒ **第一趟就红 4 条，然后进程死在半路**
  （上一版单趟时是 rc=139 段错误、这次退出码 127 —— 死在哪一步取决于刚释放的那块被谁先用，
  所以这里能引的不是那个数字，是"旧写法跑不完这一趟"）。改回来 → **300 条 0 红、rc=0、5964 ms**。
- 总数：`--plugintest` 257 → **300**。加的是 19 条单删（第一趟逐颗）+ 3 条"第 N 趟颗颗干净"汇总
  + 24 条（逐颗直达 1、拆墙后直达 2、第二扇直达 1、整扇窗关掉后再拿 19 + 汇总 1）；
  减的是原来只钉 base64 的那 3 条（被上面那些包住）。257 − 3 + 22 + 24 = 300。全套离线仍然 0 红：
  `--storetest 104`、`--cliptest 88`、`--startuptest 55`、`--pathcfg 37`、`--cfgcheck` 失败 0 项、`--pinyin 68`。
- **没测的那几样，说清楚免得当成测过了**：右键动作 / F1 / F2 那一路只验"找得到人"，一条都不真执行
  （真执行会截他的屏、会开录、会删文件）；商店的下载·卸载走 `--storetest` 的纯函数那一层，
  真删 dll 那条没跑；启动时那颗 `Activate` 只验牌和符号对不对，没在这儿真起后台轮询。

## 方案 A：把探针里的真账号换成运行时取法，并给仓库本体立一道闸（2026-10-22）

- 查出来的形状：`build/verify` 里 **16 个一次性探针、17 处**带着真用户名（还有桌面上一个私人夹具目录名、
  两份会话转录的绝对路径）。根因不是"忘了改"，是 `.gitignore` 那条 `build/verify/*.sh` **只挡了一半** ——
  挡得住 .sh，挡不住 .ps1/.py/.js，而且对**已经跟踪**的文件忽略规则根本不生效。
- 换法按语言各走各的运行时取法，不是抹成字面量 `%USERPROFILE%`（那会把还能用的探针弄坏）：
  python `os.path.expanduser(r"~\...")`、PowerShell `"$env:APPDATA\..."` / `[Environment]::GetFolderPath('Desktop')`
  （后者连重定向过的桌面也认）、node `process.env.USERPROFILE`、shell `"$HOME/..."`。
- 补丁只走**逐字匹配 + 数量断言**，不用正则：反斜杠在"源字符串 / 正则 / 替换模板"三层里各要逃一次，
  逃错一层的下场是"匹配不到、于是静默改了个寂寞还报绿"。任何一处数量对不上 → 整批一个字节都不写盘。
- 落地后逐张过语法闸：`python -m py_compile`（4 颗）、`node --check`（4 颗）、`bash -n`（1 颗）、
  PowerShell **只解析不执行**（`[scriptblock]::Create`，7 颗，`PS_PARSE_BAD=0`）。BOM 与 LF 逐张保持原样
  （无 BOM 那四颗 .ps1 换进去的字必须纯 ASCII，中文进无 BOM 的 .ps1 = 整条 ParserError）。
- 自己踩的一次：脚本的"用了 `expanduser` 却没 `import os`"这条检查排在**写盘之后**，
  于是 16 张已落地、第 17 张红着退出 —— 当场补上 `import os` 再重扫。**收尾检查要排在写盘前面**，
  排在后面就等于给自己留了一个"改坏一半"的中间态。
- 新闸 `tools/tracked_leak_check.py`（CI 每推都跑，572 张）：图案和脱敏那颗同源，但加了一张占位白名单 ——
  直接拿 `USER_HOME_RE` 判会把 `C:/Users/x`、`zzname` 这些占位也判红（这一轮"形状命中 11 行、真用户名 0 行"），
  那就是 #64 那颗坑的第二次踩法。反向验：`git add -N` 一颗带 `C:\Users\<某个真账号>\...` 的探针 →
  `TRACKED_LEAK_HITS=1`、退出码 1；撤掉 → 0。YAML 本地过 js-yaml：`jobs=1 windows`、`steps=25`（原来 24）。

## 第 20 颗插件：随机数生成（2026-10-22，core 1.4.1 → 1.5.0）

- 形状：卡片墙一张新卡 + 搜索框直达，四档一排（整数 / 小数 / 字符串 / UUID），
  **按下档位当场生成一批**；参数一行一件事、动作（生成·复制·清空）排在行尾、状态行常驻在最下面。
- 生成那一层是 `Rand::generate()` 一颗**纯函数**，页面和判据读同一份。
  为什么单独拿出来：区间够不够、池子多大、不重复取不取得出来 —— 既要在界面上说成人话，又要在
  `--randtest` 里被逐条钉住；写在成员函数里，判据要么重抄一遍算法（两份一漂就各自绿），
  要么"脑内减掉"（假绿）。字符集同理：`Rand::poolOf()` 只有一份，界面画勾选框、判据验"没勾的不许出现"都读它。
- 三条**故意不做**的事，都在判据里钉住了（不是"没来得及"，是"不许偷偷做"）：
  ① 区间不够时**静默少给** —— 红；② 小数档被问「不重复」时**偷偷实现一个** —— 说不；
  ③ 结果弄坏之后**留着上一轮那五行** —— 清空 + 改口说 ✘（那条是 QML 那批"瞬时文案被缓存成永久状态"的坑在这儿的重演）。
- 窄区间和宽区间的「不重复」是两条分支（抽样洗 / 掷到没撞），判据**分开量**：
  `1…10` 取满 10 验前者，`1…10 亿` 取 500 验后者。合成一条就变成"只测了一条分支的另一条"。
- 上下界用 `qint64`：`span` 靠**无符号回绕**算差值（`(quint64)hi - (quint64)lo == hi - lo`），
  整条 2^64 那一档单独挡掉 —— 不加那句，`span` 会加回 0，于是"一个值都取不出"被当成正常结果。
- 数字：`--randtest` **52 条 0 红 / 136 ms**；`--plugintest` 因为多了一颗：**300 → 312 条 0 红 / 6104 ms**
  （那三趟按颗铺开的闪退判据自动多铺一颗，新插件从第一天起就用 `QPointer` 存那张页）；
  离线全套 **760 条 + `--cfgcheck` 28 行 ok，0 红**。
- 反向验（两处一起坏）：静默少给 + 切模式时拿"已经展开过的样式表"再 `expand` 一遍
  ⇒ `--randtest` **红 3 条、rc=1**；改回来 0 红。第二处就是本轮我自己先写错、再被判据抓住的那个：
  `changeEvent` 里我先用 `m_status->styleSheet()` 当模板重挂 —— 样式表里早就没有 `@okText` 这几个字了，
  那是"读上一轮留下的状态"，所以改成把最后一句原文和 ok/not-ok 各存一份（`m_statusText` / `m_statusOk`）。
- 第一次编译红了 5 处，全是新代码：`bounded()` 不是 const（探针函数签名照抄错了）、
  `QChar` 没有 `++`（改成 `ushort` 递增）、`QSet::insert` 在 Qt6 返回的是 iterator 不是 bool、
  `generateDouble()` 是实例方法不是静态。**主仓库有 PCH 兜着，插件仓库没有** ——
  这类"少一个 include / 用错签名"要在插件仓库独立编那一次才暴露，所以那一趟也不能省。
- 一颗新插件要动的地方（这次逐条数过）：`plugins/<id>/` 四份文件、`CMakeLists.txt` 的
  `ltool_add_tool` + `add_dependencies`、`src/main.cpp` 的 `kTools` 名单 + `--randtest` 旗标 +
  沙箱名单、README（功能表一行 + 插件清单 + 判据表一行）、`docs/notes.md`。
  顺手把**还活着的"19 颗"计数**清掉：CI 步骤名、`deliver.ps1` 的文案、四份工具里的句子改成
  "每一颗"或现算（`postrelease_check.py` 现在从 `plugins/*/plugin.json` 数），
  因为钉死的那颗数字第二次同步就过期 —— 上一轮在插件仓库刚踩过同一个形状。
- 版本：**1.4.1 → 1.5.0**（可感知的新功能 = minor，按"每次改代码判断升不升版本"那条）。
  1.5.0 还没发版 ⇒ 商店那条 `releases/download/v1.5.0/plugins-index.json` 是 404，属预期；
  发版之后清单会是 **20 条**（`plugins-index` 的颗数由 `plugins/*/plugin.json` 现算）。
- 还没做的两件事，等你一句话：① 插件仓库 `ltool_plugin` 那边要不要跟着收第 20 颗
  （`sync_plugins.py --apply` 会新放 1 颗 + 给那边补一份 per-plugin CMakeLists；
  core 公开头一字未动，所以 sdk/ 的头不用重生成，只有 `INFO.txt` 那颗版本章会变）；
  ② 本机要不要跑 `deliver.ps1` 那趟闭环 —— 它会**停掉你现在开着的那个实例**（`pid=16012`），
  所以我没自己按。
## 商店那面墙加了两维过滤：名字 + 全部/已装/未装（2026-10-23，core 还停在 1.5.0）
- 他要的是"搜索插件名称"，第二句补成"增加过滤，安装状态，全部、已装、未装"。落法是**常驻一行**：一颗名称框（带 ✕ 清空）+ 三档单选，底下再留一行常驻的话 —— 不是弹层、不是模态，点一下当场换那一面墙。
- 两维是**与**，而且各自只有一份实现：名字那一维只有 `ToolsPage::nameMatches()` 一颗（只比卡片上那个显示名，关键词和 id 一概不算 —— 想按关键词找走主搜索框）；状态那一维只有 `PluginStore::countsAsInstalled()` 一颗，界面不自己再判一遍。已装/可更新/与商店不同/清单外/已禁用 = 「已装」，能装/接口对不上/条目不能用 = 「未装」，排进待删而文件还在盘上的那条 = 「已装」（盘上有它就得说在）。
- **正看着一张插件页时改过滤不把人拽回卡片墙**：重排欠着（`m_filterDirty`），按「返回商店」那一步补上，那一行如实说「返回商店时才重排这一面」。判"现在是不是正看着一张页"只准问 `isHidden()` —— 离屏那扇窗从没真 show 过，`isVisible()` 对它的每个孩子恒 false，第一版就是这么判成"没在看页"、当场跳回墙。
- 数字：`--plugintest` **312 → 339 条 0 红**（过滤这一栏 27 条），整趟实测 **7764 ms**（连着三趟 7764 / 7753 / 7793）；离线全套按 CI 那 19 面旗、`QT_QPA_PLATFORM=offscreen` 跑 = **1001 条 0 红 / 3 条点名 SKIP**（没提权读不了 `$MFT`、offscreen 的剪贴板是内存桩、`--pathcfg` 那一条）。
- 反向验两处，各红各的：① `passesShelf()` 一上来就 return true（三档全废）⇒ **红 3 条**（「点未装：两档相加」got=40 want=20、「两维是与」got=1 want=0、0 命中那一行改口）；② `passedRows()` 里把名字当空串（名称框全废）⇒ **红 5 条**（名字筛「随机」got=20、不存在的串 got=20、活得过拆墙 got=20、重排后那一行 got=no、返回商店补上 got=20）。两处都改回来 0 红。
- **这台机量不到的一栏如实说**：本机 20 颗全在盘上 ⇒「未装」那一档 0 颗。所以只坏状态那一维时，「点已装 = 卡片数 = 20」是白过的（红的是"两档相加"和"与"那两条）；等哪台机真有一颗没装，那一条才算真量到。
- 上一轮那条 `rc=139` 不是产品崩：那一笔编译是**红着退出的**（`src/main.cpp(1496)` C2001），跑的还是旧 exe —— 旧的那版过滤框没有 accessibleName ⇒ 判据先 `FAIL`，探针接着拿 null 指针去 `setText` ⇒ 段错误，还把后面 30 条和整段尾巴（缓冲的 stdout）一起吃掉。两条教训：编红了不许说"跑过了"；探针在"那颗控件没找到"之后还继续 deref 是自己给自己挖坑（本轮没动那 60 行缩进，记在这儿当下一次的活）。
- 版本：**1.5.0 不动** —— 还没发版，同批功能吸收进那个号（`ltool.iss` 那行仍是 1.5.0）。`ToolsPage` 在 core 里、不在 SDK 那 26 份头的闭包里，加成员不动插件 ABI ⇒ API 号还是 4。

## 「图片排版」那条角标判据：本机红、offscreen 绿，根子是探针赌了一个像素（2026-10-23）
- 现象：`--gridtest` 63 绿 1 红，红的是那条**探针自己的体检**（`板上的角标真的画出来了（grab 不是空图）`）。`QT_QPA_PLATFORM=offscreen` 跑就 64 条全绿，不设（= `windows` 平台）就那一条红，三次都是同一条。
- 先分"是不是今天带进来的"：拿装在软件目录里那颗 **1.5.0**（= 昨天 `641ccef` 那一次构建的产物）跑同一条旗、同一个平台 —— **也红同一条**。所以跟今天这两笔（商店过滤、CI 名单）无关，是那条判据自己一直脆。
- 病因：那条只读**一个像素** `(130,23)`，而实测角标矩形是 `(119,12 22x22)` —— 那个点正落在 ✕ 的字身上。角标是"深色半透明底 + 白色那个 ✕"，`windows` 平台用雅黑、offscreen 用默认字体，字形一换那一笔就盖住采样点：读到白（G=255）就判成"没画"。两次 `dpr` 都是 1，不是缩放的事。
- 改法：**数整块矩形里有几个比图暗的点**（底压在图上一定比图暗），并且不许探针自己重算矩形拼法 —— 把产品自己那颗 `GridBoard::closeRect()` 从 private 提到 public，判据量它（画角标、命中 ✕、探针数的都是同一块，漂不了）。顺手把 grab 的取色按 `devicePixelRatio()` 换算再读（本机 1 所以那两条没动，换缩放就不漂）。
- 实测数字定门槛：`角标矩形里 256 个点：比图暗 228、比图亮 28`（亮的那 28 个就是白色 ✕ 那一笔）⇒ 门槛拍在 **40**（比实测低一个数量级、空图给 0，两条都够远）。
- 反向验：只把 cell 0 那颗角标的绘制跳过（`if (cell.left() != 8)`）⇒ 暗点 **0**、亮点 **0**，那条红、`rc=1`；改回来 64 条 0 红，两个平台都绿。
- 号：`grid` **1.0.3 → 1.0.4**（`plugin_version_gate.sh` 那条纪律：`plugins/<id>/` 里除 plugin.json 之外改了源码，那颗自己 +1）。这轮只改了探针和一颗内部 helper 的可见性，**产品行为一字未变**。

## 「一键下载全部」：两段就地确认，排的还是那条「一次只飞一颗」的队列（2026-10-23 中午，core 还是 1.5.0）
- 他一句话点的功能。两处读法他拍的：范围 = **整个商店里没装的那几颗**（不跟着上午那行过滤走 —— 跟着走"全部"就名不副实），确认 = **就地两段**（不是模态框：模态框挂在失焦就自藏的启动器里，他看不见也答不了）。
- 排谁只认一颗：`ToolsPage::bulkTargets()` 只收 `State::Missing`。接口对不上的那条在 `compare()` 里就已经换成 `ApiMismatch` 了，所以这里不再自己比一遍 Api（比第二处就会和那里说法不一）；"另有 N 颗用不了"那句数的是 `ApiMismatch + Blocked`。落没落地也只认一颗：`bulkMissed()` 问的是数据层现在的说法，不是"我数了几次回调"（回调次数会把"下成了但哈希不对被扔掉"那一支算成功）。
- 第二下走的是**同一颗** `installEntry()`：卡片按钮、他一张一张点、一键这一批，三条都进同一个队列。被过滤挡在墙外那几张拿不到卡片控件就传 `nullptr` —— 那条队列本来就允许"没地方写字、下载照样走完"，不另开一条后台下载路。上一批还在飞时再按它不插队。
- 确认态的寿命：`rebuildGrid()` 第一件事就是收回它。理由是那个 N 是**上一秒的商店** —— 中间他点了「刷新清单」、或哪颗装完，数字就不作准了，得重新按第一下看新的。标签两版（「一键下载全部」/「确认这 N 颗」）的宽度按最宽那版预留过，点下去只换字、不撑页眉那一排。
- 数字：`--plugintest` **339 → 368 条 0 红**（一键那一栏 29 条），整趟实测 **7691 ms**（连着四趟 7949 / 7709 / 7691 / 7130 都 rc=0）；离线全套（CI 那 19 面旗、offscreen）**1030 条 0 红 / 3 条点名 SKIP**。闸全绿：theme 0 漏、proxy 0、plugin_version NEEDS_BUMP=0、core_abi 0（`ToolsPage` 不在 SDK 那 26 份头的闭包里 ⇒ API 仍是 4）。
- **反向验三处，各红各的**（各改一版编一遍，不是三处一起编）：
  ① 绕过队列直接 `runInstall`（一次全飞出去）⇒ 红 2 条，两条都是 `got=[2/2] want=[1/2]` —— 就是"他连点九颗那次崩"那一类；
  ② 拆墙时不收回确认 ⇒ 红 3 条（收回那条 + 它后面两条跟着红）；
  ③ 把确认那一档改成永不停留 / 永不放行两个方向 ⇒ 分别红 5 条和 3 条，红的正是"第一下一颗都不动"和"第二下真排队"那两组。三处都改回来 0 红。
- **一条先红了才想明白的**：我原本让那两条假下载**真失败**之后再量"收尾那一行"。本机实测同一条 4200 ms 和 12000 ms（撞上限）都出现过；换成 `127.0.0.1` 上没人听的端口更糟 —— 这台机每次连接要磨 2 秒，两条 × 两个候选 × 两趟就是十几秒。**一面闸不该看 DNS 的脸色** ⇒ 把"收尾说成了几颗"抽成纯函数 `bulkMissed()`，界面那侧只留确定性的三条（一颗在飞 + 其余排着 / 在飞途中拆一次墙 / 在飞途中把这扇窗关掉）。顺带删了一条"目录里该留下两个 `.part`"的判据 —— 第二颗要等第一颗失败才开始，数文件同样是看脸色。
- **一条不该写的判据**：「在飞途中关窗：没崩」我一开始写成 `has(..., true)` —— 一条永远真不了的判据就是装饰性绿。改成一行 INFO + 证据是整趟 rc=0（真崩就死在那两句之间，缓冲的 stdout 一起没）。
- 假清单怎么造（离线判据里最省的一招）：往沙箱数据目录写 `plugins_cache.json`（两颗 `api=4` 的假条目 + 一颗 `api=3` 的），再把 `store/lastFetchDate` 写成今天 —— 不写这颗日期，构造函数会自己起那 100 ms 的自动拉清单定时器，中途把这一面重排掉，判据就成了看网络脸色的（`--storeexit` 那条注释里早钉过同一件事）。地址全用 `.invalid`：RFC 2606 保留域，本机实测 31 ms 解析失败，一条字节都不出这台机器。
- 自己造的自己收：那两条假下载会在插件目录留下 `ltool-fake*.part`，路径由 `PluginHost::pluginsDir()` 现算成绝对路径、只删这一个前缀的名字，最后一句判据量的是"删完再数 = 0"。
- 版本：**1.5.0 不动**（还没发版，同批功能吸收进那个号）。**这轮没装机** —— 上午那句「打包安装」给的是上午那两笔，授权不延续。

## 建页那一条边界加了 SEH 外壳：插件里读坏内存，程序不再跟着死（2026-10-23 下午，core 仍 1.5.0）
- 他问"有没有办法插件报错但程序不崩溃"。三档里拍的是**第二档**（SEH 包边界），第三档（插件进子进程）要重写整个插件模型 —— 插件交回来的是 `QWidget` 页，跨进程就得把控件树和事件走 IPC。这一轮**只铺建页那一条**（历史上真崩过的就这一条）。
- 落点只有一颗：`PluginHost::buildPage()` —— 全仓库唯一一处跨进插件代码造页的地方（原来那句 `p->createPage(parent)` 长在 `page()` 里，`grep createPage` 数的）。`page()` 现在只做"查缓存 → 找接口 → 交给这一颗 → 成了才缓存"。
- 外壳的形状：`__try/__except` 只能待在一个**没有任何需要析构的局部对象**的函数里（MSVC C2712），所以那层是一个纯指针进出的小壳（`runCreatePageSeh`），`QString` 全留在外面拼；异常码和指令地址先落在 POD 的 `SehHit` 里，**出壳之后**才写日志（异常展开里再分配大对象，第二次崩会把线索一起吃掉 —— 和 `SetUnhandledExceptionFilter` 那条同一个理由）。
- 界面那一行说的是什么：`<id> 建页时撞在异常 0xC0000005（指令地址 7ff8…）· 这一趟没给你页，程序没跟着死`，走的是 `ToolsPage::showTool()` 早就有的那句"页眉说实话"（页没开成时原因写在标题旁边，不是只进日志）。
- **买到什么、没买到什么**：买到的是窗口不消失 + 证据留下来 + 半建那张不当好页缓存；**没买到**"这个 bug 被关住了" —— 那一趟已经碰过坏内存，后面是带着未知损伤继续跑，所以 WER 转储、`pe_rva.py` 那条取证路一律保留。**一律接住**也意味着 `0xC00000FD`（栈溢出）和 C++ 异常（`0xE06D7402`）同样被接住并如实写码，不再"只有访问违例外都陪着死"。非 Windows 那边这层**如实不装**（`#else` 里就是直调 + 一句注释：接信号要 `sigsetjmp`，从信号里继续跑更不可靠；真做得走进程隔离），所以那几条判据也 `#ifdef Q_OS_WIN` —— 在 Linux 上拿故意崩的接口去喂它不是"红"，是假红。
- 数字：`--plugintest` **368 → 377 条 0 红**（SEH 这一栏 9 条），整趟实测 **8702 ms**（三趟 8801/8634/8702）；离线全套 **1039 条 0 红 / 3 条点名 SKIP**；闸全绿（`core_abi_gate` 查的是 SDK 那 26 份头，`PluginHost.h` 不在里面 ⇒ 加一颗函数不动插件布局、API 仍 4；`plugin_version_gate` NEEDS_BUMP=0，这颗改动一颗插件源码都没动）。
- **A/B 是决定性的一条**：把 `plugins/json` 的 `createPage` 改成开头就写坏内存（真插件、真装载、真走 `page()`），只改这一个变量，编两颗二进制对着跑 ——
  有外壳：**rc=1**（判据红是对的），359 条落地 / 13 条红**全点在 json 那颗**（`FAIL json：createPage 给出一张页（json 建页时撞在异常 0xC0000005…）`），其余 19 颗照常，日志 **36120 字节 / 384 行**，尾巴完整；
  把 `page()` 那句换回直调（外壳绕过）：**rc=139 段错误**，只有 55 条落地、日志 **4152 字节**，后面全部连同尾巴被吃掉。
  这一对就是"接线了"的证据 —— 单看合成接口那 9 条量不到（判据直接调 `buildPage`，产品绕过它也不会红）。两处改完都原样改回，`plugins/json` 一字未动。
- **探针自己犯的错（第一次就撞出来了，不是我看出来的）**：那 9 条第一版为了"外壳别挡好人"，直接拿真插件走了一遍 `buildPage(plugin("json"), …)`，而那张页**没进 `g_pages` 缓存**、parent 又是空 —— 于是那颗壳里的 `m_page` 被我这张孤儿页顶掉了，跑在后面的"整扇窗关掉后再拿"当场从 `refused/usable` 变成 `fed/usable`（19/20）。**判据自己弄坏被测对象**，形状和上午那条"读到上一轮留下的状态"是一族。改成三颗自造的接口（撞的 / 正常给一张挂在我自己那棵树上的 / 返回空的），用完 `delete host` 收走。
- 顺手补的一条产品规矩：`buildPage()` 进来先 `err->clear()`。契约是"给出页了就没有原因" —— 不清的话调用方复用同一个 `QString` 时会把**上一趟**的失败念成这一趟的（那句判据现在钉着：正常那一支 `page/quiet`）。
- 还没铺的边界（他拍的顺序）：`feed()`（往页里递文本）、`invoke()`（热键/右键转发）、`startBackground()`（后台那颗自己跑）—— 同一层壳套三次就行，但要先各造一条会红的判据（同这次这个 A/B 形状），不然就是"包了个没人数的守卫"。
- 版本：**1.5.0 不动**（未发版，同批吸收）。**没装机**，也不动插件产物：这轮改的是 core 的一层壳 + 判据。

## 剩下几条边界也接上同一层 SEH 壳：一份壳、五种调用（2026-10-23 下午第二笔，core 仍 1.5.0）
- 没照"每条边界套一次 `__try`"写。收敛成**全仓库一份壳**：`PluginHost.cpp` 里一个 POD 的 `Cross`（要调谁、参数、结果口子、哪一类）+ 一个 `crossCall()` 的 switch + 一个 `crossSeh()`。五种跨进插件的调用全从这一颗过：`createPage` / `feedText` / `invoke` / 那个 extern "C" 的激活函数 / `apiVersion` / `actions`。
- 为什么非得是一份：`__try` 那个函数里不能有需要析构的局部对象（C2712），照四条各写一份就是四份同样的指针舞蹈 —— 漏改一份就有一条边界没人守（#94 那次「--storenet 四条红没人看见」是同一个形状）。
- 产品那四个入口现在各自只剩"查一下，交出去"：`page()` → `buildPage()`、`feed()` → `feedInto()`、`invoke()` → `actionsInto()` + `invokeInto()`、`startBackground()` → `activateInto()`、`plugin()` 装载那一问 → `apiVersionInto()`。判据直接调的也是这同一批函数，不是另写的外壳。
- 契约写死在头文件注释里：**返回值 = 这一趟回没回来**，插件自己答"是/否"走 `accepted` / `didIt` —— 两种"否"界面上是两句话。三条具体的：
  ① 撞了**不许被算成插件答了「是」**（判据拿 `ans` 初值 true 去量，回来必须是 0）；
  ② `why` 进来先 `clear()`（上一笔那条教训的推广：不清就会把上一趟的失败念成这一趟的）；
  ③ 各条撞了之后的**产品行为**不一样 —— 装载那一问撞了 = 这颗 dll 直接拒掉并写进 `g_loadErrors`（10-22 那批"名字对得上、一用就崩"就死在这一脚）；路由时 `actions()`/`invoke()` 撞了 = 跳过这一家但**照样把 `who` 给出去**（不然"按了 F1 什么都没发生"又回来）；后台激活撞了 = **不算进 `started` 那个数**并点名（悄悄少一个后台功能是最难查的）。
- 数字：`--plugintest` **377 → 388 条 0 红**（五条边界共 20 条：建页 9 + 另四条 11），整趟实测 **7706 ms**（两趟 7706/7660）；离线全套 **1050 条 0 红 / 3 条点名 SKIP**；闸全绿（theme / proxy / plugin_version NEEDS_BUMP=0 / core_abi 0 —— `PluginHost.h` 不在 SDK 那 26 份头里，加函数不动插件布局，**API 仍 4**；tracked_leak 0）。
- **A/B 做在「递文本」这条上**（真插件、真走产品入口 `feed()`，只改一个变量）：把 `plugins/json` 的 `feedText` 开头改成写坏内存 ——
  有壳：**rc=1**、386 条落地、红**只有 2 条**且都点名 json 递文本（`把一段 JSON 递给 json 工具，插件接住了` / `那段文本真的进了工具`），其余 19 颗照常；stdout 37916 字节；沙箱日志里那句实话出现 6 次：`plugin: json 递文本时撞在异常 0xC0000005（指令地址 7ff8d9ee4d44）· 这一趟没干成，程序没跟着死`。
  把 `feed()` 换回直调（绕过壳）：**rc=139**、只有 181 条落地、stdout 16566 字节，后面连同尾巴被吃。
  两处都原样改回，`plugins/json` 一字未动（提交里也没有它）。
- **两条没做 A/B，如实说**：`invoke()` / `startBackground()` 的接线只靠"产品入口各调一次 + 同一份壳 + 建页和递文本两次已证的机制"。原因是真路由一个动作得走 `--fileinfo` 那面旗，而那颗真插件会把对话框弹到他屏幕上（自测不许碰他的桌面）；后台那颗要在自测里真 `activate` 就会把 linuxdo 的轮询定时器起起来，判据又变成看网络脸色。要补的话，得先给自测造一个不弹 UI、不联网的动作口子。
- 判据自己写错被当场抓的一条：`QStringLiteral("%1/%2").arg(一个 bool)` —— 第二个洞没填，那条报 `got=[1/%2]`，红给我看。数格式串里的 `%N` 和 `.arg()` 的个数，这轮又欠了一次。
- 补一条（14:5x）：那颗按钮的预留宽度改成拿 `sizeHint()` 量两版标签取大的。上一版按「几个字 × 一个字宽」估，只按「确认这 12 颗」留，而本机 `windows` 平台下基线那句「一键下载全部」更宽（六个纯汉字 vs 夹数字和空格）⇒ 一点就把旁边两颗按钮挤走。判据也跟着换成量 `sizeHint()`（不再自己估字数），并补一条"探针借道试过宽标签之后要把标签还回去"。两个平台各两趟：`offscreen` 389 / `windows` 389，全 0 红；离线全套 1051 条 0 红。

## 「广告弹窗 → 忽略的进程」加了一颗「从正在跑的进程里挑一个」（2026-10-23 下午，ads 1.0.1 → 1.0.2）
- 他一句话："广告弹窗的忽略进程可用点击一个按钮选择进程列表的一个进程进行忽略"。落在那页「编辑规则…」里 **那一栏底下**（草稿框旁边），不是新开一页：按下弹一张"此刻在跑的进程"，双击一行或选中按「加进去」就写进「忽略的进程」那一栏，**可以连着挑几颗**，最后还要按「保存」才生效 —— 和那四栏同一个规矩，不搞两套保存模型。
- 两份唯一实现：① 进程名 → 名单那一行的规整法抽成 `AdBlocker::allowProcFor()`（小写、去 `.exe`），原来这条只长在"右键放行这一颗"那一句里，两处各写一遍迟早会不一样；② "这颗是不是本来就被盖住了"抽成 `coveredByAllowProc()`，方向按那一侧的真匹配来（**进程名包含片段**），命中就回那条片段，界面上原样点名「被「wechat」盖着」。
- 列进程是平台缝第五道加的：`Platform::runningImageNames()` —— Windows 用 `CreateToolhelp32Snapshot`，按映像名去重（这台机 **260 个进程压成 89 行**，浏览器/IM/svchost 那种一份跑好几个的都并掉）、不区分大小写排序；POSIX 那颗**如实返回空**并写明"是没写、不是做不到"，界面上那句"这台机器列不出进程"不许变成一张空表。
- 筛是**藏行不重建**（重建会把选中项和滚动位置一起丢掉，软件页那条老规矩）。
- 数字：`--plugintest` **389 → 396 条 0 红**（这一栏 8 条：规整法四种输入、盖住/没盖住/方向、列表非空且没有空名和带路径的名字、去重这件事本身），`offscreen` 和 `windows` 两个平台各跑一遍都 0 红，整趟实测 **7650 ms**；插件号 `ads` 1.0.1 → **1.0.2**（`plugin_version_gate` 认的）。
- **反向验把我自己的一条判据抓成假绿**：那条写的是"名单里那行空的不能盖住一切"，我把 `continue` 改成不跳，函数回的还是空串、调用方照样当"没盖住" ⇒ **判据一动不动**（`--plugintest` 396 条全绿）。原因是那条量的输入根本到不了这儿（存进名单前 `linesToPatterns` 就把空行丢了），而"空串"这个返回值同时表示"没盖住"和"被一条空的盖住"，两个意思挤在一个值里。换成一条打得动的：**名单里那条比这颗更长 ⇒ 不算盖住**（包含方向不能反）—— 把 `one.contains(p)` 翻成 `p.contains(one)` 重编，它当场红。这条也算今天第二次撞到"判据在 0 个对象上空转"那一族（第一次是 `has(..., true)`）。
- 没做的一条：**那颗按钮和那张表本身没有判据** —— `plugins/ads` 没有自带判据文件（`--gridtest` / `--randtest` 那种），要跑得先给它加一颗 `--adstest` + 接进 CI 那栏；规整法和方向这两颗纯函数已经钉住了，界面那半目前是"我本机没点过、CI 也没数"。

## 「为什么时不时有大量的磁盘 IO 和 CPU」：这轮全部拿数回答（2026-10-23 下午）

- **静置基线**（常驻那颗 pid=23336，11:54 起的）：连采 12 个窗口 `\Process(*)\IO Read/Write Bytes/sec` ⇒ ltool **读 0.0 KB/s、写 0.0 KB/s**，整榜排不进前 10；20 秒 `TotalProcessorTime` 增量 **16 ms = 单核的 0.1%**；工作集 152 MB → 152 MB。⇒ 5 秒一次的窗口扫描和 2 秒一次的剪贴板轮询都太轻，量不出来，**常驻状态不是 IO 的来源**。
- **同窗整机的排名**（KB/s，平均 12 个采样窗口）：`qoder cn` 读 1,221 / 写 1,217（就是我这个会话所在的进程）、`chrome` 967 / 970、`qqpctray` 54 / 6、`wechatappex` 13 / 13、`registry` 0 / 14、`svchost` 8 / 0、`system` 1 / 9、`qqpcrtp` 4 / 3。⇒ 他看见的"大量磁盘 IO"，此刻主要不是 ltool 给的。
- **一次完整扫描的代价**（拿 `--smoke` 走产品同一条 `startScan()`，在临时数据目录里跑，没碰他的）：**墙钟 2,952 ms、CPU 1.11 核·秒、文件索引 9,409 条目**；应用那半它自己报过：`应用目录 216ms` + `AppsFolder 464ms`。同窗 `\Memory\Pages Input/sec`（硬缺页）只有 **0–28 次/秒**，累计约 9 页 ⇒ **基本没从盘上拉东西**：目录枚举读的是 NTFS 元数据，一是全在系统文件缓存里，二是这类读记在 System/缓存管理器头上、**不记在 ltool 自己的进程 IO 计数里** —— 所以"进程读 0"和"盘灯会亮"两件事不矛盾。（同一份探针里 `\PhysicalDisk(0 C:)\Disk Read Bytes/sec` 那栏采样器没匹配上实例名，是空的，这一格我拿的是硬缺页那列，不拿它说事。）
- **触发节奏**（他日志里的原时间戳）：12:06 / 12:27 / 12:45 / 13:16 / 13:58 / 14:44 各一次 `scan finished: 350 apps` + `apps: watching 116 shortcut dirs (2 new)` ⇒ 3.5 小时 6 次，约每 20–45 分钟一次。点火的是挂在开始菜单 **116 个目录**上的 `QFileSystemWatcher`，节流只保证"距上次 ≥60 秒"（[src/SearchWindow.cpp:314-323](file:///D:/project/cpp/ltool/src/SearchWindow.cpp#L314)），另有一颗和 `index/refreshHours=7` 同周期的（[src/SearchWindow.cpp:339-347](file:///D:/project/cpp/ltool/src/SearchWindow.cpp#L339)）。**要点：重扫不只重扫快捷方式** —— `startScan()` 尾巴上挂着 `FileIndexer::reindexAsync()`（[src/SearchWindow.cpp:1835](file:///D:/project/cpp/ltool/src/SearchWindow.cpp#L1835)），所以桌面 4 层 / 文档·下载 3 层 / 图片·音乐·视频 2 层 / 主目录 1 层每次跟着整棵再走一遍（[src/FileIndexer.cpp:220-236](file:///D:/project/cpp/ltool/src/FileIndexer.cpp#L220)）。这就是"时不时一下"的形状：一次约 1 核·秒的 CPU 脉冲，盘上很轻。
- **深索引那条被排除掉了**：`deep index` / `MFT` / `volume` 在他这份日志（10-21 16:12 起）里 **0 次命中**，配置里 `index/deep=false` ⇒ 那条 6 小时、每卷直读 `$MFT` 的路根本没跑；`deepindex.dat` 那 335 MB 是 10-03 留下的死文件，占地方不产生 IO。
- **15:00:26–15:02:09 那一串网络+写入是我造成的，不是它的**：`--smoke`、`--mfttest` 两面旗**不在沙箱名单里**（[src/main.cpp:622-632](file:///D:/project/cpp/ltool/src/main.cpp#L622) 只列了 `--plugintest`/`--randtest` 那一串），于是它们直接落进他真实的数据目录，把"一天一次拉清单"点着 ⇒ 三个镜像对 `v1.5.0/plugins-index.json` 报 404、直连两次 20 秒超时，并往他的缓存里写了东西。15:02 之后到 15:24 没再出现这一串。
- **顺着这条查出来的一个真问题**：[src/main.cpp:227](file:///D:/project/cpp/ltool/src/main.cpp#L227) 里 `--smoke` 在非提权环境会把 `index/deep` 写成 `false`（本意是"CI 干净环境别花几分钟做全盘深扫"，这条守卫只该管沙箱里那份配置）。全项目只有两处写这个键（另一处是配置页那颗勾）⇒ 如果他没自己关过深索引，**就是他机器上那份被我某次不带沙箱的 `--smoke` 关掉的**。要修的是把这两面旗挪进沙箱名单，顺带决定要不要替他把深索引打开 —— 等他拍。
- 探针留在仓库：`build/verify/io_probe2.ps1`（沙箱里跑一趟，同时采 CPU/硬缺页）、`build/verify/io_top.ps1`（整机按进程排名 IO）。两张都走 `$env:TEMP`，没有写死的家目录。
- **版本**：这轮没动产品代码一行（只有文档 + 两张一次性探针）⇒ **不升号**；core 仍是 1.5.0，插件接口仍是 API 4。

## 商店顶部那两行：短话上页面，全文进悬停说明和日志（2026-10-23 傍晚）
- 他一句话 + 一张截图：「不要放到插件详情页面」。截图里是那两行 —— 「网络拉取未成功：Operation timed out（HTTP 0，走的是 https://…v1.5.0/plugins-index.json）—— 当前展示本地缓存清单（共 19 条）」和「自动更新：10-23 11:02 · 没有要换的（网络没成（Error transferring … 404 …）），拿本地缓存那 19 条清单试)」。摆给他三个落点（页上留短话 / 整行挪到「关于」/ 页面一行都不留），他选 **A：页上留一句短话，全文进日志**。
- 一条规矩只留一个出口：`ToolsPage::setStoreLine(短话, 全文)` 接管那两颗标签的**全部 11 处写入**（拉取中 / 失败有缓存 / 失败无缓存 / 成功那一条 / 一键那五句 / 建墙时那两句），`setAutoNote()` 管自动更新那一行。原来那五句一键状态是直接 `m_storeStatus->setText(...)` 各写各的，一并改走这颗 —— 不是顺手，是"上一句的悬停说明要收掉"这条规矩只有在一个出口的前提下才成立。
- 页面那四句变成：「正在拉清单…」「网络没通，先看本地缓存那 19 条」「清单没拉到，这一面退回本机装着的那几个，它们照常能用」「清单 20 条 · 这一面 20 张卡 · 能装或可更新 0 · 拒收 0 · 清单外 0 —— …」。URL、走的哪一档、试了几次、拒收的是谁，全在悬停说明里；日志那三份本来就带原文，一处没减。
- **两份唯一实现**（不这么写就会漂）：① 自动更新那句的"哪条链拿到的"从 note 里搬进新的 `store/autoUpdateWhy`，note 只留「清单 19 条」这种短话；② `PluginStore::autoBrief()` 是"存在配置里的那条 note → 页面上那句短话"那颗谓词，**读的时候也要过一刀** —— 1.5.0 之前 note 存的就是整条原文，只改写入点的话，他屏幕上那行要一直难看到明天早上下一次自动更新跑完。切法：带 http 的从第一个「（」切掉；切完还带 http（老格式里没括号那句）就退成「上次那一趟没成（原文在悬停说明里）」，宁可退这一句也不把原文推上页面。原文一个字不丢：没写 why 时悬停说明拿回那条老 note。
- 数字：`--storetest` **109 条 0 红 / 9,661 ms**（这一轮新加 5 条：带 http 的切短、不带 http 的原样过不许多切一刀、没括号的退固定短话、老 note 进悬停说明、写了 why 之后两份各管各的；另有一条改了名字 —— 原来那条问"清单是从哪条拿到的要带出来"，现在带出来的是短话，链在 why 那份），`--plugintest` **396 → 399 条 0 红 / 8,125 ms**（`windows` 平台，新三条：建墙时短话带悬停说明、短话接管后收掉、页面上那句不许有 http）；离线九面旗全套 **859 条 0 红**（`--rectest` 单独在 `windows` 平台 0 红 —— 它在 `offscreen` 下必红 7 条，那颗要真窗口才录得到，不是回归）；闸六道全 0（theme / proxy / plugin_version 20 颗 / core_abi / tracked_leak / notes_history_leak）。
- **我自己这一笔带出来的两条红**（都是真红，先说清再改）：① 我把 `check()` 的第三个位置当成"报告文本"用了，它是 want ⇒ 那条自己判自己红（`got=[yes] want=[没有要换的]`）；② 我在"没跑过时那一行说今天还没跑"那条判据**之前**先把 note 写进了配置，它读到上一句留下的值（`got=[no]`）。第二条正是记忆里那条"判据读到上一查询留下的状态"，这次是红给我看的，改法是那条判据前先 `remove` 两个键。
- 反向验（两条各咬住一处）：`autoBrief` 改成恒等 ⇒ `--storetest` 红 3 条；`setStoreLine` 里"清悬停说明"改成只在非空时才设 ⇒ `--plugintest` 红 1 条。两处都改回来，重编后两边 0 红（`grep -c MUTATION` = 0 才继续）。
- 顺带把 `--storenet` 那栏的等待认了新的短句：它以前只等「清单…」开头的状态行，而失败有缓存那句现在改成「网络没通…」开头 —— 不认它，这一栏在未发版那次只会白等 20 秒，而"页面上不许有 URL"那条判据就永远只在一条跑不到的成功路径上量（装饰性的绿）。现在正则读 `文本 + 悬停说明` 合起来那一份，"页面上那句不许有 http"读的是短的那一份。
- 版本：**不升号**。core 仍在未发布的 1.5.0 里（这一档可以吸收同批功能），插件一颗没动（`plugin_version_gate` 认这个），API 仍 4。数据格式多了一个 `store/autoUpdateWhy` 键，但**老配置读得动**（没这个键就走"悬停说明拿回老 note"那条），不算不兼容。
- 没做的一条，如实说：这轮**没装机**（他今天连推的这七笔都没装）。装完之后他屏幕上那两行才会变短 —— 现在装着的还是 11:54 那份（`1900d67`），它照旧显示整条原文。
## 2026-10-23 晚 v1.5.0 发版（tag `v1.5.0` → `468f960`）

- 打 tag 前六道闸在这棵树上重量一遍，全 0：theme（48 角色无重复）/ proxy（8 处出站）/ plugin_version（20 颗，NEEDS_BUMP=0、WENT_BACK=0，`random` 是新颗从 1.0.0 起算）/ core_abi（API 仍 4，`da1cd99` 以来 26 个公开头无布局漂移）/ `make_release_notes.py --check`（6 项）/ notes_history_leak（377 个提交目录，家目录命中 0）。
- tag 推完的 remote 对账：`ls-remote` 报的是**注解 tag 对象** `37027ae`，它指向提交 `468f960`（`git rev-parse v1.5.0^{}` 才是提交那一头）。拿对象 sha 直接比 HEAD 会误判成"推错了个东西"。
- CI run 39951821904：11 次轮询（约 8 分钟）到 `completed success`。Release id 421810352、非 draft 非预发、**24 个资产**（20 颗 dll + setup + sdk.zip + plugins-index.json + sha256.txt），比 v1.4.0 那次多 1 个 = 多的那颗是 `random`。
- 公开面三查（`postrelease_check.py --tag v1.5.0`，`POSTRELEASE_BAD=0`）：Release 说明里带家目录的行 = 0；商店清单 HTTP 200 且条目 = 20，和**从那个 tag 数出来**的颗数一致；`sha256.txt` 23 行，每行 digest 与资产 API 逐字对上，且除 `sha256.txt` 自己以外没有资产漏列。
- 我另外手算了脚本没量的那一层：20 条 `url` 全钉在 `releases/download/v1.5.0/`、`api` 全是 4、每条的 `sha256`/`bytes` 与资产 API 的 digest/size 一致、没有一条指向不存在的资产（`MANIFEST_FILE_NOT_IN_ASSETS=0`）。
- **踩坑（已修）**：`postrelease_check.py` 在这台机上第 2 步必崩 —— 资产那一跳 302 到 CDN 主机，schannel 拿不到吊销服务器就整条 `rc=35 CRYPT_E_REVOCATION_OFFLINE`，直连和经 9980 代理都一样，而 `api.github.com` 那一跳没事 ⇒ 极容易被读成"那条链没活"。修法是 `get()` 在 Windows 上加 `--ssl-no-revoke`（只关吊销检查，证书链照验）。
- 补完还崩了一次，那是**另一种**失败：`rc=28 github.com:443 连不上`（这台机的老毛病），原样重跑第 2 次整条绿。两种 rc 要分开读，别把 28 也当成"链没活"。
- 版本：这一笔只动 `tools/postrelease_check.py` + 这份文档 ⇒ **不升号**。但 1.5.0 从此是**已发布**的号，而树里 `ltool.iss:6` 还写着它 —— 下一次动产品代码必须往前挪（1.5.1 / 1.6.0），不能拿 1.5.0 再出第二份产物。
- 没做的两件事，如实说：① 这一版**没装机**（`deliver.ps1` 会杀他正在跑的实例，等他点头）；② 插件源码仓库 `ltool_plugin` 还差 5 个文件（`sync_plugins.py --check`：新放 0 / 改动 5 / 一致 106）—— `ads/AdsPage.cpp`、`ads/plugin.json`（1.0.1→1.0.2）与 `grid/GridBoard.h`、`grid/gridselftest.cpp`、`grid/plugin.json`（1.0.3→1.0.4）；`sdk/` 那份不用重铺（26 个公开头没动布局、API 仍 4）。那是往**另一个仓库**推，等他说"跟"。

## 2026-10-25 下午：1.5.1 的料齐了（未 tag，等点头）

- **两笔产品修复（v1.5.0 之后树里的全部产品差异，`git diff v1.5.0 --stat` = FileIndexer.cpp + main.cpp）**：
  1. **FileIndexer 两个并发崩溃窗口**：全量重建收尾的 unmap/remap、USN 增量对 m_add/m_addArena/m_removed 的追加，原先都在无锁状态下和搜索线程（读锁遍历同一批数据）赛跑 —— 重建窗口期打字搜索，最坏访问已解除映射的视图（访问违例）。改法：增量先无锁收集（含 OpenFileById 那些磁盘 IO）、短写锁一次性提交；remap 整段拿写锁。实测重建进行中持续搜索不崩。
  2. **`--smoke` 落盘改用户配置**（10-23 记录里"等他拍"的那条，本机实测又中招一次）：非管理员裸跑会把真实数据目录的 `index/deep` 写成 false。改法：只有 LTOOL_DATA_DIR 沙箱里才许落这个键；裸 --smoke 用进程内环境变量 LTOOL_SMOKE_SKIP_DEEP 跳深扫、一行配置不写。1.5.1 装出來裸跑验证：`data/config.ini` 压根没被创建。他机器上被写坏的 deep 已恢复 true（快照 335MB 还在，重启后会增量接上）。
- **六道闸全 0**（theme 48 角色 / proxy 8 处 / plugin_version 20 颗 NEEDS_BUMP=0 / core_abi API 4 / release-notes 6 项 / notes_history 388 行 0 家目录）。
- **安装包**：`installer/ltool-1.5.1-setup.exe`（23,468,680 B）。装到临时目录验过内容物：装出的 ltool.exe 与 build/Release 那份 **md5 一致**（69a243e2…）——加这道是因为今天撞见"安装器静默退出 0 但 exe 根本没换"（旧实例占用、重启管理器挂起替换，`OVERWRITE_OK` 全是假绿）。
- **删了一份违规产物**：今天 13:51 有人（我）在本机重出过一份 `ltool-1.5.0-setup.exe` —— 1.5.0 是已发布的号，不能有第二份产物，已删；那份从未离开本机。
- **如实记一笔事故**：部署竞态修复时用 `robocopy /E` 把 build\Release\data\ 一起盖进了 D:\software\ltool\data\，吞了他的 config.ini（21KB→21B）和 webs.json。从 %APPDATA% 的迁移前副本恢复（config 21,595B、webs 4,286B 含全部自定义关键字、deep=true），他的插件（linuxdo/开机启动分析等）重载正常。**仍丢的**：10-21 16:12 迁移之后到 10-25 之间他在 data\config.ini 里改过的设置（商店地址版本号等）、data\log.txt 历史。教训：往安装目录复制必须排除 data\。
- **树里的账**：产品两笔 + 版本号 1.5.0→1.5.1 + 本份文档；build/verify 下三颗未跟踪脚本（crashscan/pngcost/startapps）不是这轮的，没动。
- **没做的，等他拍**：tag `v1.5.1`、push、CI、GitHub Release（商店那 20 颗 dll + sdk.zip + plugins-index.json 由 CI 出）；`ltool_plugin` 仓库还差的 5 个文件（10-23 记的那笔）也没跟。装机这轮已经装了当前构建（他机器上跑着的就是这两笔修复），但不是走 deliver.ps1。

## 2026-10-25 晚：v1.5.1 第一趟 CI 红了，是我自己的锁写的嵌套死锁（已修 + 重打 tag）

- **形状**：tag 那趟 run（40173776361）在「无界面回归」整步红，--storeexit 挂死超时。本地离线复现同一形状（120 秒 timeout、1171 字节后无声）；把 FileIndexer/main 两文件退回 v1.5.0 版本重编即绿 —— 是 1.5.1 自己带进去的，不是环境。
- **根因**：给 remap 加锁时，`rebuildDeepAll` 的写锁块里调 `mapSnapshot()`，而 `mapSnapshot` 尾部自己也拿同一把 `QReadWriteLock` 的写锁 —— 非递归锁嵌套写，当场死锁。smoke 那条路不经过深索引（非管理员跳过），所以本地 smoke 一直绿，CI 的 storeexit（真启动、深索引真跑）第一个撞上。
- **修法**：锁改为**调用方持有**的约定（mapSnapshot/unmapSnapshot 不再自拿，函数头注释写明），`reindexAsync` 的首映射调用点补上写锁。修后 19 面旗本地全套 0 红（含 --mfttest/--diskbranch）。
- **tag 处理**：旧 v1.5.1（指向 0e12655）删掉重打到修复提交 a743b23。那趟没产出任何资产（`/releases/tags/v1.5.1` API 404、latest 仍指 v1.5.0），网页上那个"Release v1.5.1"只是 GitHub 给已存在 tag 渲染的占位页，不算发过 —— 同号没有出过两份产物。
- 顺带：把这台机上装着的第一版 1.5.1（带死锁）换成修复版（robocopy 排除 data\captures\recordings，这次没再碰他的数据）。

## 2026-10-30 · v1.5.1 发布完成（对账全绿）；顺带挖出并修掉"重建风暴"（用户报高读写+CPU）
- **发布**：tag run 40186085197 success，Release v1.5.1 非 draft，资产 24 个（20 颗插件 dll + setup 21.58MB + sdk.zip + plugins-index.json + sha256.txt）；postrelease_check 三查 POSTRELEASE_BAD=0（说明无家目录泄漏、商店链 200/20 条、sha256 对得上）。api.github.com 这会儿直连通，没走 9980 代理。
- **现象**：装机 log.txt 一天 40~131 条 "deep index full rebuild in 22~37s"，每场 30 秒级全盘遍历；进程日烧 2.2 CPU 时，任务管理器磁盘/CPU 居高。
- **根因链**：① 10-26 起进程非管理员（此前提权会话 USN 增量毫秒级），卷 err=5；② usnstate 游标为 0（快照头也 0，出自无管理员 walk 重建），incrementalDeep 见 0 直接 NeedRebuild、不探卷；③ rebuildDeepAll 回退全盘 walk，快照提交时旧 mmap 还开着（先 commit 后 unmap、CreateFileW 无 FILE_SHARE_DELETE）→ rename-over 必败 → deepindex.dat 停在 10-03，状态永不愈合；④ walk 重建又把 0 游标写回 usnstate → 死循环。触发器：116 个被监视快捷方式目录的任何变更（60s 节流），机器一动就是一场。
- **修法（FileIndexer）**：游标 0 先探卷，开不了 → NoAccess 保留快照（cacheTooOld 不再否决这条退路）；usnstate 丢游标而快照头有效时就地恢复；提交前先 unmap + FILE_SHARE_DELETE；重建后 fresh 游标在 mapSnapshot 之后覆盖写回 m_vols（原来只写快照头，管理员机器也会每轮重建）；全量重建限流 1h；NoAccess 日志每会话每卷一次。
- **部署坑（又一次）**：首次换装只拷了 ltool.exe —— 逻辑在 ltoolcore.dll，装上后行为还是旧的。对 dll 补 MD5 校验后重启才真生效。**以后部署必须 exe+全部项目 dll 一起校验。**
- **验证**：重启即 "no admin rights, keep snapshot"；注入目录变更触发重扫只多一条 keep 日志、0 重建、skip 日志不再刷；IO 采样 ltool 已不在榜上。本修复在 master，随 1.5.2 发。
