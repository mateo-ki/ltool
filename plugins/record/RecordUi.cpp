#include "RecordUi.h"
#include "platform/Launch.h"

#include "Recorder.h"
#include "Utils.h"
#include "WindowPreview.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>
#include <QComboBox>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {

struct WinRow {
    WId hwnd = 0;
    QString title;
    QString exe;
    QString fullExe;
    int w = 0;
    int h = 0;
};

// 桌面上"看得见、有标题、够大"的顶层窗口。被虚拟桌面收起（cloaked）的不算，
// 因为那种窗口 WGC 也不会给帧。
QVector<WinRow> visibleWindows(HWND skip1, HWND skip2)
{
    QVector<WinRow> out;
    struct Ctx { QVector<WinRow>* v; HWND s1; HWND s2; } ctx { &out, skip1, skip2 };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h))
            return TRUE;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked || h == c->s1 || h == c->s2)
            return TRUE;
        RECT rc {};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))))
            GetWindowRect(h, &rc);
        const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
        if (w < 80 || hh < 60)
            return TRUE;
        WCHAR title[512] = {};
        GetWindowTextW(h, title, 512);
        if (!title[0])
            return TRUE;
        WinRow r;
        r.hwnd = reinterpret_cast<WId>(h);
        r.title = QString::fromWCharArray(title);
        r.w = w;
        r.h = hh;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            WCHAR exe[512] = {};
            DWORD n = 512;
            if (QueryFullProcessImageNameW(p, 0, exe, &n)) {
                r.fullExe = QString::fromWCharArray(exe, int(n));
                r.exe = QFileInfo(r.fullExe).fileName();
            }
            CloseHandle(p);
        }
        c->v->push_back(r);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

QIcon getMonitorIcon()
{
    static QIcon s_icon;
    if (!s_icon.isNull())
        return s_icon;
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // 屏幕外框
    p.setPen(QPen(QColor(56, 189, 248), 1.6));
    p.setBrush(QColor(14, 40, 65, 220));
    p.drawRoundedRect(QRectF(3.5, 4.5, 25, 17), 3, 3);
    // 荧幕亮斑
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(56, 189, 248, 160));
    p.drawRoundedRect(QRectF(6, 7, 20, 11), 1.5, 1.5);
    // 支架底座
    p.setPen(QPen(QColor(148, 163, 184), 1.6));
    p.drawLine(QPointF(16, 21.5), QPointF(16, 26.5));
    p.drawLine(QPointF(11, 26.5), QPointF(21, 26.5));
    s_icon = QIcon(pm);
    return s_icon;
}

QIcon getWindowIcon(HWND hwnd, const QString& fullExe)
{
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 30, &res) && res) {
        QImage img = QImage::fromHICON(reinterpret_cast<HICON>(res));
        if (!img.isNull())
            return QIcon(QPixmap::fromImage(img));
    }
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 30, &res) && res) {
        QImage img = QImage::fromHICON(reinterpret_cast<HICON>(res));
        if (!img.isNull())
            return QIcon(QPixmap::fromImage(img));
    }
    HICON hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!hIcon)
        hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    if (hIcon) {
        QImage img = QImage::fromHICON(hIcon);
        if (!img.isNull())
            return QIcon(QPixmap::fromImage(img));
    }
    if (!fullExe.isEmpty()) {
        HICON hLarge = nullptr, hSmall = nullptr;
        if (ExtractIconExW((LPCWSTR)fullExe.utf16(), 0, &hLarge, &hSmall, 1) > 0) {
            HICON chosen = hSmall ? hSmall : hLarge;
            QImage img = QImage::fromHICON(chosen);
            if (hLarge) DestroyIcon(hLarge);
            if (hSmall && hSmall != hLarge) DestroyIcon(hSmall);
            if (!img.isNull())
                return QIcon(QPixmap::fromImage(img));
        }
    }
    static QIcon s_fallback;
    if (s_fallback.isNull()) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(100, 116, 139), 1.5));
        p.setBrush(QColor(30, 41, 59, 200));
        p.drawRoundedRect(QRectF(3.5, 3.5, 25, 25), 4, 4);
        p.setPen(QPen(QColor(148, 163, 184), 1.2));
        p.drawLine(QPointF(3.5, 10.5), QPointF(28.5, 10.5));
        s_fallback = QIcon(pm);
    }
    return s_fallback;
}

class WindowItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(option.rect.width(), 46);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = (option.state & QStyle::State_Selected);
        const bool hover = (option.state & QStyle::State_MouseOver);

        if (selected) {
            painter->setPen(QPen(QColor(59, 130, 246, 220), 1.0));
            painter->setBrush(QColor(33, 44, 72));
            painter->drawRoundedRect(r, 6, 6);
            // 选中左边竖条指示线
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(59, 130, 246));
            painter->drawRoundedRect(QRectF(r.left() + 2.5, r.top() + 9, 3, r.height() - 18), 1.5, 1.5);
        } else if (hover) {
            painter->setPen(QPen(QColor(55, 58, 70), 1.0));
            painter->setBrush(QColor(32, 34, 42));
            painter->drawRoundedRect(r, 6, 6);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(22, 23, 28));
            painter->drawRoundedRect(r, 6, 6);
        }

        // 图标
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const int iconSize = 26;
        const QRect iconRect(r.left() + (selected ? 10 : 8), r.top() + (r.height() - iconSize) / 2, iconSize, iconSize);
        if (!icon.isNull()) {
            icon.paint(painter, iconRect, Qt::AlignCenter);
        }

        // 文本
        const QString rawTitle = index.data(Qt::UserRole + 1).toString();
        const bool isMonitor = index.data(Qt::UserRole + 2).toBool();
        const QString exe = index.data(Qt::UserRole + 3).toString();
        const QString res = index.data(Qt::UserRole + 4).toString();

        const int textX = iconRect.right() + 8;
        const int textW = r.right() - textX - 8;

        // 第一行：标题
        QFont titleFont = painter->font();
        titleFont.setPointSize(9);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(selected ? QColor(255, 255, 255) : QColor(235, 238, 245));

        QFontMetrics fm(titleFont);
        const QString titleStr = rawTitle.isEmpty() ? index.data(Qt::DisplayRole).toString() : rawTitle;
        const QString elidedTitle = fm.elidedText(titleStr, Qt::ElideRight, textW);
        painter->drawText(QRect(textX, r.top() + 5, textW, 17), Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

        // 第二行：子标题（exe / 屏幕 + 分辨率）
        QFont subFont = painter->font();
        subFont.setPointSize(8);
        subFont.setBold(false);
        painter->setFont(subFont);

        QString subText;
        if (isMonitor) {
            subText = QStringLiteral("主显示器 · 物理屏幕 · %1").arg(res.isEmpty() ? QStringLiteral("整块屏幕") : res);
            painter->setPen(selected ? QColor(147, 197, 253) : QColor(56, 189, 248));
        } else {
            subText = QStringLiteral("%1 · %2").arg(exe.isEmpty() ? QStringLiteral("应用程序") : exe, res);
            painter->setPen(selected ? QColor(190, 200, 220) : QColor(130, 138, 150));
        }
        painter->drawText(QRect(textX, r.top() + 23, textW, 16), Qt::AlignLeft | Qt::AlignVCenter, subText);

        painter->restore();
    }
};

Recorder* g_rec = nullptr;
RecordBar* g_bar = nullptr;
WindowPicker* g_picker = nullptr;

// 主显示器那块屏的 HMONITOR + 物理像素尺寸（选择器顶上那条"整个屏幕"就用它）。
// 主屏一定包含虚拟桌面原点，所以不用比屏幕名字（Qt 的名字和 GetMonitorInfo 的没有可靠对应）。
struct ScreenRow { WId mon = 0; int w = 0; int h = 0; };
ScreenRow primaryMonitor()
{
    ScreenRow r;
    const POINT origin { 0, 0 };
    if (HMONITOR m = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi)) {
            r.mon = reinterpret_cast<WId>(m);
            r.w = mi.rcMonitor.right - mi.rcMonitor.left;
            r.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    }
    return r;
}

struct QualityPreset {
    const char* name;
    double bpp;          // bits per pixel per frame
};
// 顺序就是下拉框里的顺序（存在 config.ini 的 ui/recQuality 里是下标）
const QualityPreset kQualities[] = {
    { "流畅", 0.060 },
    { "标准", 0.145 },     // 就是以前那个固定 8000kbps 在 2560x1440@15 上的等效值
    { "高清", 0.250 },
    { "超清", 0.400 },
};
const int kQualityCount = int(sizeof(kQualities) / sizeof(kQualities[0]));
// 长边上限档位（存在 ui/recMaxWidth 里的是这个数，0 = 不缩）
const int kWidthCaps[] = { 0, 1920, 1280, 854 };
// 那一档显示成什么字：下拉框里那四行和插件状态页上那句「画面档」都由它生成 ——
// 档位表只有一份，字面量抄两遍迟早对不上。
QString widthCapText(int cap)
{
    return cap > 0 ? QStringLiteral("长边 ≤ %1").arg(cap) : QStringLiteral("原始（不缩）");
}

int recQualityIndex()
{
    const int i = QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                      .value(QStringLiteral("ui/recQuality"), 1).toInt();
    return qBound(0, i, kQualityCount - 1);
}
int recMaxWidth()
{
    const int w = QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                      .value(QStringLiteral("ui/recMaxWidth"), 0).toInt();
    for (int c : kWidthCaps)
        if (c == w)
            return w;
    return 0;        // 配置里被手改成不认识的数：按"不缩"处理，别拿它去算画幅
}
// 码率按**编码后**的画幅算：缩到 1280 就按 1280 的量给，不然等于白花一倍的钱
int recBitrateKbps(int quality, int w, int h, int fps)
{
    // 算法本体在 Recorder::bitrateKbps（测试 --recq 也调它），这里只挑档
    return Recorder::bitrateKbps(kQualities[qBound(0, quality, kQualityCount - 1)].bpp, w, h, fps);
}

