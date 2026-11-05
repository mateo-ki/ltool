#include "RepairPage.h"
#include "platform/Launch.h"
#include "DevEnv.h"
#include "SilentInstallLog.h"
#include "Utils.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <windows.h>

namespace {

struct WingetPkg {
    const char* id;
    const char* arch;       // "x64" / "x86" / "" = 不分架构
    const char* file;       // 该架构安装后必然存在的系统文件（检测兜底），空 = 只查注册表
    const char* directUrl;  // 微软官方直链安装包
};

struct RepairRow {
    const char* name;
    QString match;              // 卸载表显示名包含片段
    QVector<WingetPkg> pkgs;    // 可安装的包
    const char* url;            // 微软官方下载主页（可直接在浏览器打开）
    bool builtin;               // Win10+ 系统内置
};

const RepairRow kRows[] = {
    {".NET Desktop Runtime", "windows desktop runtime",
     {{"Microsoft.DotNet.DesktopRuntime.8", "", "", "https://dotnet.microsoft.com/download/dotnet/8.0"}},
     "https://dotnet.microsoft.com/download/dotnet", false},
    {"VC++ 2005 Redistributable", "visual c++ 2005",
     {{"Microsoft.VCRedist.2005.x64", "x64", "msvcr80.dll",
       "https://download.microsoft.com/download/4/A/2/4A22001F-FA3B-4C13-BF4E-42EC249D51C4/vcredist_x64.EXE"},
      {"Microsoft.VCRedist.2005.x86", "x86", "msvcr80.dll",
       "https://download.microsoft.com/download/4/A/2/4A22001F-FA3B-4C13-BF4E-42EC249D51C4/vcredist_x86.EXE"}},
     "https://www.microsoft.com/zh-cn/download/details.aspx?id=26347", false},
    {"VC++ 2008 Redistributable", "visual c++ 2008",
     {{"Microsoft.VCRedist.2008.x64", "x64", "msvcr90.dll",
       "https://download.microsoft.com/download/5/D/8/5D8C65CB-C849-4025-8E95-C3966CAFD8AE/vcredist_x64.exe"},
      {"Microsoft.VCRedist.2008.x86", "x86", "msvcr90.dll",
       "https://download.microsoft.com/download/5/D/8/5D8C65CB-C849-4025-8E95-C3966CAFD8AE/vcredist_x86.exe"}},
     "https://www.microsoft.com/zh-cn/download/details.aspx?id=26368", false},
    {"VC++ 2010 Redistributable", "visual c++ 2010",
     {{"Microsoft.VCRedist.2010.x64", "x64", "msvcr100.dll",
       "https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe"},
      {"Microsoft.VCRedist.2010.x86", "x86", "msvcr100.dll",
       "https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x86.exe"}},
     "https://www.microsoft.com/zh-cn/download/details.aspx?id=26999", false},
    {"VC++ 2012 Redistributable", "visual c++ 2012",
     {{"Microsoft.VCRedist.2012.x64", "x64", "msvcr110.dll",
       "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe"},
      {"Microsoft.VCRedist.2012.x86", "x86", "msvcr110.dll",
       "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x86.exe"}},
     "https://www.microsoft.com/zh-cn/download/details.aspx?id=30679", false},
    {"VC++ 2013 Redistributable", "visual c++ 2013",
     {{"Microsoft.VCRedist.2013.x64", "x64", "msvcr120.dll",
       "https://download.visualstudio.microsoft.com/download/pr/10912041/cee5d6bca2ddbcd039da727bf4acb48a/vcredist_x64.exe"},
      {"Microsoft.VCRedist.2013.x86", "x86", "msvcr120.dll",
       "https://download.visualstudio.microsoft.com/download/pr/10912113/5da66ddebb0ad32ebd4b922fd82e8e25/vcredist_x86.exe"}},
     "https://support.microsoft.com/zh-cn/help/4032938", false},
    {"VC++ 2015-2022 Redistributable", "visual c++ redistributable",
     {{"Microsoft.VCRedist.2015+.x64", "x64", "vcruntime140.dll",
       "https://aka.ms/vs/17/release/vc_redist.x64.exe"},
      {"Microsoft.VCRedist.2015+.x86", "x86", "vcruntime140.dll",
       "https://aka.ms/vs/17/release/vc_redist.x86.exe"}},
     "https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist", false},
    {"Visual Studio 2010 Tools For Office Runtime", "visual studio 2010 tools for office runtime",
     {}, "https://www.microsoft.com/zh-cn/download/details.aspx?id=48208", false},
    {"DirectX End-User Runtime", "directx runtime",
     {{"Microsoft.DirectX", "", "d3dx9_43.dll",
       "https://download.microsoft.com/download/1/7/1/1718CCC4-6315-4D8E-9543-8E28A4E18C4C/dxwebsetup.exe"}},
     "https://www.microsoft.com/zh-cn/download/details.aspx?id=35", false},
    {"Universal CRT", "universal c runtime",
     {}, "https://support.microsoft.com/zh-cn/topic/kb2999226", true},
};

const WingetPkg* findPkg(const QString& id)
{
    for (const RepairRow& r : kRows) {
        for (const WingetPkg& pkg : r.pkgs) {
            if (QString::fromLatin1(pkg.id).compare(id, Qt::CaseInsensitive) == 0)
                return &pkg;
        }
    }
    return nullptr;
}

const RepairRow* findRowForPkg(const QString& id)
{
    for (const RepairRow& r : kRows) {
        for (const WingetPkg& pkg : r.pkgs) {
            if (QString::fromLatin1(pkg.id).compare(id, Qt::CaseInsensitive) == 0)
                return &r;
        }
    }
    return nullptr;
}

struct InstalledInfo {
    QString name;
    QString version;
    bool wow = false; // 来自 32 位视图（WOW6432Node）
};

QVector<InstalledInfo> installedDisplayNames()
{
    QVector<InstalledInfo> names;
    struct Root { HKEY hive; const wchar_t* sub; bool wow; };
    const Root roots[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", false},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", true},
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", false},
    };
    for (const Root& root : roots) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root.hive, root.sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;
        wchar_t name[256];
        DWORD nameLen = 256;
        DWORD idx = 0;
        while (RegEnumKeyExW(k, idx++, name, &nameLen, nullptr, nullptr, nullptr, nullptr)
               == ERROR_SUCCESS) {
            nameLen = 256;
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, name, 0, KEY_READ, &sk) == ERROR_SUCCESS) {
                DWORD type = 0, size = 0;
                if (RegQueryValueExW(sk, L"DisplayName", nullptr, &type, nullptr, &size)
                    == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    QByteArray buf(int(size), 0);
                    if (RegQueryValueExW(sk, L"DisplayName", nullptr, &type,
                                         (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
                        QString dn = QString::fromWCharArray(
                            (const wchar_t*)buf.constData(), size / 2);
                        while (dn.endsWith(QChar(0)))
                            dn.chop(1);
                        QString ver;
                        DWORD vs = 0;
                        if (RegQueryValueExW(sk, L"DisplayVersion", nullptr, &type,
                                             nullptr, &vs) == ERROR_SUCCESS) {
                            QByteArray vb(int(vs), 0);
                            if (RegQueryValueExW(sk, L"DisplayVersion", nullptr, &type,
                                                 (BYTE*)vb.data(), &vs) == ERROR_SUCCESS) {
                                ver = QString::fromWCharArray(
                                    (const wchar_t*)vb.constData(), vs / 2);
                                while (ver.endsWith(QChar(0)))
                                    ver.chop(1);
                            }
                        }
                        if (!dn.trimmed().isEmpty())
                            names.append({dn.trimmed().toLower(), ver.trimmed(), root.wow});
                    }
                }
                RegCloseKey(sk);
            }
        }
        RegCloseKey(k);
    }
    return names;
}

