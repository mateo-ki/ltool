#include "ToolsPage.h"

#include "ShellMenu.h"

#include "DevEnv.h"
#include "PluginHost.h"
#include "PluginStore.h"
#include "Theme.h"
#include "Utils.h"
#include "platform/ProcInfo.h"

#include <QColor>
#include <QDesktopServices>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QScrollArea>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <functional>
#include <memory>

namespace {

constexpr int kGridColumns = 3;

// 清单里那个 color 是 "r,g,b" 三个数（plugin.json 里就这么写），宿主扫出来的那份是 QColor。
// 认不出来就按 id 派生一个稳的颜色 —— 不能因为少一个字段画出一张黑卡。
QColor colorOf(const QString& spec, const QString& id)
{
    const QStringList parts = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() == 3) {
        bool ok = false;
        const int r = parts[0].trimmed().toInt(&ok);
        if (ok) {
            QColor c(r, parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
            if (c.isValid())
                return c;
        }
    }
    const size_t seed = qHash(id);
    return QColor(90 + int(seed % 120), 90 + int((seed / 7) % 120), 110 + int((seed / 13) % 110));
}

// 一张商店卡片：图标 + 名字 + 说明 + 状态，操作按钮排在卡片最底下一行。
class ShopCard : public QFrame {
public:
    ShopCard(const QString& title, const QString& desc, const QColor& color, const QString& version, QWidget* parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("row"));
        setAttribute(Qt::WA_StyledBackground);
        setMinimumHeight(128);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(14, 11, 14, 11);
        lay->setSpacing(6);

        auto* topRow = new QHBoxLayout;
        topRow->setSpacing(8);
        topRow->setContentsMargins(0, 0, 0, 0);

        auto* icon = new QLabel(this);
        icon->setFixedSize(32, 32);
        icon->setPixmap(Utils::letterIcon(title, color).pixmap(32, 32));
        icon->setScaledContents(true);
        icon->setAttribute(Qt::WA_TranslucentBackground);
        topRow->addWidget(icon, 0, Qt::AlignVCenter);

        auto* name = new QLabel(title, this);
        QFont f = name->font();
        f.setPointSize(f.pointSize() + 1);
        f.setBold(true);
        name->setFont(f);
        topRow->addWidget(name, 0, Qt::AlignVCenter);

        if (!version.isEmpty()) {
            const QString vText = (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V')))
                                      ? version
                                      : QStringLiteral("v%1").arg(version);
            auto* verBadge = new QLabel(vText, this);
            verBadge->setObjectName(QStringLiteral("verBadge"));
            Theme::bind(verBadge,
                "background: @card; "
                "color: @groupText; "
                "border: 1px solid @cardLine; "
                "border-radius: 4px; "
                "padding: 1px 6px; "
                "font-size: 8.5pt; "
                "font-weight: 500;");
            topRow->addWidget(verBadge, 0, Qt::AlignVCenter);
        }

        topRow->addStretch(1);

        // 可更新那颗卡片的标记：一个绿色向上箭头，钉在右上角。
        m_arrow = new QLabel(QStringLiteral("\u2191"), this);
        Theme::bind(m_arrow,
            "color: @ok; font-size: 14pt; font-weight: 700; background: transparent;");
        m_arrow->setVisible(false);
        topRow->addWidget(m_arrow, 0, Qt::AlignVCenter);

        m_desc = new QLabel(desc, this);
        m_desc->setObjectName(QStringLiteral("muted"));
        m_desc->setWordWrap(true);

        // 状态信息独占一行（整卡宽度），再也不和右下角的 1~3 颗按钮在同一行互挤
        m_state = new QLabel(this);
        m_state->setObjectName(QStringLiteral("muted"));
        m_state->setProperty("shopState", true);
        m_state->setWordWrap(true);

        auto* footRow = new QHBoxLayout;
        footRow->setSpacing(6);
        footRow->setContentsMargins(0, 0, 0, 0);
        footRow->addStretch(1);

        m_foot = new QHBoxLayout;
        m_foot->setSpacing(6);
        m_foot->setContentsMargins(0, 0, 0, 0);
        footRow->addLayout(m_foot, 0);

        lay->addLayout(topRow);
        lay->addWidget(m_desc, 1);
        lay->addWidget(m_state);
        lay->addLayout(footRow);
    }

    void setUpdateArrow(bool on) { if (m_arrow) m_arrow->setVisible(on); }
    QLabel* stateLabel() const { return m_state; }
    QPushButton* addButton(const QString& text, bool isPrimary = false)
    {
        auto* b = new QPushButton(text, this);
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumWidth(56);
        b->setFixedHeight(30);
        if (isPrimary) {
            b->setObjectName(QStringLiteral("cardBtnPrimary"));
            Theme::bind(b,
                "QPushButton {"
                "  background: @accent;"
                "  border: 1px solid @accent;"
                "  color: @onAccent;"
                "  border-radius: 6px;"
                "  font-size: 9pt;"
                "  font-weight: 600;"
                "  padding: 0 10px;"
                "}"
                "QPushButton:hover { background: @accentUp; border-color: @accentUp; }"
                "QPushButton:pressed { background: @accentDown; }"
                "QPushButton:disabled { background: @field; border: 1px solid @line; color: @disabled; }"
            );
        } else {
            b->setObjectName(QStringLiteral("cardBtn"));
            Theme::bind(b,
                "QPushButton {"
                "  background: @button;"
                "  border: 1px solid @borderHi;"
                "  color: @text;"
                "  border-radius: 6px;"
                "  font-size: 9pt;"
                "  font-weight: 500;"
                "  padding: 0 10px;"
                "}"
                "QPushButton:hover { background: @buttonUp; border-color: @borderHiUp; }"
                "QPushButton:pressed { background: @buttonDown; }"
                "QPushButton:disabled { color: @disabled; background: @field; border-color: @line; }"
            );
        }
        m_foot->addWidget(b);
        return b;
    }
    void setBodyClick(std::function<void()> fn)
    {
        m_onClick = std::move(fn);
        // 点不动的那张卡（没页可看、又没按钮可按）不该画成一只手
        setCursor(m_onClick ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    // 判据量的是"这件事到底给没给"（m_onClick 那个决策本身），不是光标形状那种二手证据 ——
    // 他 10-20 报"点不开"那次，卡片状态词是对的、按钮也对，只有正文点了个空函数。
    bool hasBodyClick() const { return static_cast<bool>(m_onClick); }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        QFrame::mouseReleaseEvent(e);
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && m_onClick)
            m_onClick();
    }

private:
    QLabel* m_desc = nullptr;
    QLabel* m_arrow = nullptr;
    QLabel* m_state = nullptr;
    QHBoxLayout* m_foot = nullptr;
    std::function<void()> m_onClick;
};

// 装了的东西既能禁用也能卸载，所以一张卡上最多三颗按钮。enableIt=true 就是那颗「启用」
void addToggleButton(ToolsPage* page, ShopCard* card, const QString& file, bool enableIt)
{
    QPushButton* btn = card->addButton(enableIt ? QStringLiteral("启用") : QStringLiteral("禁用"));
    QObject::connect(btn, &QPushButton::clicked, page, [page, file, card, enableIt] {
        const QString err = PluginStore::setEnabled(file, enableIt);
        card->stateLabel()->setText(err.isEmpty()
                                        ? QStringLiteral("已%1 · 下次启动加载它")
                                              .arg(enableIt ? QStringLiteral("启用") : QStringLiteral("禁用"))
                                        : err);
        Utils::log(QStringLiteral("store: %1 %2%3")
                       .arg(enableIt ? QStringLiteral("启用") : QStringLiteral("禁用"), file,
                            err.isEmpty() ? QString() : QStringLiteral(" 失败：") + err));
        if (err.isEmpty()) {
            PluginHost::reload();
            ShellMenu::syncWithPlugin();     // 禁用「右键动作」= 右键里那几项也该没了
            QTimer::singleShot(0, page, [page] { page->rebuildGrid(); });
        }
    });
}

// 卸载 = 真把那个文件从目录里删掉。删得掉就当场没了；这个进程已经加载过它的话 Windows 不放行，
// PluginStore 把它记进待删表、下次启动第一件事删掉 —— 那时候这句状态就该说"下次启动生效"。
void addUninstallButton(ToolsPage* page, ShopCard* card, const QString& name, const QString& file,
                        const QString& again)
{
    QPushButton* btn = card->addButton(QStringLiteral("卸载"));
    QObject::connect(btn, &QPushButton::clicked, page, [page, card, name, file, again] {
        const auto answer = QMessageBox::question(
            page, QStringLiteral("卸载插件"),
            QStringLiteral("这会把 %1 从\n%2\n删掉（配置和数据都不动）。%3\n\n确定吗？")
                .arg(file, PluginHost::pluginsDir(), again),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);   // 默认停在"不"
        if (answer != QMessageBox::Yes) {
            card->stateLabel()->setText(QStringLiteral("没动。"));
            return;
        }
        QTimer::singleShot(0, page, [page, file, name] {
            page->applyUninstall(file, name, nullptr);
        });
    });
}


} // namespace

