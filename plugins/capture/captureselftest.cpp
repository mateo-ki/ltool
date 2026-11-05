// ltool 插件 plugins/capture 的判据本体
//
// 这 12 面旗标原来写在 src/main.cpp 里。截图那三个窗口（CaptureOverlay / PinWindow / Anno）
// 和 OCR 那一族搬进这个 dll 之后，主程序链接不到它们内部的函数，只能由这里 extern "C" 导出、
// 主程序按名字点名要（见 src/main.cpp 的 runPluginSelftest；符号找不到 = 判失败，空输出 = 判失败）。
// 搬的时候一个字没改：PASS/FAIL/INFO 那些行、"<旗标>: 失败 N 项"那一行、返回的那个 N，
// 和原来在主程序里跑出来的一模一样。
//
// 形状分三种：
//   int ()              —— snaprect / pintool / ocrget / capgeo
//   int (const char*)   —— 收一个图片路径：ocrtest / ocrlayout / pinpick / ocrchain /
//                          pindel / ocrpanel / pinshot
//   int (int)           —— capflash 的"开合几次"
#include "CaptureHistory.h"
#include "CaptureOverlay.h"
#include "Ocr.h"
#include "OcrBundle.h"
#include "OcrPanel.h"
#include "OcrPp.h"
#include "PinWindow.h"
#include "Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>

// --snaprect：F1 悬停吸附出来的选区到底贴不贴边（无界面、不开浮层）。
// 三条判据：① 吸附框必须落在 GetWindowRect 之内 —— 那条里外多出来的圈就是这次的病；
// ② 完整落在显示器内的窗口上，吸附框必须和 DWM 的可见边界**一模一样**；
// ③ 至少要有一个窗口吸附框比 GetWindowRect 紧 —— 这台机器上确实存在那圈看不见的边框，
//    不然 ① 是空转（两个矩形本来就相等时，什么实现都能"通过"）。
extern "C" Q_DECL_EXPORT int ltoolSnaprectRun()
{
    int checked = 0, fail = 0, tighter = 0;
    for (HWND h = GetWindow(GetDesktopWindow(), GW_CHILD); h && checked < 12;
         h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || GetWindowTextLengthW(h) <= 0)
            continue;
        RECT raw{};
        if (!GetWindowRect(h, &raw) || raw.right - raw.left < 60 || raw.bottom - raw.top < 40)
            continue;
        RECT vis{};
        const bool haveVis =
            SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))
            && vis.right > vis.left && vis.bottom > vis.top;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const bool fullyOn =
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)
            && raw.left >= mi.rcMonitor.left && raw.top >= mi.rcMonitor.top
            && raw.right <= mi.rcMonitor.right && raw.bottom <= mi.rcMonitor.bottom;
        const QRect own = CaptureOverlay::snapRectFor(reinterpret_cast<WId>(h));
        const QRect rRaw(raw.left, raw.top, raw.right - raw.left, raw.bottom - raw.top);
        const QRect rVis = haveVis ? QRect(vis.left, vis.top, vis.right - vis.left, vis.bottom - vis.top)
                                   : QRect();
        ++checked;
        wchar_t name[64] = L"";
        GetWindowTextW(h, name, 64);
        const bool inside = !own.isNull()
                            && own.left() >= rRaw.left() && own.top() >= rRaw.top()
                            && own.right() <= rRaw.right() && own.bottom() <= rRaw.bottom();
        const bool sameVisible = !haveVis || !fullyOn || own == rVis;
        if (own != rRaw)
            ++tighter;
        if (!inside || !sameVisible)
            ++fail;
        std::printf("%s %-24s 原始=%dx%d 可见=%s 吸附=%dx%d 四周去掉的余量 左%d 上%d 右%d 下%d\n",
                    (!inside || !sameVisible) ? "FAIL" : "ok  ",
                    QString::fromWCharArray(name).left(20).toUtf8().constData(),
                    rRaw.width(), rRaw.height(),
                    haveVis ? QString("%1x%2").arg(rVis.width()).arg(rVis.height()).toUtf8().constData()
                            : QString("拿不到").toUtf8().constData(),
                    own.width(), own.height(),
                    own.left() - rRaw.left(), own.top() - rRaw.top(),
                    rRaw.right() - own.right(), rRaw.bottom() - own.bottom());
    }
    std::printf("snaprect: 验了 %d 个窗口，失败 %d 项；吸附框比原始矩形紧的 %d 个", checked, fail, tighter);
    if (tighter == 0) {
        std::printf("  ← 一个都没有，这条测试什么都没测到，判失败");
        ++fail;
    }
    std::printf("\n");
    std::fflush(stdout);
    return fail;
}


// --ocrtest <图片>：无界面跑一遍"图里取字"，把识别结果原样打出来，用来验准确率
extern "C" Q_DECL_EXPORT int ltoolOcrtestRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    QImage img(target);
    if (img.isNull()) {
        std::printf("ocrtest: 读不了这张图 %s\n", target.toUtf8().constData());
        std::fflush(stdout);
        return 2;
    }
    QElapsedTimer t;
    t.start();
    QString err;
    const QString text = Ocr::recognize(img, &err);
    std::printf("ocrtest: ms=%lld chars=%d err=%s\n--- 识别结果 ---\n%s\n",
                t.elapsed(), int(text.size()), err.toUtf8().constData(), text.toUtf8().constData());
    std::fflush(stdout);
    return err.isEmpty() ? 0 : 1;
}