void beginRecording(WId target, const QString& title, bool monitor)
{
    QDir().mkpath(Utils::recordingsDir());
    const QString path = QString("%1/rec-%2.mp4")
                             .arg(Utils::recordingsDir(),
                                  QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    if (!g_rec)
        g_rec = new Recorder;
    QSettings st(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    Recorder::Options o;
    o.fps = qBound(5, st.value(QStringLiteral("ui/recFps"), 15).toInt(), 60);
    const int q = recQualityIndex();
    o.maxWidth = recMaxWidth();
    // 码率要看编码后的画幅，所以先把这一局的画幅量出来（屏幕/窗口的原始尺寸都在这儿算）
    int capW = 0, capH = 0;
    if (monitor) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target), &mi)) {
            capW = mi.rcMonitor.right - mi.rcMonitor.left;
            capH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    } else {
        RECT rc {};
        if (GetWindowRect(reinterpret_cast<HWND>(target), &rc)) {
            capW = rc.right - rc.left;
            capH = rc.bottom - rc.top;
        }
    }
    const QSize enc = Recorder::encodedSize(capW, capH, o.maxWidth);
    o.videoBitrateKbps = recBitrateKbps(q, enc.width(), enc.height(), o.fps);
    QString err;
    if (!g_rec->start(target, o, path, &err, monitor)) {
        Utils::log(QString("record start failed: %1").arg(err));
        QMessageBox::warning(nullptr, QStringLiteral("录不了"),
                             err.isEmpty() ? QStringLiteral("不知道哪里不对，反正没录起来") : err);
        return;
    }
    if (g_bar) {
        g_bar->close();
        g_bar = nullptr;
    }
    g_bar = new RecordBar(g_rec, path);
    g_bar->show();
    int rw = 0, rh = 0;
    if (monitor) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target), &mi)) {
            rw = mi.rcMonitor.right - mi.rcMonitor.left;
            rh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    } else {
        RECT rc {};
        if (GetWindowRect(reinterpret_cast<HWND>(target), &rc)) {
            rw = rc.right - rc.left;
            rh = rc.bottom - rc.top;
        }
    }
    Utils::log(QString("record start: %1 title=%2 kind=%3 fps=%4 画质=%5 kbps=%6 采集=%7x%8 编码=%9x%10")
                   .arg(path, title, monitor ? QStringLiteral("屏幕") : QStringLiteral("窗口"))
                   .arg(o.fps).arg(QString::fromLatin1(kQualities[q].name))
                   .arg(o.videoBitrateKbps).arg(rw).arg(rh).arg(enc.width()).arg(enc.height()));
}

} // namespace

// ---------------- 窗口选择 ----------------

