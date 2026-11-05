#include "StartupPage.h"
#include "platform/Launch.h"

#include "InfoRow.h"
#include "StartupAudit.h"
#include "Utils.h"

#include <windows.h>
#include <shellapi.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFrame>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

QPixmap iconForStartup(const StartupItem& it)
{
    static QHash<QString, QPixmap> s_cache;
    if (!it.targetPath.isEmpty()) {
        if (s_cache.contains(it.targetPath))
            return s_cache.value(it.targetPath);
        HICON hLarge = nullptr, hSmall = nullptr;
        if (ExtractIconExW((LPCWSTR)it.targetPath.utf16(), 0, &hLarge, &hSmall, 1) > 0) {
            HICON chosen = hSmall ? hSmall : hLarge;
            QImage img = QImage::fromHICON(chosen);
            if (hLarge) DestroyIcon(hLarge);
            if (hSmall && hSmall != hLarge) DestroyIcon(hSmall);
            if (!img.isNull()) {
                QPixmap pm = QPixmap::fromImage(img.scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                s_cache.insert(it.targetPath, pm);
                return pm;
            }
        }
    }
    const QString key = QStringLiteral("src_%1").arg(int(it.source));
    if (s_cache.contains(key))
        return s_cache.value(key);
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor col;
    QString sym;
    switch (it.source) {
    case StartupItem::Source::RegistryRun:
        col = QColor(249, 115, 22); sym = QStringLiteral("⚡"); break;
    case StartupItem::Source::StartupFolder:
        col = QColor(234, 179, 8); sym = QStringLiteral("📁"); break;
    case StartupItem::Source::Service:
        col = QColor(6, 182, 212); sym = QStringLiteral("⚙"); break;
    case StartupItem::Source::Task:
        col = QColor(168, 85, 247); sym = QStringLiteral("⏱"); break;
    case StartupItem::Source::ShellExtension:
        col = QColor(59, 130, 246); sym = QStringLiteral("🧩"); break;
    }
    p.setPen(QPen(col, 1));
    p.setBrush(QColor(col.red(), col.green(), col.blue(), 40));
    p.drawRoundedRect(QRectF(1, 1, 18, 18), 4, 4);
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(col);
    p.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, sym);
    s_cache.insert(key, pm);
    return pm;
}

bool isProblem(const StartupItem& it)
{
    return it.targetMissing || !it.enabled || (it.checkedSig && !it.hasSignature);
}

// 系统服务这条页默认不列（他 2026-10-21 点名）。一台机器上自动启动的服务几十上百条，
// 全在 %SystemRoot% 下面，混进来只会把"要看的那几条"挤掉。
// 没给出绝对路径的那些（svchost 那种裸名字）也归这里：这条页对它唯一能说的话就是
// "系统在开机时把它拉起来了"，那是所有服务的共同点，不是信息。
// 但"藏了一批"必须在汇总里点名，不许列表悄悄变短。
bool isHiddenService(const StartupItem& it)
{
    if (it.source != StartupItem::Source::Service)
        return false;
    return !it.targetPath.contains(QLatin1Char(':'))
        || StartupAudit::isSystemService(it.targetPath, qEnvironmentVariable("SystemRoot"));
}

// 下面三个函数只负责"把事实说成人话"：什么时候跑 / 谁签的名 / 要不要管。
// 结论全部从已有事实推出来，不去猜某个软件是干什么的。

QString whenLabel(const StartupItem& it)
{
    switch (it.source) {
    case StartupItem::Source::RegistryRun:
        return it.note.contains(QLatin1String("RunOnce"))
            ? QStringLiteral("下次登录时只跑一次")
            : QStringLiteral("每次登录以后");
    case StartupItem::Source::StartupFolder:
        return QStringLiteral("每次登录以后");
    case StartupItem::Source::Service:
        return it.note.contains(QStringLiteral("延迟"))
            ? QStringLiteral("开机后稍等一会儿自动起来")
            : QStringLiteral("开机就自动起来");
    case StartupItem::Source::Task:
        if (it.note.contains(QStringLiteral("启动时")) && it.note.contains(QStringLiteral("登录时")))
            return QStringLiteral("开机和登录时");
        return it.note.contains(QStringLiteral("启动时")) ? QStringLiteral("开机时")
                                                          : QStringLiteral("登录时");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("别的程序启动时被塞进去");
    }
    return {};
}