// --ocrlayout <图片>：把"带位置的识别"原样打出来（行框/词框/切出来的字框），
// 划词全靠这些坐标，所以拿一张字在已知位置的图来对一下偏了多少
extern "C" Q_DECL_EXPORT int ltoolOcrlayoutRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    QImage img(target);
    if (img.isNull()) {
        std::printf("ocrlayout: 读不了这张图 %s\n", target.toUtf8().constData());
        std::fflush(stdout);
        return 2;
    }
    QElapsedTimer t;
    t.start();
    QString err;
    const QVector<Ocr::Line> lines = Ocr::recognizeLayout(img, &err);
    std::printf("ocrlayout: img=%dx%d lines=%d ms=%lld err=%s\n",
                img.width(), img.height(), int(lines.size()), t.elapsed(),
                err.toUtf8().constData());
    for (int i = 0; i < lines.size(); ++i) {
        const Ocr::Line& l = lines.at(i);
        std::printf("  L%d rect=(%d,%d %dx%d) words=%d text=%s\n", i, l.rect.x(), l.rect.y(),
                    l.rect.width(), l.rect.height(), int(l.words.size()),
                    l.text.toUtf8().constData());
        for (int w = 0; w < l.words.size() && w < 6; ++w) {
            const Ocr::Word& wd = l.words.at(w);
            std::printf("      W%d (%d,%d %dx%d) \"%s\"\n", w, wd.rect.x(), wd.rect.y(),
                        wd.rect.width(), wd.rect.height(), wd.text.toUtf8().constData());
        }
        if (i < 2) {
            const QVector<Ocr::Cell> cs = Ocr::cells(l);
            QString boxes;
            for (const Ocr::Cell& c : cs)
                boxes += QString("[%1,%2 %3x%4]%5 ").arg(c.rect.x()).arg(c.rect.y())
                                   .arg(c.rect.width()).arg(c.rect.height()).arg(c.ch);
            std::printf("      cells=%d %s\n", int(cs.size()), boxes.toUtf8().constData());
        }
    }
    std::fflush(stdout);
    return err.isEmpty() ? 0 : 1;
}


// --pinpick <图片>：不碰桌面，把鼠标事件直接送给置顶窗，验"图上划词"整条链
// （按下→拖→松手→剪贴板）。合成鼠标会抢用户的键鼠，所以这条链用送事件的方式测。
extern "C" Q_DECL_EXPORT int ltoolPinpickRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    QImage img(target);
    if (img.isNull()) {
        std::printf("pinpick: 读不了这张图 %s\n", target.toUtf8().constData());
        std::fflush(stdout);
        return 2;
    }
    auto* pw = new PinWindow(QPixmap::fromImage(img), QString(), false);
    // 这条自测要往剪贴板写东西，先把用户原有的内容存下来，跑完归还
    auto* savedClip = new QMimeData();
    if (const QMimeData* md = QGuiApplication::clipboard()->mimeData())
        for (const QString& fmt : md->formats())
            savedClip->setData(fmt, md->data(fmt));
    // 同步认完字并把划词模式开着（真按钮走的是后台线程，结果一样）
    pw->pickProbe(QRect(100000, 100000, 1, 1));
    QClipboard* clip = QGuiApplication::clipboard();
    const auto clipWrite = [clip](const QString& v) {
        for (int i = 0; i < 10; ++i) {
            clip->setText(v);
            QCoreApplication::processEvents();
            if (clip->text() == v)
                return;
            QThread::msleep(50);
        }
    };
    const auto clipRead = [clip]() {
        for (int i = 0; i < 10; ++i) {
            const QString v = clip->text();
            if (!v.isEmpty())
                return v;
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        return clip->text();
    };
    const auto send = [pw](QEvent::Type type, const QPoint& imgPos, Qt::MouseButton btn,
                           Qt::KeyboardModifiers mod = Qt::NoModifier, int key = 0) {
        const QPoint wp = pw->toWidgetRect(QRect(imgPos, QSize(1, 1))).topLeft();
        const QPoint gp = pw->mapToGlobal(wp);
        if (key) {
            QEvent* ev = type == QEvent::KeyPress
                             ? static_cast<QEvent*>(new QKeyEvent(QEvent::KeyPress, key, mod))
                             : static_cast<QEvent*>(new QKeyEvent(QEvent::KeyRelease, key, mod));
            QApplication::sendEvent(pw, ev);
            delete ev;
            return;
        }
        QEvent* ev = type == QEvent::MouseButtonDblClick
                         ? static_cast<QEvent*>(new QMouseEvent(type, QPointF(wp), QPointF(gp), btn,
                                                                btn, mod))
                         : type == QEvent::MouseMove
                         ? static_cast<QEvent*>(new QMouseEvent(type, QPointF(wp), QPointF(gp), btn,
                                                                Qt::LeftButton, mod))
                         : static_cast<QEvent*>(new QMouseEvent(type, QPointF(wp), QPointF(gp), btn,
                                                                type == QEvent::MouseButtonPress ? btn
                                                                                                : Qt::NoButton,
                                                                mod));
        QApplication::sendEvent(pw, ev);
        delete ev;
    };
    const auto drag = [&](const QPoint& from, const QPoint& to) {
        clipWrite(QStringLiteral("SENTINEL"));
        send(QEvent::MouseButtonPress, from, Qt::LeftButton);
        for (int i = 1; i <= 5; ++i)
            send(QEvent::MouseMove, QPoint(from.x() + (to.x() - from.x()) * i / 5,
                                           from.y() + (to.y() - from.y()) * i / 5),
                 Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton);
        return clipRead();
    };
    int bad = 0;
    const auto expect = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    // 英文词那两个用例按"引擎自己报的词框"定坐标：写死像素的话，一换引擎框的左沿就挪，
    // 测出来的"故障"其实是坐标对不上（PP-OCRv4 给的 Search 框从 43 起，WinRT 从 45 起，
    // 而选中的判定是字的中心要落在拖动带内，1 个像素就能差出半个字母）
    QRect searchBox;
    for (const Ocr::Line& l : Ocr::recognizeLayout(img))
        for (const Ocr::Word& w : l.words)
            if (w.text == QLatin1String("Search"))
                searchBox = w.rect;
    std::printf("INFO Search 词框=%d,%d %dx%d\n", searchBox.left(), searchBox.top(),
                searchBox.width(), searchBox.height());
    expect("拖半行只选带内的字", drag(QPoint(60, 35), QPoint(140, 35)),
           QString::fromUtf8("件安装"));
    expect("单击选整词", drag(QPoint(143, 35), QPoint(144, 36)), QString::fromUtf8("路"));
    expect("罩住两行=两行全文", drag(QPoint(0, 20), QPoint(200, 100)),
           QString::fromUtf8("软件安装路径\n默认搜索引擎"));
    expect("单击英文词=整词", drag(searchBox.center(), searchBox.center() + QPoint(1, 1)),
           QString::fromUtf8("Search"));
    // 词内拆到字：从词框左沿起，拖到六分之五处停手，就该只要前 5 个字母（这是这次选的那档粒度）
    expect("词内只拖到一半=半个词",
           drag(QPoint(searchBox.left() + 1, searchBox.center().y()),
                QPoint(searchBox.left() + searchBox.width() * 5 / 6, searchBox.center().y())),
           QString::fromUtf8("Searc"));
    expect("一行里跨词要补空格", drag(QPoint(45, 130), QPoint(240, 140)),
           QString::fromUtf8("Search Engine 123"));
    expect("整张图都能框到", drag(QPoint(0, 0), QPoint(639, 179)),
           QString::fromUtf8("软件安装路径\n默认搜索引擎\nSearch Engine 123"));
    // Enter = 复制全部
    clipWrite(QStringLiteral("SENTINEL"));
    send(QEvent::KeyPress, QPoint(), Qt::NoButton, Qt::NoModifier, Qt::Key_Return);
    send(QEvent::KeyRelease, QPoint(), Qt::NoButton, Qt::NoModifier, Qt::Key_Return);
    expect("Enter 复制全部", clipRead(),
           QString::fromUtf8("软件安装路径\n默认搜索引擎\nSearch Engine 123"));
    // Esc 只退出划词：退出后再拖一下，不该再往剪贴板写东西
    send(QEvent::KeyPress, QPoint(), Qt::NoButton, Qt::NoModifier, Qt::Key_Escape);
    send(QEvent::KeyRelease, QPoint(), Qt::NoButton, Qt::NoModifier, Qt::Key_Escape);
    clipWrite(QStringLiteral("SENTINEL"));
    const QString afterEsc = drag(QPoint(60, 35), QPoint(140, 35));
    expect("Esc 之后不再复制", afterEsc, QStringLiteral("SENTINEL"));
    std::printf("pinpick: 失败 %d 项\n", bad);
    std::fflush(stdout);
    QGuiApplication::clipboard()->setMimeData(savedClip);   // 归还用户的剪贴板
    delete pw;
    return bad == 0 ? 0 : 1;
}


