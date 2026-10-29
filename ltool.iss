; ltool Inno Setup 打包脚本
; 编译: "D:\software\InnoSetup6\ISCC.exe" ltool.iss
; 产物: installer\ltool-{#MyAppVersion}-setup.exe（按用户安装到 %LOCALAPPDATA%\Programs\ltool，无需管理员）

#define MyAppName "ltool"
#define MyAppVersion "1.5.2"
#define MyAppPublisher "ltool"
#define MyAppExeName "ltool.exe"

[Setup]
AppId={{B7E6F3A1-2C48-4D95-9A21-5F0C8D6E13A7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
; 2026-10-21：Qt 6.10 自己就只支持 Windows 10 及以上，而这套东西还硬依赖 UCRT
; （`api-ms-win-crt-*`，Win7/8.1 得单独装 KB2999226）—— 以前没写这一行，Win7/8.1 上
; **装是装得上的，双击却没反应**（Qt6Core 起不来），那是最难查的一种坏。
; 写下来之后安装器当场拒装并说清"需要 Windows 10 或以上"。Win10 用户不受任何影响。
MinVersion=10.0
OutputDir=installer
OutputBaseFilename=ltool-{#MyAppVersion}-setup
SetupIconFile=resources\ltool.ico
; 压缩档位（2026-10-12 实测，payload 71.1MB / 34 个二进制，本机 20 核；括号里是整条
; deliver.ps1 闭环的总耗时）：
;   lzma2/max    solid   9.9 秒 / 21.52 MB   （16.5 秒）—— 最省体积，以前默认就是这个
;   lzma2/max    非 solid 7.9 秒 / 22.02 MB   （14.1 秒）—— 每个文件独立压，多线程吃得更满
;   lzma2/normal solid   6.7 秒 / 22.22 MB   （12.9 秒）—— ★ 现在的默认：比 max 省 3.2 秒，只大 0.7MB
;   lzma2/normal 非 solid 6.5 秒 / 22.43 MB
;   lzma2/fast   solid   2.4 秒 / 26.54 MB   —— 只想快速出一个能装的包：deliver.ps1 -Quick
; 默认选 normal+solid 的理由就一条：安装包是本地装的，多 0.7MB 没人看得见，而每轮闭环
; 少 3.6 秒是每天都摸得到的。要出最小的正式包用 deliver.ps1 -Final（回到 max+solid）。
; CompressionThreads 不用再试了：2026-10-12 用耗时量过（上次是看产物 md5 相同判的"无效"，
; 那只能证明输出确定），不设=10.7 秒、1 线程=20.9 秒、2/4/8/16 全在 10.4~10.5 秒，
; 也就是默认已经吃满多线程。
; payload 也剪不动：按 PE 导入表从 ltool.exe 走一遍闭包，71.1MB 里只有 4.0MB 不在闭包上，
; 而那 4.0MB 全是运行时按目录加载的插件（platforms/qwindows、styles、imageformats、tls）
; 加按需加载的 7zip.dll。最肥的 icudt78.dll（31.58MB，占整包 44%）是 Qt6Core 硬导入的
; ICU 数据，实测从安装目录藏掉它，ltool.exe 直接起不来。
; 注意别用 #elifdef —— ISPP 6 不认（"Unknown preprocessor directive"，整个编译当场 abort，
; 而 deliver.ps1 那层只看文件在不在，会把它当成"打包成功、体积没变"糊过去），只能嵌套 #ifdef。
#ifdef FINAL
Compression=lzma2/max
#else
    #ifdef QUICK
    Compression=lzma2/fast
    #else
    Compression=lzma2/normal
    #endif
#endif
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
ChangesEnvironment=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; \
    GroupDescription: "附加图标："
Name: "autostart"; Description: "开机自动启动 ltool"; \
    GroupDescription: "其他：" ; Flags: unchecked

[Dirs]
; 开箱零插件：安装包不内建任何插件 DLL，保持纯净轻量；运行时在此空目录按需从插件商店拉取
Name: "{app}\ltool-plugins"

[Files]
Source: "build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; "=" 是历史上一次 shell 重定向打错产生的垃圾文件；genicon.exe 是构建期生成图标的工具，
; 都别跟着 * 一起进包
; qjp2/qwebp/qtiff 是死插件：它们硬导入 jasper.dll / libwebp*.dll / tiff.dll，而这些
; 依赖在 vcpkg 树里根本没装，插件永远加载不了，干净系统上还会弹"缺少 xxx.dll"
; D3Dcompiler_47/dxcompiler/dxil/Qt6Svg/qsvg/qsvgicon/qtuiotouchplugin 见 CMakeLists 里
; windeployqt 的 --no-system-d*-compiler / --exclude-plugins，这里是挡历史遗留副本的第二道
; captures / recordings：截图/录像的默认落点就是 exe 旁边这两个文件夹，本机自测要是
; 手滑在 build\Release 里跑过一次 F1，别让那几张图跟着安装包出去
Source: "build\Release\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "=,genicon.exe,*.lib,*.exp,captures,recordings,data,D3Dcompiler_47.dll,dxcompiler.dll,dxil.dll,Qt6Svg.dll,qsvg.dll,qsvgicon.dll,qtuiotouchplugin.dll,qjp2.dll,qwebp.dll,qtiff.dll,qoffscreen.dll,ltool-plugins,ltool-plugins\*"

; VC++ 运行库（vcruntime140/msvcp140*）故意不打包：Qt6Core 等确实硬导入它们，但
; 缺了是"进程根本起不来"，而能弹出设置页的机器一定有；ISCC 是 32 位进程，
; System32 路径还会被重定向到 SysWOW64（那里没有 vcruntime140_1.dll）。
; 真遇到"找不到 MSVCP140.dll"的机器，再改用 {#WinDir}\Sysnative\ 取 64 位副本。

; 老版本装出去的无用文件：Inno 不会自动删掉已经不在包里的东西，得点名清理，
; 否则升级完安装目录还是 90MB
[InstallDelete]
Type: files; Name: "{app}\D3Dcompiler_47.dll"
Type: files; Name: "{app}\dxcompiler.dll"
Type: files; Name: "{app}\dxil.dll"
Type: files; Name: "{app}\Qt6Svg.dll"
Type: files; Name: "{app}\imageformats\qsvg.dll"
Type: files; Name: "{app}\iconengines\qsvgicon.dll"
Type: files; Name: "{app}\generic\qtuiotouchplugin.dll"
Type: files; Name: "{app}\genicon.exe"
Type: files; Name: "{app}\="
; 上一版把导入库和 .exp 一起收进了包（构建产物躺在 build\Release 里，插件目标每个都留一对），
; Inno 又不会自己删已经不在包里的东西 —— 升级完安装目录还留着那 20 个没用的文件。通配删一遍。
Type: files; Name: "{app}\*.lib"
Type: files; Name: "{app}\*.exp"
Type: files; Name: "{app}\imageformats\qjp2.dll"
Type: files; Name: "{app}\imageformats\qwebp.dll"
Type: files; Name: "{app}\imageformats\qtiff.dll"

[Icons]
Name: "{autoprograms}\{#MyAppName}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
    ValueName: "ltool"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; 退出托盘实例由 CloseApplications/系统重启管理器处理，无需额外命令

[Messages]
SelectDirDesc=ltool 安装位置
SelectDirLabel3=安装程序将把 ltool 安装到以下文件夹。
WelcomeLabel2=这将安装 ltool——uTools 风格的快速启动器：%nAlt+Space 呼出搜索框，应用/文件/网站直达，开发环境与定时任务管理。

