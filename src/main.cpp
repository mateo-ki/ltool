#include "AppScanner.h"
#include "platform/ProcInfo.h"
#include "platform/AppsFolder.h"
#include "platform/FileMeta.h"
#include "platform/PluginNames.h"
#include "platform/SysConfig.h"
#include "QRegularExpression"
#include "QCoreApplication"
#include "platform/Autostart.h"
#include "platform/Paths.h"
#include "platform/Launch.h"
#include "AdBlocker.h"
#include "CaptureHistory.h"
#include "ConfigPage.h"
#include "ClipboardStore.h"
#include "DevEnv.h"
#include "ToolsPage.h"
#include "PluginHost.h"
#include "PluginStore.h"
#include "AboutPage.h"
#include <QPixmap>
#include <algorithm>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include "ltool-plugin.h"
#include "DiskScan.h"
#include "FileIndexer.h"
#include "FileOps.h"
#include "Archives.h"
#include "Pinyin.h"
#include "ShellMenu.h"
#include "SysTools.h"
#include "Theme.h"
#include "RegClean.h"
#include "Firewall.h"
#include "StartupAudit.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "SilentInstallLog.h"
#include "TaskScheduler.h"

#include "UsnIndex.h"
#include "Utils.h"
#include "WebShortcuts.h"

#include <QApplication>
#include <clocale>
#include <QAbstractNativeEventFilter>
#include <QDir>
#include <QDirIterator>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QToolButton>
#include <QMouseEvent>
#include <QLineEdit>
#include <QThread>
#include <QFrame>
#include <QRegularExpression>
#include <QPushButton>
#include <QRadioButton>
#include <QGroupBox>
#include <QLayout>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabWidget>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QPointer>
#include <QProcess>
#include <QEventLoop>
#include <QImage>
#include <QKeySequence>
#include <QMap>
#include <QPainter>
#include <QTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPalette>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QVector>

#include <functional>
#include <cmath>

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>

namespace {

constexpr WPARAM kHotkeyId = 1;
constexpr WPARAM kCaptureId = 2;
// F2 录屏。热键注册留在这层薄壳里（它必须是那个还能起得来的进程），按下之后把动作名
// "record" 交给 PluginHost::invoke —— 2026-10-15 起，录屏本体在 plugins/record。
constexpr WPARAM kRecordId = 3;

// 资源管理器右键能进来的那几个旗标（ShellMenu 往注册表写的就是这份）。
// 主程序只负责把"旗标去掉两个短横"当动作名转给插件，所以这张表**同时是 --plugintest 的判据**：
// 少一个动作没人声明 = 右键点下去什么都不发生，那条最难查，不能靠人肉对齐两处写法。
const char* const kShellFlags[] = { "--shred", "--fileinfo", "--whohas", "--extract",
                                    "--extract-sub", "--compress-zip", "--compress-7z" };

// 托盘图标的"安装进行中"动画帧：原图标右下角盖一小块深色圆，圆里转一段弧。
// 预渲染好轮播，避免每次 tick 都重画一遍 base（托盘 setIcon 本身就够贵了）
QVector<QIcon> spinnerFrames(const QIcon& base)
{
    static constexpr int kFrames = 12;
    const QPixmap src = base.pixmap(64, 64);
    QVector<QIcon> frames;
    frames.reserve(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawPixmap(0, 0, src);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(23, 24, 28));
        p.drawEllipse(QRectF(33, 33, 30, 30));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(94, 200, 120), 4, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(QRectF(36, 36, 24, 24), (360 - i * 30) * 16, 110 * 16);
        frames.append(QIcon(pm));
    }
    return frames;
}

class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyFilter(std::function<void(WPARAM)> fn) : m_fn(std::move(fn)) {}

    bool nativeEventFilter(const QByteArray&, void* message, qint64* result) override
    {
        MSG* msg = static_cast<MSG*>(message);
        if (msg && msg->message == WM_HOTKEY
            && (msg->wParam == kHotkeyId || msg->wParam == kCaptureId
            || msg->wParam == kRecordId)) {
            m_fn(msg->wParam);
            if (result)
                *result = 0;
            return true;
        }
        return false;
    }

private:
    std::function<void(WPARAM)> m_fn;
};

// QKeySequence → RegisterHotKey 的 (修饰键, 虚拟键)
bool keySeqToHotkey(const QKeySequence& ks, UINT* mods, UINT* vk)
{
    if (ks.count() != 1)
        return false;
    const QKeyCombination combo = ks[0];
    const Qt::KeyboardModifiers m = combo.keyboardModifiers();
    const Qt::Key key = combo.key();
    UINT mm = 0;
    if (m & Qt::ControlModifier) mm |= MOD_CONTROL;
    if (m & Qt::AltModifier)     mm |= MOD_ALT;
    if (m & Qt::ShiftModifier)   mm |= MOD_SHIFT;
    if (m & Qt::MetaModifier)    mm |= MOD_WIN;
    UINT k = 0;
    if (key == Qt::Key_Space) k = VK_SPACE;
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) k = UINT(VK_F1) + UINT(key - Qt::Key_F1);
    else if (key >= Qt::Key_A && key <= Qt::Key_Z) k = UINT('A') + UINT(key - Qt::Key_A);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) k = UINT('0') + UINT(key - Qt::Key_0);
    else return false;
    if (!mm && !(key >= Qt::Key_F1 && key <= Qt::Key_F24))
        return false; // 必须带修饰键（F 键除外）
    *mods = mm;
    *vk = k;
    return true;
}

// --smoke：无界面自检，扫描应用 + 检测插件 + 建立文件索引后打印结果退出
int runSmokeTest()
{
    // 诊断：打印各扫描根目录与 .lnk 数量
    struct FolderCheck { const char* label; Platform::KnownDir kf; };
    static const FolderCheck folders[] = {
        {"desktop", Platform::KnownDir::UserDesktop},
        {"publicDesktop", Platform::KnownDir::SharedDesktop},
        {"userPrograms", Platform::KnownDir::UserApps},
        {"commonPrograms", Platform::KnownDir::SystemApps},
    };
    for (const FolderCheck& fc : folders) {
        const QString dir = Platform::knownDir(fc.kf);
        const int lnkCount = QDir(dir).entryList({"*.lnk"}, QDir::Files).size();
        std::fprintf(stdout, "folder %s: %s exists=%d lnk=%d\n",
                     fc.label, dir.toUtf8().constData(),
                     QDir(dir).exists() ? 1 : 0, lnkCount);
    }

    const auto apps = AppScanner::scan();
    AppScanner::saveIndex(apps);
    int withIcon = 0;
    for (const AppEntry& a : apps)
        if (!a.iconFile.isEmpty())
            ++withIcon;
    std::fprintf(stdout, "apps=%zu icons=%d\n", size_t(apps.size()), withIcon);

    auto plugins = DevEnv::loadPlugins();
    for (DevPlugin& p : plugins) {
        DevEnv::detect(&p);
        int okCount = 0;
        for (const DevItem& it : p.items)
            if (it.ok)
                ++okCount;
        std::fprintf(stdout, "plugin %s: %d/%zu ok\n",
                     p.id.toUtf8().constData(), okCount, size_t(p.items.size()));
    }

    // 无管理员权限（如 CI 干净环境）无法使用 USN，避免全盘深扫描耗时数分钟触发超时。
    // 2026-10-25：这里改过真实用户配置 —— smoke 拿默认数据目录跑时把用户的
    // index/deep 直接写成 false（本机实测中招）。只有跑在 LTOOL_DATA_DIR 沙箱里
    // （带 --xxx 自测旗标那种）才允许落这个开关；裸 --smoke 只在内存里跳过深扫。
    {
        UsnIndex::VolumeHandle vol;
        const bool hasUsn = vol.open(QStringLiteral("C:/"));
        if (hasUsn)
            vol.close();
        const bool hasSnapshot = QFileInfo::exists(Utils::dataDir() + "/deepindex.dat");
        if ((!hasUsn || !hasSnapshot) && qEnvironmentVariableIsSet("LTOOL_DATA_DIR")) {
            QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
            cfg.setValue(QStringLiteral("index/deep"), false);
            cfg.sync();
        }
        // 裸 --smoke（没沙箱）：无管理员或尚无快照时不写配置，只把本次全盘深扫跳掉 ——
        // CI 干净机器没有快照，跑全盘重建必撞 30s files=TIMEOUT 并在退出时卡死线程池（240s exit 124）。
        if ((!hasUsn || !hasSnapshot) && !qEnvironmentVariableIsSet("LTOOL_DATA_DIR"))
            qputenv("LTOOL_SMOKE_SKIP_DEEP", "1");
    }

    FileIndexer::instance().reindexAsync(false); // 与生产一致：增量优先
    QObject::connect(&FileIndexer::instance(), &FileIndexer::indexingFinished,
                     [](int total) {
                         std::fprintf(stdout, "files=%d\n", total);
                         std::fflush(stdout);
                         QApplication::quit();
                     });
    QTimer::singleShot(30000, [] { std::fprintf(stdout, "files=TIMEOUT\n"); QApplication::quit(); });
    const int ret = QApplication::exec();
    std::fflush(stdout);
    return ret;
}

// --regscan：把注册表分析的结论原样打出来，用来人工核对有没有误报
int runRegScan()
{
    QElapsedTimer t;
    t.start();
    const QVector<RegIssue> quick = RegClean::quickScan();
    const qint64 quickMs = t.elapsed();
    const QVector<RegIssue> all = RegClean::scan();
    std::printf("quick=%d in %lldms  full=%d in %lldms\n", int(quick.size()), quickMs,
                int(all.size()), t.elapsed());
    QMap<QString, int> byCat;
    for (const RegIssue& it : all)
        byCat[RegClean::categoryLabel(it)]++;
    for (auto i = byCat.constBegin(); i != byCat.constEnd(); ++i)
        std::printf("  cat %s = %d\n", i.key().toUtf8().constData(), i.value());
    for (const RegIssue& it : all) {
        std::printf("[%s|%s] %s || %s || %s\\%s @%s\n",
                    RegClean::categoryLabel(it).toUtf8().constData(),
                    it.deletable() ? "del" : "ro", it.title.toUtf8().constData(),
                    it.detail.toUtf8().constData(), it.hiveName.toUtf8().constData(),
                    it.subKey.toUtf8().constData(), it.valueName.toUtf8().constData());
    }
    std::fflush(stdout);
    return 0;
}

// --startupscan：把开机启动分析的结论原样打出来，用来人工核对
int runStartupScan()
{
    QElapsedTimer t;
    t.start();
    const QVector<StartupItem> items = StartupAudit::scan();
    std::printf("items=%d in %lldms\n", int(items.size()), t.elapsed());
    QMap<QString, int> bySrc;
    for (const StartupItem& it : items)
        bySrc[StartupAudit::sourceLabel(it.source)]++;
    for (auto i = bySrc.constBegin(); i != bySrc.constEnd(); ++i)
        std::printf("  src %s = %d\n", i.key().toUtf8().constData(), i.value());
    for (const StartupItem& it : items) {
        std::printf("[%s] %s || sig=%d/%d %s || missing=%d enabled=%d || %s || %s || %s\n",
                    StartupAudit::sourceLabel(it.source).toUtf8().constData(),
                    it.name.toUtf8().constData(), it.hasSignature ? 1 : 0,
                    it.signatureValid ? 1 : 0, it.publisher.toUtf8().constData(),
                    it.targetMissing ? 1 : 0, it.enabled ? 1 : 0,
                    it.targetPath.toUtf8().constData(), it.location.toUtf8().constData(),
                    it.note.toUtf8().constData());
    }
    std::fflush(stdout);
    return 0;
}

// --fw <exe>：读一遍防火墙规则，核对枚举是否真的读到了（0 条要能区分"没有"和"没读到"）
int runFwProbe(const QStringList& args)
{
    QStringList a = args.mid(args.indexOf("--fw") + 1);
    a.removeAll(QStringLiteral("--fw"));
    const QString exe = a.join(QLatin1Char(' '));
    const Firewall::Match m = Firewall::rulesFor(exe.isEmpty() ? QStringLiteral("__none__") : exe);
    std::printf("rules seen=%d  match.any=%d  match.allowIn=%d  names=%s\n", m.total, m.any ? 1 : 0,
                m.allowIn ? 1 : 0, m.names.join(" | ").toUtf8().constData());
    std::fflush(stdout);
    return 0;
}

// --uptime：无界面地把"软件"菜单那一行的两个页面各构造一次，量出卡顿在哪一段
// --adscan：量一次"广告拦截每 5 秒在 GUI 线程上做的事"到底有多贵（无界面）
// 跑某个插件自带的判据（插件里导出 extern "C" 的那个函数，名字由调用方点名）。
// 页面/动作代码搬进 DLL 之后主程序链接不到它内部的测试函数，只能这样要符号。
// 找不到符号**必须算失败**，不能当通过 —— 否则"插件没加载起来"会伪装成"测试全绿"。
//
// arg / ms / text：判据入口的签名不止一种，按那面旗标原来的写法给 ——
//   None            → int()                    （disk / user 那一类，只返回失败数）
//   Ms              → int(int)                 （--recprobe 的毫秒、--rectest/--recordui 的秒）
//   MsAndText       → int(int, const char*)    （--memloop 的轮数 + 「宽x高」）
//   Text            → int(const char*)         （截图那族收一个图片路径）
// flag：非空 = 判据**自己**已经打了最后那行 "<flag>: 失败 N 项"（record / capture 那几条旗标就是这么写的），
//        这儿不能再打一遍（同一次跑出两个失败数就没法看了）；空 = 按老样子用插件 id 补那一行。
// badOut：非空 = 把判据返回的那个**原始失败数**带回去。--capgeo / --capflash 的退出码就是这个数
//        （不是"有没有失败"的 0/1），调用方要在事件循环结束后拿它当进程退出码。
enum class SelftestArg { None, Ms, MsAndText, Text };

    // 按 accessibleName 认控件：findChild<T*>() 的顺序是对象树顺序，这一墙里现在挂着好几张
    // 插件页，它们的工具栏里也有 QToolButton —— 猜错一颗就是"判据替人按下了一次真动作"。
    template <class T>
    static T* findChildByAccessible(QWidget* root, const QString& name)
    {
        const auto kids = root->findChildren<T*>();
        for (T* w : kids)
            if (w->accessibleName() == name)
                return w;
        return nullptr;
    }

int runPluginSelftest(const QString& id, const char* symbol = "ltoolPluginSelftest",
                      SelftestArg arg = SelftestArg::None, int ms = 0,
                      const QString& text = QString(), const char* flag = nullptr,
                      int* badOut = nullptr)
{
    void* raw = PluginHost::entry(id, symbol);
    if (!raw) {
        // "插件根本没起来"和"起来了但没导出这个符号"是两种坏法，分开说才有人能照着修
        if (!PluginHost::plugin(id))
            std::printf("FAIL %s：插件没能加载（%s 里没有能用的 %s —— 安装包少文件了？）\n",
                        id.toUtf8().constData(),
                        PluginHost::pluginsDir().toUtf8().constData(),
                        Platform::pluginFileName(id).toUtf8().constData());
        std::printf("FAIL %s：没有可用的判据入口（插件没加载起来，或没导出 %s）\n",
                    id.toUtf8().constData(), symbol);
        std::printf("%s: 失败 1 项\n", flag ? flag : id.toUtf8().constData());
        std::fflush(stdout);
        if (badOut)
            *badOut = 1;
        return 1;
    }
    int bad = 0;
    switch (arg) {
    case SelftestArg::None:
        bad = reinterpret_cast<int (*)()>(raw)();
        break;
    case SelftestArg::Ms:
        bad = reinterpret_cast<int (*)(int)>(raw)(ms);
        break;
    case SelftestArg::MsAndText:
        bad = reinterpret_cast<int (*)(int, const char*)>(raw)(ms, text.toUtf8().constData());
        break;
    case SelftestArg::Text:
        bad = reinterpret_cast<int (*)(const char*)>(raw)(text.toUtf8().constData());
        break;
    }
    if (badOut)
        *badOut = bad;
    if (!flag) {
        std::printf("%s: 失败 %d 项\n", id.toUtf8().constData(), bad);
        std::fflush(stdout);
    }
    return bad == 0 ? 0 : 1;
}

int runAdScan()
{
    for (int pass = 0; pass < 3; ++pass) {
        QElapsedTimer t;
        t.start();
        const auto hits = AdBlocker::instance().scan();
        std::printf("scan pass %d: %d windows, %lld ms\n", pass, int(hits.size()),
                    t.nsecsElapsed() / 1000000);
        std::fflush(stdout);
    }
    return 0;
}

int runUiTiming(const QStringList& args)
{
    auto timed = [](const char* tag, std::function<void()> f) {
        QElapsedTimer t;
        t.start();
        f();
        std::printf("%-20s %8lld ms\n", tag, t.nsecsElapsed() / 1000000);
        std::fflush(stdout);
    };
    const bool wantSoft = !args.contains("un");
    const bool wantUn = !args.contains("soft");
    // 这两张页现在是 plugins/software 里的实现，主程序够不着那两个类了 —— 和自测同一条件路：
    // 按名字向那个插件点名要一颗导出符号。拿不到就是**装出来的那份 dll 太旧**，要报出来不是当 0 ms。
    const auto makePage = [](const char* symbol) -> QWidget* {
        using PageFn = QWidget* (*)();
        auto* fn = reinterpret_cast<PageFn>(PluginHost::entry(QStringLiteral("software"), symbol));
        if (!fn) {
            std::printf("%-20s 没有导出 %s（插件是旧的，这个量不到）\n", "缺符号", symbol);
            return nullptr;
        }
        return fn();
    };
    QWidget* soft = nullptr;
    QWidget* un = nullptr;
    timed("SoftwarePage ctor", [&] {
        if (!wantSoft)
            return;
        soft = makePage("ltoolSoftwarePage");
        if (soft)
            soft->setAttribute(Qt::WA_DontShowOnScreen);
    });
    timed("SoftwarePage show", [&] { if (soft) soft->show(); });
    timed("UninstallPage ctor", [&] {
        if (!wantUn)
            return;
        un = makePage("ltoolUninstallPage");
        if (un)
            un->setAttribute(Qt::WA_DontShowOnScreen);
    });
    timed("UninstallPage show", [&] { if (un) un->show(); });
    {
        QElapsedTimer t;
        t.start();
        QEventLoop loop;
        QTimer::singleShot(2500, &loop, &QEventLoop::quit);
        loop.exec();
        std::printf("%-20s %8lld ms (slices drained)\n", "event loop", t.nsecsElapsed() / 1000000);
    }
    const Firewall::Match m = Firewall::rulesFor(QStringLiteral("D:\\software\\ltool\\ltool.exe"));
    std::printf("rulesFor           %8d rules\n", m.total);
    std::fflush(stdout);
    delete soft;
    delete un;
    return 0;
}


// --fwtry <exe> <显示名>：跑一遍页面上"加入允许列表"按钮走的同一条代码（含提权与回读）
int runFwTry(const QStringList& args)
{
    QStringList rest = args.mid(args.indexOf(QStringLiteral("--fwtry")) + 1);
    rest.removeAll(QStringLiteral("--fwtry"));
    const QString exe = rest.isEmpty() ? QString() : rest.first();
    const QString name = rest.size() > 1 ? rest.at(1) : QFileInfo(exe).completeBaseName();
    const QString out = Firewall::allowExe(exe, name);
    std::printf("allowExe -> %s\n", out.toUtf8().constData());
    std::fflush(stdout);
    return 0;
}

// --fwallow <exe> <规则名> / --fwremove <规则名>：只做一次 COM 写入。
// 这是给"被 UAC 提权拉起来的自己"用的入口（软件列表里点"加入允许列表"时自家人叫自家人），
// 也方便命令行核对权限行为。
int runFwWrite(const QStringList& args)
{
    const bool remove = args.contains(QStringLiteral("--fwremove"));
    const QString flag = remove ? QStringLiteral("--fwremove") : QStringLiteral("--fwallow");
    QStringList rest = args.mid(args.indexOf(flag) + 1);
    rest.removeAll(flag);
    const QString a0 = rest.isEmpty() ? QString() : rest.first();
    const QString a1 = rest.size() > 1 ? rest.at(1) : QString();
    const bool ok = remove ? Firewall::removeRuleNow(a0)
                           : Firewall::addRuleNow(a0, a1.isEmpty() ? QFileInfo(a0).completeBaseName()
                                                                   : a1);
    std::printf("fw%s -> %s\n", remove ? "remove" : "allow", ok ? "ok" : "failed");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

// --searchtime a b c：加载索引后逐个查询并计时（无界面），用于定位搜索框输入卡顿
int runSearchTiming(const QStringList& queries)
{
    FileIndexer::instance().reindexAsync(false);
    QObject::connect(&FileIndexer::instance(), &FileIndexer::indexingFinished,
                     [queries](int total) {
                         std::fprintf(stdout, "index=%d entries\n", total);
                         for (const QString& q : queries) {
                             QElapsedTimer t;
                             t.start();
                             const auto hits = FileIndexer::instance().search(q, 8);
                             std::fprintf(stdout, "search %-14s %7lld ms hits=%d\n",
                                          q.toUtf8().constData(),
                                          t.nsecsElapsed() / 1000000, int(hits.size()));
                             for (int i = 0; i < hits.size() && i < 3; ++i)
                                 std::fprintf(stdout, "    #%d %s\n", i + 1,
                                              hits[i].path.toUtf8().constData());
                         }
                         std::fflush(stdout);
                         QApplication::quit();
                     });
    QTimer::singleShot(180000, [] { std::fprintf(stdout, "searchtime=TIMEOUT\n");
                                   QApplication::quit(); });
    return QApplication::exec();
}

// --searchdump a b c：把每个词的完整结果列表按顺序打出来。
// 改搜索实现（分桶/并行/跳字节）最怕的是"快了但结果顺序变了"，所以这条就是
// 改前改后各跑一遍、拿 diff 当判据的入口 —— diff 空才算过。
int runSearchDump(const QStringList& queries)
{
    FileIndexer::instance().reindexAsync(false);
    QObject::connect(&FileIndexer::instance(), &FileIndexer::indexingFinished,
                     [queries](int total) {
                         std::fprintf(stdout, "index=%d entries\n", total);
                         for (const QString& q : queries) {
                             const auto hits = FileIndexer::instance().search(q, 20);
                             std::fprintf(stdout, "=== %s hits=%d\n", q.toUtf8().constData(),
                                          int(hits.size()));
                             for (int i = 0; i < hits.size(); ++i)
                                 std::fprintf(stdout, "%2d %s%s | %s\n", i,
                                              hits[i].isDir ? "d" : "f",
                                              hits[i].name.toUtf8().constData(),
                                              hits[i].path.toUtf8().constData());
                         }
                         std::fflush(stdout);
                         QApplication::quit();
                     });
    QTimer::singleShot(180000, [] { std::fprintf(stdout, "searchdump=TIMEOUT\n");
                                    QApplication::quit(); });
    return QApplication::exec();
}

// --disktime：A/B 两种磁盘遍历在 C: 上的耗时（旧 QDirIterator vs 新 DiskScan），
// 用来回答"占用分析为什么卡、改完快了多少"
int runDiskTiming()
{
    QElapsedTimer t;
    std::printf("DiskScan (FindFirstFileEx + 并行 + 一次建树)…\n");
    std::fflush(stdout);
    t.start();
    QStringList cleanPaths;
    for (const DiskScan::CleanTarget& c : DiskScan::cleanTargets())
        cleanPaths << c.path;
    const QVector<qint64> sizes = DiskScan::sizesFor(cleanPaths);
    qint64 cleanTotal = 0;
    for (qint64 v : sizes)
        cleanTotal += v;
    const qint64 quick = t.elapsed();
    // 这一段决定用户点「整理」后多久能看到第一个 tab 有结果，比整盘耗时更影响体感
    std::printf("  可清理位置（%d 项）: %lld ms  合计 %.2f GB\n", int(cleanPaths.size()), quick,
                cleanTotal / 1073741824.0);
    std::fflush(stdout);
    t.start();
    const DiskScan::Tree tree = DiskScan::run("C:/", 1 << 20);
    const qint64 fastCold = t.elapsed();
    // 三段分开报，不然"太慢了"只能靠猜：扫盘 / 合并+建树 / 排序剪枝
    const DiskScan::Timing tm = DiskScan::lastTiming();
    std::printf("  新（第一趟，目录项多半是冷的）: %lld ms  目录节点=%d 合计=%.2f GB\n",
                fastCold, int(tree.name.size()), tree.total / 1073741824.0);
    std::printf("      分段：扫盘 %lld ms、合并建树 %lld ms、排序剪枝 %lld ms"
                "（%d 个任务 / %d 线程，扫过 %lld 个目录 / %lld 个文件）\n",
                tm.walkMs, tm.mergeMs, tm.sortMs, tm.jobs, tm.threads, tm.dirsSeen, tm.files);
    for (int i = tree.firstKid[0]; i < tree.firstKid[0] + tree.kidCount[0] && i < tree.kids.size(); ++i) {
        const int k = tree.kids[i];
        std::printf("      %-28s %8.2f GB\n", tree.name[k].toUtf8().constData(),
                    tree.size[k] / 1073741824.0);
        if (i - tree.firstKid[0] >= 4)
            break;
    }
    std::fflush(stdout);

    // A/B 必须两趟都跑在**同一种缓存状态**上：先前后面旧，旧的那趟吃的是新那趟焐热的缓存，
    // 于是量出"新比旧慢 0.7x"这个假结论（本机实测过：同一棵树 32 秒 / 13.9 秒，差别全在冷缓存）。
    // 所以现在是 冷新 → 冷旧 → 热旧 → 热新，两两同状态才比。
    t.restart();
    const qint64 slowCold = DiskScan::legacyWalk("C:/");
    const qint64 slowColdMs = t.elapsed();
    std::printf("  旧（冷）: %lld ms\n", slowColdMs);
    t.restart();
    const qint64 slowWarm = DiskScan::legacyWalk("C:/");
    const qint64 slowWarmMs = t.elapsed();
    t.restart();
    const DiskScan::Tree warm2 = DiskScan::run("C:/", 1 << 20);
    const qint64 fastWarm = t.elapsed();
    std::printf("  同一棵树两趟同缓存比：冷 %lld(新) / %lld(旧) ms，热 %lld(新) / %lld(旧) ms"
                "  → 热态提速 %.1fx\n",
                fastCold, slowColdMs, fastWarm, slowWarmMs,
                slowWarmMs > 0 ? double(slowWarmMs) / double(fastWarm) : 0.0);
    std::printf("  合计对不对得上：新 %.2f GB、旧 %.2f GB、热的再扫一遍 %.2f GB\n",
                tree.total / 1073741824.0, slowWarm / 1073741824.0,
                warm2.total / 1073741824.0);
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // libarchive 在 UTF-8 和窄字符之间转文件名时用的是 C 运行时 locale，而 Qt 不会去设它：
    // 默认 "C" 下任何非 ASCII 名字都转不过去，压缩带中文名的东西会直接失败。
    setlocale(LC_CTYPE, "");
    // --pindel 会真删 captures 目录里的往期截图，--ocrchain 要验"引擎还没下载"那条链（会真下 26MB），
    // --pathcfg 会写 config.ini 并建目录，--appdirs 会写 apps/roots 并扫盘，
    // --storetest 会写 store/indexUrl 这一项（它就是验"配置里改了地址就跟着走"），
    // --storeexit 会写 store/pendingRemove（那一条判据走的就是"卸载排进待删表"那段），
    // --linuxdotest 会写 linuxdo/* 这一节（间隔、关键词、中转前缀、上次看到哪条）：
    // --startuptest 要把那张页建起来跑一遍（它导出报告时写 dataDir，所以也一并挪走；
    //   它真写注册表那几条走的是 HKCU 下自造的临时键 Software\ltool-startuptest-<pid>，跑完删掉）。
    // 这几种都把数据目录整个挪到临时目录再跑，碰不到真实的用户数据。
    // 挪的是 `LTOOL_DATA_DIR`（dataDir() 的第一优先档）—— 以前挪的是 APPDATA，
    // 而 1.4.0 之后数据默认落在**软件目录下的 data\**，挪 APPDATA 就管不着它了。
    // 必须在第一次 dataDir() 之前设好。
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--pindel") || a == QLatin1String("--ocrchain")
            || a == QLatin1String("--pathcfg") || a == QLatin1String("--pintool")
            || a == QLatin1String("--appdirs") || a == QLatin1String("--cfgcheck")
            || a == QLatin1String("--storetest") || a == QLatin1String("--storeexit")
            || a == QLatin1String("--plugintest")        // ← 2026-10-21 丢的那 24 条网站关键字就是它漏的：
                                                         //    deliver 的 installed 那一栏跑的就是这一面旗，
                                                         //    没沙箱 ⇒ 它往**还没搬**的数据目录播了默认 webs.json
            || a == QLatin1String("--linuxdotest")
            || a == QLatin1String("--linuxdonet") || a == QLatin1String("--cliptest")
            || a == QLatin1String("--randtest")
            || a == QLatin1String("--startuptest")) {
            const QString sandbox = QDir::tempPath() + "/" + a.mid(2);
            QDir(sandbox).removeRecursively();
            QDir().mkpath(sandbox);
            qputenv("LTOOL_DATA_DIR", sandbox.toUtf8());
            // 截图/录像的默认落点现在是 exe 旁边（软件目录），挪 APPDATA 管不到它。
            // 会往往期里写东西的那两个自测改用配置项把两个目录也指进沙箱 ——
            // 不然它们会在 build\Release 里留下 captures\，而那会被安装包收走。
            if (a == QLatin1String("--pindel") || a == QLatin1String("--pintool")) {
                QSettings cfg(sandbox + "/config.ini", QSettings::IniFormat);
                cfg.setValue(QStringLiteral("paths/captures"),
                             QDir::toNativeSeparators(sandbox + "/captures"));
                cfg.setValue(QStringLiteral("paths/recordings"),
                             QDir::toNativeSeparators(sandbox + "/recordings"));
                cfg.sync();
            }
            break;
        }
    }
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        const PVOID addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        Utils::log(QStringLiteral("FATAL CRASH: code=0x%1 addr=%2")
                       .arg(QString::number(code, 16).toUpper())
                       .arg(reinterpret_cast<quintptr>(addr), 0, 16));
        // 再把这一趟的栈打出来：以前只能报到"崩在 Qt6Core 里读地址 8"这一格，
        // 而日志只说得出"当时正开着哪张页" —— 分不出是从哪一路掉进去的（2026-10-22 base64 那次就这样）。
        // 拿模块名 + RVA 打，之后可以喂给 build/verify/pe_rva.py 去问它旁边是哪个导出。
        // 刻意只用固定缓冲区 + fromLatin1：异常处理里再分配一次大对象，第二次崩会把线索一起吃掉。
        void* frames[12] = {};
        const USHORT n = CaptureStackBackTrace(0, 12, frames, nullptr);   // kernel32 那一版，不用链 ntdll.lib
        char line[160];
        for (USHORT i = 0; i < n; ++i) {
            HMODULE mod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(frames[i]), &mod) && mod) {
                wchar_t name[64] = L"?";
                if (GetModuleBaseNameW(GetCurrentProcess(), mod, name, 62) > 0) {
                    const auto rva = reinterpret_cast<unsigned long long>(frames[i]) - reinterpret_cast<unsigned long long>(mod);
                    _snprintf_s(line, sizeof line, _TRUNCATE, "CRASH FRAME %02u %ls+0x%llx",
                                i, name, rva);
                } else {
                    _snprintf_s(line, sizeof line, _TRUNCATE, "CRASH FRAME %02u 0x%llx",
                                i, reinterpret_cast<unsigned long long>(frames[i]));
                }
            } else {
                _snprintf_s(line, sizeof line, _TRUNCATE, "CRASH FRAME %02u 0x%llx",
                            i, reinterpret_cast<unsigned long long>(frames[i]));
            }
            Utils::log(QString::fromLatin1(line));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
    QApplication app(argc, argv);
    // 数据目录搬家：老 %APPDATA%\ltool → 软件目录下的 data\（他 2026-10-21 点名"不要放在 C 盘"）。
    // **只在"正常启动"那一次做**（命令行里没有任何 `--` 旗标）：否则我拿构建目录那份 exe 跑一条
    // 自测，就会先把 378 MB 真实数据复制进 build\Release\data。旧目录一律原样留着不删。
    // 坑（本机真崩过，0xC0000005）：`QCoreApplication::arguments()` **按值返回 QStringList**
    //（Qt 6.10 头里就写着 `static QStringList arguments();`），所以 `app.arguments().cbegin()` 和
    // `app.arguments().cend()` 是**两个临时容器**上的指针。不带旗标时头指针正好越过末尾，
    // 于是解引用别人家的堆；带任意 `--` 旗标反而不炸（predicate 第一个元素就命中，压根没走这个坏区间）——
    // 这就是为什么所有离线判据全绿、双击图标却必崩。盯着它的判据在 `--pathcfg` 最后一条：起一个**不给旗标**的子进程。
    const QStringList args = app.arguments();
    if (std::none_of(args.cbegin() + 1, args.cend(),
                     [](const QString& a) { return a.startsWith(QLatin1String("--")); })) {
        QString moved;
        const int movedN = Utils::migrateLegacyDataDir(&moved);
        if (!moved.isEmpty())
            Utils::log(QStringLiteral("data: %1（搬了 %2 个文件）").arg(moved).arg(movedN));
    }
    QApplication::setOrganizationName("ltool");
    QApplication::setApplicationName("ltool");
    QApplication::setApplicationDisplayName("ltool");
    QApplication::setWindowIcon(Utils::appIcon());
    // 常驻托盘：最后一个窗口关闭（比如关掉设置页）不能退出程序
    app.setQuitOnLastWindowClosed(false);
    Theme::applyToApp(app);   // 夜间/日间（默认夜间）：调色板 + qApp 那份悬停说明样式表都在里面

    Utils::ensureDirs();
    // 卸载排着队的先落地：上一轮点过「卸载」但当时这个 dll 还加载在进程里（Windows 不放行），
    // 此刻谁都还没加载过插件，是唯一能真把它删干净的时机。
    PluginStore::applyPendingRemoves();
    DevEnv::ensureDefaultPlugins();
    WebShortcuts::ensureDefaultWebs();

    const bool smoke = args.contains("--smoke");
    const bool showOnStart = args.contains("--show");
    const bool openSettingsOnStart = args.contains("--settings") || args.contains("--settingspage");
    const bool mftTest = args.contains("--mfttest");
    const bool searchTime = args.contains("--searchtime");

    // --smoke 必须在单实例探针**之前**派发。它原先排在后面，于是本机已有一个在跑的
    // 实例时，探针连上去把请求转交、当场 return 0 —— 输出 0 字节 + 退出码 0，
    // 看起来像"自检通过"，其实一行都没跑（2026-10-14 撞见：交付闭环里这条是哑的）。
    if (smoke)
        return runSmokeTest();

    if (mftTest) {
        // 「MFT 直读」这一路的判据。提权与否跑的是两套断言：
        //   读得了卷 → 真走一遍，并和遍历那条路逐项目对数（$MFT 的大小要是解析错了，
        //              两边总数一定对不上 —— 这就是这格数字的正确性证明）
        //   读不了卷 → 点名 SKIP，只断言回退那条路 + 原因带没带出来
        // 不这么分的话，非提权环境里这一串会全绿但其实一行都没测（--storenet 那 4 条就这么骗过人）。
        int bad = 0;
        const auto has = [&bad](const char* name, bool ok) {
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok)
                ++bad;
        };
        const QString root = QStringLiteral("C:/");
        QString why;
        const bool usable = DiskScan::mftUsable(root, &why);
        std::printf("INFO mftUsable(C:/)=%d why=[%s]\n", usable ? 1 : 0, why.toUtf8().constData());
        has("说不可用时一定带人话原因", usable || !why.isEmpty());
        has("说可用时不掺原因", !usable || why.isEmpty());
        // 3 = ERROR_PATH_NOT_FOUND：卷路径结尾多带一个反斜杠就是这个数（2026-10-17 实测，
        // 深索引的"MFT 优先"因此从来没生效过）。差提权应该是 5，不是 3。
        has("打不开卷的原因不是「路径不存在」（卷路径结尾不带反斜杠）",
            usable || !why.contains(QStringLiteral("错误码 3")));
        has("子目录不会被当成卷根（MFT 只认整盘）", !DiskScan::mftUsable(root + "Windows", &why));

        // ---- 快照文件往返：这一组不依赖提权，两边都能真跑 ----
        const DiskScan::Tree tiny = DiskScan::run(root + "Windows/System32/drivers", 1 << 20);
        has("小目录树建出来了（写读这组的料）", !tiny.isEmpty() && tiny.total > 0);
        const QString snap = QDir::tempPath()
                             + QStringLiteral("/ltool-mfttest-%1.bin")
                                   .arg(QCoreApplication::applicationPid());
        QString werr, rerr;
        const bool wrote = DiskScan::writeTree(tiny, snap, &werr);
        const DiskScan::Tree back = wrote ? DiskScan::readTree(snap, &rerr) : DiskScan::Tree();
        has("树写得出去也读得回来，节点数/总数/引擎一字不差",
            wrote && back.name.size() == tiny.name.size() && back.total == tiny.total
                && back.engine == tiny.engine);
        const int topKid = back.kidCount[back.root] > 0 ? back.firstKid[back.root] : -1;
        has("读回来的树按路径还能定位到同一个节点",
            topKid >= 0 && back.nodeForPath(back.pathOf(topKid)) == topKid);
        QFile::remove(snap);          // 只删这一个算出来的绝对路径
        {                             // 拿一份"看着像但不是"的文件试：格式对不上必须整份丢掉
            QFile junk(snap);
            const bool opened = junk.open(QIODevice::WriteOnly);
            if (opened) {
                junk.write("LTD1garbage-garbage");
                junk.close();
            }
            // 夹具写不出来就直接判红：这一组不许静默跳过
            has("格式对不上的快照一律整份丢掉（不猜）",
                opened && DiskScan::readTree(snap, &rerr).isEmpty());
        }
        QFile::remove(snap);

        if (usable) {
            // 先把 MFT 那两步**单独**量出来。理由：任何一步没成，DiskScan::run 就会掉进
            // "回退遍历"，而遍历在 CI 那台 VM 上要四分钟以上 —— 三次超时全是这么烧掉的
            // （run 39026813012 / 39027974581 / 39029141013，全是 exit 124，判据一条没错）。
            // 所以：两步不成就不跑整盘，直接判红点名是哪一步。
            UsnIndex::VolumeHandle vol;
            const bool opened = vol.open(root);
            UsnIndex::EnumResult er;
            QElapsedTimer t1;
            t1.start();
            const bool enumOk = opened && UsnIndex::enumVolumeMft(vol.vol, er);
            const qint64 enumMs = t1.elapsed();
            vol.close();
            QVector<quint64> want;
            for (int i = 0; i < er.frn.size(); ++i)
                if (!(er.attrs[i] & FILE_ATTRIBUTE_DIRECTORY))
                    want.append(er.frn[i]);
            QVector<qint64> got;
            QString sizeWhy;
            QElapsedTimer t2;
            t2.start();
            const bool sizeOk = enumOk && UsnIndex::mftSizes(root, want, got, {}, &sizeWhy, er.mftFrn);
            const qint64 sizeMs = t2.elapsed();
            qint64 sum = 0;
            qint64 nonzero = 0;
            for (const qint64 v : got) {
                sum += v;
                if (v > 0)
                    ++nonzero;
            }
            std::printf("INFO 直接量：opened=%d 枚举 %d 条/%lldms · 要问大小 %d 个文件，"
                        "真拿到非 0 的 %lld 个，合计 %lld/%lldms · %s\n",
                        opened ? 1 : 0, int(er.frn.size()), long long(enumMs), int(want.size()),
                        long long(nonzero), long long(sum), long long(sizeMs),
                        sizeWhy.toUtf8().constData());
            std::fflush(stdout);
            has("整盘记录枚举得出来（FSCTL_ENUM_USN_DATA）", enumOk && er.frn.size() > 1000);
            if (!sizeOk) {
                // 读不到 $MFT **不判红**：那是这台机器的 Windows 不让 —— CI 那台提权的 runner 实测
                // 三种名字写法 × 普通/备份语义全被拒、按记录号又拿不到序列号、按簇读能定位到 $MFT
                // 自己的记录但盘区段解不出来（原因整条印在下面，不藏）。产品侧在这种情况下走的是
                // 回退遍历，而"回退出数 + 界面说出为什么慢"那两条判据照样是红的就完蛋的硬判据。
                // 但也不许它静默腐烂：原因每轮都印出来，成功路径那几条全部点名跳过。
                std::printf("SKIP $MFT 大小这一档 + 整盘树 + 三条对账（读不到 $MFT，原因见上）\n");
                has("读不到 $MFT 也说得出卡在哪一格（不许只回一个 false）", !sizeWhy.isEmpty());
            } else {
                has("$MFT 的大小读得出来（不是靠回退蒙过去）", nonzero > want.size() * 9 / 10);
                has("读回来的大小合计像个真盘（1 GB 以上、且不是全 0）", sum > (1LL << 30));
                const DiskScan::Tree mft = DiskScan::run(root, 1 << 20, [](const DiskScan::Progress& p) {
                    std::printf("STAGE %s\n", p.current.toUtf8().constData());
                    std::fflush(stdout);
                });
                const DiskScan::Timing tm = DiskScan::lastTiming();
                std::printf("INFO MFT：%d 个目录 %lld 个文件 合计 %lld（枚举 %lldms + 大小 %lldms）\n",
                            int(mft.name.size()), long long(tm.files), long long(mft.total),
                            long long(tm.enumMs), long long(tm.sizeMs));
                std::fflush(stdout);
                has("能读卷时走的就是 MFT 那条路", mft.engine == DiskScan::engineMft());
                has("MFT 这条路拼得出目录树", mft.name.size() > 100 && mft.total > (1LL << 30));
                // 整盘合计和**文件系统自己报的"已用"**对一眼 —— 第三条完全独立的量法
                // （既不读 MFT 也不遍历）。容差只能给粗：两边口径天生不同（它按簇算分配量、
                // 还算 System Volume Information），压缩过的系统文件会让"逻辑"明显大于"分配"，
                // 硬链接与 ADS 又反向。这条只抓"差一个量级"，精确对账交给下面逐目录那条。
                const DiskScan::Usage u = DiskScan::usageOf(root);
                std::printf("INFO 文件系统报的已用 %lld，MFT 整盘合计 %lld\n", long long(u.used),
                            long long(mft.total));
                has("MFT 的整盘合计与文件系统报的已用在同一量级",
                    u.ok && mft.total >= u.used / 2 && mft.total <= u.used * 3);
                // 和遍历逐目录对账：**只走一个子树**（整盘走一遍遍历就是那四分钟）
                const QString probe = root + "Windows/System32";
                qputenv("LTOOL_DISK_ENGINE", "walk");
                const DiskScan::Tree walk = DiskScan::run(probe, 1 << 20);
                qunsetenv("LTOOL_DISK_ENGINE");
                const DiskScan::Timing tw = DiskScan::lastTiming();
                const int node = mft.nodeForPath(probe);
                std::printf("INFO 对账目录 %s：MFT %lld / 遍历 %lld（遍历 %lldms、%d 个目录）\n",
                            probe.toUtf8().constData(), node >= 0 ? long long(mft.size[node]) : -1,
                            long long(walk.total), long long(tw.walkMs), int(walk.name.size()));
                has("同一个目录（System32）两条路的大小对得上（±10%）",
                    node >= 0 && !walk.isEmpty() && mft.size[node] >= walk.total * 9 / 10
                        && mft.size[node] <= walk.total * 11 / 10);
            }
        } else {
            std::printf("SKIP 整盘树 + 两条对账（这台机器没提权，读不了卷）\n");
            const DiskScan::Tree t = DiskScan::run(root, 1 << 20);
            has("读不了卷照样出数（回退遍历）", t.engine == DiskScan::engineWalk() && !t.isEmpty());
            has("回退时把原因带回来了（界面要说为什么慢）", !t.engineNote.isEmpty());
        }
        std::printf("mft: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad;
    }

    // --mftree <盘> <输出文件>：**这是给提权了的自己调用的**，不是给人敲的。
    // 磁盘页的「直读 MFT 重扫」平时没管理员权限读不了卷，于是起一个提权的 ltool 跑这一条：
    // 只读整盘、把树写成一个文件、退出。退出码 0=写好了，2=这个卷走不了 MFT，1=别的失败。
    if (const int mi = args.indexOf(QStringLiteral("--mftree")); mi >= 0) {
        const QString root = mi + 1 < args.size() ? args[mi + 1] : QString();
        const QString out = mi + 2 < args.size() ? args[mi + 2] : QString();
        QString why;
        if (root.isEmpty() || out.isEmpty()) {
            std::fprintf(stdout, "用法：--mftree <盘符根> <输出文件>\n");
            return 1;
        }
        if (!DiskScan::mftUsable(root, &why)) {
            std::fprintf(stdout, "MFT_UNAVAILABLE %s\n", why.toUtf8().constData());
            std::fflush(stdout);
            return 2;
        }
        const DiskScan::Tree t = DiskScan::run(root, 1 << 20);
        const DiskScan::Timing tm = DiskScan::lastTiming();
        if (t.isEmpty() || t.engine != DiskScan::engineMft()) {
            std::fprintf(stdout, "MFT_FAILED engine=%s note=%s\n", t.engine.toUtf8().constData(),
                         t.engineNote.toUtf8().constData());
            std::fflush(stdout);
            return 2;
        }
        QString err;
        if (!DiskScan::writeTree(t, out, &err)) {
            std::fprintf(stdout, "MFT_WRITE_FAILED %s\n", err.toUtf8().constData());
            std::fflush(stdout);
            return 1;
        }
        std::fprintf(stdout, "MFT_OK dirs=%lld files=%lld total=%lld enum=%lldms size=%lldms\n",
                     long long(t.name.size()), long long(tm.files), long long(t.total),
                     long long(tm.enumMs), long long(tm.sizeMs));
        std::fflush(stdout);
        return 0;
    }

    if (searchTime) {
        QStringList qs = args.mid(args.indexOf("--searchtime") + 1);
        qs.removeAll("--searchtime");
        if (qs.isEmpty())
            qs << "no" << "notepad" << "chrome" << "de" << "use";
        return runSearchTiming(qs);
    }

    if (args.contains("--searchdump")) {
        QStringList qs = args.mid(args.indexOf("--searchdump") + 1);
        qs.removeAll("--searchdump");
        if (qs.isEmpty())
            qs << "no" << "notepad" << "chrome" << "de" << "use"
               << QString::fromUtf8("软件") << QString::fromUtf8("回收")
               << QString::fromUtf8("截图");
        return runSearchDump(qs);
    }

    if (args.contains("--disktime"))
        return runDiskTiming();


    // --rowmenu：磁盘页两栏列表的右键（只有界面在用，且真按一下就会拉起资源管理器，
    // 所以判据放在插件里 DiskPage.cpp，这里只负责点名跑它）。
    if (args.contains("--rowmenu"))
        return runPluginSelftest(QStringLiteral("disk"));

    // --username：验「工具 → Windows 用户名」这一页的判据。页面搬进 DLL 插件之后，
    // 判据跟着页一起进插件（主程序链接不到 UserAccountPage），这儿只负责点名跑它。
    // **一条都不真的去改账户/建账户** —— 那是不可逆级别的动作，只能在界面上由人按下。
    if (args.contains("--username"))
        return runPluginSelftest(QStringLiteral("user"));

    // --gridtest：「图片排版」那张板的判据（宫格数、拖进来收不收、换顺序是移动还是交换、
    // 合成出来的尺寸和像素对不对）。全在 plugins/grid/gridselftest.cpp 里，主程序只点名。
    // 合成只在临时目录里算，产出文件那条判据自己算路径自己删，不碰他的截图目录。
    if (args.contains("--gridtest"))
        return runPluginSelftest(QStringLiteral("grid"));

    // --cliptest：「剪贴板」那一页的判据（过滤那条纯函数、**行号↔存储下标**的映射、
    // 状态行说实话、双击复用 / Delete 删这条 / 清空 三条动作真落到数据上）。
    // 跑在沙箱 APPDATA 里：它要往 clipboard.json 写夹具、要点清空，碰不到他真实的剪贴板历史。
    if (args.contains("--cliptest"))
        return runPluginSelftest(QStringLiteral("clipboard"), "ltoolPluginSelftest",
                                 SelftestArg::None, 0, QString(), "clipboard");

    // --randtest：「随机数生成」那一页的判据（区间内、数量对、不重复取满、区间不够就说不行
    // 而不是静默少给、小数位数夹紧、字符集池子只算一份、UUID 形状与不撞 + 页面那八条）。
    // 一次都不 show()；剪贴板那一支在 offscreen 是内存桩，那条判据自己标 SKIP。
    if (args.contains("--randtest"))
        return runPluginSelftest(QStringLiteral("random"));

    // --linuxdotest：「linux.do 盯帖」那一路的判据（解析/命中/去重/前缀拼法/配置夹紧/
    // 页面摆控件）。**一次网都不上、一个气泡都不弹** —— 真取回那条在 --linuxdonet。
    // 它跑在沙箱 APPDATA 里（上面那串名单），因为这一路要写 linuxdo/* 那一节。
    if (args.contains("--linuxdotest"))
        return runPluginSelftest(QStringLiteral("linuxdo"));

    // --linuxdonet [秒]：真去取一趟（联网，不进 CI）。上面那 33 条全是喂 fixture 的，
    // 它们绿只能证明算法对，证明不了中转还活着 —— 这条才是"今天这条路通不通"。
    if (args.contains("--linuxdonet")) {
        const int secs = args.mid(args.indexOf(QStringLiteral("--linuxdonet")) + 1).value(0).toInt();
        return runPluginSelftest(QStringLiteral("linuxdo"), "ltoolLinuxdoNetRun",
                                 SelftestArg::Ms, secs > 0 ? secs : 60);
    }

    // --startuptest：「开机启动分析」那一路的判据（StartupApproved 字节读/写、什么算系统服务、
    // 那张页藏了多少条 + 「就地两段」的第一下必须一个字节都不写）。
    // 真写那几条走的是**自己造的临时键** `HKCU\Software\ltool-startuptest-<pid>`，跑完删掉 ——
    // 他机器上真实的那六条启动项一条都不改（第二下确认在判据里明确不点）。
    // 页面那半要真扫一次这台机器（只读，几百个文件的签名，秒级 —— 耗时打在日志里）。
    if (args.contains("--startuptest"))
        return runPluginSelftest(QStringLiteral("startup"));

    // --plugintest：DLL 插件那条链从头到尾真跑一遍（这次是**真的 LoadLibrary**，
    // 但一次都不 show()，不碰他的桌面）。
    // 名字为什么不叫 --plugin：Qt 自己把 "--plugin" 当命令行选项吃掉了 ——
    // 实测 argc 直接变成 1（--pluginx 就还在），所以这里换个不带歧义的名字。
    if (args.contains("--plugintest")) {
        int bad = 0;
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const auto has = [&bad](const char* name, bool ok) {
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok)
                ++bad;
        };
        // 十八个自带工具现在全是 dll 插件。这张名单是"都编出来了没有"的判据：
        // 少一个就是那棵 dll 没通过检查 —— 别让它在界面上悄悄少一张卡。
        static const char* const kTools[] = { "repair", "ads", "disk", "network", "registry",
                                              "startup", "user", "json", "base64", "shell",
                                              "record", "capture", "software", "downloads",
                                              "tasks", "grid", "language", "linuxdo",
                                              "clipboard", "random" };
        const QDir dir(PluginHost::pluginsDir());
        std::printf("INFO 插件目录：%s\n", dir.absolutePath().toUtf8().constData());
        const auto& metas = PluginHost::scan();
        std::printf("INFO 扫到 %d 个 dll，其中通过检查 %d 个\n", int(metas.size()),
                    int(std::count_if(metas.begin(), metas.end(),
                                      [](const PluginHost::Meta& m) { return m.ok(); })));
        for (const char* id : kTools) {
            const PluginHost::Meta* m = PluginHost::meta(QLatin1String(id));
            has((QString::fromLatin1(id) + " 在目录里且元数据通过检查").toUtf8().constData(),
                m != nullptr && m->ok());
        }
        // 每个插件都跑一遍同一条链：加载 → 建页 → 没 show → 缓存 → 元数据和自报一致
        for (const char* idc : kTools) {
            const QString id = QLatin1String(idc);
            const PluginHost::Meta* m = PluginHost::meta(id);
            if (!m || !m->ok())
                continue;
            LToolPlugin* p = PluginHost::plugin(id);
            has((id + "：真的 LoadLibrary + instance + 版本闸放行").toUtf8().constData(),
                p != nullptr);
            if (!p)
                continue;
            check((id + "：插件自报的 API 版本").toUtf8().constData(),
                  QString::number(p->apiVersion()), QString::number(LTOOL_PLUGIN_API_VERSION));
            // plugin.json 那颗 Version 是商店判定的主判据（PluginStore::compare 先比版本、哈希只用来
            // 区分同号的两份）。写成空或垃圾，这颗在商店里永远只能报「说不清有没有新版」，
            // 所以形状要当场量 —— 每颗自己递增那条纪律要有个能红的地方。
            check((id + "：plugin.json 的版本号是 x.y.z 形状 [" + m->version + "]").toUtf8().constData(),
                  QRegularExpression(QStringLiteral("^\\d+\\.\\d+\\.\\d+$")).match(m->version).hasMatch()
                      ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // plugin.json 那份和 C++ 里那五个 override 是同一件事的两处写法，对不上就是漂移
            check((id + "：plugin.json 与插件自报的名字/描述/关键词一致").toUtf8().constData(),
                  QStringLiteral("%1/%2/%3").arg(p->name(), p->desc())
                      .arg(p->keywords().size()),
                  QStringLiteral("%1/%2/%3").arg(m->name, m->desc).arg(m->keywords.size()));
            // 动作也是两处写法（API 3）：门口挂的牌和实例自己认的必须同一份。
            // 路由最终看实例那份（见 PluginHost::invoke），所以这条红了 = 主程序会找不到它。
            check((id + "：plugin.json 的 Actions 与实例自报的一致").toUtf8().constData(),
                  m->actions.join(QLatin1Char(',')), p->actions().join(QLatin1Char(',')));
            QString err;
            QWidget* page = PluginHost::page(id, nullptr, &err);
            has((id + "：createPage 给出一张页（" + err + "）").toUtf8().constData(), page != nullptr);
            if (!page)
                continue;
            check((id + "：没 show 过（不碰他的桌面）").toUtf8().constData(),
                  page->isVisible() ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("no"));
            check((id + "：再要一次是同一张页（懒创建 + 缓存）").toUtf8().constData(),
                  PluginHost::page(id, nullptr, &err) == page ? QStringLiteral("yes")
                                                              : QStringLiteral("no"),
                  QStringLiteral("yes"));
        }
        // 缓存里那张页**被别人删掉**之后，`page(id)` 不许再把那块已释放的内存发出去，
        // 插件壳里那颗 `m_page` 也不许再往它身上写字。
        // 2026-10-22 他报的「b64 会闪退」就是这个形状：页是 `createPage(parent)` 交给调用方那棵树
        // 拥有的，而 `ToolsPage::rebuildGrid()` 会把摘掉的那一格 deleteLater —— 宿主这头和壳那头
        // 各存了一份裸指针，谁都不知道对方已经没了。
        // 原来这一趟只钉 base64 一颗。它不该只钉一颗：`g_pages`、壳里的 `m_page`、卡片墙这三样
        // 对每颗插件是同一套代码，形状和"是哪一颗"无关 —— 下次轮到谁都有可能。
        int evictTotal = 0, evictClean = 0;
        // 三趟而不是只跑一遍：这类生命周期 bug 的形状和堆怎么复用有关（"新造那张正好落在刚释放的
        // 地址上"就是跑第二遍才会撞上的那种）。第一趟逐颗出声，后两趟只在坏的时候点名。
        for (int round = 1; round <= 3; ++round) {
            int total = 0, clean = 0;
            const bool talk = round == 1;
            for (const char* idc : kTools) {
                const QString id = QLatin1String(idc);
                const PluginHost::Meta* m = PluginHost::meta(id);
                if (!m || !m->ok())
                    continue;
                QString err;
                QWidget* first = PluginHost::page(id, nullptr, &err);
                if (!first) {
                    check((id + "：删了再拿这一趟得先有一张页").toUtf8().constData(),
                          QStringLiteral("no"), QStringLiteral("yes"));
                    continue;
                }
                ++total;
                QPointer<QWidget> guard(first);
                delete first;                    // 那一刀（同步删，不等事件循环）
                const bool died = guard.isNull();
                // 缓存还没来得及清的时候先递一次文本：壳里那颗 m_page 指的正是刚死的这张
                //（旧写法在这里是一次 use-after-free = 他看到的闪退）
                const bool fed = PluginHost::feed(id, QStringLiteral("123"));
                QWidget* again = PluginHost::page(id, nullptr, &err);
                QPointer<QWidget> freshGuard(again);
                const bool usable = again != nullptr && !freshGuard.isNull();
                // 这里**不比对地址**：`delete` 之后新造那张经常落在同一块回收内存上（grid 就这么被
                // 判成过 "same"）。地址相等说明不了"发的是旧的"，指针跟着死 + 当场用得动才说明得了。
                if (again == first && talk)
                    std::printf("INFO %s：新造的那张落在回收地址上（不是同一对象）\n", idc);
                const bool okShape = died && !fed && usable;
                if (okShape)
                    ++clean;
                if (talk || !okShape)
                    check((id + QStringLiteral("（第 %1 趟）：页删掉之后 缓存跟着死 / 递文本不接 / 再要一张用得动")
                                .arg(round)).toUtf8().constData(),
                          QStringLiteral("%1/%2/%3")
                              .arg(died ? QStringLiteral("dead") : QStringLiteral("alive"))
                              .arg(fed ? QStringLiteral("fed") : QStringLiteral("refused"))
                              .arg(usable ? QStringLiteral("usable") : QStringLiteral("none")),
                          QStringLiteral("dead/refused/usable"));
            }
            check(QStringLiteral("第 %1 趟「页被删后再拿」：颗颗干净").arg(round).toUtf8().constData(),
                  QStringLiteral("%1/%2").arg(clean).arg(total),
                  QStringLiteral("%1/%1").arg(total));
            if (round == 1) {
                evictTotal = total;
                evictClean = clean;
            }
        }
        // 递文本那条路（API 2 新加的）：搜索框"json 这串"/"b64 这串"靠它
        QString err;
        QWidget* jp = PluginHost::page(QStringLiteral("json"), nullptr, &err);
        has("json 页在（递文本测试要用它）", jp != nullptr);
        if (jp) {
            has("把一段 JSON 递给 json 工具，插件接住了",
                PluginHost::feed(QStringLiteral("json"), QStringLiteral("{ \"a\": 1 }")));
            auto* edit = jp->findChild<QPlainTextEdit*>();
            check("那段文本真的进了工具", edit && edit->toPlainText().contains('1')
                  ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
            // ---- 左右各是谁（他 2026-10-21：「左边输入，右边看效果」）----
            // 判的是**真实控件的几何**，不是"splitter 第 0 个是谁"那种自己造的断言。
            if (edit) {
                // 量几何之前必须真 show 一次：隐藏着的控件不跑布局，量出来是 minimumSizeHint
                //（第一版就量到"输入框 48px 宽、树 48px 宽"——那不是布局，那是没布局）。
                jp->show();
                jp->resize(900, 600);
                QCoreApplication::processEvents();
                auto* tree = jp->findChild<QTreeWidget*>();
                const QRect e = edit->rect().translated(edit->mapTo(jp, QPoint(0, 0)));
                const QRect t = tree ? tree->rect().translated(tree->mapTo(jp, QPoint(0, 0))) : QRect();
                std::printf("INFO json 页布局：页宽=%d 输入框 x=%d 宽=%d · 结果树 x=%d 宽=%d\n",
                            jp->width(), e.x(), e.width(), t.x(), t.width());
                check("输入框在左边（x 比结果树小）", tree && e.x() < t.x()
                      ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
                check("输入框可编辑（左半边不是只读展示区）",
                      edit->isReadOnly() ? QStringLiteral("no") : QStringLiteral("yes"), QStringLiteral("yes"));
                // 阈值按**页宽**算，不写死像素：离屏那个虚拟屏比真机窄，写死 300 会先被环境夹掉
                check("两边都真的分到了宽度（不是挤成一条）", tree
                      && e.width() >= jp->width() / 4 && t.width() >= jp->width() / 4
                      ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
                jp->hide();
            }
        }
        has("没实现 feedText 的工具老实返回 false（不假装收下了）",
            !PluginHost::feed(QStringLiteral("disk"), QStringLiteral("随便一段文本")));
        // 插件自带的判据：resolve 得到符号才算这条链通
        // 挂牌和符号必须一起有：plugin.json 写 "Activate":true 而 dll 里没导出
        // ltoolPluginActivate = 启动时它那句"我在后台跑着"是假的（startBackground 会点名报错，
        // 但这条判据要更早、在交付时就拦住）。反过来导出了却没挂牌 = 主程序根本不会去叫它。
        for (const char* idc : kTools) {
            const PluginHost::Meta* m = PluginHost::meta(QLatin1String(idc));
            if (!m || !m->ok())
                continue;
            void* fn = PluginHost::entry(QLatin1String(idc), "ltoolPluginActivate");
            check((QStringLiteral("插件 %1 的 Activate 牌和它导出的符号一致").arg(idc)).toUtf8().constData(),
                  QStringLiteral("%1/%2").arg(m->wantsActivate ? 1 : 0).arg(fn ? 1 : 0),
                  m->wantsActivate ? QStringLiteral("1/1") : QStringLiteral("0/0"));
        }
        has("disk 插件带着自己的判据入口", PluginHost::selftestEntry(QStringLiteral("disk")) != nullptr);
        has("user 插件带着自己的判据入口", PluginHost::selftestEntry(QStringLiteral("user")) != nullptr);
        // 动作路由（API 3 新加的）：**一条都不真去执行** —— 右键那些动作会弹窗、会解压、会粉碎文件。
        // 这里只验"找得到人"这一半：主程序那张 verb 表里每个旗标都必须有人声明接它。
        for (const char* f : kShellFlags) {
            const QString action = QString::fromLatin1(f).mid(2);
            check((QStringLiteral("右键动作 %1 有人接（谁）").arg(action)).toUtf8().constData(),
                  PluginHost::actionOwner(action), QStringLiteral("shell"));
        }
        // 右键那几项"该不该在"= 用户那个勾 × 提供者（plugins/shell）在不在。
        // 卸载了插件，文件夹右键里就不该再有 ltool —— 静态注册表项不会自己判断，得有人执行这条。
        check("右键菜单：勾了且插件在 = 该写", ShellMenu::menuShouldBe(true, true) ? "yes" : "no", "yes");
        check("右键菜单：勾了但插件被卸载/禁用 = 不该有（用户报的就是这一条）",
              ShellMenu::menuShouldBe(true, false) ? "yes" : "no", "no");
        check("右键菜单：没勾 = 不该有（哪怕插件装着）", ShellMenu::menuShouldBe(false, true) ? "yes" : "no", "no");
        check("右键菜单：两个都不 = 不该有", ShellMenu::menuShouldBe(false, false) ? "yes" : "no", "no");
        check("右键菜单：这台机器上提供者是谁（plugins/shell 在不在）",
              ShellMenu::providerPresent() ? "yes" : "no", "yes");
        // 2026-10-16 他报"装了右键动作，文件夹右键里没有 ltool"：根因不在上面那张与表，
        // 在**缺键时的默认值**读的是注册表现状，而现状正是这条规则自己删出来的 ——
        // 卸载一次 → 菜单删掉 → 默认变成 false → 再装回来永远解不开（单向闩锁）。
        // 认领规则单独抽成纯函数，就是为了让这四格能离线钉住（真写注册表没法沙箱）。
        check("意图认领：开关从没动过 + 插件在 = 认领成\"要\"（装了这把插件就是为了右键）",
              ShellMenu::shouldAdoptIntent(false, true) ? "yes" : "no", "yes");
        check("意图认领：人明确勾掉过（键存在）就不许认领 —— 绝不替他打开",
              ShellMenu::shouldAdoptIntent(true, true) ? "yes" : "no", "no");
        check("意图认领：插件不在就别凭空写个\"要\"（撤销/禁用那一步不能被顶回去）",
              ShellMenu::shouldAdoptIntent(false, false) ? "yes" : "no", "no");
        check("意图认领：键存在且为 false、插件也不在 = 什么都不做",
              ShellMenu::shouldAdoptIntent(true, false) ? "yes" : "no", "no");
        // 这条盯的是"别把默认值又改回读注册表现状"：那条路就是这次那个闩锁本身。
        check("意图认领那条规则必须在算之前就走（这台机器实测：键存在与否不该影响与表的结论）",
              ShellMenu::menuShouldBe(ShellMenu::shouldAdoptIntent(false, true), true) ? "yes" : "no", "yes");
        check("没人声明的动作老实返回空（不是随便抓一个插件顶上）",
              PluginHost::actionOwner(QStringLiteral("compress-lzx")), QString());
        // F2 那一档（2026-10-15 录屏本体进了 plugins/record）：热键还在这层薄壳里，
        // 动作名 "record" 必须有人在门口挂着牌子。这条红了 = 按 F2 什么都没发生，
        // 而"按了没反应"是这程序里最难查的一个形状，所以它和上面那七条同权重。
        check("热键动作 record 有人接（谁）",
              PluginHost::actionOwner(QStringLiteral("record")), QStringLiteral("record"));
        // 录屏的判据也跟着进了插件：符号点名不到 = 那面旗会红（这里只 resolve，一次都不跑 ——
        // 那五条全都要真屏幕，跑起来就是十几秒外加往屏幕上贴窗口）。
        has("record 插件导出 --recq 的判据入口",
            PluginHost::entry(QStringLiteral("record"), "ltoolRecqSelftest") != nullptr);
        // F1 那一档（2026-10-15 截图族 + OCR 族进了 plugins/capture）：路数和 F2 一样 ——
        // 热键还注册在这层薄壳里，动作名 "capture" 必须有人在门口挂牌。
        // 这条红了 = 按 F1 什么都没发生，而那正是这程序里最难查的一个形状。
        check("热键动作 capture 有人接（谁）",
              PluginHost::actionOwner(QStringLiteral("capture")), QStringLiteral("capture"));
        // "按了没反应"那句提示本身也要说真话（2026-10-19 他按 F1 弹的是"是不是缺 ltool-capture.dll"，
        // 可 dll 明明在目录里 —— 是商店那份产物和这版 core 的导出面对不上，LoadLibrary 失败）。
        // 本机这批全都起得来，所以量的是**分支**：装了的不许说"是不是缺"，没装的才许说。
        {
            const QString installed = PluginHost::whyNoOwner(QStringLiteral("capture"));
            has("接 capture 的插件在目录里时，那句提示不许甩锅成「是不是缺文件」",
                !installed.contains(QStringLiteral("是不是缺")));
            std::printf("INFO 装了但不认这个动作时那句怎么说：%s\n", installed.toUtf8().constData());
            const QString absent = PluginHost::whyNoOwner(QStringLiteral("no-such-action"));
            has("真没人接的动作，那句提示点名缺哪个文件",
                absent.contains(QStringLiteral("是不是缺")) && absent.contains(QStringLiteral("no-such-action")));
        }
        // 那 12 面旗标的判据本体也全在插件里：符号点名不到 = 对应那面旗当场红（不会伪装成绿）。
        // 这里只 resolve、一次都不跑 —— 截图/置顶窗/OCR 那些要真屏幕，还要真下 27MB 引擎。
        static const char* const kCaptureSyms[] = {
            "ltoolSnaprectRun", "ltoolOcrtestRun", "ltoolOcrlayoutRun", "ltoolPinpickRun",
            "ltoolOcrchainRun", "ltoolPintoolRun", "ltoolPindelRun", "ltoolOcrgetRun",
            "ltoolOcrpanelRun", "ltoolPinshotRun", "ltoolCapgeoRun", "ltoolCapflashRun" };
        for (const char* s : kCaptureSyms)
            has((QStringLiteral("capture 插件导出判据入口 ") + QLatin1String(s)).toUtf8().constData(),
                PluginHost::entry(QStringLiteral("capture"), s) != nullptr);
        QString routedTo;
        check("invoke 一个没人接的动作 = false + 说不出是谁",
              !PluginHost::invoke(QStringLiteral("compress-lzx"), { QStringLiteral("x.zip") }, &routedTo)
              && routedTo.isEmpty() ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        // 搜索框那一档：工具那一节要出得来，而且不能像「计划任务」那次一样把整页独占掉
        SearchWindow w;
        auto* input = w.findChild<QLineEdit*>(QStringLiteral("input"));
        auto* list = w.findChild<QListWidget*>(QStringLiteral("list"));
        const auto settle = [] {
            for (int i = 0; i < 40; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
        };
        const auto section = [list](const QString& s) {
            if (!list)
                return QStringLiteral("no");
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->text() == s)
                    return QStringLiteral("yes");
            return QStringLiteral("no");
        };
        has("拿得到输入框和结果列表", input != nullptr && list != nullptr);
        if (input && list) {
            input->setText(QStringLiteral("b64"));
            settle();
            check("搜 b64 出「工具」那一节", section(QStringLiteral("工具")), QStringLiteral("yes"));
            input->setText(QStringLiteral("cipanqingli"));
            settle();
            check("拼音直达也认插件（cipanqingli → 磁盘清理）",
                  section(QStringLiteral("工具")), QStringLiteral("yes"));
            input->setText(QStringLiteral("renwu"));
            settle();
            check("搜 renwu 不该被工具顶掉", section(QStringLiteral("工具")), QStringLiteral("no"));
            check("搜 renwu 时应用那一节还在（工具绝不独占整页）",
                  section(QStringLiteral("── 应用 ──")), QStringLiteral("yes"));
            // ---- 节的先后（他 2026-10-21 拍的：应用第一、网站第二；命中直达时直达仍第一）----
            // 数的是**行号**，不是"在不在"—— 两条都在但顺序反了，正是这次要改的东西。
            const auto pos = [list](const QString& s) {
                if (!list)
                    return -1;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text() == s)
                        return i;
                return -1;
            };
            std::printf("INFO 搜 renwu 时：应用节在第 %d 行 · 网站节在第 %d 行\n",
                        pos(QStringLiteral("── 应用 ──")), pos(QStringLiteral("── 网站 ──")));
            check("没命中直达：应用节排在网站节之前",
                  pos(QStringLiteral("── 应用 ──")) >= 0
                      && pos(QStringLiteral("── 网站 ──")) > pos(QStringLiteral("── 应用 ──"))
                  ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
            input->setText(QStringLiteral("b64"));
            settle();
            std::printf("INFO 搜 b64（工具直达）时：工具节在第 %d 行 · 应用节在第 %d 行\n",
                        pos(QStringLiteral("工具")), pos(QStringLiteral("── 应用 ──")));
            check("命中工具直达：那一节仍在第 0 行（回车还是它）",
                  pos(QStringLiteral("工具")) == 0 ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // `windows` 是这条最刺眼的一档：「Windows 用户名」那把工具名字就以 windows 开头，
            // 而本机确实有一堆 Windows* 应用 —— 直达拿第一行，但**不许把应用那一节铲平**。
            input->setText(QStringLiteral("windows"));
            settle();
            std::printf("INFO 搜 windows 时：工具节在第 %d 行 · 应用节在第 %d 行 · 网站节在第 %d 行\n",
                        pos(QStringLiteral("工具")), pos(QStringLiteral("── 应用 ──")),
                        pos(QStringLiteral("── 网站 ──")));
            check("搜 windows：直达仍占第 0 行",
                  pos(QStringLiteral("工具")) == 0 ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("搜 windows：应用那一节没被铲平，且排在直达之后、网站之前",
                  pos(QStringLiteral("── 应用 ──")) > 0
                      && pos(QStringLiteral("── 应用 ──")) < pos(QStringLiteral("── 网站 ──"))
                  ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
            input->setText(QStringLiteral("zzqqxx"));
            settle();
            check("网站那一组现在有表头（以前那几行跟在上一节表头底下，看着像工具的结果）",
                  section(QStringLiteral("── 网站 ──")), QStringLiteral("yes"));
            input->clear();
            settle();
        }
        // 卡片墙：这十八个现在全靠插件目录，卡片数必须等于"通过检查的插件数"，
        // 而且每张卡都要能在布局里走到（数 findChild 不等于界面可见 —— 配置页那栏
        // 就是这么隐身的，2026-10-12 他报的）。这条替掉了原来那两条"内嵌槽位 0..N-1"。
        ToolsPage wall;
        QSet<const QWidget*> laidOut;
        std::function<void(QLayout*)> walk = [&](QLayout* l) {
            if (!l)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* w = it->widget()) {
                    laidOut.insert(w);
                    walk(w->layout());
                    if (auto* sc = qobject_cast<QScrollArea*>(w))
                        if (sc->widget())
                            walk(sc->widget()->layout());
                } else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        walk(wall.layout());
        int cards = 0;
        for (QFrame* c : wall.findChildren<QFrame*>())
            if (c->objectName() == QLatin1String("row"))
                ++cards;
        int reachable = 0;
        for (QFrame* c : wall.findChildren<QFrame*>())
            if (c->objectName() == QLatin1String("row") && laidOut.contains(c))
                ++reachable;
        int okCount = 0;
        for (const PluginHost::Meta& m : metas)
            if (m.ok())
                ++okCount;
        check("卡片数 = 通过检查的插件数", QString::number(cards), QString::number(okCount));
        check("每张卡都真的摆在布局里（不是只 new 出来）", QString::number(reachable),
              QString::number(cards));
        check("通过检查的插件卡片都在卡片墙上", QString::number(cards), QString::number(okCount));
        // 卡片墙的几何：离屏 resize 之后布局会算，不用 show（show 就把设置窗拍到他桌面上）。
        // 这条盯的是"这十七张卡摆得开吗"。
        // WA_DontShowOnScreen：走完整的 show/布局激活路径，但不往屏幕上贴一个像素、
        // 也不抢焦点。裸 resize() 不够 —— 没 show 过的顶层窗口布局根本不算，
        // 第一版就是这么量出"九张卡全在 (0,0,0,0)"，那是探针坏了不是卡片坏了。
        wall.setAttribute(Qt::WA_DontShowOnScreen, true);
        wall.resize(940, 720);
        wall.show();
        for (int i = 0; i < 20; ++i)
            QCoreApplication::processEvents();
        QList<QRect> rects;
        for (QFrame* c : wall.findChildren<QFrame*>())
            if (c->objectName() == QLatin1String("row"))
                rects << c->geometry();
        bool sized = true;
        bool overlap = false;
        for (int i = 0; i < rects.size(); ++i) {
            if (rects[i].width() < 120 || rects[i].height() < 80)
                sized = false;
            for (int j = i + 1; j < rects.size(); ++j)
                if (rects[i].intersects(rects[j]))
                    overlap = true;
        }
        if (!rects.isEmpty())
            std::printf("INFO 第一张卡的矩形=(%d,%d %dx%d)\n", rects.first().x(), rects.first().y(),
                        rects.first().width(), rects.first().height());
        wall.hide();
        check("每张卡都有画得出来的尺寸（最窄 120x80）", sized ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        check("卡片互不重叠（三列排版真的分开了）", overlap ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("no"));
        // ---- 他报「b64 会闪退」那一条路：搜索框直达 → 弹设置页 → 展开那张卡 → 顺手把后面那段文本递进去 ----
        // SearchWindow 那边是 `openTool(id, extra)` → `SettingsWindow::showTool` → 这里同一个函数，
        // 所以他"每次都会"的动作在这条上是被判据走得到的（他 2026-10-22 确认稳定复现）。
        const bool opened = wall.showTool(QStringLiteral("base64"), QStringLiteral("aGVsbG8="));
        check("直达那一路（展开卡片 + 递文本）走通了，没在路上崩",
              opened ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
        QString bpErr;
        QWidget* bp = PluginHost::page(QStringLiteral("base64"), nullptr, &bpErr);
        const auto edits = bp ? bp->findChildren<QPlainTextEdit*>() : QList<QPlainTextEdit*>();
        // 哪个框是哪个：按**角色**认（只读的那个是结果框），不按 findChildren 的顺序 ——
        // 第一版按"第 0 个是输入"写，红了一次：那个顺序是对象树顺序，不是界面左右顺序。
        QPlainTextEdit* bin = nullptr;
        QPlainTextEdit* bout = nullptr;
        for (QPlainTextEdit* e : edits) {
            if (e->isReadOnly())
                bout = e;
            else
                bin = e;
        }
        check("递给 base64 的那串真的进了输入框",
              bin && bin->toPlainText() == QStringLiteral("aGVsbG8=")
                  ? QStringLiteral("yes") : QStringLiteral("[%1 个框，输入框=%2]")
                                                 .arg(edits.size())
                                                 .arg(bin ? bin->toPlainText() : QStringLiteral("无")),
              QStringLiteral("yes"));
        check("那一串当场解出来了（结果框里是 hello）",
              bout && bout->toPlainText() == QStringLiteral("hello")
                  ? QStringLiteral("yes") : QStringLiteral("[%1]").arg(bout ? bout->toPlainText() : QStringLiteral("无")),
              QStringLiteral("yes"));
        check("stack 当前那一格就是这张页（不是回到卡片墙）",
              bp && wall.findChild<QStackedWidget*>() && wall.findChild<QStackedWidget*>()->currentWidget() == bp
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        // ---- 每一颗都走一遍他报的那条路，中间插一次「拆墙」----
        // 上面那三条钉的是他报的那一次（base64）。这一段把同一条路按颗铺开：直达 → 拆墙 → 再直达。
        // 这一趟量的是"进程还活着"，不是功能对不对 —— 所以它红了就等于有人又要开始闪退。
        int hitTotal = 0, hitOk = 0;
        QStringList hitIds;
        for (const char* idc : kTools) {
            const QString id = QLatin1String(idc);
            const PluginHost::Meta* m = PluginHost::meta(id);
            if (!m || !m->ok())
                continue;
            ++hitTotal;
            hitIds << id;
            if (wall.showTool(id, QStringLiteral("123")))
                ++hitOk;
        }
        check("逐颗直达（带一段文本）：每颗都展开成了当前那一格",
              QStringLiteral("%1/%2").arg(hitOk).arg(hitTotal), QStringLiteral("%1/%1").arg(hitTotal));
        for (int round = 1; round <= 2; ++round) {
            wall.rebuildGrid();                    // 第 0 格摘掉 deleteLater = 那一刀
            for (int i = 0; i < 30; ++i)
                QCoreApplication::processEvents();   // 让它真的落地，别停在队列里
            int back = 0;
            for (const QString& id : hitIds)
                if (wall.showTool(id))
                    ++back;
            check(QStringLiteral("第 %1 次拆墙之后再逐颗直达：一颗没崩").arg(round).toUtf8().constData(),
                  QStringLiteral("%1/%2").arg(back).arg(hitTotal), QStringLiteral("%1/%1").arg(hitTotal));
        }
        // ---- 商店页那两维过滤：名字（只比显示名）+ 安装状态（全部/已装/未装）----
        // 放最后：前面那几栏都按"满墙 20 张"在数，这里开始会改那一面墙的内容。
        {
            check("筛的规则①：空查询 = 全通过（不是全挡住）",
                  ToolsPage::nameMatches(QStringLiteral("Base64 编解码"), QString()) ? "yes" : "no", "yes");
            check("筛的规则②：大小写不敏感（base64 命中「Base64 编解码」）",
                  ToolsPage::nameMatches(QStringLiteral("Base64 编解码"), QStringLiteral("base64"))
                      ? "yes" : "no", "yes");
            check("筛的规则③：两边首尾空格都当没看见",
                  ToolsPage::nameMatches(QStringLiteral("  剪贴板 "), QStringLiteral("  剪贴  "))
                      ? "yes" : "no", "yes");
            // 这两条是"只比名字"这句话的证据：关键词和 id 一概不算（哪天要放宽，这两条会红，得明着改）
            check("筛的规则④：关键词不算（「宫格」筛不出「图片排版」）",
                  ToolsPage::nameMatches(QStringLiteral("图片排版"), QStringLiteral("宫格"))
                      ? "yes" : "no", "no");
            check("筛的规则⑤：id 不算（「b64」筛不出「Base64 编解码」）",
                  ToolsPage::nameMatches(QStringLiteral("Base64 编解码"), QStringLiteral("b64"))
                      ? "yes" : "no", "no");
            // 「算不算已装」那一颗（界面和这里读同一份）：五档算、三档不算、待删那一档按盘上说
            const QString instWord = QStringLiteral("%1%2%3%4%5")
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Installed, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Update, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Different, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::LocalOnly, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Disabled, false) ? 1 : 0);
            check("状态①：已装/可更新/与商店不同/清单外/已禁用 都算「已装」（盘上都有它）",
                  instWord, QStringLiteral("11111"));
            const QString missWord = QStringLiteral("%1%2%3")
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Missing, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::ApiMismatch, false) ? 1 : 0)
                .arg(PluginStore::countsAsInstalled(PluginStore::State::Blocked, false) ? 1 : 0);
            check("状态②：能装/装不了/条目不能用 算「未装」", missWord, QStringLiteral("000"));
            check("状态③：排进待删表而状态已是 Missing 的那条 —— 文件还在盘上，就算已装",
                  PluginStore::countsAsInstalled(PluginStore::State::Missing, true) ? "yes" : "no", "yes");

            // 上面那几栏留下的是"正看着一张插件页"的状态（直达测完没回去），而那一状态下改过滤
            // 按设计**不重排**（不打断人）。所以这一栏第一件事是按「返回商店」回到墙上再测。
            if (auto* backBtn = findChildByAccessible<QToolButton>(&wall, QStringLiteral("返回商店")))
                backBtn->click();
            QCoreApplication::processEvents();
            // 过滤框按 accessibleName 认：这一墙里现在挂着好几张插件页，findChild 的顺序不是界面顺序
            QLineEdit* filterBox = nullptr;
            for (QLineEdit* e : wall.findChildren<QLineEdit*>())
                if (e->accessibleName() == QString::fromUtf8("按插件名称筛插件"))
                    filterBox = e;
            has("商店页：那颗名称过滤框在（按 accessibleName 认，不靠 findChild 的顺序）",
                filterBox != nullptr);
            auto chipsOf = [&wall](const QString& word) -> QPushButton* {
                const auto btns = wall.findChildren<QPushButton*>();
                for (QPushButton* b : btns)
                    if (b->text() == word)
                        return b;
                return nullptr;
            };
            QPushButton* chipAll = chipsOf(QStringLiteral("全部"));
            QPushButton* chipInst = chipsOf(QStringLiteral("已装"));
            QPushButton* chipMiss = chipsOf(QStringLiteral("未装"));
            check("商店页：三档都在（全部/已装/未装），且默认「全部」亮着",
                  QStringLiteral("%1%2%3/%4").arg(chipAll != nullptr).arg(chipInst != nullptr)
                      .arg(chipMiss != nullptr).arg(chipAll && chipAll->isChecked() ? 1 : 0),
                  QStringLiteral("111/1"));
            has("商店页：过滤框和那三档真的摆在布局里（不是只 new 出来）",
                filterBox && laidOut.contains(filterBox));

            // 那一面墙自己的 stack：findChild 会先撞上插件页里嵌套的那些，所以要按 parent 认
            const auto wallStack = [&wall]() -> QStackedWidget* {
                for (QStackedWidget* s : wall.findChildren<QStackedWidget*>())
                    if (s->parent() == &wall)
                        return s;
                return nullptr;
            };
            // 数卡片只数**当前那一格**里的。老那一格是 deleteLater 掉的，而 processEvents()
            // 不处理 DeferredDelete（要 sendPostedEvents 才落地）—— 上一版就是这么数到 20 张假数。
            const auto countCards = [&wallStack]() {
                QCoreApplication::processEvents();
                QStackedWidget* st = wallStack();
                QWidget* gridPage = st ? st->widget(0) : nullptr;
                if (!gridPage)
                    return 0;
                auto* sc = qobject_cast<QScrollArea*>(gridPage);
                QWidget* host = sc && sc->widget() ? sc->widget() : gridPage;
                int n = 0;
                for (QFrame* c : host->findChildren<QFrame*>())
                    if (c->objectName() == QLatin1String("row"))
                        ++n;
                return n;
            };
            const auto localRows = PluginStore::localRows(QString());
            int wantInst = 0, wantMiss = 0;
            for (const PluginStore::Row& r : localRows) {
                if (PluginStore::countsAsInstalled(r.state, r.queuedRemove))
                    ++wantInst;
                else
                    ++wantMiss;
            }
            check("筛之前那一面是满的（卡片数 = 清单颗数，且已装 + 未装 = 全部）",
                  QStringLiteral("%1/%2").arg(countCards()).arg(wantInst + wantMiss),
                  QStringLiteral("%1/%1").arg(wantInst + wantMiss));
            chipInst->click();
            check("点「已装」：墙上的卡片数 = 数据层现算的已装颗数（不是界面自己再数一遍）",
                  QStringLiteral("%1").arg(countCards()), QStringLiteral("%1").arg(wantInst));
            has("点「已装」之后，那一行说了「筛出 N / 共 M」这句实话",
                wall.filterNote().contains(QStringLiteral("已装"))
                    && wall.filterNote().contains(QStringLiteral("共 %1 张").arg(wantInst + wantMiss)));
            chipMiss->click();
            check("点「未装」：两档相加 = 全部（没有一颗被漏在两边之外）",
                  QStringLiteral("%1").arg(countCards() + wantInst),
                  QStringLiteral("%1").arg(wantInst + wantMiss));
            chipAll->click();
            filterBox->setText(QStringLiteral("随机"));
            check("名字筛「随机」：只剩那一张卡", QStringLiteral("%1").arg(countCards()), QStringLiteral("1"));
            chipMiss->click();
            check("两维是「与」：名字命中而状态不命中的那张也不许留下",
                  QStringLiteral("%1").arg(countCards()), QStringLiteral("0"));
            has("0 命中时那一行改口说「上面没有可列的卡」（不让人对着白墙猜）",
                wall.filterNote().contains(QStringLiteral("没有可列的卡")));
            chipAll->click();
            filterBox->setText(QStringLiteral("这个串不可能有"));
            check("纯名字筛一个不存在的串：0 张", QStringLiteral("%1").arg(countCards()), QStringLiteral("0"));
            has("墙上那一句说清是被哪一维挡的、怎么回去",
                [&wall]() {
                    const auto labels = wall.findChildren<QLabel*>();
                    for (const QLabel* l : labels)
                        if (l->text().contains(QStringLiteral("没有同时满足")))
                            return l->text().contains(QStringLiteral("✕")) && l->text().contains(QStringLiteral("全部"));
                    return false;
                }());
            filterBox->clear();
            check("清掉过滤：那一面回到满墙", QStringLiteral("%1").arg(countCards()),
                  QStringLiteral("%1").arg(wantInst + wantMiss));
            filterBox->setText(QStringLiteral("剪贴"));
            wall.rebuildGrid();                 // 拆一次墙（禁用/装完一颗走的也是这条）
            check("过滤串活得过「拆墙重排」（不是重排一下就忘了自己筛过什么）",
                  QStringLiteral("%1").arg(countCards()), QStringLiteral("1"));
            check("重排之后那一行还在说筛出的数",
                  wall.filterNote().contains(QStringLiteral("筛出 1")) ? "yes" : "no", "yes");

            // 正看着一张页时改过滤：不许把人拽回卡片墙，重排欠着，返回那一步才补
            has("打开一张插件页（下面那三条要用它验「不打断」这一件事）", wall.showTool(QStringLiteral("json")));
            QWidget* openedPage = PluginHost::page(QStringLiteral("json"), nullptr, nullptr);
            filterBox->setText(QStringLiteral("zzz不可能"));
            QCoreApplication::processEvents();
            check("改过滤时正看着一张页：当前那一格还是这张页（没被跳回商店）",
                  wallStack() && wallStack()->currentWidget() == openedPage ? "yes" : "no", "yes");
            has("那一行照实说「返回商店时才重排这一面」（不装成已经生效）",
                wall.filterNote().contains(QStringLiteral("返回商店")));
            if (auto* back = wall.findChild<QToolButton*>())
                back->click();
            QCoreApplication::processEvents();
            check("返回商店之后，欠着的那一次重排补上了（这一面 0 张）",
                  QStringLiteral("%1").arg(countCards()), QStringLiteral("0"));
            filterBox->clear();
            check("最后清干净：那一面又是满墙", QStringLiteral("%1").arg(countCards()),
                  QStringLiteral("%1").arg(wantInst + wantMiss));
        }
        // ---- 「一键下载全部」：两段就地确认 + 一次只飞一颗 ----
        // 这一栏分两块：先在**没拉清单**的那一面上量"一颗都不许动"，
        // 再写一份假清单（地址全是 .invalid，永远解析不出去）把"第二下真排队、还是一颗一颗飞、
        // 全失败时那一行说成了几颗"量出来 —— 这一趟既不上外网、也不会往目录里落假 dll。
        {
            const auto bulkOf = [](ToolsPage* p) {
                const auto btns = p->findChildren<QPushButton*>();
                for (QPushButton* b : btns)
                    if (b->accessibleName() == QString::fromUtf8("一键下载全部"))
                        return b;
                return static_cast<QPushButton*>(nullptr);
            };
            QPushButton* bulk = bulkOf(&wall);
            has("那一颗「一键下载全部」在（按 accessibleName 认）", bulk != nullptr);
            has("而且真的摆在页眉那一排布局里（不是只 new 出来）", bulk && laidOut.contains(bulk));
            // 不许按"几个字 × 一个字宽"估：上一版就是这么留的，本机 `windows` 平台真字体下
            // 基线那句「一键下载全部」装不下（CI 跑 offscreen，字体度量不一样，它看不见）。
            // 这里量的是产品自己那颗 `sizeHint()` —— 两版标签各自该占多宽，都得塞进预留的那块。
            const int baseHint = bulk ? bulk->sizeHint().width() : -1;
            const QString keepText = bulk ? bulk->text() : QString();
            if (bulk)
                bulk->setText(QStringLiteral("确认这 12 颗"));
            const int armedHint = bulk ? bulk->sizeHint().width() : -1;
            if (bulk)
                bulk->setText(keepText);
            check("两版标签都塞得进预留的那块（点下去只换字、不挤旁边那一排按钮）",
                  QStringLiteral("%1/%2").arg(baseHint <= (bulk ? bulk->minimumWidth() : -1))
                      .arg(armedHint <= (bulk ? bulk->minimumWidth() : -1)),
                  QStringLiteral("1/1"));
            has("换字之后那颗按钮上的话原样还回来（探针自己借道试宽度，不许把状态留下）",
                bulk && bulk->text() == QString::fromUtf8("一键下载全部"));

            // 纯规则那五条：排谁、第一下怎么说、没得排怎么说、飞完怎么说
            auto mkRow = [](const QString& id, PluginStore::State st, bool usable) {
                PluginStore::Row r;
                r.remote.id = id;
                r.remote.name = id;
                r.remote.file = QStringLiteral("ltool-%1.dll").arg(id);
                r.remote.bytes = 4096;
                r.state = st;
                r.entryUsable = usable;
                return r;
            };
            const QVector<PluginStore::Row> fake = {
                mkRow(QStringLiteral("a"), PluginStore::State::Missing, true),
                mkRow(QStringLiteral("b"), PluginStore::State::Missing, true),
                mkRow(QStringLiteral("c"), PluginStore::State::Installed, true),
                mkRow(QStringLiteral("d"), PluginStore::State::ApiMismatch, false),
                mkRow(QStringLiteral("e"), PluginStore::State::Blocked, false),
                mkRow(QStringLiteral("f"), PluginStore::State::LocalOnly, true),
            };
            check("排的就只是「没装的那几颗」（已装/清单外/装不了/条目坏的一颗都不算）",
                  QStringLiteral("%1").arg(ToolsPage::bulkTargets(fake).size()), QStringLiteral("2"));
            const QString ask = ToolsPage::bulkAskLine(3, 3 * 1024 * 1024, 2);
            has("第一下那句说了四件事：一颗没动、排几颗、大约多少、还是一次一颗",
                ask.contains(QStringLiteral("第一下一颗都没动"))
                    && ask.contains(QStringLiteral("排 3 颗"))
                    && ask.contains(QStringLiteral("约 3.00 MB"))
                    && ask.contains(QStringLiteral("一次只飞一颗")));
            has("没有装不了的那几颗时，就不提那句（不编一个数出来）",
                !ToolsPage::bulkAskLine(1, 1024, 0).contains(QStringLiteral("用不了")));
            has("装不了的那几颗要点名有几颗（这一面不硬排它们）",
                ask.contains(QStringLiteral("另有 2 颗")));
            has("一颗都排不了、是因为清单还没拉 —— 那句话要给出路",
                ToolsPage::bulkEmptyLine(false).contains(QStringLiteral("刷新清单")));
            has("一颗都排不了、是因为全装着了 —— 那句不许谎称「还没拉清单」",
                ToolsPage::bulkEmptyLine(true).contains(QStringLiteral("全装着了"))
                    && !ToolsPage::bulkEmptyLine(true).contains(QStringLiteral("刷新清单")));
            const QStringList twoFail = { QStringLiteral("x"), QStringLiteral("y") };
            has("飞完那一行报的是「成了几颗 / 没成几颗」，没成的要点名",
                ToolsPage::bulkDoneLine(4, twoFail).contains(QStringLiteral("成了 2 颗"))
                    && ToolsPage::bulkDoneLine(4, twoFail).contains(QStringLiteral("没成 2 颗"))
                    && ToolsPage::bulkDoneLine(4, twoFail).contains(QStringLiteral("x、y")));
            has("全成的时候不许拖一句空的「没成的：」",
                !ToolsPage::bulkDoneLine(3, QStringList()).contains(QStringLiteral("没成的：")));

            // 没拉清单那一面：第一下**一颗都不许排**，并且要说清为什么排不了
            const int busyBefore = wall.installsBusy();
            bulk->click();
            QCoreApplication::processEvents();
            check("没清单时点第一下：一颗都没排（队列和在飞都还是空）",
                  QStringLiteral("%1").arg(wall.installsBusy() - busyBefore), QStringLiteral("0"));
            has("没清单时那一行给的是出路，不是「排了 0 颗」这种废话",
                wall.storeLine().contains(QStringLiteral("刷新清单")));
            has("没清单时那颗按钮没进确认态（标签还是原话）",
                bulk->text() == QString::fromUtf8("一键下载全部") && !wall.bulkArmed());

            // ---- 下面那一栏要一份"清单已拉到"的商店页：假清单，地址全走 .invalid ----
            // 为什么用 .invalid（和 --storeexit 那半颗假条目同一个写法）：这颗域名按 RFC 2606 永远不会被解析出去，
            // 本机实测解它 31 ms 就失败 —— 换成 127.0.0.1 上没人听的端口，这台机每次连接要磨 2 秒，
            // 两条 × 两个候选 × 两趟就是十几秒，一面旗为了证明"一次只飞一颗"不该吃那么多时间。
            const QString cachePath = Utils::dataDir() + QStringLiteral("/plugins_cache.json");
            QFile::remove(cachePath);                       // 先清掉上一轮可能留下的那一份
            const QString zeros(64, QLatin1Char('0'));
            const QString fakeIndex = QStringLiteral(
                R"({"schema":1,"plugins":[)"
                R"({"id":"fakewhole","name":"假的那颗整的","desc":"","file":"ltool-fakewhole.dll",)"
                R"("url":"https://example.invalid/ltool-fakewhole.dll","sha256":"%1","bytes":2048,)"
                R"("api":%2,"version":"9.9.9"},)"
                R"({"id":"fakehalf","name":"假的那颗半的","desc":"","file":"ltool-fakehalf.dll",)"
                R"("url":"https://example.invalid/ltool-fakehalf.dll","sha256":"%1","bytes":1024,)"
                R"("api":%2,"version":"9.9.9"},)"
                R"({"id":"fakeoldapi","name":"假的那颗旧接口","desc":"","file":"ltool-fakeoldapi.dll",)"
                R"("url":"https://example.invalid/ltool-fakeoldapi.dll","sha256":"%1","bytes":512,)"
                R"("api":3,"version":"9.9.9"}]})")
                .arg(zeros).arg(LTOOL_PLUGIN_API_VERSION);
            {
                QDir().mkpath(Utils::dataDir());
                QFile f(cachePath);
                has("那份假清单写下去了（沙箱数据目录，绝对路径现算）",
                    f.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && f.write(fakeIndex.toUtf8()) == fakeIndex.toUtf8().size());
                // 不写这颗日期，构造函数会自己起那 100ms 的自动拉清单定时器 ——
                // 它会中途把这一面重排掉，这一栏就成了看网络脸色的判据（--storeexit 那条注释里钉过同一件事）。
                QSettings cfg(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat);
                cfg.setValue(QStringLiteral("store/lastFetchDate"),
                             QDate::currentDate().toString(Qt::ISODate));
                cfg.sync();
            }
            // 这一扇用**堆上的** ToolsPage：这一栏最后一刀是"在飞的途中把这扇窗关掉"
            // （他中途关设置窗就是这件事），栈对象做不到半途关。
            auto* batch = new ToolsPage;
            batch->setAttribute(Qt::WA_DontShowOnScreen, true);
            batch->resize(940, 720);
            batch->show();
            for (int i = 0; i < 20; ++i)
                QCoreApplication::processEvents();
            QPushButton* bulk2 = bulkOf(batch);
            has("那一扇上那颗「一键下载全部」也在（假清单那一面）", bulk2 != nullptr);
            if (!bulk2) {
                // 这一栏剩下的全要用它，拿不到就点名红一条然后收手 ——
                // 上一轮那条 rc=139 就是"判据判了缺控件、探针还继续 deref"换来的（任务 #133 那条规矩在这儿先用上）
                has("拿不到那颗按钮：这一栏剩下的量不了，不许当成绿", false);
            } else {
                // 目录里那两颗假 dll 与它们留下的 .part：名字是这一栏自己造的，路径全部现算
                const auto junkInDir = [] {
                    QStringList out;
                    const QDir d(PluginHost::pluginsDir());
                    for (const QString& n : d.entryList(QDir::Files))
                        if (n.startsWith(QStringLiteral("ltool-fake")))
                            out << d.absoluteFilePath(n);
                    return out;
                };
                const auto landedDlls = [] {
                    QStringList out;
                    const QDir d(PluginHost::pluginsDir());
                    for (const QString& n : d.entryList(QDir::Files))
                        if (n.startsWith(QStringLiteral("ltool-fake")) && !n.endsWith(QStringLiteral(".part")))
                            out << n;
                    return out;
                };
                check("那两条下载确实没落假 dll（此刻目录里 ltool-fake* 只有 .part 那两个）",
                      QStringLiteral("%1").arg(landedDlls().size()), QStringLiteral("0"));
                check("假清单那一面：能排的就是那 2 颗（旧接口那颗不在里面）",
                      QStringLiteral("%1").arg(ToolsPage::bulkTargets(
                          PluginStore::rows(PluginStore::parseIndex(fakeIndex.toUtf8(), nullptr))).size()),
                      QStringLiteral("2"));
                // 悬停说明那一份也要有数：它由"这一面刚建起来"那一句挂上，被后面任何一句短话接管时
                // 必须收掉 —— 挂着不放就是把上一次失败的原因配到这一句没头没尾的短话上。
                const QString tipBefore = batch->storeLineWhy();
                has("这一面刚建起来：那句短话带着悬停说明（缓存是哪份、清单地址）",
                    tipBefore.contains(QStringLiteral("plugins_cache.json"))
                        && tipBefore.contains(QStringLiteral("http")));
                bulk2->click();
                QCoreApplication::processEvents();
                has("短话接管那一行之后，上一句的悬停说明收掉了（不留着上一次的）",
                    batch->storeLineWhy().isEmpty());
                has("页面上那句短话里不许出现 URL / HTTP 码（那是悬停说明和日志的活）",
                    !batch->storeLine().contains(QStringLiteral("http")));
                check("第一下之后：按钮进确认态，而队列里一颗都没排",
                      QStringLiteral("%1/%2").arg(batch->bulkArmed() ? "armed" : "cold")
                          .arg(batch->installsBusy()),
                      QStringLiteral("armed/0"));
                check("第一下那颗按钮上写的是「确认这 2 颗」（数字来自数据层现算）",
                      bulk2->text(), QStringLiteral("确认这 2 颗"));
                has("第一下那一行照实说：还没动一颗、排几颗、另有 1 颗用不了",
                    batch->storeLine().contains(QStringLiteral("第一下一颗都没动"))
                        && batch->storeLine().contains(QStringLiteral("排 2 颗"))
                        && batch->storeLine().contains(QStringLiteral("另有 1 颗")));
                // 确认态里那个数字是**上一秒的商店**：中间只要拆过一次墙（他点「刷新清单」、
                // 或哪颗装完），那个数就作废，得重新按第一下看新的。
                batch->rebuildGrid();
                for (int i = 0; i < 10; ++i)
                    QCoreApplication::processEvents();
                has("进确认态之后拆一次墙：确认收回、标签回到原话（不许拿旧数字排新的一份）",
                    !batch->bulkArmed() && bulk2->text() == QString::fromUtf8("一键下载全部"));
                bulk2->click();
                QCoreApplication::processEvents();
                has("收回之后那颗按钮还活着：再按一次照样进确认态",
                    batch->bulkArmed() && bulk2->text() == QString::fromUtf8("确认这 2 颗"));
                bulk2->click();
                QCoreApplication::processEvents();
                check("第二下之后：只有一颗真在飞、另一颗排着（他连点九颗崩的那一类）",
                      QStringLiteral("%1/%2").arg(batch->inflightCount()).arg(batch->installsBusy()),
                      QStringLiteral("1/2"));
                has("第二下那一行改口说已排几颗（不留在「一颗都没动」那句）",
                    batch->storeLine().contains(QStringLiteral("已排 2 颗")));
                // 「确实起来了」的证据是上面那颗 `inflightCount()==1`（飞行标记在下载发起前就挂了号），
                // 不是数目录里的 .part —— 第二颗要等第一颗失败才开始，数文件就成了看 DNS 脸色。
                // 一颗在飞、一颗排着的时候拆墙：那次闪退的同一条形状 —— 队列里存的是卡片上的控件，
                // 墙拆了它们就该自己变 null（QPointer 兜着），下载照样走完、一颗都不许多起。
                batch->rebuildGrid();
                for (int i = 0; i < 10; ++i)
                    QCoreApplication::processEvents();
                check("在飞的时候拆一次墙：还是 1 颗在飞 + 1 颗排着（没多起一趟，也没把这一批弄丢）",
                      QStringLiteral("%1/%2").arg(batch->inflightCount()).arg(batch->installsBusy()),
                      QStringLiteral("1/2"));
                // 「收尾那一行说成了几颗」抽成了纯函数：等那两条真失败 = 看 DNS 的脸色
                // （本机实测同一条 4 秒和 12 秒都出现过，一面闸不该长成那样）。这里量那颗谓词本身。
                QVector<PluginStore::Row> landedNow;
                landedNow << mkRow(QStringLiteral("fakewhole"), PluginStore::State::Installed, true)
                          << mkRow(QStringLiteral("fakehalf"), PluginStore::State::Missing, true);
                const QStringList twoIds = { QStringLiteral("fakewhole"), QStringLiteral("fakehalf") };
                check("收尾数的是数据层的说法：一颗落了地、一颗还在 Missing ⇒ 没成的就是那颗",
                      QStringLiteral("%1").arg(ToolsPage::bulkMissed(twoIds, landedNow)
                                                   .join(QStringLiteral(","))),
                      QStringLiteral("fakehalf"));
                has("收尾那一行按这个报「成了 1 颗，没成 1 颗」并且点名那颗",
                    ToolsPage::bulkDoneLine(2, ToolsPage::bulkMissed(twoIds, landedNow))
                        .contains(QStringLiteral("成了 1 颗"))
                    && ToolsPage::bulkDoneLine(2, ToolsPage::bulkMissed(twoIds, landedNow))
                           .contains(QStringLiteral("fakehalf")));
                // 收这一栏：先**在飞的途中把这扇窗关掉**（那两条下载跟着 parent 一起没 ——
                // 这就是他中途关设置窗那一条路），再删自己造的那几份东西。
                delete batch;
                for (int i = 0; i < 10; ++i) {
                    QThread::msleep(20);
                    QCoreApplication::processEvents();
                }
                // 这一刀自己没有"绿"可报：它的证据是**整趟跑下来 rc=0**（真崩就在这两句之间死掉，
                // 缓冲的 stdout 一起没 —— 上一轮那条 rc=139 见过这个形状）。写一条永远真不了的判据是自欺。
                std::printf("INFO 在飞的途中把商店那扇窗关掉了（两颗：一颗在飞、一颗排着）—— 这一趟没崩就是它的证据\n");
                QFile::remove(cachePath);
                for (const QString& p : junkInDir())
                    QFile::remove(p);
                check("这一栏自己留下的东西清干净了（假清单 + .part 一个都不留在插件目录里）",
                      QStringLiteral("%1").arg(junkInDir().size()), QStringLiteral("0"));
            }
        }
        // ---- 建页那一条边界的 SEH 外壳（2026-10-23）：插件里读坏内存不该把整个程序带走 ----
        // 这颗接口只为这条边界存在：它不进目录、商店列不出来、也不会有第二处调用它。
        // 只在 Windows 上量：非 Win 那边这层守卫如实不装（见 PluginHost.cpp 里那句注释），
        // 拿一颗故意崩的接口去喂它 = 判据自己把进程撞死，那不是"红"，那是假红。
#ifdef Q_OS_WIN
        {
            // 撞的那颗：建页时写坏内存（0xc0000005）—— 和 10-22 那次「b64 会闪退」同一类
            struct Boom : LToolPlugin {
                int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
                QString id() const override { return QStringLiteral("boomprobe"); }
                QString name() const override { return QStringLiteral("故意撞一下的那颗"); }
                QWidget* createPage(QWidget*) override
                {
                    volatile int* p = nullptr;
                    *p = 7;
                    return nullptr;
                }
            } boom;
            // 好的那颗：页交给传进来的那棵树（判据自己造的页，判据自己收走）
            struct Good : LToolPlugin {
                int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
                QString id() const override { return QStringLiteral("goodprobe"); }
                QString name() const override { return QStringLiteral("正常给一张页的那颗"); }
                QWidget* createPage(QWidget* parent) override { return new QWidget(parent); }
            } good;
            // 空页那颗：接口自己说不干（这一支的说法和"撞异常"是两句话，得各钉一条）
            struct None : LToolPlugin {
                int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
                QString id() const override { return QStringLiteral("noneprobe"); }
                QString name() const override { return QStringLiteral("不给页的那颗"); }
                QWidget* createPage(QWidget*) override { return nullptr; }
            } none;
            QString err;
            QWidget* got = PluginHost::buildPage(&boom, QStringLiteral("boomprobe"), nullptr, &err);
            // 下面这几句能打完，本身就是"程序没跟着死"的证据（真没接住，这里就到不了了）
            check("撞异常的接口进得去也出得来：没给页（半建那张也没当成好页缓存）",
                  QStringLiteral("%1").arg(got == nullptr ? "null" : "widget"), QStringLiteral("null"));
            has("那句原因里带着异常码（0xC0000005 = 访问违例）", err.contains(QStringLiteral("0xC0000005")));
            has("那句原因点名是哪颗插件撞的（界面上那一行要能说清）", err.contains(QStringLiteral("boomprobe")));
            std::printf("INFO 撞一次之后界面拿到的那句话：%s\n", err.toUtf8().constData());
            // 正常那一支：外壳放行，且页的命跟着传进去的那棵树走 ——
            // 上一版这里图省事直接拿真插件（json）走了一遍，结果那颗壳里的 `m_page` 被我这张
            // 没进缓存的页顶掉了，后面"整扇窗关掉后再拿"那一条当场变红（判据自己弄坏被测对象）。
            QWidget* host = new QWidget;
            QWidget* p1 = PluginHost::buildPage(&good, QStringLiteral("goodprobe"), host, &err);
            check("接口正常给出页那一支：外壳放行、并且没说任何错话",
                  QStringLiteral("%1/%2").arg(p1 != nullptr ? "page" : "null").arg(err.isEmpty() ? "quiet" : "said"),
                  QStringLiteral("page/quiet"));
            check("那张页的命在传进来的那棵树上（不是宿主偷偷留一份）",
                  QStringLiteral("%1").arg(p1 && p1->parent() == host ? 1 : 0), QStringLiteral("1"));
            delete host;                 // 关掉那扇"窗"：页跟着没，判据不留自己造的东西在场上
            QWidget* p2 = PluginHost::buildPage(&none, QStringLiteral("noneprobe"), nullptr, &err);
            check("接口自己说不干（返回空）那一支：也不给页，但说的是另一句话",
                  QStringLiteral("%1").arg(p2 == nullptr), QStringLiteral("1"));
            has("那一支的说法里点着那颗的名字、并且不带异常码（两种病因不许混成一句）",
                err.contains(QStringLiteral("noneprobe")) && !err.contains(QStringLiteral("0x")));
            QWidget* nada = PluginHost::buildPage(nullptr, QStringLiteral("nada"), nullptr, &err);
            check("接口没实例化出来那一支：照样不给页、并且说清是哪颗",
                  QStringLiteral("%1").arg(nada == nullptr), QStringLiteral("1"));
            has("那一支的说法里点着那颗的名字", err.contains(QStringLiteral("nada")));
        }
        {
            // 同一层壳现在也罩着另外几条边界：递文本、执行动作、自报接哪些动作、自报 API、后台激活。
            // 判据直接调的就是那五颗 `*Into` —— 它们是产品**唯一**跨进插件的地方
            // （`feed()` / `invoke()` / `plugin()` / `startBackground()` 各自只调它们一次）。
            struct BoomAll : LToolPlugin {
                int apiVersion() const override { volatile int* z = nullptr; *z = 7; return 0; }
                QString id() const override { return QStringLiteral("boomall"); }
                QString name() const override { return QStringLiteral("条条都撞的那颗"); }
                QWidget* createPage(QWidget*) override { volatile int* z = nullptr; *z = 7; return nullptr; }
                bool feedText(const QString&) override { volatile int* z = nullptr; *z = 7; return true; }
                QStringList actions() const override { volatile int* z = nullptr; *z = 7; return {}; }
                bool invoke(const QString&, const QStringList&) override { volatile int* z = nullptr; *z = 7; return true; }
            } boomAll;
            struct OkAll : LToolPlugin {
                int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
                QString id() const override { return QStringLiteral("okall"); }
                QString name() const override { return QStringLiteral("条条都答得上的那颗"); }
                QWidget* createPage(QWidget* parent) override { return new QWidget(parent); }
                bool feedText(const QString& t) override { return !t.isEmpty(); }
                QStringList actions() const override { return { QStringLiteral("probe-action") }; }
                bool invoke(const QString& a, const QStringList&) override
                {
                    return a == QLatin1String("probe-action");
                }
            } okAll;
            QString why;
            bool ans = true;
            PluginHost::feedInto(&boomAll, QStringLiteral("boomall"), QStringLiteral("那段文本"), &ans, &why);
            check("递文本撞了：回不来，而且没把「撞」算成「它接住了」",
                  QStringLiteral("%1/%2").arg(why.contains(QStringLiteral("0xC0000005")) &&
                                              why.contains(QStringLiteral("递文本")) ? 1 : 0).arg(ans),
                  QStringLiteral("1/0"));
            ans = true;
            const bool feedOk = PluginHost::feedInto(&okAll, QStringLiteral("okall"),
                                                     QStringLiteral("那段文本"), &ans, &why);
            check("递文本没撞：答案照原样递出来（真插件答「是」就是「是」，外壳不改口）",
                  QStringLiteral("%1/%2/%3").arg(feedOk).arg(ans).arg(why.isEmpty() ? 1 : 0),
                  QStringLiteral("1/1/1"));
            ans = true;
            PluginHost::invokeInto(&boomAll, QStringLiteral("boomall"), QStringLiteral("probe-action"),
                                   QStringList(), &ans, &why);
            check("执行动作撞了：回不来、那句话点名是「执行动作」这一条边界",
                  QStringLiteral("%1/%2").arg(why.contains(QStringLiteral("执行动作")) &&
                                              why.contains(QStringLiteral("probe-action")) ? 1 : 0).arg(ans),
                  QStringLiteral("1/0"));
            ans = true;
            const bool invOk = PluginHost::invokeInto(&okAll, QStringLiteral("okall"),
                                                      QStringLiteral("probe-action"), QStringList(), &ans, &why);
            check("执行动作没撞：它答的「干成了」原样递回来",
                  QStringLiteral("%1/%2/%3").arg(invOk).arg(ans).arg(why.isEmpty() ? 1 : 0),
                  QStringLiteral("1/1/1"));
            QStringList acts = { QStringLiteral("脏的没清") };
            check("自报动作撞了：回不来，并且那份列表没留下一半",
                  QStringLiteral("%1").arg(PluginHost::actionsInto(&boomAll, QStringLiteral("boomall"),
                                                                   &acts, &why)
                                              ? 1 : 0),
                  QStringLiteral("0"));
            check("撞之前那份列表先清干净（不许把调用方留下的旧数据当成它报的）",
                  QStringLiteral("%1").arg(acts.isEmpty() ? 1 : 0), QStringLiteral("1"));
            acts.clear();
            const bool actsOk = PluginHost::actionsInto(&okAll, QStringLiteral("okall"), &acts, &why);
            check("自报动作没撞：它报的那份就是那份",
                  QStringLiteral("%1/%2").arg(actsOk ? 1 : 0)
                      .arg(acts == QStringList({ QStringLiteral("probe-action") }) ? 1 : 0),
                  QStringLiteral("1/1"));
            int api = 999;
            const bool apiCrash = PluginHost::apiVersionInto(&boomAll, QStringLiteral("boomall"), &api, &why);
            check("装载时那一问（实例自报 API）撞了：回不来，api 没留下脏数",
                  QStringLiteral("%1/%2").arg(apiCrash).arg(api == 0),
                  QStringLiteral("0/1"));
            api = 0;
            const bool apiOk = PluginHost::apiVersionInto(&okAll, QStringLiteral("okall"), &api, &why);
            check("装载时那一问没撞：报上来的就是它自己那版（主程序拿这个数判装不装）",
                  QStringLiteral("%1/%2").arg(apiOk).arg(api == LTOOL_PLUGIN_API_VERSION),
                  QStringLiteral("1/1"));
            void (*bangFn)() = [] { volatile int* z = nullptr; *z = 7; };
            void (*noopFn)() = [] {};
            const bool actCrash = PluginHost::activateInto(QStringLiteral("linuxdo"), bangFn, &why);
            const QString activateWhy = why;      // 留着当证据打出来（后面那句会把它清掉）
            check("后台激活撞了：回不来，那句话点名是「后台激活」这一条边界",
                  QStringLiteral("%1/%2").arg(actCrash).arg(why.contains(QStringLiteral("后台激活"))),
                  QStringLiteral("0/1"));
            std::printf("INFO 后台激活撞那一下，日志/界面拿到的那句话：%s\n",
                        activateWhy.toUtf8().constData());
            why.clear();
            const bool actOk = PluginHost::activateInto(QStringLiteral("okall"), noopFn, &why);
            check("后台激活没撞：回得来就不留原因（`startBackground()` 那个数只算真起来的）",
                  QStringLiteral("%1/%2").arg(actOk).arg(why.isEmpty() ? 1 : 0),
                  QStringLiteral("1/1"));
        }
#endif
        // ---- 「广告弹窗」那颗「从正在跑的进程里挑一个放行」：两颗纯函数 + 那份进程列表 ----
        {
            check("allowProcFor：映像名规整成名单里那一行（小写、去掉 .exe、没后缀的原样）",
                  QStringLiteral("%1|%2|%3|%4")
                      .arg(AdBlocker::allowProcFor(QStringLiteral("WeChat.exe")),
                           AdBlocker::allowProcFor(QStringLiteral("DingTalk.EXE")),
                           AdBlocker::allowProcFor(QStringLiteral("bar")),
                           AdBlocker::allowProcFor(QString())),
                  QStringLiteral("wechat|dingtalk|bar|"));
            const QStringList list = { QStringLiteral("chrome"), QStringLiteral("DingTalk") };
            check("自己就是名单里那一条 ⇒ 说「本来就在」，不许再加一遍",
                  AdBlocker::coveredByAllowProc(list, QStringLiteral("chrome.exe")),
                  QStringLiteral("chrome"));
            check("被更短的片段盖住也算盖住（那一侧的匹配就是「进程名包含片段」）",
                  AdBlocker::coveredByAllowProc(list, QStringLiteral("dingtalk.exe")),
                  QStringLiteral("DingTalk"));
            check("没盖住的那颗回空串（否则界面上那句「本来就在」是假的）",
                  AdBlocker::coveredByAllowProc(list, QStringLiteral("Notepad.exe")).isEmpty()
                      ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // 原来这里钉的是"名单里那行空的不能盖住一切"，但反向验打不动它（去掉那句跳过，
            // 函数回的还是空串、调用方照样当"没盖住"）—— 那是一条量不到东西的装饰性绿，
            // 而且那个输入真到不了这儿（存进名单前 `linesToPatterns` 就把空行丢了）。
            // 换成一条打得动的：**包含的方向**不能反（比这颗更长的片段盖不住它）。
            check("名单里那条比这颗更长 ⇒ 不算盖住（「进程名包含片段」这个方向不能反）",
                  AdBlocker::coveredByAllowProc({ QStringLiteral("wechat.exe") },
                                                QStringLiteral("WeChat.exe")).isEmpty()
                      ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            const QStringList names = Platform::runningImageNames();
            QSet<QString> lower;
            int dirty = 0;
            for (const QString& n : names) {
                lower.insert(n.toLower());
                if (n.trimmed().isEmpty() || n.contains(QLatin1Char('/')) || n.contains(QLatin1Char('\\')))
                    ++dirty;      // 那张表只该有映像名：混进路径或空行，界面就变成看不懂的一栏
            }
            std::printf("INFO 这台机上此刻列出的进程：%d 颗（去重后 %d 个名字；带路径或空的 %d 个）\n",
                        int(names.size()), int(lower.size()), dirty);
            check("runningImageNames 在这台机器上列得出来，而且名字里没有空串和路径",
                  QStringLiteral("%1/%2").arg(!names.isEmpty()).arg(dirty == 0),
                  QStringLiteral("1/1"));
            check("同一个 exe 跑好几份只占一行（判据量的就是「去重」这件事本身）",
                  QStringLiteral("%1/%2").arg(lower.size()).arg(names.size()),
                  QStringLiteral("%1/%1").arg(names.size()));
        }
        // ---- 整扇商店页关掉那一趟（他真正的形状：页挂在 `ToolsPage` 那棵树上，一关全没）----
        // 上面 delete 的是单张页；这里是**一扇窗里点开过的页一起没**。
        // 一颗一棵树：`createPage(tree)` 之后那张页的命就归这棵树，删掉树 = 关掉那扇窗。
        // 这里刻意**不刷 `DeferredDelete`**：判据手上还有一批"拆墙拆下来的旧卡片"排在待删表里，
        // 半路把它们真删掉会把别处还在用的控件一起带走（今天就在这一句上稳定崩了一轮，
        // 现象是 `sendPostedEvents(DeferredDelete)` 一进来就死在下一颗插件的建页上）。
        int safe = 0;
        for (const QString& id : hitIds) {
            {
                // 产品那条路，一步都不绕：开一扇商店窗 → 点开这张页（页就挂在这扇窗的树上）→ 关掉这扇窗。
                ToolsPage* one = new ToolsPage;
                const bool shown = one->showTool(id);
                if (!shown)
                    std::printf("INFO %s：这扇窗里没能点开（这一趟就没搭起来）\n", id.toUtf8().constData());
                delete one;
                for (int i = 0; i < 5; ++i)
                    QCoreApplication::processEvents();
            }
            // 顺序是**先递文本、再要页**：`page()` 会把壳里那颗 `m_page` 顶成新造的那张，
            // 那样"往已经没掉的页上写字"这一支就永远量不到了 —— 量不到不等于没有。
            const bool refused = !PluginHost::feed(id, QStringLiteral("123"));
            QString err;
            QWidget* w = PluginHost::page(id, nullptr, &err);
            QPointer<QWidget> wGuard(w);
            const bool usable = w != nullptr && !wGuard.isNull();
            if (refused && usable)
                ++safe;
            check((id + "：那扇窗关掉之后 递文本不接 / 再要一张用得动").toUtf8().constData(),
                  QStringLiteral("%1/%2").arg(refused ? QStringLiteral("refused") : QStringLiteral("fed"))
                      .arg(usable ? QStringLiteral("usable") : QStringLiteral("none")),
                  QStringLiteral("refused/usable"));
        }
        check("每颗插件都过了「那扇商店窗关掉后再拿」这一趟",
              QStringLiteral("%1/%2").arg(safe).arg(hitTotal), QStringLiteral("%1/%1").arg(hitTotal));
        std::printf("INFO 三趟按颗铺开的闪退判据：%d 颗插件，各过了「页被单删」「拆墙后再直达 ×2」「整扇窗关掉后再拿」\n",
                    evictTotal);
        std::printf("plugintest: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }

    // --about：「关于」这一页里那些不用联网就能定的事，逐条量清楚。
    // 版本号是不是真的从构建传进来、比较函数会不会把 1.10 排在 1.9 后面、
    // 服务器给的资产名能不能变成 "../"、那段要动安装目录的 PowerShell 有没有逃逸口、
    // 四颗按钮是不是真的摆在布局里（数 findChild 不等于界面可见，配置页那栏栽过）。
    if (args.contains("--about")) {
        int bad = 0;
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const auto has = [&bad](const char* name, bool ok) {
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok)
                ++bad;
        };
        const QString ver = AboutPage::currentVersion();
        has(QStringLiteral("版本号长得对（x.y.z，预发可以带 -beta.N）: [%1]").arg(ver).toUtf8().constData(),
            QRegularExpression(QStringLiteral("^\\d+\\.\\d+\\.\\d+(-[0-9A-Za-z.]+)?$")).match(ver).hasMatch());
        // 检查更新不再碰 api.github.com（匿名 60 次/小时的额度会被同一出口 IP 上的一切
        // 脚本吃光，他这次看到的 403 就是这么来的）：这里钉的是"走网页那条 + 直链推得出来"。
        check("最新那条走的是网页地址（不是限流的 api）", AboutPage::releasesPageUrl(),
              QStringLiteral("https://github.com/mateo-ki/ltool/releases/latest"));
        check("安装包直链按 tag + 版本号推得出来",
              AboutPage::downloadUrlFor(QStringLiteral("v1.2.3"), QStringLiteral("1.2.3")),
              QStringLiteral("https://github.com/mateo-ki/ltool/releases/download/v1.2.3/ltool-1.2.3-setup.exe"));
        check("给人点的发布页", AboutPage::releasesPageUrl(),
              QStringLiteral("https://github.com/mateo-ki/ltool/releases/latest"));

        // 版本比较：这一段最容易写错成字符串比（"10" < "9"）
        check("1.10 必须大于 1.9", QString::number(Utils::compareVersions("1.10", "1.9")), "1");
        check("0.5.0 等于自己", QString::number(Utils::compareVersions("0.5.0", "0.5.0")), "0");
        check("0.6.0 大于 0.5.9", QString::number(Utils::compareVersions("0.6.0", "0.5.9")), "1");
        check("0.6 小于 0.6.1（段数不齐也别判成相等）",
              QString::number(Utils::compareVersions("0.6", "0.6.1")), "-1");
        check("垃圾输入不炸也不判高（abc 当 0）",
              QString::number(Utils::compareVersions("abc", "0.0.0")), "0");
        check("tag 里的 v 前缀和尾巴都能剥掉", AboutPage::versionFromTag("v0.6.0-beta"),
              QStringLiteral("0.6.0"));
        check("tag 里没数字就返回空（别拿整个 tag 当版本）",
              AboutPage::versionFromTag("nightly"), QString());
        // 预发/稳定这条通道靠的就是这三条。GitHub 的 /releases/latest 实测跳过 pre-release
        // （拿 microsoft/PowerToys 对过：列表最新是 v0.102.2862.0 pre=True，latest 回的是
        // v0.101.2362.0），所以稳定通道不用改代码；而装预发的人要能看见正式版可升 ——
        // 那就是"同数字时带后缀的更旧"这一条说了算。
        check("0.8.1-beta.1 必须小于 0.8.1（预发是奔着它去的，不是它之后）",
              QString::number(Utils::compareVersions("0.8.1-beta.1", "0.8.1")), "-1");
        check("0.8.1-beta.2 大于 0.8.1-beta.1（同数字下后缀自己排）",
              QString::number(Utils::compareVersions("0.8.1-beta.2", "0.8.1-beta.1")), "1");
        check("0.9 大于 0.8.1-beta.9（数字段先分胜负，后缀不参与）",
              QString::number(Utils::compareVersions("0.9", "0.8.1-beta.9")), "1");

        // 资产名是服务器给的：路径分隔符、开头一个点、引号都不能带进落盘路径
        const QString evil = AboutPage::safeFileName(QStringLiteral("../evil.exe"));
        has(("资产名里的 ../ 被去掉：" + evil).toUtf8().constData(),
            !evil.contains('/') && !evil.contains('\\') && !evil.startsWith('.'));
        const QString quotedName = AboutPage::safeFileName(QStringLiteral("a\"b.exe"));
        has(("资产名里的引号被换掉：" + quotedName).toUtf8().constData(),
            !quotedName.contains('"'));

        // 拼给 PowerShell 的那段：值全部走单引号字面量，翻倍之后不许留逃逸口
        check("psQuote 把单引号翻倍", AboutPage::psQuote(QStringLiteral("a'b")),
              QStringLiteral("'a''b'"));
        const QString q = AboutPage::psQuote(QStringLiteral("x'; Stop-Process; #"));
        const QString inner = q.mid(1, q.size() - 2);
        bool paired = true;
        for (int i = 0; i < inner.size(); ++i) {
            if (inner.at(i) != QLatin1Char('\''))
                continue;
            if (i + 1 >= inner.size() || inner.at(i + 1) != QLatin1Char('\'')) {
                paired = false;
                break;
            }
            ++i;
        }
        has("psQuote 对注入串：单引号只能成对出现", paired
            && q.startsWith(QLatin1Char('\'')) && q.endsWith(QLatin1Char('\'')));

        const QString self = QCoreApplication::applicationFilePath();
        const QString appDir = QCoreApplication::applicationDirPath();
        QString err;
        const QString script = AboutPage::updateScript(self, appDir, 4321, &err);
        std::printf("INFO 那段脚本第一行：%s\n",
                    script.section('\n', 0, 0).toUtf8().constData());
        has(("脚本建得出来（拿刚编好的 exe 当安装包试）：" + err).toUtf8().constData(),
            !script.isEmpty());
        has("脚本里静默安装 + /DIR 指回现在这个目录",
            script.contains("/VERYSILENT") && script.contains(AboutPage::psQuote(appDir)));
        has("脚本先等我们自己退出（按 pid 轮询），装完再拉起来",
            script.contains("Get-Process -Id 4321") && script.contains("Start-Process"));
        err.clear();
        const QString withQuote = AboutPage::updateScript(self + QStringLiteral("'x"), appDir, 1, &err);
        has("路径里带单引号就整段拒绝（不给 PowerShell 留逃逸口）",
            withQuote.isEmpty() && !err.isEmpty());
        err.clear();
        has("安装包不存在也拒绝",
            AboutPage::updateScript(QStringLiteral("Z:/nope.exe"), appDir, 1, &err).isEmpty()
                && !err.isEmpty());

        // 页面本身：四颗按钮要在布局里、初始不该已经发过请求、下载/安装初始禁用
        AboutPage page;
        QSet<const QWidget*> laidOut;
        std::function<void(QLayout*)> walk = [&](QLayout* l) {
            if (!l)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* w = it->widget()) {
                    laidOut.insert(w);
                    walk(w->layout());
                } else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        walk(page.layout());
        int onLayout = 0;
        for (const char* objName : { "checkUpdate", "downloadUpdate", "installUpdate", "openReleases" }) {
            QWidget* w = page.findChild<QWidget*>(QString::fromLatin1(objName));
            if (w && laidOut.contains(w))
                ++onLayout;
        }
        check("检查/下载/装并重启/发布页 四颗按钮都在布局里", QString::number(onLayout),
              QStringLiteral("4"));
        auto* status = page.findChild<QLabel*>(QStringLiteral("updateStatus"));
        check("刚建好还没碰过网络（状态还是初始那句）",
              status ? status->text() : QStringLiteral("(没有状态行)"),
              QStringLiteral("还没检查过"));
        bool downloadOff = false, installOff = false;
        for (const QPushButton* b : page.findChildren<QPushButton*>()) {
            if (b->objectName() == QLatin1String("downloadUpdate"))
                downloadOff = !b->isEnabled();
            if (b->objectName() == QLatin1String("installUpdate"))
                installOff = !b->isEnabled();
        }
        has("没查出结果之前下载/装都不给点", downloadOff && installOff);
        // 界面显示的版本必须和编译期那个同源（不是"我记得是 0.6.0"）
        bool showsVersion = false;
        for (const QLabel* l : page.findChildren<QLabel*>())
            if (l->text().contains(ver))
                showsVersion = true;
        has(("页面上显示的版本 == 编译期传进来的 " + ver).toUtf8().constData(), showsVersion);

        std::printf("about: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }

    // --updatecheck：真去问一次 github（走配置里那条下载代理）。这是唯一能量出
    // "这台机器现在能不能问到新版"的入口 —— 点的是界面同一颗按钮，跑的是同一条异步路径。
    if (args.contains("--updatecheck")) {
        AboutPage page;
        auto* btn = page.findChild<QPushButton*>(QStringLiteral("checkUpdate"));
        auto* status = page.findChild<QLabel*>(QStringLiteral("updateStatus"));
        if (!btn || !status) {
            std::printf("FAIL 找不到「检查更新」那颗按钮或状态行\n");
            return 1;
        }
        btn->click();
        QString text = status->text();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 40000 && text.contains(QStringLiteral("正在问"))) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
            text = status->text();
        }
        const bool done = !text.contains(QStringLiteral("正在问"));
        std::printf("INFO 用时 %lld ms\n", t.elapsed());
        std::printf("INFO 界面状态：%s\n", text.toUtf8().constData());
        std::printf("%s 检查更新跑完并给出了一句实话\n", done ? "PASS" : "FAIL");
        std::printf("updatecheck: 失败 %d 项\n", done ? 0 : 1);
        return done ? 0 : 1;
    }

    // --diskbranch：验"边扫边报的一级分支大小"和最后树上的是不是一回事。
    // 这条必须能红：如果拆出去的孩子没算进父分支（或者算了两遍），
    // 完成时报出来的数就和定稿树对不上 —— 那界面上先给用户看的数字是假的。
    if (args.contains("--diskbranch")) {
        int bad = 0;
        QVector<QPair<QString, qint64>> last;
        qint64 firstHitMs = 0;
        QElapsedTimer wall;
        wall.start();
        QString scanRoot = QStringLiteral("C:/");
        const int flagIdx = args.indexOf(QStringLiteral("--diskbranch"));
        if (flagIdx >= 0 && flagIdx + 1 < args.size() && !args.at(flagIdx + 1).startsWith(QLatin1Char('-'))) {
            scanRoot = args.at(flagIdx + 1);
        } else if (const QByteArray envRoot = qgetenv("LTOOL_DISKBRANCH_DIR"); !envRoot.isEmpty()) {
            scanRoot = QString::fromUtf8(envRoot);
        }
        std::printf("INFO 磁盘分支扫描目标: %s\n", scanRoot.toUtf8().constData());
        std::fflush(stdout);
        const DiskScan::Tree tree = DiskScan::run(scanRoot, 1 << 20, [&](const DiskScan::Progress& p) {
            if (!p.doneBranches.isEmpty() && firstHitMs == 0)
                firstHitMs = wall.elapsed();
            last = p.doneBranches;
        });
        const qint64 totalMs = wall.elapsed();
        int rootKids = 0;
        for (int i = tree.firstKid[0]; i < tree.firstKid[0] + tree.kidCount[0]; ++i)
            ++rootKids;
        const auto say = [&bad](const char* name, bool ok, const QString& got) {
            std::printf("%s %s：%s\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
            if (!ok)
                ++bad;
            std::fflush(stdout);
        };
        say("报出来的分支数 == 树上的一级目录数",
            last.size() == rootKids, QStringLiteral("%1 / %2 个").arg(last.size()).arg(rootKids));
        QString mismatch;
        qint64 sum = 0;
        for (const auto& pr : last) {
            const int node = tree.nodeForPath(tree.rootPath + "/" + pr.first);
            const qint64 treeBytes = node >= 0 ? tree.size[node] : -1;
            sum += pr.second;
            if (treeBytes != pr.second && mismatch.isEmpty())
                mismatch = QStringLiteral("%1 报 %2 字节、树上 %3 字节")
                               .arg(pr.first).arg(pr.second).arg(treeBytes);
        }
        say("每个分支报的数都和定稿树一模一样", mismatch.isEmpty(),
            mismatch.isEmpty() ? QStringLiteral("%1 个分支全对上").arg(last.size()) : mismatch);
        say("一级分支之和 = 全盘减去卷根直属", sum + tree.fileBytes[0] == tree.total,
            QStringLiteral("%1 + %2 = %3").arg(sum).arg(tree.fileBytes[0]).arg(tree.total));
        const QString when = QStringLiteral("第一个分支 %1ms 就出数（整趟 %2ms）")
                                 .arg(firstHitMs).arg(totalMs);
        say("边扫边出数：第一个分支在整趟结束之前就报出来了",
            (firstHitMs > 0 && firstHitMs <= totalMs) || (totalMs < 100 && !last.isEmpty()),
            when);
        std::printf("diskbranch: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }

    // --diskui：磁盘页那条链整条走一遍（点「整理」→ 途中有没有边扫边出数 →
    // 定稿 → 换盘再换回来吃不吃缓存）。判据在插件里 DiskPage.cpp，因为它要点真按钮、
    // 要看真行卡片，主程序链接不到那些类型。这条慢（要真扫一整盘），所以不进 --plugintest。
    if (args.contains("--diskui"))
        return runPluginSelftest(QStringLiteral("disk"), "ltoolPluginUiSelftest");

    // --diskscale：量"多线程到底有没有用"。同一棵树按 1/2/4/8/16 线程各扫一遍
    // （第一趟之后目录项都在系统缓存里，比的是扩展性而不是冷缓存）。
    // 加这条是因为"磁盘分析太慢"要先分清：是活儿本来就这么多（那就得换 MFT 那条路），
    // 还是线程没吃满（那就调线程数/拆任务）。
    if (args.contains("--diskscale")) {
        static const int kCounts[] = { 1, 2, 4, 8, 16 };
        qint64 base = 0;
        for (int c : kCounts) {
            qputenv("LTOOL_DISKTHREADS", QByteArray::number(c));
            QElapsedTimer t;
            t.start();
            const DiskScan::Tree tree = DiskScan::run("C:/", 1 << 20);
            const qint64 ms = t.elapsed();
            const DiskScan::Timing tm = DiskScan::lastTiming();
            if (!base)
                base = ms;
            std::printf("  %2d 线程: %lld ms（扫盘 %lld、建树 %lld）%lld 目录 / %lld 文件 / %.2f GB"
                        "  相对第一趟 %.2fx\n",
                        c, ms, tm.walkMs, tm.mergeMs, tm.dirsSeen, tm.files,
                        tree.total / 1073741824.0, double(base) / double(ms));
            std::fflush(stdout);
        }
        qputenv("LTOOL_DISKTHREADS", QByteArray());
        std::printf("diskscale: 失败 0 项\n");
        return 0;
    }

    if (args.contains("--regscan"))
        return runRegScan();

    if (args.contains("--startupscan"))
        return runStartupScan();

    if (args.contains("--fw"))
        return runFwProbe(args);

    if (args.contains("--uptime"))
        return runUiTiming(args);

    if (args.contains("--adscan"))
        return runAdScan();

    if (args.contains("--fwtry"))
        return runFwTry(args);

    if (args.contains("--fwallow") || args.contains("--fwremove"))
        return runFwWrite(args);

    // --whohastest <路径> [kill]：无界面跑一遍占用枚举，把每一步的结果打出来。
    // 带 kill 再走「解除占用」那一步（逐个结束占用者后再数一遍）—— 这两条是第七道缝
    // （platform/FileOps_*）搬家前后的对照物，所以打得要细：谁、几个句柄、结束成没成。
    if (args.contains("--whohastest")) {
        const QStringList rest = args.mid(args.indexOf("--whohastest") + 1);
        const QString target = rest.value(0);
        QString err;
        const auto holders = FileOps::whoHas(target, &err);
        std::printf("whohastest: target=%s holders=%d err=%s\n", target.toUtf8().constData(),
                    int(holders.size()), err.toUtf8().constData());
        for (const FileOps::Holder& h : holders)
            std::printf("  pid=%llu %s handles=%d\n", h.pid, h.processName.toUtf8().constData(), h.handles);
        if (rest.value(1) == QLatin1String("kill")) {
            for (const FileOps::Holder& h : holders) {
                QString killErr;
                const bool ok = FileOps::killProcess(h.pid, &killErr);
                std::printf("  kill pid=%llu ok=%d err=%s\n", h.pid, ok ? 1 : 0,
                            killErr.toUtf8().constData());
            }
            QString err2;
            const auto left = FileOps::whoHas(target, &err2);
            std::printf("whohastest: 结束后 holders=%d err=%s\n", int(left.size()),
                        err2.toUtf8().constData());
        }
        std::fflush(stdout);
        return 0;
    }

    // --archivetest extract <包> <目标目录> / make <输入> <输出> [7z] [档位]
    //                 / probe <文件> / progress <输入> <输出> [7z] [第几次按取消]
    // 无界面跑一遍压缩包读写，给回归测试用；probe 只回答"右键点它会当成压缩包吗"；
    // progress 专门量进度与取消这两条只有界面在用的线（7z 后端换了 bit7z，它们必须重验）。
    if (args.contains("--archivetest")) {
        const QStringList a = args.mid(args.indexOf("--archivetest") + 1);
        if (a.value(0) == QLatin1String("extract")) {
            const Archive::Result r = Archive::extract(a.value(1), a.value(2), nullptr, nullptr);
            std::printf("extract ok=%d files=%d dirs=%d bytes=%lld target=%s err=%s\n",
                        r.ok ? 1 : 0, r.files, r.dirs, r.bytes,
                        r.target.toUtf8().constData(), r.error.toUtf8().constData());
        } else if (a.value(0) == QLatin1String("probe")) {
            std::printf("probe looksLikeArchive=%d\n", Archive::looksLikeArchive(a.value(1)) ? 1 : 0);
        } else if (a.value(0) == QLatin1String("progress")) {
            const bool seven = a.value(3) == QLatin1String("7z");
            // 第 4 个参数：回调第几次时"用户按了取消"。给个不可能达到的数就是全程不取消。
            bool stopsOk = false;
            const int stopAfter = a.value(4).toInt(&stopsOk) && stopsOk ? a.value(4).toInt() : 3;
            qint64 calls = 0, last = -1, peak = 0, total = 0;
            bool monotonic = true;
            const Archive::Report rep = [&](const Archive::Progress& pr) {
                ++calls;
                total = pr.total;
                if (pr.done < last)
                    monotonic = false;
                last = pr.done;
                peak = qMax(peak, pr.done);
                return calls < qint64(stopAfter);
            };
            const Archive::Result r = Archive::compress({ a.value(1) }, a.value(2), seven, rep, nullptr);
            const QString part = QDir::toNativeSeparators(a.value(2)) + QStringLiteral(".part");
            std::printf("progress ok=%d cancelled=%d calls=%lld total=%lld peak=%lld 单调=%d "
                        "半成品还在=%d 成品在=%d err=%s\n",
                        r.ok ? 1 : 0, r.cancelled ? 1 : 0, calls, total, peak,
                        monotonic ? 1 : 0, QFile::exists(part) ? 1 : 0,
                        QFile::exists(QDir::toNativeSeparators(a.value(2))) ? 1 : 0,
                        r.error.toUtf8().constData());
        } else {
            const bool seven = a.value(3) == QLatin1String("7z");
            // 第 5 个参数：7z 的档位（0-9，就是 7-Zip 自己的 -mx 数法，默认 5 = 它的默认档）。
            // zip 那条不吃这个数（libarchive 的 deflate 恒为 6 档）。
            bool lvlOk = false;
            const int level = a.value(4).toInt(&lvlOk);
            const Archive::Result r = Archive::compress({ a.value(1) }, a.value(2), seven, nullptr,
                                                        nullptr, lvlOk ? level : 5);
            std::printf("make ok=%d files=%d dirs=%d bytes=%lld target=%s err=%s\n",
                        r.ok ? 1 : 0, r.files, r.dirs, r.bytes,
                        r.target.toUtf8().constData(), r.error.toUtf8().constData());
        }
        std::fflush(stdout);
        return 0;
    }

    // --snaprect：吸附框贴不贴边。判据本体在 plugins/capture（导出 ltoolSnaprectRun）。
    if (args.contains("--snaprect"))
        return runPluginSelftest(QStringLiteral("capture"), "ltoolSnaprectRun",
                                 SelftestArg::None, 0, QString(), "snaprect");


    // --appdirs：配置页「应用目录」那条路（往里 N 层找 exe）的无界面回归。
    // 夹具建在沙箱数据目录里（见上面那份白名单），路径全程是绝对路径 ——
    // 清理代码拿相对路径去 removeRecursively，删的就是"当时的工作目录"，
    // 这条曾经把整个工程目录删掉过一次，所以这里连删夹具都只删自己建的那一个子目录。
    if (args.contains("--appdirs")) {
        int bad = 0;
        const QString root = Utils::dataDir() + "/appdirs-fixture";
        const auto check = [&bad](const char* name, const QStringList& got,
                                  const QStringList& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.join(",").toUtf8().constData(), want.join(",").toUtf8().constData());
        };
        const QString flt = Utils::dataDir() + "/appdirs-filter";
        if (!root.startsWith(QDir::tempPath()) || !root.contains("appdirs-fixture")
            || !flt.startsWith(QDir::tempPath()) || !flt.contains("appdirs-filter")) {
            std::printf("FAIL 夹具路径不对，什么都不建：%s\n", root.toUtf8().constData());
            std::fflush(stdout);
            return 1;
        }
        QDir().mkpath(root + QString::fromUtf8("/中文目录/三层/四层"));
        const auto mk = [&root](const QString& rel) {
            QFile f(root + "/" + rel);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write("MZ");
            f.close();
            return true;
        };
        // 再建一棵"像不像主程序"的夹具树。上面那几个 exe 只写了 "MZ" 两个字节，PE 头根本读不
        // 出来 —— 那是刻意的：解析失败不能变成"什么都不收"（真应用照样收得到），这条由老四条顺带守住。
        const auto mkPe = [&flt](const QString& rel, int subsystem) {
            QFile f(flt + "/" + rel);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            QByteArray img(512, '\0');
            img[0] = 'M'; img[1] = 'Z';
            img[60] = char(64);                       // e_lfanew = 64
            img[64] = 'P'; img[65] = 'E';
            img[64 + 24 + 68] = char(subsystem);      // Subsystem：可选头 +68（2=GUI，3=控制台）
            f.write(img);
            f.close();
            return true;
        };
        QDir().mkpath(flt + "/7.4.1");
        QDir().mkpath(flt + "/locales");
        // 光有自造字节不够 —— 上一版实现和夹具按同一个错偏移写，测试当场白过。
        // 再拷两个系统里现成的：ping 是控制台、notepad 是 GUI，让真实 PE 也过一遍这条判据。
        const QString sysroot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
        const bool copiedReal =
            QFile::copy(sysroot + "/System32/ping.exe", flt + "/ping.exe")
            && QFile::copy(sysroot + "/System32/notepad.exe", flt + "/notepad.exe");
        const bool built = mk("one.exe") && mk(QString::fromUtf8("中文目录/中文two.exe"))
                           && mk("中文目录/三层/three.exe")
                           && mk("中文目录/三层/四层/four.exe")
                           && mk("Uninstall Bar.exe")
                           && mkPe("gui.exe", 2) && mkPe("tool.exe", 3)
                           && mkPe("FooUpdater.exe", 2) && mkPe("gui2.exe", 2)
                           && mkPe("7.4.1/Inside.exe", 2) && mkPe("locales/X.exe", 2)
                           && copiedReal;
        if (!built)
            std::printf("FAIL 夹具没建全（中文路径写不进去？）\n");

        QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        // 扫出来的"来自目录"的那批名字，就是这条功能的全部产出
        const auto namesWith = [&](const QStringList& dirs, int depth) {
            cfg.setValue("apps/roots", dirs);
            cfg.setValue("apps/depth", depth);
            cfg.sync();
            QStringList out;
            for (const AppEntry& e : AppScanner::scan())
                if (e.category == QLatin1String("folder"))
                    out << e.name;
            out.sort();   // 按码元排：ASCII 名字在中文前面，期望值也照这个写
            return out;
        };
        const QString nat = QDir::toNativeSeparators(root);
        const QString cn = QString::fromUtf8("中文two");
        check("层数 0 只看这个目录本身", namesWith({nat}, 0), {"one"});
        check("层数 1 进一层（中文目录要能进去）", namesWith({nat}, 1), {"one", cn});
        check("层数 2 进两层", namesWith({nat}, 2), {"one", "three", cn});
        check("层数 3 进三层", namesWith({nat}, 3), {"four", "one", "three", cn});
        check("卸载器不算应用", namesWith({nat}, 3).contains("Uninstall Bar") ? QStringList{"混进来了"}
                                                                             : QStringList{},
              QStringList{});
        check("直接指到一个 exe 上也收", namesWith({QDir::toNativeSeparators(
                  root + QString::fromUtf8("/中文目录/中文two.exe"))}, 3), {cn});
        check("目录不存在只是跳过，不报错", namesWith({"Q:/没有这个东西/ltool"}, 3), QStringList{});
        // ---- "像不像主程序"那两道：控制台子系统、名字/路径像助手，都不该进应用列表 ----
        const QString fltNat = QDir::toNativeSeparators(flt);
        check("目录里只留看起来是主程序的（命令行工具/助手名/版本目录/运行时目录都跳过）",
              namesWith({fltNat}, 1), QStringList{ "gui", "gui2", "notepad" });
        check("真实系统 exe 也算得对：ping（控制台）被跳、notepad（GUI）留下",
              namesWith({fltNat}, 1).contains("ping") ? QStringList{ "ping 混进来了" } : QStringList{},
              QStringList{});
        // 同目录已有主程序就不再多收一个：点名 gui.exe 之后，整个 flt 目录只留它
        // （notepad.exe 也在这棵树里，所以它一起被"同目录已有主程序"这条挡掉 —— 要它就再点名一次）
        check("点名了同目录的主程序，同目录其余 exe 不再自动塞进来",
              namesWith({fltNat, QDir::toNativeSeparators(flt + "/gui.exe")}, 1),
              QStringList{ "gui" });
        check("点名指到那个命令行 exe：一定要它，不过滤",
              namesWith({QDir::toNativeSeparators(flt + "/tool.exe")}, 3), QStringList{ "tool" });
        // 搜索框读的是 apps.json（loadIndex），不是 scan() 现扫的那份 ——
        // 所以还要验一遍"存进去再读回来，目录来源的那些条一条没丢"
        cfg.setValue("apps/roots", QStringList{nat});
        cfg.setValue("apps/depth", 3);
        cfg.sync();
        AppScanner::saveIndex(AppScanner::scan());
        QStringList back;
        for (const AppEntry& e : AppScanner::loadIndex())
            if (e.category == QLatin1String("folder"))
                back << e.name;
        back.sort();
        check("存进 apps.json 再读回来还在", back, {"four", "one", "three", cn});
        cfg.remove("apps/roots");
        cfg.remove("apps/depth");
        cfg.sync();
        if (!QDir(root).removeRecursively())
            std::printf("FAIL 夹具没删干净：%s\n", root.toUtf8().constData());
        if (!QDir(flt).removeRecursively())
            std::printf("FAIL 过滤夹具没删干净：%s\n", flt.toUtf8().constData());
        std::printf("appdirs: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad;
    }

    // --cfgcheck：离屏建一次配置页，把表单行名数一遍。
    // 为什么要这条：这次「下载代理」在配置菜单里出现了两遍（回放历史时那一行被插了两次），
    // 而配置页是 GUI —— 我不往他桌面上点，就只能把"某个行名出现几次"变成可断言的数字。
    // 顺带盯住那行从来没生效过的「扫描根目录」别再被人加回来。沙箱 APPDATA，不碰真实配置。
    if (args.contains("--cfgcheck")) {
        int bad = 0;
        ConfigPage page;
        const auto labels = page.findChildren<QLabel*>();
        const auto rowCount = [&labels](const QString& name) {
            int n = 0;
            for (const QLabel* l : labels)
                if (l->text().trimmed() == name)
                    ++n;
            return n;
        };
        const auto hasRow = [&rowCount](const QString& name) { return rowCount(name) > 0; };
        const auto say = [&bad](const char* what, bool ok, const QString& got) {
            std::printf("%s %s：%s\n", ok ? "ok  " : "FAIL", what, got.toUtf8().constData());
            if (!ok)
                ++bad;
            std::fflush(stdout);
        };
        const int proxy = rowCount(QStringLiteral("下载代理："));
        say("「下载代理」只有一行", proxy == 1, QStringLiteral("数到 %1 行").arg(proxy));
        const int roots = rowCount(QStringLiteral("扫描根目录："));
        say("没生效过的「扫描根目录」已拿掉", roots == 0, QStringLiteral("数到 %1 行").arg(roots));
        const bool hasList = hasRow(QStringLiteral("项目列表："));
        const bool hasDepth = hasRow(QStringLiteral("往里找几层："));
        say("应用那一栏在（列表 + 层数）", hasList && hasDepth,
            QStringLiteral("项目列表=%1 往里找几层=%2").arg(hasList ? 1 : 0).arg(hasDepth ? 1 : 0));
        int addExeBtn = 0;
        int addDirBtn = 0;
        for (QPushButton* b : page.findChildren<QPushButton*>()) {
            if (b->objectName() == QStringLiteral("addAppExe"))
                ++addExeBtn;
            if (b->objectName() == QStringLiteral("addAppDir"))
                ++addDirBtn;
        }
        say("「添加应用…」和「添加目录…」各只有一个", addExeBtn == 1 && addDirBtn == 1,
            QStringLiteral("应用=%1 目录=%2").arg(addExeBtn).arg(addDirBtn));
        const auto lists = page.findChildren<QListWidget*>();
        say("列表控件真的建出来了", !lists.isEmpty(),
            QStringLiteral("%1 个 QListWidget").arg(lists.size()));
        // 上面那两条只数得到"控件建没建"，数不出"摆没摆进布局"。应用目录那一栏就是这么
        // 在页面上隐身的：分组框 new 出来时认了 this 当父件，却从没 addWidget 到那一列，
        // 于是标签 findChild 找得到、--cfgcheck 一片绿，界面上却一行都没有（2026-10-12 他报的）。
        // 所以这条把"布局里能走到"当成结构不变量，逐个分组框验一遍，谁没挂上就点谁的名。
        QSet<const QWidget*> laidOut;
        std::function<void(QLayout*)> walk;
        // 「访问一个控件」这件事只写一遍：它自己的布局 + 两种"内容不是布局项"的容器
        //（滚动区的 viewport 件、tab 的每一页）。上一版这里只有 walk() 一条递归，
        // 配置页拆成 通用/外观 两个 tab 之后当场把 7 个分组框全报成"没摆进布局" ——
        // 因为 tab 的第一页本身就是那颗 QScrollArea，从它走只能拿到它自己的 layout()。
        std::function<void(QWidget*)> visit = [&](QWidget* w) {
            if (!w || laidOut.contains(w))
                return;
            laidOut.insert(w);
            walk(w->layout());
            if (auto* sc = qobject_cast<QScrollArea*>(w))   // 滚动区里那层不是布局项
                if (sc->widget())
                    visit(sc->widget());
            if (auto* tw = qobject_cast<QTabWidget*>(w))    // tab 的非当前页也取不到布局项
                for (int i = 0; i < tw->count(); ++i)
                    visit(tw->widget(i));
        };
        walk = [&](QLayout* l) {
            if (!l)
                return;
            for (int i = 0; i < l->count(); ++i) {
                QLayoutItem* it = l->itemAt(i);
                if (!it)
                    continue;
                if (QWidget* w = it->widget())
                    visit(w);
                else if (QLayout* sub = it->layout())
                    walk(sub);
            }
        };
        visit(&page);
        QString orphans;
        int groups = 0;
        for (const QGroupBox* g : page.findChildren<QGroupBox*>()) {
            ++groups;
            if (!laidOut.contains(g))
                orphans += QStringLiteral("「%1」").arg(g->title());
        }
        say("每个分组框都真的摆在布局里（没摆=界面上看不见）", orphans.isEmpty(),
            orphans.isEmpty() ? QStringLiteral("%1 个分组框全在布局里").arg(groups) : orphans);
        // ---- 2026-10-20 他把配置页拆成「通用 / 外观」两个 tab，日间夜间那颗搬进外观 ----
        // 这三条盯的是搬家的形状，不是"有没有 tab"：
        // ① 就两个、名字对得上（多一个少一个都算没照做）；
        // ② 那两颗单选**在外观这一页上**（搬一半 = 界面上找不到，比不搬更糟）；
        // ③ 通用那一页不许长出外观的东西、外观那一页也不许被塞进别的分组框 ——
        //    这条是防"以后加开关的人随手往外观页里塞下载代理"那种回潮。
        {
            auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("cfgTabs"));
            const QStringList tabNames = [&] {
                QStringList n;
                if (tabs)
                    for (int i = 0; i < tabs->count(); ++i)
                        n << tabs->tabText(i);
                return n;
            }();
            say("配置页正好两个 tab，顺序是 通用/外观",
                tabNames.join(QStringLiteral(" / ")) == QStringLiteral("通用 / 外观"),
                tabNames.isEmpty() ? QStringLiteral("没找到 cfgTabs 那颗 QTabWidget")
                                   : tabNames.join(QStringLiteral(" / ")));
            QWidget* generalPage = tabs && tabs->count() > 0 ? tabs->widget(0) : nullptr;
            QWidget* lookPage = tabs && tabs->count() > 1 ? tabs->widget(1) : nullptr;
            const auto radios = page.findChildren<QRadioButton*>();
            int onLook = 0, elsewhere = 0;
            for (const QRadioButton* r : radios) {
                if (lookPage && lookPage->isAncestorOf(const_cast<QRadioButton*>(r)))
                    ++onLook;
                else
                    ++elsewhere;
            }
            say("夜间/日间那两颗单选都在「外观」这一页上（不在通用那一页）",
                radios.size() == 2 && onLook == 2 && elsewhere == 0,
                QStringLiteral("找到 %1 颗 · 在外观页 %2 颗 · 别处 %3 颗")
                    .arg(radios.size()).arg(onLook).arg(elsewhere));
            int generalGroups = 0, lookGroups = 0;
            for (QGroupBox* g : page.findChildren<QGroupBox*>()) {
                if (generalPage && generalPage->isAncestorOf(g))
                    ++generalGroups;
                if (lookPage && lookPage->isAncestorOf(g))
                    ++lookGroups;
            }
            say("其余分组框全在通用那一页、外观那一页只有「外观」自己这一颗分组框",
                generalGroups == groups - 1 && lookGroups == 1,
                QStringLiteral("通用页 %1 颗 · 外观页 %2 颗 · 整页共 %3 颗")
                    .arg(generalGroups).arg(lookGroups).arg(groups));
        }

        // 「语言与环境」已剥离为独立插件，配置页不再塞入该分组框
        {
            QStringList titles;
            for (const QGroupBox* g : page.findChildren<QGroupBox*>())
                if (g->title().contains(QStringLiteral("语言")))
                    titles << g->title();
            say("配置页不再有「语言」分组框（已剥离至插件）", titles.isEmpty(),
                titles.isEmpty() ? QStringLiteral("已剥离至插件") : titles.join(QStringLiteral("；")));
        }

        // 设置窗那一面：菜单只剩四行，而且每一行点开都真建出一张页。
        // 这条盯的是"删掉四行菜单之后有没有留下点不开的空行"——残留入口就是没删干净。
        {
            SettingsDialog dlg;
            dlg.setAttribute(Qt::WA_DontShowOnScreen, true);
            auto* menu = dlg.findChild<QListWidget*>(QStringLiteral("menuList"));
            QStringList names;
            if (menu) {
                for (int i = 0; i < menu->count(); ++i)
                    names << menu->item(i)->text();
            }
            say("菜单正好四行、顺序是 网站/插件商店/配置/关于",
                names.join(QStringLiteral(" / ")) == QStringLiteral("网站 / 插件商店 / 配置 / 关于"),
                names.join(QStringLiteral(" / ")));
            auto* stack = dlg.findChild<QStackedWidget*>();
            int built = 0;
            for (int i = 0; menu && i < menu->count(); ++i) {
                menu->setCurrentRow(i);            // currentRowChanged → ensurePage，和真人点同一条路
                QCoreApplication::processEvents();
                QWidget* cur = stack ? stack->currentWidget() : nullptr;
                if (cur && cur->findChildren<QWidget*>().size() > 1)
                    ++built;
            }
            say("每一行点开都不是一块空面板", built == (menu ? menu->count() : 0),
                QStringLiteral("有内容的 %1 / 共 %2 行").arg(built).arg(menu ? menu->count() : 0));

            // 图标这一条盯两件事：「插件商店」那颗**不再是第一个字**，其余三行**还是**。
            // 底块颜色从图标自己身上取（取出现得最多的那个不透明色 = 那块圆角色），
            // 这样和 Utils::letterIcon(名字, 同一块色) 比才是公道的比法 —— 不然换个色就永远不等。
            const auto imgOf = [](const QIcon& i) { return i.pixmap(48, 48).toImage(); };
            const auto tileColor = [](const QImage& im) {
                QMap<QRgb, int> hist;
                for (int y = 0; y < im.height(); ++y)
                    for (int x = 0; x < im.width(); ++x)
                        if (qAlpha(im.pixel(x, y)) > 200)
                            ++hist[QRgb(im.pixel(x, y))];
                QRgb best = 0;
                int n = -1;
                for (auto it = hist.constBegin(); it != hist.constEnd(); ++it)
                    if (it.value() > n) { n = it.value(); best = it.key(); }
                return QColor(best);
            };
            QStringList bagged, lettered;
            for (int i = 0; menu && i < menu->count(); ++i) {
                const QImage im = imgOf(menu->item(i)->icon());
                const QImage asLetter = imgOf(Utils::letterIcon(menu->item(i)->text(), tileColor(im)));
                (im == asLetter ? lettered : bagged) << menu->item(i)->text();
            }
            say("只有「插件商店」那颗换成了画出来的袋子",
                bagged == QStringList { QStringLiteral("插件商店") },
                QStringLiteral("换过的=%1 还是字的=%2").arg(bagged.join(QStringLiteral("、")),
                                                           lettered.join(QStringLiteral("、"))));
            // 改外观的东西要能看见，不能只剩一条断言：四颗图标拼成一条，铺在菜单那块深色底上存成 PNG。
            // 数断言能证明"它不是第一个字"，证明不了"它看得出来是个袋子"。
            {
                const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
                QDir().mkpath(shots);
                const int n = menu ? menu->count() : 0;
                QPixmap strip(48 * n, 64);
                strip.fill(QColor(30, 32, 38));
                QPainter sp(&strip);
                for (int i = 0; i < n; ++i)
                    sp.drawPixmap(48 * i + 8, 8, menu->item(i)->icon().pixmap(40, 40));
                sp.end();
                const QString file = shots + QStringLiteral("/menu-icons.png");
                say("四颗菜单图标存成了一张 PNG（肉眼复核用）", strip.save(file, "PNG"),
                    QStringLiteral("%1 · %2x%3").arg(file).arg(strip.width()).arg(strip.height()));
            }
        }
        // 悬停说明（tooltip）的深色：他报的是"夜间模式鼠标悬停说明，灰字压白底完全看不见字"。
        // 量不了像素 —— 离屏看不到 windows11 风格那层**系统浅底**（2026-10-17 那次菜单判据就栽在
        // 这儿，见 captureselftest 里 makeStyledMenu 那段注释），所以量的是"真挂上去的那条规则里
        // 到底写了什么"：挂在 qApp 上（tooltip 是独立顶层窗口，挂在某个窗口上管不到它），
        // 并且底色和字色两头都写死、亮差够大。撤掉 main() 里那句 setStyleSheet，这两条立刻红。
        {
            const QString css = qApp->styleSheet();
            say("悬停说明的样式挂在 qApp 上（不是某个窗口）",
                css.contains(QLatin1String("QToolTip")), QStringLiteral("qApp 样式表长度=%1").arg(css.size()));
            const int from = css.indexOf(QLatin1String("QToolTip"));
            const QString block = from < 0 ? QString()
                                           : css.mid(from, css.indexOf(QLatin1Char('}'), from) - from);
            const auto hexOf = [block](const QString& prop) {
                const QRegularExpression re(QStringLiteral("%1:\\s*#([0-9a-fA-F]{6})").arg(prop));
                const auto m = re.match(block);
                return m.hasMatch() ? QColor(QStringLiteral("#") + m.captured(1)) : QColor();
            };
            const QColor bg = hexOf(QStringLiteral("background"));
            const QColor fg = hexOf(QStringLiteral("color"));
            say("那条规则里字比底色亮出一大截（看得清）",
                bg.isValid() && fg.isValid() && fg.lightness() - bg.lightness() > 120,
                QStringLiteral("底=%1 字=%2 亮差=%3")
                    .arg(bg.name(), fg.name())
                    .arg(bg.isValid() && fg.isValid() ? fg.lightness() - bg.lightness() : -999));
        }
        // ---- 夜间 / 日间（2026-10-20 他点名要配置页那颗切换）----
        // 这套判据钉的是"切换真的换了颜色、而且可逆、而且落盘"，不是"有个控件在那"。
        {
            const int themeRows = rowCount(QStringLiteral("外观："));
            const auto radios = page.findChildren<QRadioButton*>();
            QStringList radioNames;
            int checked = 0;
            for (const QRadioButton* r : radios) {
                radioNames << r->text();
                if (r->isChecked())
                    ++checked;
            }
            say("「外观」那一行只有一行，里面两颗单选",
                themeRows == 1 && radioNames.size() == 2,
                QStringLiteral("行数=%1 单选=%2").arg(themeRows).arg(radioNames.join(QStringLiteral("/"))));
            say("默认是夜间（这一版之前的行为，不该被切换功能改掉）",
                Theme::current() == Theme::Mode::Dark && checked == 1,
                QStringLiteral("current=%1 选中数=%2").arg(Theme::name(Theme::current())).arg(checked));

            const QColor darkWindow = Theme::color("window");
            const QString darkTip = qApp->styleSheet();
            // 拨到日间
            Theme::setMode(Theme::Mode::Light);
            const QColor lightWindow = Theme::color("window");
            const QColor palWindow = qApp->palette().color(QPalette::Window);
            say("切到日间：current 跟着变，调色板也真的换了",
                Theme::current() == Theme::Mode::Light && palWindow == lightWindow
                    && lightWindow != darkWindow,
                QStringLiteral("夜底=%1 日底=%2 调色板=%3").arg(darkWindow.name(), lightWindow.name(),
                                                               palWindow.name()));
            {
                const QColor text = Theme::color("text");
                const int delta = qAbs(text.lightness() - lightWindow.lightness());
                say("日间那份正文压在窗口底上够亮差（和夜间同一条口径 >120）",
                    delta > 120, QStringLiteral("底=%1 字=%2 亮差=%3")
                                      .arg(lightWindow.name(), text.name()).arg(delta));
            }
            say("切到日间：挂在 qApp 上的悬停说明跟着换底色（那一份是独立顶层窗口，只能靠它）",
                !qApp->styleSheet().isEmpty() && qApp->styleSheet() != darkTip
                    && qApp->styleSheet().contains(Theme::color("field").name().mid(1),
                                                   Qt::CaseInsensitive),
                QStringLiteral("新样式表长度=%1").arg(qApp->styleSheet().size()));
            {
                QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
                const QString stored = cfg.value("ui/theme").toString();
                say("切换是落盘的（重开程序还认），不是只改内存",
                    stored.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0,
                    QStringLiteral("config.ini ui/theme=%1").arg(stored));
            }
            // 反向：拨回夜间，一切得回到原值 —— 不然就是单向棘轮
            Theme::setMode(Theme::Mode::Dark);
            say("拨回夜间：底色/样式表一字不差回到原来那一份（不是单向的）",
                Theme::color("window") == darkWindow && qApp->styleSheet() == darkTip,
                QStringLiteral("底=%1 样式表回到原样=%2")
                    .arg(Theme::color("window").name())
                    .arg(qApp->styleSheet() == darkTip ? 1 : 0));
            // expand() 那一步：认得的名字换成值，认不出的原样留着（写错 token 时看得见）
            {
                const QString probe = Theme::expand("@window and @nope");
                say("样式表里的 @token 换成值；写错的名字原样留着，不许静默变空",
                    probe == QStringLiteral("%1 and @nope").arg(Theme::color("window").name()),
                    QStringLiteral("得到 %1").arg(probe));
            }
            // 前缀陷阱：这张表里有 5 对"一个是另一个的前缀"的角色
            //（card/cardLine、accent/accentUp/accentDown、group/groupText、
            //  disabled/disabledLine、border/borderHi/borderHiUp）。
            // 按表顺序挨个 replace 的话，`@cardLine` 会先被 `@card` 吃掉，剩下一个
            // `<card 的值>Line` 混在 QSS 里 —— 界面上是"边框颜色不对"，日志里一个字都没有。
            {
                const QString probe = Theme::expand(
                    QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                        .arg(QStringLiteral("@card"), QStringLiteral("@cardLine"),
                             QStringLiteral("@accent"), QStringLiteral("@accentUp"),
                             QStringLiteral("@group"), QStringLiteral("@groupText"),
                             QStringLiteral("@border"), QStringLiteral("@borderHi")));
                const QString want = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                    .arg(Theme::token("card"), Theme::token("cardLine"),
                         Theme::token("accent"), Theme::token("accentUp"),
                         Theme::token("group"), Theme::token("groupText"),
                         Theme::token("border"), Theme::token("borderHi"));
                say("长名字的角色不被它的短前缀抢走（@cardLine 不等于 @card 拼上 \"Line\"）",
                    probe == want && !probe.contains(QLatin1String("LineLine"))
                        && !probe.contains(QLatin1String("UpUp")),
                    QStringLiteral("得到 %1").arg(probe));
            }
            // Theme::bind 的那份样式表要能在切换时自己重挂，而且**子控件**也得收到通知 ——
            // 样式表挂在具体控件身上，只给顶层窗口发事件的话，配置页里的控件全是老颜色。
            {
                auto* win = new QWidget(nullptr);
                win->setObjectName(QStringLiteral("themeBindProbe"));
                auto* inner = new QLabel(QStringLiteral("探针"), win);
                auto* lay = new QVBoxLayout(win);
                lay->addWidget(inner);
                Theme::bind(inner, "color: @text; background: @window;");
                const QString nightQss = inner->styleSheet();
                Theme::setMode(Theme::Mode::Light);
                const QString dayQss = inner->styleSheet();
                const bool followsMode = nightQss != dayQss
                    && dayQss.contains(Theme::color("text").name(), Qt::CaseInsensitive)
                    && dayQss.contains(Theme::color("window").name(), Qt::CaseInsensitive)
                    && nightQss.contains(QStringLiteral("#1b1c20"), Qt::CaseInsensitive);
                Theme::setMode(Theme::Mode::Dark);
                const bool reversible = inner->styleSheet() == nightQss;
                delete win;
                say("挂在子控件上的那份样式表（Theme::bind）会跟着切换重挂，不是只有顶层窗口变脸",
                    followsMode && reversible,
                    QStringLiteral("夜间=%1 日间=%2 拨回来=%3")
                        .arg(nightQss, dayQss).arg(reversible ? QStringLiteral("原样")
                                                              : QStringLiteral("没回来")));
            }
            // 最后这一条不看样式表文本，看**渲出来的像素**：两种模式各截一张配置页存成 PNG，
            // 再量整张的平均亮度。为什么要多这一条 —— 前面那些判据量的是"字符串里写的是哪个色号"，
            // 而样式表写得对、画出来还是黑的是可能的（那份 QSS 没挂到这个控件上、或者控件用的是
            // 调色板不是样式表）。2026-10-20 加切换时就是把颜色从 380 处字面量搬进角色名，
            // 这类"搬漏一处"只有像素能抓到。
            {
                const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
                QDir().mkpath(shots);
                // 渲的是**整扇设置窗**（左边菜单 + 右边那张页），不是光一张 ConfigPage ——
                // 2026-10-20 他回"太丑了太难受了"看的就是这个整体，只渲单页会漏掉
                // 菜单那一列和页眉跟内容之间的配色关系。
                SettingsDialog dlg;
                dlg.setAttribute(Qt::WA_DontShowOnScreen, true);
                dlg.resize(1140, 790);
                if (auto* menu = dlg.findChild<QListWidget*>(QStringLiteral("menuList")))
                    menu->setCurrentRow(2);   // 「配置」那一行，走的是真人点的那条路
                QCoreApplication::processEvents();
                const auto meanLightness = [&dlg](const QString& file) {
                    const QPixmap pm = dlg.grab();
                    const QImage img = pm.toImage().convertToFormat(QImage::Format_RGB32);
                    pm.save(file, "PNG");
                    long long sum = 0, n = 0;
                    for (int y = 0; y < img.height(); y += 4) {
                        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                        for (int x = 0; x < img.width(); x += 4, ++line) {
                            sum += qGray(*line);
                            ++n;
                        }
                    }
                    return n > 0 ? int(sum / n) : -1;
                };
                Theme::setMode(Theme::Mode::Dark);
                const int night = meanLightness(shots + QStringLiteral("/theme-night.png"));
                Theme::setMode(Theme::Mode::Light);
                const int day = meanLightness(shots + QStringLiteral("/theme-day.png"));
                // 再补一张「外观」那一页单独的图：拆 tab 之后只截第一页就等于没截到新东西，
                // 而"这一页是不是空得只剩一行"必须用眼睛过一遍，不能只靠上面那条数分组框。
                if (auto* tw = dlg.findChild<QTabWidget*>(QStringLiteral("cfgTabs"))) {
                    tw->setCurrentIndex(1);
                    QCoreApplication::processEvents();
                    const QPixmap pm = dlg.grab();
                    pm.save(shots + QStringLiteral("/theme-look-tab.png"), "PNG");
                    tw->setCurrentIndex(0);
                }
                Theme::setMode(Theme::Mode::Dark);
                say("两种模式各渲一整扇设置窗，日间那张整张平均亮度明显更亮（量的是像素不是字符串）",
                    night >= 0 && day - night > 60,
                    QStringLiteral("夜间平均灰度=%1 日间=%2 差=%3 · PNG 在 %4")
                        .arg(night).arg(day).arg(day - night).arg(shots));
            }
            // 启动器那扇是 WA_TranslucentBackground + 无边框 —— 也就是说"搜索框在哪儿"
            // 这件事**只由那一圈 1px 描边**说给他看。2026-10-20 他第一句反馈就是
            // "这个白色不好，完全看不清搜索框的边框"：日间那份底是纯白、描边是黑压 26/255，
            // 两者差 1.15:1，等于没有。所以这条量的就是渲染出来的**描边那一行 vs 紧贴里面的底**，
            // 按 WCAG 1.4.11（非文本对比度 ≥3:1）判，两个模式各判一次。
            // 这条不读 Theme 里的任何常量，读的是 grab() 出来的像素 —— 换个画法它就红。
            {
                auto* launcher = new SearchWindow;
                const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
                launcher->resize(560, 300);
                // 亮度/对比度的算法在 Theme 里只有一份（原来这里抄过一份局部 lambda，
                // 剪贴板那张卡也要问同样的问题，就来第二处 —— 搬进 Theme 收掉）。
                const auto edgeContrast = [launcher](const QString& file, QString* why) {
                    const QPixmap pm = launcher->grab();
                    const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
                    pm.save(file, "PNG");
                    // 沿**左边缘那一列**、竖着中间那一行取点：路径是 QRectF(0.5, 0.5, …) 配
                    // 1px 画笔 → 描边正好盖满第 0 列（上下那两行不行，那里有 13px 圆角，
                    // 第一版我从顶行取，日间那颗底一变成不透明就"描边和底同一行"了）。
                    // Format_ARGB32 非预乘，所以第 0 列的 r/g/b 就是描边本身的颜色；
                    // 往里第 8 列过了布局那 14px 左边距的空白区，是纯窗口底。
                    const int cy = img.height() / 2;
                    if (qAlpha(img.pixel(0, cy)) == 0 || qAlpha(img.pixel(8, cy)) == 0) {
                        *why = QStringLiteral("第 0 列或第 8 列在这一行是空的（窗口没画满？）");
                        return -1.0;
                    }
                    const QRgb e = img.pixel(0, cy);
                    const QRgb f = img.pixel(8, cy);
                    *why = QStringLiteral("描边=%1 底=%2").arg(QColor(e).name(), QColor(f).name());
                    return Theme::contrast(e, f);
                };
                Theme::setMode(Theme::Mode::Dark);
                QString whyN;
                const double cn = edgeContrast(shots + QStringLiteral("/launcher-night.png"), &whyN);
                Theme::setMode(Theme::Mode::Light);
                QString whyD;
                const double cd = edgeContrast(shots + QStringLiteral("/launcher-day.png"), &whyD);
                Theme::setMode(Theme::Mode::Dark);
                delete launcher;
                // 日间按 WCAG 1.4.11（非文本对比度）要 ≥3:1；夜间那颗 1.86:1 是这一版一直以来
                // 的样子，他这次点的是"这个白色不好"、没说要改夜里 —— 所以夜里这条只钉
                // "别再变糊"，不拿 3:1 去动他用了很久的默认外观。
                say("启动器那圈描边和它自己的底差得开（无边框窗，看得见边=看得见框）："
                    "日间 ≥3:1、夜间只钉住不比现在这份更糊",
                    cd >= 3.0 && cn >= 1.8,
                    QStringLiteral("夜间 %1:1（%2）· 日间 %3:1（%4）")
                        .arg(cn, 0, 'f', 2).arg(whyN).arg(cd, 0, 'f', 2).arg(whyD));
            }
        }
        std::printf("cfgcheck: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad;
    }

    // --seamtest：平台层那两道缝本身的可观测行为（src/platform/Paths.h、Launch.h）。
    // 为什么要这条：这一轮把 SHGetKnownFolderPath / ShellExecuteExW / %VAR% 展开从 Utils
    // 原样搬到 src/platform/*_win.cpp，纯搬家、行为零变化 —— 可"switch 里把枚举项配错一行"
    // 这种事故，原有那 221+52+36 条一条都发现不了（没人断言过桌面到底扫的是哪个目录）。
    // 之后每搬一道缝都要留下这种"缝自己能被断言"的判据，否则 POSIX 那份写完，
    // 没人有证据说它和 Windows 这份对得上。最后两条不许真去启动任何东西。
    if (args.contains("--seamtest")) {
        int bad = 0;
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const QString desk = Platform::knownDir(Platform::KnownDir::UserDesktop);
        const QString shared = Platform::knownDir(Platform::KnownDir::SharedDesktop);
        const QString userApps = Platform::knownDir(Platform::KnownDir::UserApps);
        const QString sysApps = Platform::knownDir(Platform::KnownDir::SystemApps);
        std::printf("INFO 桌面=[%s] 共用桌面=[%s] 用户条目=[%s] 系统条目=[%s]\n",
                    desk.toUtf8().constData(), shared.toUtf8().constData(),
                    userApps.toUtf8().constData(), sysApps.toUtf8().constData());
        check("用户桌面问得到", desk.isEmpty() ? "no" : "yes", "yes");
        check("用户级启动条目目录问得到", userApps.isEmpty() ? "no" : "yes", "yes");
        check("全体用户级启动条目目录问得到", sysApps.isEmpty() ? "no" : "yes", "yes");
        QStringList dirs;
        dirs << desk << shared << userApps << sysApps;
        dirs.removeAll(QString());
        int dupes = 0;
        for (int i = 0; i < dirs.size(); ++i)
            for (int j = i + 1; j < dirs.size(); ++j)
                if (dirs[i] == dirs[j])
                    ++dupes;
        check("问出来的目录两两不同（枚举项配错一行就会撞车）", dupes == 0 ? "yes" : "no", "yes");
#ifdef Q_OS_WIN
        check("用户桌面就是 profile 下那个 Desktop",
              desk.startsWith(QDir::homePath() + QStringLiteral("/Desktop")) ? "yes" : "no", "yes");
        check("共用桌面走的是 Public 那一棵",
              shared.contains(QStringLiteral("/Public")) ? "yes" : "no", "yes");
        check("用户级条目在用户自己 profile 下",
              userApps.startsWith(QDir::homePath()) ? "yes" : "no", "yes");
        check("全体级条目不在用户 profile 下",
              sysApps.startsWith(QDir::homePath()) ? "yes" : "no", "no");
#endif
#ifdef Q_OS_WIN
        const QString knownToken = QStringLiteral("%SystemRoot%");
        const QString knownValue = qEnvironmentVariable("SystemRoot");
        const QString unknownToken = QStringLiteral("%LTOOL_NO_SUCH_VAR%");
#else
        const QString knownToken = QStringLiteral("$HOME");
        const QString knownValue = QDir::homePath();
        const QString unknownToken = QStringLiteral("$LTOOL_NO_SUCH_VAR");
#endif
        // 这两条要跟"真的那个值"比，不能跟 expandEnv 自己吐出来的东西比：
        // 反向验（把 expandEnv 改成直接 return s）时，自己比自己那版照样绿，等于没判据。
        check("认得出的占位符换成了系统里那个真值",
              Platform::expandEnv(knownToken) == knownValue ? "yes" : "no", "yes");
        check("句子里夹着的占位符也换，两头文字不动",
              Platform::expandEnv(QStringLiteral("A") + knownToken + QStringLiteral("B"))
                  == QStringLiteral("A") + knownValue + QStringLiteral("B") ? "yes" : "no", "yes");
        check("认不出的占位符原样留着（不许吃掉字符串）",
              Platform::expandEnv(unknownToken) == unknownToken ? "yes" : "no", "yes");
        check("没有占位符的普通路径原样回去",
              Platform::expandEnv(QStringLiteral("/tmp/plain")) == QStringLiteral("/tmp/plain")
              ? "yes" : "no", "yes");
        // 3) 开机自启：这条缝的可观测面只有"落点在哪"和"条目写了什么"，勾一下没反应这种事
        //    永远不会报错。所以**只问这两个纯事实，不调 setAutostart** —— 它会真去动
        //    HKCU 的 Run 键（那是他机器的开机自启，不该被一次自测改掉）。
        const QString autoTarget = Platform::autostartTarget();
#ifdef Q_OS_WIN
        check("自启落点是 HKCU 那个 Run 键下的 ltool 值",
              autoTarget.contains(QStringLiteral("CurrentVersion\\Run"))
                  && autoTarget.endsWith(QStringLiteral("\\ltool")) ? "yes" : "no", "yes");
        check("自启条目写的就是当前这个 exe（挪过安装目录要跟着走）",
              Platform::autostartEntry(QStringLiteral("C:/x/y/ltool.exe"))
                  == QStringLiteral("C:\\x\\y\\ltool.exe") ? "yes" : "no", "yes");
        std::printf("INFO 自启落点=[%s] 这台机器当前=%s（自测只读不写，不替他勾）\n",
                    autoTarget.toUtf8().constData(),
                    Platform::autostartEnabled() ? "已开" : "没开");
#else
        check("自启落点在 HOME 下那个自启动目录里",
              autoTarget.startsWith(QDir::homePath())
                  && (autoTarget.endsWith(QStringLiteral("/ltool.desktop"))
                      || autoTarget.endsWith(QStringLiteral("/com.ltool.autostart.plist")))
              ? "yes" : "no", "yes");
        const QString entry = Platform::autostartEntry(QStringLiteral("/tmp/My Apps/ltool"));
        check("Exec 那一行带上 exe，且空格被转义（不然那一行会被切成两个参数）",
              entry.contains(QStringLiteral("Exec=/tmp/My\\ Apps/ltool")) ? "yes" : "no", "yes");
#endif
        // 4) 第五道缝（进程/文件元信息）：这些在两个平台上的答案本来就不一样，
        //    分开断言才对得上 —— 特别是"做不到"那件，不许假装成功。
        check("只给文件名时系统找得到（cmd.exe / 没有这个东西）",
              (!Platform::findExecutable(QStringLiteral("cmd.exe")).isEmpty()
               && Platform::findExecutable(QStringLiteral("ltool-no-such-exe-xyz")).isEmpty())
              ? "yes" : "no", "yes");
        std::printf("INFO 常驻内存=%d MB\n", Platform::workingSetMB());
        check("当前进程的常驻内存量得到（-1 = 读不到）",
              Platform::workingSetMB() > 0 ? "yes" : "no", "yes");
#ifdef Q_OS_WIN
        check("Windows 上能把工作集还给系统", Platform::trimWorkingSet() ? "yes" : "no", "yes");
        check("Windows 上读得到版本号（拿系统里的 cmd.exe 量）",
              QRegularExpression(QStringLiteral("^\\d+\\.\\d+")).match(
                  Platform::fileVersion(Platform::expandEnv(QStringLiteral("%SystemRoot%"))
                                         + QStringLiteral("/System32/cmd.exe"))).hasMatch()
              ? "yes" : "no", "yes");
        // 顺手量出来的一条事实，不当判据（判据该断言真相，不该为了绿去改期望）：
        // 我们自己的 exe **没有** VERSIONINFO 资源 —— 关于页那个版本号是编译期宏，
        // PE 里读不到，所以任务管理器/属性面板的版本列是空的。要补就得给 ltool.rc 加
        // VERSIONINFO 并把 LTOOL_VERSION 传进 rc 编译，那是单独一件事。
        std::printf("INFO 自己的 exe 版本号=[%s]（空=PE 里没有 VERSIONINFO 资源，已知缺口）\n",
                    Platform::fileVersion(QCoreApplication::applicationFilePath()).toUtf8().constData());
#else
        check("这个平台上「把工作集还给系统」做不到，要如实报 false",
              Platform::trimWorkingSet() ? "yes" : "no", "no");
        check("这个平台上普通文件没有版本资源，返回空串（不许编一个）",
              Platform::fileVersion(QStringLiteral("/bin/sh")).isEmpty() ? "yes" : "no", "yes");
#endif
        check("空路径不许启动（launch）", Platform::launch(QString()) ? "yes" : "no", "no");
        check("空路径不许提权启动（launchAsAdmin）",
              Platform::launchAsAdmin(QString()) ? "yes" : "no", "no");
        // 5) 第七道缝（粉碎 / 占用查询）：这四条两边期望值相同 —— 报的字节数、"没了"、
        //    "-1 + 原因"、"没人开着就是空表"。Linux 那侧同一组断言在 tools/seam_probe.sh 里，
        //    数字对得上才说明这两份实现做的是同一件事（POSIX 探针量的目录树也是 12288）。
        //    删除只往 QDir::tempPath() 下算出来的那棵走，别的一律不碰。
        const QString box = QDir::tempPath() + QStringLiteral("/ltool-seam-fileops");
        QDir(box).removeRecursively();
        QDir().mkpath(box + QStringLiteral("/tree/sub"));
        const auto put = [](const QString& path, int bytes) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write(QByteArray(bytes, 'A'));
            f.close();
            return true;
        };
        put(box + "/a.bin", 1 << 20);
        QString serr;
        const qint64 shallow = FileOps::shred(box + "/a.bin", false, &serr);
        check("粉碎一颗 1MB 文件报的字节数 = 文件大小",
              QString::number(shallow), QStringLiteral("1048576"));
        check("粉碎之后文件真没了（不是移到回收站）",
              QFileInfo::exists(box + "/a.bin") ? "yes" : "no", "no");
        put(box + "/b.bin", 4096);
        serr.clear();
        const qint64 deep = FileOps::shred(box + "/b.bin", true, &serr);
        check("thorough 三遍覆写也按大小报一遍", QString::number(deep), QStringLiteral("4096"));
        check("改名用的 ~*.tmp 没留在原地",
              QDir(box).entryList({QStringLiteral("~*.tmp")}, QDir::Files).isEmpty() ? "yes" : "no",
              "yes");
        put(box + "/tree/a.bin", 4096);
        put(box + "/tree/sub/b.bin", 8192);
        serr.clear();
        const qint64 dirErased = FileOps::shred(box + "/tree", false, &serr);
        check("整棵目录树抹掉的字节数是两颗之和", QString::number(dirErased),
              QStringLiteral("12288"));
        check("目录从最深往上删干净、没报错",
              (!QFileInfo::exists(box + "/tree") && serr.isEmpty()) ? "yes" : "no", "yes");
        QString missErr;
        const qint64 missing = FileOps::shred(box + "/nope.bin", false, &missErr);
        check("不存在的目标返回 -1 并说出原因",
              (missing == -1 && !missErr.isEmpty()) ? "yes" : "no", "yes");
        put(box + "/quiet.bin", 1024);
        QString werr;
        const auto nobody = FileOps::whoHas(box + "/quiet.bin", &werr);
        check("没人开着的文件：占用表是空的、也不报错",
              (nobody.isEmpty() && werr.isEmpty()) ? "yes" : "no", "yes");
        // 自己开着的文件能不能认出来：两份实现走的是完全不同的机制（Windows 问 Restart
        // Manager，POSIX 扫 /proc/self/fd），"看不看得到查询者自己"本来就是平台差异，
        // 所以这里只把实测打出来，不当判据 —— 认不出自己不算坏，认得出更好。
        QFile mine(box + "/quiet.bin");
        mine.open(QIODevice::ReadOnly);
        const auto selfish = FileOps::whoHas(box + "/quiet.bin", &werr);
        std::printf("INFO 自持句柄时扫到 %d 个占用者（本机 pid=%lld，err=[%s]）\n",
                    int(selfish.size()), qint64(QCoreApplication::applicationPid()),
                    werr.toUtf8().constData());
        mine.close();
        QDir(box).removeRecursively();
        // 6) DevEnv 那三颗（代理怎么套 / 装在哪）—— 搬家（第九刀）前后跑的是同一组断言，
        //    期望值由"这台机器现在的配置"推出来，所以量到的是真值不是硬编码：
        //    net/proxy 留空 = 跟系统代理走（他本机注册表里是 9980），装东西的根在 Windows
        //    上是盘符、在 POSIX 上是家目录下一个真能用的路径。
        const QString px = DevEnv::proxyUrl();
        std::printf("INFO 生效代理=[%s] 系统那半=[%s]\n", px.toUtf8().constData(),
                    DevEnv::systemProxyUrl().toUtf8().constData());
        check("生效的代理要么没有、要么写成带主机的 URL",
              (px.isEmpty()
               || (QUrl(px).isValid() && !QUrl(px).host().isEmpty()))
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        const QString root = DevEnv::installLocation();
#ifdef Q_OS_WIN
        check("安装根非空，而且是这台机器上那种盘符写法",
              (!root.isEmpty() && root.size() >= 2 && root.at(1) == QLatin1Char(':')
               && QDir(root.left(2) + QLatin1Char('\\')).exists())
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
#else
        check("安装根非空，而且是绝对路径（POSIX 上没有盘符这回事）",
              (!root.isEmpty() && root.startsWith(QLatin1Char('/'))) ? QStringLiteral("yes")
                                                                     : QStringLiteral("no"),
              QStringLiteral("yes"));
#endif
        {
            QNetworkAccessManager nam;
            DevEnv::applyProxy(&nam);
            const QNetworkProxy got = nam.proxy();
            const bool same = px.isEmpty()
                                  ? got.type() == QNetworkProxy::NoProxy
                                  : (got.type() == QNetworkProxy::HttpProxy
                                     && got.hostName() == QUrl(px).host()
                                     && got.port() == quint16(QUrl(px).port(
                                            QUrl(px).scheme() == QLatin1String("https") ? 443 : 80)));
            check("applyProxy 真的把同一个代理装到了管理器上",
                  same ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
        }
        // 这轮还收了一道"起进程别弹控制台"（原来 8 处各写一遍 CREATE_NO_WINDOW 的 lambda）：
        // Windows 挂旗标，POSIX 没这个概念 ⇒ 空动作。黑窗弹没弹在无界面下量不到，
        // 这里量能量的那半：过了这道缝的进程照样起得来、输出照样读得到。
        {
            QProcess hidden;
            Platform::runWithoutConsole(&hidden);
#ifdef Q_OS_WIN
            hidden.start(QStringLiteral("cmd"),
                         {QStringLiteral("/c"), QStringLiteral("echo seamok")});
#else
            hidden.start(QStringLiteral("/bin/echo"), {QStringLiteral("seamok")});
#endif
            const bool up = hidden.waitForStarted(3000);
            const bool done = up && hidden.waitForFinished(3000);
            check("过了「不弹控制台」那道缝的进程照样起得来、输出读得到",
                  (done && QString::fromLocal8Bit(hidden.readAllStandardOutput())
                                .contains(QStringLiteral("seamok")))
                      ? QStringLiteral("yes")
                      : QStringLiteral("no"),
                  QStringLiteral("yes"));
        }
        // 上面那三条量的是"这台机器现在的配置"，本机 net/proxy 留空、系统代理也读不到 ⇒
        // 走的是"没代理"那一支。规则本身要用夹具量，不然这两支里的一支永远没被人碰过
        // （装饰性的绿）：normalizeProxy 两边共用一份，usableInstallRoot 按平台各一份。
        check("normalizeProxy：host:port 补成 http://（winget 和 QNAM 都只认 URL）",
              Platform::normalizeProxy(QStringLiteral("127.0.0.1:9980")),
              QStringLiteral("http://127.0.0.1:9980"));
        check("normalizeProxy：已经是 URL 的原样留下",
              Platform::normalizeProxy(QStringLiteral("https://proxy.internal:8443")),
              QStringLiteral("https://proxy.internal:8443"));
        check("normalizeProxy：空、以及连冒号都没有的写法都当没代理",
              Platform::normalizeProxy(QStringLiteral()).isEmpty()
                      && Platform::normalizeProxy(QStringLiteral("nocolonhere")).isEmpty()
                          ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
#ifdef Q_OS_WIN
        check("Windows：安装根留空就是留空（= 不指定 --location，不是偷偷给个默认）",
              Platform::usableInstallRoot(QString()), QString());
        check("Windows：配的盘符不在这台机器上（Z:）退到 C 盘同一个路径",
              Platform::usableInstallRoot(QStringLiteral("Z:\\software")),
              QStringLiteral("C:\\software"));
        check("Windows：C 盘自己和网络路径都不动",
              Platform::usableInstallRoot(QStringLiteral("C:\\tools"))
                      == QStringLiteral("C:\\tools")
                  && Platform::usableInstallRoot(QStringLiteral("\\\\nas\\share"))
                         == QStringLiteral("\\\\nas\\share")
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
#else
        check("POSIX：留空 / Windows 写法 / 相对路径都退到家目录下那个真能用的根",
              Platform::usableInstallRoot(QString())
                      == QDir::homePath() + QStringLiteral("/software")
                  && Platform::usableInstallRoot(QStringLiteral("D:\\software"))
                         == QDir::homePath() + QStringLiteral("/software")
                  && Platform::usableInstallRoot(QStringLiteral("software"))
                         == QDir::homePath() + QStringLiteral("/software")
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        check("POSIX：绝对路径原样用",
              Platform::usableInstallRoot(QStringLiteral("/opt/dev")),
              QStringLiteral("/opt/dev"));
#endif
        std::printf("seamtest: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad;
    }

    // --pinyin：无界面验拼音匹配。核心是"表和码点对齐"（错一位就会把"垃圾"当成"任务"）
    // + 多音字 + 首字母只认开头 + 纯英文名不掺和 + 每次按键的耗时。
    if (args.contains("--pinyin")) {
        int bad = 0;
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const auto kind = [](Pinyin::Kind k) {
            return k == Pinyin::Full ? QStringLiteral("full")
                 : k == Pinyin::Initial ? QStringLiteral("initial") : QStringLiteral("none");
        };
        check("表：任 → ren", Pinyin::fullReadings(QStringLiteral("任")).contains("ren") ? "yes" : "no", "yes");
        check("表没错位：垃圾 → laji", Pinyin::fullReadings(QStringLiteral("垃圾")).contains("laji") ? "yes" : "no", "yes");
        check("renwu 命中 任务", kind(Pinyin::match(QStringLiteral("任务"), "renwu")), "full");
        check("renwu 命中 任务管理器（整名）",
              kind(Pinyin::match(QStringLiteral("任务管理器"), "renwu")), "full");
        check("rw 命中 任务（首字母）", kind(Pinyin::match(QStringLiteral("任务"), "rw")), "initial");
        check("chongqing 命中 重庆", kind(Pinyin::match(QStringLiteral("重庆"), "chongqing")), "full");
        check("zhongqing 也命中 重庆（多音字全留）", kind(Pinyin::match(QStringLiteral("重庆"), "zhongqing")), "full");
        check("cq 命中 重庆（首字母）", kind(Pinyin::match(QStringLiteral("重庆"), "cq")), "initial");
        check("中间片段也算：guanli 命中 任务管理器",
              kind(Pinyin::match(QStringLiteral("任务管理器"), "guanli")), "full");
        check("全拼允许中段：wu 也算 任务", kind(Pinyin::match(QStringLiteral("任务"), "wu")), "full");
        check("首字母只认开头：gl 不算 任务管理器",
              kind(Pinyin::match(QStringLiteral("任务管理器"), "gl")), "none");
        check("不误伤：wx 不命中 任务", kind(Pinyin::match(QStringLiteral("任务"), "wx")), "none");
        // 直达那一档必须从开头命中：中段命中只是"名字里正好含这几个字母"，
        // 拿它独占整页会把用户要的东西顶掉（renwu 被"计划任务"抢走那次就是这么来的）。
        check("中段命中：renwu 算 计划任务（match 允许中段，应用那一档靠它）",
              kind(Pinyin::match(QStringLiteral("计划任务"), "renwu")), "full");
        check("直达不认中段：renwu 不算 计划任务（matchStart）",
              kind(Pinyin::matchStart(QStringLiteral("计划任务"), "renwu")), "none");
        check("直达认开头：cipanqingli 算 磁盘清理（matchStart）",
              kind(Pinyin::matchStart(QStringLiteral("磁盘清理"), "cipanqingli")), "full");
        check("直达认开头：cpql 也算 磁盘清理（matchStart）",
              kind(Pinyin::matchStart(QStringLiteral("磁盘清理"), "cpql")), "initial");
        // 他报的第四件事的根因：直达吃到了生僻读音。广 在表里是 "guang,yan,an"，
        // 于是 "an" 被当成「广告」的全拼开头 → 工具直达独占整页 → Antigravity 没了。
        // 应用那一档（match）故意留着这个读音：那边只是排序，多一条召回没坏处。
        check("直达不吃生僻读音：an 不算「广告」直达",
              kind(Pinyin::matchStart(QStringLiteral("广告"), "an")), "none");
        check("但应用那一档还留着：match(广告, an) 仍是全拼",
              kind(Pinyin::match(QStringLiteral("广告"), "an")), "full");
        check("换成主读音照样直达：guanggao 算「广告」",
              kind(Pinyin::matchStart(QStringLiteral("广告"), "guanggao")), "full");
        check("直达的首字母那档没被带歪：ggdc 算 广告弹窗",
              kind(Pinyin::matchStart(QStringLiteral("广告弹窗"), "ggdc")), "initial");
        check("多音字直达只吃第一条读音：重庆（表里是 zhong,chong,tong）走 zhongqing",
              kind(Pinyin::matchStart(QStringLiteral("重庆"), "zhongqing")), "full");
        check("第二条 chongqing 就不到达了 —— 代价就这一条，应用那边（match）还留着",
              kind(Pinyin::matchStart(QStringLiteral("重庆"), "chongqing")), "none");
        check("纯英文名不走拼音这一档", kind(Pinyin::match(QStringLiteral("Qoder"), "oder")), "none");
        check("单字母不启用", Pinyin::usableQuery(QStringLiteral("r")) ? "yes" : "no", "no");
        check("带数字的名字：腾讯会议3",
              Pinyin::fullReadings(QStringLiteral("腾讯会议3")).contains("tengxunhuiyi3") ? "yes" : "no", "yes");
        check("带数字的首字母", Pinyin::initialReadings(QStringLiteral("腾讯会议3")).contains("txhy3") ? "yes" : "no", "yes");

        const auto apps = AppScanner::loadIndex();
        // 耗时：真实应用表里中文名没几个，光量它测不出最坏情况 —— 再掺 600 个六字中文名，
        // 量的是"每次按键都要扫一遍全部名字"那条路。
        QStringList names;
        for (const AppEntry& a : apps)
            names << a.name;
        const QString pool = QStringLiteral("任务管理器设置网络磁盘清理软件更新注册防火墙剪贴板截图录像拼音搜索");
        for (int i = 0; i < 600; ++i) {
            QString nm;
            for (int j = 0; j < 6; ++j)
                nm += pool.at((i * 7 + j * 13) % pool.size());
            names << nm;
        }
        QElapsedTimer t;
        t.start();
        for (const QString& nm : names)
            Pinyin::fullReadings(nm);
        const qint64 buildUs = t.nsecsElapsed() / 1000;
        t.restart();
        int hits = 0;
        for (int i = 0; i < 200; ++i)
            for (const QString& nm : names)
                if (Pinyin::match(nm, "renwu") != Pinyin::None)
                    ++hits;
        // 注意别先除以 200 再打：整数除法会把 8ms 打成 0ms，看着像"快到没有"（假绿）。
        const qint64 scanUs = t.nsecsElapsed() / 1000 / 200;
        std::printf("INFO 名字 %d 个（含 600 个六字中文名）：首次展开 %lldus，缓存后扫全表 %lldus/次，"
                    "renwu 命中 %d 个\n",
                    int(names.size()), long long(buildUs), long long(scanUs), hits / 200);
        check("首次展开全表 <300ms", buildUs < 300000 ? "yes" : "no", "yes");
        check("缓存后一次查询 <3ms", scanUs < 3000 ? "yes" : "no", "yes");
        // ---- 端到端：真建一个不显示的启动器窗口，往输入框里打拼音，看结果列表出不出来。
        //      行是 delegate 画的、没有文字，所以数的是"节标题 + 可执行行的个数"。----
        SearchWindow w;
        auto* input = w.findChild<QLineEdit*>(QStringLiteral("input"));
        auto* list = w.findChild<QListWidget*>(QStringLiteral("list"));
        const bool haveUi = input && list;
        check("拿得到输入框和结果列表", haveUi ? "yes" : "no", "yes");
        if (haveUi) {
            const auto settle = [] {
                for (int i = 0; i < 40; ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(10);
                }
            };
            const auto hasSection = [list](const QString& s) {
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text() == s)
                        return true;
                return false;
            };
            const auto rows = [list] {
                int n = 0;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().isEmpty() && list->item(i)->data(Qt::UserRole).toInt() >= 0)
                        ++n;
                return n;
            };
            // 把结果里出现了哪些小节原样打出来：查"某个词为什么搜不到 X"的时候，
            // 光看断言红不红不够，得看见是哪一节把页占了（直达 return 那一类）。
            const auto sections = [list] {
                QStringList out;
                for (int i = 0; i < list->count(); ++i) {
                    const QString t = list->item(i)->text();
                    if (!t.isEmpty())
                        out << t;
                }
                return out.join(QStringLiteral(" / "));
            };
            const auto query = [&](const QString& q) {
                input->setText(q);
                settle();
                return qMakePair(hasSection(QStringLiteral("工具")), rows());
            };
            const auto t0 = query(QStringLiteral("zzqqxxww"));   // 乱码：什么都不该出
            const auto t1 = query(QStringLiteral("cipanqingli"));  // 磁盘清理的全拼
            const auto t2 = query(QStringLiteral("cpql"));         // 它的首字母
            std::printf("INFO 端到端：乱码 工具节=%d 行=%d ｜全拼 工具节=%d 行=%d ｜首字母 工具节=%d 行=%d\n",
                        t0.first ? 1 : 0, int(t0.second), t1.first ? 1 : 0, int(t1.second),
                        t2.first ? 1 : 0, int(t2.second));
            check("乱码查询不出工具", t0.first ? "yes" : "no", "no");
            check("全拼 cipanqingli 直达工具", t1.first ? "yes" : "no", "yes");
            check("首字母 cpql 也直达工具", t2.first ? "yes" : "no", "yes");
            // 他实际报的那一个：任务管理器不在卡片墙那六件事里，走的是「应用」那一节
            // （SysTools::systemTools() 并进应用索引），所以这条要断言的是应用节。
            input->setText(QStringLiteral("renwu"));
            settle();
            const bool appSect = hasSection(QStringLiteral("── 应用 ──"));
            const bool toolSect = hasSection(QStringLiteral("工具"));
            std::printf("INFO 端到端 renwu：应用节=%d 工具节=%d 可执行行=%d\n",
                        appSect ? 1 : 0, toolSect ? 1 : 0, int(rows()));
            check("renwu 在搜索框里能搜到应用（任务管理器）", appSect ? "yes" : "no", "yes");
            check("renwu 不再被「计划任务」那条关键词当成工具直达（它会把应用整个顶掉）",
                  toolSect ? "yes" : "no", "no");
            // 取整之后直达不能变哑：qingli 不是「磁盘清理」这个名字的开头，
            // 但它是关键词「清理」的开头 —— 这条守的是"别把直达搞成搜不到"。
            const auto t4 = query(QStringLiteral("qingli"));
            check("取整不伤直达：qingli 还是直达磁盘清理", t4.first ? "yes" : "no", "yes");
            // 新插件「图片排版」进的是同一张表（PluginHost::scan()），搜索框这边不该要特判。
            // 这两条盯的就是"进商店列表 = 也进搜索"这条自动接上的链有没有断。
            const auto t5 = query(QStringLiteral("paiban"));
            check("全拼 paiban 直达「图片排版」（关键词「排版」开头）", t5.first ? "yes" : "no", "yes");
            const auto t6 = query(QStringLiteral("gongge"));
            check("全拼 gongge 直达「图片排版」（关键词「宫格」开头）", t6.first ? "yes" : "no", "yes");
            // 电源那两条（2026-10-19 他点名：输 sleep/休眠 就休眠，输 shutdown/关机 就关机）。
            // **这里只量路由**：第一行是不是那颗、target 对不对 —— 一次都不真按回车，
            // 否则跑判据的这台机器当场就睡过去了。
            const auto powerOf = [&](const QString& q) {
                input->setText(q);
                settle();
                // 第 0 行是那一节的标题（addSection 先落一行标题），所以要找**第一条能按的行**
                for (int i = 0; i < w.resultCount(); ++i) {
                    const SearchResult& r = w.resultAt(i);
                    if (!r.isSection)
                        return r.target;
                }
                return QString();
            };
            check("输「休眠」第一行就是休眠那一条", powerOf(QStringLiteral("休眠")),
                  QStringLiteral("power:hibernate"));
            check("输 sleep 同上", powerOf(QStringLiteral("sleep")), QStringLiteral("power:hibernate"));
            check("输「关机」第一行就是关机那一条", powerOf(QStringLiteral("关机")),
                  QStringLiteral("power:shutdown"));
            check("输 shutdown 同上", powerOf(QStringLiteral("shutdown")), QStringLiteral("power:shutdown"));
            // 120ms 去抖那一拍不能被回车漏掉（他 2026-10-19 报"搜 shutdown 什么都没有搜出来"）：
            // 故意不 settle —— 先看列表（还是上一个查询那一版，没有这条），再走 flushSearch
            // （= 回车做的第一件事），这条就该出现了。
            input->setText(QStringLiteral("zzz-这条什么都搜不到-qqq"));
            settle();                     // 先把列表清干净，下面量的才是"少一拍"那一版
            input->setText(QStringLiteral("shutdown"));
            bool staleHas = false;
            QStringList staleTargets;
            for (int i = 0; i < w.resultCount(); ++i) {
                staleTargets << (w.resultAt(i).isSection ? QStringLiteral("§")
                                                         : w.resultAt(i).target.left(22));
                if (w.resultAt(i).target == QLatin1String("power:shutdown"))
                    staleHas = true;
            }
            std::printf("INFO 没补那一拍时列表长这样：%s\n",
                        staleTargets.join(QStringLiteral(",")).left(200).toUtf8().constData());
            w.flushSearch();
            bool flushedHas = false;            for (int i = 0; i < w.resultCount(); ++i)
                if (w.resultAt(i).target == QLatin1String("power:shutdown"))
                    flushedHas = true;
            check("回车前那一拍补得回来（不补=旧列表没这条，补了=有）",
                  (!staleHas && flushedHas) ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            std::printf("INFO 去抖那一拍：不补时列表里有 power 条=%d，补了之后=%d\n", staleHas ? 1 : 0,
                        flushedHas ? 1 : 0);
            // 这台机器（实测 `powercfg /a`：休眠「尚未启用」）不能休眠，那一行就得**当场**把
            // powercfg /h on 说出来，而不是等他按下回车再报错。判据按机器的实际状态分支，
            // 两种机器上都成立。
            QString hibSub;
            input->setText(QStringLiteral("休眠"));
            settle();
            for (int i = 0; i < w.resultCount(); ++i) {
                const SearchResult& r = w.resultAt(i);
                if (!r.isSection) {
                    hibSub = r.subtitle;
                    break;
                }
            }
            const bool canHib = SysTools::hibernationEnabled();
            std::printf("INFO 这台机器能休眠=%d，那一行写的是：%s\n", canHib ? 1 : 0,
                        hibSub.toUtf8().constData());
            check("没开休眠的那台机器，那一行点名 powercfg /h on（开了的不许出现这句）",
                  (canHib != hibSub.contains(QStringLiteral("powercfg"))) ? QStringLiteral("yes")
                                                                        : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // 反向那条是安全绳：只有**整条相等**才算要点电源，搜"关机后…"绝不能把机器关了
            const QString nearMiss = powerOf(QStringLiteral("关机后怎么恢复文件"));
            check("整条相等才算：搜「关机后怎么恢复文件」不许把关机那条排第一",
                  nearMiss == QStringLiteral("power:shutdown") ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("no"));
            // 英文词按"命令行形状"放宽（他 2026-10-19 问"为什么没有 shutdown 的下拉选项"）：
            // `shutdown.exe` / `shutdown /s` 是要动手，`shutdown 怎么用` 是在查资料。
            const auto hasPower = [&](const QString& s, const char* want) {
                input->setText(s);
                settle();
                for (int i = 0; i < w.resultCount(); ++i)
                    if (w.resultAt(i).target == QLatin1String(want))
                        return true;
                return false;
            };
            check("输 shutdown.exe 也算要点电源",
                  hasPower(QStringLiteral("shutdown.exe"), "power:shutdown") ? QStringLiteral("yes")
                                                                            : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("输 shutdown /s 也算",
                  hasPower(QStringLiteral("shutdown /s"), "power:shutdown") ? QStringLiteral("yes")
                                                                           : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("但「shutdown 怎么用」不算（那是查资料，不许把机器关了）",
                  hasPower(QStringLiteral("shutdown 怎么用"), "power:shutdown") ? QStringLiteral("yes")
                                                                               : QStringLiteral("no"),
                  QStringLiteral("no"));
            // 他 2026-10-19 补的一句：`shut` 也要出关机。走的是同一条"尾巴"规矩，
            // 所以 `shut` / `shut.exe` 算，`shutter`、`shut down 怎么用` 这种尾巴是普通词的不算。
            check("输 shut 也算要点电源",
                  hasPower(QStringLiteral("shut"), "power:shutdown") ? QStringLiteral("yes")
                                                                     : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("shut 后面带开关也算（shut /f）",
                  hasPower(QStringLiteral("shut /f"), "power:shutdown") ? QStringLiteral("yes")
                                                                       : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("但 shut 当前缀不算：shutter / 「shut down 怎么用」不许把机器关了",
                  (!hasPower(QStringLiteral("shutter"), "power:shutdown")
                   && !hasPower(QStringLiteral("shut down 怎么用"), "power:shutdown"))
                      ? QStringLiteral("yes")
                      : QStringLiteral("no"),
                  QStringLiteral("yes"));
            check("中文那条没放宽：「关机」整条才算，「关机后…」不算",
                  (hasPower(QStringLiteral("关机"), "power:shutdown")
                   && !hasPower(QStringLiteral("关机后怎么恢复文件"), "power:shutdown"))
                      ? QStringLiteral("yes")
                      : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // 他报的第三件事：搜 `gg` 搜不到 Google。原因是「广告弹窗」的首字母也是 gg，
            // 工具直达那一节 return 得早，网站那节根本没机会跑。规则改成"显式网站关键字 >
            // 工具名首字母拼音"之后：gg 归 Google，广告弹窗还能靠整串首字母 ggdc 或 广告 到达。
            // 这条要看 webs.json 里到底有没有配 gg —— 没有就点名跳过（CI 那台是干净配置，
            // 拿它判红就是环境依赖的假故障）。行是 delegate 画的，所以读 w.resultAt() 那一份。
            bool hasGg = false;
            for (const WebEntry& we : WebShortcuts::loadWebs())
                if (we.keys.contains(QStringLiteral("gg"))
                    || we.name.compare(QStringLiteral("gg"), Qt::CaseInsensitive) == 0)
                    hasGg = true;
            if (!hasGg) {
                std::printf("INFO 跳过 gg 那三条：这台机器的 webs.json 里没配 gg 这个关键字\n");
                std::fflush(stdout);
            } else {
                const auto resultTexts = [&w] {
                    QStringList out;
                    for (int i = 0; i < w.resultCount(); ++i) {
                        const SearchResult& r = w.resultAt(i);
                        if (r.isSection)
                            continue;
                        out << (r.title + QStringLiteral(" / ") + r.subtitle);
                    }
                    return out;
                };
                input->setText(QStringLiteral("gg"));
                settle();
                const QStringList r1 = resultTexts();
                bool googleRow = false;
                for (const QString& t : r1)
                    if (t.contains(QStringLiteral("Google")))
                        googleRow = true;
                std::printf("INFO 端到端 gg：前两条[%s]\n",
                            QStringList(r1.mid(0, 2)).join(QStringLiteral(" | ")).left(100).toUtf8().constData());
                std::fflush(stdout);
                check("gg 搜得到网站那一条（不被「广告弹窗」的首字母抢走）",
                      QStringLiteral("%1/%2").arg(googleRow ? 1 : 0)
                          .arg(hasSection(QStringLiteral("工具")) ? 1 : 0),
                      "1/0");
                input->setText(QStringLiteral("gg ltool"));
                settle();
                const QStringList r2 = resultTexts();
                bool searchRow = false;
                for (const QString& t : r2)
                    if (t.contains(QStringLiteral("ltool")) && t.contains(QStringLiteral("搜索")))
                        searchRow = true;
                std::printf("INFO 端到端 gg ltool：前两条[%s]\n",
                            QStringList(r2.mid(0, 2)).join(QStringLiteral(" | ")).left(100).toUtf8().constData());
                std::fflush(stdout);
                check("gg 空格 查询词 多出一行「在 Google 搜索」", searchRow ? "yes" : "no", "yes");
                const auto t5 = query(QStringLiteral("ggdc"));   // 广告弹窗的整串首字母
                check("工具那边没被弄哑：ggdc 还是直达「广告弹窗」", t5.first ? "yes" : "no", "yes");
            }
            // 他报的第四件事：搜 `An` 搜不到 Antigravity。同样先看索引里有没有这个名字
            //（CI 那台没装它，拿它判红就是环境依赖的假故障）。
            bool hasAnti = false;
            for (const AppEntry& a2 : apps)
                if (a2.name.startsWith(QLatin1String("Antigravity"), Qt::CaseInsensitive))
                    hasAnti = true;
            if (!hasAnti) {
                std::printf("INFO 跳过 An 那两条：这台机器的应用索引里没有 Antigravity\n");
                std::fflush(stdout);
            } else {
                input->setText(QStringLiteral("An"));      // 首字母大写也照样要能搜到
                settle();
                QStringList appRows;
                for (int i = 0; i < w.resultCount(); ++i)
                    if (w.resultAt(i).kind == SearchResult::App)
                        appRows << w.resultAt(i).title;
                bool antiRow = false;
                for (const QString& s : appRows)
                    if (s.startsWith(QLatin1String("Antigravity")))
                        antiRow = true;
                const bool toolSectAn = hasSection(QStringLiteral("工具"));
                std::printf("INFO 端到端 An：小节=[%s] 应用行=%d 工具节=%d 前 8=[%s]\n",
                            sections().toUtf8().constData(), int(appRows.size()),
                            toolSectAn ? 1 : 0,
                            QStringList(appRows.mid(0, 8)).join(QStringLiteral(" | ")).toUtf8().constData());
                std::fflush(stdout);
                check("An 搜得到 Antigravity（英文前缀，大写开头也一样）", antiRow ? "yes" : "no", "yes");
                // 抢页的那一站要单独断：光看"有没有 Antigravity 行"，将来别处再冒出一个
                // 生僻读音命中，测试只会红在"行数不对"上，看不出是直达又吃掉了整页。
                check("An 不该有工具直达这一节（「广告」的 an 是生僻读音，没资格独占）",
                      toolSectAn ? "yes" : "no", "no");
                input->setText(QStringLiteral("antig"));
                settle();
                bool antiRow2 = false;
                for (int i = 0; i < w.resultCount(); ++i)
                    if (w.resultAt(i).kind == SearchResult::App
                        && w.resultAt(i).title.startsWith(QLatin1String("Antigravity")))
                        antiRow2 = true;
                check("长一点的前缀 antig 也照样搜得到（不是只测两字母那种边界）", antiRow2 ? "yes" : "no", "yes");
            }
            // 2026-10-16 立的新规矩：直达**没有"独占整页"这个特权**了。这一组钉的是新那件事 ——
            // 工具仍然摆第一行（回车还是它），但后面几节照常出，谁也不许被铲平。
            // 为什么必须单独钉：把 return 摘掉之后，上面所有旧判据一条都没红 —— 也就是说
            // 它们里面没有一条盯得住"有人把 return 加回来"，那正是这套测试最容易被悄悄退回老行为的地方。
            // "第一行是谁"要跳过小节标题那条 —— 回车激活的是第一条**可点**的行，
            // 断言必须量同一个东西，不然第一节 "工具" 那个标题行会把断言指错。
            const auto headIsTool = [&w] {
                for (int i = 0; i < w.resultCount(); ++i) {
                    const SearchResult& r = w.resultAt(i);
                    if (r.isSection || !r.actionable)
                        continue;
                    return r.target.startsWith(QStringLiteral("tool:"));
                }
                return false;
            };
            {
                input->setText(QStringLiteral("cpql"));   // 磁盘清理的整串首字母：一定算直达
                settle();
                std::printf("INFO 端到端 cpql：小节=[%s] 第一行=[%s]\n",
                            sections().toUtf8().constData(),
                            w.resultCount() > 0 ? w.resultAt(0).title.toUtf8().constData() : "");
                std::fflush(stdout);
                check("直达改完还是排第一（回车那条仍然是工具，不是应用）",
                      headIsTool() ? "yes" : "no", "yes");
                // 这条是"老代码在这里 return"的直接探针：兜底那行是函数最后才加的，
                // 只要有人把独占加回来，它立刻消失 —— 不依赖任何一台机器装了什么。
                bool netFallback = false;
                for (int i = 0; i < w.resultCount(); ++i)
                    if (w.resultAt(i).title.contains(QStringLiteral("用搜索引擎搜索")))
                        netFallback = true;
                check("直达不清场：末尾那条「用搜索引擎搜索」加得回来（老行为当场红）",
                      netFallback ? "yes" : "no", "yes");
            }
            // 真实碰撞那一类：查询串既是某工具的关键词、又是某个应用的名字开头。
            // 挑 `disk` 而不是 `python`：这台机器的 webs.json 里显式配了 python/rust/cpp/npm/pypi/qt/docker
            // 这些网站关键字，`python` 走的是 webKeywordWins 那条"网站让工具让路"的规矩（他自己拍的），
            // 用它当探针测到的是那条规矩、不是"直达不清场"。disk 没配在网站里，才是干净的那一条。
            // 索引里没有以 disk 开头的应用就点名跳过（CI 那台是干净索引，判红就是环境依赖的假故障）。
            bool hasDiskApp = false;
            for (const AppEntry& a : apps)
                if (a.name.startsWith(QLatin1String("disk"), Qt::CaseInsensitive))
                    hasDiskApp = true;
            if (!hasDiskApp) {
                std::printf("INFO 跳过 disk 那两条：这台机器的应用索引里没有 Disk* 应用\n");
                std::fflush(stdout);
            } else {
                input->setText(QStringLiteral("disk"));
                settle();
                const bool diskToolHead = headIsTool();
                const bool diskAppSect = hasSection(QStringLiteral("── 应用 ──"));
                std::printf("INFO 端到端 disk：小节=[%s] 第一行是工具=%d 应用节=%d\n",
                            sections().toUtf8().constData(), diskToolHead ? 1 : 0, diskAppSect ? 1 : 0);
                std::fflush(stdout);
                check("disk 这种撞名：工具还在第一行（回车那条）", diskToolHead ? "yes" : "no", "yes");
                check("disk 这种撞名：应用那一节不再被铲平（老行为 = Disk Cleanup 整个搜不出来）",
                      diskAppSect ? "yes" : "no", "yes");
            }
            // ---- 打包应用那一族（shell:AppsFolder，2026-10-20 加的第四个索引来源）----
            // 他报的原始形状是"开始菜单搜得到 ChatGPT，ltool 搜索框搜不到"：那台机器开始菜单
            // 312 条里有 43 条是打包应用（ChatGPT = OpenAI.Codex_2p2nqsd0c76g0!App），
            // 磁盘上没有 .lnk，老的三个来源一颗都覆盖不到。
            // 量的是**现扫**（AppScanner::scan()）+ **那道缝本身**（Platform::packagedApps()），
            // 不是盘上那份 apps.json —— 第一版我拿 loadIndex() 的量，反向把拒收规则撤掉判据
            // 照样绿：量的其实是上次 --smoke 写好的那份文件（"判据要量被测代码真造的对象"又踩一遍）。
            const auto rawPackaged = Platform::packagedApps();
            const QVector<AppEntry> scanned = AppScanner::scan();
            const QString appsPrefix = QStringLiteral("shell:AppsFolder\\");
            int pkgCount = 0;
            int pkgBadEntry = 0;
            int pkgJunk = 0;
            for (const AppEntry& a : scanned) {
                if (a.category != QLatin1String("appsfolder"))
                    continue;
                ++pkgCount;
                if (!a.lnkPath.startsWith(appsPrefix))
                    ++pkgBadEntry;
                const QString aumid = a.lnkPath.mid(appsPrefix.size());
                if (aumid.startsWith(QLatin1String("Microsoft.AutoGenerated."), Qt::CaseInsensitive)
                    || aumid.contains(QLatin1Char(':'))
                    || aumid.contains(QLatin1Char('\\')))
                    ++pkgJunk;
                if (aumid == a.lnkPath)
                    ++pkgJunk;                       // 前缀没拼上 = 入口根本不是那个写法
                const QString low = a.name.toLower();
                if (low.contains(QLatin1String("uninstall"))
                    || a.name.startsWith(QString::fromUtf8("\xe5\x8d\xb8\xe8\xbd\xbd")))
                    ++pkgJunk;
            }
            std::printf("INFO 打包应用：那道缝列出 %d 条、现扫的索引里收了 %d 颗（入口不对 %d、垃圾 %d）\n",
                        int(rawPackaged.size()), pkgCount, pkgBadEntry, pkgJunk);
            if (rawPackaged.isEmpty()) {
                std::printf("INFO 跳过打包应用那四条：这台机器列不出 shell:AppsFolder（老系统或被策略关掉）\n");
                std::fflush(stdout);
            } else {
                check("缝能列出东西时索引里就必须收到这一族（一颗都没收=接了没接上）",
                      pkgCount > 0 ? "yes" : "no", "yes");
                check("每一颗的启动入口都是 shell:AppsFolder 加 AUMID 那个写法",
                      pkgBadEntry == 0 ? "yes" : "no", "yes");
                check("没把开始菜单 .lnk 的镜像（AutoGenerated）和卸载器收进第二遍",
                      pkgJunk == 0 ? "yes" : "no", "yes");
                // 撞名只钉这一族：.lnk 与"应用目录"之间本来就有重名（Qoder / nasm / YierPai…），
                // 那是历史形状、不是这次改动该管的；打包这一族必须让位，不然搜索框里两条一样。
                QSet<QString> others;
                for (const AppEntry& a : scanned)
                    if (a.category != QLatin1String("appsfolder"))
                        others.insert(a.name.toLower());
                int pkgDup = 0;
                for (const AppEntry& a : scanned)
                    if (a.category == QLatin1String("appsfolder")
                        && others.contains(a.name.toLower()))
                        ++pkgDup;
                std::printf("INFO 打包应用里和别的来源同名的：%d 颗\n", pkgDup);
                check("打包应用不许和别的来源同名（撞名那条让位给快捷方式）",
                      pkgDup == 0 ? "yes" : "no", "yes");
            }
            // 端到端：这台装了 ChatGPT 才跑（CI 那台没有就点名跳过，红的是环境不是代码）。
            // 这条看的是搜索框自己那份索引（apps），因为"能不能搜到"就是它的链。
            bool hasChatGpt = false;
            for (const AppEntry& a : apps)
                if (a.name.compare(QLatin1String("ChatGPT"), Qt::CaseInsensitive) == 0)
                    hasChatGpt = true;
            if (!hasChatGpt) {
                std::printf("INFO 跳过 ChatGPT 那一条：这份索引里没有这颗打包应用\n");
                std::fflush(stdout);
            } else {
                input->setText(QStringLiteral("chatgpt"));
                settle();
                bool appRow = false;
                for (int i = 0; i < w.resultCount(); ++i)
                    if (w.resultAt(i).kind == SearchResult::App
                        && w.resultAt(i).title.compare(QLatin1String("ChatGPT"),
                                                       Qt::CaseInsensitive) == 0)
                        appRow = true;
                std::printf("INFO 端到端 chatgpt：小节=[%s] 出应用行=%d\n",
                            sections().toUtf8().constData(), appRow ? 1 : 0);
                check("搜 chatgpt 出的是那颗打包应用（他报的原始形状）", appRow ? "yes" : "no", "yes");
            }
            // 他报的第二件事：搜 qq 找不到 QQ。这台机器上 QQ 在应用索引里（apps.json 有
            // name=QQ、target 在盘上），所以这里验的是搜索框那一条链，不是索引。
            bool hasQQ = false;
            for (const AppEntry& a : apps) {
                if (a.name.compare(QLatin1String("QQ"), Qt::CaseInsensitive) == 0) {
                    hasQQ = true;
                    break;
                }
            }
            if (hasQQ) {
                input->setText(QStringLiteral("qq"));
            settle();
            std::printf("INFO 端到端 qq：小节=[%s] 可执行行=%d\n",
                        sections().toUtf8().constData(), int(rows()));
            check("qq 能搜到应用（这台机器上 QQ 就在应用索引里）",
                  hasSection(QStringLiteral("── 应用 ──")) ? "yes" : "no", "yes");
            // 第一行必须是 QQ 本人。以前不是：使用次数记在 lnkPath 上，而目录扫来的那些没有 lnk，
            // 全共用同一个空键（这台机器那个键涨到 43），QQEX 就靠"别人家的 43 次"压在 QQ 前面。
            QString firstApp;
            for (int i = 0; i < w.resultCount(); ++i)
                if (w.resultAt(i).kind == SearchResult::App) {
                    firstApp = w.resultAt(i).title;
                    break;
                }
            check("搜 qq 的第一行就是 QQ（使用次数不再共用那个空键）", firstApp, QStringLiteral("QQ"));
            // 中文输入法在"全角"下打出来的 ＱＱ 不是 ASCII 的 qq —— 这条就是他报的那个形状，
            // 现在归一到半角，两种打法都必须出同一个应用节
            input->setText(QString::fromUtf8("\xef\xbc\xb1\xef\xbc\xb1"));
            settle();
            std::printf("INFO 端到端 全角ＱＱ：小节=[%s] 可执行行=%d\n",
                        sections().toUtf8().constData(), int(rows()));
            check("全角ＱＱ 也当半角 qq 用（输入法切错不该搜不到）",
                  hasSection(QStringLiteral("── 应用 ──")) ? "yes" : "no", "yes");
            } else {
                std::printf("INFO 本机应用索引里没有 QQ，跳过针对 QQ 本身的端到端特例校验\n");
            }
            input->setText(QStringLiteral("qq"));
            settle();
            input->setText(QStringLiteral("rw"));
            settle();
            const bool appSect2 = hasSection(QStringLiteral("── 应用 ──"));
            check("rw（首字母）也能搜到应用", appSect2 ? "yes" : "no", "yes");
            // 应用那一侧：拿真实索引里第一个中文名当靶子，用它的拼音去搜必须回到它
            QString cjkName;
            for (const AppEntry& a : apps)
                if (!Pinyin::fullReadings(a.name).isEmpty() && a.name != a.name.toLower()) {
                    bool hasHan = false;
                    for (const QChar& c : a.name)
                        if (c.unicode() >= 0x3400 && c.unicode() <= 0x9FFF)
                            hasHan = true;
                    if (hasHan) { cjkName = a.name; break; }
                }
            if (cjkName.isEmpty()) {
                std::printf("INFO 本机应用索引里没有中文名应用，应用那一侧的端到端只验节标题逻辑\n");
            } else {
                const QString py = Pinyin::fullReadings(cjkName).first();
                input->setText(py);
                settle();
                std::printf("INFO 用「%s」的拼音 %s 搜：应用节=%d 行数=%d\n",
                            cjkName.toUtf8().constData(), py.toUtf8().constData(),
                            hasSection(QStringLiteral("── 应用 ──")) ? 1 : 0, rows());
                check("中文应用名可以用拼音搜到",
                      hasSection(QStringLiteral("── 应用 ──")) ? "yes" : "no", "yes");
            }
            input->clear();
            settle();
        }
        std::printf("pinyin: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }

    // --archspeed [每文件KB] [文件数]：量压缩包读写的吞吐（MB/s），并拿系统里的 7-Zip 当参照，
    // 用来回答"解压/压缩很慢"到底慢在哪：容器格式、编码器单线程、还是每条目录项的开销。
    // 默认 4096KB × 15 = 60MB 的伪日志文本；海量小文件那一档用 `--archspeed 2 2000`。
    // 夹具全在 %TEMP%\ltool-archspeed，不碰用户目录。
    if (args.contains("--archspeed")) {
        const QStringList rest = args.mid(args.indexOf("--archspeed") + 1);
        const qint64 fileKb = rest.value(0, QStringLiteral("4096")).toLongLong();
        const int fileCount = rest.value(1, QStringLiteral("15")).toInt();
        const QString root = QDir::tempPath() + QStringLiteral("/ltool-archspeed");
        QDir(root).removeRecursively();
        const QString src = root + QStringLiteral("/src");
        QDir().mkpath(src);

        quint64 s = 0x2545F4914F6CDD1DULL;
        qint64 total = 0;
        for (int i = 0; i < fileCount; ++i) {
            QFile o(src + QStringLiteral("/f%1.log").arg(i, 4, 10, QLatin1Char('0')));
            if (!o.open(QIODevice::WriteOnly | QIODevice::Truncate))
                continue;
            QByteArray body;
            body.reserve(int(fileKb) * 1024);
            while (qint64(body.size()) < fileKb * 1024) {
                s ^= s << 13;
                s ^= s >> 7;
                s ^= s << 17;
                body += "2026-10-11 12:00:00 INFO worker-";
                body += QByteArray::number(qint64(s % 97));
                body += " processed item ";
                body += QByteArray::number(qint64(s % 100000));
                body += " in ";
                body += QByteArray::number(qint64(s % 500));
                body += "ms status=OK\n";
            }
            body.truncate(int(fileKb) * 1024);
            o.write(body);
            total += body.size();
        }
        std::printf("INFO 夹具：%d 个文件 × %lld KB = %.1f MB（伪日志文本，压得动的那种）\n",
                    fileCount, fileKb, double(total) / 1048576.0);

        QElapsedTimer t;
        const auto rate = [](qint64 bytes, qint64 ms) {
            return ms > 0 ? double(bytes) / 1048576.0 * 1000.0 / double(ms) : 0.0;
        };
        const auto row = [&](const char* op, const char* who, qint64 bytes, qint64 ms, qint64 outBytes) {
            std::printf("%s %-12s %8lld ms  %7.1f MB/s  产物 %6.1f MB\n", op, who, ms,
                        rate(bytes, ms), double(outBytes) / 1048576.0);
            std::fflush(stdout);
        };
        const auto ours = [&](const QString& pkg, const QString& dest) {
            t.restart();
            const Archive::Result r = Archive::extract(pkg, dest, nullptr, nullptr);
            return qMakePair(t.elapsed(), r.ok ? r.files : -1);
        };

        const QString seven = QStringLiteral("C:/Program Files/7-Zip/7z.exe");
        const bool have7z = QFileInfo::exists(seven);
        int last7zRc = -99;
        const auto run7z = [&](const QStringList& a) {
            QElapsedTimer e;
            e.start();
            QProcess p;
            p.setProgram(seven);
            p.setArguments(a);
            p.setProcessChannelMode(QProcess::SeparateChannels);
            p.start();
            if (!p.waitForFinished(600000))
                return qint64(-1);
            last7zRc = p.exitCode();
            return e.elapsed();
        };
        if (!have7z)
            std::printf("INFO 没装 7-Zip，跳过参照组与互操作检查\n");

        const QString ourZip = root + QStringLiteral("/our.zip");
        const QString our7z = root + QStringLiteral("/our.7z");
        t.start();
        const Archive::Result cz = Archive::compress({ src }, ourZip, false, nullptr, nullptr);
        row("压缩", "我们 zip", total, t.elapsed(), QFileInfo(ourZip).size());
        t.restart();
        const Archive::Result c7 = Archive::compress({ src }, our7z, true, nullptr, nullptr);
        row("压缩", "我们 7z=默认5", total, t.elapsed(), QFileInfo(our7z).size());
        if (!cz.ok || !c7.ok)
            std::printf("INFO 我们这边没压出来：zip=%s 7z=%s\n", cz.error.toUtf8().constData(),
                        c7.error.toUtf8().constData());
        row("解压", "我们 zip", total, ours(ourZip, root + "/d1").first, 0);
        row("解压", "我们 7z", total, ours(our7z, root + "/d2").first, 0);

        // 7z 这条现在是 7-Zip 自己的编码器（多线程），档位照旧逐档量。每档验两件事：
        // 我们自己的读取器解得回来、7-Zip 也认这个包 —— 换档位最怕的就是
        // "快是快了，别人打不开"。7-Zip 的枚举只定义了 0/1/3/5/7/9，就扫这几个。
        for (int lvl : { 1, 3, 5, 7, 9 }) {
            const QString p = root + QStringLiteral("/lv%1.7z").arg(lvl);
            t.restart();
            const Archive::Result r = Archive::compress({ src }, p, true, nullptr, nullptr, lvl);
            const qint64 ms = t.elapsed();
            const int back = ours(p, root + QStringLiteral("/lv%1d").arg(lvl)).second;
            const QString tag = QStringLiteral("我们 档%1").arg(lvl);
            row("压缩", tag.toUtf8().constData(), total, ms, QFileInfo(p).size());
            std::printf("        ok=%d 我们自己解回=%d 个文件", r.ok ? 1 : 0, back);
            if (have7z) {
                run7z({ QStringLiteral("t"), p, QStringLiteral("-y") });
                std::printf("  7z t 退出码=%d", last7zRc);
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        if (have7z) {
            const QString refZip = root + QStringLiteral("/ref.zip");
            const QString ref7z = root + QStringLiteral("/ref.7z");
            QFile::remove(refZip);
            QFile::remove(ref7z);
            // 注意别把 run7z(...) 和 QFileInfo(...).size() 写成同一个调用的两个实参：
            // C++ 不保证实参求值顺序，size 会在 7z 还没跑之前就读到一个不存在的文件（=0）
            const qint64 m1 = run7z({ QStringLiteral("a"), QStringLiteral("-tzip"),
                                      QStringLiteral("-mx=5"), refZip, src });
            row("压缩", "7z -tzip", total, m1, QFileInfo(refZip).size());
            const qint64 m2 = run7z({ QStringLiteral("a"), QStringLiteral("-t7z"),
                                      QStringLiteral("-mx=5"), ref7z, src });
            row("压缩", "7z -t7z", total, m2, QFileInfo(ref7z).size());
            const qint64 m3 = run7z({ QStringLiteral("x"), refZip,
                                      QStringLiteral("-o") + root + QStringLiteral("/r1"),
                                      QStringLiteral("-y") });
            row("解压", "7z x zip", total, m3, 0);
            const qint64 m4 = run7z({ QStringLiteral("x"), ref7z,
                                      QStringLiteral("-o") + root + QStringLiteral("/r2"),
                                      QStringLiteral("-y") });
            row("解压", "7z x 7z", total, m4, 0);
            row("解压", "我们 x 7z", total, ours(ref7z, root + "/d3").first, 0);
        }
        QDir(root).removeRecursively();
        return 0;
    }

    // --ocrtest <图片>：判据本体在 plugins/capture（符号 ltoolOcrtestRun）。
    if (args.contains("--ocrtest")) {
        const QString spec = args.mid(args.indexOf("--ocrtest") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolOcrtestRun",
                                 SelftestArg::Text, 0, spec, "ocrtest");
    }


    // --ocrlayout <图片>：判据本体在 plugins/capture（符号 ltoolOcrlayoutRun）。
    if (args.contains("--ocrlayout")) {
        const QString spec = args.mid(args.indexOf("--ocrlayout") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolOcrlayoutRun",
                                 SelftestArg::Text, 0, spec, "ocrlayout");
    }


    // --pinpick <图片>：判据本体在 plugins/capture（符号 ltoolPinpickRun）。
    if (args.contains("--pinpick")) {
        const QString spec = args.mid(args.indexOf("--pinpick") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolPinpickRun",
                                 SelftestArg::Text, 0, spec, "pinpick");
    }


    // --ocrchain <图片>：判据本体在 plugins/capture（符号 ltoolOcrchainRun）。
    if (args.contains("--ocrchain")) {
        const QString spec = args.mid(args.indexOf("--ocrchain") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolOcrchainRun",
                                 SelftestArg::Text, 0, spec, "ocrchain");
    }


    // --memloop [轮数] [宽x高]：内存这事只能量，不能猜。每轮做一遍"解压一个压不动的 32MB 包"
    // + "录一段指定尺寸的窗口"，每一步采 私有字节/工作集/句柄数。
    // 私有字节随轮数线性涨 = 真泄漏；涨一两轮就封顶 = 分配器留着页不还操作系统（那不是泄漏）。
    // 夹具和产物都在 %TEMP%\ltool-memloop 里，跑完删掉。
    // 2026-10-15：录屏本体进了 plugins/record，这一条量的是"录一整场泄多少"，判据跟着
    // Recorder 走（导出符号 ltoolMemloopRun），这儿只剩读命令行 + 点名要符号。
    if (args.contains("--memloop")) {
        const QString rArg = args.mid(args.indexOf("--memloop") + 1).value(0);
        const int rounds = rArg.isEmpty() ? 3 : qBound(1, rArg.toInt(), 12);
        const QString sizeSpec = args.mid(args.indexOf("--memloop") + 2).value(0);
        return runPluginSelftest(QStringLiteral("record"), "ltoolMemloopRun",
                                 SelftestArg::MsAndText, rounds, sizeSpec, "memloop");
    }

    // --pathcfg：验配置页那两行"存放目录"真的管用。APPDATA 已挪到 %TEMP%\ltool-pathcfg
    //（见 main() 开头），所以这一步会写 config.ini、会建目录，但碰不到你真实的文件。
    if (args.contains("--pathcfg")) {
        int bad = 0;
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const QString defCap = QCoreApplication::applicationDirPath() + "/captures";
        // 先记一份"开跑之前存不存在"：这条要判的是**这一问有没有把目录建出来**，不是"它在不在"。
        // 装在软件目录里那份 captures 本来就有用户真实的截图（本机 D:\software\ltool\captures 就是），
        // 拿"存在=no"去断言，对着安装包跑就会红一项（2026-10-15 实测：build 那份绿、装出来的那份红）。
        const bool defCapBefore = QFileInfo::exists(defCap);
        check("没配置时截图存到软件目录下（不是 C 盘）", Utils::capturesDir() == defCap ? "yes" : "no", "yes");
        check("没配置时录像存到软件目录下",
              Utils::recordingsDir() == QCoreApplication::applicationDirPath() + "/recordings" ? "yes" : "no",
              "yes");

        const QString newCap = QDir::tempPath() + "/ltool-pathcfg/新截图处";
        const QString newRec = QDir::tempPath() + "/ltool-pathcfg/新录像处";
        {
            QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
            cfg.setValue("paths/captures", QDir::toNativeSeparators(newCap));
            cfg.setValue("paths/recordings", QDir::toNativeSeparators(newRec));
            cfg.sync();   // 产品里就是靠这一步让后面新开的 QSettings 读到
        }
        check("改了配置：截图目录跟着变", Utils::capturesDir() == newCap ? "yes" : "no", "yes");
        check("改了配置：录像目录跟着变", Utils::recordingsDir() == newRec ? "yes" : "no", "yes");

        // 截一张纯色图入往期：文件必须落在新目录，而且老目录不该被建出来
        QPixmap pm(24, 24);
        pm.fill(QColor(200, 30, 30));
        const QString key = CaptureHistory::add(pm);
        std::printf("INFO 往期的 key=%s\n", key.toUtf8().constData());
        check("新截图真的写进新目录", QFile::exists(newCap + "/" + key) ? "yes" : "no", "yes");
        check("光问一句不该把软件目录下的 captures 建出来（构建目录会被安装包收走）",
              QFileInfo::exists(defCap) == defCapBefore ? "same" : "created", "same");

        CaptureHistory::reload();
        check("重载往期：还是那一张", CaptureHistory::count() == 1 ? "yes" : "no", "yes");
        check("重载往期：key 对得上", CaptureHistory::at(0).key == key ? "yes" : "no", "yes");

        QDir().mkpath(Utils::recordingsDir());
        QFile rf(Utils::recordingsDir() + "/probe.bin");
        const bool canWrite = rf.open(QIODevice::WriteOnly)
            && rf.write(QByteArray(8, '\0')) == 8;
        rf.close();
        QFile::remove(rf.fileName());
        check("录像目录能建能写", canWrite ? "yes" : "no", "yes");

        // ---- 数据目录本身落在哪（2026-10-21 他点名"所有的日志和数据都放软件目录下，不要放 C 盘"）----
        // 前三条问的是纯函数（三档优先各自给死输入）；第四条问的是**这条旗标进程真读到的那个值**，
        // 免得解析器绿了而 dataDir() 里那行 `qEnvironmentVariable` 写错名字。
        check("第一优先：LTOOL_DATA_DIR 设了就用它（哪怕软件目录写得进去）",
              Utils::resolveDataDir("D:/software/ltool", "C:/Temp/沙箱", "C:/Users/x/AppData/Roaming", true)
                  == "C:/Temp/沙箱" ? "yes" : "no", "yes");
        check("默认：软件目录写得进去就落 `<exe>/data`（不往 C 盘堆）",
              Utils::resolveDataDir("D:/software/ltool", QString(), "C:/Users/x/AppData/Roaming", true)
                  == "D:/software/ltool/data" ? "yes" : "no", "yes");
        check("退路：软件目录写不了（比如塞进 Program Files）才退回 %APPDATA%\\ltool",
              Utils::resolveDataDir("C:/Program Files/ltool", QString(), "C:/Users/x/AppData/Roaming", false)
                  == "C:/Users/x/AppData/Roaming/ltool" ? "yes" : "no", "yes");
        // 沙箱目录名是那串循环里 `tempPath + "/" + 旗标去掉两个杠` 算出来的，也就是 `pathcfg`。
        // 我第一版在这里写死成 "ltool-pathcfg"（那是别处见过的另一个名字），当场红 —— 红的是判据自己。
        check("这一趟真跑起来时 dataDir() 就是沙箱那个目录（环境变量真被读到了，不是只有解析器对）",
              QDir::cleanPath(Utils::dataDir()) == QDir::cleanPath(QDir::tempPath() + "/pathcfg")
                  ? "yes" : "no", "yes");
        std::printf("INFO 沙箱里 dataDir()=%s\n", Utils::dataDir().toUtf8().constData());
        {
            // 搬家那一段拿两个临时目录真跑一遍：源目录一个字都不许动，目标里每个文件都要对得上哈希。
            const QString from = QDir::tempPath() + "/ltool-migsrc";
            const QString to = QDir::tempPath() + "/ltool-migdst";
            QDir(from).removeRecursively();
            QDir(to).removeRecursively();
            QDir().mkpath(from + "/icons");
            const auto readBack = [](const QString& path) {
                QFile f(path);
                return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
            };
            const auto put = [](const QString& path, const QByteArray& body) {
                QFile f(path);
                f.open(QIODevice::WriteOnly | QIODevice::Truncate);
                f.write(body);
                f.close();
            };
            put(from + "/config.ini", "[ui]\ntheme=day\n");
            put(from + "/usage.json", "{\"a\":1}");
            put(from + "/icons/x.png", QByteArray(4096, 'z'));
            put(from + "/log.txt", "旧目录里那份运行日志\n");
            QStringList problems;
            int skipped = 0;
            // 目标里先放一颗同名不同内容的文件（真机上就是这一趟自己先写出来的 `log.txt`）：
            // 搬家**不许覆盖它**。本机第一次实测就是栽在这儿 —— 守卫写成"目录非空就别搬"，
            // 于是 deliver 的 installed 那一栏先跑了一次 `--plugintest`（留下一条日志），
            // relaunch 那次就判定"搬过了"，378 MB 一个字节都没动。
            QDir().mkpath(to);   // 目标目录得先存在，否则下面那颗"已有文件"根本写不出来（第一版就白绿在这儿）
            put(to + "/log.txt", "这一趟自己的日志，比旧目录里那份新\n");
            const int n = Utils::copyVerifiedTree(from, to, &problems, &skipped);
            check("搬家：三个文件（含一个子目录里的）全部复制并逐个按 sha256 对过",
                  n == 3 && problems.isEmpty() ? "yes" : "no", "yes");
            check("搬家：目标里已有的同名文件不覆盖（那条是这一趟自己写的 log.txt）",
                  skipped == 1 && readBack(to + "/log.txt") == QStringLiteral("这一趟自己的日志，比旧目录里那份新\n")
                      ? "yes" : "no", "yes");
            check("搬家：目标里那个子目录真的建出来了、文件也在",
                  QFile::exists(to + "/icons/x.png") ? "yes" : "no", "yes");
            check("搬家：源目录一个字都不动（旧的那份是回退用的，不是复制完就删）",
                  QFile::exists(from + "/config.ini") && QFile::exists(from + "/icons/x.png")
                      ? "yes" : "no", "yes");
            {
                // 第一版这里连着 new 了两个 QFile：第二个没 open 就 readAll，拿到空串 ⇒ 判据自己红。
                QFile dst(to + "/config.ini");
                const bool opened = dst.open(QIODevice::ReadOnly);
                const QByteArray body = opened ? dst.readAll() : QByteArray();
                check("搬家：内容对得上（拿最要紧那个 config.ini 比字节）",
                      body == QByteArrayLiteral("[ui]\ntheme=day\n") ? "yes" : "no", "yes");
            }
            {
                // 程序自带的默认表必须给用户数据让路。2026-10-21 真丢过一次：deliver 的 `installed`
                // 那一栏先拿装好的 exe 跑了一次 `--plugintest`，那一趟在**还没搬**的数据目录里写下了
                // 默认 webs.json（2 条），于是"目标已有同名就不覆盖"把你那份 24 条永远挡在了老目录里。
                const QString stockBox = QDir::tempPath() + "/ltool-stockway";
                QDir(stockBox).removeRecursively();
                QDir().mkpath(stockBox);
                put(stockBox + "/webs.json", WebShortcuts::stockWebsJson());
                check("目标里那份逐字节等于内置默认的 webs.json 会让路",
                      QString::number(Utils::clearStockDefaultsForMigration(stockBox)), "1");
                check("让路是真删掉（留个空文件的话，复制那一段仍然当它存在）",
                      QFile::exists(stockBox + "/webs.json") ? QStringLiteral("no") : QStringLiteral("yes"),
                      "yes");
                put(stockBox + "/webs.json", QStringLiteral("这是用户自己改过的一份").toUtf8());
                check("内容不是默认表的目标文件一个字节都不许动",
                      QString::number(Utils::clearStockDefaultsForMigration(stockBox))
                          + QStringLiteral("/")
                          + (QFile::exists(stockBox + "/webs.json") ? QStringLiteral("kept")
                                                                    : QStringLiteral("gone")),
                      "0/kept");
                QDir(stockBox).removeRecursively();
            }
            {
                // 「还没接住真数据的目录不许播默认表」那两道守卫的判断（WebShortcuts / DevEnv 问的就是它）。
                // 拆开问：两个目录给死，不碰真文件系统（同 resolveDataDir 那两条规矩）。
                const QString b = QDir::tempPath() + "/ltool-pending";
                QDir(b).removeRecursively();
                QDir().mkpath(b + "/old");
                QDir().mkpath(b + "/fresh");
                check("pending：老目录不存在 = 不欠任何东西（新装的机器）",
                      Utils::migrationPendingFor(b + "/gone", b + "/fresh") ? "yes" : "no", "no");
                check("pending：新目录还没搬过、老目录还在 = 是（这时候写默认表就会挡住真数据）",
                      Utils::migrationPendingFor(b + "/old", b + "/fresh") ? "yes" : "no", "yes");
                put(b + "/fresh/config.ini", "[ui]\ntheme=day\n");
                check("pending：新目录里已经有一份真配置 = 它已经是你的家了",
                      Utils::migrationPendingFor(b + "/old", b + "/fresh") ? "yes" : "no", "no");
                QFile(b + "/fresh/config.ini").remove();
                put(b + "/fresh/.migrated-from-appdata", "搬过\n");
                check("pending：搬过的标记也在（两种真信号认一个就算）",
                      Utils::migrationPendingFor(b + "/old", b + "/fresh") ? "yes" : "no", "no");
                check("pending：新目录就是老目录（还没落到软件目录下）= 没什么可搬的",
                      Utils::migrationPendingFor(b + "/old", b + "/old") ? "yes" : "no", "no");
                // 这一条盯的是"沙箱那一档"：设着 LTOOL_DATA_DIR 时这颗必须说 false，
                // 不然每一条离屏判据都会被自己的守卫挡住（默认表播不进去，夹具就建不起来）。
                check("这一趟真跑起来时 pending 是 false（沙箱设着 LTOOL_DATA_DIR，该写默认表就写）",
                      Utils::legacyMigrationPending() ? "yes" : "no", "no");
                QDir(b).removeRecursively();
            }
            {
                // 已经搬过的那一趟补一次：只认"现在这份历史引用着、而本地缺"的图。
                // 走的是 migrateBetween 那条真路（漏调用处 = helper 绿了也白绿，上面刚栽过一次）。
                const QString b = QDir::tempPath() + "/ltool-backfill";
                QDir(b).removeRecursively();
                QDir().mkpath(b + "/old/clipimg");
                QDir().mkpath(b + "/fresh");
                put(b + "/old/clipimg/abc.png", QByteArray(64, 'A'));
                put(b + "/old/clipimg/abc.t120.png", QByteArray(8, 'a'));
                put(b + "/old/clipimg/unreferenced.png", QByteArray(32, 'U'));   // 他后来删掉的那一张
                put(b + "/fresh/.migrated-from-appdata", "搬过\n");
                put(b + "/fresh/clipboard.json",
                    QStringLiteral("[{\"image\":true,\"path\":\"%1/clipimg/abc.png\"}]").arg(b + "/old")
                        .toUtf8());
                QString note;
                const int fixed = Utils::migrateBetween(b + "/old", b + "/fresh", &note);
                check("已经搬过的那一趟会补图：补了几个（原图 + 它的缩略图）", QString::number(fixed), "2");
                check("补进来的就是历史引用着的那两张",
                      QStringLiteral("%1,%2")
                          .arg(QFile::exists(b + "/fresh/clipimg/abc.png") ? "y" : "n")
                          .arg(QFile::exists(b + "/fresh/clipimg/abc.t120.png") ? "y" : "n"),
                      "y,y");
                check("历史里没引用的一张都不许复活（不然你删掉的往期图每次启动都回来）",
                      QFile::exists(b + "/fresh/clipimg/unreferenced.png") ? "yes" : "no", "no");
                check("note 说了补回几个、补的是哪些",
                      note.contains(QStringLiteral("补回 2 个")) && note.contains(QStringLiteral("clipimg/abc.png"))
                          ? "yes" : QStringLiteral("note=[%1]").arg(note),
                      "yes");
                put(b + "/fresh/clipimg/abc.png", QStringLiteral("%1").arg(QChar('Z')).repeated(99).toUtf8());
                QString note2;
                const int again = Utils::migrateBetween(b + "/old", b + "/fresh", &note2);
                const QByteArray kept = readBack(b + "/fresh/clipimg/abc.png").toUtf8();
                check("跑第二遍什么都不补，也不动本地已有的那一份",
                      QStringLiteral("%1/%2").arg(again)
                          .arg(kept == QByteArray(99, 'Z') ? QStringLiteral("kept")
                                                           : QStringLiteral("changed")),
                      QStringLiteral("0/kept"));
                QDir(b).removeRecursively();
            }
            {
                // 上面那三条量的是 helper。**漏掉调用处**才是这次的真实事故（搬家跑了、helper 也对，
                // 只是那条链没叫它），所以这里把整条链再跑一遍：两个临时目录给死，
                // 老目录放你那份 24 条，新目录先放程序自己写的默认表（2 条）+ 一颗这一趟的 log.txt。
                // 判的是"界面最后看到几条"，不是"某颗函数返回值是几"。
                const QString box = QDir::tempPath() + "/ltool-chain";
                const QString oldDir = box + "/legacy";
                const QString newDir = box + "/now";
                QDir(box).removeRecursively();
                QDir().mkpath(oldDir);
                QDir().mkpath(newDir);
                QByteArray yours = "{\"webs\":[{\"k\":\"ZZZ你那份\",\"n\":\"24 条里的那一条\","
                                   "\"u\":\"https://example.invalid\"}]}";
                put(oldDir + "/webs.json", yours);
                put(oldDir + "/usage.json", "{\"freq\":24}");
                put(newDir + "/webs.json", WebShortcuts::stockWebsJson());   // 上一趟自测自己写下的默认表
                put(newDir + "/log.txt", "这一趟自己的日志\n");
                QString note;
                const int moved = Utils::migrateBetween(oldDir, newDir, &note);
                const QByteArray after = readBack(newDir + "/webs.json").toUtf8();
                check("搬家整条链：新目录里那份默认表没挡住老目录的真数据，界面上看到的是你那份",
                      after == yours ? QStringLiteral("yes")
                                      : QStringLiteral("%1 字节 vs %2 字节").arg(after.size()).arg(yours.size()),
                      QStringLiteral("yes"));
                check("搬家整条链：搬成的个数（默认表让路 + 这一趟的 log.txt 仍不覆盖）",
                      QString::number(moved), QStringLiteral("2"));
                check("搬家整条链：默认表那份确实是让路删掉的（不是复制时覆盖了目标）",
                      note.contains(QStringLiteral("1 份程序自带的默认表给用户数据让了路"))
                          ? QStringLiteral("yes") : QStringLiteral("note=[%1]").arg(note),
                      QStringLiteral("yes"));
                check("搬家整条链：源目录一个字都不动，回退用的旧数据还在",
                      QFile::exists(oldDir + "/webs.json") && QFile::exists(oldDir + "/usage.json")
                          ? QStringLiteral("yes") : QStringLiteral("no"),
                      QStringLiteral("yes"));
                QString note2;
                const int again = Utils::migrateBetween(oldDir, newDir, &note2);
                check("搬家整条链：跑第二遍不再搬（新目录里已经有真配置了）",
                    QStringLiteral("%1").arg(again), QStringLiteral("0"));
                QDir(box).removeRecursively();
            }
            QDir(from).removeRecursively();
            QDir(to).removeRecursively();
        }
        {
            // **冷启动那条路自己得能走通**：上面那个守卫的形状是"一个 `--` 旗标都没有才搬家"，
            // 而这一套判据、以及所有别的自测，全都带着旗标跑 —— 那条分支一次都没被任何判据踩过。
            // 本机就是这么漏掉一个必崩的：`app.arguments()` 按值返回，`.cbegin()` 和 `.cend()` 分属
            // 两个临时 QStringList，没有旗标时那头指针越过末尾去解引用别人家的堆 ⇒ 0xC0000005，
            // 现象是"双击图标就闪退，而跑任何一条自测都好好的"。
            // 只能起子进程、且**一个旗标都不给**，才是被测的那条路。
            // 有没有常驻实例：**不能用 QLocalSocket 连一下问** —— 那条 server 那边算"第二个实例来交接"，
            // 他那份常驻实例会当场把启动器叫到屏幕上（本机 21:15 就这么发生了一次，日志里留着那句
            // `show: 呼出（要求者=第二个实例转交过来的 show）`）。命名管道问一句就够了：
            // `WaitNamedPipeW` 只看"有没有人在听"，不建立连接、不写一个字节。
            const QString pipe = QStringLiteral("\\\\.\\pipe\\") + QString::fromWCharArray(kInstanceKey);
            if (WaitNamedPipeW((LPCWSTR)pipe.utf16(), 0)) {
                std::printf("SKIP 冷启动子进程 —— 这台机器上有常驻实例，它会把启动器叫到他屏幕上，不打扰\n");
            } else {
                const QString box = QDir::tempPath() + "/ltool-coldstart";
                QDir(box).removeRecursively();
                QDir().mkpath(box);
                // 截图/录像的落点是 exe 旁边（软件目录），不给它指进沙箱就会在构建目录留下 captures\，
                // 那个会被安装包收走（同一族的坑，见 --pindel 那里）。
                QSettings boxCfg(box + "/config.ini", QSettings::IniFormat);
                boxCfg.setValue(QStringLiteral("paths/captures"), box + "/captures");
                boxCfg.setValue(QStringLiteral("paths/recordings"), box + "/recordings");
                boxCfg.sync();
                QProcess child;
                QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
                pe.insert(QStringLiteral("LTOOL_DATA_DIR"), box);
                pe.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
                child.setProcessEnvironment(pe);
                QElapsedTimer et;
                et.start();
                child.start(QCoreApplication::applicationFilePath(), {});   // ← 一个参数都不给
                child.waitForStarted(5000);
                const bool stillRunning = child.waitForFinished(6000);      // true = 到点也没退 = 活着
                QString verdict;
                if (child.state() != QProcess::NotRunning)
                    verdict = QStringLiteral("alive");
                else if (child.exitStatus() == QProcess::CrashExit)
                    verdict = QStringLiteral("crashed-0x%1").arg(quint32(child.exitCode()), 8, 16, QLatin1Char('0'));
                else
                    verdict = QStringLiteral("exit%1").arg(child.exitCode());
                check("冷启动（不给任何旗标）6 秒内不许崩", verdict, "alive");
                std::printf("INFO 冷启动子进程 verdict=%s ms=%lld box=%s\n", verdict.toUtf8().constData(),
                            static_cast<long long>(et.elapsed()), box.toUtf8().constData());
                std::fflush(stdout);
                if (child.state() != QProcess::NotRunning) {
                    // 这个活着的孩子现在就坐在 `ltool.instance` 那根管道上 —— 顺手把
                    // **"只连不写"到底算不算"把窗口叫出来"** 量一遍：算的话，任何探测都会抢他的屏幕
                    //（本机 21:15 就是被判据自己这么抢了一次）。
                    const QString logPath = box + "/log.txt";
                    const auto showCount = [&logPath] {
                        QFile f(logPath);
                        if (!f.open(QIODevice::ReadOnly))
                            return 0;
                        return int(f.readAll().count("show: 呼出"));
                    };
                    QCoreApplication::processEvents();   // 等它把 server 挂上（它自己那 1.2 秒预热都还没完）
                    QElapsedTimer waitPipe;
                    waitPipe.start();
                    while (!WaitNamedPipeW((LPCWSTR)pipe.utf16(), 0) && waitPipe.elapsed() < 8000)
                        QThread::msleep(200);
                    const int before = showCount();
                    QLocalSocket bare;
                    bare.connectToServer(QString::fromWCharArray(kInstanceKey));
                    if (bare.waitForConnected(1500)) {
                        QThread::msleep(600);            // 那边读一次、什么都不该做
                        bare.disconnectFromServer();
                    }
                    check("只连上、一个字节都不写的连接，不许被当成「把启动器叫出来」",
                          QString::number(showCount() - before), "0");
                    QLocalSocket ask;
                    ask.connectToServer(QString::fromWCharArray(kInstanceKey));
                    if (ask.waitForConnected(1500)) {
                        ask.write("show\n");
                        ask.waitForBytesWritten(1500);
                        QThread::msleep(600);
                        ask.disconnectFromServer();
                    }
                    check("真写了 show 的那条路还得照常把窗口叫出来（别让上面那条把门焊死）",
                          showCount() - before >= 1 ? QStringLiteral("yes") : QStringLiteral("no"), "yes");
                    std::printf("INFO 交接探测 before=%d pipe=%s\n", before,
                                WaitNamedPipeW((LPCWSTR)pipe.utf16(), 0) ? "有人听" : "没人听");
                    std::fflush(stdout);
                    child.kill();   // 它是这一趟自造的实例，留着就是第二个托盘
                    child.waitForFinished(3000);
                }
                QDir(box).removeRecursively();
            }
        }
        QFile::remove(newCap + "/" + key);
        QDir(QDir::tempPath() + "/ltool-pathcfg").removeRecursively();
        std::printf("pathcfg: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }
    // --storetest：验插件商店那一条链 —— 解析清单、和本地比、按哈希落盘、改名禁用/恢复。
    // **一次都不联网**：联网那两段（拉 index.json、下 dll）只是把字节交给下面这几个纯函数，
    // 这里直接喂文件进去。数据目录已经被挪到 %TEMP%\ltool-storetest（见 main() 开头那个循环），
    // 假 dll 全写进那里面，最后按算出来的绝对路径删掉。
    if (args.contains("--storetest")) {
        int bad = 0;
        using Entry = PluginStore::Entry;   // 这一段里全是商店的条目，写全名会把判据挤出行宽
        const auto check = [&bad](const char* name, const QString& got, const QString& want) {
            const bool ok = got == want;
            if (!ok)
                ++bad;
            std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                        got.toUtf8().constData(), want.toUtf8().constData());
        };
        const QString box = Utils::dataDir() + "/store";
        QDir().mkpath(box);
        const auto word = [](PluginStore::State s) {
            return PluginStore::stateWord(s);
        };
        // 造一个"插件 dll"：内容随便，哈希是真算出来的（商店判的就是哈希，内容不需要真是 PE）
        const auto makeFile = [](const QString& path, const QByteArray& content) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return QString();
            f.write(content);
            f.close();
            return PluginStore::sha256Of(path);
        };

        // ---- 1. 清单解析：好的收下，坏的点名拒收 ----
        // 文件名一律从 platform/PluginNames.h 现算，不写死 .dll：这套判据挪到 Linux 上跑的时候，
        // 喂给校验的就得是那一侧真正认的名字。foreign 那条专门喂**另一个平台**的后缀 ——
        // 它测的是"认得边界在哪"，不是"dll 这三个字母对不对"。
        const QString goodSha = makeFile(box + "/src-good.bin", QByteArray("GOOD-DLL-CONTENT"));
        const QString otherSha = makeFile(box + "/src-other.bin", QByteArray("OTHER"));
        const QString foreignFile = QStringLiteral("ltool-foreign.")
            + (Platform::pluginFileSuffix() == QLatin1String("dll")
                   ? QStringLiteral("so") : QStringLiteral("dll"));
        const QString indexJson = QStringLiteral(
            R"({"schema":1,"core":"0.9.0","plugins":[)"
            R"({"id":"good","name":"好的那个","desc":"d","api":%2,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"%4","bytes":16,"sha256":"%1",)"
            R"("url":"https://raw.githubusercontent.com/mateo-ki/ltool_plugin/main/dist/%4"},)"
            R"({"id":"trav","name":"路径穿越","desc":"d","api":%2,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"../%5","bytes":1,"sha256":"%3",)"
            R"("url":"https://raw.githubusercontent.com/mateo-ki/ltool_plugin/main/dist/%5"},)"
            R"({"id":"plain","name":"不加密","desc":"d","api":%2,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"%6","bytes":1,"sha256":"%3",)"
            R"("url":"http://example.invalid/%6"},)"
            R"({"id":"badhash","name":"哈希不像哈希","desc":"d","api":%2,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"%7","bytes":1,"sha256":"deadbeef",)"
            R"("url":"https://example.invalid/%7"},)"
            R"({"id":"oldapi","name":"接口太旧","desc":"d","api":1,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"%8","bytes":1,"sha256":"%3",)"
            R"("url":"https://example.invalid/%8"},)"
            R"({"id":"foreign","name":"另一个平台的那颗","desc":"d","api":%2,"keywords":[],"actions":[],"color":"1,2,3",)"
            R"("file":"%9","bytes":1,"sha256":"%3",)"
            R"("url":"https://example.invalid/%9"}]})"
            ).arg(goodSha).arg(LTOOL_PLUGIN_API_VERSION).arg(otherSha)
             .arg(Platform::pluginFileName(QStringLiteral("good")),
                  Platform::pluginFileName(QStringLiteral("trav")),
                  Platform::pluginFileName(QStringLiteral("plain")),
                  Platform::pluginFileName(QStringLiteral("bad")),
                  Platform::pluginFileName(QStringLiteral("old")),
                  foreignFile);

        QStringList problems;
        const auto entries = PluginStore::parseIndex(indexJson.toUtf8(), &problems);
        // 没收下的每一条都说出来：这一面红了要能在三秒内看出是哪个字段被拒
        for (const QString& q : problems)
            std::printf("INFO 拒收 %s\n", q.toUtf8().constData());
        // 六条里 oldapi 是"条目本身没毛病、只是接口版本不对"，它要走到 compare 才被判装不了，
        // 所以解析这一步该收两条、拒四条。
        check("六条里收下两条（good 和 oldapi）", QString::number(entries.size()), "2");
        check("拒掉的四条各说一句", QString::number(problems.size()), "4");
        check("拒收点名到 id", problems.join(QStringLiteral("|")).contains(QStringLiteral("trav"))
                  && problems.join(QStringLiteral("|")).contains(QStringLiteral("plain"))
                  && problems.join(QStringLiteral("|")).contains(QStringLiteral("badhash"))
                  && problems.join(QStringLiteral("|")).contains(QStringLiteral("foreign"))
              ? "yes" : "no", "yes");
        // 这一条盯的是"界面能不能说实话"：拒的理由里必须点出**这台机器**认的那个后缀，
        // 而不是只说"不是 .dll"（Linux 上看那句话等于没说）。
        {
            QString foreignWhy;
            for (const QString& q : problems)
                if (q.contains(QStringLiteral("foreign")))
                    foreignWhy = q;
            check("拒 foreign 的理由说清了这台机器要哪个后缀",
                  foreignWhy.contains(QStringLiteral("这里要 .") + Platform::pluginFileSuffix())
                      ? "yes" : "no", "yes");
        }
        check("好的那条认得出的确是好的", entries.isEmpty() ? QStringLiteral("-") : entries[0].id, "good");
        {
            QStringList p2;
            const auto none = PluginStore::parseIndex(
                QByteArrayLiteral(R"({"schema":2,"plugins":[]})"), &p2);
            check("schema 不认就一条都不接", QString::number(none.size()), "0");
            check("schema 不认要说清是哪一侧", p2.join(QString()).contains(QStringLiteral("schema"))
                      ? "yes" : "no", "yes");
            QStringList p3;
            check("不是 JSON 也别当没事", PluginStore::parseIndex(QByteArrayLiteral(" plainly-not-json"), &p3).isEmpty()
                      && !p3.isEmpty() ? "yes" : "no", "yes");
        }

        // ---- 2. 和本地比：没装/已装/可更新/装不了 ----
        const Entry* found = nullptr;
        for (const Entry& e : entries)
            if (e.id == QLatin1String("good"))
                found = &e;
        check("收下的那两条里有 good", found ? "yes" : "no", "yes");
        if (!found) {
            std::printf("storetest: 失败 %d 项（good 没被收下，后面没法跑）\n", bad + 1);
            QDir(box).removeRecursively();
            return 1;
        }
        const Entry& good = *found;
        QString note;
        check("目录里什么都没有 = 能装", word(PluginStore::compare(good, &note, box)), "能装");
        QFile::copy(box + "/src-good.bin", box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("good")));
        check("哈希就是清单里那个 = 已装", word(PluginStore::compare(good, &note, box)), "已装");
        {
            // 这几颗放自己的子目录：后面那两段要数"这个目录里有几颗插件"，
            // 借来的真插件和假文件混进去会把它们的计数带跑（第一版就这样红了两条）。
            const QString vbox = box + "/versions";
            QDir().mkpath(vbox);
                        // 判"有没有新版"吃的是版本号，不是哈希 —— 哈希只说"不是同一颗"。
            // 这一族用例要真插件产物才量得出来（本机版本号是从嵌在 dll 里那份 plugin.json 读的，
            // 不装载），所以从本机插件目录借一颗 json。
            const QString realName = Platform::pluginFileName(QStringLiteral("json"));
            const QString realSrc = PluginHost::pluginsDir() + QLatin1Char('/') + realName;
            const QString localVer = PluginHost::metaVersionOf(realSrc);
            check("借来的那颗真插件读得出自己声明的版本号", localVer.isEmpty() ? QStringLiteral("空") : QStringLiteral("非空"),
                  QStringLiteral("非空"));
            QFile::remove(vbox + QLatin1Char('/') + realName);
            QFile::copy(realSrc, vbox + QLatin1Char('/') + realName);
            Entry v = good;
            v.file = realName;
            v.sha256 = QStringLiteral("0000000000000000000000000000000000000000000000000000000000000000");
            QString vnote;
            // a) 清单版本号比本机高 —— 这才叫可更新
            v.version = localVer + QStringLiteral(".1");
            check("清单版本更高才叫可更新", word(PluginStore::compare(v, &vnote, vbox)), QStringLiteral("可更新"));
            check("可更新那句把两边版本号都说出来",
                  vnote.contains(QStringLiteral("本机 v")) && vnote.contains(QStringLiteral("商店 v"))
                      ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
            // b) 版本号一样、哈希不一样 —— 内容不同不等于有新版（商店每次 CI 重编哈希都会变）
            v.version = localVer;
            check("版本号一样而哈希不同 = 与商店不同（不许报可更新）",
                  word(PluginStore::compare(v, &vnote, vbox)), QStringLiteral("与商店不同"));
            // c) 清单版本号更旧 —— 更不能叫可更新，而且要劝住别把本机这份盖掉
            v.version = QStringLiteral("0.0.1");
            check("清单版本更旧也不叫可更新", word(PluginStore::compare(v, &vnote, vbox)), QStringLiteral("与商店不同"));
            check("清单更旧那句要劝住降级", vnote.contains(QStringLiteral("别按")) ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // d) 有一侧读不到版本号 —— 说不清就不许当"有新版"卖，只说内容不同
            const QString fakeName = Platform::pluginFileName(QStringLiteral("noversion"));
            makeFile(vbox + QLatin1Char('/') + fakeName, QByteArray("不是插件产物，读不到版本号"));
            Entry f = good;
            f.file = fakeName;
            f.version = localVer;
            f.sha256 = QStringLiteral("1111111111111111111111111111111111111111111111111111111111111111");
            check("读不到本机版本号时也不叫可更新", word(PluginStore::compare(f, &vnote, vbox)), QStringLiteral("与商店不同"));

        }
        {
            // 先把"已装"那一步用的那颗文件撤掉：这一段两条判据分别要"本机没这颗"和"本机有这颗"，
            // 谁先谁后写死在这里，不然上一条会读到上一条留下的文件（第一版就因为这个红了：
            // 拿着"装着"的目录去问"没装着"那一条，got=[已装] want=[装不了]）。
            QFile::remove(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("good")));
            Entry old = good;
            old.api = 1;
            check("清单里的 Api 和宿主对不上、本机又没这颗 = 装不了",
                  word(PluginStore::compare(old, &note, box)), "装不了");
            check("装不了那条 = 界面不许给「装/更新/换成商店那份」",
                  PluginStore::installable(old) ? "yes" : "no", "no");
            // 原来这里钉的是"就算文件在，Api 不对也先拒" —— 那正是 10-20 他报
            // "注册表分析和开机分析还是点不开"的形状：本机明明装着、也起得来，
            // 卡片却因为**商店那一条**对不上号被锁成"装不了"，一颗按钮都不给、点正文没反应。
            // 现在两件事分开判：本机这颗能不能用看宿主认不认它，商店那条能不能装看 Api。
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("old2")), QByteArray("x"));
            Entry o2 = good;
            o2.file = Platform::pluginFileName(QStringLiteral("old2"));
            o2.api = 99;
            check("本机装着那颗时，商店那条 Api 不对不再锁死卡片（还是按内容比）",
                  word(PluginStore::compare(o2, &note, box)), QStringLiteral("与商店不同"));
            check("那句原因把两半说开：换不了商店那份 · 本机这颗照旧",
                  note.contains(QStringLiteral("换不了商店那份"))
                      && note.contains(QStringLiteral("本机这颗照旧"))
                      ? "yes" : "no", "yes");
            check("按钮该不该给问的是同一个谓词（这条装不了）",
                  PluginStore::installable(o2) ? "yes" : "no", "no");
            Entry o3 = good;
            o3.api = LTOOL_PLUGIN_API_VERSION;
            check("Api 对得上就装得了（按钮回来了）",
                  PluginStore::installable(o3) ? "yes" : "no", "yes");
        }

        // ---- 3. 落盘：先验哈希，再改名；半成品不许冒充 ----
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("new")) + QStringLiteral(".part"), QByteArray("GOOD-DLL-CONTENT"));
            Entry e = good;
            e.file = Platform::pluginFileName(QStringLiteral("new"));
            const QString err = PluginStore::finishDownload(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("new")) + QStringLiteral(".part"), e, box);
            check("哈希对了就落进目录（回报空串）", err.isEmpty() ? "ok" : err, "ok");
            check("落下来的文件哈希还是那个", PluginStore::sha256Of(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("new"))), goodSha);
            check("落下来之后 .part 不该留着", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("new")) + QStringLiteral(".part")) ? "yes" : "no", "no");
        }
        {
            // 「更新」那条路：目录里**已经有**一颗旧的。Windows 上 QFile::rename 不覆盖已存在的目标，
            // 少了"先拿掉旧的"这一步，12 颗可更新的全会撞在 rename 上（他点出来的就是这个）。
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd")), QByteArray("本机那份旧的 dll 内容"));
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd")) + QStringLiteral(".part"), QByteArray("GOOD-DLL-CONTENT"));
            Entry e = good;
            e.file = Platform::pluginFileName(QStringLiteral("upd"));
            const QString err = PluginStore::finishDownload(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd")) + QStringLiteral(".part"), e, box);
            check("更新能盖掉目录里已存在的那颗（回报空串）", err.isEmpty() ? "ok" : err, "ok");
            check("盖完之后目录里那颗就是清单那份", PluginStore::sha256Of(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd"))), goodSha);
            check("更新也不许留 .part", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd")) + QStringLiteral(".part")) ? "yes" : "no", "no");
            // 这一节后面还要数目录里有几颗插件，把这颗用完的拿掉（路径是算出来的绝对路径，不是相对）
            QFile::remove(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("upd")));
        }
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("badhash")) + QStringLiteral(".part"), QByteArray("被人动过的内容"));
            Entry e = good;
            e.file = Platform::pluginFileName(QStringLiteral("badhash"));
            const QString err = PluginStore::finishDownload(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("badhash")) + QStringLiteral(".part"), e, box);
            check("哈希不对必须报错", err.isEmpty() ? "ok" : "err", "err");
            check("报错里说清是哈希", err.contains(QStringLiteral("哈希")) ? "yes" : "no", "yes");
            check("哈希不对的那份要删掉，不许留下半成品",
                  QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("badhash")) + QStringLiteral(".part")) ? "yes" : "no", "no");
            check("没落进目录", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("badhash"))) ? "yes" : "no", "no");
        }
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("trunc")) + QStringLiteral(".part"), QByteArray("GOOD-DLL-CONTENT"));
            Entry e = good;
            e.file = Platform::pluginFileName(QStringLiteral("trunc"));
            e.bytes = 1 << 20;    // 清单说要 1MB，下到的是 16B —— 传输被截断的形状
            const QString err = PluginStore::finishDownload(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("trunc")) + QStringLiteral(".part"), e, box);
            check("字节数不符也拒", err.contains(QStringLiteral("字节数")) ? "yes" : "no", "yes");
            check("字节数不符的那份也删掉", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("trunc")) + QStringLiteral(".part")) ? "yes" : "no", "no");
        }

        // ---- 4. 禁用/恢复 = 在 <file> 和 <file>.off 之间改名 ----
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("off")), QByteArray("GOOD-DLL-CONTENT"));
            Entry e = good;
            e.file = Platform::pluginFileName(QStringLiteral("off"));
            check("禁用回报空串", [&] {
                const QString r = PluginStore::setEnabled(e.file, false, box);
                return r.isEmpty() ? QStringLiteral("ok") : r;
            }(), "ok");
            check("禁用之后 live 没了", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("off"))) ? "yes" : "no", "no");
            check("禁用之后 .off 在", QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("off")) + QStringLiteral(".off")) ? "yes" : "no", "yes");
            check("目录里那种状态认得出已禁用", word(PluginStore::compare(e, &note, box)), "已禁用");
            check("再禁一次不算失败", [&] {
                const QString r = PluginStore::setEnabled(e.file, false, box);
                return r.isEmpty() ? QStringLiteral("ok") : r;
            }(), "ok");
            check("恢复：.off 改回 live", [&] {
                const QString r = PluginStore::setEnabled(e.file, true, box);
                return (r.isEmpty() && QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("off")))
                        && !QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("off")) + QStringLiteral(".off")))
                          ? QStringLiteral("ok") : r;
            }(), "ok");
            check("目录里又认得出已装", word(PluginStore::compare(e, &note, box)), "已装");
            check("没装过的东西谈不上禁用",
                  PluginStore::setEnabled(Platform::pluginFileName(QStringLiteral("never")), false, box).isEmpty()
                      ? QStringLiteral("ok") : QStringLiteral("err"), "err");
        }

        // ---- 4a. 「目录里有什么」这一面（没拉清单时列表就是它） ----
        {
            // 到这儿为止目录里有三个 *.dll（new / off / old2，都是上面几段落的产物），
            // 再造一个改名成 .off 的，就能同时看到"活着"和"被禁用"两种。
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("extra")), QByteArray("要被禁的那个"));
            PluginStore::setEnabled(Platform::pluginFileName(QStringLiteral("extra")), false, box);
            const auto locals = PluginStore::localRows(box);
            QStringList who;
            int on = 0, off = 0;
            for (const PluginStore::Row& r : locals) {
                who << QStringLiteral("%1=%2").arg(r.remote.file, PluginStore::stateWord(r.state));
                if (r.state == PluginStore::State::Installed)
                    ++on;
                if (r.state == PluginStore::State::Disabled)
                    ++off;
            }
            // 目录里有 *.dll / *.dll.off / *.bin（bin 不算）、还有那个 .part 留下的别的文件也不算
            check("只把本机那个后缀和它的 .off 当插件（旁边那些 *.bin 不算）", QString::number(who.size()), "4");
            std::printf("INFO 目录里认出来的：%s\n", who.join(QStringLiteral("，")).toUtf8().constData());
            check("活着的报已装、改名 .off 的报已禁用",
                  QStringLiteral("%1/%2").arg(on).arg(off), QStringLiteral("3/1"));
            check("每一条的 note 都写着自己的文件名（不是空话）",
                  std::all_of(locals.begin(), locals.end(), [](const PluginStore::Row& r) {
                      return r.note.contains(r.remote.file);
                  })
                      ? "yes" : "no", "yes");
            check("活着的那几条还带着大小（一律 MB，不许裸字节）",
                  std::all_of(locals.begin(), locals.end(), [](const PluginStore::Row& r) {
                      return r.state != PluginStore::State::Installed
                             || (r.note.contains(QStringLiteral(" MB"))
                                 && !r.note.contains(QStringLiteral(" B")));
                  })
                      ? "yes" : "no", "yes");
        }

        // ---- 4b. 本机装着、清单里却没列的那几个 ----
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("stranger")), QByteArray("不在清单上的那个"));
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("good")), QByteArray("GOOD-DLL-CONTENT"));
            // `listed` 就是第 1 段解析出来的那两条（good 的文件名是 ltool-good.dll）：
            // 目录里那个文件既然被清单覆盖了，就不该再被报成"清单外"。
            QStringList names;
            int localOnly = 0;
            int off = 0;
            const auto collect = [&](const QVector<PluginStore::Row>& rs) {
                names.clear();
                localOnly = off = 0;
                for (const PluginStore::Row& r : rs) {
                    names << r.remote.file;
                    if (r.state == PluginStore::State::LocalOnly)
                        ++localOnly;
                    if (r.state == PluginStore::State::Disabled)
                        ++off;
                }
            };
            collect(PluginStore::localOnly(entries, box));
            check("清单外那一栏报出没被清单覆盖的文件",
                  names.contains(Platform::pluginFileName(QStringLiteral("stranger")))
                      && names.contains(Platform::pluginFileName(QStringLiteral("new"))) ? "yes" : "no", "yes");
            check("清单里已经列过的那个文件不再报成清单外",
                  names.contains(Platform::pluginFileName(QStringLiteral("good"))) ? "yes" : "no", "no");
            check("没禁用之前：清单外的条数 = 总条数减掉已禁用的那几条", QString::number(localOnly),
                  QString::number(names.size() - off));
            const QString err = PluginStore::setEnabled(Platform::pluginFileName(QStringLiteral("stranger")), false, box);
            collect(PluginStore::localOnly(entries, box));
            check("清单外那条也能禁用（禁完它还在栏里，已禁用的数比上一轮多一条）",
                  err.isEmpty() && names.contains(Platform::pluginFileName(QStringLiteral("stranger")))
                      && off == 2 && localOnly == names.size() - off ? "yes" : "no", "yes");
        }

        // ---- 4c. 卸载：删得掉就当场没了，删不动的记进待删表、下次启动删 ----
        {
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("doomed")), QByteArray("要被卸的那个"));
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("doomed")) + QStringLiteral(".off"), QByteArray("它禁用的那份也该一起走"));
            const QString res = PluginStore::uninstall(Platform::pluginFileName(QStringLiteral("doomed")), box);
            check("卸载把 live 和 .off 一起删掉（回报空串）",
                  res.isEmpty() && !QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("doomed")))
                      && !QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("doomed")) + QStringLiteral(".off"))
                      ? QStringLiteral("yes") : QStringLiteral("no"), "yes");
            const QString again = PluginStore::uninstall(Platform::pluginFileName(QStringLiteral("doomed")), box);
            check("再卸一次要说「没装过」，不是假装又删了一次",
                  again.contains(QStringLiteral("没装过")) ? QStringLiteral("yes") : QStringLiteral("no"), "yes");
            check("卸完不该在待删表里留账", QString::number(PluginStore::pendingRemoves().size()), "0");

            // 待删表这条真实路径：表里排着的文件名，下次启动由 applyPendingRemoves 落地。
            // （"这个进程还加载着它、删不动"那个分支要真加载过的 dll 才进得去，离线进不去 ——
            //  所以这里验的是账记上了、能删的删掉了、账也销了，不是假装验过 busy。）
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("pending")), QByteArray("排队等删的"));
            {
                QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
                QStringList pend = cfg.value(QStringLiteral("store/pendingRemove")).toStringList();
                pend << Platform::pluginFileName(QStringLiteral("pending"));
                cfg.setValue(QStringLiteral("store/pendingRemove"), pend);
                cfg.sync();
            }
            check("待删表读得出来", QString::number(PluginStore::pendingRemoves().size()), "1");
            // 排过队的那颗必须在自己卡片上说出来：文件还在、进程还加载着，点第二下也不会有
            // 别的解释。以前这句只写给那张已经要被重建掉的标签看，界面上就是"点了没反应"。
            makeFile(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("notpending")), QByteArray("没排队的对照"));
            const auto noteOf = [box](const QString& file) {
                for (const PluginStore::Row& r : PluginStore::localRows(box))
                    if (r.remote.file == file)
                        return r.note;
                return QStringLiteral("<没有这一行>");
            };
            check("排过卸载队的那颗，状态里写明「下次启动真删掉」",
                  noteOf(Platform::pluginFileName(QStringLiteral("pending"))).contains(
                      QStringLiteral("下次启动真删掉")) ? QStringLiteral("yes") : QStringLiteral("no"),
                  "yes");
            check("没排队的那颗不许也被说成排队中",
                  noteOf(Platform::pluginFileName(QStringLiteral("notpending"))).contains(
                      QStringLiteral("下次启动真删掉")) ? QStringLiteral("yes") : QStringLiteral("no"),
                  "no");
            check("对照那颗自己得在列表里（不然上一条是空列表假绿）",
                  noteOf(Platform::pluginFileName(QStringLiteral("notpending"))) == QStringLiteral("<没有这一行>")
                      ? QStringLiteral("no") : QStringLiteral("yes"),
                  "yes");
            const int gone = PluginStore::applyPendingRemoves(box);
            check("下次启动那一步真把排队的删掉了",
                  gone == 1 && !QFile::exists(box + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("pending")))
                      ? QStringLiteral("yes") : QStringLiteral("no"), "yes");
            check("删完账要销掉，不许留下一条删过的", QString::number(PluginStore::pendingRemoves().size()), "0");
            const int again2 = PluginStore::applyPendingRemoves(box);
            check("表是空的时候这一步不动任何东西", QString::number(again2), "0");
        }

        // ---- 4d. 「上哪儿拉」这条顺序：镜像（直连）→ 原件（代理）→ 原件（直连） ----
        {
            const QString raw = QStringLiteral(
                "https://raw.githubusercontent.com/mateo-ki/ltool_plugin/main/dist/index.json");
            const auto a = PluginStore::attemptsFor(raw);
            QStringList shape;
            for (const PluginStore::Attempt& at : a)
                shape << QStringLiteral("%1:%2").arg(at.label,
                                                     at.useProxy ? QStringLiteral("代理")
                                                                 : QStringLiteral("直连"));
            check("github raw 那条排出四个候选（两镜像 + 代理 + 直连）", QString::number(a.size()), "4");
            check("顺序是镜像→代理→直连，而且镜像那两条走直连",
                  shape.join(QStringLiteral(" | ")),
                  QStringLiteral("镜像 gh-proxy.com:直连 | 镜像 ghfast.top:直连 | 本地代理:代理 | 直连:直连"));
            check("每个候选都是前缀套原件（镜像不改路径，只加一层）",
                  std::all_of(a.begin(), a.end(), [&](const PluginStore::Attempt& at) {
                      return at.url.endsWith(raw);
                  })
                      ? "yes" : "no", "yes");
            // 用户把地址指到自己的服务器时套镜像，只会拿回 404 让他以为自己的源坏了
            const auto own = PluginStore::attemptsFor(QStringLiteral("https://example.invalid/index.json"));
            check("不是 github raw 就只剩代理和直连两条", QString::number(own.size()), "2");
            // 1.3.0 起商店的 dll 和清单都是 Release 资产（github.com/<仓库>/releases/download/…），
            // 这条线路比 raw 更容易整条挂死，所以它必须也吃到那两条镜像 —— 少排一条就是
            // "商店在国内装不上插件"，而现场只会看到一句"下载失败"。
            const QString asset = QStringLiteral(
                "https://github.com/mateo-ki/ltool/releases/download/v1.3.0/ltool-disk.dll");
            const auto ad = PluginStore::attemptsFor(asset);
            check("Release 资产那条也排出四个候选（两镜像 + 代理 + 直连）",
                  QString::number(ad.size()), "4");
            check("资产地址套镜像只加前缀，路径一个字不动",
                  ad.size() == 4 && ad[0].url.endsWith(asset) && ad[1].url.endsWith(asset)
                      ? "yes" : "no", "yes");
            // 反过来也得钉住：github.com 上**不是** releases/download 的路径（比如 /releases/latest
            // 那种网页地址）不该被套上镜像 —— 判据只认 host 就会把网页地址也代理掉。
            check("github.com 上不是发版资产的路径不套镜像",
                  QString::number(PluginStore::attemptsFor(
                      QStringLiteral("https://github.com/mateo-ki/ltool/releases/latest")).size()), "2");
            {
                QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
                cfg.setValue(QStringLiteral("store/mirrors"),
                             QStringLiteral("https://mirror-a.example,https://mirror-b.example/"));
                cfg.sync();
                const auto mine = PluginStore::attemptsFor(raw);
                check("配置里换镜像就用换的那两条（少写的尾斜杠补上）",
                      mine.size() == 4
                          && mine[0].url == QStringLiteral("https://mirror-a.example/") + raw
                          && mine[1].url == QStringLiteral("https://mirror-b.example/") + raw
                              ? "yes" : QStringLiteral("no"), "yes");
                cfg.setValue(QStringLiteral("store/mirrors"), QStringLiteral("   "));
                cfg.sync();
                check("清空等于回到内置那两条", PluginStore::mirrorList(),
                      QStringLiteral("https://gh-proxy.com/,https://ghfast.top/"));
            }
        }

        // ---- 5. 索引地址：留空用内置那条，配了就用配的 ----
        {
            // 内置那条从 1.3.0 起是**这次发版配套的商店**（清单和 19 颗 dll 由主仓库同一次
            // 构建发成 Release 资产），不再是插件仓库 main 上手工提交的那份 dist/ ——
            // 那个目录最后一次 10-16，而 core 之后一直没停，磁盘清理就是被它对崩的（#91）。
            const QString want = QStringLiteral("https://github.com/mateo-ki/ltool/releases/download/v%1/")
                                 .arg(AboutPage::currentVersion());
            check("内置地址指向我自己这一版配套的清单（标签 = v + 版本号）",
                  PluginStore::defaultIndexUrl().startsWith(want)
                      && PluginStore::defaultIndexUrl().endsWith(QStringLiteral("plugins-index.json"))
                          ? "yes" : "no", "yes");
            check("这条地址要能吃到那两条镜像（github.com 直连间歇挂死是常态）",
                  QString::number(PluginStore::attemptsFor(PluginStore::defaultIndexUrl()).size()), "4");
            QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
            check("没配过就用内置那条", PluginStore::indexUrl() == PluginStore::defaultIndexUrl()
                      ? "yes" : "no", "yes");
            cfg.setValue(QStringLiteral("store/indexUrl"), QStringLiteral("https://example.invalid/index.json"));
            cfg.sync();
            check("配了就用配的", PluginStore::indexUrl(), "https://example.invalid/index.json");
            cfg.setValue(QStringLiteral("store/indexUrl"), QStringLiteral("   "));
            cfg.sync();
            check("清空等于回到内置", PluginStore::indexUrl() == PluginStore::defaultIndexUrl()
                      ? "yes" : "no", "yes");
        }

        // ---- 6. 本地持久化缓存与每日拉取检测 ----
        {
            const QByteArray fakeIndex = "{\"schema\":1,\"plugins\":[]}";
            check("写入清单缓存", PluginStore::saveCachedIndex(fakeIndex) ? "yes" : "no", "yes");
            check("读回清单缓存", QString::fromUtf8(PluginStore::loadCachedIndex()), QString::fromUtf8(fakeIndex));
            PluginStore::markFetched();
            check("打上标记后今天不重复自动拉取", PluginStore::shouldAutoRefresh() ? "yes" : "no", "no");
        }
        // ---- 7. 禁用插件检测 ----
        {
            check("不存在的插件不报禁用", PluginHost::isDisabled(QStringLiteral("nonexistent_plug")) ? "yes" : "no", "no");
        }

        // ---- 「本机装着这个 dll 但它起不来」那行卡片文案的**拼法** ----
        // 为什么测拼法而不是测卡片：这一支只有装载真失败过才走得到，判据造不出一颗坏 dll
        //（造出来就得往真插件目录里写东西）。搬进 --storetest 是因为 --storenet 要联网、
        // 不进 CI，放那儿等于没人跑 —— 第一次我就把它插进 storenet 了，73 条一条没多。
        // 2026-10-20 他问"注册表分析和开机分析为什么点不开"，那两张卡上写的是
        // "已安装 · 本机有这个 dll 但没通过检查：" —— 冒号后面是空的，因为老写法在
        // `meta` 存在那一支三元取的是 `QString()`。第三条钉的就是"结尾不许是冒号"。
        {
            const QString head = QStringLiteral("已安装 · 本机有这个 dll 但它起不来：");
            const QString real = ToolsPage::cannotOpenText(
                QStringLiteral("已安装"), QStringLiteral("缺少导入函数 Utils::revealInExplorer"),
                QString());
            check("有装载原因时那句话把原因原样带出来",
                  real == head + QStringLiteral("缺少导入函数 Utils::revealInExplorer")
                      ? "yes" : "no", "yes");
            const QString pri = ToolsPage::cannotOpenText(QStringLiteral("已安装"),
                                                          QStringLiteral("装载失败 A"),
                                                          QStringLiteral("元数据失败 B"));
            check("装载原因和元数据原因都在时说装载那个（他看见的症状是点不开）",
                  pri == head + QStringLiteral("装载失败 A") ? "yes" : "no", "yes");
            const QString none = ToolsPage::cannotOpenText(QStringLiteral("已安装"), QString(), QString());
            check("三个来源全空也必须落到一句有人话的兜底，不许留一个以冒号结尾的空原因",
                  none == head + QStringLiteral("目录里认不出这个文件") ? "yes" : "no", "yes");
        }

        // ---- 8. 每天一次的插件自动更新（2026-10-21 他点名：「一天自动更新一次，如果有新插件」）----
        // 联网那一段在这儿测不了（这面旗标一次网都不上），所以钉的是**决策**：
        // 谁进这一趟、今天该不该跑、跑完那句话长什么样、界面那一行说不出假话。
        // 「新插件不许自动装」这条边界必须由判据守着 —— 它一旦只写在 fetch 的回调里就没人测得到。
        {
            QSettings cfg(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat);
            for (const QString& k : {QStringLiteral("store/autoUpdate"), QStringLiteral("store/autoUpdateDate"),
                                     QStringLiteral("store/autoUpdateNote"), QStringLiteral("store/autoUpdateAt")})
                cfg.remove(k);
            cfg.sync();

            check("自动更新默认是开的（他这句是「自动做 X」，不是给我个按钮我按了才做）",
                  PluginStore::autoUpdateEnabled() ? "yes" : "no", "yes");
            cfg.setValue(QStringLiteral("store/autoUpdate"), false);
            cfg.sync();
            check("配置页关掉之后谓词跟着关（不是只多写了一行没人读的配置）",
                  PluginStore::autoUpdateEnabled() ? "yes" : "no", "no");
            cfg.setValue(QStringLiteral("store/autoUpdate"), true);
            cfg.sync();

            const QString today = QDate::currentDate().toString(Qt::ISODate);
            cfg.setValue(QStringLiteral("store/autoUpdateDate"), today);
            cfg.sync();
            check("今天已经跑过 => 这一趟不再跑第二次", PluginStore::autoUpdateDue() ? "yes" : "no", "no");
            cfg.setValue(QStringLiteral("store/autoUpdateDate"), QStringLiteral("2020-01-01"));
            cfg.sync();
            check("日期是过去 => 今天这一趟该跑", PluginStore::autoUpdateDue() ? "yes" : "no", "yes");
            cfg.remove(QStringLiteral("store/autoUpdateDate"));
            cfg.sync();
            check("从来没跑过也算该跑（刚装好的机器第一次启动就该补一趟）",
                  PluginStore::autoUpdateDue() ? "yes" : "no", "yes");

            auto rowOf = [](const QString& id, PluginStore::State st, bool usable = true) {
                PluginStore::Row r;
                r.remote.id = id;
                r.remote.name = id;
                r.remote.file = Platform::pluginFileName(id);
                r.state = st;
                r.entryUsable = usable;
                return r;
            };
            const QVector<PluginStore::Row> all {
                rowOf(QStringLiteral("upd1"), PluginStore::State::Update),
                rowOf(QStringLiteral("brandnew"), PluginStore::State::Missing),  // 清单里有、本机没装 = 新插件
                rowOf(QStringLiteral("same"), PluginStore::State::Installed),
                rowOf(QStringLiteral("off"), PluginStore::State::Disabled),      // 他亲手禁用的，不许顺手启用回来
                rowOf(QStringLiteral("ours"), PluginStore::State::Different),    // 哈希不同但版本不比本机旧
                rowOf(QStringLiteral("upd2"), PluginStore::State::Update, false) // 有新版但 Api 对不上 = 装不了
            };
            QStringList picked;
            for (const PluginStore::Entry& e : PluginStore::autoUpdateTargets(all))
                picked << e.id;
            check("这一趟只换「已装、而清单那条更新、而且这台机器装得了」的那一档",
                  picked.join(QLatin1Char(',')), QStringLiteral("upd1"));
            {
                QStringList onlyNew;
                for (const PluginStore::Entry& e : PluginStore::autoUpdateTargets(
                         { rowOf(QStringLiteral("brandnew"), PluginStore::State::Missing),
                           rowOf(QStringLiteral("api4"), PluginStore::State::ApiMismatch) }))
                    onlyNew << e.id;
                check("满墙的新插件也不会被自动装（他拍的是「只更新已装的」）",
                      onlyNew.isEmpty() ? "yes" : "no", "yes");
            }

            const QStringList two { QStringLiteral("甲"), QStringLiteral("乙") };
            const QString okNote = PluginStore::autoUpdateSummary(two, {}, QStringLiteral("清单 19 条"));
            check("成了两颗：那句话点名几颗、是哪几颗",
                  okNote.contains(QStringLiteral("自动更新了 2 颗")) && okNote.contains(QStringLiteral("甲、乙"))
                      ? "yes" : "no", "yes");
            const QString mix = PluginStore::autoUpdateSummary(
                { QStringLiteral("甲") }, { QStringLiteral("乙 没下完：超时") }, QStringLiteral("清单 19 条"));
            check("有没成的：点名是哪一颗、为什么没成（不许只报「更新完成」）",
                  mix.contains(QStringLiteral("1 颗没成")) && mix.contains(QStringLiteral("乙 没下完：超时"))
                      ? "yes" : "no", "yes");
            const QString idle = PluginStore::autoUpdateSummary({}, {}, QStringLiteral("清单 19 条"));
            check("一颗都没动：说「没有要换的」，不许写成「更新完成」（那是说他动过文件）",
                  idle.contains(QStringLiteral("没有要换的")) ? "yes" : "no", "yes");
            check("真换了东西的那句才说「已落盘」；一颗都没动的那句不许说（那是假话）",
                  okNote.contains(QStringLiteral("已落盘")) && mix.contains(QStringLiteral("已落盘"))
                      && !idle.contains(QStringLiteral("已落盘")) ? "yes" : "no", "yes");
            check("三种说法都把「清单几条」带上；哪条链拿到的（URL/镜像/试了几次）在 why 那份里",
                  okNote.contains(QStringLiteral("清单 19 条")) && mix.contains(QStringLiteral("清单 19 条"))
                      && idle.contains(QStringLiteral("清单 19 条")) ? "yes" : "no", "yes");
            // 页面上那句短话 / 悬停说明里那份全文，是 2026-10-23 他拍的一刀：
            // 「不要放到插件详情页面」+ 选的是「页上留一句短话，全文进日志」。
            // 这三条量的是 autoBrief 那颗谓词（读老配置时也走它），不是"我有没有记得写短的"。
            check("短话那半句不许带 URL / HTTP 码（老格式那条整条原文带 http 的也一样）",
                  !PluginStore::autoBrief(QStringLiteral(
                      "没有要换的（网络没成（Error transferring https://x/y - 404）），拿本地缓存试"))
                       .contains(QStringLiteral("http"))
                      ? "yes" : "no", "yes");
            check("干净的 note 原样过，不许多切一刀（切了就等于这条在改一句本来没毛病的话）",
                  PluginStore::autoBrief(QStringLiteral("没有要换的（清单 19 条 · 已落盘）")),
                  QStringLiteral("没有要换的（清单 19 条 · 已落盘）"));
            check("老格式那条没有括号的失败句：宁可退成一句固定短话，也不把原文推上页面",
                  PluginStore::autoBrief(QStringLiteral(
                      "今天没跑成：Error transferring https://x/y - server replied 404，本地也没有缓存清单"))
                      .contains(QStringLiteral("原文在悬停说明里"))
                      ? "yes" : "no", "yes");
            check("被切掉的原文一个字都不丢：没写 why 时，悬停说明拿回那条老 note",
                  (cfg.setValue(QStringLiteral("store/autoUpdateNote"),
                                QStringLiteral("没有要换的（网络没成（HTTP 0，走的是 https://x/y））")),
                   cfg.sync(),
                   ToolsPage::autoUpdateLine().contains(QStringLiteral("没有要换的"))
                       && !ToolsPage::autoUpdateLine().contains(QStringLiteral("http"))
                       && PluginStore::autoUpdateWhy().contains(QStringLiteral("HTTP 0")))
                      ? "yes" : "no", "yes");
            check("写了 why 之后，note 是短的、why 是那条链（两份各管各的，不是一句拆两半）",
                  (cfg.setValue(QStringLiteral("store/autoUpdateNote"), QStringLiteral("清单 19 条")),
                   cfg.setValue(QStringLiteral("store/autoUpdateWhy"),
                                QStringLiteral("清单 19 条，走的是 gh-proxy.com，一共试了 2 次")),
                   cfg.sync(),
                   PluginStore::autoUpdateNote().endsWith(QStringLiteral("清单 19 条"))
                       && PluginStore::autoUpdateWhy().contains(QStringLiteral("gh-proxy.com")))
                      ? "yes" : "no", "yes");

            // 上面那两条各写了一次 note/why，这一条问的是"从来没跑过"—— 不清场就是读上一句留下的值
            // （第一次跑就是它把这条判红的，那条红是真的）。
            cfg.remove(QStringLiteral("store/autoUpdateNote"));
            cfg.remove(QStringLiteral("store/autoUpdateWhy"));
            cfg.sync();
            check("没跑过时商店那一行说「今天这一趟还没跑」，不许留一行空白",
                  ToolsPage::autoUpdateLine().contains(QStringLiteral("还没跑")) ? "yes" : "no", "yes");
            cfg.setValue(QStringLiteral("store/autoUpdateNote"),
                         QStringLiteral("自动更新了 1 颗：剪贴板（清单 19 条 · 已落盘，下次启动用新的）"));
            cfg.setValue(QStringLiteral("store/autoUpdateAt"),
                         QDateTime::currentDateTime().toString(Qt::ISODate));
            cfg.sync();
            check("写过结果之后那一行把结果带出来（而且带时间，不是只说「更新过」）",
                  ToolsPage::autoUpdateLine().contains(QStringLiteral("自动更新了 1 颗"))
                      && PluginStore::autoUpdateNote().contains(QStringLiteral("-")) ? "yes" : "no", "yes");
            cfg.setValue(QStringLiteral("store/autoUpdate"), false);
            cfg.sync();
            check("关掉开关：那一行要说「没开」并指路配置页（关着的样子不能像坏了）",
                  ToolsPage::autoUpdateLine().contains(QStringLiteral("没开")) ? "yes" : "no", "yes");
            // 收尾：清掉这一节留下的状态。留着的话，下次有人在这后面加判据会读到"上一查询留下的值"。
            for (const QString& k : {QStringLiteral("store/autoUpdate"), QStringLiteral("store/autoUpdateDate"),
                                     QStringLiteral("store/autoUpdateNote"),
                                     QStringLiteral("store/autoUpdateWhy"),
                                     QStringLiteral("store/autoUpdateAt")})
                cfg.remove(k);
            cfg.sync();
        }

        // ---- 9. 搬出来的那颗 installEntry：一趟装完/装挂之后，"在飞"那颗标记有没有放掉 ----
        // 为什么在这儿测：`fetch` 的所有连接都挂在 parent 上（页拆了下载就该停），
        // 于是 **done 永远不会回**。标记要是只在 done 里放，那颗 dll 就从此永远卡在
        // "已经有一趟在飞"，界面每次点它都是这句 —— 2026-10-21 把它从 ToolsPage 搬进 core 时
        // 正是这个形状：以前那个计数是页面成员（跟着页一起没了），现在是进程级的，不放就是永久。
        // 空 url 那一条不会碰网络：地址是空的，Qt 当场给一条错误 reply。
        {
            const QString leakFile = Platform::pluginFileName(QStringLiteral("leak"));
            PluginStore::Entry bogus;
            bogus.id = QStringLiteral("leak");
            bogus.name = QStringLiteral("泄漏夹具");
            bogus.file = leakFile;
            bogus.sha256 = QStringLiteral("00");
            int doneCalls = 0;
            const auto wait = [leakFile] {
                for (int i = 0; i < 300; ++i) {
                    QCoreApplication::processEvents();
                    if (!PluginStore::installInFlight(leakFile))
                        return;
                    QThread::msleep(10);
                }
            };
            {   // ① 先证明"在飞"这件事真的被量到了：起一趟，立刻看标记
                auto* page = new QObject;
                PluginStore::installEntry(bogus, page, {}, [&doneCalls](const PluginStore::InstallResult&) { ++doneCalls; });
                check("起了一趟之后这颗确实挂在「在飞」上（判据量的是被测代码真造的那趟）",
                      PluginStore::installInFlight(leakFile) ? "yes" : "no", "yes");
                delete page;   // 商店页在下载中途被拆掉：回调永远不会回
                check("页没了 ⇒ 标记必须跟着放掉（不放=这颗 dll 从此永远「已经有一趟在飞」）",
                      PluginStore::installInFlight(leakFile) ? "yes" : "no", "no");
            }
            {   // ② 50 趟正常失败：每趟都要回话、标记都要放掉、私有字节不许线性涨
                const int wsBefore = Platform::workingSetMB();
                auto* owner = new QObject;
                for (int i = 0; i < 200; ++i) {
                    PluginStore::installEntry(bogus, owner, {}, [&doneCalls](const PluginStore::InstallResult&) { ++doneCalls; });
                    wait();
                }
                const int wsAfter = Platform::workingSetMB();
                delete owner;
                std::printf("INFO installEntry 连跑 200 趟：回话 %d 次 · 工作集 %d → %d MB（涨 %d）\n",
                            doneCalls, wsBefore, wsAfter, wsAfter - wsBefore);
                // ① 那一趟**不该**回话（页被拆了，连接跟着没了），所以这里数的就是 ② 那 50 趟
                check("200 趟每趟都回了话（一次都不许静默：静默=标记和队列一起卡死）",
                      QString::number(doneCalls), "200");
                check("跑完 200 趟之后没有残留的「在飞」标记",
                      PluginStore::installInFlight(leakFile) ? "yes" : "no", "no");
                check("200 趟之后工作集涨得不多（≤8 MB：涨的是分配器留页，线性涨才是泄漏）",
                      wsAfter - wsBefore <= 8 ? "yes" : "no", "yes");
            }
        }

        // 全程没碰真目录：商店判的是传进去的那个 dir
        check("没把假 dll 写进真正的插件目录",
              QFile::exists(PluginHost::pluginsDir() + QLatin1Char('/') + Platform::pluginFileName(QStringLiteral("new"))) ? "yes" : "no", "no");
        QDir(box).removeRecursively();
        std::printf("storetest: 失败 %d 项\n", bad);
        std::fflush(stdout);
        return bad == 0 ? 0 : 1;
    }


    // --pintool：离屏验置顶窗那两个"收起"开关。判据本体在 plugins/capture。
    if (args.contains("--pintool"))
        return runPluginSelftest(QStringLiteral("capture"), "ltoolPintoolRun",
                                 SelftestArg::None, 0, QString(), "pintool");


    // --pindel <图片>：判据本体在 plugins/capture（符号 ltoolPindelRun）。
    if (args.contains("--pindel")) {
        const QString spec = args.mid(args.indexOf("--pindel") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolPindelRun",
                                 SelftestArg::Text, 0, spec, "pindel");
    }


    // --ocrget：下载并校验 OCR 引擎包。判据本体在 plugins/capture。
    if (args.contains("--ocrget"))
        return runPluginSelftest(QStringLiteral("capture"), "ltoolOcrgetRun",
                                 SelftestArg::None, 0, QString(), "ocrget");


    // --recprobe [观察毫秒]：录屏之前先量机器（有没有 H.264/AAC 编码器、MP4 写得出吗、
    // Win11 的按窗口取帧能不能用、一帧拷到 CPU 要多久）。会弹一个自己的小窗动 1 秒多，
    // 不碰用户其它窗口。RecProbe.cpp 现在也在 plugins/record 里编，符号叫 ltoolRecprobeRun。
    if (args.contains("--recprobe")) {
        const QString arg = args.mid(args.indexOf("--recprobe") + 1).value(0);
        const int observeMs = arg.isEmpty() ? 1500 : arg.toInt();
        return runPluginSelftest(QStringLiteral("record"), "ltoolRecprobeRun",
                                 SelftestArg::Ms, observeMs, QString(), "recprobe");
    }


    // --recq：录屏那两个新选择（画面大小 / 画质）到底进没进编码器。
    // 全部拿真编码器量：录自制卡 → 把 mp4 解回来说话。目标窗口是自己造的、
    // 摆在屏幕右下角、不抢焦点，不碰用户桌面上任何东西。判据本体在 plugins/record。
    if (args.contains("--recq"))
        return runPluginSelftest(QStringLiteral("record"), "ltoolRecqSelftest",
                                 SelftestArg::None, 0, QString(), "recq");

    // --rectest [秒]：验一遍"只录某个窗口 → mp4 → 再读回来数帧"，外加第二路"录整块屏幕"。
    // 目标窗口是自己造的一张每 30ms 换一个数字的卡片，不碰用户桌面上任何东西；
    // 判据不是"文件存在"，而是拿 SourceReader 把文件解回来数出 N 帧、且 N 和采集到的帧数一致。
    // 本体在 plugins/record/recordplugin.cpp（Recorder 就在那儿编，测试跟着实现走）。
    // 可选的「宽x高」用来钉"奇数尺寸被 H.264 拒"那一条 —— 导出符号收 (int, const char*)，
    // 解析在插件里那一个 cardSize()（--memloop 的画幅参数走同一条）。
    if (args.contains("--rectest")) {
        const QString a = args.mid(args.indexOf("--rectest") + 1).value(0);
        const int secs = a.isEmpty() ? 4 : qBound(2, a.toInt(), 30);
        const QString sizeSpec = args.mid(args.indexOf("--rectest") + 2).value(0);
        return runPluginSelftest(QStringLiteral("record"), "ltoolRectestRun",
                                 SelftestArg::MsAndText, secs, sizeSpec, "rectest");
    }

    // --recordui [秒]：验 F2 那套界面的链条（选窗口的列表有没有内容、悬浮条会不会计数、
    // 点了停止之后文件能不能解回帧）。按钮是用 click() 程序化按的，不合成鼠标，不抢焦点。
    // 2026-10-15：判据跟着 RecordUi 一起搬进 plugins/record（那边导出的符号就叫
    // ltoolRecorduiRun），主程序只剩"读命令行 + 点名要符号"这一步。
    if (args.contains("--recordui")) {
        const QString a = args.mid(args.indexOf("--recordui") + 1).value(0);
        const int secs = a.isEmpty() ? 3 : qBound(2, a.toInt(), 20);
        return runPluginSelftest(QStringLiteral("record"), "ltoolRecorduiRun",
                                 SelftestArg::Ms, secs, QString(), "recordui");
    }

    // --ocrpanel <图片>：判据本体在 plugins/capture（符号 ltoolOcrpanelRun）。
    if (args.contains("--ocrpanel")) {
        const QString spec = args.mid(args.indexOf("--ocrpanel") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolOcrpanelRun",
                                 SelftestArg::Text, 0, spec, "ocrpanel");
    }


    // --pinshot <图片>：判据本体在 plugins/capture（符号 ltoolPinshotRun）。
    if (args.contains("--pinshot")) {
        const QString spec = args.mid(args.indexOf("--pinshot") + 1).value(0);
        return runPluginSelftest(QStringLiteral("capture"), "ltoolPinshotRun",
                                 SelftestArg::Text, 0, spec, "pinshot");
    }


    // --shredtest <路径> yes：不弹界面直接跑一遍粉碎，用来验证覆写真的发生。
    // 必须带 "yes"，否则只报告目标信息不动手。
    if (args.contains("--shredtest")) {
        const QStringList rest = args.mid(args.indexOf("--shredtest") + 1);
        const QString target = rest.value(0);
        if (rest.value(1) != QLatin1String("yes")) {
            std::printf("shredtest: 没带 yes，只报告不动手 target=%s exists=%d\n",
                        target.toUtf8().constData(), QFileInfo(target).exists() ? 1 : 0);
            std::fflush(stdout);
            return 0;
        }
        QString err;
        const qint64 erased = FileOps::shred(target, rest.value(2) == QLatin1String("deep"), &err);
        std::printf("shredtest: erased=%lld exists_after=%d err=%s\n", erased,
                    QFileInfo(target).exists() ? 1 : 0, err.toUtf8().constData());
        std::fflush(stdout);
        return erased < 0 ? 1 : 0;
    }

    // 资源管理器右键菜单进来的动作：这是另起的一个进程，得先于单实例守卫处理，
    // 否则它只会把主窗口叫醒然后自己退出。
    //
    // 2026-10-15：窗口本体搬进 plugins/shell 了（用户拍的板：入口留在主程序，按下之后转发）。
    // 这儿只剩"旗标 → 动作名"这一手，主程序不再链接占用分析/粉碎/解压/压缩那些界面。
    // **转不出去必须把话讲明白并给非 0 退出码** —— 右键点一下什么都不发生是最难查的形状。
    for (const char* f : kShellFlags) {
        if (!args.contains(QLatin1String(f)))
            continue;
        const QString path = args.mid(args.indexOf(QLatin1String(f)) + 1).value(0);
        const QString action = QString::fromLatin1(f).mid(2);      // 去掉那两个短横
        QString who;
        if (PluginHost::invoke(action, { path }, &who))
            return 0;
        const QString why = who.isEmpty()
            ? PluginHost::whyNoOwner(action)
            : QStringLiteral("%1 接了但没干成").arg(who);
        std::printf("FAIL 右键动作 %s：%s 路径=%s\n", action.toUtf8().constData(),
                    why.toUtf8().constData(), path.toUtf8().constData());
        std::fflush(stdout);
        Utils::log(QStringLiteral("shellmenu: %1 没人接：%2").arg(action, why));
        QMessageBox::warning(nullptr, QStringLiteral("ltool"),
                             QStringLiteral("这个右键动作做不了：%1").arg(why));
        return 1;
    }

    // 单实例：第二个实例唤醒已运行的窗口后退出
    //
    // 但自测判据不能被单实例守卫转交给常驻实例（否则会变成"0 字节输出 + 退出码 0"的假绿形状）。
    // 所有带 '--' 命令行旗标的调用（包括在事件循环内跑的 --capgeo/--capflash/--toggle/--settingspage 等），
    // 均直接放行，不走单实例唤醒逻辑，避免后台常驻 ltool 时自测被静默吞掉。
    const bool inLoopSelftest = std::any_of(args.cbegin() + 1, args.cend(), [](const QString& a) {
        return a.startsWith(QLatin1String("--"));
    });
    if (!inLoopSelftest) {
        QLocalSocket probe;
        probe.connectToServer(QString::fromWCharArray(kInstanceKey));
        if (probe.waitForConnected(300)) {
            probe.write("show\n");
            probe.waitForBytesWritten(300);
            probe.disconnectFromServer();
            return 0;
        }
    }

    SearchWindow win;

    QLocalServer::removeServer(QString::fromWCharArray(kInstanceKey));
    QLocalServer server;
    server.listen(QString::fromWCharArray(kInstanceKey));
    QSystemTrayIcon* trayPtr = nullptr; // 稍后指向托盘，供热键提示和"递过来的气泡"使用
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        while (server.hasPendingConnections()) {
            QLocalSocket* c = server.nextPendingConnection();
            c->waitForReadyRead(100);
            const QByteArray msg = c->readAll();
            c->disconnectFromServer();
            c->deleteLater();
            // **空的就当没这回事**：只连上、没写东西的连接太多了（判据探"这台机器上有没有常驻实例"、
            // 半开的连接、被杀掉的发送方），把它们当成「把主窗口叫出来」= 别人一探测就抢他的屏幕。
            // 真要叫窗口的那条路径自己会写 "show\n"（本机实测：判据那条探测真把他那份启动器叫出来了，
            // 日志里留下 `show: 呼出（要求者=第二个实例转交过来的 show）`）。
            if (msg.trimmed().isEmpty())
                continue;
            // 右键解压/压缩那个一次性进程把气泡递过来（"note\t标题\t正文"），
            // 它自己不该为了举着气泡多活 5 秒。其余任何消息都还是"把主窗口叫出来"。
            const QList<QByteArray> parts = msg.split('\t');
            if (parts.value(0) == "note" && parts.size() >= 3) {
                if (trayPtr)
                    trayPtr->showMessage(QString::fromUtf8(parts.value(1).trimmed()),
                                         QString::fromUtf8(parts.value(2).trimmed()),
                                         QSystemTrayIcon::Information, 5000);
                else
                    Utils::log("note 到了但托盘还没建好，这条提示丢了");
                continue;
            }
            win.showLauncher("第二个实例转交过来的 show");
        }
    });

    // 热键背后那个插件动作没人接的时候怎么说：日志（事后能查）+ 托盘气泡（当下能看见）+
    // stderr（从命令行起着的时候能看见），**三层都说、绝不弹模态框** ——
    // 那会盖在用户正做的事情上面（2026-10-15 右键那一路就是这么被他撞上的）。
    // who 空 = 根本没插件声明接这个动作；非空 = 它接了但没干成（那一份插件自己会说明为什么）。
    const auto routeHotkey = [&](const QString& keyLabel, const QString& action) {
        QString who;
        if (PluginHost::invoke(action, {}, &who))
            return;
        const QString why = who.isEmpty()
            ? PluginHost::whyNoOwner(action)
            : QStringLiteral("%1 接了但没干成").arg(who);
        Utils::log(QStringLiteral("%1 没人接：%2").arg(keyLabel, why));
        std::fprintf(stderr, "%s: %s\n", keyLabel.toUtf8().constData(), why.toUtf8().constData());
        std::fflush(stderr);
        if (trayPtr)
            trayPtr->showMessage(keyLabel, why, QSystemTrayIcon::Warning, 5000);
    };

    // F1 = 截图（本体在 plugins/capture，2026-10-15 搬过去的）、F2 = 录屏（plugins/record）：
    // 这层壳只把热键编号翻译成动作名递过去。注册仍然在这里 —— 主程序必须是那个还能起得来的薄壳。
    HotkeyFilter filter([&](WPARAM id) {
        if (id == kCaptureId)
            routeHotkey(QStringLiteral("F1 截图"), QStringLiteral("capture"));
        else if (id == kRecordId)
            routeHotkey(QStringLiteral("F2 录屏"), QStringLiteral("record"));
        else
            win.toggleVisible();
    });
    app.installNativeEventFilter(&filter);

    // 热键：优先读配置（可在设置页修改），被占用时降级到备选组合，
    // 之后每 30 秒自动尝试"夺回"首选热键（占用者退出后无需重启）
    UINT curMods = 0, curVk = 0;
    bool hotkeyOk = false;
    QString hotkeyLabel;

    const auto unregisterCurrent = [&] {
        UnregisterHotKey(nullptr, (int)kHotkeyId);
    };
    const auto tryRegister = [&](UINT mods, UINT vk) {
        return RegisterHotKey(nullptr, (int)kHotkeyId, mods | MOD_NOREPEAT, vk);
    };

    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const QString preferred = cfg.value("general/hotkey", "Alt+Space").toString();
    UINT prefMods = MOD_ALT, prefVk = VK_SPACE;
    keySeqToHotkey(QKeySequence(preferred), &prefMods, &prefVk);

    // 降级链：不用 Win+Space（与系统输入法切换冲突）
    struct HotkeyCombo { UINT mods; const char* label; };
    static const HotkeyCombo combos[] = {
        {MOD_CONTROL | MOD_ALT, "Ctrl+Alt+Space"},
        {MOD_CONTROL | MOD_SHIFT, "Ctrl+Shift+Space"},
    };

    UINT m2 = 0, v2 = 0;
    if (keySeqToHotkey(QKeySequence(preferred), &m2, &v2)
        && tryRegister(m2, v2)) {
        hotkeyOk = true;
        hotkeyLabel = preferred;
        curMods = m2;
        curVk = v2;
    } else {
        Utils::log(QString("RegisterHotKey failed: %1 (err %2)")
                       .arg(preferred).arg(GetLastError()));
        for (const HotkeyCombo& c : combos) {
            if (tryRegister(c.mods, VK_SPACE)) {
                hotkeyOk = true;
                hotkeyLabel = QString::fromUtf8(c.label);
                curMods = c.mods;
                curVk = VK_SPACE;
                break;
            }
            Utils::log(QString("RegisterHotKey failed: %1 (err %2)")
                           .arg(c.label).arg(GetLastError()));
        }
    }
    Utils::log(QString("startup: hotkey=%1 pid=%2 exe=%3")
                   .arg(hotkeyOk ? hotkeyLabel : "FAILED")
                   .arg(GetCurrentProcessId())
                   .arg(QCoreApplication::applicationFilePath()));
    win.setHotkeyLabel(hotkeyOk ? hotkeyLabel : QString());

    // 右键菜单是静态注册表项：表里新加了项（比如"压缩为 .lzx"），已经集成过的机器
    // 不会自己长出来，启动时补写一次
    // 右键那几项该不该在 = 用户那个勾 × 「右键动作」插件在不在（卸载/禁用就撤掉，
    // 装回来自动写上）。里面顺带做了老那件"表里加了项、老机器不会自己变"的重写。
    ShellMenu::syncWithPlugin();

    // 托盘气泡提示当前实际热键
    std::function<void()> updateMenuHotkey = [] {}; // 托盘创建后赋值
    const auto notifyHotkey = [&](const QString& label) {
        if (trayPtr)
            trayPtr->showMessage("ltool",
                                 QString("当前呼出热键：%1\n（可在设置中修改）").arg(label),
                                 QSystemTrayIcon::Information, 4000);
    };

    // 设置页修改快捷键 → 重新注册
    auto applyHotkeyChange = [&](const QString& seq) {
        UINT nm = 0, nv = 0;
        unregisterCurrent();
        if (!keySeqToHotkey(QKeySequence(seq), &nm, &nv)
            || !tryRegister(nm, nv)) {
            tryRegister(curMods, curVk); // 回退当前热键
            Utils::log(QString("hotkey change to %1 FAILED, keep %2").arg(seq, hotkeyLabel));
            return;
        }
        curMods = nm;
        curVk = nv;
        hotkeyLabel = seq;
        win.setHotkeyLabel(seq);
        Utils::log(QString("hotkey changed to %1").arg(seq));
    };

    // 每 30 秒尝试夺回首选热键（热键不可用窗口约 1ms）
    auto reclaimHotkey = [&]() {
        if (!hotkeyOk || hotkeyLabel == preferred)
            return;
        unregisterCurrent();
        UINT pm = 0, pv = 0;
        if (keySeqToHotkey(QKeySequence(preferred), &pm, &pv) && tryRegister(pm, pv)) {
            curMods = pm;
            curVk = pv;
            hotkeyLabel = preferred;
            win.setHotkeyLabel(preferred);
            Utils::log(QString("hotkey reclaimed: %1").arg(preferred));
            notifyHotkey(preferred);
        } else {
            tryRegister(curMods, curVk); // 重新挂上当前降级组合
        }
    };
    auto* reclaimTimer = new QTimer(&app);
    reclaimTimer->setInterval(30 * 1000);
    QObject::connect(reclaimTimer, &QTimer::timeout, reclaimHotkey);
    reclaimTimer->start();

    // 托盘
    QSystemTrayIcon tray(Utils::appIcon());
    trayPtr = &tray;
    QMenu menu;
    // 这份样式全程序只有 Theme::menuStyle() 那一份（这里以前另抄了一份字面量，
    // 抄两份的代价就是"改了四处漏一处"，10-17 置顶窗那个白底菜单就是这么来的）。
    // 托盘这棵是启动时建一次、活一整天的，所以必须走 bind —— 直接 setStyleSheet
    // 的话，他拨完「日间」这颗菜单还是黑的。
    Theme::bindMenu(&menu);
    QAction* showAction = menu.addAction(QString("显示搜索框 (%1)").arg(hotkeyLabel));
    QObject::connect(showAction, &QAction::triggered, [&win] { win.showLauncher("托盘右键菜单「显示搜索框」"); });
    updateMenuHotkey = [&] {
        showAction->setText(QString("显示搜索框 (%1)").arg(hotkeyLabel));
    };
    QObject::connect(menu.addAction("设置"), &QAction::triggered, &win, &SearchWindow::openSettings);
    // 「关于」就在设置最底下那一行，托盘直接把它翻到面前（省得他在七行菜单里找）
    QObject::connect(menu.addAction("关于"), &QAction::triggered, [&win] {
        win.openSettingsPage(3);   // 关于是第四行（菜单只剩四行）
    });
    QObject::connect(menu.addAction("重新扫描"), &QAction::triggered, &win, &SearchWindow::rescan);
    // 这两项以前是直接 new 那两个工具窗（JsonTool/Base64Tool）。它们搬进 DLL 插件之后，
    // 主程序够不着那两个类了，只能走和搜索框同一条路：弹设置页 → 展开那张卡。
    QObject::connect(menu.addAction("JSON 格式化"), &QAction::triggered,
                     [&win] { win.openTool(QStringLiteral("json")); });
    QObject::connect(menu.addAction("Base64 编解码"), &QAction::triggered,
                     [&win] { win.openTool(QStringLiteral("base64")); });
    QAction* logAction = menu.addAction("安装日志");
    QObject::connect(logAction, &QAction::triggered, [] { SilentInstallLog::showLog(); });
    QAction* autoRun = menu.addAction("开机自启");
    autoRun->setCheckable(true);
    autoRun->setChecked(Platform::autostartEnabled());
    QObject::connect(autoRun, &QAction::toggled, &Platform::setAutostart);
    menu.addSeparator();
    QObject::connect(menu.addAction("退出"), &QAction::triggered, &QApplication::quit);
    tray.setContextMenu(&menu);
    tray.show();
    QObject::connect(&tray, &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger && r != QSystemTrayIcon::DoubleClick)
            return;
        // 有安装在跑时，点图标的语义变成"打开看台"：进度不该只藏在右键菜单里
        if (SilentInstallLog::busy())
            SilentInstallLog::showLog();
        else
            win.showLauncher("托盘图标左键单击");
    });

    // 后台静默安装不再自动弹日志窗（会抢焦点），改成托盘图标右下角转圈：
    // 转圈 = 有安装在跑，点图标看进度和「取消安装」，装完自动停下换回原图标。
    const QIcon idleIcon = Utils::appIcon();
    const QVector<QIcon> spinFrames = spinnerFrames(idleIcon);
    auto* spinTimer = new QTimer(&app);
    spinTimer->setInterval(120);
    bool trayBusy = false;
    int trayFrame = 0;
    QObject::connect(spinTimer, &QTimer::timeout, [&] {
        const int n = SilentInstallLog::busyCount();
        if (n <= 0) {
            if (!trayBusy)
                return;
            trayBusy = false;
            tray.setIcon(idleIcon);
            tray.setToolTip("ltool");
            logAction->setText("安装日志");
            return;
        }
        trayFrame = (trayFrame + 1) % spinFrames.size();
        tray.setIcon(spinFrames[trayFrame]);
        tray.setToolTip(QString("正在静默安装 %1 项，点击查看安装日志").arg(n));
        const QString text = QString("安装日志（%1 项进行中）").arg(n);
        if (logAction->text() != text)
            logAction->setText(text);
        trayBusy = true;
    });
    spinTimer->start();

    if (!hotkeyOk) {
        tray.showMessage("ltool", "所有热键组合注册失败（可能被其他软件占用）\n"
                                  "可通过托盘图标打开搜索框，详见 %APPDATA%\\ltool\\log.txt",
                         QSystemTrayIcon::Warning, 8000);
        Utils::log("hotkey: all combos failed");
    } else if (hotkeyLabel != preferred) {
        // 启动即降级：明确告知当前实际热键（此后每 30 秒自动尝试夺回首选热键）
        QTimer::singleShot(1500, &win, [&] { notifyHotkey(hotkeyLabel); });
    }

    // F1 截图热键（独立注册，失败仅记录）
    if (!RegisterHotKey(nullptr, (int)kCaptureId, MOD_NOREPEAT, VK_F1))
        Utils::log(QString("F1 capture hotkey failed (err %1)").arg(GetLastError()));
    // F2 录屏（只录选中的那一个窗口）。失败只记日志：F2 被别的程序占了也不该拖住启动
    if (!RegisterHotKey(nullptr, (int)kRecordId, MOD_NOREPEAT, VK_F2))
        Utils::log(QString("F2 record hotkey failed (err %1)").arg(GetLastError()));

    win.startInitialScan(); // 启动即后台扫描（增量优先）
    TaskScheduler::instance().start(); // 定时任务调度（每 30 秒检查）
    // 广告弹窗拦截器：常驻，配置里勾了"自动拦截"就一直在跑 ——
    // 以前它是设置页里的一个 QTimer，页面一销毁就停，等于从来没真正生效过
    AdBlocker::instance().start();
    // 挂了牌（plugin.json 的 "Activate"）的插件在这一步被叫起来：它们后台跑的东西
    // 不靠页面活着 —— 页是点开卡片才建的。没人挂牌就是零成本（一个 dll 都不多装）。
    PluginHost::startBackground();
    // 剪贴板历史：默认关闭，勾了才在后台收（见设置 → 配置 → 剪贴板）
    ClipboardStore::instance().start();

    // 以前这里每 10 分钟 EmptyWorkingSet 一次，看起来"内存漂亮"，代价是下一次按热键
    // 要把整个 Qt + 索引的页换回来，实测就是"唤出很卡"。只在截图那种一次性大块内存后回收。
    // 启动 1.2 秒后把启动器窗口预热一次（见 SearchWindow::warmUp）
    QTimer::singleShot(1200, &win, &SearchWindow::warmUp);
    // 每天一次的插件自动更新（他 2026-10-21 点名：「插件商店增加一个自动更新功能，一天自动更新一次」）。
    // 延迟十几秒才跑：冷启动那阵子在扫应用 + 预热窗口，别让一条慢网络把"呼出很卡"带上；
    // 带 '--' 旗标的自测进程一律不跑（那会变成边测边往插件目录写 dll）。
    if (!inLoopSelftest) {
        QTimer::singleShot(15000, &app, [] {
            auto sw = std::make_shared<QElapsedTimer>();
            sw->start();
            PluginStore::runDailyUpdate(nullptr, [sw](const QString& note) {
                if (!note.isEmpty())   // 空串 = 今天没到点 / 没开：那种"心跳"不配占一行日志
                    Utils::log(QStringLiteral("store: 自动更新这一趟 %1 ms：%2").arg(sw->elapsed()).arg(note));
            });
        });
    }
    // --autoupdatenow：立刻跑那一趟（不等 15 秒）。要联网、会**真往插件目录写 dll**，
    // 所以不进 CI，也不该在别的自测里顺带跑。它存在的理由：那条链（拉清单 → 挑 → 一颗一颗装 →
    // 写那句 note）离屏判据一段都测不到，只有真跑一遍才知道通不通。
    // 先把"今天跑过没跑过"那颗闸推开（删掉日期），其余走的是同一颗 runDailyUpdate —— 不另写一条路。
    if (args.contains("--autoupdatenow")) {
        QSettings cfg(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat);
        cfg.remove(QStringLiteral("store/autoUpdateDate"));
        cfg.sync();
        QTimer::singleShot(120, &app, [&app] {
            auto sw = std::make_shared<QElapsedTimer>();
            sw->start();
            PluginStore::runDailyUpdate(nullptr, [&app, sw](const QString& note) {
                std::printf("autoupdatenow: %s · 这一趟 %lld ms\n",
                            note.isEmpty() ? QStringLiteral("(没跑：开关关着)").toUtf8().constData()
                                           : note.toUtf8().constData(),
                            static_cast<long long>(sw->elapsed()));
                std::fflush(stdout);
                app.quit();
            });
        });
        QTimer::singleShot(180000, &app, [&app] {
            std::printf("autoupdatenow: TIMEOUT（网络那条一直没回话）\n");
            std::fflush(stdout);
            app.quit();
        });
    }

    if (showOnStart)
        QTimer::singleShot(200, &win, [&win] { win.showLauncher("--show 启动即显示"); });
    if (openSettingsOnStart)
        QTimer::singleShot(300, &win, &SearchWindow::openSettings);

    // --settingspage <菜单行> [tab]：截图核对某个设置页用（也能从搜索框直达）
    if (args.contains("--settingspage")) {
        const QStringList rest = args.mid(args.indexOf("--settingspage") + 1);
        const int row = rest.value(0, "0").toInt();
        const int tab = rest.size() > 1 ? rest.value(1).toInt() : -1;
        const QString emb = rest.size() > 2 ? rest.value(2) : QString();
        QTimer::singleShot(700, &app, [row, tab, emb] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* dlg = qobject_cast<SettingsDialog*>(w)) {
                    dlg->show();
                    dlg->raise();
                    dlg->activateWindow();
                    dlg->openAt(row, tab, emb);
                    return;
                }
            }
        });
    }

    // --toggle：验"按热键到底出不出来"。三种真实情形各按一次：冷状态、被最小化过
    // （Win+D 也算）、窗口还"可见但躲在别的程序后面"。不合成鼠标键盘，只开自己的窗口抢前台。
    // 必须冷启动跑：另有实例在跑时命令行参数会被转发掉，那种情况下这个测试不会执行。
    int toggleBad = 0;
    if (args.contains("--toggle")) {
        const auto check = [&toggleBad](const char* name, bool ok, const QString& got) {
            if (!ok)
                ++toggleBad;
            std::printf("%s %s got=[%s]\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
            std::fflush(stdout);
        };
        QTimer::singleShot(1000, &app, [&win, &app, check, &toggleBad] {
            const auto state = [&win] {
                const HWND h = reinterpret_cast<HWND>(win.winId());
                return QStringLiteral("visible=%1 前台=%2 最小化=%3 位置=%4,%5 %6x%7")
                    .arg(win.isVisible() ? 1 : 0)
                    .arg(GetForegroundWindow() == h ? 1 : 0)
                    .arg((win.windowState() & Qt::WindowMinimized) ? 1 : 0)
                    .arg(win.x()).arg(win.y()).arg(win.width()).arg(win.height());
            };
            const auto pump = [](int times) {
                for (int i = 0; i < times; ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(20);
                }
            };
            const auto mine = [&win] { return reinterpret_cast<HWND>(win.winId()); };

            win.toggleVisible();
            pump(10);
            bool onScreen = false;
            const QRect g = win.geometry();
            for (const QScreen* s : QGuiApplication::screens())
                if (s->availableGeometry().intersects(g))
                    onScreen = true;
            check("冷状态第一次按：出来了，而且落在某个显示器内", win.isVisible() && onScreen, state());

            ShowWindow(mine(), SW_MINIMIZE);
            pump(10);
            std::printf("INFO 最小化之后：%s\n", state().toUtf8().constData());
            // 注意这里不断言"前台不是它"：实测最小化之后 GetForegroundWindow() 仍然算它自己
            //（正是 toggleVisible 必须额外看最小化位的原因）
            // 但这一支在 offscreen 上根本造不出来：那个平台没有真 HWND，`SW_MINIMIZE` 是空操作。
            // 以前它当红项算（HEAD 上就是红的，而 --toggle 不在 CI 那 14 面里，所以一直没人看见），
            // 按仓库的规矩改成点名 SKIP —— 不算绿，也不算红。
            if (!(win.windowState() & Qt::WindowMinimized))
                std::printf("SKIP 确实造出了「最小化」这个状态：offscreen 没有真 HWND，SW_MINIMIZE 不生效（这一支要 QT_QPA_PLATFORM=windows 跑）\n");
            else
                check("确实造出了\"最小化\"这个状态", win.isVisible(), state());
            win.toggleVisible();
            pump(15);
            std::printf("INFO 再按一次：%s\n", state().toUtf8().constData());
            // 只断言"窗口拉回来了"这一件确定的事。"顺手抢到前台"脚本环境保证不了：
            // Windows 的前台锁看的是最后一次真实输入，而这里没有任何键盘动作
            //（实测 10 次里会有 1 次拉回来了但前台没给我们）。真实按热键时 Alt 就是那次输入，
            // 到底会不会抢不到，看 showLauncher 里那条"置前失败"日志，不在这里猜。
            check("被最小化过再按热键能拉回来", win.isVisible()
                  && !(win.windowState() & Qt::WindowMinimized), state());

            QWidget* other = new QWidget(nullptr, Qt::Window);
            other->resize(240, 120);
            other->setWindowTitle(QStringLiteral("ltool-toggle-other"));
            other->show();
            pump(5);
            SetForegroundWindow(reinterpret_cast<HWND>(other->winId()));
            // 失焦收起有 500ms 宽限（防止刚弹出来就被自己的激活竞争收掉），宽限过后
            // 还有一次 600ms 的复查 —— 所以要等过这两档再看它收没收
            pump(70);
            std::printf("INFO 盖上别的窗口之后：%s\n", state().toUtf8().constData());
            // "可见但躲在别的窗口后面"这个状态实际到不了：这个窗口失焦会自己收起
            //（event() 里 WindowDeactivate 那支，带 500ms 宽限）。所以这里核对的是失焦确实收起了。
            check("失焦之后确实自己收起了（设计如此）", !win.isVisible(), state());
            win.toggleVisible();
            pump(15);
            std::printf("INFO 再按一次：%s\n", state().toUtf8().constData());
            check("收起之后再按：能再呼出（他一天要按几百次这一支）", win.isVisible(), state());

            if (GetForegroundWindow() == mine()) {
                win.toggleVisible();
                pump(5);
                check("它就在前台时再按一次=收起", !win.isVisible(), state());
            } else {
                std::printf("SKIP 在前台时再按=收起（这个环境抢不到前台，测不了这一支）\n");
            }
            // ---- 失焦到底该不该收：问那颗纯函数（离屏抢不到前台，真造不出这两支）----
            // 他 2026-10-21 报的"隐藏的时候会卡一下"就是第 1 条：以前这里只问"离上次呼出够不够 500ms"，
            // 于是**呼出后 0.5 秒内点走**一律被当成激活竞争吞掉，要等 600ms 的复查才收，
            // 看起来就是"我点开了别的东西，它还挂在那儿"。现在先问"这一次有没有真的拿到过前台"。
            {
                const auto name = [](SearchWindow::BlurAction a) {
                    return QString::fromUtf8(a == SearchWindow::BlurAction::HideNow ? "立刻收"
                                   : a == SearchWindow::BlurAction::IgnoreActivationRace ? "当竞争，稍后复查"
                                   : a == SearchWindow::BlurAction::WaitCaptureEnds ? "等截图" : "菜单开着不动");
                };
                using BA = SearchWindow::BlurAction;
                const auto act = [](bool fg, bool back, qint64 ms, bool menu, bool cap) {
                    return SearchWindow::blurAction(fg, back, ms, menu, cap);
                };
                check("拿到过前台 + 呼出后 120ms 点走 = 立刻收（他说的就是这一支）",
                      act(true, false, 120, false, false) == BA::HideNow, name(act(true, false, 120, false, false)));
                check("拿到过前台 + 隔了 2 秒点走 = 立刻收（同一支，不该看那个数字）",
                      act(true, false, 2000, false, false) == BA::HideNow, name(act(true, false, 2000, false, false)));
                check("从没拿到过前台 + 120ms = 先不动，稍后复查（#40 那一支不许退化）",
                      act(false, false, 120, false, false) == BA::IgnoreActivationRace,
                      name(act(false, false, 120, false, false)));
                check("从没拿到过前台 + 2 秒 = 收起（宽限早过了，没有理由还挂着）",
                      act(false, false, 2000, false, false) == BA::HideNow, name(act(false, false, 2000, false, false)));
                check("自己的右键菜单开着 = 不收（哪怕已经拿到过前台）",
                      act(true, false, 120, true, false) == BA::StayMenuOpen, name(act(true, false, 120, true, false)));
                check("F1 截图进行中 = 等它结束再收（--capturehold 那一支优先）",
                      act(true, false, 120, false, true) == BA::WaitCaptureEnds, name(act(true, false, 120, false, true)));
                check("拿到过前台、可前台又回到了呼出之前那个窗口 = 当激活竞争（#40 那支没被换掉）",
                      act(true, true, 120, false, false) == BA::IgnoreActivationRace,
                      name(act(true, true, 120, false, false)));
                check("拿到过前台、前台去了**别的**窗口 = 真点走了，立刻收（这就是他报的那一下）",
                      act(true, false, 120, false, false) == BA::HideNow,
                      name(act(true, false, 120, false, false)));
                check("这两支必须给不同答案，否则「拿到过前台」这个输入等于没读",
                      act(true, false, 120, false, false) != act(false, false, 120, false, false),
                      QStringLiteral("%1 vs %2").arg(name(act(true, false, 120, false, false)),
                                                     name(act(false, false, 120, false, false))));
            }
            delete other;
            std::printf("toggle: 失败 %d 项\n", toggleBad);
            std::fflush(stdout);
            app.quit();
        });
        QTimer::singleShot(30000, &app, [&app, &toggleBad] {
            std::printf("toggle: 30 秒没跑完，退出（失败 %d 项）\n", toggleBad);
            ++toggleBad;
            app.quit();
        });
    }

    // --capgeo / --capflash：判据本体在 plugins/capture（ltoolCapgeoRun / ltoolCapflashRun），
    // 但这两条**必须留在单实例守卫之后**：浮层要在事件循环里真开出来才算数。
    // 延一拍再跑是原来的行为（窗口刚建好那一下屏幕还在动，量出来的几何不作数）。
    int capGeoBad = 0;
    int capFlashBad = 0;
    if (args.contains("--capgeo")) {
        QTimer::singleShot(80, &app, [&app, &capGeoBad] {
            runPluginSelftest(QStringLiteral("capture"), "ltoolCapgeoRun",
                              SelftestArg::None, 0, QString(), "capgeo", &capGeoBad);
            app.quit();
        });
    }
    if (args.contains("--capflash")) {
        const QString nArg = args.mid(args.indexOf("--capflash") + 1).value(0);
        const int times = nArg.isEmpty() ? 2 : qBound(1, nArg.toInt(), 5);
        QTimer::singleShot(500, &app, [&app, times, &capFlashBad] {
            runPluginSelftest(QStringLiteral("capture"), "ltoolCapflashRun",
                              SelftestArg::Ms, times, QString(), "capflash", &capFlashBad);
            app.quit();
        });
    }

    // --storenet：商店**联网那一段**的界面判据 —— 建一个离屏的 ToolsPage，点「刷新清单」，
    // 等回复落地，然后数画出来的行、和状态行里那个数对一遍。
    // 它和 --storetest 分家是必须的：那条一次都不联网；这条要真去拉 github raw，
    // 没网的环境跑它就是自找的红，所以也不进 CI（他点头之后再手动跑）。
    int storeNetBad = 0;
    if (args.contains("--storenet")) {
        QTimer::singleShot(120, &app, [&app, &storeNetBad] {
            ToolsPage page;
            page.setAttribute(Qt::WA_DontShowOnScreen, true);
            page.resize(940, 720);
            page.show();
            for (int i = 0; i < 10; ++i)
                QCoreApplication::processEvents();
            const auto check = [&storeNetBad](const char* name, bool ok, const QString& got) {
                std::printf("%s %s got=[%s]\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
                if (!ok)
                    ++storeNetBad;
            };
            // 先把"上哪儿拉"打出来：1.3.0 起这条按版本号点名（releases/download/v<版本>/），
            // 这一版还没发商店资产就是 404 —— 后面那些"卡片张数"会一起红，
            // 那**不是商店坏了**，是没发版。不印这行的话下一次没人分得清这两件事。
            std::printf("storenet: 清单地址 = %s\n",
                        PluginStore::indexUrl().toUtf8().constData());
            QPushButton* refresh = page.findChild<QPushButton*>(QStringLiteral("storeRefresh"));
            check("标题那一行真有一颗「刷新清单」，而且在布局里", refresh && refresh->isVisible(),
                  QStringLiteral("找得到=%1 可见=%2").arg(refresh ? 1 : 0)
                      .arg(refresh && refresh->isVisible() ? 1 : 0));
            if (!refresh) {
                std::printf("storenet: 失败 %d 项（没有刷新按钮，后面没法跑）\n", storeNetBad);
                app.quit();
                return;
            }
            // 数之前先把 deleteLater 排干净：rebuildGrid 换整格时旧卡片是"待删"状态，
            // 这时候数 findChildren 会把新旧两批一起数进去（探针第一版就是这么量出 30 张卡的）。
            const auto drain = [] { for (int i = 0; i < 30; ++i) QCoreApplication::processEvents(); };
            drain();
            const auto countButtons = [&page](const QStringList& texts) {
                int n = 0;
                for (QPushButton* b : page.findChildren<QPushButton*>()) {
                    if (b->objectName() == QLatin1String("storeRefresh") || !b->isVisible())
                        continue;
                    if (texts.contains(b->text()))
                        ++n;
                }
                return n;
            };
            const int cardsBefore = page.findChildren<QFrame*>(QStringLiteral("row")).size();
            // 还没拉清单：这一面只报目录里有什么，所以一颗「装」都没有 —— 装什么得先问仓库
            const int installersBefore = countButtons({QStringLiteral("装"), QStringLiteral("更新")});
            refresh->click();
            // 等到状态行变成"清单 N 条…"为止。raw 平时几百毫秒，慢了通常是代理在排队 ——
            // 20 秒上限，到点就把看到的说出来，不许静默当"通过"，也不许把"网络慢"读成"界面坏"。
            QString st;
            QLabel* status = nullptr;
            for (int i = 0; i < 400; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
                status = nullptr;
                for (QLabel* l : page.findChildren<QLabel*>()) {
                    // 「网络没通…」是拉失败但本地有缓存时页面上那句短的（2026-10-23 那一刀之后）。
                    // 不认它的话，这一栏在未发版那次只会白等 20 秒，而"页面上不许有 URL"那条
                    // 判据就永远只在一条跑不到的成功路径上量 —— 那是装饰性的绿。
                    const QString t = l->text();
                    if (t.startsWith(QStringLiteral("清单")) || t.startsWith(QStringLiteral("网络没通"))) {
                        status = l;
                        break;
                    }
                }
                st = status ? status->text() : QString();
                if (st.startsWith(QStringLiteral("清单 ")) || st.startsWith(QStringLiteral("清单没拉到"))
                    || st.startsWith(QStringLiteral("网络没通")))
                    break;
            }
            drain();
            std::printf("INFO 状态行：%s\n", st.toUtf8().constData());
            // 2026-10-23 那一刀之后，页面上那句只剩数，URL / 走的哪一档 / 试了几次挂在悬停说明上。
            // 所以这一格的"说清"是两句合起来说的 —— 少一句都算没说清，判据读合起来那一份。
            const QString tip = status ? status->toolTip() : QString();
            std::printf("INFO 悬停说明：%s\n", tip.toUtf8().constData());
            const QString both = st + QStringLiteral(" · ") + tip;
            check("页面上那一句不许带 URL / HTTP 码（全文只该在悬停说明和日志里）",
                  st.contains(QStringLiteral("http")) ? "no" : "yes", "yes");
            const QRegularExpression re(QStringLiteral(
                "清单 (\\d+) 条（(.+?) · 走的是 (.+?)，一共试了 (\\d+) 次）· 这一面 (\\d+) 张卡"
                " · 能装或可更新 (\\d+) · 拒收 (\\d+) · 清单外 (\\d+)"));
            const auto m = re.match(both);
            check("拉到了就在状态行把数说清（没拉到会走另一句，不当成通过）", m.hasMatch(), st.left(70));
            if (!m.hasMatch()) {
                // 这一句别说漏：下面那一大段"逐张卡片核对状态词↔按钮↔正文可点"全挂在 hasMatch 里。
                // 清单没落地（比如这一版还没发商店资产）时它们一条都不会跑，
                // 而 storenet 只红这一条 —— 不说出来就像"卡片都验过了"。
                std::printf("WARN 逐张卡片的核对这次没跑（清单没落地，卡片来自缓存或压根没画）：别当成验过\n");
                for (QLabel* l : page.findChildren<QLabel*>())
                    if (!l->text().isEmpty())
                        std::printf("  [%s]\n", l->text().left(90).toUtf8().constData());
            } else {
                const int said = m.captured(1).toInt();
                const QString via = m.captured(3);
                const int tries = m.captured(4).toInt();
                const int cards = m.captured(5).toInt();
                const int canInstall = m.captured(6).toInt();
                const int rejected = m.captured(7).toInt();
                const int outside = m.captured(8).toInt();
                // 「这次是从哪条拿到的」必须说得出出口：镜像活着就是镜像，镜像挂了才算代理，都不行才是直连
                check("回话说清了这次走的是哪条",
                      via.startsWith(QStringLiteral("镜像 ")) || via == QStringLiteral("本地代理")
                          || via == QStringLiteral("直连"),
                      via);
                check("至少试了一次", tries >= 1, QStringLiteral("试了 %1 次").arg(tries));
                const int nowCards = page.findChildren<QFrame*>(QStringLiteral("row")).size();
                // 只剩一面列表：卡片数就是状态行报的那个数，而且它是"清单 + 清单外"合起来
                check("一面列表：画出的卡片数 = 状态行说的数", nowCards == cards,
                      QStringLiteral("画出 %1 / 说了 %2").arg(nowCards).arg(cards));
                check("卡片数 = 清单条数 + 清单外那几个", cards == said + outside,
                      QStringLiteral("清单 %1 + 清单外 %2 = %3").arg(said).arg(outside).arg(cards));
                check("刷新前后是同一面列表（卡片数随目录/清单变，不是另开一屏）", nowCards >= cardsBefore,
                      QStringLiteral("刷新前 %1 / 刷新后 %2").arg(cardsBefore).arg(nowCards));
                check("插件仓库那十二个都在清单里", said >= 12, QStringLiteral("清单 %1 条").arg(said));
                // 装了的那张卡**不许再有「装」这颗按钮**，没装的那张必须有 —— 这是这次改的正面判据
                // 六个状态词都从 stateWord 取，别把中文抄第二遍（抄的那份第一次加状态就过期）
                const QVector<QString> words = {
                    PluginStore::stateWord(PluginStore::State::Missing),      // 0 能装
                    PluginStore::stateWord(PluginStore::State::Update),       // 1 可更新
                    PluginStore::stateWord(PluginStore::State::Installed),    // 2 已装
                    PluginStore::stateWord(PluginStore::State::Disabled),     // 3 已禁用
                    PluginStore::stateWord(PluginStore::State::ApiMismatch),  // 4 装不了
                    PluginStore::stateWord(PluginStore::State::LocalOnly),    // 5 清单外
                    PluginStore::stateWord(PluginStore::State::Different),
                };
                // words 的下标要和这张表一一对上：判据认的是**数据层那个枚举**，不是文案
                const PluginStore::State stateOrder[7] = {
                    PluginStore::State::Missing, PluginStore::State::Update,
                    PluginStore::State::Installed, PluginStore::State::Disabled,
                    PluginStore::State::ApiMismatch, PluginStore::State::LocalOnly,
                    PluginStore::State::Different,
                };
                // 逐张卡看：状态词说什么，按钮就该给什么。这是这次"一张列表"的正面判据 ——
                // 装了的不许出现「装」，没装的不许出现「禁用」，可更新的两颗都得有。
                int states = 0, nMissing = 0, nOnDisk = 0, wrong = 0;
                int perState[7] = {0};
                int diffNewer = 0;
                QStringList wrongs;
                for (QFrame* card : page.findChildren<QFrame*>(QStringLiteral("row"))) {
                    int idx = -1;
                    // 先认数据层那颗状态（makeCard 挂在卡片属性上），再要求界面**确实把那个词画出来了**：
                    // 两边都得对上，缺一边就是"卡片说的和给的不一致"。
                    // 旧写法只拿 startsWith 认文案，"↑ 可更新"那个箭头前缀一加上，14 张卡全成"没有状态词的卡"，
                    // 而第 3 条「可更新那几张有绿箭头」一直在"可更新 0 张"上空转 —— 假绿比假红更贵。
                    const QVariant st = card->property("ltoolState");
                    if (st.isValid()) {
                        for (int i = 0; i < 7; ++i)
                            if (st.toInt() == static_cast<int>(stateOrder[i]))
                                idx = i;
                        if (idx >= 0) {
                            bool drawn = false;
                            for (QLabel* l : card->findChildren<QLabel*>())
                                if (l->text().contains(words[idx]))
                                    drawn = true;
                            if (!drawn)
                                idx = -1;      // 状态有、字没画 = 界面在骗人，照样按"认不出"报
                        }
                    }
                    bool hasInstall = false, hasToggle = false, hasUninstall = false;
                    for (QPushButton* b : card->findChildren<QPushButton*>()) {
                        if (!b->isVisible())
                            continue;
                        const QString t = b->text();
                        if (t == QStringLiteral("装") || t == QStringLiteral("更新"))
                            hasInstall = true;
                        if (t == QStringLiteral("禁用") || t == QStringLiteral("启用"))
                            hasToggle = true;
                        if (t == QStringLiteral("卸载"))
                            hasUninstall = true;
                    }
                    if (idx < 0) {
                        ++wrong;
                        // 取证要带上那张卡自己写了什么：上一版只说"一张没有状态词的卡"，
                        // 于是"文案换了个头"和"状态真的没画出来"长得一模一样，来回两轮才分清。
                        QString seen;
                        for (QLabel* l : card->findChildren<QLabel*>())
                            if (!l->text().isEmpty())
                                seen += QStringLiteral(" [%1]").arg(l->text().left(24));
                        wrongs << QStringLiteral("(没有状态词的卡:%1)").arg(seen.isEmpty()
                                                                      ? QStringLiteral(" 全空") : seen);
                        continue;
                    }
                    ++states;
                    ++perState[idx];
                    // 「装了却点不开」这一类要当场抓到：正文该不该有点击，界面上是三样东西一起动的
                    // （状态词、按钮、手形），而只有 makeCard 里那个 `openable` 是决定它的一半 ——
                    // 判据量那颗 bool（卡片属性），不量光标形状那种二手证据。
                    // 10-20 那次两张卡的正文被给了空函数，状态词和按钮都还是对的。
                    const bool clickable = card->property("ltoolClickable").toBool();
                    const bool open = card->property("ltoolOpenable").toBool();
                    const bool wantClickable = idx == 0                       // 没装那张：点正文 = 按「装」
                        || (idx >= 1 && idx != 3 && idx != 4 && open);        // 装了的、宿主认得出来的那张
                    if (clickable != wantClickable) {
                        ++wrong;
                        wrongs << QStringLiteral("「%1」(%2) 那张 正文可点=%3（该=%4，宿主认得出=%5）")
                                      .arg(words[idx], card->property("ltoolId").toString())
                                      .arg(clickable ? QStringLiteral("有") : QStringLiteral("没"))
                                      .arg(wantClickable ? QStringLiteral("有") : QStringLiteral("没"))
                                      .arg(open ? QStringLiteral("是") : QStringLiteral("否"));
                    }
                    // 「与商店不同」其实两回事：本机号比商店高（新号已经发了、商店还没重打那批产物），
                    // 和同号但内容不同（本机构建 vs CI 构建，天生两串哈希）。这两句只写在悬停里，
                    // 不量就得人手开界面逐张看 —— 「每颗自己递增」到底生效没有就靠这两个数说话。
                    if (idx == 6 && card->toolTip().contains(QStringLiteral("还新")))
                        ++diffNewer;
                    const bool wantInstall = (idx == 0 || idx == 1)
                        && card->property("ltoolEntryUsable").toBool();
                    const bool wantToggle = idx >= 1 && idx != 4;   // 装了的（含可更新、清单外、已禁用）都能动
                    // 「卸载」跟着 toggle 走：本机真有那个文件（含 .off）才谈得上删它
                    const bool wantUninstall = wantToggle;
                    if (idx == 0)
                        ++nMissing;
                    if (wantToggle)
                        ++nOnDisk;
                    if (hasInstall != wantInstall || hasToggle != wantToggle
                        || hasUninstall != wantUninstall) {
                        ++wrong;
                        wrongs << QStringLiteral("「%1」那张 装=%2 禁用=%3 卸载=%4（该给 装=%5 禁用=%6 卸载=%7）")
                                      .arg(words[idx],
                                           hasInstall ? QStringLiteral("有") : QStringLiteral("没"),
                                           hasToggle ? QStringLiteral("有") : QStringLiteral("没"),
                                           hasUninstall ? QStringLiteral("有") : QStringLiteral("没"),
                                           wantInstall ? QStringLiteral("有") : QStringLiteral("没"),
                                           wantToggle ? QStringLiteral("有") : QStringLiteral("没"),
                                           wantUninstall ? QStringLiteral("有") : QStringLiteral("没"));
                    }
                }
                QString dist;
                for (int i = 0; i < 7; ++i)
                    if (perState[i] > 0)
                        dist += QStringLiteral(" %1=%2").arg(words[i]).arg(perState[i]);
                std::printf("INFO 这一面各档：%s（其中「与商店不同」里本机号更高、等商店重发产物的是 %d 张）\n",
                            dist.trimmed().toUtf8().constData(), diffNewer);
                check("每张卡的状态词都认得出", states == nowCards,
                      QStringLiteral("有状态 %1 / 卡片 %2").arg(states).arg(nowCards));
                check("每张卡的按钮和自己那句状态对得上（装了的不给装、没装的不给禁用/卸载）", wrong == 0,
                      wrongs.isEmpty() ? QStringLiteral("%1 张全对上").arg(nowCards)
                                       : QStringLiteral("%1 张不对：%2").arg(wrong).arg(wrongs.join(QStringLiteral("；"))));
                // 大小一律 MB：卡片上任何一行文字都不许留"N B"这种裸字节（MB/KB 这些带单位的单位词不算）
                int bare = 0;
                QStringList bares;
                for (QLabel* l : page.findChildren<QLabel*>()) {
                    if (l->text().contains(QRegularExpression(QStringLiteral("\\d+\\sB(?![a-zA-Z])")))) {
                        ++bare;
                        bares << l->text().left(70);
                    }
                }
                check("卡片上的大小一律写成 MB（一处裸字节都没有）", bare == 0,
                      QStringLiteral("%1 处还写着 B%2").arg(bare)
                          .arg(bares.isEmpty() ? QString() : QStringLiteral("：%1").arg(bares.join(QStringLiteral("；")))));
                // 本机装着的那几张（能给禁用/卸载的）必须报得出大小
                // 本机装着的那几张（能给禁用/启用的）必须报得出自己的大小。
                // 注意按**按钮上的字**筛，别按 objectName：卡片上的按钮都没起名字。
                int sized = 0, needSized = 0;
                for (QFrame* card : page.findChildren<QFrame*>(QStringLiteral("row"))) {
                    bool canToggle = false;
                    for (QPushButton* b : card->findChildren<QPushButton*>())
                        if (b->text() == QStringLiteral("禁用") || b->text() == QStringLiteral("启用"))
                            canToggle = true;
                    if (!canToggle)
                        continue;
                    ++needSized;
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->text().contains(QStringLiteral(" MB")))
                            ++sized;
                }
                check("本机装着的那几张都写出了自己的大小", sized == needSized,
                      QStringLiteral("有大小 %1 / 该有 %2（装着 %3 张）").arg(sized).arg(needSized).arg(nOnDisk));
                // 原来这里有一条"操作按钮排在卡片最底下那一行"的像素判据，撤了：卡片三层布局
                // （73b0bf1）把状态词和按钮放进同一行贴底，状态词换成两行时按钮在同一行里垂直
                // 居中，"按钮底边比状态底边高几像素"是正常形状 —— 那条断言已经表达不了它想表达的事，
                // 留着只会时不时红一条假故障。按钮该不该在，由上面「每张卡的按钮和自己那句状态对得上」盯。
                // 可更新那几张：右上角一颗绿色 ↑ 就是答复，不许再把"本机 …≠ 清单 …"那串写在脸上。
                // 箭头和不等号都用码点拼，不抄字面量（这个文件里中文丢过字符）。
                const QString arrowGlyph = QString(QChar(0x2191));          // ↑
                const QString neqGlyph = QString(QChar(0x2260));            // ≠
                int upCards = 0, upNoArrow = 0, upVerbose = 0;
                for (QFrame* card : page.findChildren<QFrame*>(QStringLiteral("row"))) {
                    QLabel* st = nullptr;
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->property("shopState").toBool())
                            st = l;
                    // 是不是"可更新"看卡片属性那颗（数据层），文案只要求**出现了**那个词：
                    // 画出来的是「↑ 可更新 v1.0.0 · 0.10 MB」，拿 startsWith 认整句会把 14 张全漏掉，
                    // 于是这条判据一直在 upCards=0 上空转（"0 张可更新、缺箭头 0 张"看着像绿的）。
                    if (!st || card->property("ltoolState").toInt()
                                  != static_cast<int>(PluginStore::State::Update))
                        continue;
                    if (!st->text().contains(PluginStore::stateWord(PluginStore::State::Update)))
                        continue;
                    ++upCards;
                    if (st->text().contains(neqGlyph))
                        ++upVerbose;
                    bool arrow = false;
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->text() == arrowGlyph && l->isVisible())
                            arrow = true;
                    if (!arrow)
                        ++upNoArrow;
                }
                check("可更新那几张卡不再写哈希对照那串文案", upVerbose == 0,
                      QStringLiteral("%1 / %2 张还写着").arg(upVerbose).arg(upCards));
                check("每张可更新的卡右上角都有那颗绿箭头（本次量的张数在下面）", upNoArrow == 0,
                      QStringLiteral("可更新 %1 张，缺箭头 %2 张").arg(upCards).arg(upNoArrow));
                int installers = 0, toggles = 0;
                for (QPushButton* b : page.findChildren<QPushButton*>()) {
                    if (b->objectName() == QLatin1String("storeRefresh") || !b->isVisible())
                        continue;
                    if (b->text() == QStringLiteral("装") || b->text() == QStringLiteral("更新"))
                        ++installers;
                    else if (b->text() == QStringLiteral("禁用") || b->text() == QStringLiteral("启用"))
                        ++toggles;
                }
                check("装/更新按钮颗数 = 状态行说的能装或可更新", installers == canInstall,
                      QStringLiteral("按钮 %1 颗 / 说了 %2 条").arg(installers).arg(canInstall));
                check("禁用/启用按钮颗数 = 本机真装着的那几个", toggles == nOnDisk,
                      QStringLiteral("禁用/启用 %1 颗 / 装着 %2 张（能装 %3 张）")
                          .arg(toggles).arg(nOnDisk).arg(nMissing));
                std::printf("INFO 刷新前那一面已经有 %d 颗装/更新按钮：清单有持久化缓存，启动就按缓存画"
                            "（以前那条「没拉过就一颗都不给」的判据跟着 d9c9ed7 作废了）\n",
                            installersBefore);
                std::printf("INFO 拒收 %d 条（本仓库的 dist 现在是干净的）\n", rejected);
            }
            // 「检查更新」那条也真跑一遍：走 /releases/latest 的 302 读版本号，不碰限流的 api。
            // 他刚看到的 403 就是 api 那条（匿名 60 次/小时，整台出口 IP 共享），这条判据盯住它别再回去。
            {
                auto* nam2 = new QNetworkAccessManager(&page);
                DevEnv::applyProxy(nam2);
                QNetworkRequest rq { QUrl(AboutPage::releasesPageUrl()) };
                rq.setRawHeader("User-Agent", QByteArrayLiteral("ltool-updater"));
                rq.setTransferTimeout(20000);
                QNetworkReply* rp = nam2->get(rq);
                for (int i = 0; i < 400 && !rp->isFinished(); ++i) {
                    QCoreApplication::processEvents();
                    QThread::msleep(50);
                }
                const int code = rp->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QString fin = rp->url().toString();
                rp->deleteLater();
                nam2->deleteLater();
                const QString gotVer = AboutPage::versionFromReleasesUrl(fin);
                check("检查更新走网页 302 就读得出最新版本（不碰限流的 api）", !gotVer.isEmpty(),
                      QStringLiteral("HTTP %1 · 最终 %2 · 读到 %3")
                          .arg(code).arg(fin.left(64),
                                         gotVer.isEmpty() ? QStringLiteral("(没认出)") : gotVer));
            }

            // 自动更新那一趟「会挑谁」，拿真清单 + 真目录量一遍（**只挑不装**，一颗都不写盘）。
            // 沙箱那条钉的是谓词逻辑，这一条钉的是它对真实数据也成立：挑出来的每一颗，
            // 本机目录里必须已经有那颗 dll —— 没有就说明"新插件被自动装上了"，那正是他否掉的那一档。
            {
                QStringList aProblems;
                const QVector<PluginStore::Entry> listed =
                    PluginStore::parseIndex(PluginStore::loadCachedIndex(), &aProblems);
                const QString dir = PluginHost::pluginsDir();
                const QVector<PluginStore::Entry> targets = PluginStore::autoUpdateTargets(PluginStore::rows(listed));
                QStringList strays;
                for (const PluginStore::Entry& e : targets)
                    if (!QFileInfo::exists(dir + QLatin1Char('/') + e.file))
                        strays << e.id;
                check("拿真清单量：自动更新挑出来的每一颗本机都已经有 dll（没被顺手装新插件）",
                      strays.isEmpty(),
                      strays.isEmpty()
                          ? QStringLiteral("清单 %1 条 · 挑了 %2 颗（%2=0 时这条在 0 个对象上，不算量到）")
                                .arg(listed.size()).arg(targets.size())
                          : QStringLiteral("越界：%1").arg(strays.join(QStringLiteral(","))));
            }

            page.hide();
            std::printf("storenet: 失败 %d 项\n", storeNetBad);
            std::fflush(stdout);
            app.quit();
        });
    }

    // --storemulti [颗数]：一次点好几颗「装/更新」，然后在下载还没落地的时候把卡片墙拆掉重排 ——
    // 他报的"多点挤了几个插件安装软件就闪退"就是这个形状：卡片是 deleteLater 掉的，而回调连的是
    // 页面（还活着），于是进度/完成回调再碰那颗按钮就是往已删的 QWidget 上写。
    // 要联网，而且会往**运行目录**的 ltool-plugins 里真写 dll，所以必须拿一份临时副本去跑
    // （build/verify/storemulti.ps1 干这件事）；和 --storenet 一样不进 CI。
    int storeMultiBad = 0;
    if (args.contains("--storemulti")) {
        const int howMany = qMax(2, args.mid(args.indexOf(QStringLiteral("--storemulti")) + 1).value(0).toInt());
        QTimer::singleShot(150, &app, [&app, &storeMultiBad, howMany] {
            ToolsPage page;
            page.setAttribute(Qt::WA_DontShowOnScreen, true);
            page.resize(940, 720);
            page.show();
            // drain 必须把 DeferredDelete 也结算掉：processEvents() 默认**不**投递它，
            // 那样旧卡片其实还活着，"回调打到已删控件"这条根本测不出来（第一版就是这么假绿的）。
            const auto drain = [] {
                for (int i = 0; i < 30; ++i) {
                    QCoreApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            };
            const auto check = [&storeMultiBad](const char* name, bool ok, const QString& got) {
                std::printf("%s %s got=[%s]\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
                std::fflush(stdout);
                if (!ok)
                    ++storeMultiBad;
            };
            drain();
            const QString dir = PluginHost::pluginsDir();
            const int dllsBefore = QDir(dir).entryList(Platform::pluginFileGlobs(), QDir::Files).size();
            QPushButton* refresh = page.findChild<QPushButton*>(QStringLiteral("storeRefresh"));
            if (!refresh) {
                std::printf("FAIL 没有「刷新清单」，后面没法跑\n");
                ++storeMultiBad;
                app.quit();
                return;
            }
            refresh->click();
            QString st;
            for (int i = 0; i < 400 && st.isEmpty(); ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(50);
                for (QLabel* l : page.findChildren<QLabel*>())
                    if (l->text().startsWith(QStringLiteral("清单 "))) {
                        st = l->text();
                        break;
                    }
            }
            check("清单拉到了（没有清单就没有「装」可点）", !st.isEmpty(), st.left(60));
            if (st.isEmpty()) {
                std::printf("storemulti: 失败 %d 项\n", storeMultiBad);
                app.quit();
                return;
            }
            drain();
            QList<QPushButton*> installs;
            for (QPushButton* b : page.findChildren<QPushButton*>())
                if (b->isVisible()
                    && (b->text() == QStringLiteral("装") || b->text() == QStringLiteral("更新")))
                    installs << b;
            const int clicked = qMin(howMany, installs.size());
            for (int i = 0; i < clicked; ++i)
                installs[i]->click();   // 一颗接一颗点，中间不歇：他那个"挤"就是这一下
            check("至少点下去两颗（只有一颗就测不到并发）", clicked >= 2,
                  QStringLiteral("可点 %1 / 点了 %2").arg(installs.size()).arg(clicked));
            // 第一颗装完时产品自己就会重排卡片墙；这里不等它，立刻手动拆一次，
            // 把"还在下的那几颗，它们的控件已经没了"这个状态摆出来。
            page.rebuildGrid();
            drain();
            QStringList parts;
            int waited = 0;
            QString lastSig;
            int stableCount = 0;
            for (; waited < 6000; ++waited) {   // 300 秒上限（每格 50ms）
                QCoreApplication::processEvents();
                QThread::msleep(50);
                if (waited % 20 != 0)
                    continue;                   // 每秒看一次
                parts = QDir(dir).entryList({QStringLiteral("*.part")}, QDir::Files);
                QStringList sz;
                for (const QString& f : parts)
                    sz << QStringLiteral("%1=%2KB").arg(f,
                             QString::number(QFileInfo(dir + QLatin1Char('/') + f).size() / 1024));
                const QString sig = sz.join(QLatin1Char(' '));
                if (!sig.isEmpty())
                    std::printf("INFO %3ds 桌面上的 .part：%s\n", waited / 20, sig.toUtf8().constData());
                // "字节数不动"只有在**已经有字节**的时候才说明落地了：刚点开那几秒 .part 是 0KB，
                // 拿 0KB 的三次相同当"稳定"会在下载还没起来时就收工（上一版就是这么把一次
                // 正常排队读成了"留了个 .part 没清"）。
                const bool anyBytes = !sz.isEmpty() && !sz.contains(QStringLiteral("=0KB"));
                const bool stable = (sig == lastSig && !sig.isEmpty() && anyBytes);
                stableCount = stable ? stableCount + 1 : 0;                lastSig = sig;
                // 终止条件看"字节数不再动"而不是".part 消失"—— 装失败的那份就是留着不动的，
                // 拿"消失"当条件会一路等到超时（第一版就这么把一次失败读成了"下载卡死"）。
                if (parts.isEmpty() && waited > 80)
                    break;
                if (stableCount >= 3)
                    break;
            }
            check("跑到底还活着（真闪退的话这几行都打不出来）", true,
                  QStringLiteral("等了 %1 ms").arg(waited * 50));
            check("目录里不留 .part（要么改成名要么删干净）", parts.isEmpty(), parts.join(QStringLiteral("；")));
            const int dllsAfter = QDir(dir).entryList(Platform::pluginFileGlobs(), QDir::Files).size();
            check("dll 颗数没变（更新是换文件，不是加文件）", dllsAfter == dllsBefore,
                  QStringLiteral("点之前 %1 / 之后 %2").arg(dllsBefore).arg(dllsAfter));
            std::printf("storemulti: 失败 %d 项\n", storeMultiBad);
            std::fflush(stdout);
            app.quit();
        });
    }

    // --uninst [插件id]：把"用过那个工具，然后卸载它"这条路整个走一遍 ——
    // 先真把那张页开出来（dll 就此映射进这个进程），再点卡片上那颗「卸载」，然后重排、再切回那张页。
    // 他报的是"一卸载网络插件就崩溃"，所以这条判据要跑到**卸载之后还能碰那张页**为止。
    // 它会真删运行目录里那颗 dll，必须拿临时副本跑：build/verify/storemulti.ps1 -Flag --uninst。
    int uninstBad = 0;
    if (args.contains("--uninst")) {
        const QString id = args.mid(args.indexOf(QStringLiteral("--uninst")) + 1).value(0, QStringLiteral("network"));
        QTimer::singleShot(150, &app, [&app, &uninstBad, id] {
            ToolsPage page;
            page.setAttribute(Qt::WA_DontShowOnScreen, true);
            page.resize(940, 720);
            page.show();
            const auto drain = [] {
                for (int i = 0; i < 30; ++i) {
                    QCoreApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            };
            const auto check = [&uninstBad](const char* name, bool ok, const QString& got) {
                std::printf("%s %s got=[%s]\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
                std::fflush(stdout);
                if (!ok)
                    ++uninstBad;
            };
            // 那张卡的状态标签（applyUninstall 的答复就写在里面）
            const auto stateOf = [&page](const QString& cardName) -> QLabel* {
                for (QFrame* card : page.findChildren<QFrame*>(QStringLiteral("row"))) {
                    bool mine = false;
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->text() == cardName)
                            mine = true;
                    if (!mine)
                        continue;
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->property("shopState").toBool())
                            return l;
                }
                return nullptr;
            };
            drain();
            const QString dir = PluginHost::pluginsDir();
            // 两种情形各来一遍：没打开过 → 文件删得掉 → reload + 重排；打开过 → dll 映射着删不掉
            // → 该排到下次启动。他报的是"一卸载网络插件就崩溃"，两条都得活着走完。
            const QVector<QPair<QString, bool>> cases = { { QStringLiteral("base64"), false }, { id, true } };
            for (const auto& c : cases) {
                const QString cid = c.first;
                const PluginHost::Meta* md = PluginHost::meta(cid);
                check(("[" + cid + "] 元数据里认得出").toUtf8().constData(), md != nullptr, cid);
                if (!md)
                    continue;
                const QString name = md->name;
                const QString file = QFileInfo(md->file).fileName();
                QString err;
                if (c.second) {
                    QWidget* w = PluginHost::page(cid, &page, &err);
                    check(("[" + cid + "] 那张页建得出来（dll 已映射进进程）").toUtf8().constData(),
                          w != nullptr, err);
                    page.showTool(cid);
                    drain();
                    for (QToolButton* b : page.findChildren<QToolButton*>())
                        if (b->text().contains(QStringLiteral("返回")))
                            b->click();
                    drain();
                }
                QLabel* state = stateOf(name);
                check(("[" + cid + "] 那张卡找得到（状态标签在）").toUtf8().constData(), state != nullptr, name);
                // 走的是「卸载」按下并答了"是"之后那同一段代码（ToolsPage::applyUninstall）。
                // 不能靠点按钮：无窗口平台上 question() 立刻返回默认值"不"，那条支路进不去。
                page.applyUninstall(file, name, state);
                drain();
                check(("[" + cid + "] 卸载那一下之后还活着").toUtf8().constData(), true,
                      ("先打开过=" + QString::number(c.second ? 1 : 0)));
                const bool gone = !QFileInfo::exists(dir + QLatin1Char('/') + file);
                const bool queued = PluginStore::pendingRemoves().contains(file);
                check(("[" + cid + "] 要么真删了，要么排到下次启动").toUtf8().constData(), gone || queued,
                      ("文件还在=" + QString::number(gone ? 0 : 1) + " 排了队=" + QString::number(queued ? 1 : 0)));
                page.rebuildGrid();
                drain();
                check(("[" + cid + "] 重排卡片墙之后还活着").toUtf8().constData(), true,
                      ("卡片 " + QString::number(page.findChildren<QFrame*>(QStringLiteral("row")).size()) + " 张"));
                if (c.second) {
                    page.showTool(cid);
                    drain();
                    check(("[" + cid + "] 再切回那张已卸载的页还活着").toUtf8().constData(), true, cid);
                }
            }
            std::printf("uninst: 失败 %d 项\n", uninstBad);
            std::fflush(stdout);
            app.quit();
        });
    }

    // --storeexit：卡片墙 ↔ 那张插件页这条**出口**的判据，全程用真鼠标事件走卡片那颗按钮。
    // 他报的形状是"卸载完插件，商店自己跳到了注册表分析那一页，而且找不到退出的按钮"
    // （2026-10-18 11:16:52 建页 registry → 11:17:24、11:17:35 连点两次卸载，日志里两次都是"还加载着，删不掉"）。
    // 不删文件：离屏平台上那颗确认框立刻返回"不"，走的正是"他按了取消"那一条支路 ——
    // 而这一按同样不该把那张页打开，卡片墙得留在原地。
    int storeExitBad = 0;
    if (args.contains("--storeexit")) {
        QTimer::singleShot(150, &app, [&app, &storeExitBad] {
            // 先把这一面钉成"离线、清单已拉到"：不这么做，ToolsPage 构造函数会自己去拉清单
            // （沙箱里没有缓存、lastFetchDate 也是空的），CI 上就变成一条看网络脸色的判据。
            // 那份假条目只借"清单里有 registry 这一条"这个形状，哈希故意不对 —— 卡片于是落在
            // 「与商店不同」，那颗「卸载」照样在（这一面不点「换成商店那份」，不会下载任何东西）。
            {
                QString zeros(64, QLatin1Char('0'));
                const QString idx = QStringLiteral(
                                        "{\"schema\":1,\"plugins\":[{\"id\":\"registry\",\"name\":\"注册表分析\","
                                        "\"desc\":\"\",\"file\":\"ltool-registry.dll\","
                                        "\"url\":\"https://example.invalid/ltool-registry.dll\",\"sha256\":\"%1\","
                                        "\"bytes\":4096,\"api\":%2,\"version\":\"0.0.1\"}]})")
                                        .arg(zeros).arg(LTOOL_PLUGIN_API_VERSION);
                QDir().mkpath(Utils::dataDir());
                QFile f(Utils::dataDir() + QStringLiteral("/plugins_cache.json"));
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    f.write(idx.toUtf8());
                QSettings cfg(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat);
                cfg.setValue(QStringLiteral("store/lastFetchDate"),
                             QDate::currentDate().toString(Qt::ISODate));
                cfg.sync();
            }
            ToolsPage page;
            page.setAttribute(Qt::WA_DontShowOnScreen, true);
            page.resize(940, 720);
            page.show();
            const auto drain = [] {
                for (int i = 0; i < 30; ++i) {
                    QCoreApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            };
            const auto check = [&storeExitBad](const char* name, bool ok, const QString& got) {
                std::printf("%s %s got=[%s]\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
                std::fflush(stdout);
                if (!ok)
                    ++storeExitBad;
            };
            // 这一面全是"某个串等不等"的判据，包一层免得每处自己拼 == 又把两个数印反
            const auto eq = [&check](const char* name, const QString& got, const QString& want) {
                check(name, got == want, got + "  want=[" + want + "]");
            };
            drain();
            const auto backBtn = [&page] {
                for (QToolButton* b : page.findChildren<QToolButton*>())
                    if (b->text().contains(QStringLiteral("返回")))
                        return b;
                return static_cast<QToolButton*>(nullptr);
            };
            const auto cardNamed = [&page](const QString& name) {
                for (QFrame* card : page.findChildren<QFrame*>(QStringLiteral("row")))
                    for (QLabel* l : card->findChildren<QLabel*>())
                        if (l->text() == name)
                            return card;
                return static_cast<QFrame*>(nullptr);
            };
            const auto btnOn = [](QFrame* card, const QString& text) {
                if (!card)
                    return static_cast<QPushButton*>(nullptr);
                for (QPushButton* b : card->findChildren<QPushButton*>())
                    if (b->text() == text)
                        return b;
                return static_cast<QPushButton*>(nullptr);
            };
            // 一次完整的按下 + 抬起，发给那颗控件本身 —— QApplication::notify 里"事件没被接受就
            // 往父控件冒泡"那一段就是这么走的，卡片正文的点击正是父控件收抬起。
            const auto click = [](QWidget* w) {
                const QPoint p = w->rect().center();
                const QPoint g = w->mapToGlobal(p);
                QMouseEvent pr(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton,
                               Qt::NoModifier);
                QMouseEvent rl(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton,
                               Qt::NoModifier);
                QApplication::sendEvent(w, &pr);
                QApplication::sendEvent(w, &rl);
            };
            auto* stack = page.findChild<QStackedWidget*>();
            // 离屏里 question() 不是"立刻返回默认值"，它是自己开一轮嵌套事件循环等一个永远不会来的
            // 答复 —— 实测整条判据挂在那儿（timeout 60 杀掉，rc=124）。所以按下之前先架一颗定时器，
            // 等那颗框冒出来就**用真鼠标事件点它那颗按钮**（不是 done(which)：那样走的不是他那条路，
            // "嵌套循环套在鼠标抬起处理里"这个形状就量不到），这样外层那颗按钮的抬起才可能出问题。
            const auto armClick = [](QMessageBox::StandardButton which) {
                auto* t = new QTimer(qApp);
                QObject::connect(t, &QTimer::timeout, qApp, [t, which] {
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        auto* mb = qobject_cast<QMessageBox*>(w);
                        if (!mb || !mb->isVisible())
                            continue;
                        if (QAbstractButton* pb = mb->button(which)) {
                            const QPoint p = pb->rect().center();
                            const QPoint g = pb->mapToGlobal(p);
                            QMouseEvent pr(QEvent::MouseButtonPress, p, g, Qt::LeftButton,
                                           Qt::LeftButton, Qt::NoModifier);
                            QMouseEvent rl(QEvent::MouseButtonRelease, p, g, Qt::LeftButton,
                                           Qt::NoButton, Qt::NoModifier);
                            QApplication::sendEvent(pb, &pr);
                            QApplication::sendEvent(pb, &rl);
                        }
                        t->stop();
                        t->deleteLater();
                        return;
                }
                });
                t->start(10);
            };
            const auto where = [&stack, &backBtn] {
                QWidget* cur = stack ? stack->currentWidget() : nullptr;
                return QStringLiteral("index=%1 类=%2 返回可见=%3")
                    .arg(stack ? stack->currentIndex() : -9)
                    .arg(cur ? QString::fromLatin1(cur->metaObject()->className())
                             : QStringLiteral("无"))
                    .arg(backBtn() && backBtn()->isVisible() ? 1 : 0);
            };
            eq("起点：那一面卡片墙在前，返回那颗藏着", where(),
               "index=0 类=QScrollArea 返回可见=0");

            QPointer<QFrame> card = cardNamed(QStringLiteral("注册表分析"));
            check("那张「注册表分析」的卡画出来了", card != nullptr,
                  card ? QStringLiteral("yes") : QStringLiteral("no"));
            QStringList btnTexts;
            if (card)
                for (QPushButton* b : card->findChildren<QPushButton*>())
                    btnTexts << b->text();
            std::printf("INFO 那张卡上的按钮：%s\n", btnTexts.join(QStringLiteral(" / ")).toUtf8().constData());

            if (card) {
                // ① 点正文 = 点开那张页，出口那颗必须跟着出来
                click(card);
                drain();
                card = cardNamed(QStringLiteral("注册表分析"));   // 清单回包会重排卡片墙，指针得现拿
                check("点正文之后：页在前、返回那颗出来了",
                      where().contains(QStringLiteral("返回可见=1"))
                          && !where().contains(QStringLiteral("类=QScrollArea")),
                      where());
                // ② 按返回 = 回到卡片墙
                if (backBtn())
                    backBtn()->click();
                drain();
                eq("按返回之后：回到卡片墙、返回那颗收起来", where(),
                   "index=0 类=QScrollArea 返回可见=0");
                // ③ 按「卸载」那颗，确认框答"不"：这一按只该在卡片上写一句"没动。"，
                //     不该把那张页翻出来 —— 抬起冒泡到卡片正文就是"卸载完自动跳到注册表分析"那个形状
                if (QPushButton* ub = btnOn(card, QStringLiteral("卸载"))) {
                    armClick(QMessageBox::No);
                    click(ub);
                    drain();
                    eq("按「卸载」+ 答「不」之后：还在卡片墙（页面没被这一按翻出来）", where(),
                       QStringLiteral("index=0 类=QScrollArea 返回可见=0"));
                    QLabel* st = nullptr;
                    if (QFrame* again = cardNamed(QStringLiteral("注册表分析")))
                        for (QLabel* l : again->findChildren<QLabel*>())
                            if (l->property("shopState").toBool())
                                st = l;
                    check("答「不」之后卡片上留了话", st && !st->text().isEmpty(),
                          st ? st->text() : QStringLiteral("那张卡找不到了"));
                } else {
                    check("按「卸载」那颗之后：还在卡片墙（页面没被这一按翻出来）", false,
                          "那张卡上没有「卸载」那颗，这条没量到");
                }
                // ④ 答"是"那一段。确认框在离屏这一按里答不出"是"（实测：done(Yes) 之后 exec() 立刻返回，
                //     那颗 singleShot(0) 反而没轮到跑），所以按这个仓库一直的规矩走：把"答了是"那一段
                //     抽出来的方法直接调 —— 界面那条支路调的就是这一颗，不是抄的第二遍。
                //     这颗的 dll 在第①步已经被映射进进程，删不动 → PluginStore 记进待删表
                //     （写的是沙箱里的 config，碰不到他真实的 %APPDATA%）。
                //     要量的是：重排之后那张卡**还说不说得出真话**（文件明明还在盘上）。
                const PluginHost::Meta* md = PluginHost::meta(QStringLiteral("registry"));
                if (md) {
                    page.applyUninstall(QFileInfo(md->file).fileName(), md->name, nullptr);
                    drain();
                    eq("答「是」之后：还在卡片墙", where(),
                       QStringLiteral("index=0 类=QScrollArea 返回可见=0"));
                    QString got;
                    if (QFrame* again = cardNamed(QStringLiteral("注册表分析")))
                        for (QLabel* l : again->findChildren<QLabel*>())
                            if (l->property("shopState").toBool())
                                got = l->text();
                    check("那张卡说出了「已经安排卸载，下次启动才真删」",
                          got.contains(QStringLiteral("下次启动")), got);
                } else {
                    check("答「是」那一段（本机得有 registry 那颗 dll 可卸）", false,
                          QStringLiteral("宿主认不出 registry 这颗"));
                }
                // ⑥ 他给的稳定复现（原话）："先打开插件A，返回插件页面，卸载插件B，
                //     插件页面自动显示插件A内容，然后内存爆炸卡死"。
                //     A=注册表分析（先点开、再返回），B=Base64（也点开过一次 → dll 映射进进程，
                //     于是卸它走的是"排进待删表"那一条：**这一面判据一颗 dll 都不真删**，CI 能跑）。
                //     量的三件事：卡片墙还在原地、A 那张页没被闪出来、内存没涨。
                page.showTool(QStringLiteral("registry"));
                drain();
                page.showTool(QStringLiteral("base64"));
                drain();
                if (backBtn())
                    backBtn()->click();
                drain();
                {
                    const PluginHost::Meta* b = PluginHost::meta(QStringLiteral("base64"));
                    check("B（Base64）这颗在目录里", b != nullptr, b ? b->file : QStringLiteral("无"));
                    if (b) {
                        // A 那张页还缓存着。给它挂一颗 Show 计数器：
                        // rebuildGrid 旧顺序是"先把卡片那格摘掉、再插回去"，摘掉那一瞬 QStackedLayout
                        // 会把后面的格子顶成当前 —— 于是**每次重排都把 A 那张页闪出来一次**（showEvent）。
                        // 他看到的"自动显示插件A内容"就是这个形状。
                        QString perr;
                        QWidget* pageA = PluginHost::page(QStringLiteral("registry"), &page, &perr);
                        class ShowCounter : public QObject {
                        public:
                            explicit ShowCounter(QObject* p) : QObject(p) {}
                            int n = 0;
                        protected:
                            bool eventFilter(QObject*, QEvent* e) override
                            {
                                if (e->type() == QEvent::Show)
                                    ++n;
                                return false;
                            }
                        };
                        auto* counter = new ShowCounter(&page);
                        if (pageA)
                            pageA->installEventFilter(counter);
                        const int wsBefore = Platform::workingSetMB();
                        QFrame* cardB = nullptr;
                        for (QFrame* c : page.findChildren<QFrame*>(QStringLiteral("row")))
                            for (QLabel* l : c->findChildren<QLabel*>())
                                if (l->text() == b->name)
                                    cardB = c;
                        if (QPushButton* ub = btnOn(cardB, QStringLiteral("卸载"))) {
                            armClick(QMessageBox::Yes);
                            click(ub);
                            drain();
                        } else {
                            // 没这颗就一行都没量到 —— 报红点名，别让它变成"计数器是 0 所以过了"
                            check("B 那张卡上有「卸载」那颗", false,
                                  QStringLiteral("cardB=%1").arg(cardB ? QStringLiteral("找到了卡")
                                                                       : QStringLiteral("找不到那张卡")));
                        }
                        const int wsAfter = Platform::workingSetMB();
                        std::printf("INFO 卸掉 B 之后：工作集 %d→%d MB，stack 见下\n", wsBefore, wsAfter);
                        QString cells;
                        for (int i = 0; i < stack->count(); ++i)
                            cells += QStringLiteral(" [%1]%2").arg(i).arg(
                                QString::fromLatin1(stack->widget(i)->metaObject()->className()));
                        std::printf("INFO stack：%s\n", cells.toUtf8().constData());
                        eq("卸掉另一颗 B 之后：还在卡片墙（A 那张页没被翻出来）", where(),
                           QStringLiteral("index=0 类=QScrollArea 返回可见=0"));
                        check("这一按没把内存整炸（工作集涨幅 < 200MB）", wsAfter - wsBefore < 200,
                              QStringLiteral("%1→%2 MB").arg(wsBefore).arg(wsAfter));
                        // 这条才是"自动显示插件A内容"的量化：重排那一下 A 那张页被 Show 了几次。
                        check("重排那一下没把 A 那张页闪出来（Show 次数=0）", counter->n == 0,
                              QStringLiteral("Show %1 次").arg(counter->n));
                    }
                }
                // ⑤ 那张页的出口。他原话是"找不到退出按钮" —— 一颗 autoRaise 的小箭头
                //     （"‹ 返回商店"）在深色页眉里就是找不着，所以量两条：实心、并且 Esc 也能回去。
                if (QPushButton* ub = btnOn(cardNamed(QStringLiteral("注册表分析")),
                                            QStringLiteral("卸载")))
                    std::printf("INFO 卸载那颗还在（答是之后卡片重排过）：%s\n",
                                ub->text().toUtf8().constData());
                page.showTool(QStringLiteral("registry"));
                drain();
                {
                    QToolButton* b = backBtn();
                    check("出口那颗是实心的（不是 autoRaise 的淡箭头）", b && !b->autoRaise(),
                          b ? (b->autoRaise() ? QStringLiteral("autoRaise") : QStringLiteral("实心"))
                            : QStringLiteral("找不到那颗"));
                    const QString before = where();
                    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(&page, &esc);
                    drain();
                    check("在那张页上按 Esc = 回卡片墙", where() != before && where().contains(QStringLiteral("类=QScrollArea")),
                          before + QStringLiteral(" -> ") + where());
                }
            }
            std::printf("storeexit: 失败 %d 项\n", storeExitBad);
            std::fflush(stdout);
            app.quit();
        });
    }

    QObject::connect(&win, &SearchWindow::hotkeyChanged, &win, applyHotkeyChange);    const int rc = app.exec();
    if (args.contains("--toggle"))
        return toggleBad;
    if (args.contains("--capgeo"))
        return capGeoBad;
    if (args.contains("--capflash"))
        return capFlashBad;
    if (args.contains("--storenet"))
        return storeNetBad;
    if (args.contains("--storeexit"))
        return storeExitBad;
    return rc;
}