// --ocrchain <图片>：从"一个文件都没下"开始，验 下载 → 就绪 → 起引擎 → 认字 这条整链。
// APPDATA 已经挪到 %TEMP%\ltool-ocrchain（见 main() 开头），所以这条会真下 26MB、真往沙箱里写，
// 碰不到用户 %APPDATA%\ltool\ocr 那一份。它跑一次半分钟量级，是这套里最全的一条回归。
extern "C" Q_DECL_EXPORT int ltoolOcrchainRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    const QImage img(target);
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    check("沙箱是干净的（引擎不在）", OcrBundle::status().present ? "yes" : "no", "no");
    QString why;
    check("available 说的是要下载",
          (!Ocr::available(&why) && why.contains(QStringLiteral("下载"))) ? "yes" : "no", "yes");
    QString lerr;
    check("没引擎时 OcrPp 不硬跑", OcrPp::load(&lerr) ? "yes" : "no", "no");

    QObject ctx;
    QVector<int> marks;
    bool done = false, okFetch = false, secondDone = false, secondOk = true;
    QString ferr, secondErr;
    OcrBundle::fetchLater(&ctx, [&marks](int pct, const QString&) { marks.push_back(pct); },
                          [&](bool ok, const QString& err, const QString&) {
                              done = true;
                              okFetch = ok;
                              ferr = err;
                          });
    OcrBundle::fetchLater(&ctx, nullptr, [&](bool ok, const QString& err, const QString&) {
        secondDone = true;
        secondOk = ok;
        secondErr = err;
    });
    QElapsedTimer wall;
    wall.start();
    while (!done && wall.elapsed() < 300000) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    check("下载中再要一次不会重开一个",
          (secondDone && !secondOk && secondErr.contains(QStringLiteral("正在下载"))) ? "yes" : "no",
          "yes");
    check("下载跑完了", done ? "yes" : "no", "yes");
    check("下载成功", okFetch ? "yes" : "no", "yes");
    std::printf("INFO 进度报了 %d 次，err=%s，用了 %llds\n", int(marks.size()),
                ferr.toUtf8().constData(), long long(wall.elapsed() / 1000));
    int regress = 0;
    for (int i = 1; i < marks.size(); ++i) {
        if (marks[i] < marks[i - 1])
            ++regress;
    }
    check("进度只朝前（回退会被当成坏了）", regress == 0 ? "yes" : "no", "yes");
    check("进度最后到 100%",
          marks.isEmpty() ? QStringLiteral("none") : QString::number(marks.last()),
          QStringLiteral("100"));
    check("下完的文件齐了", OcrBundle::status().present ? "yes" : "no", "yes");
    QString rerr;
    const QString text = Ocr::recognize(img, &rerr);
    std::printf("INFO 认出来：[%s] err=%s\n", text.toUtf8().constData(),
                rerr.toUtf8().constData());
    check("下完直接就能认字", text,
          QString::fromUtf8("软件安装路径\n默认搜索引擎\nSearch Engine 123"));
    std::printf("ocrchain: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad == 0 ? 0 : 1;
}