// 答了"是"之后那一段。单独拿出来是因为离屏判据跑不到 `answer == Yes` 那一支 ——
// 无窗口平台上 question() 会立刻返回默认值（"不"），所以只能让测试直接调这一段。
// 走的是同一份代码，不是照着抄的第二遍。
void ToolsPage::applyUninstall(const QString& file, const QString& name, QLabel* state)
{
    const QString res = PluginStore::uninstall(file);
    if (res == QLatin1String("pending")) {
        if (state)
            state->setText(QStringLiteral("已安排卸载 · 这个进程还加载着它，下次启动真删掉"));
        rebuildGrid();
        return;
    }
    if (!res.isEmpty()) {
        if (state)
            state->setText(res);
        return;
    }
    Utils::log(QStringLiteral("store: 卸载 %1（%2）完成，重扫目录").arg(file, name));
    PluginHost::reload();
    ShellMenu::syncWithPlugin();             // 卸载了「右键动作」，文件夹右键里就不该再有 ltool
    rebuildGrid();
}


ToolsPage::ToolsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 6, 4, 4);
    lay->setSpacing(8);

    auto* head = new QHBoxLayout;
    head->setSpacing(8);
    m_back = new QToolButton(this);
    m_back->setText(QStringLiteral("‹ 返回商店"));
    m_back->setToolTip(QStringLiteral("回到那一面卡片墙（在这张页上按 Esc 也可以）"));
    m_back->setAccessibleName(QStringLiteral("返回商店"));
    m_back->setCursor(Qt::PointingHandCursor);
    m_back->setVisible(false);
    m_heading = new QLabel(QStringLiteral("插件商店"), this);
    m_heading->setObjectName(QStringLiteral("pageTitle"));
    m_refreshBtn = new QPushButton(QStringLiteral("刷新清单"), this);
    m_refreshBtn->setObjectName(QStringLiteral("storeRefresh"));
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setToolTip(QStringLiteral("拉商店那份 plugins-index.json。\n"
                                           "内置地址是**和这版 ltool 同一次构建发出去的那一份**（GitHub Release 资产），"
                                           "地址在配置页「插件清单地址」那一行，代理沿用「下载代理」那一行。"));
    m_repoBtn = new QPushButton(QStringLiteral("插件仓库"), this);
    m_repoBtn->setObjectName(QStringLiteral("storeRepo"));
    m_repoBtn->setCursor(Qt::PointingHandCursor);
    m_repoBtn->setToolTip(QStringLiteral("前往 ltool_plugin GitHub 插件仓库网页"));
    connect(m_repoBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/mateo-ki/ltool_plugin")));
    });

    const char* const kTopBtnQss =
        "QPushButton {"
        "  background: @button;"
        "  border: 1px solid @borderHi;"
        "  color: @text;"
        "  border-radius: 7px;"
        "  font-size: 9pt;"
        "  font-weight: 500;"
        "  padding: 4px 12px;"
        "  min-height: 26px;"
        "}"
        "QPushButton:hover { background: @buttonUp; border-color: @borderHiUp; }"
        "QPushButton:pressed { background: @buttonDown; }"
    ;
    Theme::bind(m_refreshBtn, kTopBtnQss);
    Theme::bind(m_repoBtn, kTopBtnQss);
    // 出口那颗和它们同款：autoRaise 的淡箭头在深色页眉里就是"找不到退出按钮"（他 2026-10-18 的原话）
    Theme::bind(m_back, kTopBtnQss);

    // 「一键下载全部」：把商店里这台机器还没装的那几颗一次排进那条「一次只飞一颗」的队列。
    // 为什么不摆到底下那行过滤里：这一颗管的范围是**整个商店**，跟着筛出来的那几张走，"全部"就名不副实。
    m_bulk = new QPushButton(QStringLiteral("一键下载全部"), this);
    m_bulk->setObjectName(QStringLiteral("storeBulk"));
    m_bulk->setAccessibleName(QStringLiteral("一键下载全部"));
    m_bulk->setCursor(Qt::PointingHandCursor);
    m_bulk->setToolTip(QStringLiteral(
        "把商店里这台机器还没装的那几颗一次排上队 —— 还是一次只飞一颗，一颗落地才接下一颗。\n"
        "接口对不上的、条目用不了的那几颗不在里面。\n"
        "第一下一颗都不动，只把「排几颗 · 大约多少」摆出来；再按一次才真排队。"));
    Theme::bind(m_bulk, kTopBtnQss);
    // 预留到两版标签里**最宽的那一版**：点下去只换字、不撑页眉那一排（他否过"一点就重新排"）。
    // 宽度拿 `sizeHint()` 量，不按"几个字 × 一个字宽"估 —— 本机 `windows` 平台实测
    // 「一键下载全部」比「确认这 12 颗」宽（前者六个纯汉字，后者夹了数字和空格），
    // 上一版只按后者留，基线那一版装不下 ⇒ 一点就把旁边两颗按钮挤走。
    {
        m_bulk->ensurePolished();
        const QString baseText = m_bulk->text();
        int widest = m_bulk->sizeHint().width();
        m_bulk->setText(QStringLiteral("确认这 12 颗"));
        widest = qMax(widest, m_bulk->sizeHint().width());
        m_bulk->setText(baseText);
        m_bulk->setMinimumWidth(widest);
    }
    connect(m_bulk, &QPushButton::clicked, this, [this] { onBulkClicked(); });

    head->addWidget(m_back);
    head->addWidget(m_heading);
    head->addStretch(1);
    head->addWidget(m_bulk);
    head->addWidget(m_repoBtn);
    head->addWidget(m_refreshBtn);
    lay->addLayout(head);

    // 启动优先载入持久化清单缓存
    const QByteArray cached = PluginStore::loadCachedIndex();
    if (!cached.isEmpty()) {
        QStringList probs;
        const auto entries = PluginStore::parseIndex(cached, &probs);
        if (!entries.isEmpty()) {
            m_haveStore = true;
            m_lastEntries = entries;
            m_lastProblems = probs;
        }
    }

    m_storeStatus = new QLabel(this);
    m_storeStatus->setObjectName(QStringLiteral("tip"));
    m_storeStatus->setWordWrap(true);
    if (m_haveStore) {
        setStoreLine(QStringLiteral("已载入本地清单缓存（共 %1 条插件）· 每天自动检查更新一次")
                         .arg(m_lastEntries.size()),
                     QStringLiteral("这份缓存来自本机 plugins_cache.json · 清单地址 %1").arg(PluginStore::indexUrl()));
    } else {
        setStoreLine(QStringLiteral(
            "这一面列的是本机 `ltool-plugins\\` 里装着的那几个。点右上「刷新清单」，仓库里能装的也进这同一面。"));
    }
    lay->addWidget(m_storeStatus);

    // 自动更新那一趟的结果：常驻一行，不挂在某次动作的状态上（他不一定在商店页时才跑完）。
    m_autoNote = new QLabel(this);
    m_autoNote->setObjectName(QStringLiteral("tip"));
    m_autoNote->setWordWrap(true);
    setAutoNote();
    lay->addWidget(m_autoNote);

    // ---- 过滤那一行：常驻在两行说明之后、那一面墙之前 ----
    // 不"点了才出现"：点一下才新插一块东西，是他否过的那类表现（10-18/10-21 各一次）。
    // 两维各管一维，是"与"的关系：名字（只比显示名）+ 安装状态（全部/已装/未装）。
    // 状态的判据不在这里 —— 在 PluginStore::countsAsInstalled()，卡片那句话也读它，两份不许各算各的。
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(8);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("storeFilter"));
    m_filter->setPlaceholderText(QStringLiteral("按插件名称筛（只比名字，不比关键词）"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setAccessibleName(QStringLiteral("按插件名称筛插件"));
    const char* const kFilterQss =
        "QLineEdit {"
        "  background: @field;"
        "  border: 1px solid @borderHi;"
        "  border-radius: 7px;"
        "  color: @editText;"
        "  padding: 4px 8px;"
        "  min-height: 26px;"
        "  font-size: 9pt;"
        "}"
        "QLineEdit:focus { border-color: @accent; }";
    Theme::bind(m_filter, kFilterQss);
    const char* const kChipQss =
        "QPushButton {"
        "  background: @button;"
        "  border: 1px solid @borderHi;"
        "  color: @text;"
        "  border-radius: 7px;"
        "  font-size: 9pt;"
        "  padding: 4px 12px;"
        "  min-height: 26px;"
        "}"
        "QPushButton:hover { background: @buttonUp; border-color: @borderHiUp; }"
        "QPushButton:checked { background: @accent; border-color: @accentUp; color: @onAccent; }";
    static const char* const kShelfWords[3] = { "全部", "已装", "未装" };
    for (int i = 0; i < 3; ++i) {
        auto* b = new QPushButton(QString::fromUtf8(kShelfWords[i]), this);
        b->setCheckable(true);
        b->setChecked(i == 0);   // 默认「全部」
        b->setCursor(Qt::PointingHandCursor);
        b->setAccessibleName(QStringLiteral("只看%1的插件").arg(QString::fromUtf8(kShelfWords[i])));
        Theme::bind(b, kChipQss);
        const PluginStore::Shelf s = static_cast<PluginStore::Shelf>(i);
        connect(b, &QPushButton::clicked, this, [this, s] { applyShelf(s); });
        m_shelfBtn[i] = b;
        filterRow->addWidget(b);
    }
    filterRow->insertWidget(0, m_filter, 1);
    lay->addLayout(filterRow);

    // 筛出来几句话：0 命中、筛掉了几张、改了但还没重排 —— 三种情况都得有话说（常驻，不是临时弹一下）
    m_filterNote = new QLabel(this);
    m_filterNote->setObjectName(QStringLiteral("tip"));
    m_filterNote->setWordWrap(true);
    lay->addWidget(m_filterNote);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& t) { applyFilter(t); });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildGrid());     // 0 = 那一面卡片，插件页往后追加
    lay->addWidget(m_stack, 1);

    connect(m_back, &QToolButton::clicked, this, [this] { backToGrid(); });
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] { refreshStore(); });

    // 每天自动拉取一次清单更新
    if (PluginStore::shouldAutoRefresh() || !m_haveStore) {
        QTimer::singleShot(100, this, [this] { refreshStore(); });
    }
}

