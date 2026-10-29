#include "FileIndexer.h"
#include "platform/Paths.h"
#include "UsnIndex.h"
#include "Utils.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>
#include <winioctl.h>

namespace {

// 快索引 + 深索引共用的跳过名单（目录名，忽略大小写）
const QStringList& skipDirNames()
{
    static const QStringList names = {
        "AppData", "Application Data", "$Recycle.Bin", "System Volume Information",
        ".git", ".svn", ".vs", ".idea", "node_modules", "__pycache__",
        ".venv", "venv", "site-packages", ".cargo", ".rustup", ".gradle", ".m2",
        "WinSxS", "WindowsApps", "Installer", "servicing", "WinRE", "Panther",
        "target", "CMakeFiles",
    };
    return names;
}

bool shouldSkip(const QString& name)
{
    for (const QString& s : skipDirNames())
        if (name.compare(s, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

// ASCII 大小写无关查找（UTF-8 多字节序列按原字节精确比较，中文不受影响）
int indexOfCI(const char* hay, int hayLen, const char* needle, int needleLen)
{
    if (needleLen == 0)
        return 0;
    if (needleLen > hayLen)
        return -1;
    for (int i = 0; i + needleLen <= hayLen; ++i) {
        int j = 0;
        for (; j < needleLen; ++j) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == needleLen)
            return i;
    }
    return -1;
}

void walk(const QString& dir, int depth, QVector<FileEntry>* out, int cap)
{
    if (depth < 0 || out->size() >= cap)
        return;
    QDir d(dir);
    const auto list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& fi : list) {
        const QString fn = fi.fileName();
        if (shouldSkip(fn))
            continue;
        if (out->size() >= cap)
            return;
        out->append({fn, fi.absoluteFilePath(), fi.isDir()});
        if (fi.isDir())
            walk(fi.absoluteFilePath(), depth - 1, out, cap);
    }
}

// 目录遍历全量构建一个卷的深索引（无管理员权限时的回退；frn 不可用）
// 递归写成普通函数而不是 std::function + [&] 捕获：开机那次崩溃（0xc0000005）就落在
// 这个闭包的捕获槽上（见 CrashDumps/ltool.exe.12504.dmp，栈里 15 层同一个 lambda 帧），
// 按引用捕获的 this 指针被写坏后没有任何编译期约束；普通函数的参数走寄存器，也不吃
// std::function 的类型擦除开销。
void walkDeep(const QString& dir, int depth, QVector<FileDeepEntry>* entries, QByteArray* arena)
{
    if (depth < 0 || entries->size() >= 5000000)
        return;
    QDir d(dir);
    const auto list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& fi : list) {
        const QString fn = fi.fileName();
        if (shouldSkip(fn))
            continue;
        if (entries->size() >= 5000000)
            return;
        const QString path = QDir::fromNativeSeparators(fi.absoluteFilePath());
        FileDeepEntry e;
        e.offset = quint32(arena->size());
        arena->append(path.toUtf8());
        e.len = quint32(arena->size() - e.offset);
        e.nameStart = e.len - quint32(fn.toUtf8().size());
        e.isDir = fi.isDir() ? 1 : 0;
        entries->append(e);
        if (fi.isDir())
            walkDeep(path, depth - 1, entries, arena);
    }
}

void walkDeepVolume(const QString& root, QVector<FileDeepEntry>* entries, QByteArray* arena)
{
    walkDeep(root, 32, entries, arena);
}

// 所有固定磁盘根（不含光驱/网络盘）
QStringList fixedDriveRoots()
{
    QStringList roots;
    for (const QStorageInfo& si : QStorageInfo::mountedVolumes()) {
        if (!si.isReady() || si.device().isEmpty())
            continue;
        const QString root = si.rootPath();
        if (root.size() < 2 || root[1] != ':')
            continue;
        const UINT type = GetDriveTypeW((LPCWSTR)QDir::toNativeSeparators(root).utf16());
        if (type == DRIVE_FIXED)
            roots.append(root);
    }
    return roots;
}

// 映射视图里的一条打包条目（21 字节，与 deepindex.dat 的存储布局一致）
#pragma pack(push, 1)
struct PackedDeepEntry {
    quint32 offset;
    quint32 len;
    quint32 nameStart;
    quint8 isDir;
    quint64 frn;
};
#pragma pack(pop)

constexpr int kPackedEntrySize = 21;

// ---------- 实时监听（快索引） ----------

constexpr DWORD kQuickWatchBuffer = 64 * 1024;

struct WatchRoot {
    QString root;           // 原生分隔符
    HANDLE dir = nullptr;
    HANDLE event = nullptr;
    OVERLAPPED ov {};
    QByteArray buffer;
};

struct WatchCtx {
    HANDLE stopEvent = nullptr;
    QVector<WatchRoot> roots;
};

bool issueWatch(WatchRoot& r)
{
    ResetEvent(r.event);
    ZeroMemory(&r.ov, sizeof(r.ov));
    r.ov.hEvent = r.event;
    DWORD bytes = 0;
    return ReadDirectoryChangesW(r.dir, r.buffer.data(), DWORD(r.buffer.size()), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                 &bytes, &r.ov, nullptr) != 0;
}

// 路径是否属于可索引范围（不含被跳过的组件、不含 $ 前缀组件）
bool pathIndexable(const QString& path)
{
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        if (p.compare("C:", Qt::CaseInsensitive) == 0
            || (p.size() == 2 && p[1] == ':'))
            continue;
        if (shouldSkip(p) || p.startsWith('$'))
            return false;
    }
    return true;
}

} // namespace

FileIndexer& FileIndexer::instance()
{
    static FileIndexer inst;
    return inst;
}