// --pintool：离屏验置顶窗那两个"收起"开关（不显示窗口、不在你桌面上闪）。
// 核心断言只有一件事：收起工具栏 / 收起往期带之后，**图区的尺寸必须一模一样**，
// 变的是窗口尺寸；反过来放回去要回到原值。APPDATA 挪到临时目录（会往 captures 写一张）。
extern "C" Q_DECL_EXPORT int ltoolPintoolRun()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    QPixmap pm(320, 200);
    pm.fill(QColor(90, 140, 200));
    const QString key = CaptureHistory::add(pm);
    check("先造一张往期", key.isEmpty() ? "empty" : "ok", "ok");
    const auto rectStr = [](const QRect& r) {
        return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    };
    const auto sizeStr = [](const QSize& s) { return QStringLiteral("%1x%2").arg(s.width()).arg(s.height()); };
    PinWindow pw(pm, key, false);
    // 默认就是"两样都收起"：刚截完看到的应该是一张干净的图。
    check("默认两样都收起", (pw.barHidden() && pw.stripHidden()) ? "yes" : "no", "yes");
    check("默认收起时点左上角不命中缩略图", QString::number(pw.probeThumbAt(QPoint(10, 10))), "-1");
    const QSize hidden0 = pw.size();
    const QString shots = QDir::tempPath() + "/ltool-pintool";
    QDir().mkpath(shots);
    pw.grab().save(shots + "/c_default.png");   // 用户每次截完图看到的就是这张
    // 从默认状态把两样放出来。以前默认就是展开，这条路径不存在；现在它是用户第一次
    // 右键「显示工具栏」走的那条 —— "展开时该多大"必须在建窗时就记好，不然回不去。
    pw.setBarHidden(false);
    pw.setStripHidden(false);
    const QRect r0 = pw.probeImageRect();
    const QSize s0 = pw.size();
    const int strip0 = pw.stripW();      // 用实例问出来的宽度，不去碰私有常量
    const int bar0 = pw.barH();
    std::printf("INFO 初始：窗口 %s，图区 %s，带宽 %d，栏高 %d\n",
                sizeStr(s0).toUtf8().constData(), rectStr(r0).toUtf8().constData(), strip0, bar0);
    check("从默认收起放出两样：图区就是那张图", sizeStr(r0.size()), QStringLiteral("320x200"));
    check("从默认收起放出两样：高出一个栏高",
          QString::number(s0.height() - hidden0.height()), QString::number(bar0));
    check("现在两样都开着", (pw.barHidden() || pw.stripHidden()) ? "yes" : "no", "no");

    pw.setBarHidden(true);
    check("收起工具栏：窗口矮了一个栏高", QString::number(pw.size().height()),
          QString::number(s0.height() - bar0));
    check("收起工具栏：图区尺寸没变", sizeStr(pw.probeImageRect().size()), sizeStr(r0.size()));
    check("收起工具栏：窗口抱住图，不留左右黑边", QString::number(pw.size().width()),
          QString::number(strip0 + r0.width()));
    pw.setBarHidden(false);
    check("放出工具栏：窗口回到原尺寸", sizeStr(pw.size()), sizeStr(s0));
    check("放出工具栏：图区还是那个图区", rectStr(pw.probeImageRect()), rectStr(r0));

    check("收起之前点左上角会命中第 0 张", QString::number(pw.probeThumbAt(QPoint(10, 10))), "0");
    pw.setStripHidden(true);
    // 注意这里不能要求"正好窄 76"：窗口的最小宽度是工具栏那一排按钮撑出来的
    //（这张 320px 宽的图，窗口最小就要 612），收起往期带时缩不到最小值以下。
    // 正确的不变量是：不宽于原来、不低于最小值、图区尺寸一点不动。
    check("收起往期带：窗口不宽于原来", pw.size().width() <= s0.width() ? "yes" : "no", "yes");
    check("收起往期带：不低于最小宽度",
          pw.size().width() >= pw.minimumWidth() ? "yes" : "no", "yes");
    check("收起往期带：图区尺寸没变", sizeStr(pw.probeImageRect().size()), sizeStr(r0.size()));
    check("收起往期带：点左上角不再命中缩略图", QString::number(pw.probeThumbAt(QPoint(10, 10))),
          "-1");
    pw.setStripHidden(false);
    check("放出往期带：窗口回到原尺寸", sizeStr(pw.size()), sizeStr(s0));
    check("放出往期带：又命中第 0 张", QString::number(pw.probeThumbAt(QPoint(10, 10))), "0");

    // ---- 拖边缩放：图的左上角不许漂 ----
    // 他报的是"缩放大小拖动的时候位置不要变"。窗口那条边按住的点当然跟着鼠标走，
    // 但**图本身**不该跟着重新居中一遍：以前 imageRect() 把图居中在客户区里，
    // 于是拖右下角时长宽各分掉一点余量，图就往右下漂 —— 看着像"一缩放就跑位"。
    const QPoint winTL = pw.geometry().topLeft();
    const QPoint imgTL = pw.probeImageRect().topLeft();
    const QSize imgSz = pw.probeImageRect().size();
    const auto sendAt = [&pw](QEvent::Type type, const QPoint& wp) {
        const QPoint gp = pw.mapToGlobal(wp);
        const Qt::MouseButton btn = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent ev(type, QPointF(wp), QPointF(gp), Qt::LeftButton,
                       type == QEvent::MouseMove ? Qt::LeftButton : btn, Qt::NoModifier);
        QApplication::sendEvent(&pw, &ev);
    };
    const QPoint corner(pw.width() - 3, pw.height() - 3);   // 右下角 8px 内 = 右 + 下两条边
    sendAt(QEvent::MouseButtonPress, corner);
    for (int i = 1; i <= 4; ++i)
        sendAt(QEvent::MouseMove, corner + QPoint(15 * i, 10 * i));
    sendAt(QEvent::MouseButtonRelease, corner + QPoint(60, 40));
    check("拖右下角：窗口左上角没被带着走",
          QStringLiteral("%1,%2").arg(pw.geometry().x()).arg(pw.geometry().y()),
          QStringLiteral("%1,%2").arg(winTL.x()).arg(winTL.y()));
    check("拖右下角：图的左上角一字不动",
          QStringLiteral("%1,%2").arg(pw.probeImageRect().x()).arg(pw.probeImageRect().y()),
          QStringLiteral("%1,%2").arg(imgTL.x()).arg(imgTL.y()));
    check("拖右下角：图确实跟着变大了（不是没生效）",
          pw.probeImageRect().width() > imgSz.width() ? "yes" : "no", "yes");
    check("拖右下角：窗口宽了 60 高了 40",
          sizeStr(pw.size()), sizeStr(s0 + QSize(60, 40)));
    // 拖左边那条：窗口左上角该跟着走（这是应有行为），但图的左上角必须**始终贴着客户区左上**，
    // 不能因为居中而往右退回一条余量。
    pw.resize(s0);
    const QPoint leftTL = pw.geometry().topLeft();
    const QPoint area0 = pw.probeImageRect().topLeft();
    sendAt(QEvent::MouseButtonPress, QPoint(3, pw.height() / 2));
    sendAt(QEvent::MouseMove, QPoint(3 + 25, pw.height() / 2));
    sendAt(QEvent::MouseButtonRelease, QPoint(3 + 25, pw.height() / 2));
    check("拖左边：窗口左上角跟着走（应有行为）",
          pw.geometry().x() == leftTL.x() + 25 ? "yes" : "no", "yes");
    check("拖左边：图的左上角还是贴着客户区左上（不重新居中）",
          pw.probeImageRect().topLeft() == QPoint(pw.stripW(), 0) ? "yes" : "no", "yes");
    std::printf("INFO 缩放前图区 %s，拖右下角之后 %s（左上角 %d,%d → %d,%d）\n",
                rectStr(QRect(imgTL, imgSz)).toUtf8().constData(),
                rectStr(pw.probeImageRect()).toUtf8().constData(),
                imgTL.x(), imgTL.y(), pw.probeImageRect().x(), pw.probeImageRect().y());

    // 离屏抓两张图自己看一眼（不显示窗口、不在你桌面上闪）：一张正常、一张两样都收起
    pw.grab().save(shots + "/a_normal.png");
    pw.setBarHidden(true);
    pw.setStripHidden(true);
    check("两样都收起：窗口正好等于图本身", sizeStr(pw.size()), sizeStr(r0.size()));
    pw.grab().save(shots + "/b_hidden.png");
    // 最容易被写坏的一步：两个开关叠着用之后再依次放开。
    // 收起工具栏时"记住的原尺寸"是在往期带还开着的时候记的，中间往期带又收过一次，
    // 放开时必须还是回到最初那个尺寸 —— 不然来回点几下窗口就越走越歪（这一条抓到过真 bug）。
    pw.setStripHidden(false);
    pw.setBarHidden(false);
    check("叠用之后再放开：窗口回到原尺寸", sizeStr(pw.size()), sizeStr(s0));
    check("叠用之后再放开：图区还是那个图区", rectStr(pw.probeImageRect()), rectStr(r0));
    check("叠用之后再放开：又命中第 0 张", QString::number(pw.probeThumbAt(QPoint(10, 10))), "0");
    check("叠用之后再放开：两个开关都回到开", (pw.barHidden() || pw.stripHidden()) ? "yes" : "no", "no");
    std::printf("INFO 离屏抓图：%s/a_normal.png 与 b_hidden.png\n", shots.toUtf8().constData());
    QFile::remove(Utils::capturesDir() + "/" + key);
    // ---- 右键菜单的样式挂没挂上 ----
    // 置顶窗是**另一个顶层窗口**，拿不到设置窗/搜索框各自那份 QMenu 样式表；而 Qt6 的
    // windows11 风格画弹出菜单用的是**系统**的浅色底，字色却来自我们的深色调色板
    // ⇒ 白底 + 浅灰字 = 看不见（2026-10-17 他截图报的就是这个）。
    // 判据量的是 `pw.makeStyledMenu()` 那个真对象。第一版在这里自己 `new QMenu`，
    // 改了样式表也量不出变化（离屏还照样"底色是深的"，因为调色板本来就是深的）—— 白绿一场；
    // 现在认的是**只有样式表能给出来的那两个数**：底色 == CSS 里那格 #232429
    // （不挂样式表时离屏量到的是调色板那格 #191a1e），以及 `padding: 4px` 推出来的首项 y。
    {
        QMenu* probeMenu = pw.makeStyledMenu();
        probeMenu->addAction(QStringLiteral("复制到剪贴板"));
        probeMenu->addAction(QStringLiteral("关闭"));
        const QImage im = probeMenu->grab().toImage();
        const QRect ig = probeMenu->actionGeometry(probeMenu->actions().first());
        const QColor bg = im.pixelColor(5, qMax(4, im.height() / 2));
        int brightest = 0;
        for (int y = 0; y < im.height(); y += 2)
            for (int x = 0; x < im.width(); x += 2)
                brightest = qMax(brightest, im.pixelColor(x, y).lightness());
        delete probeMenu;
        std::printf("INFO 菜单：底像素 %s、首项 y=%d 高=%d、最亮像素 %d、图 %dx%d\n",
                    bg.name(QColor::HexRgb).toUtf8().constData(), ig.y(), ig.height(), brightest,
                    im.width(), im.height());
        check("那份深色样式真挂上了（底色 == 样式里那格）",
              bg.name(QColor::HexRgb), QStringLiteral("#232429"));
        check("CSS 的 padding 生效了（首项不贴边）", ig.y() >= 4 ? "yes" : "no", "yes");
        check("字比底色亮出一大截（看得清）", brightest - bg.lightness() > 120 ? "yes" : "no", "yes");
    }
    // ---- 工具栏那一排到底多宽（黑边的根因就在这）----
    // setPixmap 里那句 setMinimumSize(这一排的 sizeHint, 140) 会把窗口撑到比图还宽，
    // 多出来的那一条就是近黑色的空档。原来一排 612px；图标化之后必须显著降下来。
    pw.setBarHidden(false);
    pw.setStripHidden(false);
    int nameless = 0, btns = 0;
    for (QPushButton* b : pw.findChildren<QPushButton*>()) {
        ++btns;
        if (b->accessibleName().isEmpty() && b->text().isEmpty())
            ++nameless;          // 图标按钮没文字，可访问名就是它唯一的"名字"
    }
    std::printf("INFO 放出工具栏：窗口最小宽 %d，按钮 %d 颗（既没文字又没可访问名的 %d 颗）\n",
                pw.minimumWidth(), btns, nameless);
    check("一排按钮收得下 460px（图宽 ≥384 就不会被撑出黑边）",
          pw.minimumWidth() <= 460 ? "yes" : "no", "yes");
    check("图标化之后每颗按钮都还认得出（文字或可访问名至少留一个）",
          nameless == 0 ? "yes" : "no", "yes");
    // 直接验他报的那件事：图够宽时，放出工具栏**不该**把窗口撑宽
    // 直接验他报的那件事：图够宽时，放出工具栏**不该**把窗口撑宽。
    // 图取 380 宽：离屏平台报的虚拟屏是 800x600，而 setPixmap 有一条"显示尺寸不超过屏宽 60%"
    // （第一版拿 520 宽的图测，被这条夹成 478，红得莫名其妙 —— 那是夹具撞了屏幕，不是产品）。
    QPixmap wide(380, 200);
    wide.fill(QColor(120, 60, 60));
    PinWindow pw2(wide, QString(), false);
    pw2.setBarHidden(false);
    pw2.setStripHidden(false);
    std::printf("INFO pw2：窗口 %s，图区 %s，最小宽 %d（屏 %dx%d）\n", sizeStr(pw2.size()).toUtf8().constData(),
                rectStr(pw2.probeImageRect()).toUtf8().constData(), pw2.minimumWidth(),
                QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->geometry().width() : 0,
                QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->geometry().height() : 0);
    check("图够宽时放出工具栏：窗口不比「往期带 + 图宽」更宽（右边不多一块黑）",
          pw2.width() <= pw2.stripW() + 380 ? "yes" : "no", "yes");
    check("图够宽时放出工具栏：图按原尺寸摆（没被夹小）",
          pw2.probeImageRect().width() == 380 ? "yes" : "no", "yes");

    // ---- 验证置顶特性：默认必须一直置顶（钉在桌面，切别的软件绝不能掉到最下面）----
    check("默认必须是一直置顶", pw2.isPinned() ? "yes" : "no", "yes");
    QPushButton* pinBtn = nullptr;
    for (QPushButton* b : pw2.findChildren<QPushButton*>()) {
        if (b->accessibleName() == QStringLiteral("一直置顶") || b->accessibleName() == QStringLiteral("自动让位")) {
            pinBtn = b;
            break;
        }
    }
    check("找到置顶切换按钮", pinBtn != nullptr ? "yes" : "no", "yes");
    if (pinBtn) {
        check("置顶按钮支持选中态", pinBtn->isCheckable() ? "yes" : "no", "yes");
        check("置顶按钮默认处于选中态", pinBtn->isChecked() ? "yes" : "no", "yes");
    }
    pw2.setPinned(false);
    check("切换为自动让位生效", !pw2.isPinned() ? "yes" : "no", "yes");
    if (pinBtn)
        check("切换后按钮取消选中态", !pinBtn->isChecked() ? "yes" : "no", "yes");
    pw2.setPinned(true);
    check("切回一直置顶生效", pw2.isPinned() ? "yes" : "no", "yes");

    // ---- 验证黑边修复：默认收起状态下，单点放出工具栏（往期带保持收起），绝不能多出 76px 黑边 ----
    {
        QPixmap wideShot(600, 300);
        wideShot.fill(Qt::blue);
        PinWindow pw3(wideShot, QString(), false); // 默认两样都收起
        check("新窗默认收起时宽=图宽", QString::number(pw3.width()), "600");
        pw3.setBarHidden(false); // 用户只单点「显示工具栏」！
        check("单点显示工具栏后宽仍等于图宽（绝不多出76px黑边）", QString::number(pw3.width()), "600");
        check("单点显示工具栏后图区宽=图宽", QString::number(pw3.probeImageRect().width()), "600");
    }

    std::printf("pintool: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad == 0 ? 0 : 1;
}