bool fileExistsIn(const QStringList& dirs, const QString& fileName)
{
    for (const QString& d : dirs)
        if (QFileInfo::exists(d + "/" + fileName))
            return true;
    return false;
}

// 检测某个具体包是否已在本机就绪：优先匹配卸载注册表，老运行库支持 WinSxS 程序集探测
bool isPkgInstalled(const RepairRow& r, const WingetPkg& pkg,
                    const QVector<InstalledInfo>& names, QString* outVer = nullptr)
{
    const bool isX64 = QString(pkg.arch) == QLatin1String("x64");
    for (const InstalledInfo& n : names) {
        if (!n.name.contains(r.match, Qt::CaseInsensitive))
            continue;
        const bool archOk = pkg.arch[0] == 0
            || (isX64 ? (!n.wow || n.name.contains("x64"))
                      : (n.wow || n.name.contains("x86")));
        if (archOk) {
            if (outVer && outVer->isEmpty() && !n.version.isEmpty())
                *outVer = QString("v%1").arg(n.version);
            return true;
        }
    }

    // 针对 VC++ 2005 (CRT 8.0) 和 VC++ 2008 (CRT 9.0) 探测 WinSxS 程序集目录
    if (pkg.file[0] != 0) {
        const QString fileName = QString::fromLatin1(pkg.file);
        if (fileName == QLatin1String("msvcr80.dll") || fileName == QLatin1String("msvcr90.dll")) {
            const QString crtPrefix = (fileName == QLatin1String("msvcr80.dll"))
                ? QStringLiteral("microsoft.vc80.crt")
                : QStringLiteral("microsoft.vc90.crt");
            QDir winsxs(QStringLiteral("C:/Windows/WinSxS"));
            const QString filter = (isX64 ? QStringLiteral("amd64_") : QStringLiteral("x86_"))
                + crtPrefix + QLatin1Char('*');
            const QStringList dirs = winsxs.entryList({filter}, QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString& d : dirs) {
                if (QFileInfo::exists(winsxs.filePath(d + QLatin1Char('/') + fileName)))
                    return true;
            }
        }
        // 关键系统文件常规探测
        if (fileExistsIn({isX64 ? "C:/Windows/System32" : "C:/Windows/SysWOW64"}, fileName))
            return true;
    }
    return false;
}