QVector<PluginStore::Row> ToolsPage::shopRows() const
{
    // 一条列表、一条规则，绝不出现"墙是一批、清单是另一批"：
    //   · 没拉过清单 → 只报目录里有什么（不猜仓库里有什么）
    //   · 拉过 → 以清单为准，后面跟上本机装着而清单没列的那几个
    QVector<PluginStore::Row> out;
    if (!m_haveStore) {
        out = PluginStore::localRows(QString());
    } else {
        out = PluginStore::rows(m_lastEntries);
        const QVector<PluginStore::Row> extra = PluginStore::localOnly(m_lastEntries);
        for (const PluginStore::Row& r : extra)
            out.append(r);
    }
    // "排进待删表"这件事只有配置里那张表知道，所以在这里补到数据层上 ——
    // 卡片重排之后它照样说得出真话（写在卡片标签上活不过一次重排）。
    const QVector<QString> queued = PluginStore::pendingRemoves();
    if (!queued.isEmpty())
        for (PluginStore::Row& r : out)
            r.queuedRemove = queued.contains(r.remote.file);
    return out;
}

bool ToolsPage::nameMatches(const QString& name, const QString& query)
{
    const QString q = query.trimmed();
    if (q.isEmpty())
        return true;                                        // 没筛 = 全通过，不是全挡住
    return name.trimmed().contains(q, Qt::CaseInsensitive); // 只比显示名：描述和关键词一概不算
}

bool ToolsPage::passesShelf(const PluginStore::Row& r) const
{
    if (m_shelf == PluginStore::Shelf::All)
        return true;
    // "算不算已装"这一判只在 PluginStore::countsAsInstalled 那一份里，界面不重算
    const bool inst = PluginStore::countsAsInstalled(r.state, r.queuedRemove);
    return m_shelf == PluginStore::Shelf::Installed ? inst : !inst;
}

QVector<PluginStore::Row> ToolsPage::passedRows(const QVector<PluginStore::Row>& rows) const
{
    QVector<PluginStore::Row> out;
    for (const PluginStore::Row& r : rows)
        if (nameMatches(r.remote.name, m_filterText) && passesShelf(r))
            out.append(r);
    return out;
}

QString ToolsPage::filterDesc() const
{
    QStringList parts;
    const QString q = m_filterText.trimmed();
    if (!q.isEmpty())
        parts << QStringLiteral("名字「%1」").arg(q);
    if (m_shelf == PluginStore::Shelf::Installed)
        parts << QStringLiteral("只看已装");
    else if (m_shelf == PluginStore::Shelf::Missing)
        parts << QStringLiteral("只看未装");
    return parts.isEmpty() ? QStringLiteral("没筛") : parts.join(QStringLiteral(" · "));
}

