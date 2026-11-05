#include "shellaction.h"

#include "Archives.h"
#include "FileOps.h"
#include "Utils.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPair>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include <atomic>
#include <functional>

namespace {

QString windowTitle(const QString& what, const QString& path)
{
    return QStringLiteral("%1 · %2").arg(what, QFileInfo(path).fileName());
}

// 一个窗口两件事：上面"它占了多少"，下面"哪个程序正开着它"。
// 两块的重活都丢后台线程，算完由 watcher 回填 —— 目录递归求和可能要几秒。
int showUsage(const QString& path)
{
    const FileOps::Info in = FileOps::info(path);
    QDialog d;
    d.setWindowTitle(windowTitle(QStringLiteral("占用分析"), path));
    d.setMinimumSize(560, 430);
    auto* lay = new QVBoxLayout(&d);

    auto* head = new QLabel(QDir::toNativeSeparators(path), &d);
    head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    head->setWordWrap(true);
    lay->addWidget(head);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(6);
    const auto addRow = [&form](const QString& k, const QString& v) {
        auto* key = new QLabel(k);
        key->setObjectName(QStringLiteral("key"));
        auto* val = new QLabel(v);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        val->setWordWrap(true);
        form->addRow(key, val);
        return val;
    };
    if (!in.ok) {
        addRow(QStringLiteral("结果"), in.error);
    } else {
        addRow(in.isDir ? QStringLiteral("类型") : QStringLiteral("逻辑大小"),
               in.isDir ? QStringLiteral("文件夹") : FileOps::human(in.size));
        if (!in.isDir && in.alloc > 0) {
            addRow(QStringLiteral("实际占簇"), QStringLiteral("%1%2")
                       .arg(FileOps::human(in.alloc),
                            in.size > 0 && in.alloc > in.size * 2
                                ? QStringLiteral("（比文件本身大，小文件也会吃掉一整个簇）")
                                : QString()));
            if (in.compressed && in.alloc < in.size)
                addRow(QStringLiteral("压缩后"), QStringLiteral("省了 %1").arg(FileOps::human(in.size - in.alloc)));
        }
        addRow(QStringLiteral("簇大小 / 文件系统"),
               QStringLiteral("%1 · %2").arg(in.cluster > 0 ? FileOps::human(in.cluster) : QStringLiteral("—"),
                                             in.fsName));
        if (in.hardLinks > 1)
            addRow(QStringLiteral("硬链接"), QStringLiteral("%1 个入口共用同一份数据").arg(in.hardLinks));
        if (!in.attrs.isEmpty())
            addRow(QStringLiteral("属性"), in.attrs);
        addRow(QStringLiteral("修改时间"), in.lastWrite);
    }
    QLabel* usageVal = addRow(in.isDir ? QStringLiteral("占用（含子文件）") : QStringLiteral("占用"),
                              QStringLiteral("统计中…"));
    lay->addLayout(form);

    auto* usageWatcher = new QFutureWatcher<FileOps::Usage>(&d);
    QObject::connect(usageWatcher, &QFutureWatcherBase::finished, &d, [usageVal, usageWatcher, in]() {
        const FileOps::Usage u = usageWatcher->result();
        if (!u.ok) {
            usageVal->setText(u.error);
            return;
        }
        QString s = FileOps::human(u.bytes);
        if (u.alloc > 0 && u.alloc != u.bytes)
            s += QStringLiteral("（实际占簇 %1）").arg(FileOps::human(u.alloc));
        if (in.isDir)
            s += QStringLiteral(" · %1 个文件 / %2 个子目录").arg(u.files).arg(u.dirs);
        usageVal->setText(s);
    });
    usageWatcher->setFuture(QtConcurrent::run([path] { return FileOps::usage(path); }));

    auto* sep = new QLabel(QStringLiteral("什么程序在使用这个文件"), &d);
    sep->setObjectName(QStringLiteral("muted"));
    lay->addWidget(sep);
    auto* status = new QLabel(QStringLiteral("正在问系统哪些进程开着它…（毫秒级，只查不动任何进程）"), &d);
    status->setObjectName(QStringLiteral("muted"));
    status->setWordWrap(true);
    lay->addWidget(status);

    auto* list = new QListWidget(&d);
    list->setWordWrap(true);
    lay->addWidget(list, 1);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* kill = new QPushButton(QStringLiteral("结束选中的进程"), &d);
    kill->setEnabled(false);
    auto* again = new QPushButton(QStringLiteral("重新查一次"), &d);
    again->setEnabled(false);
    auto* close = new QPushButton(QStringLiteral("关闭"), &d);
    btns->addWidget(again);
    btns->addWidget(kill);
    btns->addWidget(close);
    lay->addLayout(btns);

    auto* holderWatcher = new QFutureWatcher<QPair<QVector<FileOps::Holder>, QString>>(&d);
    QObject::connect(holderWatcher, &QFutureWatcherBase::finished, &d,
                     [list, status, kill, again, holderWatcher]() {
        const auto res = holderWatcher->result();
        list->clear();
        if (res.first.isEmpty()) {
            status->setText(res.second.isEmpty()
                                ? QStringLiteral("没有进程正开着这个文件 —— 现在可以安全地删除或改名。")
                                : res.second);
            again->setEnabled(true);
            return;
        }
        status->setText(QStringLiteral("有 %1 个进程正开着它。结束进程会丢掉那个程序里没保存的东西，"
                                       "先确认一下再动手。").arg(res.first.size()));
        for (const FileOps::Holder& h : res.first) {
            auto* it = new QListWidgetItem(QStringLiteral("%1  (PID %2)  ·  开了 %3 个句柄\n%4")
                                               .arg(h.processName.isEmpty() ? QStringLiteral("未知进程") : h.processName)
                                               .arg(h.pid).arg(h.handles)
                                               .arg(h.exePath.isEmpty() ? h.objects.value(0) : h.exePath), list);
            it->setData(Qt::UserRole, h.pid);
        }
        again->setEnabled(true);
    });
    QObject::connect(list, &QListWidget::itemSelectionChanged, &d, [list, kill]() {
        kill->setEnabled(!list->selectedItems().isEmpty());
    });
    QObject::connect(kill, &QPushButton::clicked, &d, [list, status, path]() {
        auto* it = list->currentItem();
        if (!it)
            return;
        const quint64 pid = it->data(Qt::UserRole).toULongLong();
        if (QMessageBox::question(nullptr, QStringLiteral("结束进程"),
                QStringLiteral("要强制结束这个进程：\n\n%1\n\n它里面没保存的东西会直接丢掉。确定吗？")
                    .arg(it->text().section(QLatin1Char('\n'), 0, 0)))
            != QMessageBox::Yes)
            return;
        QString err;
        status->setText(FileOps::killProcess(pid, &err)
                            ? QStringLiteral("已结束 PID %1，现在这个文件应该没人占着了（可以「重新查一次」确认）").arg(pid)
                            : err);
    });
    auto startQuery = [&d, list, status, again, holderWatcher, path]() {
        list->clear();
        again->setEnabled(false);
        status->setText(QStringLiteral("正在问系统哪些进程开着它…"));
        holderWatcher->setFuture(QtConcurrent::run([path] {
            QString err;
            const auto got = FileOps::whoHas(path, &err);
            return qMakePair(got, err);
        }));
    };
    QObject::connect(again, &QPushButton::clicked, &d, [startQuery]() { startQuery(); });
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::accept);
    if (in.isDir) {
        // 系统的"谁开着这个文件"只按文件报，拿目录路径去问只会拿到 ERROR_ACCESS_DENIED(5)，
        // 所以这里不问，直接说清楚该怎么做。
        status->setText(QStringLiteral("文件夹本身问不到 —— 系统只按文件报谁开着它。"
                                       "想知道是哪个程序占着里面的文件，右键那个文件再点「占用分析」。"));
    } else {
        startQuery();
    }
    return d.exec();
}