// 这一行里还没装上的 winget 包
QStringList missingPkgs(const RepairRow& r, const QVector<InstalledInfo>& names)
{
    QStringList miss;
    for (const WingetPkg& pkg : r.pkgs) {
        if (!isPkgInstalled(r, pkg, names))
            miss.append(QString::fromLatin1(pkg.id));
    }
    return miss;
}

} // namespace

RepairPage::RepairPage(QWidget* parent)
    : QWidget(parent)
{
    m_nam = new QNetworkAccessManager(this);
    DevEnv::applyProxy(m_nam);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 4, 4, 4);

    auto* head = new QHBoxLayout;
    auto* tip = new QLabel("启动软件提示 dll 缺失？检测微软常用运行库（区分 x64/x86），支持一键静默安装、内置直链下载与官网直达。");
    tip->setObjectName("tip");
    tip->setWordWrap(true);
    head->addWidget(tip, 1);
    m_allBtn = new QPushButton("一键修复", this);
    m_allBtn->setObjectName("primary");
    m_allBtn->setEnabled(false);
    head->addWidget(m_allBtn);
    auto* rescanBtn = new QPushButton("重新检测", this);
    head->addWidget(rescanBtn);
    lay->addLayout(head);

    auto* rowsHost = new QWidget(this);
    rowsHost->setObjectName("rowsHost");
    m_rowsLay = new QVBoxLayout(rowsHost);
    m_rowsLay->setContentsMargins(0, 0, 0, 0);
    m_rowsLay->setSpacing(4);
    m_rowsLay->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(rowsHost);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");
    lay->addWidget(scroll, 1);

    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    lay->addWidget(m_status);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(150);
    m_log->setPlaceholderText("安装过程日志…");
    lay->addWidget(m_log);

    connect(rescanBtn, &QPushButton::clicked, this, &RepairPage::rescan);
    connect(m_allBtn, &QPushButton::clicked, this, &RepairPage::repairAll);
    rescan();
}

RepairPage::~RepairPage()
{
    if (m_downloadReply) {
        m_downloadReply->abort();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }
}