QString whoLabel(const StartupItem& it)
{
    if (!it.publisher.isEmpty())
        return it.publisher + QStringLiteral(" 签的名");
    return it.checkedSig ? QStringLiteral("没有数字签名")
                         : QStringLiteral("没查到签名（拿不到文件路径）");
}

QString verdictLabel(const StartupItem& it)
{
    if (it.targetMissing)
        return QStringLiteral("文件已经不在了 · 建议清掉（工具 → 注册表分析）");
    if (!it.enabled)
        return it.approvedPath.isEmpty() ? QStringLiteral("已经被关掉 · 不用管")
                                         : QStringLiteral("已经被关掉 · 选中它可以恢复");
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive))
        return QStringLiteral("微软的东西 · 建议留着");
    switch (it.source) {
    case StartupItem::Source::Service:
        return QStringLiteral("后台服务，不在桌面上抢开机时间 · 不认识就先留着");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("挂在别的进程里 · 出问题时才需要查它");
    default:
        // "没签名"这件事由 whoLabel 说，这里别重复一遍
        return QStringLiteral("正常自启动 · 选中它，点「禁用这一项」就能关");
    }
}

QString plainGroup(StartupItem::Source s)
{
    switch (s) {
    case StartupItem::Source::RegistryRun:    return QStringLiteral("登录以后自动启动的程序");
    case StartupItem::Source::StartupFolder:  return QStringLiteral("放在“启动”文件夹里的东西");
    case StartupItem::Source::Service:        return QStringLiteral("开机自动起来的后台服务");
    case StartupItem::Source::Task:           return QStringLiteral("计划任务里设了开机/登录触发的");
    case StartupItem::Source::ShellExtension: return QStringLiteral("塞进浏览器 / 资源管理器的加载项");
    }
    return {};
}

QString groupLabel(const QString& base, StartupItem::Source src, bool hideSysSvc)
{
    if (hideSysSvc && src == StartupItem::Source::Service)
        return base + QStringLiteral("，系统服务除外");
    return base;
}

// 徽章只放结论关键词，完整那句在选中后的下面一行里
QString shortVerdict(const StartupItem& it, InfoRow::Badge* kind)
{
    *kind = InfoRow::Neutral;
    if (it.targetMissing) {
        *kind = InfoRow::Bad;
        return QStringLiteral("文件没了 · 建议清");
    }
    if (!it.enabled) {
        *kind = InfoRow::Muted;
        return QStringLiteral("已关掉 · 不用管");
    }
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive)) {
        *kind = InfoRow::Good;
        return QStringLiteral("微软的 · 建议留着");
    }
    if (it.publisher.isEmpty()) {
        *kind = InfoRow::Warn;
        return QStringLiteral("没签名 · 自己判断");
    }
    if (it.source == StartupItem::Source::Service) {
        *kind = InfoRow::Good;
        return QStringLiteral("后台服务 · 不用管");
    }
    *kind = InfoRow::Good;
    return QStringLiteral("正常自启动");
}

} // namespace