QString ToolsPage::filterText() const
{
    return m_filterText;
}

QString ToolsPage::filterNote() const
{
    return m_filterNote ? m_filterNote->text() : QString();
}

void ToolsPage::updateFilterNote(int shown, int total, bool pending)
{
    if (!m_filterNote)
        return;
    const QString desc = filterDesc();
    QString line;
    if (total == 0)
        line = QStringLiteral("这一面还没东西可筛。");
    else if (desc == QLatin1String("没筛"))
        line = QStringLiteral("没筛：这一面 %1 张全列着。").arg(total);
    else
        line = QStringLiteral("筛出 %1 / 共 %2 张（%3）%4")
                   .arg(shown).arg(total).arg(desc,
                        shown == 0 ? QStringLiteral(" · 上面那一面现在没有可列的卡，清掉过滤就回来")
                                   : QString());
    if (pending)
        line += QStringLiteral(" · 你现在正看着一张页，返回商店时才重排这一面");
    m_filterNote->setText(line);
}

void ToolsPage::applyFilter(const QString& text)
{
    m_filterText = text;
    // "现在是不是正看着一张页"只准问 isHidden()：离屏这扇窗从没真 show 过，
    // isVisible() 对它的每一个孩子都是 false（第一版就是这么判成"没在看页"，当场把人拽回墙）。
    if (m_back && !m_back->isHidden()) {
        // 正看着一张插件页：不许把人拽回卡片墙（他报过"商店自己跳到了那张页"那一类）。
        // 先把欠着重排记下来，那一句"挡住几张"照实更新 —— 数的是同一颗 passedRows。
        m_filterDirty = true;
        const QVector<PluginStore::Row> all = shopRows();
        updateFilterNote(passedRows(all).size(), all.size(), true);
        return;
    }
    m_filterDirty = false;
    rebuildGrid();
}

void ToolsPage::applyShelf(PluginStore::Shelf s)
{
    m_shelf = s;
    for (int i = 0; i < 3; ++i)
        if (m_shelfBtn[i])
            m_shelfBtn[i]->setChecked(static_cast<PluginStore::Shelf>(i) == s);
    applyFilter(m_filterText);      // 换档走同一条：同一个"要不要打断"的规矩，不另开一份
}