int FileIndexer::count() const
{
    QReadLocker lock(&m_lock);
    return m_entries.size();
}

int FileIndexer::deepCount() const
{
    QReadLocker lock(&m_lock);
    int n = 0;
    for (const MappedVol& v : m_base)
        n += int(v.count);
    n -= m_removed.size();
    n += m_add.size();
    return qMax(0, n);
}

QVector<FileIndexer::QuickRoot> FileIndexer::quickRoots()
{
    QVector<QuickRoot> roots;
    const auto add = [&roots](const QString& dir, int depth) {
        if (!dir.isEmpty())
            roots.append({dir, depth});
    };
    add(Platform::knownDir(Platform::KnownDir::UserDesktop), 4);
    add(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), 3);
    add(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), 3);
    add(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), 2);
    add(QStandardPaths::writableLocation(QStandardPaths::MusicLocation), 2);
    add(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), 2);
    add(QStandardPaths::writableLocation(QStandardPaths::HomeLocation), 1);
    return roots;
}

void FileIndexer::reindexAsync(bool forceDeep)
{
    if (!m_running.testAndSetRelaxed(0, 1))
        return;

    auto future = QtConcurrent::run([this, forceDeep]() -> int {
        QElapsedTimer timer;
        timer.start();

        QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        // 裸 --smoke（无沙箱）时用环境开关跳过深扫，不落盘改用户配置（见 main.cpp runSmokeTest）
        const bool deepEnabled = qEnvironmentVariableIsSet("LTOOL_SMOKE_SKIP_DEEP")
            ? false : cfg.value("index/deep", true).toBool();

        // 1) 深索引：映射磁盘上的快照（不占私有内存），启动立即可搜
        // （mapSnapshot 自身不持锁，调用方负责 —— 搜索线程此刻可能已在读旧映射）
        bool haveCache = false;
        if (deepEnabled) {
            QWriteLocker lock(&m_lock);
            haveCache = mapSnapshot();
        }

        // 2) 快索引全量
        QVector<FileEntry> quick;
        quick.reserve(8192);
        for (const QuickRoot& r : quickRoots())
            walk(r.dir, r.depth, &quick, 200000);
        {
            QWriteLocker lock(&m_lock);
            m_entries = quick;
        }

        // 3) 启动常用目录实时监听（新增/删除秒级反映）
        startWatchers();

        // 4) 深索引更新：USN 增量优先（管理员 + 有状态），否则全量重建
        if (deepEnabled) {
            const DeepStatus st = (haveCache && !forceDeep)
                ? incrementalDeep() : DeepStatus::NeedRebuild;
            const bool rateLimited = haveCache && !forceDeep
                && m_hadFullRebuild && m_lastFullRebuild.elapsed() < 3600 * 1000;
            if (st == DeepStatus::Ok) {
                Utils::log(QString("deep index incremental ok in %1ms (quick=%2 deep=%3)")
                               .arg(timer.elapsed()).arg(m_entries.size()).arg(deepCount()));
            } else if (st == DeepStatus::NoAccess && !forceDeep) {
                // 非管理员没有刷新深索引的手段：快照再旧也留着——旧快照好过
                // 每次重扫都来一场全盘遍历（cacheTooOld 不再否决这条退路）
                Utils::log(QString("deep index: no admin rights, keep snapshot "
                                   "(quick=%1 deep=%2)").arg(m_entries.size()).arg(deepCount()));
            } else if (rateLimited) {
                Utils::log("deep index: full rebuild rate-limited (<1h), keep snapshot");
            } else {
                QElapsedTimer t2;
                t2.start();
                rebuildDeepAll();
                m_lastFullRebuild.start();
                m_hadFullRebuild = true;
                Utils::log(QString("deep index full rebuild in %1s (quick=%2 deep=%3)")
                               .arg(t2.elapsed() / 1000).arg(m_entries.size()).arg(deepCount()));
            }
        }

        int total = 0;
        {
            QReadLocker lock(&m_lock);
            total = m_entries.size();
            for (const MappedVol& v : m_base)
                total += int(v.count);
            total += m_add.size();
            total -= qMin<qint64>(m_removed.size(), total > 0 ? total : 0);
        }
        return total;
    });

    auto* watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
        m_running.storeRelaxed(0);
        emit indexingFinished(watcher->result());
        watcher->deleteLater();

        // 定时刷新：增量路径毫秒级；无管理员权限时为一次后台全量重扫
        QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        const int hours = cfg.value("index/refreshHours", 6).toInt();
        if (hours > 0) {
            QTimer::singleShot(int(hours) * 3600 * 1000, this, [this] {
                if (m_running.loadRelaxed() == 0)
                    reindexAsync();
            });
        }
    });
    watcher->setFuture(future);
}

// ---------- 深索引：映射快照 ----------

void FileIndexer::unmapSnapshot()
{
    if (m_mapView)
        UnmapViewOfFile(m_mapView);
    if (m_mapObj)
        CloseHandle(m_mapObj);
    if (m_mapFile && m_mapFile != INVALID_HANDLE_VALUE)
        CloseHandle((HANDLE)m_mapFile);
    m_mapView = nullptr;
    m_mapObj = nullptr;
    m_mapFile = nullptr;
    m_mapSize = 0;
    m_base.clear();
}

