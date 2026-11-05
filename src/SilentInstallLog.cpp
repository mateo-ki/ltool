#include "SilentInstallLog.h"
#include "platform/ProcInfo.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "Theme.h"
#include "Utils.h"

namespace {

constexpr int kMaxJobs = 30;
constexpr int kMaxLogChars = 200000;   // 单个任务的输出上限，超了丢头部

QPointer<SilentInstallLog> g_win;
QHash<int, QPointer<QProcess>> g_procs;   // jobId → 正在跑的安装进程（取消用）

QString stateText(int code, bool cancelled = false)
{
    if (cancelled)
        return QStringLiteral("已取消");
    if (code < 0)
        return QStringLiteral("进行中…");
    if (code == 0)
        return QStringLiteral("完成");
    if (quint32(code) == 0x8A15002B)
        return QStringLiteral("已装·无升级");
    return QString("0x%1").arg(quint32(code), 8, 16, QLatin1Char('0'));
}

QString outcomeText(int code, bool cancelled = false)
{
    if (cancelled)
        return QStringLiteral("已取消安装");
    if (code == 0)
        return QStringLiteral("安装完成");
    // winget 认为"这个版本已经装好了、没有可升级的"：不是失败
    if (quint32(code) == 0x8A15002B)
        return QStringLiteral("已安装且没有可升级的版本");
    return QStringLiteral("安装失败");
}

} // namespace

SilentInstallLog::SilentInstallLog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("静默安装日志");
    setModal(false);
    resize(780, 460);
    // 这扇窗是启动时建一次、之后反复 show/hide 复用的，所以走 Theme::bind：
    // 直接 setStyleSheet 的话，他拨完「日间」再打开它，里面还是一片黑。
    Theme::bind(this, R"QSS(
QDialog, QWidget { background: @window; }
QLabel { color: @text; background: transparent; }
QLabel#muted { color: @muted; }
QListWidget {
    background: @base; border: 1px solid @line; border-radius: 8px;
    color: @text; outline: none;
}
QListWidget::item { padding: 6px 8px; border-radius: 6px; }
QListWidget::item:selected { background: @selBg; }
QPlainTextEdit {
    background: @base; border: 1px solid @line; border-radius: 8px;
    color: @okText; font-family: Consolas, monospace; font-size: 9pt;
}
QPushButton {
    background: @button; color: @text; border: 1px solid @borderHi;
    border-radius: 8px; padding: 6px 14px;
}
QPushButton:hover { background: @hover; }
)QSS");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    m_head = new QLabel(this);
    m_head->setObjectName("muted");
    m_head->setWordWrap(true);
    m_head->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_head);

    auto* split = new QHBoxLayout;
    split->setSpacing(8);
    m_jobList = new QListWidget(this);
    m_jobList->setMaximumWidth(260);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(4000);
    split->addWidget(m_jobList, 0);
    split->addWidget(m_log, 1);
    lay->addLayout(split, 1);

    auto* btns = new QHBoxLayout;
    auto* openDirBtn = new QPushButton("打开安装目录", this);
    auto* closeBtn = new QPushButton("关闭", this);
    m_cancel = new QPushButton("取消安装", this);
    closeBtn->setDefault(true);
    btns->addStretch(1);
    btns->addWidget(m_cancel);
    btns->addWidget(openDirBtn);
    btns->addWidget(closeBtn);
    lay->addLayout(btns);

    connect(m_jobList, &QListWidget::currentRowChanged, this, &SilentInstallLog::selectRow);
    connect(closeBtn, &QPushButton::clicked, this, &SilentInstallLog::hide);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_shownJob >= 0)
            cancel(m_shownJob);
    });
    connect(openDirBtn, &QPushButton::clicked, this, [this] {
        const int i = indexOf(m_shownJob);
        if (i < 0 || jobs()[i].dir.isEmpty())
            return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(jobs()[i].dir));
    });

    rebuildJobs();
    if (!jobs().isEmpty())
        selectRow(rowOf(jobs().last().id));
    else
        m_head->setText("还没有后台安装任务。语言页/软件页点「安装」或选版本后会出现在这里。");
}

QVector<SilentInstallLog::Job>& SilentInstallLog::jobs()
{
    static QVector<Job> v;
    return v;
}

int SilentInstallLog::indexOf(int jobId)
{
    const QVector<Job>& v = jobs();
    for (int i = 0; i < v.size(); ++i)
        if (v[i].id == jobId)
            return i;
    return -1;
}

int SilentInstallLog::rowOf(int jobId) const
{
    for (int i = 0; i < m_jobList->count(); ++i)
        if (m_jobList->item(i)->data(Qt::UserRole).toInt() == jobId)
            return i;
    return -1;
}

void SilentInstallLog::rebuildJobs()
{
    const int keep = m_shownJob;
    m_jobList->blockSignals(true);
    m_jobList->clear();
    for (const Job& j : jobs()) {
        auto* it = new QListWidgetItem(QString("%1\n%2").arg(stateText(j.code, j.cancelled), j.title), m_jobList);
        it->setData(Qt::UserRole, j.id);
        it->setToolTip(j.command);
    }
    m_jobList->blockSignals(false);
    const int row = rowOf(keep);
    if (row >= 0)
        m_jobList->setCurrentRow(row);
    refreshCancel();
}

void SilentInstallLog::refreshCancel()
{
    // 只有选中的任务还在跑才允许取消，否则按钮是个假动作
    const int i = indexOf(m_shownJob);
    m_cancel->setEnabled(i >= 0 && jobs()[i].code < 0);
}