StartupPage::StartupPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // 顶部标题与特性标签
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("开机自启动分析与治理"), this);
    QFont tf = title->font();
    tf.setPointSize(11);
    tf.setBold(true);
    title->setFont(tf);

    auto* tag1 = new QLabel(QStringLiteral("⚡ 就地两段安全切换"), this);
    tag1->setStyleSheet("color:#ea580c;background:rgba(234,88,12,0.08);border:1px solid rgba(234,88,12,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag2 = new QLabel(QStringLiteral("🛡️ 原生同步任务管理器"), this);
    tag2->setStyleSheet("color:#0284c7;background:rgba(2,132,199,0.08);border:1px solid rgba(2,132,199,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag3 = new QLabel(QStringLiteral("🔍 5 大自启源全量排查"), this);
    tag3->setStyleSheet("color:#7c3aed;background:rgba(124,58,237,0.08);border:1px solid rgba(124,58,237,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    titleRow->addWidget(title);
    titleRow->addWidget(tag1);
    titleRow->addWidget(tag2);
    titleRow->addWidget(tag3);
    titleRow->addStretch(1);
    lay->addLayout(titleRow);

    auto* tip = new QLabel(QStringLiteral(
        "这一页回答三个问题：开机后有东西自己起来吗？都是谁？哪些该管？\n"
        "每一行读法：软件名 — 什么时候跑 · 谁签的名 · 要不要管。"
        "红字是文件已经不在了的（建议清），灰字是已经被关掉的（不用管），黄字是没有数字签名的（自己判断）。\n"
        "选中「登录以后自动启动的程序」或「放在“启动”文件夹里的东西」那两栏里的某一行，点「禁用这一项」就能关掉它"
        "（要按两下，第一下只是确认）；已经关掉的选中后点「恢复这一项」就回来。"
        "写的就是任务管理器 · 启动 里那同一个开关，随时能反回来。服务、计划任务、加载项这一页不动。"), this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    lay->addWidget(tip);

    // 操作工具栏
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    m_scanBtn = new QPushButton(QStringLiteral("开始分析"), this);
    m_scanBtn->setObjectName(QStringLiteral("primary"));
    m_scanBtn->setCursor(Qt::PointingHandCursor);

    auto* reportBtn = new QPushButton(QStringLiteral("导出报告"), this);
    reportBtn->setCursor(Qt::PointingHandCursor);

    auto* tsMgr = new QPushButton(QStringLiteral("打开任务管理器的启动项设置"), this);
    tsMgr->setCursor(Qt::PointingHandCursor);

    // 就地两段，不弹模态框：这一页是从设置窗里翻进来的，模态框会被自藏的启动器吞掉
    //（那的表现就是"这个功能永久点了没反应"，他在那一侧吃过一次）。
    m_toggleBtn = new QPushButton(QStringLiteral("禁用这一项"), this);
    m_toggleBtn->setObjectName(QStringLiteral("startupToggle"));
    m_toggleBtn->setCursor(Qt::PointingHandCursor);
    m_toggleBtn->setEnabled(false);

    row->addWidget(m_scanBtn);
    row->addWidget(reportBtn);
    row->addWidget(tsMgr);
    row->addWidget(m_toggleBtn);
    row->addStretch(1);
    lay->addLayout(row);

    // 过滤选择栏
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(12);
    auto* filterLbl = new QLabel(QStringLiteral("只看哪一类："), this);
    filterLbl->setStyleSheet("font-size:12px;font-weight:500;");
    filterRow->addWidget(filterLbl);

    m_filter = new QComboBox(this);
    m_filter->setCursor(Qt::PointingHandCursor);
    m_filter->addItem(QStringLiteral("全部"));
    for (const auto s : { StartupItem::Source::RegistryRun, StartupItem::Source::StartupFolder,
                          StartupItem::Source::Service, StartupItem::Source::Task,
                          StartupItem::Source::ShellExtension })
        m_filter->addItem(plainGroup(s), int(s));

    m_onlyProblems = new QCheckBox(QStringLiteral("只看需要我处理的"), this);
    m_onlyProblems->setCursor(Qt::PointingHandCursor);

    // 系统服务默认不列（他 2026-10-21 点名）：一台机器上自动启动的服务几十上百条，
    // 全在 %SystemRoot% 下面，混在"谁开机自己起来"那一堆里只会把要看的东西挤掉。
    // 但不许"悄悄少一批" —— 这颗开关 + 汇总那句"另有 N 条没列出"就是留给这件事的出口。
    m_showSysSvc = new QCheckBox(QStringLiteral("显示系统服务"), this);
    m_showSysSvc->setObjectName(QStringLiteral("startupShowSysSvc"));
    m_showSysSvc->setCursor(Qt::PointingHandCursor);

    filterRow->addWidget(m_filter);
    filterRow->addWidget(m_onlyProblems);
    filterRow->addWidget(m_showSysSvc);
    filterRow->addStretch(1);
    lay->addLayout(filterRow);

    // 状态统计看板
    auto* sumRow = new QHBoxLayout;
    sumRow->setSpacing(6);
    auto* sumIcon = new QLabel(QStringLiteral("📊"), this);
    m_summary = new QLabel(QStringLiteral("点「开始分析」，把这台机器开机时会自己起来的东西列一遍，"
                                          "每一行都会告诉你它什么时候跑、谁家的、要不要管。"), this);
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setProperty("testId", "startupSummary");
    m_summary->setWordWrap(true);

    sumRow->addWidget(sumIcon);
    sumRow->addWidget(m_summary, 1);
    lay->addLayout(sumRow);

    m_list = new QListWidget(this);
    lay->addWidget(m_list, 1);

    // 底部详情栏
    auto* statRow = new QHBoxLayout;
    statRow->setSpacing(6);
    auto* statIcon = new QLabel(QStringLiteral("📍"), this);
    m_status = new QLabel(QStringLiteral("选中一行可以看它的完整注册位置和命令行。"), this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setProperty("testId", "startupStatus");
    m_status->setWordWrap(true);

    statRow->addWidget(statIcon);
    statRow->addWidget(m_status, 1);
    lay->addLayout(statRow);

    setStyleSheet("QPushButton#startupToggle[armed=\"true\"]{"
                  "background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #dc2626,stop:1 #ef4444);"
                  "color:#ffffff;font-weight:600;border:1px solid #ef4444;border-radius:8px;padding:6px 14px;}");

    connect(m_scanBtn, &QPushButton::clicked, this, &StartupPage::startScan);
    connect(reportBtn, &QPushButton::clicked, this, &StartupPage::exportReport);
    connect(tsMgr, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:startupapps")));
    });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { rebuildList(); });
    connect(m_onlyProblems, &QCheckBox::toggled, this, [this] { rebuildList(); });
    connect(m_showSysSvc, &QCheckBox::toggled, this, [this] { rebuildList(); });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const auto sel = m_list->selectedItems();
        if (sel.isEmpty())
            return;
        showSelected(sel.first()->data(Qt::UserRole).toInt());
    });
    connect(m_toggleBtn, &QPushButton::clicked, this, &StartupPage::toggleSelected);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int i = item->data(Qt::UserRole).toInt();
        if (i >= 0 && i < m_items.size()) {
            QGuiApplication::clipboard()->setText(m_items[i].command.isEmpty()
                                                      ? m_items[i].location
                                                      : m_items[i].command);
            m_status->setText(QStringLiteral("已把命令行复制到剪贴板"));
        }
    });
}

