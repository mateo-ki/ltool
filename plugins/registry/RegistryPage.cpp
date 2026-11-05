#include "RegistryPage.h"
#include "platform/Launch.h"

#include "InfoRow.h"
#include "RegClean.h"
#include "Utils.h"

#include <algorithm>

#include <QDesktopServices>
#include <QFile>
#include <QFrame>
#include <QFutureWatcher>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// 分组顺序：删了最没风险的排前面，只提示的排最后
const RegIssue::Kind kOrder[] = {
    RegIssue::Kind::UninstallOrphan, RegIssue::Kind::RunOrphan, RegIssue::Kind::AppPathsOrphan,
    RegIssue::Kind::MuiCacheOrphan,  RegIssue::Kind::ServiceOrphan, RegIssue::Kind::DeadDrive,
    RegIssue::Kind::LegacyKey,       RegIssue::Kind::AssociationOrphan, RegIssue::Kind::ComOrphan,
    RegIssue::Kind::PathEntry,
};

QString kindLabel(RegIssue::Kind kind)
{
    RegIssue it;
    it.kind = kind;
    return RegClean::categoryLabel(it);
}

bool kindDeletable(RegIssue::Kind kind)
{
    RegIssue it;
    it.kind = kind;
    return it.deletable();
}

QIcon categoryIcon(RegIssue::Kind kind)
{
    static QHash<RegIssue::Kind, QIcon> s_cache;
    if (s_cache.contains(kind))
        return s_cache.value(kind);

    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bg, fg;
    QString sym;
    switch (kind) {
    case RegIssue::Kind::UninstallOrphan:
        bg = QColor(244, 63, 94, 35); fg = QColor(244, 63, 94); sym = QStringLiteral("🗑"); break;
    case RegIssue::Kind::RunOrphan:
        bg = QColor(249, 115, 22, 35); fg = QColor(249, 115, 22); sym = QStringLiteral("⚡"); break;
    case RegIssue::Kind::AppPathsOrphan:
        bg = QColor(56, 189, 248, 35); fg = QColor(56, 189, 248); sym = QStringLiteral("↗"); break;
    case RegIssue::Kind::MuiCacheOrphan:
        bg = QColor(234, 179, 8, 35); fg = QColor(234, 179, 8); sym = QStringLiteral("⚡"); break;
    case RegIssue::Kind::ServiceOrphan:
        bg = QColor(6, 182, 212, 35); fg = QColor(6, 182, 212); sym = QStringLiteral("⚙"); break;
    case RegIssue::Kind::DeadDrive:
        bg = QColor(100, 116, 139, 35); fg = QColor(148, 163, 184); sym = QStringLiteral("💾"); break;
    case RegIssue::Kind::LegacyKey:
        bg = QColor(120, 113, 108, 35); fg = QColor(168, 162, 158); sym = QStringLiteral("⌛"); break;
    case RegIssue::Kind::AssociationOrphan:
        bg = QColor(99, 102, 241, 35); fg = QColor(129, 140, 248); sym = QStringLiteral("🔗"); break;
    case RegIssue::Kind::ComOrphan:
        bg = QColor(168, 85, 247, 35); fg = QColor(192, 132, 252); sym = QStringLiteral("🧩"); break;
    case RegIssue::Kind::PathEntry:
        bg = QColor(20, 184, 166, 35); fg = QColor(45, 212, 191); sym = QStringLiteral("📁"); break;
    }

    p.setPen(QPen(fg, 1.2));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

    QFont f = p.font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(QRect(0, 0, 24, 24), Qt::AlignCenter, sym);

    QIcon ic(pm);
    s_cache.insert(kind, ic);
    return ic;
}

QListWidgetItem* sectionItem(const QString& text)
{
    auto* it = new QListWidgetItem(text);
    it->setFlags(Qt::NoItemFlags);
    it->setSizeHint(QSize(0, 32));
    it->setForeground(QColor(148, 163, 184));
    QFont f = it->font();
    f.setBold(true);
    f.setPointSize(9);
    it->setFont(f);
    return it;
}

} // namespace