// --pindel <图片>：无界面验"右键缩略图删除"。这张图只用来给置顶窗当初始内容，
// 往期是现场造的三张纯色图，删的是临时目录里的文件，碰不到用户真实的 captures。
extern "C" Q_DECL_EXPORT int ltoolPindelRun(const char* spec)
{
    const QImage img(QString::fromUtf8(spec ? spec : ""));
    if (img.isNull()) {
        std::printf("pindel: 读不了这张图\n");
        std::fflush(stdout);
        return 2;
    }
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    QPixmap old3(100, 100); old3.fill(QColor(220, 30, 30));
    QPixmap old2(100, 100); old2.fill(QColor(30, 200, 30));
    QPixmap old1(100, 100); old1.fill(QColor(30, 60, 220));
    const QString k3 = CaptureHistory::add(old3);   // add 是 prepend：列表顺序 = old1, old2, old3
    const QString k2 = CaptureHistory::add(old2);
    const QString k1 = CaptureHistory::add(old1);
    const QString capDir = Utils::capturesDir();
    auto* pw = new PinWindow(old1, k1, false);
    check("起点：往期 3 张", QString::number(CaptureHistory::count()), "3");
    // 删中间那张（不是当前显示的）：当前不动，文件要没了
    check("删中间那张后当前还是原图", pw->deleteThumb(1), k1);
    check("往期剩 2 张", QString::number(CaptureHistory::count()), "2");
    check("被删的文件落盘也没了", QFile::exists(capDir + "/" + k2) ? "yes" : "no", "no");
    check("剩下的第 2 张是那张旧的", CaptureHistory::at(1).key, k3);
    // 删当前这张：应该自动挪到邻居，图上画的也该换了
    check("删当前那张=挪到邻居", pw->deleteThumb(0), k3);
    {
        const QImage shot = pw->grab().toImage();
        int r = 0, g = 0, b = 0;
        for (int y = 0; y < shot.height(); ++y)
            for (int x = 76; x < shot.width(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                if (c.red() > 150 && c.green() < 100) ++r;
                else if (c.green() > 150 && c.red() < 100) ++g;
                else if (c.blue() > 150 && c.red() < 100) ++b;
            }
        // 剩下的那张是 old3（红）：窗口里画的得是它，不能还是被删掉的蓝
        check("窗口里画的是剩下那张(红)", QString::number(r > g && r > b ? 1 : 0), "1");
    }
    // 删最后一张：往期空了，图还显示着，但 key 已经没有去处了
    check("删掉最后一张后 key 空", pw->deleteThumb(0), QString());
    check("往期 0 张", QString::number(CaptureHistory::count()), "0");
    check("越界删除不炸", pw->deleteThumb(7), QString());
    std::printf("pindel: 失败 %d 项\n", bad);
    std::fflush(stdout);
    delete pw;
    return bad == 0 ? 0 : 1;
}


// --ocrget：下载 OCR 引擎（PaddleOCR PP-OCRv4 + ONNX Runtime，27.9MB）并校验/解包。
// 有界面入口走的也是这套代码，所以这条跑通就说明"新机器没装中文语言包也能用"这件事成立。
extern "C" Q_DECL_EXPORT int ltoolOcrgetRun()
{
    std::printf("目标：%s\n", OcrBundle::describe().toUtf8().constData());
    std::fflush(stdout);
    QString err;
    const bool ok = OcrBundle::fetchSync(&err,
        [](int pct, const QString& s) { std::printf("  %3d%% %s\n", pct, s.toUtf8().constData()); std::fflush(stdout); });
    const OcrBundle::Files f = OcrBundle::status();
    std::printf("ocrget: ok=%d err=%s\n  det=%lld KB rec=%lld KB ort=%lld KB\n", ok ? 1 : 0,
                err.toUtf8().constData(), QFileInfo(f.det).size() / 1024,
                QFileInfo(f.rec).size() / 1024, QFileInfo(f.onnxruntime).size() / 1024);
    std::fflush(stdout);
    return ok ? 0 : 1;
}


// --ocrpanel <图片>：把"提取文字"结果窗真开一次（不经过截图遮罩），用来验识别→回填这条链
extern "C" Q_DECL_EXPORT int ltoolOcrpanelRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    QImage img(target);
    if (img.isNull()) {
        std::printf("ocrpanel: 读不了这张图 %s\n", target.toUtf8().constData());
        std::fflush(stdout);
        return 2;
    }
    OcrPanel::extractFrom(img);
    auto* tick = new QTimer(qApp);
    tick->setInterval(300);
    QObject::connect(tick, &QTimer::timeout, qApp, [] {
        if (qApp->topLevelWidgets().isEmpty())
            qApp->quit();
    });
    tick->start();
    const int rc = QCoreApplication::exec();
    std::printf("ocrpanel: 窗口已关闭\n");
    std::fflush(stdout);
    return rc;
}