void StartupPage::startScan()
{
    if (m_busy)
        return;
    m_busy = true;
    m_scanBtn->setEnabled(false);
    m_status->setText(QStringLiteral("分析中…"));
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<QVector<StartupItem>>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            m_items = m_watcher->result();
            m_busy = false;
            m_scanBtn->setEnabled(true);
            rebuildList();
        });
    }
    m_watcher->setFuture(QtConcurrent::run([] { return StartupAudit::scan(); }));
}

void StartupPage::rebuildList()
{
    m_list->clear();
    m_armed.clear();
    if (m_items.isEmpty()) {
        m_summary->setText(QStringLiteral("还没有结果，或这台机器上一条启动项都没扫出来。"));
        m_toggleBtn->setEnabled(false);
        return;
    }
    const int srcSel = m_filter->currentIndex();          // 0 = 全部
    const bool hideSysSvc = !m_showSysSvc->isChecked();
    int missing = 0, unsigned_ = 0, disabled = 0, hiddenSvc = 0;
    for (const StartupItem& it : m_items) {
        if (hideSysSvc && isHiddenService(it)) {
            ++hiddenSvc;
            continue;
        }
        if (it.targetMissing)
            ++missing;
        if (it.checkedSig && !it.hasSignature)
            ++unsigned_;
        if (!it.enabled)
            ++disabled;
    }

    const auto sources = { StartupItem::Source::RegistryRun, StartupItem::Source::StartupFolder,
                           StartupItem::Source::Service, StartupItem::Source::Task,
                           StartupItem::Source::ShellExtension };
    for (const auto src : sources) {
        if (srcSel > 0 && int(src) != m_filter->itemData(srcSel).toInt())
            continue;
        QVector<int> rows;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i].source != src)
                continue;
            if (hideSysSvc && isHiddenService(m_items[i]))
                continue;
            rows.append(i);
        }
        if (rows.isEmpty())
            continue;
        // 有问题的排前面，其余按名字
        std::sort(rows.begin(), rows.end(), [this](int a, int b) {
            const bool pa = isProblem(m_items[a]), pb = isProblem(m_items[b]);
            if (pa != pb)
                return pa;
            return m_items[a].name.compare(m_items[b].name, Qt::CaseInsensitive) < 0;
        });
        m_list->addItem([this] {
            auto* s = new QListWidgetItem();
            s->setFlags(Qt::NoItemFlags);
            s->setSizeHint(QSize(0, 32));
            s->setForeground(QColor(148, 163, 184));
            QFont f = s->font();
            f.setBold(true);
            f.setPointSize(9);
            s->setFont(f);
            return s;
        }());
        m_list->item(m_list->count() - 1)
            ->setText(QString("── %1（%2）──")
                          .arg(groupLabel(plainGroup(src), src, hideSysSvc))
                          .arg(rows.size()));
        for (const int i : rows) {
            const StartupItem& it = m_items[i];
            if (m_onlyProblems->isChecked() && !isProblem(it))
                continue;
            auto* item = new QListWidgetItem();
            item->setData(Qt::UserRole, i);
            item->setSizeHint(QSize(0, 48));
            InfoRow::Badge kind = InfoRow::Neutral;
            const QString badge = shortVerdict(it, &kind);
            auto* row = new InfoRow(it.name,
                                    QStringLiteral("%1 · %2").arg(whenLabel(it), whoLabel(it)),
                                    badge, kind);
            row->setIconPixmap(iconForStartup(it));
            m_list->addItem(item);
            m_list->setItemWidget(item, row);
        }
    }
    int normal = 0, countable = 0;
    for (const StartupItem& it : m_items) {
        if (hideSysSvc && isHiddenService(it))
            continue;
        ++countable;
        if (!isProblem(it))
            ++normal;
    }
    m_summary->setText(QString("一共 %1 项。需要处理的 %2 项（文件已经不在了，红字）· "
                               "已经被关掉的 %3 项（灰字）· 没签名的 %4 项（黄字）· 其余 %5 项正常。%6")
                           .arg(countable).arg(missing).arg(disabled).arg(unsigned_).arg(normal)
                           .arg(hiddenSvc > 0
                                        ? QString(" 另有 %1 条系统服务没列出（它们是 Windows 自己开机要起的，"
                                                  "这页改不了；想看就勾上「显示系统服务」）。")
                                              .arg(hiddenSvc)
                                        : QString()));
    m_status->setText(QStringLiteral("想关掉某项：选中它，点「禁用这一项」（要按两下，第一下只是确认）。"
                                     "文件已经不在了的那些，可以在 工具 → 注册表分析 里连记录一起清掉。"
                                     "双击一行 = 复制它的命令行。")
                      + (srcSel > 0 && m_filter->itemData(srcSel).toInt() == int(StartupItem::Source::Service)
                                && hiddenSvc > 0
                             ? QStringLiteral("\n你只看「后台服务」这一类，而系统服务默认是藏起来的（%1 条）："
                                              "勾上「显示系统服务」它们就出来。")
                                   .arg(hiddenSvc)
                             : QString()));
    // 列表刚重建，选中已经没了 —— 那颗按钮不能留着上一轮的状态
    m_toggleBtn->setEnabled(false);
    m_toggleBtn->setText(QStringLiteral("禁用这一项"));
    m_toggleBtn->setProperty("armed", false);
    m_toggleBtn->style()->unpolish(m_toggleBtn);
    m_toggleBtn->style()->polish(m_toggleBtn);
}

