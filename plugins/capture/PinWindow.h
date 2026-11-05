#pragma once

#include <QWidget>
#include <QPixmap>
#include <QHash>
#include <QString>
#include <QVector>

#include "Anno.h"
#include "CaptureHistory.h"
#include "Ocr.h"

class QPushButton;
class QMenu;
class QWidget;
namespace Ocr { struct Line; }

// 置顶截图窗口：主图 + 左侧竖向历史缩略图（点一下换图）+ 底部标注工具栏。
// 工具栏里的矩形/圆/箭头/马赛克/裁剪和截图浮层是同一套（Anno），
// 每改一笔都压进撤销栈（Ctrl+Z 或点「撤销」回上一步），编辑结果同步回历史那一条。
//
// 画出来的框/圆/箭头/打码块是"对象"（m_shapes），不是烙进图里的像素：
// 不选工具时点它们可以选中，然后拖动改位置、拉八个手柄改大小、Delete 删掉。
// 底图（m_pm）只装原图 + 涂抹/文字/裁剪这类像素级操作；对外（复制/保存/历史）
// 给的是 composited() = 底图叠上形状。
class PinWindow : public QWidget {
    Q_OBJECT
public:
    // showNow=false：建出来但不显示、不抢焦点 —— 给无界面自测用（--pinpick），
    // 免得为了测一下划词就在用户桌面上闪一张置顶窗。
    explicit PinWindow(const QPixmap& pm, const QString& key = QString(), bool showNow = true);

    // 自测用：同步识别这张图，然后回答"在图像坐标的这条带上拖动会选中什么字"。
    // 走的就是划词那套 cellsIn/pickedText，不是另写一份判定，所以能拿来当回归证据。
    QString pickProbe(const QRect& band);
    // 自测用（--pintool）：把私有几何探出来，验"收起工具栏/往期带之后图区尺寸一点没变"
    QRect probeImageRect() const { return imageRect(); }
    int probeThumbAt(const QPoint& pos) const { return thumbIndexAt(pos); }
    // 删掉往期里第 index 张（连带盘上那个文件）。返回删完之后"这张窗现在显示的是哪张"的 key：
    // 删的不是当前那张就还是原来的 key，删的是当前这张就自动挪到相邻那张，往期空了就返回空。
    QString deleteThumb(int index);
    QRect toWidgetRect(const QRect& imageR) const;   // 图像坐标 → 窗口坐标（画高亮/送事件都用它）
    // 工具栏 / 左边往期带 的显示切换。图上右键菜单和工具栏「历史」按钮走的就是这两个，
    // 自测（--pintool）也直接调它们，所以放公开。
    void setBarHidden(bool hide);
    void setStripHidden(bool hide);
    void setPinned(bool on);
    bool isPinned() const { return m_pinned; }
    void applyHug();   // 工具栏收起时把窗口收到"抱住图"（两个开关共用）
    bool barHidden() const { return m_barHidden; }
    bool stripHidden() const { return m_stripHidden; }
    // 置顶窗两处右键菜单都从这颗拿（深色样式挂在里面，见 .cpp 那颗函数的注释）。
    // 公开是给自测的：真弹一下要占屏幕，判据只能量"建好的那一份"。调用方负责删。
    QMenu* makeStyledMenu();
    int barH() const;      // 工具栏现在占多高（收起来就是 0）
    int stripW() const { return m_stripW; }   // 往期带现在占多宽（收起来就是 0）

private:

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void changeEvent(QEvent*) override;      // 失活时自动让位
    void resizeEvent(QResizeEvent*) override;

private:
    // 工具栏带 34px + 底部 8px 留给窗口自己当缩放手柄（工具栏是子控件，会吃掉那一条的按下）
    enum { kToolbarH = 42, kGrab = 8, kStripW = 76 };