void SilentInstallLog::selectRow(int row)
{
    QListWidgetItem* it = m_jobList->item(row);
    const int i = indexOf(it ? it->data(Qt::UserRole).toInt() : -1);
    if (i < 0)
        return;
    const Job& j = jobs()[i];
    m_shownJob = j.id;
    m_jobList->blockSignals(true);
    m_jobList->setCurrentRow(row);
    m_jobList->blockSignals(false);
    m_head->setText(QString("%1    %2\n%3").arg(j.title, stateText(j.code, j.cancelled), j.command));
    m_log->setPlainText(j.log);
    refreshCancel();
}

int SilentInstallLog::begin(const QString& title, const QString& dir, const QString& command)
{
    static int nextId = 0;
    Job j;
    j.id = nextId++;
    j.title = title;
    j.dir = dir;
    j.command = command;
    j.log = QString("$ %1\n").arg(command);
    jobs().append(j);
    while (jobs().size() > kMaxJobs)
        jobs().remove(0);
    if (g_win)
        g_win->rebuildJobs();
    return j.id;
}

void SilentInstallLog::showText(int jobId, const QString& text)
{
    if (text.isEmpty())
        return;
    const int i = indexOf(jobId);
    if (i < 0)
        return;
    Job& j = jobs()[i];
    j.log += text;
    if (j.log.size() > kMaxLogChars)
        j.log = j.log.right(kMaxLogChars);
    if (g_win && g_win->m_shownJob == jobId)
        g_win->m_log->appendPlainText(text.trimmed());
}

void SilentInstallLog::append(int jobId, const QByteArray& chunk)
{
    const int i = indexOf(jobId);
    if (i < 0 || chunk.isEmpty())
        return;
    showText(jobId, jobs()[i].lines.feed(chunk));
}

void SilentInstallLog::finish(int jobId, int exitCode)
{
    const int i = indexOf(jobId);
    if (i < 0)
        return;
    const bool cancelled = jobs()[i].cancelled;
    showText(jobId, jobs()[i].lines.tail());   // 收尾：最后一行可能没有换行符
    showText(jobId, QString("=== %1（退出码 %2 / 0x%3）===\n")
                             .arg(outcomeText(exitCode, cancelled), QString::number(exitCode),
                                  QString::number(quint32(exitCode), 16)));
    jobs()[i].code = exitCode;
    g_procs.remove(jobId);
    Utils::log(QString("install job #%1 exit=%2%3").arg(jobId).arg(exitCode)
                   .arg(cancelled ? " cancelled" : ""));
    if (!g_win)
        return;
    g_win->rebuildJobs();
    if (g_win->m_shownJob == jobId)
        g_win->selectRow(g_win->rowOf(jobId));
}

void SilentInstallLog::attachProcess(int jobId, QProcess* proc)
{
    if (jobId < 0 || !proc)
        return;
    g_procs.insert(jobId, proc);
    // 进程自己没了（崩溃/被外部杀掉）也要把这张表清干净，不然 busy() 一直转圈
    QObject::connect(proc, &QProcess::finished, proc, [jobId] { g_procs.remove(jobId); });
}

int SilentInstallLog::busyCount()
{
    int n = 0;
    for (const Job& j : jobs())
        if (j.code < 0 && g_procs.contains(j.id))
            ++n;
    return n;
}

bool SilentInstallLog::busy()
{
    return busyCount() > 0;
}

bool SilentInstallLog::wasCancelled(int jobId)
{
    const int i = indexOf(jobId);
    return i >= 0 && jobs()[i].cancelled;
}

bool SilentInstallLog::cancel(int jobId)
{
    const int i = indexOf(jobId);
    if (i < 0 || jobs()[i].code >= 0)
        return false;
    jobs()[i].cancelled = true;
    QPointer<QProcess> proc = g_procs.value(jobId);
    const qint64 pid = proc ? proc->processId() : 0;
    showText(jobId, "正在取消：终止安装进程及其子进程…\n");
    // winget 只是壳，真正在写文件的是它拉起的 msiexec / setup.exe，必须连树一起杀。
    // 那些进程往往已经提权，这里可能杀不动 —— 如实写进日志，别假装取消了。
    const bool killed = pid && Platform::killProcessTree(pid);
    if (!killed)
        showText(jobId, "提示：没能终止安装进程（多半是安装器已经提权在跑）。"
                        "已标记为取消，但这一次安装可能还会继续到结束，"
                        "请在弹出的安装向导里点取消，或以管理员身份运行 ltool 后再取消。\n");
    g_procs.remove(jobId);
    if (proc)
        proc->kill();   // 壳进程自己：让 finished 信号正常触发，队列才能往下走
    if (g_win) {
        g_win->rebuildJobs();
        if (g_win->m_shownJob == jobId)
            g_win->selectRow(g_win->rowOf(jobId));
    }
    return killed;
}

void SilentInstallLog::showLog(int jobId)
{
    if (!g_win) {
        // 设置页开着时挂成它的子窗口：日志窗压在设置页之上；设置页是模态的，
        // 只有它自己那一族的窗口才收得到点击。设置页销毁时这里跟着销毁也不丢历史，
        // 任务与输出都在静态 jobs() 里，下次打开重建视图并选回最后一个任务。
        QWidget* host = nullptr;
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget* w : tops) {
            if (w && w->isVisible() && w->inherits("SettingsDialog"))
                host = w;
        }
        g_win = new SilentInstallLog(host);
    }
    if (jobId < 0 && !jobs().isEmpty())
        jobId = jobs().last().id;
    if (jobId >= 0) {
        const int row = g_win->rowOf(jobId);
        if (row >= 0)
            g_win->selectRow(row);
    }
    g_win->show();
    g_win->raise();
    g_win->activateWindow();
}