int showShred(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        QMessageBox::warning(nullptr, QStringLiteral("粉碎"),
                             QStringLiteral("这个文件已经不存在了：%1").arg(path));
        return 0;
    }
    qint64 bytes = fi.size();
    bool isDir = fi.isDir();
    if (isDir) {               // 目录先算一下总量，让人知道要等多久
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        qint64 sum = 0;
        while (it.hasNext())
            sum += QFileInfo(it.next()).size();
        bytes = sum;
    }

    QDialog d;
    d.setWindowTitle(QStringLiteral("粉碎 · %1").arg(fi.fileName()));
    d.setMinimumWidth(520);
    auto* lay = new QVBoxLayout(&d);
    auto* warn = new QLabel(QStringLiteral(
        "这一步不可恢复：文件不进回收站，数据会被原地覆写。\n\n目标：%1\n总量：%2")
                                .arg(QDir::toNativeSeparators(path), FileOps::human(bytes)), &d);
    warn->setWordWrap(true);
    lay->addWidget(warn);

    auto* deep = new QCheckBox(QStringLiteral("多覆写几遍（随机→全 0→随机，再把文件名改成乱码）"), &d);
    deep->setToolTip(QStringLiteral("不勾 = 覆写一次 + 截断 + 删除；机械盘上 500MB 大约 5-8 秒。\n"
                                    "勾上 = 三遍，时间约三倍。\n\n"
                                    "注意：固态硬盘的主控做过重映射，覆写只能保证文件系统层面读不回来，"
                                    "不保证闪存芯片上的旧副本已经消失。真要彻底销毁，得用盘自带的 Secure Erase。"));
    lay->addWidget(deep);

    auto* est = new QLabel(&d);
    est->setObjectName(QStringLiteral("muted"));
    const auto setEst = [est, bytes](bool thorough) {
        const double mb = double(bytes) / (1024.0 * 1024.0);
        const double secs = qMax(0.2, mb / 60.0) * (thorough ? 3.0 : 1.0);
        est->setText(QStringLiteral("预计耗时：约 %1 秒（按 60MB/s 估，固态会更快）").arg(secs, 0, 'f', secs < 10 ? 1 : 0));
    };
    setEst(false);
    QObject::connect(deep, &QCheckBox::toggled, &d, [setEst](bool on) { setEst(on); });
    lay->addWidget(est);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &d);
    auto* go = new QPushButton(QStringLiteral("开始粉碎"), &d);
    go->setObjectName(QStringLiteral("primary"));
    btns->addWidget(cancel);
    btns->addWidget(go);
    lay->addLayout(btns);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);

    QObject::connect(go, &QPushButton::clicked, &d, [&d, path, deep]() {
        const bool th = deep->isChecked();
        d.accept();
        QProgressDialog prog(QStringLiteral("正在覆写并删除…"), QString(), 0, 0, nullptr);
        prog.setWindowTitle(QStringLiteral("粉碎"));
        prog.setWindowModality(Qt::ApplicationModal);
        prog.setMinimumDuration(0);
        prog.setCancelButton(nullptr);        // 覆写中途取消只会留下"半毁"的文件，不给取消
        prog.show();
        QApplication::processEvents();
        QElapsedTimer t;
        t.start();
        QString err;
        const qint64 erased = FileOps::shred(path, th, &err);
        prog.setValue(100);
        if (erased < 0)
            QMessageBox::warning(nullptr, QStringLiteral("粉碎失败"), err);
        else
            QMessageBox::information(nullptr, QStringLiteral("粉碎完成"),
                                     QStringLiteral("已覆写并删除 %1（%2），用时 %3 秒。")
                                         .arg(FileOps::human(erased),
                                              th ? QStringLiteral("三遍覆写") : QStringLiteral("一遍覆写"),
                                              QString::number(t.elapsed() / 1000.0, 'f', 1)));
    });
    return d.exec();
}