bool FileIndexer::mapSnapshot()
{
    unmapSnapshot();
    const QString path = Utils::dataDir() + "/deepindex.dat";
    HANDLE file = CreateFileW((LPCWSTR)QDir::toNativeSeparators(path).utf16(),
                              GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 9) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapObj = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapObj) {
        CloseHandle(file);
        return false;
    }
    void* view = MapViewOfFile(mapObj, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapObj);
        CloseHandle(file);
        return false;
    }

    // 解析卷头（只读头信息，数据本体留在映射视图里）
    const char* p = (const char*)view;
    const char* end = (const char*)view + sz.QuadPart;
    auto readU32 = [&](quint32* out) {
        if (p + 4 > end) return false;
        memcpy(out, p, 4); p += 4; return true;
    };
    auto readU64 = [&](quint64* out) {
        if (p + 8 > end) return false;
        memcpy(out, p, 8); p += 8; return true;
    };

    QVector<MappedVol> base;
    quint32 volCount = 0;
    bool ok = memcmp(p, "LTDI2", 5) == 0;
    if (ok) {
        p += 5;
        ok = readU32(&volCount) && volCount <= 64;
    }
    if (ok) {
        for (quint32 vi = 0; vi < volCount && ok; ++vi) {
            quint32 rootLen = 0;
            if (!readU32(&rootLen) || p + rootLen > end) { ok = false; break; }
            MappedVol v;
            v.root = QString::fromUtf8(p, int(rootLen));
            p += rootLen;
            if (!readU64(&v.journalId) || !readU64(&v.nextUsn)) { ok = false; break; }
            quint32 count = 0, arenaLen = 0;
            if (!readU32(&count) || !readU32(&arenaLen) || count > 20000000u) { ok = false; break; }
            const qint64 need = qint64(count) * kPackedEntrySize + arenaLen;
            if (p + need > end) { ok = false; break; }
            v.entryOff = quint64(p - (const char*)view);
            v.arenaOff = v.entryOff + quint64(count) * kPackedEntrySize;
            v.count = count;
            v.arenaLen = arenaLen;
            p += need;
            base.append(v);
        }
    }
    if (!ok) {
        UnmapViewOfFile(view);
        CloseHandle(mapObj);
        CloseHandle(file);
        return false;
    }

    // 调用方必须已持 m_lock 写锁（rebuildDeepAll 的重映射块 / reindexAsync 的首映射块）——
    // 这里再拿一次就是同一把非递归 QReadWriteLock 的嵌套写锁，当场死锁
    //（2026-10-25 --storeexit 挂死就是它：CI 那趟 v1.5.1 也是这条红）。
    m_mapFile = file;
    m_mapObj = mapObj;
    m_mapView = view;
    m_mapSize = sz.QuadPart;
    m_base = base;
    loadUsnState();
    return true;
}


// ---------- 深索引：全量重建 ----------

void FileIndexer::loadUsnState()
{
    QFile f(Utils::dataDir() + "/usnstate.dat");
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray all = f.readAll();
    f.close();
    if (all.size() < 9 || memcmp(all.constData(), "LTSU1", 5) != 0)
        return;
    const char* p = all.constData() + 5;
    const char* end = all.constData() + all.size();
    auto readU32 = [&](quint32* out) {
        if (p + 4 > end) return false;
        memcpy(out, p, 4); p += 4; return true;
    };
    auto readU64 = [&](quint64* out) {
        if (p + 8 > end) return false;
        memcpy(out, p, 8); p += 8; return true;
    };
    quint32 volCount = 0;
    if (!readU32(&volCount) || volCount > 64)
        return;
    for (quint32 i = 0; i < volCount; ++i) {
        quint32 rootLen = 0;
        if (!readU32(&rootLen) || p + rootLen > end)
            return;
        const QString root = QString::fromUtf8(p, int(rootLen));
        p += rootLen;
        quint64 jid = 0, usn = 0;
        if (!readU64(&jid) || !readU64(&usn))
            return;
        for (VolRuntime& v : m_vols) {
            if (v.root.compare(root, Qt::CaseInsensitive) == 0) {
                v.journalId = jid;
                v.nextUsn = usn;
                break;
            }
        }
    }
}

void FileIndexer::saveUsnState()
{
    QSaveFile f(Utils::dataDir() + "/usnstate.dat");
    if (!f.open(QIODevice::WriteOnly))
        return;
    QByteArray out;
    out.append("LTSU1", 5);
    const auto put32 = [&out](quint32 x) { char b[4]; memcpy(b, &x, 4); out.append(b, 4); };
    const auto put64 = [&out](quint64 x) { char b[8]; memcpy(b, &x, 8); out.append(b, 8); };
    put32(quint32(m_vols.size()));
    for (const VolRuntime& v : m_vols) {
        const QByteArray rootUtf8 = v.root.toUtf8();
        put32(quint32(rootUtf8.size()));
        out.append(rootUtf8);
        put64(v.journalId);
        put64(v.nextUsn);
    }
    f.write(out);
    f.commit();
}

