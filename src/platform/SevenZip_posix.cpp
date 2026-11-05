#include "SevenZip.h"
#include "Utils.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>

namespace SevenZip {

Archive::Result compress(const QStringList& inputs, const QString& destFile, int level,
                         const Archive::Report& report, const Archive::Cancel& cancel)
{
    Archive::Result res;
    const QString nativeDest = QDir::toNativeSeparators(destFile);
    struct archive* a = archive_write_new();
    if (!a) {
        res.error = QStringLiteral("无法创建 archive 写入对象");
        return res;
    }

    if (archive_write_set_format_7zip(a) != ARCHIVE_OK) {
        res.error = QStringLiteral("libarchive 无法设置 7z 写入格式：%1")
                        .arg(QString::fromUtf8(archive_error_string(a)));
        archive_write_free(a);
        return res;
    }

    // 设置压缩档位（7z 支持 0..9，默认 5）
    const int effectiveLevel = qBound(0, level, 9);
    const QByteArray optLevel = QStringLiteral("7zip:compression-level=%1").arg(effectiveLevel).toUtf8();
    archive_write_set_options(a, optLevel.constData());

    const QString partPath = nativeDest + QStringLiteral(".part");
    if (archive_write_open_filename(a, QFile::encodeName(partPath).constData()) != ARCHIVE_OK) {
        res.error = QStringLiteral("建不出压缩包：%1")
                        .arg(QString::fromUtf8(archive_error_string(a)));
        archive_write_free(a);
        return res;
    }

    qint64 totalBytes = 0;
    for (const QString& in : inputs) {
        const QFileInfo fi(in);
        if (fi.isDir()) {
            QDirIterator it(in, QDir::Files | QDir::NoSymLinks | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                totalBytes += QFileInfo(it.next()).size();
        } else if (fi.exists()) {
            totalBytes += fi.size();
        }
    }

    constexpr qint64 kBuf = 64 * 1024;
    qint64 done = 0;
    bool stopped = false;

    for (const QString& in : inputs) {
        const QFileInfo rootInfo(in);
        if (!rootInfo.exists())
            continue;
        const QString parent = rootInfo.absolutePath();
        QStringList members;
        if (rootInfo.isDir()) {
            members << in;
            QDirIterator dit(in, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                             QDirIterator::Subdirectories);
            while (dit.hasNext())
                members << dit.next();
            QDirIterator fit(in, QDir::Files | QDir::NoSymLinks | QDir::Hidden | QDir::System,
                             QDirIterator::Subdirectories);
            while (fit.hasNext())
                members << fit.next();
        } else {
            members << in;
        }

        for (const QString& m : members) {
            if (cancel && cancel()) {
                res.cancelled = true;
                stopped = true;
                break;
            }
            const QFileInfo mf(m);
            struct archive_entry* e = archive_entry_new();
            const QString rel = QDir(parent).relativeFilePath(m).replace(QLatin1Char('\\'), QLatin1Char('/'));
            archive_entry_set_pathname_utf8(e, rel.toUtf8().constData());

            if (mf.isDir()) {
                archive_entry_set_filetype(e, AE_IFDIR);
                archive_entry_set_perm(e, 0755);
                if (archive_write_header(a, e) != ARCHIVE_OK) {
                    res.error = QStringLiteral("目录 %1 无法写入 7z 包：%2")
                                    .arg(mf.fileName(), QString::fromUtf8(archive_error_string(a)));
                    archive_entry_free(e);
                    stopped = true;
                    break;
                }
                ++res.dirs;
                archive_entry_free(e);
                continue;
            }

            QFile f(m);
            const qint64 sz = mf.size();
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
            archive_entry_set_size(e, sz);
            archive_entry_set_mtime(e, mf.lastModified().toSecsSinceEpoch(), 0);
            if (archive_write_header(a, e) != ARCHIVE_OK) {
                res.error = QStringLiteral("%1 无法写入 7z 包：%2")
                                .arg(m, QString::fromUtf8(archive_error_string(a)));
                archive_entry_free(e);
                stopped = true;
                break;
            }
            archive_entry_free(e);

            if (!f.open(QIODevice::ReadOnly)) {
                res.error = QStringLiteral("读取失败：%1").arg(m);
                stopped = true;
                break;
            }

            bool io = false;
            while (!f.atEnd()) {
                const QByteArray chunk = f.read(kBuf);
                if (chunk.isEmpty())
                    break;
                const la_ssize_t w = archive_write_data(a, chunk.constData(), size_t(chunk.size()));
                if (w != la_ssize_t(chunk.size())) {
                    res.error = QStringLiteral("写入 7z 数据失败：%1").arg(m);
                    io = true;
                    break;
                }
                done += w;
                if (report && !report(Archive::Progress{ done, totalBytes, mf.fileName() })) {
                    res.cancelled = true;
                    stopped = true;
                    break;
                }
            }
            f.close();
            if (io || stopped)
                break;
            ++res.files;
        }
        if (stopped)
            break;
    }

    const int closeRc = archive_write_close(a);
    const QString closeMsg = closeRc == ARCHIVE_OK ? QString()
                                                   : QString::fromUtf8(archive_error_string(a));
    archive_write_free(a);
    res.bytes = done;

    if (res.cancelled) {
        QFile::remove(partPath);
        res.error = QStringLiteral("已取消，临时 7z 文件已清理");
        return res;
    }
    if (stopped) {
        QFile::remove(partPath);
        return res;
    }
    if (closeRc != ARCHIVE_OK) {
        QFile::remove(partPath);
        res.error = QStringLiteral("7z 压缩包写入未完成：%1").arg(closeMsg);
        return res;
    }

    QString renameErr;
    if (!Utils::renameStaged(partPath, nativeDest, &renameErr)) {
        res.error = renameErr;
        res.target = partPath;
        return res;
    }

    res.ok = true;
    res.target = nativeDest;
    return res;
}

} // namespace SevenZip
