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

// 系统服务默认不列（%SystemRoot% 下面的那些避免反客为主），用户勾选时才展开
bool isHiddenService(const StartupItem& it)
{
    if (it.source != StartupItem::Source::Service)
        return false;
    return !it.targetPath.contains(QLatin1Char(':'))
        || StartupAudit::isSystemService(it.targetPath, qEnvironmentVariable("SystemRoot"));
}

QString whenLabel(const StartupItem& it)
{
    switch (it.source) {
    case StartupItem::Source::RegistryRun:
        return it.note.contains(QLatin1String("RunOnce"))
            ? QStringLiteral("登录单次 (RunOnce)")
            : QStringLiteral("登录自动启动");
    case StartupItem::Source::StartupFolder:
        return QStringLiteral("登录启动文件夹");
    case StartupItem::Source::Service:
        return it.note.contains(QStringLiteral("延迟"))
            ? QStringLiteral("开机延迟启动")
            : QStringLiteral("开机自动启动");
    case StartupItem::Source::Task:
        if (it.note.contains(QStringLiteral("启动时")) && it.note.contains(QStringLiteral("登录时")))
            return QStringLiteral("开机/登录双触发");
        return it.note.contains(QStringLiteral("启动时")) ? QStringLiteral("系统启动触发")
                                                          : QStringLiteral("用户登录触发");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("进程外壳注入");
    }
    return {};
}

QString whoLabel(const StartupItem& it)
{
    if (!it.publisher.isEmpty())
        return it.publisher;
    return it.checkedSig ? QStringLiteral("无数字签名")
                         : QStringLiteral("未校验签名");
}

QString verdictLabel(const StartupItem& it)
{
    if (it.targetMissing)
        return QStringLiteral("目标路径失效 · 建议清理");
    if (!it.enabled)
        return it.approvedPath.isEmpty() ? QStringLiteral("已停用")
                                         : QStringLiteral("已停用 · 可重新启用");
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive))
        return QStringLiteral("微软系统原生组件");
    switch (it.source) {
    case StartupItem::Source::Service:
        return QStringLiteral("后台系统服务");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("资源管理器扩展项");
    default:
        return QStringLiteral("常规自启程序");
    }
}

QString plainGroup(StartupItem::Source s)
{
    switch (s) {
    case StartupItem::Source::RegistryRun:    return QStringLiteral("注册表自启 (Run)");
    case StartupItem::Source::StartupFolder:  return QStringLiteral("启动文件夹 (Startup)");
    case StartupItem::Source::Service:        return QStringLiteral("后台服务 (Service)");
    case StartupItem::Source::Task:           return QStringLiteral("计划任务 (Task)");
    case StartupItem::Source::ShellExtension: return QStringLiteral("外壳扩展 (ShellExtension)");
    }
    return {};
}

QString groupLabel(const QString& base, StartupItem::Source src, bool hideSysSvc)
{
    if (hideSysSvc && src == StartupItem::Source::Service)
        return base + QStringLiteral("，系统服务除外");
    return base;
}

// 徽章：系统化标准状态标签
QString shortVerdict(const StartupItem& it, InfoRow::Badge* kind)
{
    *kind = InfoRow::Neutral;
    if (it.targetMissing) {
        *kind = InfoRow::Bad;
        return QStringLiteral("路径失效");
    }
    if (!it.enabled) {
        *kind = InfoRow::Muted;
        return QStringLiteral("已禁用");
    }
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive)) {
        *kind = InfoRow::Good;
        return QStringLiteral("微软原生");
    }
    if (it.publisher.isEmpty()) {
        *kind = InfoRow::Warn;
        return QStringLiteral("未签名");
    }
    if (it.source == StartupItem::Source::Service) {
        *kind = InfoRow::Neutral;
        return QStringLiteral("系统服务");
    }
    *kind = InfoRow::Good;
    return QStringLiteral("正常自启");
}

} // namespace