WindowPicker::WindowPicker(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(7);

    auto* head = new QHBoxLayout;
    head->setSpacing(6);

    // 录屏指示徽章
    auto* badge = new QLabel(this);
    badge->setFixedSize(20, 20);
    badge->setStyleSheet("background:transparent;border:none;");
    {
        QPixmap pm(20, 20);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(239, 68, 68, 40));
        p.setPen(QPen(QColor(239, 68, 68, 140), 1));
        p.drawRoundedRect(QRectF(1, 1, 18, 18), 4, 4);
        p.setBrush(QColor(239, 68, 68));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(6, 6, 8, 8));
        badge->setPixmap(pm);
    }

    auto* title = new QLabel(QStringLiteral("录什么？（整块屏幕 / 某一个窗口）"), this);
    title->setStyleSheet("color:#f3f4f6;font-size:13px;font-weight:600;background:transparent;border:none;");
    // 这窗没标题栏，"按住哪儿能挪"得写在人眼看得见的地方
    title->setToolTip(QStringLiteral("按住标题栏（或空白处）可以拖动这个窗口 · Esc 或点空白处取消"));

    auto* f2Tag = new QLabel(QStringLiteral("F2"), this);
    f2Tag->setStyleSheet("color:#94a3b8;font-size:10px;font-weight:600;background:#242630;border:1px solid #363946;border-radius:4px;padding:1px 5px;");

    // 这窗没标题栏，光靠 Esc / 点到别处 / 再按一次 F2 还是得有个人眼的落点
    m_close = new QPushButton(QStringLiteral("✕"), this);
    m_close->setObjectName(QStringLiteral("close"));
    m_close->setFixedSize(24, 22);
    m_close->setCursor(Qt::PointingHandCursor);
    m_close->setToolTip(QStringLiteral("不录了（同 Esc）"));

    head->addWidget(badge);
    head->addWidget(title);
    head->addWidget(f2Tag);
    head->addStretch(1);
    head->addWidget(m_close);

    auto* filter = new QLineEdit(this);
    filter->setPlaceholderText(QStringLiteral("输入关键字过滤（输「屏」筛选屏幕，输应用名过滤窗口）… 点一条选中预览，按「开始录制」开录，Esc 取消"));
    auto* list = new QListWidget(this);
    list->setItemDelegate(new WindowItemDelegate(this));
    list->setSelectionMode(QAbstractItemView::SingleSelection);

    // 画质 / 画面大小：录之前顺手能改的两件事，改一下就记住（config.ini），下次按 F2 还是这个。
    // 挂在列表下面一行，上面各行的高度都不动 —— 这个窗每次 F2 都重建，排布必须稳定。
    auto* opts = new QHBoxLayout;
    opts->setSpacing(6);
    auto* qCap = new QLabel(QStringLiteral("画质"), this);
    qCap->setStyleSheet("color:#94a3b8;font-size:12px;font-weight:600;background:transparent;border:none;");
    auto* wCap = new QLabel(QStringLiteral("画面"), this);
    wCap->setStyleSheet("color:#94a3b8;font-size:12px;font-weight:600;background:transparent;border:none;");
    auto* quality = new QComboBox(this);
    quality->setObjectName(QStringLiteral("recQuality"));
    for (const QualityPreset& q : kQualities)
        quality->addItem(QString::fromUtf8(q.name));
    quality->setToolTip(QStringLiteral("标准这一档就是以前默认的那个大小；码率按画幅和帧率算，缩小的画面不会白花空间。"));
    quality->setCurrentIndex(recQualityIndex());
    auto* width = new QComboBox(this);
    width->setObjectName(QStringLiteral("recMaxWidth"));
    for (int cap : kWidthCaps)
        width->addItem(widthCapText(cap), cap);
    width->setToolTip(QStringLiteral("缩的是编码画幅（面积平均，不是隔行取样）。选小一点文件能小好几倍，代价是字不那么锐。"));
    {
        const int want = recMaxWidth();
        for (int i = 0; i < width->count(); ++i)
            if (width->itemData(i).toInt() == want)
                width->setCurrentIndex(i);
    }
    opts->addWidget(qCap);
    opts->addWidget(quality);
    opts->addSpacing(8);
    opts->addWidget(wCap);
    opts->addWidget(width);
    opts->addStretch(1);

    // 「开始录制」：点条目只选中，开录要按这颗（或回车）。摆在最后一行的中间，
    // 这一行其余的控件（画质/画面）都在左边，按钮在中间，右边留白把它顶出来。
    m_rec = new QPushButton(QStringLiteral("开始录制"), this);
    m_rec->setObjectName(QStringLiteral("recStart"));
    m_rec->setCursor(Qt::PointingHandCursor);
    m_rec->setMinimumWidth(130);
    m_rec->setToolTip(QStringLiteral("录当前选中那一条（同回车）"));
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);
    bottom->addLayout(opts);
    bottom->addStretch(1);
    bottom->addWidget(m_rec);
    bottom->addStretch(1);
    // 右边垫一块和左边那组一样宽的空白：这样按钮落在**整个窗的中线上**，
    // 而不是"剩下那点空间的中间"（那样它永远偏右）。
    auto* pad = new QWidget(this);
    pad->setStyleSheet("background:transparent;border:none;");
    pad->setFixedWidth(opts->sizeHint().width());
    bottom->addWidget(pad);

    // 右边那一块实时预览：拿的就是 Recorder 同一一路（WGC），并且按当前「画面」档
    // 缩到编码尺寸 —— 这块小图就是将要进 mp4 的那一张，不是另画一份示意图。
    m_pv = new WindowPreview(this);
    m_shot = new QLabel(QStringLiteral("📷 尚未选择目标"), this);
    m_shot->setObjectName(QStringLiteral("preview"));
    m_shot->setAlignment(Qt::AlignCenter);
    m_shot->setMinimumSize(270, 200);
    m_shot->setScaledContents(false);
    m_cap = new QLabel(QStringLiteral("选中条目即可在此查看录制画面实时取景"), this);
    m_cap->setObjectName(QStringLiteral("previewCap"));
    m_cap->setAlignment(Qt::AlignCenter);
    m_cap->setWordWrap(true);

    auto* pvTitle = new QLabel(QStringLiteral("实时取景 · LIVE PREVIEW"), this);
    pvTitle->setStyleSheet("color:#64748b;font-size:10px;font-weight:600;letter-spacing:1px;background:transparent;border:none;");

    auto* right = new QVBoxLayout;
    right->setSpacing(5);
    right->addWidget(pvTitle);
    right->addWidget(m_shot, 1);
    right->addWidget(m_cap);
    auto* mid = new QHBoxLayout;
    mid->setSpacing(10);
    mid->addWidget(list, 5);
    mid->addLayout(right, 4);

    lay->addLayout(head);
    lay->addWidget(filter);
    lay->addLayout(mid, 1);
    lay->addLayout(bottom);
    list->setObjectName(QStringLiteral("list"));
    filter->setObjectName(QStringLiteral("filter"));

    setStyleSheet("WindowPicker{background:transparent;}"
                  "QLineEdit#filter{background:#14151a;color:#f1f5f9;border:1px solid #2a2c36;"
                  "border-radius:6px;padding:6px 10px;font-size:12px;}"
                  "QLineEdit#filter:hover{border:1px solid #373a48;}"
                  "QLineEdit#filter:focus{background:#181920;border:1px solid #3b82f6;}"
                  "QListWidget#list{background:#14151a;border:1px solid #2a2c36;"
                  "border-radius:8px;padding:4px;outline:none;}"
                  "QListWidget#list::item{border:none;padding:0px;}"
                  "QScrollBar:vertical{background:transparent;width:6px;margin:4px 0;}"
                  "QScrollBar::handle:vertical{background:#333644;border-radius:3px;min-height:20px;}"
                  "QScrollBar::handle:vertical:hover{background:#4b5063;}"
                  "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
                  "QPushButton{background:#3a3d46;color:#e8ebf2;border:none;border-radius:4px;}"
                  "QPushButton:hover{background:#e04a4a;}"
                  "QPushButton#close{background:rgba(255,255,255,0.06);color:#94a3b8;"
                  "border:1px solid rgba(255,255,255,0.08);border-radius:11px;font-size:11px;font-weight:bold;}"
                  "QPushButton#close:hover{background:#ef4444;color:#ffffff;border:1px solid #ef4444;}"
                  "QPushButton#close:pressed{background:#dc2626;}"
                  "QComboBox{background:#1b1c22;color:#f1f5f9;border:1px solid #2e313d;"
                  "border-radius:6px;padding:4px 22px 4px 10px;font-size:12px;min-height:22px;}"
                  "QComboBox:hover{background:#22242c;border:1px solid #3e4252;}"
                  "QComboBox:focus{border:1px solid #3b82f6;}"
                  "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:top right;width:18px;border:none;}"
                  "QComboBox::down-arrow{image:none;border-left:4px solid transparent;border-right:4px solid transparent;"
                  "border-top:5px solid #94a3b8;margin-right:6px;}"
                  "QComboBox QAbstractItemView{background:#1b1c22;color:#f1f5f9;border:1px solid #323544;"
                  "border-radius:6px;padding:4px;selection-background-color:#3b82f6;selection-color:#ffffff;outline:none;}"
                  "QComboBox QAbstractItemView::item{padding:5px 8px;border-radius:4px;}"
                  "QLabel#preview{background:#0f1013;color:#64748b;border:1px solid #282a33;"
                  "border-radius:8px;font-size:12px;}"
                  "QLabel#previewCap{background:#17181e;border:1px solid #282a33;border-radius:6px;"
                  "color:#94a3b8;font-size:11px;padding:6px 8px;}"
                  "QPushButton#recStart{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #2563eb,stop:1 #3b82f6);"
                  "color:#ffffff;font-size:13px;font-weight:600;border:1px solid #3b82f6;border-radius:6px;padding:6px 22px;}"
                  "QPushButton#recStart:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #3b82f6,stop:1 #60a5fa);"
                  "border:1px solid #60a5fa;}"
                  "QPushButton#recStart:pressed{background:#1d4ed8;}"
                  "QPushButton#recStart:disabled{background:#262830;color:#64748b;border:1px solid #323540;}");
    connect(m_close, &QPushButton::clicked, this, &QWidget::hide);
    const auto saveOpts = [quality, width] {
        QSettings st(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        st.setValue(QStringLiteral("ui/recQuality"), quality->currentIndex());
        st.setValue(QStringLiteral("ui/recMaxWidth"), width->currentData().toInt());
        st.sync();
        Utils::log(QStringLiteral("record opts: 画质=%1 画面=%2")
                       .arg(quality->currentText(), width->currentText()));
    };
    // 用 currentIndexChanged 而不是 activated：activated 只有真人点才算，自测没法
    // 程序化地"按一下下拉框"。回填那两次 setCurrentIndex 都发生在这两条 connect 之前
    // （quality 在创建时就按配置摆好，width 的匹配循环也在加进布局之前），
    // 所以初始化不会触发保存 —— 以后谁把回填挪到 connect 后面，就会每次按 F2 多写一遍配置。
    connect(quality, &QComboBox::currentIndexChanged, quality, [saveOpts](int) { saveOpts(); });
    // 「画面」那一档换掉时只改预览的缩放目标：编码尺寸是每帧现算的，不必重开取帧那一路
    connect(width, &QComboBox::currentIndexChanged, this, [this, saveOpts](int) {
        saveOpts();
        m_pv->setMaxWidth(recMaxWidth());
        refreshPreview();     // 那行字要跟着换（"编码后 1280x720"），图下一帧自己会来
    });
    resize(760, 440);

    const auto rebuild = [this, list, filter] {
        list->clear();
        // 整块屏幕排在最上面一条：录屏的人十次有九次要的是"就录我看到的这一整块"。
        // UserRole+2 是"这条是屏幕不是窗口"的标志，没设的（窗口那些行）toBool() 就是 false。
        const ScreenRow scr = primaryMonitor();
        if (scr.mon) {
            auto* it = new QListWidgetItem(QStringLiteral("整个屏幕 · 主显示器 · %1x%2")
                                               .arg(scr.w).arg(scr.h), list);
            it->setData(Qt::UserRole, qlonglong(scr.mon));
            it->setData(Qt::UserRole + 1, QStringLiteral("整个屏幕"));
            it->setData(Qt::UserRole + 2, true);
            it->setData(Qt::UserRole + 3, QStringLiteral("主显示器"));
            it->setData(Qt::UserRole + 4, QStringLiteral("%1 × %2").arg(scr.w).arg(scr.h));
            it->setIcon(getMonitorIcon());
        }
        const HWND skip = reinterpret_cast<HWND>(winId());
        for (const WinRow& r : visibleWindows(skip, g_bar ? reinterpret_cast<HWND>(g_bar->winId()) : nullptr)) {
            auto* it = new QListWidgetItem(QStringLiteral("%1  ·  %2  ·  %3x%4")
                                               .arg(r.title.left(46), r.exe).arg(r.w).arg(r.h), list);
            it->setData(Qt::UserRole, qlonglong(r.hwnd));
            it->setData(Qt::UserRole + 1, r.title);
            it->setData(Qt::UserRole + 2, false);
            it->setData(Qt::UserRole + 3, r.exe);
            it->setData(Qt::UserRole + 4, QStringLiteral("%1 × %2").arg(r.w).arg(r.h));
            it->setIcon(getWindowIcon(reinterpret_cast<HWND>(r.hwnd), r.fullExe));
        }
        if (list->count())
            list->setCurrentRow(0);
    };
    rebuild();
    m_list = list;

    connect(filter, &QLineEdit::textChanged, this, [list, filter] {
        const QString needle = filter->text().trimmed();
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!needle.isEmpty() && !list->item(i)->text().contains(needle, Qt::CaseInsensitive));
    });
    // 点条目**只选中**（顺带换预览），开录要按「开始录制」那颗或敲回车。
    // 以前是 itemActivated（双击/回车）直接开录 —— 手一抖点两下就开始录了，而且没有任何
    // 一步能反悔，也没有一处能看见"录进去的到底是这么大的一张图"。
    connect(list, &QListWidget::currentRowChanged, this, [this](int) { refreshPreview(); });
    connect(m_rec, &QPushButton::clicked, this, [this] { chooseCurrent(); });
    connect(filter, &QLineEdit::returnPressed, this, [this] {
        // 回车先从可见项里挑第一个（"输入 Qoder 回车"就是录 Qoder）
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden()) {
                m_list->setCurrentRow(i);
                break;
            }
        }
        chooseCurrent();
    });
    connect(m_pv, &WindowPreview::frameReady, this, [this] {
        const QPixmap pm = m_pv->frame();
        if (!pm.isNull()) {
            m_shot->setPixmap(pm.scaled(m_shot->size() - QSize(10, 10), Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
        }
        const QString cap = m_pv->lastError().isEmpty()
            ? QStringLiteral("编码后 %1x%2 · 采集 %3x%4 · 已取 %5 帧")
                  .arg(m_pv->shownSize().width()).arg(m_pv->shownSize().height())
                  .arg(m_pv->sourceSize().width()).arg(m_pv->sourceSize().height())
                  .arg(m_pv->framesSeen())
            : m_pv->lastError();
        m_cap->setText(cap);
    });

    // Esc 要能从输入框和列表里也收得到：QLineEdit 会把 Esc 吃掉（它有自己的 escapePressed），
    // 所以除了 keyPressEvent，还得给这两个子控件装事件过滤器。
    filter->installEventFilter(this);
    list->installEventFilter(this);
}

void WindowPicker::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool WindowPicker::eventFilter(QObject* watched, QEvent* e)
{
    if (e->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        hide();
        return true;
    }
    if (watched == m_list && e->type() == QEvent::KeyPress) {
        const int k = static_cast<QKeyEvent*>(e)->key();
        if (k == Qt::Key_Return || k == Qt::Key_Enter) {
            // 列表里按回车 = 按「开始录制」。条目本身不再因为双击开录了，键盘这条快路留着。
            chooseCurrent();
            return true;
        }
    }
    if (e->type() == QEvent::WindowDeactivate) {
        // 点到别处 = 不录了。这个窗是 activateWindow 过的，失焦就是用户真的走开了；
        // 不收起的话它会一直压在要录的那个窗口上面。
        hide();
        return false;
    }
    return QWidget::eventFilter(watched, e);
}

void WindowPicker::chooseCurrent()
{
    auto* it = m_list ? m_list->currentItem() : nullptr;
    if (!it || it->isHidden())
        return;
    emit picked(WId(it->data(Qt::UserRole).toLongLong()), it->data(Qt::UserRole + 1).toString(),
                it->data(Qt::UserRole + 2).toBool());
}

void WindowPicker::refreshPreview()
{
    auto* it = m_list ? m_list->currentItem() : nullptr;
    if (!isVisible() || !it || it->isHidden()) {
        m_pv->stop();
        m_shot->clear();
        m_shot->setText(it ? QStringLiteral("该窗口当前不可见") : QStringLiteral("📷 尚未选择目标"));
        m_cap->setText(QStringLiteral("选中条目即可在此查看录制画面实时取景"));
        return;
    }
    m_pv->setMaxWidth(recMaxWidth());
    QString err;
    if (!m_pv->begin(WId(it->data(Qt::UserRole).toLongLong()),
                     it->data(Qt::UserRole + 2).toBool(), &err)) {
        m_shot->clear();
        m_shot->setText(QStringLiteral("⚠️ 无法获取预览画面"));
        m_cap->setText(err);
    } else {
        m_shot->clear();
        m_shot->setText(QStringLiteral("正在捕获第一帧…"));
        m_cap->setText(QStringLiteral("编码后 %1")
                           .arg(m_pv->shownSize().isValid() && m_pv->shownSize().width()
                                    ? QStringLiteral("%1x%2")
                                          .arg(m_pv->shownSize().width())
                                          .arg(m_pv->shownSize().height())
                                    : QStringLiteral("?")));
    }
}

void WindowPicker::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_press = e->globalPosition().toPoint() - geometry().topLeft();
    }
}