void StartupPage::showSelected(int i)
{
    if (i < 0 || i >= m_items.size())
        return;
    const StartupItem& it = m_items[i];
    m_status->setText(QStringLiteral("%1 — %2 · %3 · %4\n从哪来的：%5\n它要执行的命令：%6")
                          .arg(it.name, whenLabel(it), whoLabel(it), verdictLabel(it), it.location,
                               it.command.isEmpty() ? QStringLiteral("（看不到命令行）") : it.command));
    m_armed.clear();
    m_toggleBtn->setProperty("armed", false);
    m_toggleBtn->style()->unpolish(m_toggleBtn);
    m_toggleBtn->style()->polish(m_toggleBtn);
    // 只有记到了 StartupApproved 那条路径的（注册表 Run / 启动文件夹）能由这页开关。
    const bool canToggle = !it.approvedPath.isEmpty();
    m_toggleBtn->setEnabled(canToggle);
    m_toggleBtn->setText(it.enabled ? QStringLiteral("禁用这一项") : QStringLiteral("恢复这一项"));
    m_toggleBtn->setAccessibleName(m_toggleBtn->text());
    if (!canToggle)
        m_status->setText(m_status->text() + QStringLiteral(
                              "\n这一项不在「任务管理器 → 启动」那套开关里（服务 / 计划任务 / 加载项），"
                              "这一页不动它。"));
}