    // fitWindow=false：只换图、不动窗口尺寸（切上一张/下一张时手动缩放过的窗口不该被重置）
    void setPixmap(const QPixmap& pm, bool resetUndo = true, bool fitWindow = true);
    int thumbIndexAt(const QPoint& pos) const;
    QRect imageRect() const;
    double viewScale() const;
    QPoint toImage(const QPoint& widgetPos) const;
    bool inImage(const QPoint& widgetPos) const;
    void beginStroke();               // 真正落第一笔时才压撤销栈
    void rebuildFlat();               // 底图叠上形状 → m_flat（重画只在形状变了时做）
    void syncResult(bool write = true);   // m_flat 写回历史那一条，内存里留住底图+形状
    void finishShape(const QPoint& upTo);
    void smudgeMosaic(const QPoint& imgPos);
    void applyCrop(const QRect& r);
    void undo();
    // 形状的选中/拖动/缩放（几何算在 Anno 里，和截图浮层同一套）
    int handleAt(const QPoint& widgetPos) const;     // 选中形状的手柄编号，没命中返回 kNoDrag
    QVector<QRect> widgetShapeHandles() const;       // 画选择框用的手柄（窗口坐标）
    void beginShapeDrag(int what, const QPoint& imgPos);
    void applyShapeDrag(const QPoint& imgPos);
    void deleteSelected();
    // 图上划词：识别出来的每个字都是一个格子，拖动选一片，松手就地复制
    void togglePick();                                // 「提取文字」按钮：开/关这个模式
    void runOcr();                                    // 后台识别这张图，回来填格子
    void fillCells(const QVector<Ocr::Line>& lines);  // 识别结果 → 一格一字
    QVector<int> cellsIn(const QRect& band) const;    // 带内的格子下标，按阅读顺序
    QString pickedText() const;
    void copyPicked(bool all = false);
    void invalidatePick(const char* why = "图变了");    // 图一变（裁剪/涂抹/撤销/换图）格子就作废
    void buildToolbar();
    void showContextMenu(const QPoint& globalPos);   // 右键图上：复制 / 另存为 / 识别全文 / 关闭
    void thumbContextMenu(int index, const QPoint& globalPos);   // 右键左侧缩略图：删除这一张
    void placeToolbar();
    void refreshUndoBtn();
    void buildNav();
    void placeNav();
    void stepHistory(int delta);      // 上一张 / 下一张
    void applyTopmost(bool on);
    Qt::Edges edgeAt(const QPoint& pos) const;

    using Tool = Anno::Tool;
    using Shape = Anno::Shape;
    using enum Anno::Tool;

    enum { kNoDrag = -1 };

    struct Snap {
        QPixmap pm;
        QVector<Shape> shapes;
    };
    struct Edit {
        QPixmap base;                 // 不含形状的那张图
        QVector<Shape> shapes;
    };

    QPixmap m_pm;                     // 底图：原图 + 涂抹/文字/裁剪，不含 m_shapes
    QPixmap m_flat;                   // 底图叠上形状后的样子（画出来/交出去的都是它）
    QString m_key;            // 当前图对应的历史条目
    QVector<Shape> m_shapes;          // 画出来的框/圆/箭头/打码块（可再编辑）
    QHash<QString, Edit> m_edits;     // 本次会话里每张图的底图+形状，切回来还能改
    int m_selShape = kNoDrag;
    int m_dragWhat = kNoDrag;
    Shape m_shapeSnap;
    QPoint m_shapePress;
    bool m_shapePushed = false;
    bool m_mosaicBrush = false;       // 这次马赛克是按住 Alt 的自由涂抹（烙进图，不可编辑）
    int m_stripW = kStripW;   // 往期带当前占的宽度（隐藏时是 0，命中判定自动跟着失效）
    bool m_barHidden = false;
    bool m_stripHidden = false;
    QSize m_sizeBeforeBar;     // 收起前的窗口尺寸，放出时原样还（避免被最小尺寸夹出误差）
    QSize m_sizeBeforeStrip;
    int m_minWBeforeBar = 0;   // 收起工具栏前的最小宽度（工具栏不显示了就不该再撑着的）
    int m_thumbH = 54;
    QPoint m_press;           // 拖窗口用
    bool m_dragging = false;
    Tool m_tool = None;
    bool m_editing = false;   // 正在图上拖一笔
    bool m_pushed = false;    // 这一笔是否已压进撤销栈
    QPoint m_pressImg;
    QPoint m_lastImg;
    QVector<Snap> m_undo;
    QWidget* m_toolbar = nullptr;
    QPushButton* m_undoBtn = nullptr;
    QPushButton* m_pinBtn = nullptr;
    QWidget* m_prevBtn = nullptr;
    QWidget* m_nextBtn = nullptr;
    bool m_pinned = true;         // 默认：一直置顶（钉在桌面）；工具栏/右键可切为自动让位
    Qt::Edges m_edge;             // 正在拖哪几条边缩放
    QRect m_geoAtPress;
    QPoint m_pressGlobal;

    // ---- 图上划词 ----
    bool m_pickOn = false;
    bool m_pickBusy = false;      // 正在后台识别（这几百毫秒里图上先写"正在识别"）
    bool m_pickDrag = false;
    QPoint m_pickPress;           // 拖动起点（图像坐标）
    QRect m_pickBand;             // 正在拖的带（图像坐标）
    QVector<Ocr::Cell> m_cells;   // 这张图认出来的每个字一格
    QVector<QRect> m_lineBoxes;   // 每行的包围盒（跨行拖动时整行选中用）
    QVector<int> m_picked;        // 选中格子的下标，按阅读顺序
    QString m_ocrAll;             // 整张图的识别结果（没选东西时 Enter 复制它）
    QString m_pickNote;           // 画在图角上的那句"已复制 N 字"
    QPushButton* m_pickBtn = nullptr;
    QPushButton* m_histBtn = nullptr;   // 工具栏上的「历史」：切左边那条往期带
};