void RepairPage::clearRows()
{
    while (m_rowsLay->count() > 1) {
        QLayoutItem* item = m_rowsLay->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void RepairPage::rescan()
{
    clearRows();
    m_missing.clear();
    const QVector<InstalledInfo> names = installedDisplayNames();

    bool allOk = true;
    for (const RepairRow& r : kRows) {
        const int rowIndex = int(&r - kRows);
        bool installed = false;
        QString note;
        bool needX64 = false, needX86 = false;

        if (r.builtin) {
            installed = true;
            note = "系统内置";
        } else if (!r.pkgs.isEmpty() && r.pkgs.first().arch[0] != 0) {
            // 分架构检测：注册表条目 或 WinSxS / 关键系统文件
            for (const WingetPkg& pkg : r.pkgs) {
                QString ver;
                const bool isX64 = QString(pkg.arch) == "x64";
                const bool have = isPkgInstalled(r, pkg, names, &ver);
                if (have) {
                    installed = true;
                    if (!ver.isEmpty() && note.isEmpty())
                        note = ver;
                    continue;
                }
                if (isX64) needX64 = true;
                else needX86 = true;
            }
            if (needX64 || needX86) {
                installed = false;
                note.clear();
            }
        } else if (!r.pkgs.isEmpty()) {
            const WingetPkg& pkg = r.pkgs.first();
            QString ver;
            installed = isPkgInstalled(r, pkg, names, &ver);
            if (installed && !ver.isEmpty())
                note = ver;
        } else {
            for (const InstalledInfo& n : names) {
                if (n.name.contains(r.match, Qt::CaseInsensitive)) {
                    installed = true;
                    if (!n.version.isEmpty())
                        note = QString("v%1").arg(n.version);
                    break;
                }
            }
            if (!installed && QString(r.name).startsWith("DirectX"))
                installed = fileExistsIn({"C:/Windows/SysWOW64"}, "d3dx9_43.dll");
        }
        if (!installed) {
            allOk = false;
            m_missing.append(rowIndex);
        }

        // 行：名称 + 状态/版本 + [安装] / [官网]
        auto* row = new QWidget(this);
        row->setAttribute(Qt::WA_StyledBackground);
        row->setObjectName("row");
        row->setCursor(installed ? Qt::ArrowCursor : Qt::PointingHandCursor);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(10, 5, 8, 5);
        rowLay->setSpacing(10);

        auto* nameLabel = new QLabel(r.name, row);
        QFont f = nameLabel->font();
        f.setBold(true);
        nameLabel->setFont(f);
        rowLay->addWidget(nameLabel);
        rowLay->addStretch(1);

        auto* status = new QLabel(row);
        if (installed) {
            status->setText(note.isEmpty() ? "✔ 正常" : "✔ 正常  " + note);
            status->setStyleSheet("color:#2ea058;background:transparent;");
        } else if (needX64 || needX86) {
            QStringList miss;
            if (needX64) miss.append("x64");
            if (needX86) miss.append("x86");
            status->setText(QString("◐ 部分安装 · 缺少 %1").arg(miss.join('+')));
            status->setStyleSheet("color:#d6800f;background:transparent;");
        } else {
            status->setText("✘ 未安装");
            status->setStyleSheet("color:#d6800f;background:transparent;");
        }
        rowLay->addWidget(status);

        if (!installed) {
            if (!r.pkgs.isEmpty()) {
                auto* installBtn = new QPushButton("安装", row);
                installBtn->setObjectName("primary");
                installBtn->setFixedWidth(64);
                installBtn->setCursor(Qt::PointingHandCursor);
                connect(installBtn, &QPushButton::clicked, this,
                        [this, rowIndex] { installRow(rowIndex); });
                rowLay->addWidget(installBtn);
            }

            if (r.url[0] != 0) {
                auto* webBtn = new QPushButton(r.pkgs.isEmpty() ? "官网下载" : "官网", row);
                webBtn->setFixedWidth(r.pkgs.isEmpty() ? 72 : 56);
                webBtn->setCursor(Qt::PointingHandCursor);
                webBtn->setToolTip(QString("在浏览器中打开「%1」微软官方下载页面").arg(r.name));
                connect(webBtn, &QPushButton::clicked, this, [&r] {
                    Platform::launch(r.url);
                });
                rowLay->addWidget(webBtn);
            }
        }

        row->installEventFilter(this);
        row->setProperty("repairIndex", rowIndex);
        row->setProperty("repairMissing", !installed);

        m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);
    }

    int autoFixable = 0;
    for (int idx : m_missing)
        if (!kRows[idx].pkgs.isEmpty())
            ++autoFixable;
    m_allBtn->setEnabled(autoFixable > 0);
    m_allBtn->setText(autoFixable > 0 ? QString("一键修复（%1 项）").arg(autoFixable)
                                      : QStringLiteral("一键修复"));

    m_status->setText(allOk
        ? "全部正常 ✓"
        : (autoFixable > 0
               ? QString("有 %1 项可自动安装：点「一键修复」全装，点击「安装」装单项，或点「官网」手动下载。")
                     .arg(autoFixable)
               : "存在未安装项：点击该行或右侧「官网下载」打开官方页面。"));
}

bool RepairPage::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress) {
        auto* row = qobject_cast<QWidget*>(obj);
        if (row && row->property("repairMissing").toBool()) {
            installRow(row->property("repairIndex").toInt());
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void RepairPage::installRow(int row)
{
    static const int rowCount = int(sizeof(kRows) / sizeof(kRows[0]));
    if (row < 0 || row >= rowCount)
        return;
    const RepairRow& r = kRows[row];

    if (!r.pkgs.isEmpty()) {
        const QStringList miss = missingPkgs(r, installedDisplayNames());
        if (miss.isEmpty()) {
            m_status->setText(QString("%1 已完整安装。").arg(QString::fromLatin1(r.name)));
            return;
        }
        if (!DevEnv::wingetAvailable()) {
            // 系统没有 winget，直接给出直链/官网下载
            const WingetPkg& firstPkg = r.pkgs.first();
            if (firstPkg.directUrl && firstPkg.directUrl[0] != 0) {
                if (QMessageBox::question(
                        this, "ltool",
                        QString("系统未检测到 winget。是否使用内置下载器下载「%1」？\n\n"
                                "· 点【是】：直接下载官方安装包并自动运行\n"
                                "· 点【否】：在浏览器打开微软官方下载页")
                            .arg(QString::fromLatin1(r.name))) == QMessageBox::Yes) {
                    downloadAndRun(QString::fromLatin1(r.name), QString::fromLatin1(firstPkg.directUrl));
                    return;
                }
            }
            if (r.url && r.url[0] != 0) {
                Platform::launch(r.url);
                QMessageBox::information(this, "ltool", "已打开官方下载页，安装后点击“重新检测”。");
                return;
            }
        }
        installQueue(QString::fromLatin1(r.name), miss);
        return;
    }
    if (QString(r.url).isEmpty()) {
        QMessageBox::information(this, "ltool", "该组件没有配置自动安装方式。");
        return;
    }
    Platform::launch(r.url);
    QMessageBox::information(this, "ltool", "已打开官方下载页，安装后点击\"重新检测\"。");
}

void RepairPage::repairAll()
{
    const QVector<InstalledInfo> names = installedDisplayNames();
    QStringList ids, what;
    for (int idx : m_missing) {
        const RepairRow& r = kRows[idx];
        const QStringList miss = missingPkgs(r, names);
        if (miss.isEmpty())
            continue;
        ids += miss;
        what << QString("  · %1 → %2").arg(QString::fromLatin1(r.name), miss.join(", "));
    }
    ids.removeDuplicates();
    if (ids.isEmpty()) {
        m_status->setText("没有可以自动安装的缺失项（有些组件只能开官网手动下载）。");
        return;
    }
    if (QMessageBox::question(this, "ltool",
            QString("将用 winget 依次安装 %1 个组件：\n\n%2\n\n"
                    "运行库装到系统默认位置（不套「安装目录」），过程中可能弹管理员授权。")
                .arg(ids.size()).arg(what.join("\n"))) != QMessageBox::Yes)
        return;
    installQueue("一键修复", ids);
}

// 排队串行安装：winget 同时跑多个会互相抢安装锁，所以一个装完再下一个
void RepairPage::installQueue(const QString& title, const QStringList& pkgIds)
{
    if (m_installing) {
        QMessageBox::information(this, "ltool", "上一批安装还在进行，请等它结束。");
        return;
    }
    m_queue = pkgIds;
    m_queueTotal = pkgIds.size();
    m_queueTitle = title;
    m_log->appendPlainText(QString("=== %1：%2 个组件排队 ===").arg(title).arg(m_queueTotal));
    installNext();
}

void RepairPage::installNext()
{
    if (m_queue.isEmpty()) {
        m_installing = false;
        m_status->setText(QString("%1 完成：共 %2 个组件。详情见托盘图标的「安装日志」。")
                              .arg(m_queueTitle).arg(m_queueTotal));
        m_queueTitle.clear();
        m_queueTotal = 0;
        rescan();
        return;
    }
    const QString id = m_queue.takeFirst();
    m_status->setText(QString("%1：%2 剩余 %3 个")
                          .arg(m_queueTitle, id).arg(m_queue.size()));
    QString msg;
    m_installing = true;
    // 安装进程归 qApp 管，回调可能比这一页活得久：设置页关了就必须丢掉这次回调
    const QPointer<RepairPage> guard(this);
    const bool started = DevEnv::startSilentInstall(
        id, QString(), QString(), &msg, [this, guard, id](int code, const QString& out, int jobId) {
            if (!guard)
                return;
            // winget 的失败码是 0x8A15xxxx 的 HRESULT，打十进制没人看得懂
            const QString hex = QString("0x%1").arg(quint32(code), 8, 16, QChar('0')).toUpper();
            if (code == 0 || code == -1978335189 /* 0x8A15002B 已是最新，不算失败 */) {
                m_log->appendPlainText(QString("++ %1 完成").arg(id));
            } else if (SilentInstallLog::wasCancelled(jobId)) {
                m_log->appendPlainText(QString("-- %1 已取消，剩下 %2 个不再继续")
                                           .arg(id).arg(m_queue.size()));
                m_queue.clear();
            } else {
                m_log->appendPlainText(QString("!! %1 失败（%2）").arg(id, hex));
                const WingetPkg* pkg = findPkg(id);
                const RepairRow* row = findRowForPkg(id);
                if (pkg && pkg->directUrl && pkg->directUrl[0] != 0)
                    m_log->appendPlainText(QString("  · 官方直链：%1").arg(pkg->directUrl));
                if (row && row->url && row->url[0] != 0)
                    m_log->appendPlainText(QString("  · 官方主页：%1").arg(row->url));

                if (out.contains("1603")) {
                    m_log->appendPlainText(
                        "提示：MSI 返回 1603，说明系统里这个运行库的安装记录已损坏"
                        "（常见于 VC++ 2005 这类老组件）。可用微软“程序安装和卸载疑难解答”"
                        "清理残留后再试；若无程序依赖它，忽略即可。");
                } else if (out.contains("0x80070005")) {
                    m_log->appendPlainText("提示：权限不足，请以管理员身份重试该组件。");
                }

                const bool isDlErr = hex == "0x8A15005E" || hex == "0x80072EE7" || hex == "0x80072EFE"
                    || out.contains("0x8a15005e", Qt::CaseInsensitive)
                    || out.contains("0x80072ee7", Qt::CaseInsensitive)
                    || out.contains("0x80072efe", Qt::CaseInsensitive)
                    || out.contains(QStringLiteral("下载程序包时出错"))
                    || out.contains("Failed in attempting to download");
                if (isDlErr) {
                    m_log->appendPlainText("提示：网络连接微软服务器超时，winget 下载失败。已为您提供官方直链与官网链接。");
                    if (pkg && pkg->directUrl && pkg->directUrl[0] != 0) {
                        const int choice = QMessageBox::question(
                            this, "ltool - 下载失败",
                            QString("组件 %1 通过 winget 下载失败（微软 CDN 网络超时）。\n\n"
                                    "是否使用内置下载器直接下载官方安装包？\n\n"
                                    "· 点【是】：直接下载并自动运行安装向导\n"
                                    "· 点【否】：在浏览器打开微软官方下载页面\n"
                                    "· 点【取消】：取消操作")
                                .arg(id),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                        if (choice == QMessageBox::Yes) {
                            downloadAndRun(QString::fromLatin1(pkg->id), QString::fromLatin1(pkg->directUrl));
                        } else if (choice == QMessageBox::No && row && row->url && row->url[0] != 0) {
                            Platform::launch(row->url);
                        }
                    } else if (row && row->url && row->url[0] != 0) {
                        Platform::launch(row->url);
                    }
                }
            }
            installNext();
        });
    if (!started) {
        m_installing = false;
        m_log->appendPlainText(QString("!! %1 无法发起：%2").arg(id, msg));
        const WingetPkg* pkg = findPkg(id);
        const RepairRow* row = findRowForPkg(id);
        if (pkg && pkg->directUrl && pkg->directUrl[0] != 0)
            m_log->appendPlainText(QString("  · 官方直链：%1").arg(pkg->directUrl));
        if (row && row->url && row->url[0] != 0)
            m_log->appendPlainText(QString("  · 官方主页：%1").arg(row->url));

        if (pkg && pkg->directUrl && pkg->directUrl[0] != 0) {
            const int choice = QMessageBox::question(
                this, "ltool - 无法发起 winget",
                QString("组件 %1 无法通过 winget 安装（%2）。\n\n"
                        "是否使用内置下载器直接下载官方安装包？\n\n"
                        "· 点【是】：直接下载并自动运行安装向导\n"
                        "· 点【否】：在浏览器打开微软官方下载页面\n"
                        "· 点【取消】：取消操作")
                    .arg(id, msg),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (choice == QMessageBox::Yes) {
                downloadAndRun(QString::fromLatin1(pkg->id), QString::fromLatin1(pkg->directUrl));
            } else if (choice == QMessageBox::No && row && row->url && row->url[0] != 0) {
                Platform::launch(row->url);
            }
        }
        installNext();
    }
}

void RepairPage::downloadAndRun(const QString& title, const QString& url)
{
    if (url.isEmpty())
        return;
    if (m_downloadReply) {
        QMessageBox::information(this, QStringLiteral("ltool"), QStringLiteral("已有安装包正在下载，请等它下载完成。"));
        return;
    }

    QUrl qurl(url);
    QString fileName = QFileInfo(qurl.path()).fileName();
    if (fileName.isEmpty() || !fileName.contains(QLatin1Char('.')))
        fileName = QStringLiteral("installer.exe");
    const QString savePath = QDir::tempPath() + QLatin1Char('/') + fileName;

    m_log->appendPlainText(QString("=== 开始内置下载：%1 ===").arg(title));
    m_log->appendPlainText(QString("来源：%1").arg(url));
    m_log->appendPlainText(QString("保存至：%1").arg(savePath));
    m_status->setText(QString("正在下载 %1…").arg(title));

    auto file = std::make_shared<QFile>(savePath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_log->appendPlainText(QString("!! 创建临时文件失败：%1").arg(savePath));
        return;
    }

    QNetworkRequest req(qurl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    req.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

    m_downloadReply = m_nam->get(req);
    const QPointer<RepairPage> guard(this);

    connect(m_downloadReply, &QNetworkReply::readyRead, this, [guard, file] {
        if (!guard || !guard->m_downloadReply)
            return;
        file->write(guard->m_downloadReply->readAll());
    });

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, [this, guard, title](qint64 received, qint64 total) {
        if (!guard) return;
        if (total > 0) {
            const int pct = int((received * 100) / total);
            m_status->setText(QString("正在下载 %1：%2% (%3MB / %4MB)")
                                  .arg(title)
                                  .arg(pct)
                                  .arg(double(received) / (1024.0 * 1024.0), 0, 'f', 1)
                                  .arg(double(total) / (1024.0 * 1024.0), 0, 'f', 1));
        } else {
            m_status->setText(QString("正在下载 %1：%2MB")
                                  .arg(title)
                                  .arg(double(received) / (1024.0 * 1024.0), 0, 'f', 1));
        }
    });

    connect(m_downloadReply, &QNetworkReply::finished, this, [this, guard, file, savePath, title] {
        file->flush();
        file->close();

        if (!guard)
            return;

        if (m_downloadReply->error() != QNetworkReply::NoError) {
            const QString err = m_downloadReply->errorString();
            m_log->appendPlainText(QString("!! 内置下载失败：%1").arg(err));
            m_status->setText(QString("%1 下载失败，可点击「官网」在浏览器中下载。").arg(title));
            m_downloadReply->deleteLater();
            m_downloadReply = nullptr;
            return;
        }

        m_log->appendPlainText(QString("++ 内置下载完成：%1（%2）").arg(title, savePath));
        m_log->appendPlainText(QStringLiteral("提示：正在启动官方安装向导，请完成安装后点击「重新检测」。"));
        m_status->setText(QString("%1 下载完成，已启动安装向导。").arg(title));
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;

        // 启动安装程序（支持管理员提权请求）
        Platform::launch(savePath);
    });
}
