#pragma once
#include "core-api.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// 快索引：常用目录（限定深度）+ 实时监听，新鲜度优先
// 深索引：全盘文件名，快照文件 mmap，内存不占私有页
struct FileEntry {
    QString name;
    QString path;
    bool isDir = false;
};

// 深索引的一条：路径整体存在 arena 字节池里，nameStart 起是文件名
struct FileDeepEntry {
    quint32 offset = 0;
    quint32 len = 0;
    quint32 nameStart = 0;
    quint8 isDir = 0;
    quint64 frn = 0;
};

class LTOOL_CORE_API FileIndexer : public QObject {
    Q_OBJECT
public:
    enum class DeepStatus { Ok, NoAccess, NeedRebuild };

    struct QuickRoot {
        QString dir;
        int depth = 0;
    };

    static FileIndexer& instance();

    int count() const;
    int deepCount() const;
    QVector<QuickRoot> quickRoots();

    // forceDeep=true 跳过增量，直接重扫全盘快照
    void reindexAsync(bool forceDeep = false);
    QVector<FileEntry> search(const QString& query, int limit) const;

signals:
    void indexingFinished(int total);

private:
    FileIndexer() = default;
    Q_DISABLE_COPY(FileIndexer)

    // 快照文件里的一个卷：数据留在映射视图内，只记偏移
    struct MappedVol {
        QString root;
        quint64 journalId = 0;
        quint64 nextUsn = 0;
        quint64 entryOff = 0;
        quint64 arenaOff = 0;
        quint32 count = 0;
        quint32 arenaLen = 0;
    };

    // 增量游标（usnstate.dat）
    struct VolRuntime {
        QString root;
        quint64 journalId = 0;
        quint64 nextUsn = 0;
    };

    bool mapSnapshot();
    void unmapSnapshot();
    void loadUsnState();
    void saveUsnState();
    void rebuildDeepAll();
    DeepStatus incrementalDeep();
    bool cacheTooOld() const;
    void removeDeltaByFrn(quint64 frn);
    void applyUsnDelete(quint64 frn);

    void startWatchers();
    void watcherLoop(void* ctx);   // 跑在 detach 出来的监听线程上（单例活到进程结束）
    void rebuildQuickSync();
    void watcherApplyAdd(const QString& path);
    void watcherApplyRemove(const QString& path);

    mutable QReadWriteLock m_lock;
    QVector<FileEntry> m_entries;

    QVector<MappedVol> m_base;
    QVector<VolRuntime> m_vols;
    QVector<FileDeepEntry> m_add;
    QByteArray m_addArena;
    QSet<quint64> m_removed;

    // mmap 三件套按 void* 存，头文件不引 windows.h
    void* m_mapFile = nullptr;
    void* m_mapObj = nullptr;
    void* m_mapView = nullptr;
    qint64 m_mapSize = 0;

    void* m_watchCtx = nullptr;
    QAtomicInt m_running {0};

    // 全量重建限流：非管理员机器上游标恒为 0、卷又开不了，每次重扫都会滑进
    // 一次 30 秒级的全盘遍历（2026-10-26 起的 CPU/磁盘风暴）。有快照时 1 小时内
    // 不重复重建，宁可保留旧快照。只在 worker 线程里读写（m_running 串行化）。
    QElapsedTimer m_lastFullRebuild;
    bool m_hadFullRebuild = false;
    QSet<QString> m_noAccessLogged;   // 每会话每卷只记一次“开不了卷”，防日志刷屏
};
