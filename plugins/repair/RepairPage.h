#pragma once

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QNetworkAccessManager;
class QNetworkReply;

// 配置 - 工具里的「DLL 修复」页：检测 VC++ 运行库 / DirectX / .NET 等，未安装的用 winget 安装（可一键全修）
class RepairPage : public QWidget {
    Q_OBJECT
public:
    explicit RepairPage(QWidget* parent = nullptr);
    ~RepairPage() override;

private slots:
    void rescan();
    void installRow(int row);
    void repairAll();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void clearRows();
    void installQueue(const QString& title, const QStringList& pkgIds);
    void installNext();
    void downloadAndRun(const QString& title, const QString& url);

    QVBoxLayout* m_rowsLay = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_allBtn = nullptr;

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_downloadReply;

    QVector<int> m_missing;    // 最近一次检测出的缺失行（kRows 下标）
    QStringList m_queue;       // 排队待装的 winget 包 ID
    QString m_queueTitle;
    int m_queueTotal = 0;
    bool m_installing = false; // 队列里正有一个在装（队列空了也可能还在装最后一个）
};