void FileIndexer::rebuildDeepAll()
{
    const QStringList roots = fixedDriveRoots();

    // 构建临时卷数据（MFT 优先，失败回退目录遍历）
    struct FreshVol {
        QString root;
        QVector<FileDeepEntry> entries;
        QByteArray arena;
        quint64 journalId = 0, nextUsn = 0;
    };
    QVector<FreshVol> fresh;
    fresh.reserve(roots.size());

    for (const QString& root : roots) {
        FreshVol v;
        v.root = root;

        UsnIndex::VolumeHandle vol;
        UsnIndex::EnumResult er;
        bool mftOk = false;
        if (vol.open(root)) {
            mftOk = UsnIndex::enumVolumeMft(vol.vol, er);
            if (!mftOk)
                Utils::log(QString("MFT enum failed for %1 err=%2").arg(root).arg(GetLastError()));
            vol.close();
        } else {
            Utils::log(QString("volume open failed for %1 err=%2 (回退目录遍历)")
                           .arg(root).arg(GetLastError()));
        }

        if (mftOk) {
            // 由 FRN 父链组合完整路径（memoized）
            struct PathInfo { int off = -1; int len = 0; int nameLen8 = 0; bool emitFlag = false; bool done = false; };
            const int n = er.frn.size();
            QVector<PathInfo> info(n);
            QHash<quint64, int> byFrn;
            byFrn.reserve(n * 2);
            for (int i = 0; i < n; ++i)
                byFrn.insert(er.frn[i], i);

            std::function<void(int)> compose = [&](int i) {
                if (info[i].done)
                    return;
                info[i].done = true;
                const QString name = QString::fromUtf16(
                    reinterpret_cast<const char16_t*>(er.names.constData() + er.nameOff[i]),
                    int(er.nameLen[i]) / 2);
                QString parentPath;
                bool parentEmit = true;
                if (er.frn[i] == 5) { // 卷根
                    parentPath = v.root;
                    parentEmit = false;
                } else if (er.parent[i] == 5) {
                    parentPath = v.root;
                } else {
                    const int p = byFrn.value(er.parent[i], -1);
                    if (p >= 0 && p != i) {
                        compose(p);
                        parentPath = QString::fromUtf8(v.arena.constData() + info[p].off, info[p].len);
                        parentEmit = info[p].emitFlag;
                    } else {
                        parentPath = v.root; // 孤儿记录挂根
                    }
                }
                const QString full = parentPath.endsWith('/')
                    ? parentPath + name : parentPath + "/" + name;
                const QByteArray utf8 = full.toUtf8();
                info[i].off = v.arena.size();
                v.arena.append(utf8);
                info[i].len = utf8.size();
                info[i].nameLen8 = int(name.toUtf8().size());
                info[i].emitFlag = parentEmit && er.frn[i] != 5 && name != "."
                                   && !name.startsWith('$') && !shouldSkip(name);
            };

            for (int i = 0; i < n; ++i) {
                compose(i);
                if (!info[i].emitFlag)
                    continue;
                FileDeepEntry e;
                e.offset = quint32(info[i].off);
                e.len = quint32(info[i].len);
                e.nameStart = e.len - quint32(info[i].nameLen8);
                e.isDir = (er.attrs[i] & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 1 : 0;
                e.frn = er.frn[i];
                v.entries.append(e);
            }
            v.journalId = er.journal.journalId;
            v.nextUsn = er.journal.nextUsn;
        } else {
            // 无管理员权限 / 非 NTFS：目录遍历回退
            walkDeepVolume(root, &v.entries, &v.arena);
        }
        v.arena.squeeze();
        v.entries.squeeze();
        fresh.append(v);
    }

    // 写文件（同一 LTDI2 布局），随后映射文件并释放内存副本
    QSaveFile f(Utils::dataDir() + "/deepindex.dat");
    if (!f.open(QIODevice::WriteOnly)) {
        Utils::log("deep index: cannot write snapshot file");
        return;
    }
    QByteArray out;
    out.append("LTDI2", 5);
    const auto put32 = [&out](quint32 x) { char b[4]; memcpy(b, &x, 4); out.append(b, 4); };
    const auto put64 = [&out](quint64 x) { char b[8]; memcpy(b, &x, 8); out.append(b, 8); };
    put32(quint32(fresh.size()));
    for (const FreshVol& v : fresh) {
        const QByteArray rootUtf8 = v.root.toUtf8();
        put32(quint32(rootUtf8.size()));
        out.append(rootUtf8);
        put64(v.journalId);
        put64(v.nextUsn);
        put32(quint32(v.entries.size()));
        put32(quint32(v.arena.size()));
        QByteArray block;
        block.reserve(v.entries.size() * kPackedEntrySize);
        for (const FileDeepEntry& e : v.entries) {
            char b[kPackedEntrySize];
            memcpy(b, &e.offset, 4);
            memcpy(b + 4, &e.len, 4);
            memcpy(b + 8, &e.nameStart, 4);
            b[12] = e.isDir;
            memcpy(b + 13, &e.frn, 8);
            block.append(b, kPackedEntrySize);
        }
        out.append(block);
        out.append(v.arena);
    }
    f.write(out);
    // 提交前先解除旧快照映射：旧文件还挂着本进程的映射句柄（CreateFileW
    // 没给 FILE_SHARE_DELETE），rename-over 必败 —— deepindex.dat 停在
    // 10-03 不动、每次重建都白扫，就是这一行顺序造成的。
    {
        QWriteLocker lock(&m_lock);
        unmapSnapshot();
    }
    if (!f.commit())
        Utils::log(QString("deep index: snapshot commit failed: %1").arg(f.errorString()));

    // 重映射文件并重置增量（写锁保护：搜索线程可能正遍历旧映射视图）
    {
        QWriteLocker lock(&m_lock);
        m_add.clear();
        m_addArena.clear();
        m_removed.clear();
        if (!mapSnapshot())
            Utils::log("deep index: snapshot written but remap failed");
        // 重建后的游标以 fresh 为准。必须放在 mapSnapshot 之后：
        // 它内部的 loadUsnState 会拿 usnstate.dat 的旧值（可能是 0）合并进
        // m_vols，盖掉的话管理员机器每轮重扫都会因“游标为 0”再全量重建
        m_vols.clear();
        for (const FreshVol& v : fresh)
            m_vols.append({v.root, v.journalId, v.nextUsn});
    }
    saveUsnState();
}

// ---------- 深索引：USN 增量 ----------

// 用 OpenFileById 拿一个文件引用号对应的完整路径（用于组合新增文件的路径）
QString parentPathById(HANDLE vol, quint64 parentFrn)
{
    FILE_ID_DESCRIPTOR fid;
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    fid.FileId.QuadPart = LONGLONG(parentFrn);
    HANDLE ph = OpenFileById(vol, &fid, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, 0);
    if (ph == INVALID_HANDLE_VALUE || !ph)
        return {};
    WCHAR buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(ph, buf, 4096, FILE_NAME_NORMALIZED);
    CloseHandle(ph);
    if (n == 0)
        return {};
    QString path = QString::fromWCharArray(buf, int(n));
    path.remove("\\\\?\\");
    return QDir::fromNativeSeparators(path);
}

FileIndexer::DeepStatus FileIndexer::incrementalDeep()
{
    if (m_base.isEmpty())
        return DeepStatus::NeedRebuild;
    DeepStatus status = DeepStatus::Ok;
    int changed = 0;

    for (const MappedVol& mv : m_base) {
        // 找游标
        VolRuntime* cur = nullptr;
        for (VolRuntime& v : m_vols)
            if (v.root.compare(mv.root, Qt::CaseInsensitive) == 0)
                cur = &v;
        if (!cur) {
            m_vols.append({mv.root, mv.journalId, mv.nextUsn});
            cur = &m_vols.last();
        }
        if (cur->journalId == 0 || cur->nextUsn == 0) {
            // usnstate 里的游标丢了（比如被无管理员会话的重建写成 0）：快照头若还
            // 记着有效游标就就地恢复——快照本身就是按它构建的，从它增量即可，
            // 不必重建。两头都是 0 才探卷：开不了（非管理员）→ NoAccess 保留快照；
            // 开得开（管理员）→ 走一次全量重建把游标修好，此后就能增量。
            if (mv.journalId != 0 && mv.nextUsn != 0) {
                cur->journalId = mv.journalId;
                cur->nextUsn = mv.nextUsn;
            } else {
                UsnIndex::VolumeHandle probe;
                if (!probe.open(mv.root)) {
                    if (!m_noAccessLogged.contains(mv.root)) {
                        m_noAccessLogged.insert(mv.root);
                        Utils::log(QString("usn incremental skip %1: open failed err=%2 (需要管理员权限)")
                                       .arg(mv.root).arg(GetLastError()));
                    }
                    status = DeepStatus::NoAccess;
                } else {
                    status = DeepStatus::NeedRebuild;
                }
                continue;
            }
        }
        UsnIndex::VolumeHandle vol;
        if (!vol.open(mv.root)) {
            if (!m_noAccessLogged.contains(mv.root)) {
                m_noAccessLogged.insert(mv.root);
                Utils::log(QString("usn incremental skip %1: open failed err=%2 (需要管理员权限)")
                               .arg(mv.root).arg(GetLastError()));
            }
            status = DeepStatus::NoAccess;
            continue;
        }
        const UsnIndex::JournalInfo j = UsnIndex::queryJournal(vol.vol);
        if (!j.ok || j.journalId != cur->journalId) {
            Utils::log(QString("usn incremental skip %1: journal mismatch").arg(mv.root));
            status = DeepStatus::NeedRebuild;
            continue;
        }
        QVector<UsnIndex::RawChange> changes;
        if (!UsnIndex::readChanges(vol.vol, cur->journalId, cur->nextUsn, j.nextUsn, changes)) {
            status = DeepStatus::NeedRebuild;
            continue;
        }
        if (changes.isEmpty()) {
            cur->nextUsn = j.nextUsn;
            continue;
        }

        const HANDLE volHandle = vol.vol;
        // 两段式：先无锁收集（含磁盘 IO），再短临界区统一提交，
        // 避免与搜索线程（读锁遍历 m_add/m_addArena）产生数据竞争
        struct PendingAdd { FileDeepEntry e; QByteArray path; };
        QVector<PendingAdd> pendingAdds;
        QVector<quint64> pendingDels;

        for (const UsnIndex::RawChange& c : changes) {
            const bool isDir = (c.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (c.reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) {
                if (isDir && (c.reason & USN_REASON_RENAME_NEW_NAME))
                    return DeepStatus::NeedRebuild; // 目录改名 → 整棵子树路径失效（尚无共享变更，安全）
                if (shouldSkip(c.name) || c.name.startsWith('$') || c.name == ".")
                    continue;
                // 父目录路径：优先用 OpenFileById（不依赖任何内存索引）
                QString parentPath;
                if (c.parentFrn == 5) {
                    parentPath = mv.root;
                } else {
                    parentPath = parentPathById(volHandle, c.parentFrn);
                    if (parentPath.isEmpty()) {
                        // 父目录已不在（瞬间删除等）
                        continue;
                    }
                }
                if (!pathIndexable(parentPath))
                    continue;
                const QString full = parentPath.endsWith('/')
                    ? parentPath + c.name : parentPath + "/" + c.name;
                FileDeepEntry e;
                const QByteArray u8 = full.toUtf8();
                e.offset = 0; // 提交时在增量 arena 里定位
                e.len = quint32(u8.size());
                e.nameStart = e.len - quint32(c.name.toUtf8().size());
                e.isDir = isDir ? 1 : 0;
                e.frn = c.frn;
                pendingAdds.append({e, u8});
            }
            if (c.reason & (USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME)) {
                pendingDels.append(c.frn);
            }
        }

        if (!pendingAdds.isEmpty() || !pendingDels.isEmpty()) {
            QWriteLocker lock(&m_lock);
            for (const PendingAdd& pa : pendingAdds) {
                FileDeepEntry e = pa.e;
                e.offset = quint32(m_addArena.size());
                m_addArena.append(pa.path);
                m_add.append(e);
            }
            for (quint64 frn : pendingDels) {
                // 删除 = 墓碑（基础快照按 frn 跳过）；增量新增的条目直接移除
                for (int i = m_add.size() - 1; i >= 0; --i) {
                    if (m_add[i].frn == frn) {
                        m_add.removeAt(i);
                        break;
                    }
                }
                m_removed.insert(frn);
            }
            changed += int(pendingAdds.size() + pendingDels.size());
        }
        cur->nextUsn = j.nextUsn;
        Q_UNUSED(changed);
    }
    saveUsnState();
    if (m_add.size() + m_removed.size() > 30000) {
        Utils::log("delta too large, triggering full rebuild");
        return DeepStatus::NeedRebuild;
    }
    return status;
}

bool FileIndexer::cacheTooOld() const
{
    QFileInfo f(Utils::dataDir() + "/deepindex.dat");
    if (!f.exists())
        return true;
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const int hours = cfg.value("index/refreshHours", 6).toInt();
    if (hours <= 0)
        return false;
    return f.lastModified().msecsTo(QDateTime::currentDateTime()) > qint64(hours) * 3600 * 1000;
}

void FileIndexer::removeDeltaByFrn(quint64 frn)
{
    for (int i = m_add.size() - 1; i >= 0; --i) {
        if (m_add[i].frn == frn) {
            m_add.removeAt(i);
            return;
        }
    }
}

void FileIndexer::applyUsnDelete(quint64 frn)
{
    m_removed.insert(frn);
    removeDeltaByFrn(frn);
}

// ---------- 搜索 ----------
//
// 老写法是"整表扫 + 每个命中都 fromUtf8 造一条整路径 QString + 收完再排序去重"：
// 272 万条下一次查询 540~680ms，而绝大多数命中根本活不到前 20 名（输 "no" 时几百万
// 个命中，每个都白造一个 QString）。这里改四件事，**输出顺序和老写法逐条一致**
// —— 判据是 main.cpp 的 --searchdump：改前改后各跑一遍，diff 必须为空。
//   ① 有界分桶：排序键是 (来源, 分数, 名字长度, 路径)，来源×分数一共 8 个桶，每桶只留
//      最好的 2×limit 条。为什么 2×limit 够：去重最多再吃掉 limit 条（同一个路径会同时
//      出现在快索引和深索引里，快索引排前面，深索引那份被丢），所以真正要发出去的
//      深索引条目一定在深索引前 2×limit 名里；而"全局某桶前 K 名"必然落在"各段同桶
//      前 K 名"的并集里，所以并行分段各自截断再并起来也等价。
//   ② 桶内比大小不造 QString：名字长度 = 路径的 UTF-16 单元数 - nameStart（老写法就是
//      path.mid(nameStart)，nameStart 是**字节**偏移却被当字符下标用，中文路径下名字
//      会偏长 —— 那是老行为，这里照抄，不许顺手修），路径按 UTF-8 字节序比：UTF-8 的
//      字节序就是码点序，码点序就是 BMP 字符的 UTF-16 序。只有两边名字长度相同且任一
//      条含 4 字节序列（emoji 那类，UTF-16 里是代理对，会和码点序错开）才退回 QString 比。
//   ③ 首字节 memchr 跳：老写法在每个位置都把整串 needle 比一遍。
//   ④ 分段并行：读锁是共享的，主线程持一次，工作线程只碰只读数据。
namespace {

struct DeepCand {
    const char* path = nullptr;   // 指向映射视图里的路径 UTF-8 本体（不拷贝）
    int len = 0;                  // 路径字节数
    int nameStart = 0;            // 老写法那个"字节偏移当字符下标"的值
    int nameSize = 0;             // 老写法算出来的名字 QString 长度
    bool isDir = false;
    bool astral = false;          // 路径里有 4 字节 UTF-8 序列
};

// UTF-8 字节数 → UTF-16 单元数（码点数 + 每个星平面字符再多算一个）
int utf16LenOf(const char* p, int n, bool* astral)
{
    int units = 0;
    bool has4 = false;
    for (int i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if ((c & 0xC0) == 0x80)
            continue;                       // 续字节不另算
        ++units;
        if (c >= 0xF0) { has4 = true; i += 3; }
        else if (c >= 0xE0) i += 2;
        else if (c >= 0xC0) i += 1;
    }
    if (astral)
        *astral = has4;
    return units;
}

bool deepLess(const DeepCand& a, const DeepCand& b)
{
    if (a.nameSize != b.nameSize)
        return a.nameSize < b.nameSize;
    if (a.astral || b.astral)
        return QString::fromUtf8(a.path, a.len) < QString::fromUtf8(b.path, b.len);
    const int n = qMin(a.len, b.len);
    const int c = n ? memcmp(a.path, b.path, size_t(n)) : 0;
    if (c != 0)
        return c < 0;
    return a.len < b.len;
}

// 桶按"好的在前"排着，满了就跟最差那条比，比它好才挤进去
void pushBounded(QVector<DeepCand>& v, const DeepCand& c, int cap)
{
    if (v.size() >= cap) {
        if (!deepLess(c, v.last()))
            return;
        v.removeLast();
    }
    int i = 0;
    while (i < v.size() && deepLess(v[i], c))
        ++i;
    v.insert(i, c);
}

bool quickLess(const FileEntry& a, const FileEntry& b)
{
    if (a.name.size() != b.name.size())
        return a.name.size() < b.name.size();
    return a.path < b.path;
}

void pushQuick(QVector<const FileEntry*>& v, const FileEntry& e, int cap)
{
    if (v.size() >= cap) {
        if (!quickLess(e, *v.last()))
            return;
        v.removeLast();
    }
    int i = 0;
    while (i < v.size() && quickLess(*v[i], e))
        ++i;
    v.insert(i, &e);
}

// 在 [hay,hayLen) 里找 ne（ASCII 大小写无关、其余字节按原样比），返回首次下标。
// 和老写法那个逐位置比整串的 indexOfCI 判定完全一样，只是首字节改用 memchr 跳。
int findCi(const char* hay, int hayLen, const char* ne, int neLen)
{
    if (neLen == 0)
        return 0;
    if (neLen > hayLen)
        return -1;
    const int last = hayLen - neLen;
    const char c0 = ne[0];
    const bool alpha = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
    int i = 0;
    while (i <= last) {
        const size_t span = size_t(last - i + 1);
        const char* hit = nullptr;
        if (alpha) {
            const char* p1 = static_cast<const char*>(memchr(hay + i, c0 | 0x20, span));
            const char* p2 = static_cast<const char*>(memchr(hay + i, c0 & ~0x20, span));
            if (!p1 && !p2)
                return -1;
            hit = (!p2 || (p1 && p1 < p2)) ? p1 : p2;
        } else {
            hit = static_cast<const char*>(memchr(hay + i, c0, span));
            if (!hit)
                return -1;
        }
        i = int(hit - hay);
        int j = 1;
        for (; j < neLen; ++j)
            if (tolower(static_cast<unsigned char>(hit[j]))
                != tolower(static_cast<unsigned char>(ne[j])))
                break;
        if (j == neLen)
            return i;
        ++i;
    }
    return -1;
}

// 一段深索引条目（某个卷的 [from,to)），工作线程按段领活
struct Seg {
    quint32 vol;
    quint32 from;
    quint32 to;
};

} // namespace

QVector<FileEntry> FileIndexer::search(const QString& query, int limit) const
{
    QVector<FileEntry> res;
    const QString q = query.trimmed();
    if (q.isEmpty() || limit <= 0)
        return res;
    const int cap = 2 * limit;

    QReadLocker lock(&m_lock);

    // ---- 快索引（source 0）：本来就是 QString，不物化新东西 ----
    QVector<const FileEntry*> qb[4];
    for (const FileEntry& e : m_entries) {
        const int idx = e.name.indexOf(q, 0, Qt::CaseInsensitive);
        if (idx < 0)
            continue;
        pushQuick(qb[(idx == 0 ? 0 : 2) + (e.isDir ? 0 : 1)], e, cap);
    }

    // ---- 深索引（source 1）：映射快照分段并行 + 增量新增 ----
    QVector<DeepCand> db[4];
    const QByteArray q8 = q.toLower().toUtf8();
    const char* ne = q8.constData();
    const int neLen = int(q8.size());

    quint64 total = 0;
    for (const MappedVol& v : m_base)
        total += v.count;
    if (total > 0) {
        unsigned hw = std::thread::hardware_concurrency();
        int threads = int(hw > 0 ? hw : 4);
        if (threads > 8)
            threads = 8;                       // 再多就是抢内存带宽，不涨

        QVector<Seg> segs;
        const quint32 chunk = quint32(qMax<qint64>(8192, qint64(total / (threads * 8)) + 1));
        for (quint32 vi = 0; vi < quint32(m_base.size()); ++vi) {
            const quint32 cnt = m_base[vi].count;
            for (quint32 from = 0; from < cnt; from += chunk)
                segs.append({vi, from, qMin(cnt, from + chunk)});
        }
        // 每个工作线程自己的 4 个分数桶（slot[线程*4+分数]），最后再并起来
        // （并起来仍等价，见上面①）。用扁平 vector 而不是本地结构体数组：MSVC
        // 对"本地类型 + lambda 捕获 + 范围 for"这套组合会报看不懂的错。
        std::vector<QVector<DeepCand>> slot(size_t(threads) * 4);
        std::atomic<quint32> next {0};
        const auto work = [&](int t) {
            QVector<DeepCand>* me = &slot[size_t(t) * 4];
            for (;;) {
                const quint32 k = next.fetch_add(1);
                if (k >= quint32(segs.size()))
                    return;
                const Seg& sg = segs[int(k)];
                const MappedVol& v = m_base[int(sg.vol)];
                const char* entries = static_cast<const char*>(m_mapView) + v.entryOff;
                const char* arena = static_cast<const char*>(m_mapView) + v.arenaOff;
                for (quint32 i = sg.from; i < sg.to; ++i) {
                    PackedDeepEntry de;
                    memcpy(&de, entries + qint64(i) * kPackedEntrySize, kPackedEntrySize);
                    if (m_removed.contains(de.frn))
                        continue;
                    const int nameLen = int(de.len) - int(de.nameStart);
                    const int idx = findCi(arena + de.offset + de.nameStart, nameLen, ne, neLen);
                    if (idx < 0)
                        continue;
                    DeepCand c;
                    c.path = arena + de.offset;
                    c.len = int(de.len);
                    c.nameStart = int(de.nameStart);
                    c.isDir = de.isDir != 0;
                    bool astral = false;
                    c.nameSize = qMax(0, utf16LenOf(c.path, c.len, &astral) - c.nameStart);
                    c.astral = astral;
                    pushBounded(me[(idx == 0 ? 0 : 2) + (c.isDir ? 0 : 1)], c, cap);
                }
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(size_t(threads));
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] { work(t); });
        for (std::thread& th : pool)
            th.join();
        for (size_t i = 0; i < slot.size(); ++i)
            for (const DeepCand& c : slot[i])
                pushBounded(db[int(i % 4)], c, cap);
    }

    // 增量新增那批（几千条量级，留在本线程扫）
    const char* addArena = m_addArena.constData();
    for (const FileDeepEntry& de : m_add) {
        const char* nameStart = addArena + de.offset + de.nameStart;
        const int nameLen = int(de.len) - int(de.nameStart);
        const int idx = findCi(nameStart, nameLen, ne, neLen);
        if (idx < 0)
            continue;
        DeepCand c;
        c.path = addArena + de.offset;
        c.len = int(de.len);
        c.nameStart = int(de.nameStart);
        c.isDir = de.isDir != 0;
        bool astral = false;
        c.nameSize = qMax(0, utf16LenOf(c.path, c.len, &astral) - c.nameStart);
        c.astral = astral;
        pushBounded(db[(idx == 0 ? 0 : 2) + (c.isDir ? 0 : 1)], c, cap);
    }

    // ---- 按 (来源, 分数, 名字长度, 路径) 发出去，边发边按路径去重 ----
    QSet<QString> seen;
    for (int source = 0; source < 2; ++source) {
        for (int score = 0; score < 4; ++score) {
            if (source == 0) {
                for (const FileEntry* e : qb[score]) {
                    const QString key = e->path.toLower();
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                    res.append(*e);
                    if (res.size() >= limit)
                        return res;
                }
            } else {
                for (const DeepCand& c : db[score]) {
                    const QString path = QString::fromUtf8(c.path, c.len);
                    const QString key = path.toLower();
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                    res.append({path.mid(c.nameStart), path, c.isDir});
                    if (res.size() >= limit)
                        return res;
                }
            }
        }
    }
    return res;
}

// ---------- 快索引实时监听 ----------

void FileIndexer::startWatchers()
{
    if (m_watchCtx)
        return;
    auto* ctx = new WatchCtx;
    ctx->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    for (const QuickRoot& qr : quickRoots()) {
        // 用户主目录子树过大且与其他目录重叠，不监听（其顶层列表靠快索引快照）
        if (qr.dir == home)
            continue;
        WatchRoot r;
        r.root = QDir::toNativeSeparators(qr.dir);
        r.dir = CreateFileW((LPCWSTR)r.root.utf16(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (r.dir == INVALID_HANDLE_VALUE) {
            r.dir = nullptr;
            continue;
        }
        r.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        r.buffer.resize(int(kQuickWatchBuffer));
        if (!issueWatch(r))
            continue;
        ctx->roots.append(r);
    }
    if (ctx->roots.isEmpty()) {
        CloseHandle(ctx->stopEvent);
        delete ctx;
        return;
    }
    m_watchCtx = ctx;
    std::thread([this] { watcherLoop(m_watchCtx); }).detach();
}

void FileIndexer::watcherLoop(void* ctxPtr)
{
    auto* ctx = static_cast<WatchCtx*>(ctxPtr);
    QVector<HANDLE> handles;
    handles.append(ctx->stopEvent);
    for (const WatchRoot& r : ctx->roots)
        handles.append(r.event);

    for (;;) {
        const DWORD wait = WaitForMultipleObjects(DWORD(handles.size()), handles.data(),
                                                  FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0)
            return; // 退出
        if (wait < WAIT_OBJECT_0 + 1 || wait > WAIT_OBJECT_0 + DWORD(ctx->roots.size()))
            continue;
        WatchRoot& r = ctx->roots[int(wait - WAIT_OBJECT_0 - 1)];

        DWORD bytes = 0;
        bool overflow = false;
        if (!GetOverlappedResult(r.dir, &r.ov, &bytes, FALSE)) {
            if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
                overflow = true;
        } else if (bytes == 0) {
            overflow = true; // 缓冲区溢出
        }

        if (overflow) {
            rebuildQuickSync(); // 变化太多来不及逐条，整表重建
        } else {
            const char* base = r.buffer.constData();
            DWORD off = 0;
            for (;;) {
                if (off + sizeof(FILE_NOTIFY_INFORMATION) > bytes)
                    break;
                const FILE_NOTIFY_INFORMATION* fni =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);
                const QString name = QString::fromWCharArray(fni->FileName,
                                                             int(fni->FileNameLength / sizeof(WCHAR)));
                const QString full = QDir::fromNativeSeparators(r.root + "\\" + name);
                switch (fni->Action) {
                case FILE_ACTION_ADDED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    watcherApplyAdd(full);
                    break;
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME:
                    watcherApplyRemove(full);
                    break;
                default:
                    break; // MODIFIED 等与文件名索引无关
                }
                if (fni->NextEntryOffset == 0)
                    break;
                off += fni->NextEntryOffset;
            }
        }
        issueWatch(r);
    }
}

void FileIndexer::rebuildQuickSync()
{
    QVector<FileEntry> quick;
    quick.reserve(8192);
    for (const QuickRoot& r : quickRoots())
        walk(r.dir, r.depth, &quick, 200000);
    QWriteLocker lock(&m_lock);
    m_entries = quick;
}

void FileIndexer::watcherApplyAdd(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return;
    const QString fn = fi.fileName();
    if (shouldSkip(fn))
        return;
    QWriteLocker lock(&m_lock);
    // 重叠监听（桌面在用户目录子树内）会产生重复事件，按路径去重
    for (const FileEntry& e : m_entries)
        if (QString::compare(e.path, path, Qt::CaseInsensitive) == 0)
            return;
    m_entries.append({fn, path, fi.isDir()});
}

void FileIndexer::watcherApplyRemove(const QString& path)
{
    const QString prefix = path + '/';
    QWriteLocker lock(&m_lock);
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const FileEntry& e) {
                                       return QString::compare(e.path, path, Qt::CaseInsensitive) == 0
                                              || e.path.startsWith(prefix, Qt::CaseInsensitive);
                                   }),
                    m_entries.end());
}