QWidget* ToolsPage::buildGrid()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    const QVector<PluginStore::Row> all = shopRows();
    if (all.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral(
            "插件目录 %1 里一个 dll 都没有，清单也没拉到 —— 这一面没东西可列。"
            "重装一次 ltool，或者点「刷新清单」。").arg(PluginHost::pluginsDir()), page);
        empty->setObjectName(QStringLiteral("tip"));
        empty->setWordWrap(true);
        lay->addWidget(empty);
        lay->addStretch(1);
        updateFilterNote(0, 0, false);
        return page;
    }

    const QVector<PluginStore::Row> rows = passedRows(all);
    if (rows.isEmpty()) {
        // 0 命中也要有话说，并且要说清是被哪一维挡的、怎么回去（不能让人对着一面白墙猜）
        auto* none = new QLabel(QStringLiteral(
            "没有同时满足「%1」的插件 —— 这一面 %2 张全被挡着。清掉过滤框右边那个 ✕、"
            "或把上面那三档按回「全部」就回来了。").arg(filterDesc(), all.size()), page);
        none->setObjectName(QStringLiteral("tip"));
        none->setWordWrap(true);
        lay->addWidget(none);
        lay->addStretch(1);
        updateFilterNote(0, all.size(), false);
        return page;
    }

    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    for (int i = 0; i < rows.size(); ++i) {
        grid->addWidget(makeCard(rows[i]), i / kGridColumns, i % kGridColumns);
        grid->setColumnStretch(i % kGridColumns, 1);
    }
    lay->addLayout(grid);
    lay->addStretch(1);
    updateFilterNote(rows.size(), all.size(), false);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("toolsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

// "卸了之后还能不能装回来"只有拉过清单才答得上来；没拉过就别假装有把握
QString ToolsPage::cannotOpenText(const QString& stateWord, const QString& loadErr,
                                  const QString& metaErr)
{
    const QString why = !loadErr.isEmpty()    ? loadErr
                        : !metaErr.isEmpty()  ? metaErr
                                              : QStringLiteral("目录里认不出这个文件");
    return QStringLiteral("%1 · 本机有这个 dll 但它起不来：%2").arg(stateWord, why);
}

QString ToolsPage::reinstallHint(const PluginStore::Row& r) const
{
    if (!m_haveStore)
        return QStringLiteral("还没拉过清单，不确定仓库里有没有它 —— 删了可能要重装才回得来。");
    if (r.state == PluginStore::State::LocalOnly)
        return QStringLiteral("插件清单里没列它（本机独有的那几个）—— 删了要等仓库收了它才装得回来。");
    return QStringLiteral("清单里有它，卸完还能从这一面再装回来。");
}

QWidget* ToolsPage::makeCard(const PluginStore::Row& r)
{
    auto* card = new ShopCard(r.remote.name, r.remote.desc, colorOf(r.remote.color, r.remote.id), r.remote.version, this);
    // 状态是数据层那个枚举，判据不该靠猜文案认它（"↑ 可更新"带个箭头前缀，startsWith 就认不出了，
    // 2026-10-17 那 4 条红就是这里来的）。文案想怎么排版都行，这一颗钉住"这张卡说的是什么状态"。
    card->setProperty("ltoolState", static_cast<int>(r.state));
    QString stateWord = PluginStore::stateWord(r.state);
    // 可更新 / 与商店不同 这两档各带着两个版本号（本机一个、商店一个）。
    // 往状态词后面拼一个孤零零的 (v1.0.0) 会让人以为"要更新到 1.0.0"，
    // 而那印的本来就是本机自己那份 —— 版本对照写在悬停提示里（note 那句）。
    const bool twoVersions = r.state == PluginStore::State::Update
                             || r.state == PluginStore::State::Different;
    if (!r.remote.version.isEmpty() && !twoVersions)
        stateWord = QStringLiteral("%1 (v%2)").arg(stateWord, r.remote.version);
    QString noteText;
    if (r.state == PluginStore::State::Installed) {
        const int idx = r.note.lastIndexOf(QLatin1String(" · "));
        noteText = (idx >= 0) ? r.note.mid(idx + 3) : QString();
    } else if (r.state == PluginStore::State::Missing) {
        const int idx = r.note.indexOf(QLatin1String("（清单 "));
        if (idx >= 0) {
            noteText = r.note.mid(idx + 5);
            if (noteText.endsWith(QLatin1Char(')')) || noteText.endsWith(QString::fromUtf8("）")))
                noteText.chop(1);
        } else {
            noteText = r.note;
        }
    } else if (r.state == PluginStore::State::Update || r.state == PluginStore::State::Different) {
        // 「↑ 可更新」那颗绿箭头本身就是答复，不需要再把"本机 xxxx… ≠ 清单 yyyy…"那串写在脸上。
        // 大小留着（他要看得见装的是多大一份），哈希对照挪进悬停。
        noteText = PluginStore::humanSize(r.remote.bytes);
        card->setToolTip(r.note);
    } else {
        noteText = r.note;
    }
    // 排进待删表的那张：整条状态换掉。文件确实还在盘上，可"已装"这句已经是假话 ——
    // 他 11:17 连点两次「卸载」，就是因为这张卡每次重排都重新说"已装"（写在标签上的那句
    // 活不过一次重排，所以这条改在数据层：见 PluginStore::Row::queuedRemove）。
    if (r.queuedRemove) {
        stateWord = QStringLiteral("已安排卸载");
        noteText = QStringLiteral("这个进程还加载着它 · 下次启动真删掉");
    }
    card->setUpdateArrow(r.state == PluginStore::State::Update && !r.queuedRemove);
    card->stateLabel()->setText(noteText.isEmpty()
                                    ? stateWord
                                    : QStringLiteral("%1 · %2").arg(stateWord, noteText));
    if (r.state == PluginStore::State::Update)
        Theme::bind(card->stateLabel(), "color: @ok; font-weight: 600;");

    using S = PluginStore::State;
    const PluginStore::Entry e = r.remote;
    const QString id = r.remote.id;
    const QString file = r.remote.file;
    // "能不能点开"看的不是状态词，是宿主认不认得出那个 dll：
    // 已装但没通过检查的那条也得老实承认打不开，不能给一只手形然后什么都发生不了。
    const PluginHost::Meta* md = PluginHost::meta(id);
    const bool openable = md && md->ok();
    const bool installed = r.state == S::Installed || r.state == S::Update
                         || r.state == S::LocalOnly || r.state == S::Different;
    // 这三颗是给离屏判据挂的"决策的输入"：卡片红不红要能说出是哪一半决定的，
    // 只量状态词和按钮的话，"正文给了空函数"这一类（10-20 他报的点不开）会漏掉。
    card->setProperty("ltoolId", id);
    card->setProperty("ltoolOpenable", openable);
    // 「清单这一条在这台机器上装得了吗」管的是「装 / 更新 / 换成商店那份」那三颗按钮，
    // 不许管正文点击、禁用、卸载 —— 那是上面那颗 openable 的事。
    card->setProperty("ltoolEntryUsable", r.entryUsable);

    switch (r.state) {
    case S::Missing:
    case S::Update: {
        // 商店那一条在这台机器上装得了吗（`Api` 对得上）。对不上就**不给「装/更新」**，
        // 但本机那颗照旧能禁、照旧能卸、照旧能点开 —— 10-20 他报"注册表/开机分析还是点不开"，
        // 形状就是原来那张卡被整条锁死：一颗按钮都不给、点正文也没反应。
        QPushButton* install = nullptr;
        if (r.entryUsable) {
            install = card->addButton(r.state == S::Missing ? QStringLiteral("装")
                                                            : QStringLiteral("更新"),
                                      /*isPrimary=*/true);
            connect(install, &QPushButton::clicked, this, [this, e, card, install] {
                install->setEnabled(false);
                installEntry(e, install, card->stateLabel());
            });
        }
        if (r.state == S::Update) {
            // 本机确实装着它（只是哈希不同）：禁得掉、也卸得掉，不能只剩"更新"这一条路
            addToggleButton(this, card, file, /*enableIt=*/false);
            addUninstallButton(this, card, e.name, file,
                                QStringLiteral("清单里有它，卸完还能从这一面再装回来。"));
        }
        if (r.state == S::Missing && install) {
            // 还没装的那张：点正文就是按同一颗按钮。这里绝不能去 showTool —— 本机没那个 dll
            // （而"没装着 + 商店那条对不上号"根本走不到这里：那一档在 compare 里就叫 ApiMismatch）
            card->setBodyClick([install] { install->click(); });
        } else if (r.state == S::Update) {
            // 「可更新」是装了但哈希不同：那张页本来就能看，正文照常打开，更新交给按钮
            card->setBodyClick(openable ? [this, id] { showTool(id); } : std::function<void()>());
        }
        break;
    }
    case S::Different: {
        // 本机装着、哈希与商店那份不一样，但商店版本号不比本机新 —— 这不叫"有更新"。
        // 出路还是要给：开发机上手工同步过、或本机这份是更旧的测试产物时，能换回商店那份。
        // 商店那条对不上号时这条出路是坏的（换过去当场起不来），所以它跟着 entryUsable 走。
        if (r.entryUsable) {
            auto* swap = card->addButton(QStringLiteral("换成商店那份"), /*isPrimary=*/true);
            connect(swap, &QPushButton::clicked, this, [this, e, card, swap] {
                swap->setEnabled(false);
                installEntry(e, swap, card->stateLabel());
            });
        }
        addToggleButton(this, card, file, /*enableIt=*/false);
        addUninstallButton(this, card, e.name, file, reinstallHint(r));
        card->setBodyClick(openable ? [this, id] { showTool(id); } : std::function<void()>());
        break;
    }
    case S::Disabled:
        // .off 那个宿主本来就不会去加载它：能启用、能卸载，点正文没有页可看
        addToggleButton(this, card, file, /*enableIt=*/true);
        addUninstallButton(this, card, e.name, file, reinstallHint(r));
        card->setBodyClick(std::function<void()>());
        break;
    case S::Installed:
    case S::LocalOnly:
        addToggleButton(this, card, file, /*enableIt=*/false);
        addUninstallButton(this, card, e.name, file, reinstallHint(r));
        card->setBodyClick(openable ? [this, id] { showTool(id); } : std::function<void()>());
        break;
    case S::ApiMismatch:
    case S::Blocked:
        // 本机没有能用的那个文件：一颗按钮都不给，为什么不行写在状态那行里
        card->setBodyClick(std::function<void()>());
        break;
    }
    if (installed && !openable) {
        // 这一行原来有个洞：`md` 存在时三元取的是 `QString()` —— 界面上就是一句以冒号结尾的
        // "本机有这个 dll 但没通过检查："，后面什么都没有。他 2026-10-20 问"注册表分析和开机
        // 分析为什么点不开"，卡片上确实标了"没通过检查"，可**没说什么检查没过**。
        card->stateLabel()->setText(ToolsPage::cannotOpenText(
            stateWord, PluginHost::loadErrorOf(id), md ? md->error : QString()));
        Theme::bind(card->stateLabel(), "color: @bad; font-weight: 600;");
    }
    // 最后那颗要在所有 setBodyClick 都走完之后读：它量的是"这件事到底给没给"。
    // （第一版我把它写在 makeCard 开头，读到的永远是 false —— 19 张卡一起红，红的是判据自己。）
    card->setProperty("ltoolClickable", card->hasBodyClick());
    return card;
}

QString ToolsPage::autoUpdateLine()
{
    // 三种情况都得说得出话：关着、开着但今天还没跑、跑完了（成了几颗 / 哪几颗没成）。
    // 关着的时候不能留一行空白 —— 那看起来像"这个功能坏了"。
    if (!PluginStore::autoUpdateEnabled())
        return QStringLiteral("自动更新：没开（配置页「通用」那一节那颗开关）。这一面只在你要装的时候才动文件。");
    const QString note = PluginStore::autoUpdateNote();
    if (note.isEmpty())
        return QStringLiteral("自动更新：开着，今天这一趟还没跑（程序启动之后十几秒跑，一天一次，只换已装的那几颗）。");
    return QStringLiteral("自动更新：%1").arg(note);
}

void ToolsPage::refreshStore()
{
    if (m_storeBusy)
        return;
    m_storeBusy = true;
    const QString url = PluginStore::indexUrl();
    setStoreLine(QStringLiteral("正在拉清单…"), url);

    // 镜像 → 本地代理 → 直连，逐条试（那条顺序只在 PluginStore::fetch 里写一遍，两段联网共用）
    PluginStore::fetch(url, this, {}, {}, {}, [this, url](const PluginStore::Fetched& got) {
        m_storeBusy = false;
        setAutoNote();
        if (!got.ok) {
            if (!m_lastEntries.isEmpty()) {
                setStoreLine(QStringLiteral("网络没通，先看本地缓存那 %1 条").arg(m_lastEntries.size()),
                             QStringLiteral("网络拉取未成功：%1 —— 当前展示本地缓存清单（共 %2 条）")
                                 .arg(got.err).arg(m_lastEntries.size()));
                rebuildGrid();
                Utils::log(QStringLiteral("store: 拉清单失败 %1，使用本地缓存").arg(got.err));
                return;
            }
            m_haveStore = false;
            m_lastEntries.clear();
            m_lastProblems.clear();
            setStoreLine(QStringLiteral("清单没拉到，这一面退回本机装着的那几个，它们照常能用"),
                         QStringLiteral("清单没拉到：%1 —— 这一面退回本机装着的那几个，它们照常能用")
                             .arg(got.err));
            rebuildGrid();
            Utils::log(QStringLiteral("store: 拉清单失败 %1 url=%2").arg(got.err, url));
            return;
        }
        QStringList problems;
        const auto entries = PluginStore::parseIndex(got.body, &problems);
        m_haveStore = true;
        m_lastEntries = entries;
        m_lastProblems = problems;
        PluginStore::saveCachedIndex(got.body);
        PluginStore::markFetched();

        const QVector<PluginStore::Row> all = shopRows();
        int canInstall = 0;
        for (const PluginStore::Row& r : all) {
            if (r.state == PluginStore::State::Missing || r.state == PluginStore::State::Update)
                ++canInstall;
        }
        const int outside = PluginStore::localOnly(entries).size();
        // 页面上那句只报数（几张卡、几颗能装、几颗拒收、几张在清单外）；
        // 那条 URL、走的哪一档、试了几次、拒收的是谁 —— 全在悬停说明和下面那行日志里。
        const QString brief = QStringLiteral("清单 %1 条 · 这一面 %2 张卡 · 能装或可更新 %3 · 拒收 %4 · 清单外 %5"
                                             " —— 装了的点正文是打开，没装的点正文是装")
                                  .arg(entries.size()).arg(all.size()).arg(canInstall)
                                  .arg(problems.size()).arg(outside);
        QString full = QStringLiteral("清单 %1 条（%2 · 走的是 %3，一共试了 %4 次）· 这一面 %5 张卡"
                                      " · 能装或可更新 %6 · 拒收 %7 · 清单外 %8")
                           .arg(entries.size()).arg(url, got.via).arg(got.tries).arg(all.size())
                           .arg(canInstall).arg(problems.size()).arg(outside);
        if (!problems.isEmpty())
            full += QStringLiteral("（拒收：%1）").arg(problems.join(QStringLiteral("；")));
        setStoreLine(brief, full);
        rebuildGrid();
        Utils::log(QStringLiteral("store: 清单 %1 条（%2 拿到 · %3），卡片 %4 张，能装 %5，拒收 %6，清单外 %7")
                       .arg(entries.size()).arg(got.via, url).arg(all.size()).arg(canInstall)
                       .arg(problems.size()).arg(outside));
    });
}

// ---- 「一键下载全部」----
// 排哪几颗只认这一颗：整个商店里 State::Missing 的那些。
// 为什么是静态纯函数：这条规则一旦长在"按下去"那一段里，离屏就没人量得到它到底排了谁 ——
// #94（--storenet 四条红没人看见）就是同一个形状。
QVector<PluginStore::Row> ToolsPage::bulkTargets(const QVector<PluginStore::Row>& all)
{
    QVector<PluginStore::Row> out;
    for (const PluginStore::Row& r : all)
        if (r.state == PluginStore::State::Missing)
            out.append(r);
    return out;
}

QString ToolsPage::bulkAskLine(int n, qint64 bytes, int unusable)
{
    QString line = QStringLiteral("第一下一颗都没动 · 再按一次就排 %1 颗（约 %2）")
                       .arg(n).arg(PluginStore::humanSize(bytes));
    if (unusable > 0)
        line += QStringLiteral(" · 另有 %1 颗清单里有、这台机器用不了，不排在里面").arg(unusable);
    return line + QStringLiteral(" · 还是一次只飞一颗，一颗落地才接下一颗");
}

QString ToolsPage::bulkEmptyLine(bool haveStore)
{
    if (!haveStore)
        return QStringLiteral("一颗都排不了：这一面只有本机装着的那几个，商店那份清单还没拉下来 —— 先按「刷新清单」。");
    return QStringLiteral("一颗都不用排：商店里那几颗，这台机器全装着了。");
}

QString ToolsPage::bulkDoneLine(int n, const QStringList& failed)
{
    QString line = QStringLiteral("一键那 %1 颗：成了 %2 颗，没成 %3 颗 · 装好的下次启动进卡片墙")
                       .arg(n).arg(n - failed.size()).arg(failed.size());
    if (!failed.isEmpty())
        line += QStringLiteral("（没成的：%1 · 卡片上按「重试」）").arg(failed.join(QStringLiteral("、")));
    return line;
}

QStringList ToolsPage::bulkMissed(const QStringList& ids, const QVector<PluginStore::Row>& now)
{
    // 「落没落地」问的是数据层现在的说法（那颗 dll 在不在目录里），不是"我数了几次 done 回调" ——
    // 回调次数会把"下成了但哈希不对被扔掉"那一支算成成功。
    QStringList missed;
    for (const QString& id : ids) {
        bool landed = false;
        for (const PluginStore::Row& r : now) {
            if (r.remote.id != id)
                continue;
            // Missing 才算没落地；清单外那条（LocalOnly）也说明文件真在盘上
            if (r.state != PluginStore::State::Missing) {
                landed = true;
                break;
            }
        }
        if (!landed)
            missed << id;
    }
    return missed;
}

QString ToolsPage::storeLine() const
{
    return m_storeStatus ? m_storeStatus->text() : QString();
}

QString ToolsPage::storeLineWhy() const
{
    return m_storeStatus ? m_storeStatus->toolTip() : QString();
}

void ToolsPage::setStoreLine(const QString& brief, const QString& full)
{
    if (!m_storeStatus)
        return;
    m_storeStatus->setText(brief);
    // 没给全文就把悬停说明清掉：上一句留下的 URL/错误码挂在下一句短话上，
    // 看着就像"这句短话背后就是那次失败"（读的是上一轮的状态，不是这一轮的）。
    m_storeStatus->setToolTip(full);
}

void ToolsPage::setAutoNote()
{
    if (!m_autoNote)
        return;
    m_autoNote->setText(autoUpdateLine());
    m_autoNote->setToolTip(PluginStore::autoUpdateWhy());
}

void ToolsPage::setBulkLabel(int confirmCount)
{
    if (!m_bulk)
        return;
    // 两版标签都是六个字（九颗以内）：点一下才把标题撑宽 = 那一排按钮跟着挪，是他否过的形状
    m_bulk->setText(confirmCount > 0 ? QStringLiteral("确认这 %1 颗").arg(confirmCount)
                                     : QStringLiteral("一键下载全部"));
}

void ToolsPage::onBulkClicked()
{
    if (!m_bulk || !m_storeStatus)
        return;
    if (installsBusy() > 0) {
        // 上一批还在飞：一次只飞一颗是那条队列的规矩，这里不插队，也不起第二趟
        m_bulkArmed = false;
        setBulkLabel(0);
        setStoreLine(QStringLiteral("上一批还在飞（在飞 %1 颗 · 排着 %2 颗）· 等它空了再按这一颗")
                                   .arg(m_installing).arg(m_queue.size()));
        return;
    }
    const QVector<PluginStore::Row> all = shopRows();
    const QVector<PluginStore::Row> targets = bulkTargets(all);
    if (targets.isEmpty()) {
        m_bulkArmed = false;
        setBulkLabel(0);
        setStoreLine(bulkEmptyLine(m_haveStore));
        return;
    }
    if (!m_bulkArmed) {
        qint64 bytes = 0;
        for (const PluginStore::Row& r : targets)
            bytes += r.remote.bytes;
        int unusable = 0;
        for (const PluginStore::Row& r : all)
            if (r.state == PluginStore::State::ApiMismatch || r.state == PluginStore::State::Blocked)
                ++unusable;
        m_bulkArmed = true;
        setBulkLabel(targets.size());
        setStoreLine(bulkAskLine(targets.size(), bytes, unusable));
        Utils::log(QStringLiteral("store: 一键下载全部 第一下（还没排）：%1 颗，约 %2，挡在外面 %3 颗")
                       .arg(targets.size()).arg(PluginStore::humanSize(bytes)).arg(unusable));
        return;
    }
    // 第二下：排的是**此刻**重算的那一份。第一下到第二下之间他自己装了一颗、换了筛、
    // 或清单跟着变了，都不该照着刚才屏幕上那个数字去排（那是上一秒的商店）。
    m_bulkArmed = false;
    setBulkLabel(0);
    m_bulkIds.clear();
    // 那一面墙上的卡片按"数据层给的那个 id"找回它自己的两颗控件 —— 认的是卡片上挂的那个属性，
    // 不是 findChildren 的顺序（这一棵树里现在挂着好几张插件页，顺序不是界面顺序）。
    const auto frames = findChildren<QFrame*>();
    for (const PluginStore::Row& r : targets) {
        QFrame* card = nullptr;
        for (QFrame* f : frames) {
            if (f->objectName() == QLatin1String("row") && f->property("ltoolId").toString() == r.remote.id) {
                card = f;
                break;
            }
        }
        QPushButton* btn = card ? card->findChild<QPushButton*>(QStringLiteral("cardBtnPrimary")) : nullptr;
        QLabel* state = nullptr;
        if (card) {
            const auto labels = card->findChildren<QLabel*>();
            for (QLabel* l : labels) {
                if (l->property("shopState").isValid()) {
                    state = l;
                    break;
                }
            }
        }
        m_bulkIds << r.remote.id;
        if (btn)
            btn->setEnabled(false);   // 被过滤挡在墙外的拿不到控件：传 nullptr，那条队列本来就允许"没地方写字、照样下"
        installEntry(r.remote, btn, state);
    }
    setStoreLine(QStringLiteral("已排 %1 颗 · 一次只飞一颗，卡片上那一行逐颗报进度").arg(m_bulkIds.size()));
    Utils::log(QStringLiteral("store: 一键下载全部 第二下：排了 %1 颗").arg(m_bulkIds.size()));
}

// 「装/更新」按下去先进这里：已经有一颗在飞就排队。
// 为什么非要排队 —— 他连点九颗那次，九条下载 + 九次改名 + 中途 reload/重排叠在一起，
// 最后崩在 scan() 里（Qt6Widgets，0xc0000005）。一次一颗把这堆叠态整个拿掉，
// 顺带也不会把镜像同时打九个洞。
void ToolsPage::installEntry(const PluginStore::Entry& e, QPushButton* btn, QLabel* state)
{
    if (m_installing > 0) {
        m_queue.append({ e, btn, state });
        if (btn)
            btn->setText(QStringLiteral("排队"));
        if (state)
            state->setText(QStringLiteral("前面还有 %1 颗，一颗落地就轮到它").arg(m_queue.size()));
        return;
    }
    runInstall(e, btn, state);
}

void ToolsPage::runInstall(const PluginStore::Entry& e, QPushButton* btn, QLabel* state)
{
    // 这两颗是**卡片上的控件**，而 rebuildGrid() 会把整格换掉（装完一颗、点一次「刷新清单」都会）。
    // 连的是 this（页面还活着），所以卡片被删之后回调照样会跑 —— 用裸指针就是往已删的 QWidget 上写：
    // 他"连着点多几颗装"闪退的就是这条（第一颗装完拆墙，第二颗的进度回调打在没了的按钮上）。
    QPointer<QPushButton> btnRef = btn;
    QPointer<QLabel> stateRef = state;
    ++m_installing;   // 有几颗在飞：最后一颗落地才重排卡片墙，见下面 done 里那句
    // 下载那一段在 `PluginStore::installEntry`（2026-10-21 搬出去的，自动更新那趟没有界面、
    // 要问同一件事）。这儿只剩"把进度写成字"这一件界面的事。
    PluginStore::installEntry(e, this,
                       [btnRef](qint64 got, qint64 all) {
                           // 就地改按钮上那行字，不新插一块进度条：版面一跳就是"突然出现在上面"
                           // 卡片已经被拆掉的话，这行字没地方写 —— 直接不写，下载照下。
                           if (btnRef)
                               btnRef->setText(all > 0 ? QStringLiteral("%1%").arg(got * 100 / all)
                                                       : PluginStore::humanSize(got));
                       },
                       [this, e, btnRef, stateRef](const PluginStore::InstallResult& r) {
                           if (btnRef)
                               btnRef->setEnabled(true);
                           if (!r.ok) {
                               if (stateRef)
                                   stateRef->setText(r.err);
                               if (btnRef)
                                   btnRef->setText(QStringLiteral("重试"));
                               Utils::log(QStringLiteral("store: 装 %1 没成：%2").arg(e.file, r.err));
                           } else {
                               Utils::log(QStringLiteral("store: 装好 %1（%2 拿到，哈希对过，%3 B）")
                                              .arg(e.file, r.via).arg(e.bytes));
                               if (stateRef)
                                   stateRef->setText(QStringLiteral("已装好 · 下次启动进卡片墙"));
                           }
                           if (--m_installing == 0)
                               startNextInstall();
                       });
}

// 队列空了才做那一次性的重扫 + 重排；还有下一颗就直接开下一条（控件此刻还都活着）。
void ToolsPage::startNextInstall()
{
    if (m_queue.isEmpty()) {
        finishInstalls();
        return;
    }
    const Queued q = m_queue.takeFirst();
    // 卡片可能已经被拆掉了（他中途点了「刷新清单」，或者上一颗落地时重排过）：
    // 那就没地方写字，但这一颗照样要装下去 —— 装不装是文件的事，写字才是界面的事。
    if (q.btn)
        q.btn->setText(QStringLiteral("下载中"));
    if (q.state)
        q.state->setText(QStringLiteral("轮到它了（后面还有 %1 颗）").arg(m_queue.size()));
    runInstall(q.entry, q.btn.data(), q.state.data());
}

// 一批安装全落地之后只做一次：重扫插件目录 + 重排卡片墙。
// 一颗装完就 rebuildGrid() 是那次闪退的另一半 —— 它把还在下载的那几张卡的控件删了。
void ToolsPage::finishInstalls()
{
    PluginHost::reload();
    ShellMenu::syncWithPlugin();     // 把「右键动作」装回来 = 右键里那几项也该跟着回来
    rebuildGrid();
    if (m_bulkIds.isEmpty())
        return;
    const QStringList failed = bulkMissed(m_bulkIds, shopRows());
    setStoreLine(bulkDoneLine(m_bulkIds.size(), failed));
    Utils::log(QStringLiteral("store: 一键下载全部 收尾：排 %1 颗，没成 %2 颗（%3）")
                   .arg(m_bulkIds.size()).arg(failed.size()).arg(failed.join(QStringLiteral("、"))));
    m_bulkIds.clear();
}

bool ToolsPage::showTool(const QString& id, const QString& text)
{
    QString err;
    QWidget* w = PluginHost::page(id, this, &err);
    if (!w) {
        // 光写日志等于没写：他点了卡片，屏幕上只是回到卡片墙，谁也看不出"点了但没开成"。
        // 页眉就在卡片墙顶上，那一行本来就是"现在在哪个插件"的位置 —— 把原因写在那儿。
        const PluginHost::Meta* md = PluginHost::meta(id);
        m_heading->setText(QStringLiteral("插件商店 · %1 打不开：%2")
                               .arg(md ? md->name : id,
                                    err.isEmpty() ? QStringLiteral("原因未知（看日志 plugin: 那几行）")
                                                  : err));
        Utils::log(QStringLiteral("plugin: 打不开 %1：%2").arg(id, err));
        return false;
    }
    if (m_stack->indexOf(w) < 0)
        m_stack->addWidget(w);
    if (!text.isEmpty() && !PluginHost::feed(id, text))
        Utils::log(QStringLiteral("plugin: %1 没接住那段文本（长度 %2）").arg(id).arg(text.size()));
    const int was = m_stack->currentIndex();
    m_stack->setCurrentWidget(w);
    // 他这次报的是"商店自己跳到了那张页"，可三条路（点卡片正文 / 搜索框直达 / 设置窗 openAt）
    // 在日志里长得一模一样，查不出来。这条把"从第几格跳到第几格"写下来，下次再报就有据。
    Utils::log(QStringLiteral("store: 展开 %1（stack 第 %2 格 / 共 %3 格，之前在第 %4 格）")
                   .arg(id)
                   .arg(m_stack->indexOf(w))
                   .arg(m_stack->count())
                   .arg(was));
    m_back->setVisible(true);
    const PluginHost::Meta* md = PluginHost::meta(id);
    m_heading->setText(QStringLiteral("插件商店 · %1").arg(md ? md->name : id));
    setStoreHeaderVisible(false);
    return true;
}

void ToolsPage::setStoreHeaderVisible(bool visible)
{
    if (m_storeStatus)
        m_storeStatus->setVisible(visible);
    if (m_autoNote)
        m_autoNote->setVisible(visible);
    if (m_filter)
        m_filter->setVisible(visible);
    for (int i = 0; i < 3; ++i) {
        if (m_shelfBtn[i])
            m_shelfBtn[i]->setVisible(visible);
    }
    if (m_filterNote)
        m_filterNote->setVisible(visible);
    if (m_bulk)
        m_bulk->setVisible(visible);
    if (m_repoBtn)
        m_repoBtn->setVisible(visible);
    if (m_refreshBtn)
        m_refreshBtn->setVisible(visible);
}

void ToolsPage::rebuildGrid()
{
    // 那一面墙换了内容，「确认这 N 颗」里那个 N 就成了上一秒的商店 —— 就地收回那一次确认，
    // 让他重新按第一下看新的数（拿旧数字排新的一份是"绿了但量错对象"的界面版）。
    m_bulkArmed = false;
    setBulkLabel(0);
    // 只换卡片那一格，已经点开过的插件页留在 stack 里不动（dll 不卸载，拆了也没意义）。
    //
    // 顺序是**先插新的、再摘旧的**。反过来写（先摘后插）会把上一张点开的插件页闪出来一次：
    // QStackedLayout 在"当前那一格被摘掉"的瞬间会把后面的格子顶成当前，于是那张页收到一次
    // Show —— 屏幕上就是"卸完插件，商店自己跳到了注册表分析那一页"（2026-10-18 他报的形状，
    // --storeexit 第⑥条量的就是这件事：旧顺序 Show 1 次，这个顺序 0 次）。
    QWidget* old = m_stack->widget(0);
    QWidget* grid = buildGrid();
    m_stack->insertWidget(0, grid);
    m_stack->setCurrentWidget(grid);
    if (old) {
        m_stack->removeWidget(old);
        old->deleteLater();
    }
    backToGrid();
    // 他报的是"卸完插件……然后内存爆炸卡死"，而那台是 Win10（我这台 Win11 上离屏量到 58→63 MB，
    // 复现不了）。每重排一次就把工作集和格子数写进日志：下次炸之前那几行能分出
    // "每次重排都长一点"（= 有东西没释放）和"某一下突然长"（= 那一下真去干了重活）。
    Utils::log(QStringLiteral("store: 重排卡片墙（卡片 %1 张 / stack %2 格 / 摘掉的那格=%3 / 工作集 %4 MB）")
                   // 数**新那一格里的**卡：整座 ToolsPage 找 "row" 会把剪贴板那张页的卡片也算进来
                   //（它们的 objectName 也叫 row —— 今天日志里那个"卡片 40 张"就是这么来的）
                   .arg(grid->findChildren<QFrame*>(QStringLiteral("row")).size())
                   .arg(m_stack->count())
                   // 这半句是 2026-10-22 加的：这一行按写法**永远该摘的是卡片墙**（第 0 格），
                   // 而插件页往后追加。哪天真摘到一张插件页，那一格就是"页的命被别人拿走"的形状 ——
                   // 现在 `g_pages` 和各壳里的 `m_page` 都是 QPointer（10-22 那次 b64 闪退之后），
                   // 崩是不会崩了，但那句"缓存发了一张已释放的页"的老故事得有证据能看出来。
                   .arg(old ? QString::fromLatin1(old->metaObject()->className())
                            : QStringLiteral("无"))
                   .arg(Platform::workingSetMB()));
}

// 回到那一面卡片墙：三处入口（那颗按钮、Esc、重排之后）共用这一条
void ToolsPage::backToGrid()
{
    m_stack->setCurrentIndex(0);
    m_back->setVisible(false);
    m_heading->setText(QStringLiteral("插件商店"));
    setStoreHeaderVisible(true);
    if (m_filterDirty) {
        // 改过滤那一下人正在看一张页，重排欠着没做；回到墙上的这一步补上。
        // 先清标记再重排：rebuildGrid() 末尾会再回一次这里，不清就递归。
        m_filterDirty = false;
        rebuildGrid();
    }
}

void ToolsPage::keyPressEvent(QKeyEvent* e)
{
    // 那张页的第二个出口。他原话是"找不到退出按钮" —— 按钮再实心也还是页眉里小小一颗，
    // 所以 Esc 也得能回去。**必须把这个事件吃掉**：设置窗是个 QDialog，Esc 传上去会把整窗关掉。
    if (e->key() == Qt::Key_Escape && m_stack && m_stack->currentIndex() != 0) {
        backToGrid();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}