// 解压 / 压缩都丢到工作线程，界面上只有一条进度 + 一个"取消"。
// 进度回调在 worker 线程里被调用，所以只能往 GUI 线程 post 事件，不能直接改控件。
//
// 屏幕正中间弹窗基类：在 showEvent 中计算当前鼠标所在屏幕并居中对齐，支持多显示器
class CenterDialog : public QDialog
{
public:
    using QDialog::QDialog;

protected:
    void showEvent(QShowEvent* ev) override
    {
        QDialog::showEvent(ev);
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect g = screen->availableGeometry();
            const QRect fg = frameGeometry();
            move(g.left() + (g.width() - fg.width()) / 2,
                 g.top() + (g.height() - fg.height()) / 2);
        }
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd)
            SetForegroundWindow(hwnd);
#endif
    }
};

// 干活期间窗口不能被关闭：这个对话框是栈对象，工作线程还在引用它的控件。
// 早先是靠 setWindowFlags 摘掉关闭按钮来挡的，但那句会把已经显示的窗口隐藏掉，
// exec() 当场返回 —— 进度窗口一闪就没，"完成"两个字用户永远看不见。
class BusyDialog : public CenterDialog
{
public:
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::atomic<bool> busy { true };

    void closeEvent(QCloseEvent* ev) override
    {
        if (busy.load()) {
            cancelled->store(true);         // 想走就先把活停下来，等它自己收尾
            ev->ignore();
            return;
        }
        QDialog::closeEvent(ev);
    }
    void reject() override
    {
        if (busy.load()) {
            cancelled->store(true);
            return;
        }
        QDialog::reject();
    }
};