void StartupPage::toggleSelected()
{
    const auto sel = m_list->selectedItems();
    if (sel.isEmpty())
        return;
    const int i = sel.first()->data(Qt::UserRole).toInt();
    if (i < 0 || i >= m_items.size())
        return;
    StartupItem& it = m_items[i];
    if (it.approvedPath.isEmpty()) {
        m_status->setText(StartupAudit::setStartupEnabled(it, !it.enabled));
        return;
    }

    // 就地两段确认：第一下把按钮换成"确认要关 X 吗"，第二下才真写注册表。
    // 不弹模态框 —— 这一页是从设置窗里翻进来的，模态框会被自藏的启动器吞掉。
    const QString want = it.enabled ? QStringLiteral("禁用") : QStringLiteral("恢复");
    if (m_armed != it.name) {
        m_armed = it.name;
        m_status->setText(QStringLiteral("确认要%1「%2」吗？再点一次这个按钮就写注册表"
                                         "（这是任务管理器 · 启动 里的那个开关，随时能反回来）。")
                              .arg(want, it.name));
        m_toggleBtn->setText(QStringLiteral("再点一次确认%1").arg(want));
        m_toggleBtn->setProperty("armed", true);
        m_toggleBtn->style()->unpolish(m_toggleBtn);
        m_toggleBtn->style()->polish(m_toggleBtn);
        return;
    }
    m_armed.clear();
    m_toggleBtn->setProperty("armed", false);
    m_toggleBtn->style()->unpolish(m_toggleBtn);
    m_toggleBtn->style()->polish(m_toggleBtn);
    const QString why = StartupAudit::setStartupEnabled(it, !it.enabled);
    if (!why.isEmpty()) {
        m_toggleBtn->setText(it.enabled ? QStringLiteral("禁用这一项") : QStringLiteral("恢复这一项"));
        m_status->setText(QStringLiteral("%1「%2」没改成：%3").arg(want, it.name, why));
        return;
    }
    it.enabled = !it.enabled;
    m_status->setText(QStringLiteral("已%1「%2」：读回来核对过了，任务管理器 · 启动 里看到的也是这个状态。")
                          .arg(want, it.name));
    rebuildList();
}

void StartupPage::exportReport()
{
    if (m_items.isEmpty()) {
        m_status->setText(QStringLiteral("还没有分析结果可导出。"));
        return;
    }
    const QString path = QString("%1/startup-report.txt").arg(Utils::dataDir());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_status->setText(QStringLiteral("写报告失败：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QStringLiteral("ltool 开机启动分析报告\n\n");
    for (const StartupItem& it : m_items) {
        ts << QString("· %1\n  来源：%2  状态：%3\n  位置：%4\n  命令：%5\n  签名：%6\n\n")
                  .arg(it.name, StartupAudit::sourceLabel(it.source),
                       it.enabled ? QStringLiteral("启用") : QStringLiteral("已禁用"),
                       it.location, it.command,
                       it.publisher.isEmpty()
                           ? (it.checkedSig ? QStringLiteral("无") : QStringLiteral("未检查"))
                           : QString("%1（校验%2）").arg(it.publisher,
                                                         it.signatureValid ? "通过" : "未通过"));
    }
    f.close();
    Platform::reveal(path);
    m_status->setText(QStringLiteral("报告已导出：%1").arg(path));
}