// --pinshot <图片>：用这张图开一个置顶截图窗（验标注对象的选中/拖动/缩放/删除）
extern "C" Q_DECL_EXPORT int ltoolPinshotRun(const char* spec)
{
    const QString target = QString::fromUtf8(spec ? spec : "");
    const QPixmap pm(target);
    if (pm.isNull()) {
        std::printf("pinshot: 读不了这张图 %s\n", target.toUtf8().constData());
        std::fflush(stdout);
        return 2;
    }
    auto* w = new PinWindow(pm, CaptureHistory::add(pm));
    QObject::connect(w, &QObject::destroyed, qApp, [] {
        if (qApp->topLevelWidgets().isEmpty())
            qApp->quit();
    });
    const int rc = QCoreApplication::exec();
    std::printf("pinshot: 窗口已关闭\n");
    std::fflush(stdout);
    return rc;
}



extern "C" Q_DECL_EXPORT int ltoolCapgeoRun()
{
    int bad = 0;
    const auto check = [&bad](const char* what, bool ok, const QString& got) {
        std::printf("%s %s：%s\n", ok ? "ok  " : "FAIL", what, got.toUtf8().constData());
        if (!ok)
            ++bad;
        std::fflush(stdout);
    };
    QScreen* scr = QGuiApplication::primaryScreen();
    const qreal dpr = scr->devicePixelRatio();
    const QSize log = scr->geometry().size();
    const QPixmap shot = scr->grabWindow(0);
    std::printf("INFO 屏幕逻辑=%dx%d DPR=%.2f 抓取物理=%dx%d(图上 DPR=%.2f)\n",
                log.width(), log.height(), dpr, shot.width(), shot.height(),
                shot.devicePixelRatio());
    const int wantW = int(log.width() * dpr + 0.5);
    const int wantH = int(log.height() * dpr + 0.5);
    check("抓的图铺满整块屏（物理=逻辑×DPR）",
          qAbs(shot.width() - wantW) <= 2 && qAbs(shot.height() - wantH) <= 2,
          QStringLiteral("期望 %1x%2，拿到 %3x%4").arg(wantW).arg(wantH)
              .arg(shot.width()).arg(shot.height()));
    const QPixmap full = CaptureOverlay::cropCanvas(shot, QRect(QPoint(0, 0), log));
    check("产品那条裁剪路：逻辑选区裁全屏，出来的还是整块屏",
          qAbs(full.width() - wantW) <= 2 && qAbs(full.height() - wantH) <= 2,
          QStringLiteral("选区=%1x%2(逻辑) 裁完=%3x%4(物理)，差 %5x%6")
              .arg(log.width()).arg(log.height())
              .arg(full.width()).arg(full.height())
              .arg(wantW - full.width()).arg(wantH - full.height()));

    // ---- 合成 DPR=1.5 的画布（和真屏缩放无关，100% 的机器上照样能抓到这个错） ----
    constexpr qreal kDpr = 1.5;
    const QSize kLog(1000, 600);
    const QSize kPhys(int(kLog.width() * kDpr), int(kLog.height() * kDpr));   // 1500x900
    auto makeCanvas = [&]() {
        QPixmap pm(kPhys.width(), kPhys.height());
        pm.fill(Qt::white);
        QPainter q(&pm);
        q.fillRect(kPhys.width() - 1, kPhys.height() - 1, 1, 1, Qt::black);
        q.fillRect(300, 150, 1, 1, QColor(255, 0, 0));   // 设备 (300,150) = 逻辑 (200,100)
        q.end();
        pm.setDevicePixelRatio(kDpr);                     // 现在它的逻辑尺寸是 1000x600
        return pm;
    };
    const QPixmap whole = CaptureOverlay::cropCanvas(makeCanvas(), QRect(QPoint(0, 0), kLog));
    check("合成 1.5 倍画布上裁全屏：尺寸=物理全幅",
          whole.size() == kPhys,
          QStringLiteral("期望 %1x%2，拿到 %3x%4（少了 %5x%6）")
              .arg(kPhys.width()).arg(kPhys.height()).arg(whole.width()).arg(whole.height())
              .arg(kPhys.width() - whole.width()).arg(kPhys.height() - whole.height()));
    check("合成 1.5 倍画布上裁全屏：右下角那颗点还在",
          qAbs(whole.devicePixelRatio() - kDpr) < 0.01
              && whole.toImage().pixelColor(kPhys.width() - 1, kPhys.height() - 1) == QColor(Qt::black),
          QStringLiteral("图上 DPR=%1，右下角=%2")
              .arg(whole.devicePixelRatio())
              .arg(whole.toImage().pixelColor(kPhys.width() - 1, kPhys.height() - 1).name()));
    // 局部选区：逻辑 (200,100) 起 = 设备 (300,150)，那颗红点必须正好落到裁出来那张图的左上角
    const QRect part(200, 100, 400, 200);
    const QPixmap piece = CaptureOverlay::cropCanvas(makeCanvas(), part);
    check("合成画布上裁局部：取到的就是那块（红点跑到左上角）",
          piece.toImage().pixelColor(0, 0) == QColor(255, 0, 0),
          QStringLiteral("选区=(%1,%2,%3x%4) 左上角像素=%5，裁完尺寸=%6x%7")
              .arg(part.x()).arg(part.y()).arg(part.width()).arg(part.height())
              .arg(piece.toImage().pixelColor(0, 0).name())
              .arg(piece.width()).arg(piece.height()));

    // ---- QPainter::drawPixmap 的 sourceRect 按哪种坐标解释（浮层"选区亮起"用的就是它） ----
    const QPixmap canvas = makeCanvas();
    QImage sink(kPhys, QImage::Format_ARGB32);
    sink.fill(Qt::red);
    {
        QPainter q(&sink);
        q.drawPixmap(QRect(QPoint(0, 0), kPhys), canvas, QRect(QPoint(0, 0), kLog));
    }
    const QColor rawCorner = sink.pixelColor(kPhys.width() - 1, kPhys.height() - 1);
    std::printf("INFO 直接把逻辑矩形当 sourceRect：目标右下角=%s，说明 Qt 把它当%s看的\n",
                rawCorner.name().toUtf8().constData(),
                rawCorner == QColor(Qt::black) ? "逻辑坐标" : "设备像素");
    QImage fixed(kPhys, QImage::Format_ARGB32);
    fixed.fill(Qt::red);
    {
        QPainter q(&fixed);
        q.drawPixmap(QRect(QPoint(0, 0), kPhys), canvas,
                     CaptureOverlay::deviceRectFor(QRect(QPoint(0, 0), kLog), kDpr));
    }
    check("预览那一步换算之后取到的才是整块画布",
          fixed.pixelColor(kPhys.width() - 1, kPhys.height() - 1) == QColor(Qt::black),
          QStringLiteral("换算后右下角=%1").arg(
              fixed.pixelColor(kPhys.width() - 1, kPhys.height() - 1).name()));
    const QRect keep = CaptureOverlay::deviceRectFor(QRect(7, 9, 120, 60), 1.0);
    check("dpr=1 时换算原样返回（100% 缩放下的行为一点没动）",
          keep == QRect(7, 9, 120, 60),
          QStringLiteral("(%1,%2,%3x%4)").arg(keep.x()).arg(keep.y())
              .arg(keep.width()).arg(keep.height()));
    std::printf("capgeo: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad == 0 ? 0 : 1;
}



extern "C" Q_DECL_EXPORT int ltoolCapflashRun(int times)
{
    int ok = 0;
    for (int i = 1; i <= times; ++i) {
        CaptureOverlay::start();
        for (int k = 0; k < 20; ++k) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        QWidget* ov = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->inherits("CaptureOverlay") && w->isVisible())
                ov = w;
        const QRect scr = QGuiApplication::primaryScreen()->geometry();
        const bool full = ov && ov->geometry() == scr;
        HWND hwnd = ov ? reinterpret_cast<HWND>(ov->winId()) : nullptr;
        const bool fg = hwnd && GetForegroundWindow() == hwnd;
        POINT pt {};
        GetCursorPos(&pt);
        const bool onTop = hwnd && GetAncestor(WindowFromPoint(pt), GA_ROOT) == hwnd;
        // 判据只用确定性的三件事（浮层在、铺满整屏、鼠标底下就是它）。
        // "有没有拿到前台"在这里不当判据：脚本环境里前台归属本来就不稳
        //（同一环境下 --toggle 也会 10 次输 1 次），真实按 F1 有物理输入垫底，
        // 那一支由产品日志里的 前台=/试了N次 说话。
        const bool good = ov && full && onTop;
        const quint64 ex = hwnd ? quint64(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)) : 0;
        std::printf("%s 第%d次：浮层=%s 铺满整屏=%s 在最上=%s 前台=%s（屏 %dx%d）exstyle=0x%llX fg=0x%llX 自己=0x%llX\n",
                    good ? "PASS" : "FAIL", i,
                    ov ? "在" : "没有", full ? "是" : "否",
                    onTop ? "是" : "否", fg ? "是" : "否", scr.width(), scr.height(),
                    (unsigned long long)ex,
                    (unsigned long long)(quintptr)GetForegroundWindow(),
                    (unsigned long long)(quintptr)hwnd);
        if (good)
            ++ok;
        CaptureOverlay::cancelActive();
        for (int k = 0; k < 25 && CaptureOverlay::isActive(); ++k) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        if (CaptureOverlay::isActive()) {
            std::printf("FAIL 第%d次：取消之后浮层还开着\n", i);
        } else {
            std::printf("INFO 第%d次：取消干净了\n", i);
        }
        std::fflush(stdout);
    }
    std::printf("capflash: %d/%d 次做到了\"浮层在、铺满整屏、压在所有窗口最上\"\n",
                ok, times);
    std::printf("capflash: 失败 %d 项\n", times - ok);
    std::fflush(stdout);
    return times - ok;
}