RegistryPage::RegistryPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // 顶部标题与特性标签
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("注册表失效残留体检"), this);
    QFont tf = title->font();
    tf.setPointSize(11);
    tf.setBold(true);
    title->setFont(tf);

    auto* tag1 = new QLabel(QStringLiteral("🛡️ 自动 .reg 备份"), this);
    tag1->setStyleSheet("color:#0284c7;background:rgba(2,132,199,0.08);border:1px solid rgba(2,132,199,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag2 = new QLabel(QStringLiteral("⚡ 双击即还原"), this);
    tag2->setStyleSheet("color:#059669;background:rgba(5,150,105,0.08);border:1px solid rgba(5,150,105,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag3 = new QLabel(QStringLiteral("🔍 9 类死键排查"), this);
    tag3->setStyleSheet("color:#7c3aed;background:rgba(124,58,237,0.08);border:1px solid rgba(124,58,237,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    titleRow->addWidget(title);
    titleRow->addWidget(tag1);
    titleRow->addWidget(tag2);
    titleRow->addWidget(tag3);
    titleRow->addStretch(1);
    lay->addLayout(titleRow);

    auto* tip = new QLabel(QStringLiteral(
        "分析注册表里「指向已经不存在的文件 / 盘符」的项，以及 9x 时代留下来的死键："
        "卸载残留、启动项、App Paths、文件关联、COM 组件、服务、死盘符、PATH 失效目录。\n"
        "删除前会把原值导出成 .reg 备份到 backups\\，双击那个文件就能还原。"
        "PATH 里的某一段这类只给提示，不做整值删除。"), this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    lay->addWidget(tip);

    // 操作工具栏
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    m_scanBtn = new QPushButton(QStringLiteral("开始分析"), this);
    m_scanBtn->setObjectName(QStringLiteral("primary"));
    m_scanBtn->setCursor(Qt::PointingHandCursor);

    m_delBtn = new QPushButton(QStringLiteral("备份并删除所选"), this);
    m_delBtn->setObjectName(QStringLiteral("delBtn"));
    m_delBtn->setCursor(Qt::PointingHandCursor);
    m_delBtn->setEnabled(false);

    auto* reportBtn = new QPushButton(QStringLiteral("导出报告"), this);
    reportBtn->setCursor(Qt::PointingHandCursor);

    auto* backupBtn = new QPushButton(QStringLiteral("打开备份目录"), this);
    backupBtn->setCursor(Qt::PointingHandCursor);

    row->addWidget(m_scanBtn);
    row->addWidget(m_delBtn);
    row->addWidget(reportBtn);
    row->addWidget(backupBtn);
    row->addStretch(1);
    lay->addLayout(row);

    // 状态统计看板
    auto* sumRow = new QHBoxLayout;
    sumRow->setSpacing(6);
    auto* sumIcon = new QLabel(QStringLiteral("📊"), this);
    m_summary = new QLabel(QStringLiteral("还没分析。COM 组件那一类要遍历上万个键，大约 1~3 秒。"), this);
    m_summary->setObjectName(QStringLiteral("muted"));
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
    m_status = new QLabel(QStringLiteral("就绪。点击「开始分析」检测系统无效注册表项。"), this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);

    statRow->addWidget(statIcon);
    statRow->addWidget(m_status, 1);
    lay->addLayout(statRow);

    setStyleSheet("QPushButton#delBtn:enabled{"
                  "background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #dc2626,stop:1 #ef4444);"
                  "color:#ffffff;font-weight:600;border:1px solid #ef4444;border-radius:8px;padding:6px 14px;}"
                  "QPushButton#delBtn:enabled:hover{background:#ef4444;}");

    connect(m_scanBtn, &QPushButton::clicked, this, &RegistryPage::startScan);
    connect(m_delBtn, &QPushButton::clicked, this, &RegistryPage::deleteSelected);
    connect(reportBtn, &QPushButton::clicked, this, &RegistryPage::exportReport);
    connect(backupBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Utils::backupsDir()));
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const auto sel = m_list->selectedItems();
        if (sel.isEmpty())
            return;
        const int i = sel.first()->data(Qt::UserRole).toInt();
        if (i >= 0 && i < m_issues.size()) {
            const RegIssue& it = m_issues[i];
            m_status->setText(QString("%1\\%2\n%3  值：%4\n%5")
                                  .arg(it.hiveName, it.subKey, RegClean::categoryLabel(it),
                                       it.valueName.isEmpty() ? QStringLiteral("（默认）") : it.valueName,
                                       it.detail));
        }
    });
}

void RegistryPage::startScan()
{
    if (m_busy)
        return;
    m_busy = true;
    m_scanBtn->setEnabled(false);
    m_delBtn->setEnabled(false);
    m_status->setText(QStringLiteral("分析中…（COM 组件那一类最慢）"));
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<QVector<RegIssue>>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            m_issues = m_watcher->result();
            m_busy = false;
            m_scanBtn->setEnabled(true);
            rebuildList();
        });
    }
    m_watcher->setFuture(QtConcurrent::run([] { return RegClean::scan(); }));
}