void WindowPicker::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(e->globalPosition().toPoint() - m_press);
}

void WindowPicker::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
}

void WindowPicker::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshPreview();
}

void WindowPicker::hideEvent(QHideEvent* e)
{
    // 收起来就把 WGC 那一路放掉：不放的话它一直在别人窗口上面抓帧，白烧 CPU，
    // 而且紧接着 Recorder 要对同一个窗口再开一路，两路叠着没好处。
    m_pv->stop();
    QWidget::hideEvent(e);
}

void WindowPicker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 12, 12);
    p.fillPath(path, QColor(24, 25, 31, 250));
    p.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
    p.drawPath(path);
}

// ---------------- 悬浮条 ----------------

RecordBar::RecordBar(Recorder* rec, const QString& path, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      m_rec(rec), m_path(path)
{
    // 别抢前台：他正要点的是被录的那个窗口
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground, true);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(14, 4, 10, 4);
    lay->setSpacing(8);
    auto* dot = new QLabel(QStringLiteral("●"), this);
    dot->setObjectName(QStringLiteral("recDot"));
    dot->setStyleSheet("color:#ef4444;border:none;background:transparent;font-size:12px;");
    m_text = new QLabel(QStringLiteral("正在录…"), this);
    m_text->setObjectName(QStringLiteral("text"));
    m_stop = new QPushButton(QStringLiteral("停止"), this);
    m_stop->setObjectName(QStringLiteral("stop"));
    m_stop->setCursor(Qt::PointingHandCursor);
    m_cancel = new QPushButton(QStringLiteral("不要了"), this);
    m_cancel->setObjectName(QStringLiteral("cancel"));
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setToolTip(QStringLiteral("停下这一局并且把已经录的丢掉（文件删掉，不留在录像目录里）"));
    lay->addWidget(dot);
    lay->addWidget(m_text);
    lay->addStretch(1);
    lay->addWidget(m_cancel);
    lay->addWidget(m_stop);
    setStyleSheet("RecordBar{background:transparent;}"
                  "QLabel#recDot{background:transparent;border:none;color:#ef4444;font-size:12px;}"
                  "QLabel#text{color:#f1f5f9;background:transparent;border:none;font-size:12px;font-weight:600;"
                  "font-family:'Segoe UI','Consolas',monospace;}"
                  "QPushButton#cancel{background:rgba(255,255,255,0.08);color:#94a3b8;"
                  "border:1px solid rgba(255,255,255,0.1);border-radius:12px;padding:3px 10px;"
                  "font-size:11px;font-weight:500;}"
                  "QPushButton#cancel:hover{background:rgba(239,68,68,0.22);color:#fca5a5;"
                  "border:1px solid rgba(239,68,68,0.4);}"
                  "QPushButton#cancel:pressed{background:rgba(239,68,68,0.35);}"
                  "QPushButton#cancel:disabled{background:rgba(255,255,255,0.03);color:#475569;border:none;}"
                  "QPushButton#stop{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #ef4444,stop:1 #dc2626);"
                  "color:#ffffff;border:1px solid #ef4444;border-radius:12px;padding:3px 14px;"
                  "font-size:11px;font-weight:600;}"
                  "QPushButton#stop:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #f87171,stop:1 #ef4444);"
                  "border:1px solid #f87171;}"
                  "QPushButton#stop:pressed{background:#b91c1c;}"
                  "QPushButton#stop:disabled{background:#334155;color:#94a3b8;border:1px solid #475569;}");
    setFixedWidth(330);
    setFixedHeight(36);

    const QRect a = screen()->availableGeometry();
    move(a.right() - width() - 16, a.bottom() - height() - 12);

    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_stop->setEnabled(false);
        m_cancel->setEnabled(false);
        m_text->setText(QStringLiteral("正在收尾（编码要把最后一帧写完）…"));
        QCoreApplication::processEvents();
        m_rec->stop();
        finish(m_rec->errorText());
    });
    // 「不要了」= 停下这一局并且把文件删掉。Recorder 没有"只停不写盘"这条路
    // （MP4 是收尾才落盘的），所以照常收尾，然后把那个废文件原地删了 ——
    // 录到一半的东西留在录像目录里就是垃圾，还得人去删。
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_stop->setEnabled(false);
        m_cancel->setEnabled(false);
        m_text->setText(QStringLiteral("正在收尾（要把这一局丢掉）…"));
        QCoreApplication::processEvents();
        m_discard = true;
        m_rec->stop();
        finish(m_rec->errorText());
    });

    auto* tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &RecordBar::poll);
    tick->start(200);
}