StartupPage::StartupPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // 顶部标题与系统特性标签
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("开机自启动分析与治理"), this);
    QFont tf = title->font();
    tf.setPointSize(11);
    tf.setBold(true);
    title->setFont(tf);

    auto* tag1 = new QLabel(QStringLiteral("⚡ 安全两段切换"), this);
    tag1->setStyleSheet("color:#ea580c;background:rgba(234,88,12,0.08);border:1px solid rgba(234,88,12,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag2 = new QLabel(QStringLiteral("🛡️ 原生同步任务管理器"), this);
    tag2->setStyleSheet("color:#0284c7;background:rgba(2,132,199,0.08);border:1px solid rgba(2,132,199,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag3 = new QLabel(QStringLiteral("🔍 5 大自启源"), this);
    tag3->setStyleSheet("color:#7c3aed;background:rgba(124,58,237,0.08);border:1px solid rgba(124,58,237,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    titleRow->addWidget(title);
    titleRow->addWidget(tag1);
    titleRow->addWidget(tag2);
    titleRow->addWidget(tag3);
    titleRow->addStretch(1);
    lay->addLayout(titleRow);

    // 系统引导说明（精炼系统化，保留必要关键词）
    auto* tip = new QLabel(QStringLiteral(
        "系统 5 大自启动源排查（注册表、启动文件夹、后台服务、计划任务、外壳扩展）。"
        "识别失效目标与签名有效性；选中应用项可通过「禁用这一项」安全停用（原生同步任务管理器）。"), this);
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

    auto* tsMgr = new QPushButton(QStringLiteral("系统启动管理"), this);
    tsMgr->setCursor(Qt::PointingHandCursor);
    tsMgr->setToolTip(QStringLiteral("打开 Windows 任务管理器 / 设置中的启动项"));

    // 就地两段切换按钮
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

    // 过滤与控制栏
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(12);
    auto* filterLbl = new QLabel(QStringLiteral("类别："), this);
    filterLbl->setStyleSheet("font-size:12px;font-weight:500;");
    filterRow->addWidget(filterLbl);

    m_filter = new QComboBox(this);
    m_filter->setCursor(Qt::PointingHandCursor);
    m_filter->addItem(QStringLiteral("全部自启源"));
    for (const auto s : { StartupItem::Source::RegistryRun, StartupItem::Source::StartupFolder,
                          StartupItem::Source::Service, StartupItem::Source::Task,
                          StartupItem::Source::ShellExtension })
        m_filter->addItem(plainGroup(s), int(s));

    m_onlyProblems = new QCheckBox(QStringLiteral("只看待处理"), this);
    m_onlyProblems->setCursor(Qt::PointingHandCursor);

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
    m_summary = new QLabel(QStringLiteral("就绪。点击「开始分析」检测系统开机自启项目与安全状态。"), this);
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
    m_status = new QLabel(QStringLiteral("选中启动项查看详细注册位置、命令行与状态。"), this);
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
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        const int i = item->data(Qt::UserRole).toInt();
        if (i < 0 || i >= m_items.size())
            return;
        const StartupItem& it = m_items[i];
        if (it.command.isEmpty())
            return;
        QGuiApplication::clipboard()->setText(it.command);
        m_status->setText(QStringLiteral("已复制命令：%1").arg(it.command));
    });
    connect(m_toggleBtn, &QPushButton::clicked, this, &StartupPage::toggleSelected);
}

void StartupPage::startScan()
{
    if (m_busy)
        return;
    m_busy = true;
    m_scanBtn->setEnabled(false);
    m_status->setText(QStringLiteral("正在扫描自启动项（注册表、服务、计划任务、加载项）…"));
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
        m_summary->setText(QStringLiteral("分析完成：未检测到开机启动项。"));
        m_toggleBtn->setEnabled(false);
        return;
    }
    const int srcSel = m_filter->currentIndex();          // 0 = 全部自启源
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
        // 有问题的排前面，其余按名字排序
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
    m_summary->setText(QString("一共 %1 项 · 待处理 %2 项 · 已禁用 %3 项 · 未签名 %4 项 · 正常 %5 项%6")
                           .arg(countable).arg(missing).arg(disabled).arg(unsigned_).arg(normal)
                           .arg(hiddenSvc > 0
                                    ? QString("（另有 %1 条系统服务没列出，勾选「显示系统服务」可展开）")
                                          .arg(hiddenSvc)
                                    : QString()));
    m_status->setText(QStringLiteral("就绪：选中列表项可查看详细注册表路径、命令参数与启闭控制。双击行可快速复制启动命令。")
                      + (srcSel > 0 && m_filter->itemData(srcSel).toInt() == int(StartupItem::Source::Service)
                                && hiddenSvc > 0
                             ? QStringLiteral("\n（提示：当前分类为后台服务，系统服务默认是藏起来的，勾选「显示系统服务」可展开）")
                                   .arg(hiddenSvc)
                             : QString()));
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
    const bool canToggle = !it.approvedPath.isEmpty();

    QString text = QString("【%1】触发: %2  |  签名: %3  |  分类: %4\n"
                           "注册位置: %5\n"
                           "执行命令: %6")
                       .arg(it.name, whenLabel(it), whoLabel(it), plainGroup(it.source),
                            it.location,
                            it.command.isEmpty() ? QStringLiteral("（未提供命令行）") : it.command);
    if (!canToggle) {
        text += QStringLiteral("\n（提示：该类型为系统底层项，仅应用级项支持快捷开关，这一页不动它）");
    }
    m_status->setText(text);

    m_armed.clear();
    m_toggleBtn->setProperty("armed", false);
    m_toggleBtn->style()->unpolish(m_toggleBtn);
    m_toggleBtn->style()->polish(m_toggleBtn);

    m_toggleBtn->setEnabled(canToggle);
    m_toggleBtn->setText(it.enabled ? QStringLiteral("禁用这一项") : QStringLiteral("恢复这一项"));
    m_toggleBtn->setAccessibleName(m_toggleBtn->text());
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

    const QString want = it.enabled ? QStringLiteral("禁用") : QStringLiteral("恢复");
    if (m_armed != it.name) {
        m_armed = it.name;
        m_status->setText(QStringLiteral("确认要%1「%2」吗？再点一次该按钮即生效写入（同步任务管理器，可随时恢复）。")
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
        m_status->setText(QStringLiteral("%1「%2」失败：%3").arg(want, it.name, why));
        return;
    }
    it.enabled = !it.enabled;
    m_status->setText(QStringLiteral("已成功%1「%2」（已与 Windows 任务管理器保持同步）。")
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