// 定义在下面 200 行处（runArchiveQuiet 那条路也用）；默认值只写在这一行，
// 否则 MSVC 报 C2572（重复默认参数）。
void trayNote(const QString& title, const QString& msg, int msec = 5000);

int runArchiveTask(const QString& title, const QString& what, const QString& openTarget,
                   std::function<Archive::Result(const Archive::Report&, const Archive::Cancel&)> task)
{
    QElapsedTimer clock;
    clock.start();
    BusyDialog d;
    d.setWindowTitle(title);
    d.setMinimumWidth(540);
    auto* lay = new QVBoxLayout(&d);
    auto* line = new QLabel(what, &d);
    line->setWordWrap(true);
    lay->addWidget(line);
    auto* bar = new QProgressBar(&d);
    bar->setRange(0, 0);
    lay->addWidget(bar);
    auto* cur = new QLabel(QStringLiteral("准备中…"), &d);
    cur->setObjectName(QStringLiteral("muted"));
    cur->setWordWrap(true);
    lay->addWidget(cur);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* openBtn = new QPushButton(QStringLiteral("打开位置"), &d);
    openBtn->setEnabled(false);
    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), &d);
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), &d);
    closeBtn->setEnabled(false);
    btns->addWidget(openBtn);
    btns->addWidget(cancelBtn);
    btns->addWidget(closeBtn);
    lay->addLayout(btns);

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    d.cancelled = cancelled;
    // 「打开位置」打开的是哪一本：默认是要生成的那个路径，失败时改成结果里带回来的
    // target（多半是那个没改名的 .part，人要进去手动改名字）
    auto shownTarget = std::make_shared<QString>(openTarget);
    auto* watcher = new QFutureWatcher<Archive::Result>(&d);
    QObject::connect(watcher, &QFutureWatcherBase::finished, &d,
                     [&d, bar, cur, openBtn, cancelBtn, closeBtn, watcher, shownTarget,
                      &clock, title]() {
        d.busy.store(false);              // 活干完了，窗口这才允许关
        bar->setRange(0, 100);
        bar->setValue(100);
        cancelBtn->setEnabled(false);
        closeBtn->setEnabled(true);
        const Archive::Result r = watcher->result();
        // 弹框这条路以前跑完什么都不留：界面上那句话关掉就没了，出问题时 log 里查不到
        // 这一次动作（对照 runArchiveQuiet，那条是有这行的）。
        Utils::log(QStringLiteral("%1 弹框跑完：ok=%2 files=%3 dirs=%4 bytes=%5 target=%6 err=%7 用时=%8ms")
                       .arg(title).arg(r.ok ? 1 : 0).arg(r.files).arg(r.dirs).arg(r.bytes)
                       .arg(r.target, r.error).arg(clock.elapsed()));
        if (r.ok) {
            // 成功就自己关 —— 他要的是"完事就别杵在那儿"。那句话交给托盘去举（走常驻实例，
            // 不在这个进程里等 5 秒，理由见 trayNote 上面那段）。
            trayNote(QStringLiteral("%1完成").arg(title),
                     QStringLiteral("%2 个文件、%3 → %4")
                         .arg(r.files)
                         .arg(FileOps::human(r.bytes), QDir::toNativeSeparators(r.target)));
            if (title == QStringLiteral("解压")) {
                const double secs = qMax<qint64>(1, clock.elapsed()) / 1000.0;
                cur->setText(QStringLiteral("解压完成：共 %1 个文件、%2（用时 %3 秒）")
                                 .arg(r.files).arg(FileOps::human(r.bytes))
                                 .arg(QString::number(secs, 'f', 1)));
                *shownTarget = r.target;
                openBtn->setText(QStringLiteral("打开解压位置"));
                openBtn->setEnabled(true);
                openBtn->setDefault(true);
                openBtn->setFocus();
                cancelBtn->setVisible(false);
                closeBtn->setEnabled(true);
                d.raise();
                d.activateWindow();
                return;
            }
            d.accept();
            return;
        }
        if (r.cancelled) {
            cur->setText(r.error.isEmpty() ? QStringLiteral("已取消") : r.error);
            d.accept();      // 取消也是"这一下结束了"，不该再多要点一次关闭
            return;
        }
        // 失败：窗留着说清楚为什么，「打开位置」这时指的是那个半成品
        cur->setText(r.error.isEmpty() ? QStringLiteral("失败了，没给出原因") : r.error);
        if (!r.target.isEmpty()) {
            *shownTarget = r.target;
            openBtn->setEnabled(true);
        }
    });
    QObject::connect(cancelBtn, &QPushButton::clicked, &d, [cancelled, cur, bar]() {
        cancelled->store(true);
        bar->setRange(0, 0);
        cur->setText(QStringLiteral("正在取消（等当前这个文件写完）…"));
    });
    QObject::connect(openBtn, &QPushButton::clicked, &d, [shownTarget, &d]() {
        const QString& openTarget = *shownTarget;
        const QString target = QFileInfo(openTarget).isDir() ? openTarget
                                                             : QFileInfo(openTarget).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
        d.accept();
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &d, &QDialog::accept);

    watcher->setFuture(QtConcurrent::run([task, cancelled, cur, bar]() {
        const Archive::Report rep = [cancelled, cur, bar](const Archive::Progress& pr) {
            if (cancelled->load())
                return false;
            const qint64 done = pr.done, total = pr.total;
            const QString name = pr.current;
            QMetaObject::invokeMethod(cur, [cur, bar, done, total, name]() {
                if (total > 0) {
                    bar->setRange(0, 100);
                    bar->setValue(int(qMin<qint64>(100, done * 100 / total)));
                }
                cur->setText(name);
            }, Qt::QueuedConnection);
            return true;
        };
        const Archive::Cancel can = [cancelled]() { return cancelled->load(); };
        return task(rep, can);
    }));
    d.adjustSize();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect g = screen->availableGeometry();
        d.move(g.center().x() - d.width() / 2, g.center().y() - d.height() / 2);
    }
    return d.exec();
}