void RecordBar::poll()
{
    if (m_rec->isDone()) {              // 窗口关了/改尺寸了：worker 自己收的尾
        if (!m_finished)
            finish(m_rec->errorText());
        return;
    }
    const qint64 ms = m_rec->elapsedMs();
    // 不报"已写多少 KB"：MP4 是收尾才落盘的，录的过程中一直是 0，写上去就是撒谎
    m_text->setText(QStringLiteral("⏺ %1:%2 · %3 帧")
                        .arg(ms / 60000)
                        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
                        .arg(m_rec->framesCaptured()));
}

void RecordBar::finish(const QString& error)
{
    m_finished = true;   // 只收一次尾：以前这里每 200ms 重进一遍，日志被刷爆
    if (m_discard) {
        QFile::remove(m_path);
        m_text->setText(QStringLiteral("已取消，没保存（丢掉 %1 帧）").arg(m_rec->framesCaptured()));
        m_stop->setText(QStringLiteral("关闭"));
        m_stop->setStyleSheet("QPushButton#stop{background:rgba(255,255,255,0.1);color:#e2e8f0;border:1px solid rgba(255,255,255,0.15);border-radius:12px;padding:3px 14px;font-size:11px;font-weight:600;} QPushButton#stop:hover{background:rgba(255,255,255,0.18);}");
        m_cancel->setVisible(false);
        Utils::log(QString("record cancelled: %1 frames=%2 文件已删")
                       .arg(m_path).arg(m_rec->framesCaptured()));
        m_stop->setEnabled(true);
        disconnect(m_stop, nullptr, this, nullptr);
        connect(m_stop, &QPushButton::clicked, this, [this] { close(); });
        adjustSize();
        setFixedWidth(qMax(340, width() + 20));
        const QRect a = screen()->availableGeometry();
        move(qMax(a.left() + 10, a.right() - width() - 16),
             qMax(a.top() + 10, a.bottom() - height() - 12));
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &QWidget::close);
        t->setSingleShot(true);
        t->start(8000);
        return;
    }
    if (!error.isEmpty()) {
        m_text->setText(QStringLiteral("没录成：%1").arg(error));
        m_stop->setText(QStringLiteral("知道了"));
        m_stop->setStyleSheet("QPushButton#stop{background:#dc2626;color:#ffffff;border:none;border-radius:12px;padding:3px 14px;font-size:11px;font-weight:600;} QPushButton#stop:hover{background:#ef4444;}");
        Utils::log(QString("record failed: %1 path=%2").arg(error, m_path));
    } else {
        const qint64 bytes = m_rec->fileBytes();
        // 13KB 显示成 "0.0 MB" 就是没说实话
        const QString size = bytes >= 1024 * 1024
            ? QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1)
            : QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
        m_text->setText(QStringLiteral("已保存 %1 帧 / %2 → %3")
                            .arg(m_rec->framesCaptured())
                            .arg(size, QFileInfo(m_path).fileName()));
        m_stop->setText(QStringLiteral("打开所在目录"));
        m_stop->setStyleSheet("QPushButton#stop{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #2563eb,stop:1 #3b82f6);color:#ffffff;border:1px solid #3b82f6;border-radius:12px;padding:3px 14px;font-size:11px;font-weight:600;} QPushButton#stop:hover{background:#3b82f6;}");
        Utils::log(QString("record saved: %1 frames=%2 bytes=%3")
                       .arg(m_path).arg(m_rec->framesCaptured()).arg(bytes));
    }
    m_stop->setEnabled(true);
    disconnect(m_stop, nullptr, this, nullptr);
    connect(m_stop, &QPushButton::clicked, this, [this, error] {
        if (error.isEmpty())
            Platform::reveal(m_path);
        close();
    });
    adjustSize();
    setFixedWidth(qMax(360, width() + 20));
    const QRect a = screen()->availableGeometry();
    move(qMax(a.left() + 10, a.right() - width() - 16),
         qMax(a.top() + 10, a.bottom() - height() - 12));
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &QWidget::close);
    t->setSingleShot(true);
    t->start(error.isEmpty() ? 30000 : 12000);   // 结果留在条上，不用弹窗打断
}

void RecordBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 18, 18);
    p.fillPath(path, QColor(18, 19, 25, 240));
    p.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
    p.drawPath(path);
}

void RecordBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_press = e->globalPosition().toPoint() - geometry().topLeft();
    }
}

void RecordBar::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(e->globalPosition().toPoint() - m_press);
}

void RecordBar::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
}

// ---------------- 入口 ----------------

namespace RecordUi {

void startViaPicker()
{
    if (isRecording()) {
        if (g_bar)
            g_bar->raise();
        return;
    }
    if (!g_rec)
        g_rec = new Recorder;
    QString why;
    if (!Recorder::available(&why)) {
        QMessageBox::warning(nullptr, QStringLiteral("录屏"), why);
        return;
    }
    // 选择窗开着的时候再按一次 F2 = 取消（它没有标题栏、没有关闭按钮，
    // 光靠 Esc 和"点到别处"还不够：手就放在热键上，再按一次最自然）
    if (g_picker && g_picker->isVisible()) {
        g_picker->hide();
        return;
    }
    // 每次都要重新数一遍窗口（他可能刚开/刚关了几个）
    if (g_picker) {
        g_picker->hide();
        delete g_picker;
        g_picker = nullptr;
    }
    g_picker = new WindowPicker;
    QObject::connect(g_picker, &WindowPicker::picked,
                     [](WId target, const QString& title, bool monitor) {
        if (g_picker)
            g_picker->hide();
        beginRecording(target, title, monitor);
    });
    const QPoint cur = QCursor::pos();
    const QRect a = QGuiApplication::screenAt(cur)
                        ? QGuiApplication::screenAt(cur)->availableGeometry()
                        : QApplication::primaryScreen()->availableGeometry();
    g_picker->move(qMin(cur.x() + 12, a.right() - g_picker->width() - 8),
                   qMin(cur.y() + 12, a.bottom() - g_picker->height() - 8));
    g_picker->show();
    g_picker->raise();
    g_picker->activateWindow();
}

bool isRecording()
{
    return g_rec && g_rec->running();
}

QString qualityText()
{
    return QString::fromUtf8(kQualities[recQualityIndex()].name);
}

QString maxWidthText()
{
    return widthCapText(recMaxWidth());
}

} // namespace RecordUi