void RegistryPage::rebuildList()
{
    m_list->clear();
    m_checked.clear();
    QStringList heads;
    for (const RegIssue::Kind kind : kOrder) {
        QVector<int> rows;
        for (int i = 0; i < m_issues.size(); ++i)
            if (m_issues[i].kind == kind)
                rows.append(i);
        if (rows.isEmpty())
            continue;
        const QString label = QString("── %1（%2）──").arg(kindLabel(kind)).arg(rows.size());
        m_list->addItem(sectionItem(label));
        heads << QString("%1 %2").arg(kindLabel(kind)).arg(rows.size());
        for (const int i : rows) {
            const RegIssue& it = m_issues[i];
            auto* item = new QListWidgetItem();
            item->setData(Qt::UserRole, i);
            item->setSizeHint(QSize(0, 46));

            InfoRow::Badge kind = InfoRow::Neutral;
            QString badge;
            if (it.risk == RegIssue::Risk::Safe) { kind = InfoRow::Good; badge = QStringLiteral("可安全删"); }
            else if (it.risk == RegIssue::Risk::Careful) { kind = InfoRow::Warn; badge = QStringLiteral("谨慎删"); }
            else { kind = InfoRow::Muted; badge = QStringLiteral("只提示"); }
            auto* row = new InfoRow(it.title,
                                    QStringLiteral("%1 · %2").arg(RegClean::categoryLabel(it), it.detail),
                                    badge, kind, categoryIcon(it.kind), 30);
            row->setToolTip(QString("%1\\%2\n%3").arg(it.hiveName, it.subKey, RegClean::riskLabel(it))
                            + (it.badPath.isEmpty() ? QString() : QString("\n失效目标：%1").arg(it.badPath)));
            if (it.deletable()) {
                row->makeCheckable(false);
                connect(row, &InfoRow::checkToggled, this, [this, i](bool on) {
                    if (on)
                        m_checked.insert(i);
                    else
                        m_checked.remove(i);
                    updateDeleteBtn();
                });
            }
            m_list->addItem(item);
            m_list->setItemWidget(item, row);
        }
    }
    updateDeleteBtn();
    // 按类别汇总，顺手把"只提示"的单独说明，免得用户以为工具不干活
    if (m_issues.isEmpty()) {
        m_summary->setText(QStringLiteral("分析完成：没有发现失效项或遗留项。"));
        m_status->setText(QStringLiteral("没有可删的项。"));
    } else {
        int readOnly = 0;
        for (const RegIssue& it : m_issues)
            if (!kindDeletable(it.kind))
                ++readOnly;
        m_summary->setText(heads.join(QStringLiteral(" · "))
            + (readOnly > 0
                   ? QString(" · 其中 %1 项只提示（不能整值删除）").arg(readOnly)
                   : QString()));
        m_status->setText(QString("共 %1 项 · 勾选后要删除的，删除前自动备份").arg(m_issues.size()));
    }
}

void RegistryPage::updateDeleteBtn()
{
    m_delBtn->setEnabled(!m_checked.isEmpty() && !m_busy);
    if (!m_checked.isEmpty()) {
        m_delBtn->setText(QStringLiteral("备份并删除所选 (%1)").arg(m_checked.size()));
        if (!m_busy)
            m_status->setText(QString("已选 %1 项 · 删除前会先把原值导出成 .reg 备份到 backups\\").arg(m_checked.size()));
    } else {
        m_delBtn->setText(QStringLiteral("备份并删除所选"));
    }
}

void RegistryPage::deleteSelected()
{
    QVector<int> picked;
    for (const int i : m_checked)
        picked.append(i);
    std::sort(picked.begin(), picked.end());
    if (picked.isEmpty()) {
        m_status->setText(QStringLiteral("先勾上要处理的项。"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
            QString("要删除 %1 项注册表记录，删除前会先把原值导出成 .reg 备份。\n\n"
                    "HKLM / HKU 下的项需要管理员权限，没权限的会报失败并留在列表里。\n确定继续吗？")
                .arg(picked.size()))
        != QMessageBox::Yes)
        return;

    int ok = 0;
    QStringList fails;
    QSet<int> gone;
    for (const int i : picked) {
        QString err;
        if (RegClean::deleteIssue(m_issues[i], &err)) {
            ++ok;
            gone.insert(i);
        } else {
            fails << QString("%1：%2").arg(m_issues[i].title, err);
        }
    }
    QVector<RegIssue> left;
    for (int i = 0; i < m_issues.size(); ++i)
        if (!gone.contains(i))
            left.append(m_issues[i]);
    m_issues = left;
    rebuildList();

    QString msg = QString("已删除 %1/%2 项 · 备份在 backups\\registry-backup-*.reg")
                      .arg(ok).arg(picked.size());
    if (!fails.isEmpty())
        msg += QString(" · 失败 %1 项（%2）").arg(fails.size()).arg(fails.first());
    m_status->setText(msg);
    Utils::log(QString("registry page: deleted %1/%2").arg(ok).arg(picked.size()));
}

void RegistryPage::exportReport()
{
    if (m_issues.isEmpty()) {
        m_status->setText(QStringLiteral("还没有分析结果可导出。"));
        return;
    }
    const QString path = QString("%1/registry-report.txt").arg(Utils::dataDir());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_status->setText(QStringLiteral("写报告失败：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "ltool 注册表分析报告\n\n";
    for (const RegIssue& it : m_issues) {
        ts << QString("[%1] %2\n  位置：%3\\%4%5\n  原因：%6\n  风险：%7\n\n")
                  .arg(RegClean::categoryLabel(it), it.title, it.hiveName, it.subKey)
                  .arg(it.valueName.isEmpty() ? QString() : QString(" @%1").arg(it.valueName),
                       it.detail, RegClean::riskLabel(it));
    }
    f.close();
    Platform::reveal(path);
    m_status->setText(QString("报告已导出：%1").arg(path));
}