QString uniqueName(const QString& dir, const QString& base, const QString& suffix)
{
    QString p = QDir(dir).absoluteFilePath(base + suffix);
    for (int n = 2; QFileInfo::exists(p) && n < 100; ++n)
        p = QDir(dir).absoluteFilePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix));
    return p;
}

// 配置页那个"压缩 / 解压时弹出进度窗口"开关，默认关
bool progressWindowOn()
{
    return QSettings(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat)
        .value(QStringLiteral("ui/archiveProgress"), false).toBool();
}

// 托盘上冒一句就走：本体已经收进 ltoolcore 的 Utils::trayNote（"为什么交给常驻实例去举"
// 写在那儿），这一层只是当年它只归这个插件用时留下的名字 —— 右键那几处还在直接叫它。
void trayNote(const QString& title, const QString& msg, int msec)
{
    Utils::trayNote(title, msg, msec);
}

void showExtractDoneDialog(const QString& target, int files, qint64 bytes, qint64 elapsedMs)
{
    CenterDialog dlg;
    dlg.setWindowTitle(QStringLiteral("解压完成"));
    dlg.setMinimumWidth(520);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(14);
    lay->setContentsMargins(24, 22, 24, 20);

    // 头部行：图标 + 标题 + 统计
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    auto* iconLabel = new QLabel(&dlg);
    const QIcon icon = qApp ? qApp->style()->standardIcon(QStyle::SP_DialogApplyButton) : QIcon();
    if (!icon.isNull())
        iconLabel->setPixmap(icon.pixmap(36, 36));
    topRow->addWidget(iconLabel);

    auto* titleCol = new QVBoxLayout;
    titleCol->setSpacing(4);
    auto* titleLabel = new QLabel(QStringLiteral("解压完成"), &dlg);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));

    const double secs = qMax<qint64>(1, elapsedMs) / 1000.0;
    auto* infoLabel = new QLabel(QStringLiteral("已成功解压 %1 个文件（%2），用时 %3 秒")
                                     .arg(files)
                                     .arg(FileOps::human(bytes))
                                     .arg(QString::number(secs, 'f', 1)),
                                 &dlg);
    infoLabel->setObjectName(QStringLiteral("muted"));
    titleCol->addWidget(titleLabel);
    titleCol->addWidget(infoLabel);
    topRow->addLayout(titleCol);
    topRow->addStretch(1);
    lay->addLayout(topRow);

    // 目标路径卡片
    auto* pathBox = new QFrame(&dlg);
    pathBox->setFrameShape(QFrame::StyledPanel);
    pathBox->setStyleSheet(QStringLiteral(
        "QFrame { background: rgba(128, 128, 128, 0.08); border: 1px solid rgba(128, 128, 128, 0.15); border-radius: 6px; }"));
    auto* pathLay = new QVBoxLayout(pathBox);
    pathLay->setContentsMargins(14, 10, 14, 10);
    pathLay->setSpacing(4);

    auto* pathTitle = new QLabel(QStringLiteral("解压位置："), pathBox);
    pathTitle->setObjectName(QStringLiteral("muted"));
    pathTitle->setStyleSheet(QStringLiteral("font-size: 12px;"));
    auto* pathVal = new QLabel(QDir::toNativeSeparators(target), pathBox);
    pathVal->setWordWrap(true);
    pathVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathVal->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 13px;"));
    pathLay->addWidget(pathTitle);
    pathLay->addWidget(pathVal);
    lay->addWidget(pathBox);

    // 底部按钮栏
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* openBtn = new QPushButton(QStringLiteral("打开解压位置"), &dlg);
    openBtn->setDefault(true);
    openBtn->setFocus();
    openBtn->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 28px; padding: 0 16px; font-weight: 600; }"));
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), &dlg);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 28px; padding: 0 16px; }"));
    btns->addWidget(openBtn);
    btns->addWidget(closeBtn);
    lay->addLayout(btns);

    QObject::connect(openBtn, &QPushButton::clicked, &dlg, [&dlg, target]() {
        const QString openTarget = QFileInfo(target).isDir() ? target
                                                             : QFileInfo(target).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(openTarget));
        dlg.accept();
    });
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.adjustSize();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect g = screen->availableGeometry();
        const QRect fg = dlg.frameGeometry();
        dlg.move(g.left() + (g.width() - fg.width()) / 2,
                 g.top() + (g.height() - fg.height()) / 2);
    }

    dlg.raise();
    dlg.activateWindow();
    dlg.exec();
}

