#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class Recorder;
class WindowPreview;

// F2 录屏的界面那一层：
//  WindowPicker = 列出"整个屏幕"+ 桌面上看得见的窗口，选一个要录的（不弹就不知道该录谁）
//  RecordBar    = 录制中挂在屏幕上的小条：红点 + 计时 + 帧数 + 停止
// 二者都只是 Recorder 的壳，编码/取帧全在 Recorder 里。
//
// 2026-10-15：这一层（连同 Recorder / RecProbe）从 ltoolcore.dll 搬进了 plugins/record，
// 只有那个插件用它们，主程序只认动作名 "record"。原来 WindowPicker 上的 LTOOL_CORE_API
// 是为了让 src/main.cpp 能点名这个类型才贴的，现在跨不到边界了，宏和 core-api.h 一起删。
namespace RecordUi {

// F2 的入口：弹选择列表，选完就开录（已经在录时什么都不做）
void startViaPicker();
bool isRecording();

// 给插件那张状态页说实话用的两句：当前配置的画质档 / 画面档
//（就是选择窗里那两颗下拉框存进 config.ini 的值，档位表只有一份 —— 见 RecordUi.cpp）
QString qualityText();
QString maxWidthText();

} // namespace RecordUi

class WindowPicker : public QWidget {
    Q_OBJECT
public:
    explicit WindowPicker(QWidget* parent = nullptr);

    // 预览那一块（判据要读它拿没拿到帧、缩成了多大）；界面只管摆图和说一句实话
    WindowPreview* preview() const { return m_pv; }
    QPushButton* startButton() const { return m_rec; }

signals:
    // monitor=true 时 target 是 HMONITOR（录整块屏幕），否则是 HWND（录那一个窗口）
    void picked(WId target, const QString& title, bool monitor);

protected:
    // 这个窗没有标题栏：Esc、点到别处、再按一次 F2 都得能收掉它，否则按了 F2
    // 就只能硬着头皮选一个开始录（提示语一直写着"Esc 取消"，以前没实现）。
    // 右上角另外有个「✕」，那是给人眼看的落点。
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;
    // 没标题栏 = 没有地方能拖着它走。按住标题那一行（和四周空白）就能挪。
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    // 收起来就把取帧那一路放掉：压在别人窗口上面的实时抓取不能空转
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void refreshPreview();            // 按当前选中那条 + 当前「画面」档重开预览
    void chooseCurrent();             // 「开始录制」那颗、和回车，走的都是这一条

    QListWidget* m_list = nullptr;
    QPushButton* m_close = nullptr;
    QPushButton* m_rec = nullptr;
    QLabel* m_shot = nullptr;         // 预览那张图
    QLabel* m_cap = nullptr;          // 图下面那行字（多大 / 为什么没图）
    WindowPreview* m_pv = nullptr;
    QPoint m_press;
    bool m_dragging = false;
};

class RecordBar : public QWidget {
    Q_OBJECT
public:
    RecordBar(Recorder* rec, const QString& path, QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void poll();
    void finish(const QString& error);

    Recorder* m_rec;
    QString m_path;
    QLabel* m_text = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_cancel = nullptr;
    QPoint m_press;
    bool m_dragging = false;
    bool m_finished = false;
    bool m_discard = false;      // 「不要了」按下去：收尾之后把文件删掉，别留一个废 MP4
};