// 不弹框那一种：这个进程本来就是为这一次动作起的，直接在它自己线程里跑完就行。
// 成功在托盘冒一句（不抢焦点），失败一定要弹框 —— 静默失败会让人以为压缩包已经做好了。
int runArchiveQuiet(const QString& title, const QString& what,
                    const std::function<Archive::Result(const Archive::Report&, const Archive::Cancel&)>& task)
{
    QElapsedTimer t;
    t.start();
    const Archive::Result r = task(nullptr, nullptr);
    Utils::log(QStringLiteral("%1 后台跑完：ok=%2 files=%3 dirs=%4 bytes=%5 target=%6 err=%7 用时=%8ms")
                   .arg(title).arg(r.ok ? 1 : 0).arg(r.files).arg(r.dirs).arg(r.bytes)
                   .arg(r.target, r.error).arg(t.elapsed()));
    if (!r.ok) {
        QMessageBox::warning(nullptr, title, QStringLiteral("%1\n\n失败：%2").arg(what, r.error));
        return 1;
    }
    trayNote(QStringLiteral("%1完成").arg(title),
             QStringLiteral("%2 个文件、%3 → %4")
                 .arg(r.files)
                 .arg(FileOps::human(r.bytes), QDir::toNativeSeparators(r.target)));
    if (title == QStringLiteral("解压")) {
        showExtractDoneDialog(r.target, r.files, r.bytes, t.elapsed());
    }
    return 0;
}

int showExtract(const QString& path, bool intoSub)
{
    if (!Archive::looksLikeArchive(path)) {
        QMessageBox::warning(nullptr, QStringLiteral("解压"),
                             QStringLiteral("这个文件看起来不是压缩包（或者格式不认识）：\n%1").arg(path));
        return 0;
    }
    const QFileInfo fi(path);
    QString dest = fi.absolutePath();
    if (intoSub)
        dest = uniqueName(fi.absolutePath(), fi.completeBaseName(), QString());
    const QString what = QStringLiteral("把 %1 解压到\n%2").arg(fi.fileName(), QDir::toNativeSeparators(dest));
    const auto task = [path, dest](const Archive::Report& rep, const Archive::Cancel& can) {
        return Archive::extract(path, dest, rep, can);
    };
    const QString title = QStringLiteral("解压");
    return progressWindowOn() ? runArchiveTask(title, what, dest, task)
                              : runArchiveQuiet(title, what, task);
}

// 两种容器：zip（谁都能双击打开，实测也最快）/ 7z（体积小一些，但 libarchive 的
// LZMA 写出器是单线程，大东西要等十几秒 —— 详见 Archives.h 上那段测量）
int showCompress(const QString& path, bool sevenZip)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        QMessageBox::warning(nullptr, QStringLiteral("压缩"), QStringLiteral("这个文件已经不存在了"));
        return 0;
    }
    const QString suffix = sevenZip ? QStringLiteral(".7z") : QStringLiteral(".zip");
    const QString base = fi.isDir() ? fi.fileName() : fi.completeBaseName();
    const QString dest = uniqueName(fi.absolutePath(), base, suffix);
    const QStringList inputs{ path };
    const QString what = QStringLiteral("把 %1 打包成\n%2").arg(fi.fileName(), QDir::toNativeSeparators(dest));
    const auto task = [inputs, dest, sevenZip](const Archive::Report& rep, const Archive::Cancel& can) {
        return Archive::compress(inputs, dest, sevenZip, rep, can);
    };
    const QString title = QStringLiteral("压缩");
    return progressWindowOn() ? runArchiveTask(title, what, dest, task)
                              : runArchiveQuiet(title, what, task);
}

} // namespace

// 一张表就是"这个插件到底实现哪些动作"的唯一来源：路由表、plugin.json 里那份牌、
// 和插件自报的 actions() 三处都从这里长出来。以前 run() 里是一串 if，牌是另写的，
// 两边漂移了没人知道（用户撞上的那次就是"看着接了、其实判成失败"）。
namespace {

typedef int (*VerbFn)(const QString& path);

int vUsage(const QString& p) { return showUsage(p); }              // 占用分析 + 谁占用着它一个窗
int vWhohas(const QString& p) { return showUsage(p); }             // 老注册表项也进同一个窗，点了不落空
int vShred(const QString& p) { return showShred(p); }
int vExtract(const QString& p) { return showExtract(p, false); }
int vExtractSub(const QString& p) { return showExtract(p, true); }
int vZip(const QString& p) { return showCompress(p, false); }
int v7z(const QString& p) { return showCompress(p, true); }

struct Verb { const char* action; VerbFn fn; };

const Verb kVerbs[] = {
    { "fileinfo", vUsage },        { "whohas", vWhohas },          { "shred", vShred },
    { "extract", vExtract },       { "extract-sub", vExtractSub },
    { "compress-zip", vZip },      { "compress-7z", v7z },
};

} // namespace

namespace ShellAction {

QStringList implemented()
{
    QStringList out;
    for (const Verb& v : kVerbs)
        out << QLatin1String(v.action);
    return out;
}

int run(const QString& flag, const QString& path)
{
    const QString action = flag.startsWith(QLatin1String("--")) ? flag.mid(2) : flag;
    if (path.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("ltool"),
                             QStringLiteral("右键没传进来文件路径（%1）").arg(action));
        return 2;
    }
    Utils::log(QStringLiteral("shellmenu: run %1 on %2").arg(action, path));
    for (const Verb& v : kVerbs) {
        if (action == QLatin1String(v.action))
            return v.fn(path);
    }
    // 没有 compress-lzx：那套自造 .lzx 容器（清单 + 标准 xz 流）2026-10-12 就删了 ——
    // 实测它最快（0.48 秒 / 25.7MB），但别人拿到根本不知道是什么文件。
    // 走到这里 = 这条动作根本没实现，只有这个数能让主程序说"做不了"（见 shellaction.h）。
    QMessageBox::information(nullptr, QStringLiteral("ltool"),
                             QStringLiteral("这个右键动作还没接上：%1").arg(action));
    return kNotHandled;
}

} // namespace ShellAction
