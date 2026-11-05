#include "UserAccount.h"

#include "Utils.h"

#include <QDir>
#include <QCheckBox>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QLabel>

#include <windows.h>
#include <shellapi.h>

namespace {

// 一次 powershell 把该读的都读回来。全是只读命令。
// 输出统一 UTF-8：PS5.1 默认按控制台代码页出，中文显示名会变乱码，所以第一行先改 OutputEncoding。
const char* kReadScript =
    "[Console]::OutputEncoding=[Text.Encoding]::UTF8\n"
    "$n=$env:USERNAME; $m=$env:COMPUTERNAME\n"
    "\"MACHINE=$m\"; \"NAME=$n\"\n"
    "$u=$null; try{$u=Get-LocalUser -Name $n -ErrorAction Stop}catch{}\n"
    "if($u){\"LOCAL=1\"; \"SID=\"+$u.SID.Value}else{\"LOCAL=0\"; \"SID=\"}\n"
    "$f=''\n"
    "try{$f=([adsi]\"WinNT://$m/$n,user\").FullName}catch{}\n"
    "\"FULL=$f\"\n"
    "$p=''\n"
    "if($u){try{$p=(Get-ItemProperty \"HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\$($u.SID.Value)\" "
    "-Name ProfileImagePath -ErrorAction Stop).ProfileImagePath}catch{}}\n"
    "\"PROFILE=$p\"\n"
    // 微软账户的痕迹：这个键只有登录过 MSA 的机器上才有。只拿来出提示，不据此禁用按钮 ——
    // 它不是权威判据，禁错了就把功能整个锁死在本地账户上。
    "$msa='0'\n"
    "if(Test-Path 'HKCU:\\Software\\Microsoft\\IdentityCRL\\NumberedSignin'){$msa='1'}\n"
    "if(Test-Path 'HKCU:\\Software\\Microsoft\\IdentityCRL\\UserExtendedProperties'){$msa='1'}\n"
    "\"MSA=$msa\"\n";

QString trimmed(const QString& s)
{
    QString t = s;
    while (t.endsWith(QLatin1Char('\r')) || t.endsWith(QLatin1Char('\n')))
        t.chop(1);
    return t.trimmed();
}

} // namespace

namespace UserAccount {

Info read()
{
    Info out;
    QProcess p;
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
              QStringLiteral("-Command"), QString::fromUtf8(kReadScript) });
    if (!p.waitForFinished(15000)) {
        out.error = QStringLiteral("powershell 没在 15 秒内回话");
        return out;
    }
    const QString text = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = trimmed(line.mid(eq + 1));
        if (k == QLatin1String("MACHINE")) out.machine = v;
        else if (k == QLatin1String("NAME")) out.name = v;
        else if (k == QLatin1String("LOCAL")) out.local = (v == QLatin1String("1"));
        else if (k == QLatin1String("SID")) out.sid = v;
        else if (k == QLatin1String("FULL")) out.fullName = v;
        else if (k == QLatin1String("PROFILE")) out.profileDir = v;
        else if (k == QLatin1String("MSA")) out.msaHint = (v == QLatin1String("1"));
    }
    if (out.name.isEmpty()) {
        out.error = QStringLiteral("没读回账户名：%1").arg(trimmed(p.readAllStandardError()));
        return out;
    }
    out.readable = true;
    return out;
}

QString psQuote(const QString& s)
{
    // PowerShell 的单引号串里唯一的转义就是把 ' 写成 ''，别的字符都是字面量 ——
    // 这段是要拿去提权执行的，不能指望调用方记得引号
    QString out = QStringLiteral("'");
    for (const QChar& c : s) {
        if (c == QLatin1Char('\''))
            out += QLatin1String("''");
        else
            out += c;
    }
    return out + QLatin1Char('\'');
}

QStringList reject(const QString& proposed, bool forAccountName)
{
    QStringList why;
    const QString s = proposed.trimmed();
    if (s.isEmpty())
        why << QStringLiteral("名字是空的");
    if (s.contains(QRegularExpression(QStringLiteral("[\\r\\n\\0]"))))
        why << QStringLiteral("名字里有换行");
    // 账户名和显示名都不能带这些：它们是 Windows 的对象名限制，不是我个人偏好
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (s.contains(bad))
        why << QStringLiteral("不能含这些字符：\\\\ / : * ? \" < > |");
    if (forAccountName) {
        if (s.size() > 20)
            why << QStringLiteral("账户名最长 20 个字符（显示名可以长一点）");
        if (s.startsWith(QLatin1Char(' ')) || s.endsWith(QLatin1Char(' ')))
            why << QStringLiteral("账户名首尾不能是空格");
    } else if (s.size() > 64) {
        why << QStringLiteral("显示名别超过 64 个字符");
    }
    return why;
}

QString fullNameScript(const QString& machine, const QString& name, const QString& newFull)
{
    // 显示名 = SAM 里那个 FullName。登录界面、开始菜单头像、锁屏上显示的都是它。
    // 不动账户名，更不动用户目录。
    // ADSI 那串路径是**拼**出来的："WinNT://" + '机' + '/' + '号' + ',user'。
    // 整串塞进双引号里再套一层单引号，单引号就成了名字的一部分
    //（例：WinNT://<机器名>/'<账户名>' 这么拼出来找不到对象 —— 这里刻意不写真机名真账号）
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  $u = [adsi](\"WinNT://\" + %1 + '/' + %2 + ',user')\n"
               "  $u.FullName = %3\n"
               "  $u.SetInfo()\n"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(machine), psQuote(name), psQuote(newFull.trimmed()));
}

QString renameScript(const QString& machine, const QString& name, const QString& newName)
{
    Q_UNUSED(machine);
    // 改的是账户名本身。Rename-LocalUser 不会去动 C:\\Users\\<旧名>，界面上也照样显示旧目录 ——
    // 这条必须在按钮前头跟人说清楚，不能等他自己发现。
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  Rename-LocalUser -Name %1 -NewName %2 -ErrorAction Stop\n"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(name), psQuote(newName.trimmed()));
}

QString newUserScript(const QString& name, const QString& fullName, const QString& password,
                      bool asAdmin)
{
    // 管理员组按 SID 找（S-1-5-32-544），不写死组名：中文系统上那个组的本地化名不一定
    // 长得和英文一致，Translate 一次拿到的才是这台机器上的真名。
    // 不勾管理员就整段不加（不是留一个 if ($false)）—— 脚本里少一行就少一种可能。
    const QString addAdmin = asAdmin
        ? QStringLiteral(
              "  $adm = (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544'))"
              ".Translate([Security.Principal.NTAccount]).Value.Split('\')[-1]\n"
              "  Add-LocalGroupMember -Group $adm -Member %1 -ErrorAction Stop\n")
              .arg(psQuote(name.trimmed()))
        : QString();
    return QStringLiteral(
               "$ErrorActionPreference='Stop'\n"
               "try {\n"
               "  $sp = ConvertTo-SecureString %1 -AsPlainText -Force\n"
               "  New-LocalUser -Name %2 -Password $sp -FullName %3 "
               "-Description 'created by ltool' -PasswordNeverExpires -AccountNeverExpires -ErrorAction Stop\n"
               "%4"
               "  exit 0\n"
               "} catch {\n"
               "  exit 1\n"
               "}\n")
        .arg(psQuote(password), psQuote(name.trimmed()),
             psQuote(fullName.trimmed().isEmpty() ? name.trimmed() : fullName.trimmed()),
             addAdmin);
}

Probe probe(const QString& name)
{
    Probe out;
    QProcess p;
    const QString script = QStringLiteral(
                               "[Console]::OutputEncoding=[Text.Encoding]::UTF8\n"
                               "$n=%1\n"
                               "$u=$null; try{$u=Get-LocalUser -Name $n -ErrorAction Stop}catch{}\n"
                               "if($u){\"EXISTS=1\"; \"FULL=\"+$u.FullName}else{\"EXISTS=0\"}\n"
                               "$adm='0'\n"
                               "if($u){try{$g=(New-Object Security.Principal.SecurityIdentifier("
                               "'S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value"
                               ".Split('\\')[-1]; if(Get-LocalGroupMember -Group $g -Name $n -ErrorAction "
                               "SilentlyContinue){$adm='1'}}catch{}\n"
                               "}\n"
                               "\"ADMIN=$adm\"\n")
                               .arg(psQuote(name));
    p.start(QStringLiteral("powershell"),
            { QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
              QStringLiteral("-Command"), script });
    if (!p.waitForFinished(15000)) {
        out.error = QStringLiteral("powershell 没回话");
        return out;
    }
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = trimmed(line.mid(eq + 1));
        if (k == QLatin1String("EXISTS")) out.exists = (v == QLatin1String("1"));
        else if (k == QLatin1String("ADMIN")) out.admin = (v == QLatin1String("1"));
        else if (k == QLatin1String("FULL")) out.fullName = v;
    }
    out.readable = true;
    return out;
}

bool runElevatedScript(const QString& script, QString* err)
{
    // 落成临时 .ps1 再执行，不走 cmd.exe 拼串：中文和引号在 cmd 那一层会被剥/转义出各种花样，
    // 而这段是要提权跑的，宁可多写一个文件也不要在引号上赌。
    QTemporaryFile tf(QDir::tempPath() + "/ltool-user-XXXXXX.ps1");
    tf.setAutoRemove(false);
    if (!tf.open()) {
        if (err)
            *err = QStringLiteral("临时脚本写不出来");
        return false;
    }
    const QString path = tf.fileName();
    tf.close();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("临时脚本打不开写：%1").arg(path);
        QFile::remove(path);
        return false;
    }
    f.write("\xEF\xBB\xBF");            // PS5.1 认 BOM，没它整段中文会变乱码（实测踩过）
    f.write(script.toUtf8());
    f.close();

    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\"").arg(path);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        if (err)
            *err = e == ERROR_CANCELLED
                ? QStringLiteral("没给管理员授权，什么都没改")
                : QStringLiteral("提权没起来（错误码 %1）").arg(e);
        QFile::remove(path);
        return false;
    }
    bool ok = false;
    DWORD code = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        ok = (code == 0);
    }
    QFile::remove(path);
    if (!ok && err)
        *err = QStringLiteral("脚本跑完但退出码不是 0（%1）—— 多半是没权限或名字被系统拒了").arg(code);
    return ok;
}

} // namespace UserAccount

namespace {

// 每一步停在哪个口子都要留一行日志。这条链后面还有 UAC、脚本、回读三段，
// "日志里有没有 username: 这行"就是分界线 —— 2026-10-17 他报"改用户名失败"，
// 而整份 log 里 username: 一条都没有（同期 linuxdo: 307 行、clipboard: 534 行，
// 说明插件写日志的通路是好的）⇒ 根本没走到提权，但到底是哪个口子拦的没有证据。
void stoppedAt(const QString& action, const QString& gate, const QString& detail = {})
{
    Utils::log(QStringLiteral("username: %1 停在%2%3")
                   .arg(action, gate,
                        detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail));
}

// 摘要行：方案 B 要单列，所以一行摆完（判据认这行里要有「账户名」三个字）。
// SID 太长，截到最后一个连字符段就够认人了；完整 SID 在悬停里。
QString oneLiner(const UserAccount::Info& i)
{
    return QStringLiteral("账户名 %1 @ %2 · %3 · 显示名 %4 · %5")
        .arg(i.name, i.machine,
             i.local ? (i.msaHint ? QStringLiteral("本地账户（另有微软账户登录痕迹）")
                                   : QStringLiteral("本地账户"))
                     : QStringLiteral("SAM 里查不到（多半是微软账户）"),
             i.fullName.isEmpty() ? QStringLiteral("（没设）") : QStringLiteral("「%1」").arg(i.fullName),
             i.profileDir)
        + QStringLiteral(" · SID %1").arg(i.sid.section(QLatin1Char('-'), 0, 4));
}

// 常驻那一句说明。构造摆它、hintArmTexts 预留高度也算它 —— 一句话只许有一份。
QString baseHintText()
{
    return QStringLiteral("修改显示名只动登录界面/开始菜单名字；修改账户名不动 C:\\Users 目录，"
                          "注销重登后生效。操作需管理员授权（弹 UAC）。");
}

} // namespace

UserAccountPage::UserAccountPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // 顶部标题与系统特性标签
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("Windows 账户治理"), this);
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 3);
    tf.setBold(true);
    title->setFont(tf);

    auto* tag1 = new QLabel(QStringLiteral("🛡️ UAC 安全提权"), this);
    tag1->setStyleSheet("color:#ea580c;background:rgba(234,88,12,0.08);border:1px solid rgba(234,88,12,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag2 = new QLabel(QStringLiteral("⚡ 两段就地确认"), this);
    tag2->setStyleSheet("color:#059669;background:rgba(5,150,105,0.08);border:1px solid rgba(5,150,105,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    auto* tag3 = new QLabel(QStringLiteral("🔒 密码安全即焚"), this);
    tag3->setStyleSheet("color:#7c3aed;background:rgba(124,58,237,0.08);border:1px solid rgba(124,58,237,0.22);border-radius:4px;padding:2px 7px;font-size:11px;font-weight:500;");

    titleRow->addWidget(title);
    titleRow->addWidget(tag1);
    titleRow->addWidget(tag2);
    titleRow->addWidget(tag3);
    titleRow->addStretch(1);
    lay->addLayout(titleRow);

    auto* tip = new QLabel(QStringLiteral(
        "查看当前登录账户属性（本地/微软账户、SID 标识与 Profile 目录），支持快捷修改显示名、登录名或新建本地用户。"), this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    lay->addWidget(tip);

    // ---- 形状：方案 B（单列到底，动作一律在行尾）----
    // 摘要行 + 行尾「重读」
    auto* head = new QHBoxLayout;
    head->setSpacing(8);
    m_state = new QLabel(this);
    m_state->setObjectName(QStringLiteral("accountSummary"));
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 摘要里那一串是**机器相关**的（账户名、计算机名、C:\Users 路径、SID）。
    // 不许换行时它的 minimumSizeHint 就把整页的最小宽度顶成那个长度 ——
    // 本机 1194、CI 那台 607，同一个判据两边量的根本不是同一个版面。
    m_state->setWordWrap(true);
    head->addWidget(m_state, 1);
    auto* reload = new QPushButton(QStringLiteral("重读"), this);
    reload->setObjectName(QStringLiteral("reload"));
    reload->setCursor(Qt::PointingHandCursor);
    reload->setFixedHeight(32);
    head->addWidget(reload);
    lay->addLayout(head);

    auto* rowFull = new QHBoxLayout;
    rowFull->setSpacing(8);
    auto* lblFull = new QLabel(QStringLiteral("显示名"), this);
    lblFull->setFixedWidth(54);
    lblFull->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lblFull->setStyleSheet("color:#64748b;font-size:12px;font-weight:500;");
    m_full = new QLineEdit(this);
    m_full->setObjectName(QStringLiteral("fullInput"));
    m_full->setFixedHeight(32);
    m_full->setPlaceholderText(QStringLiteral("修改显示名称（登录界面与锁屏显示）"));
    m_applyFull = new QPushButton(QStringLiteral("改显示名"), this);
    m_applyFull->setObjectName(QStringLiteral("applyFull"));
    m_applyFull->setCursor(Qt::PointingHandCursor);
    m_applyFull->setFixedHeight(32);
    m_applyFull->setMinimumWidth(80);
    rowFull->addWidget(lblFull);
    rowFull->addWidget(m_full, 1);
    rowFull->addWidget(m_applyFull);
    lay->addLayout(rowFull);

    auto* rowName = new QHBoxLayout;
    rowName->setSpacing(8);
    auto* lblName = new QLabel(QStringLiteral("登录名"), this);
    lblName->setFixedWidth(54);
    lblName->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lblName->setStyleSheet("color:#64748b;font-size:12px;font-weight:500;");
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("nameInput"));
    m_name->setFixedHeight(32);
    m_name->setPlaceholderText(QStringLiteral("修改登录账户名（最长 20 字符，不改动用户目录）"));
    m_applyName = new QPushButton(QStringLiteral("改账户名"), this);
    m_applyName->setObjectName(QStringLiteral("applyName"));
    m_applyName->setCursor(Qt::PointingHandCursor);
    m_applyName->setFixedHeight(32);
    m_applyName->setMinimumWidth(80);
    rowName->addWidget(lblName);
    rowName->addWidget(m_name, 1);
    rowName->addWidget(m_applyName);
    lay->addLayout(rowName);

    // 「改自己」和「新建账户」是两种性质的事，中间要有一条看得见的界
    auto* gap = new QFrame(this);
    gap->setFrameShape(QFrame::HLine);
    gap->setFrameShadow(QFrame::Plain);
    gap->setStyleSheet("color:rgba(127,127,127,0.20);");
    lay->addWidget(gap);

    auto* rowNew = new QHBoxLayout;
    rowNew->setSpacing(8);
    auto* lblNew = new QLabel(QStringLiteral("新建"), this);
    lblNew->setFixedWidth(54);
    lblNew->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lblNew->setStyleSheet("color:#64748b;font-size:12px;font-weight:500;");
    m_newName = new QLineEdit(this);
    m_newName->setObjectName(QStringLiteral("newName"));
    m_newName->setFixedHeight(32);
    m_newName->setPlaceholderText(QStringLiteral("新账户名 (≤20字)"));
    m_newName->setToolTip(QStringLiteral("新本地用户的登录账户名，最长 20 个字符"));
    m_newFull = new QLineEdit(this);
    m_newFull->setObjectName(QStringLiteral("newFull"));
    m_newFull->setFixedHeight(32);
    m_newFull->setPlaceholderText(QStringLiteral("显示名 (选填)"));
    m_newFull->setToolTip(QStringLiteral("新用户的显示名称，留空则默认使用登录名"));
    m_newPass = new QLineEdit(this);
    m_newPass->setObjectName(QStringLiteral("newPass"));
    m_newPass->setFixedHeight(32);
    m_newPass->setEchoMode(QLineEdit::Password);
    m_newPass->setPlaceholderText(QStringLiteral("登录密码 (必填)"));
    m_newPass->setToolTip(QStringLiteral("新用户的登录密码（必填）"));
    m_create = new QPushButton(QStringLiteral("新建用户"), this);
    m_create->setObjectName(QStringLiteral("applyCreate"));
    m_create->setCursor(Qt::PointingHandCursor);
    m_create->setFixedHeight(32);
    m_create->setMinimumWidth(80);
    rowNew->addWidget(lblNew);
    rowNew->addWidget(m_newName, 2);
    rowNew->addWidget(m_newFull, 2);
    rowNew->addWidget(m_newPass, 2);
    rowNew->addWidget(m_create);
    lay->addLayout(rowNew);

    auto* adminRow = new QHBoxLayout;
    adminRow->setSpacing(8);
    auto* lblSpacer = new QLabel(this);
    lblSpacer->setFixedWidth(54);
    m_newAdmin = new QCheckBox(QStringLiteral("授予此新账户管理员权限（加入 Administrators 组）"), this);
    m_newAdmin->setObjectName(QStringLiteral("newAdmin"));
    m_newAdmin->setCursor(Qt::PointingHandCursor);
    m_newAdmin->setChecked(true);
    adminRow->addWidget(lblSpacer);
    adminRow->addWidget(m_newAdmin, 1);
    lay->addLayout(adminRow);

    m_armTimer = new QTimer(this);
    m_armTimer->setSingleShot(true);
    connect(m_armTimer, &QTimer::timeout, this, [this] {
        if (!m_armed)
            return;
        Utils::log(QStringLiteral("username: %1 六秒没再点，确认收回").arg(m_armed->objectName()));
        disarm();
        m_hint->setText(QStringLiteral("没动手：六秒内没再点第二下，确认已经收回。"));
    });

    auto* hintRow = new QHBoxLayout;
    hintRow->setSpacing(8);
    auto* hintIcon = new QLabel(QStringLiteral("💡"), this);
    hintIcon->setAlignment(Qt::AlignTop);
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("hintText"));
    m_hint->setWordWrap(true);
    m_hint->setText(baseHintText());
    hintRow->addWidget(hintIcon);
    hintRow->addWidget(m_hint, 1);
    lay->addLayout(hintRow);
    lay->addStretch(1);

    setStyleSheet(
        "QPushButton#applyFull, QPushButton#applyName{"
        "  padding:5px 14px;border-radius:6px;font-size:12px;font-weight:500;"
        "}"
        "QPushButton#applyCreate{"
        "  padding:5px 14px;border-radius:6px;font-size:12px;font-weight:600;"
        "  background:rgba(120,90,200,0.12);border:1px solid rgba(120,90,200,0.30);color:#7c3aed;"
        "}"
        "QPushButton#applyCreate:hover{"
        "  background:rgba(120,90,200,0.22);"
        "}"
        "QPushButton#reload{"
        "  padding:5px 12px;border-radius:6px;font-size:12px;"
        "}"
        "QPushButton[armed=\"true\"]{"
        "  background:#ea580c !important;color:#ffffff !important;"
        "  border:1px solid #c2410c !important;font-weight:600 !important;"
        "}"
        "QPushButton[armed=\"true\"]:hover{"
        "  background:#f97316 !important;"
        "}"
        "QLineEdit{"
        "  border:1px solid rgba(127,127,127,0.25);border-radius:6px;"
        "  padding:4px 8px;font-size:12px;"
        "}"
        "QLineEdit:focus{"
        "  border:1px solid #0284c7;"
        "}"
        "QLabel#accountSummary{"
        "  background:rgba(127,127,127,0.06);border:1px solid rgba(127,127,127,0.18);"
        "  border-radius:6px;padding:6px 10px;font-size:12px;font-family:Consolas, 'Segoe UI', monospace;"
        "}"
        "QLabel#hintText{"
        "  color:#64748b;background:rgba(127,127,127,0.05);border:1px dashed rgba(127,127,127,0.20);"
        "  border-radius:6px;padding:6px 10px;font-size:12px;line-height:1.4;"
        "}");

    connect(m_applyFull, &QPushButton::clicked, this, &UserAccountPage::applyFullName);
    connect(m_applyName, &QPushButton::clicked, this, &UserAccountPage::applyAccountName);
    connect(m_create, &QPushButton::clicked, this, &UserAccountPage::createNewUser);
    connect(reload, &QPushButton::clicked, this, &UserAccountPage::refresh);
    reserveHintHeight();     // 先按常驻那句钉好高度；读到账户后 applyInfo 会再钉一次
    refresh();
}

UserAccountPage::~UserAccountPage()
{
    if (m_watcher)
        m_watcher->disconnect(this);
    if (m_armTimer)
        m_armTimer->stop();
}

void UserAccountPage::refresh()
{
    // 读一次要走 powershell（半秒到一秒），所以放在后台线程；页面先给一句"正在读"，
    // 不能让人对着一个空白页猜它在不在干活
    m_state->setText(QStringLiteral("正在读账户信息…"));
    m_applyFull->setEnabled(false);
    m_applyName->setEnabled(false);
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<UserAccount::Info>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            applyInfo(static_cast<QFutureWatcher<UserAccount::Info>*>(m_watcher)->result());
        });
    }
    static_cast<QFutureWatcher<UserAccount::Info>*>(m_watcher)
        ->setFuture(QtConcurrent::run(UserAccount::read));
}

void UserAccountPage::applyInfo(const UserAccount::Info& got)
{
    disarm();
    m_info = got;
    if (!m_info.readable) {
        m_state->setText(QStringLiteral("读不到账户信息：%1").arg(m_info.error));
        m_applyFull->setEnabled(false);
        m_applyName->setEnabled(false);
        return;
    }
    m_state->setText(oneLiner(m_info));
    m_state->setToolTip(m_info.sid);            // 摘要行只放截过的 SID，完整那串在悬停里
    m_full->setText(m_info.fullName);
    m_name->setText(m_info.name);
    m_applyFull->setEnabled(true);
    // 微软账户在 SAM 里查不到，Rename-LocalUser 对它没意义：那一颗直接不给按，
    // 并在旁边说清去哪儿改（不是把失败藏到按下之后）
    m_applyName->setEnabled(m_info.local);
    // 风险说明里嵌着真机名/真账户名，长度每台机器不一样 —— 所以读到之后按**真那几句**
    // 再钉一次。钉子只在"数据到齐"时挪一次，绝不在按下按钮时挪（判据认的是后者）。
    reserveHintHeight();
}

// 就地两段确认：第一下把同一颗按钮变成「确认…」并亮 6 秒，第二下才真动手。
//
// 为什么不用模态确认框（这是这次的实际修法，不只是审美）：这页是从启动器里叫出来的，
// 启动器一失焦就把自己藏起来，挂在它里面的模态框跟着看不见，答案算「不是」——
// 实测整份日志里 username: 一条都没有（同期 linuxdo: 307 行、clipboard: 534 行，
// 说明插件写日志的通路是好的）：三颗按钮按下去哪儿也没去。
//
// 按钮在行尾，文字变长只会往左挤输入框：行高不变、块数不变（他否过"点一下突然多出一块"）。
bool UserAccountPage::armOrRun(QPushButton* btn, const QString& confirmText, const QString& risk)
{
    if (m_armed == btn) {
        disarm();
        return true;
    }
    disarm();
    m_armedText = btn->text();
    m_armed = btn;
    btn->setText(confirmText);
    btn->setProperty("armed", true);
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
    m_hint->setText(risk);
    m_armTimer->start(6000);
    Utils::log(QStringLiteral("username: %1 摆出两段确认（再点一下才动手）").arg(btn->objectName()));
    return false;
}

void UserAccountPage::disarm()
{
    if (!m_armed)
        return;
    m_armed->setText(m_armedText);
    m_armed->setProperty("armed", false);
    m_armed->style()->unpolish(m_armed);
    m_armed->style()->polish(m_armed);
    m_armed = nullptr;
    m_armTimer->stop();
}

// 校验没过就写在同一行说明上，不再弹框：弹框在这个页上是会被隐藏的启动器吞掉的那一类东西
void UserAccountPage::refused(const QString& action, const QStringList& why)
{
    stoppedAt(action, "校验", why.join(QStringLiteral("；")));
    m_hint->setText(QStringLiteral("%1 没动：%2").arg(action, why.join(QStringLiteral("；"))));
}

void UserAccountPage::applyFullName()
{
    const QString want = m_full->text();
    if (want.trimmed() == m_info.fullName) {
        stoppedAt("改显示名", "名字没变");
        m_hint->setText(QStringLiteral("显示名没变，什么都没做。"));
        return;
    }
    const QStringList why = UserAccount::reject(want, false);
    if (!why.isEmpty()) {
        refused(QStringLiteral("改显示名"), why);
        return;
    }
    if (!armOrRun(m_applyFull, QStringLiteral("确认改成这个"), riskFullText()))
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::fullNameScript(m_info.machine, m_info.name, want), &err);
    refresh();                                    // 回读：读到什么就说什么，不拿"跑了"当"改了"
    const bool now = (m_info.fullName == want.trimmed());
    m_hint->setText(now ? QStringLiteral("显示名已改成「%1」（回读确认）").arg(m_info.fullName)
                        : QStringLiteral("没改成：回读到的还是「%1」。%2")
                              .arg(m_info.fullName.isEmpty() ? m_info.name : m_info.fullName,
                                   ran ? QStringLiteral("脚本本身跑成功了，但值没变 —— "
                                                        "如果这是微软账户，请去 account.microsoft.com 改")
                                       : err));
    Utils::log(QStringLiteral("username: 改显示名 ok=%1 now=%2 err=%3")
                   .arg(now ? 1 : 0).arg(m_info.fullName, ran ? QStringLiteral("ok") : err));
}

void UserAccountPage::applyAccountName()
{
    const QString want = m_name->text();
    if (want.trimmed() == m_info.name) {
        stoppedAt("改账户名", "名字没变");
        m_hint->setText(QStringLiteral("账户名没变，什么都没做。"));
        return;
    }
    const QStringList why = UserAccount::reject(want, true);
    if (!why.isEmpty()) {
        refused(QStringLiteral("改账户名"), why);
        return;
    }
    if (!armOrRun(m_applyName, QStringLiteral("确认改名"), riskNameText(m_info.name)))
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::renameScript(m_info.machine, m_info.name, want), &err);
    const QString before = m_info.name;
    refresh();
    const bool now = (m_info.name == want.trimmed());
    m_hint->setText(now ? QStringLiteral("账户名已改成「%1」（回读确认）。注销再登录后全面生效；"
                                         "C:\\Users\\%2 那个目录还是旧名字。")
                              .arg(m_info.name, before)
                        : QStringLiteral("没改成：现在读到的是「%1」。%2")
                              .arg(m_info.name, ran ? QStringLiteral("脚本跑成功了但名字没变") : err));
    Utils::log(QStringLiteral("username: 改账户名 %1→%2 ok=%3 err=%4")
                   .arg(before, want.trimmed()).arg(now ? 1 : 0)
                   .arg(ran ? QStringLiteral("ok") : err));
}

void UserAccountPage::createNewUser()
{
    const QString name = m_newName->text().trimmed();
    const QString full = m_newFull->text().trimmed();
    const QString pass = m_newPass->text();
    QStringList why = UserAccount::reject(name, true);
    if (pass.isEmpty())
        why << QStringLiteral("密码不能空着 —— 空密码的本地账户根本登不进去，还不如不建");
    if (why.isEmpty() && UserAccount::probe(name).exists)
        why << QStringLiteral("账户 %1 已经存在了").arg(name);
    if (!why.isEmpty()) {
        refused(QStringLiteral("新建用户"), why);
        return;
    }
    const bool admin = m_newAdmin->isChecked();
    if (!armOrRun(m_create, QStringLiteral("确认创建"), riskCreateText(m_info.machine, name, admin)))
        return;
    QString err;
    const bool ran = UserAccount::runElevatedScript(
        UserAccount::newUserScript(name, full, pass, admin), &err);
    const UserAccount::Probe after = UserAccount::probe(name);   // 回读，不拿"跑了"当"建了"
    m_hint->setText(after.exists
        ? QStringLiteral("已建好「%1」：显示名=%2，管理员=%3。注销一下就能在登录列表里看到它。")
              .arg(name, after.fullName.isEmpty() ? name : after.fullName,
                   after.admin ? QStringLiteral("是") : QStringLiteral("否"))
        : QStringLiteral("没建成：%1").arg(ran ? QStringLiteral("脚本说成功了，但回读不到这个账户") : err));
    m_newPass->clear();
    Utils::log(QStringLiteral("username: 新建用户 %1 admin=%2 ok=%3 err=%4")
                   .arg(name).arg(admin ? 1 : 0).arg(after.exists ? 1 : 0)
                   .arg(ran ? QStringLiteral("ok") : err));
}

// ---- 说明行那块高度：按「第一下能摆出来的每一句」预留 ----
// 为什么要在结构上钉死：这一行以前跟着自己那一句的换行长短走。风险说明比常驻那句长，
// 一多折一行，整页就进入"装不下"的状态，上面每一行（改显示名/新建登录名/管理员）全被重排。
// 他把"点一下才把版面重排"砍过一次；这条在我这台宽版面上永远差不到那一行，
// 是 CI 那台窄机器（607）先红出来的。所以判据拿的是同一批句子，不是我另数一份。
QString UserAccountPage::riskFullText() const
{
    return QStringLiteral("第二下才会提交：只改登录界面/开始菜单显示的名字，"
                          "不动账户名，也不动 C:\Users 目录。要管理员授权。");
}

QString UserAccountPage::riskNameText(const QString& who) const
{
    return QStringLiteral("第二下才会提交：C:\Users\%1 那个目录不会跟着改名，写死了旧路径的"
                          "程序也不会自己跟上；改完要注销再登录才全面生效。要管理员授权。").arg(who);
}

QString UserAccountPage::riskCreateText(const QString& machine, const QString& who, bool admin) const
{
    return QStringLiteral("第二下才会真建：在本机 %1 上建本地账户「%2」%3；"
                          "它第一次登录时 Windows 会给它建一个新的 C:\Users 目录；"
                          "密码会临时写进一个脚本文件、跑完立刻删 —— 那一瞬间它是在盘上的。")
        .arg(machine, who,
             admin ? QStringLiteral("并加入 Administrators 组（那是完整的管理员权限）")
                   : QStringLiteral("（普通用户，不给管理员）"));
}

QStringList UserAccountPage::hintArmTexts() const
{
    // 账户名那一格的上限是 reject() 定的 20 个字（按下之前就先过那一关，所以这是个有界的数）；
    // 机名那一格同样拿 20 占位（NetBIOS 上限 15）。拿占位而不是真值，是为了"要留几行"
    // 在每棵机器上都算出同一个数 —— 这台和 CI 那台以前差就差在这儿。
    const QString probe = QStringLiteral("n").repeated(20);
    return QStringList{ baseHintText(),
                        riskFullText(),
                        riskNameText(probe),
                        riskCreateText(probe, probe, true),
                        riskCreateText(probe, probe, false) };
}

void UserAccountPage::reserveHintHeight()
{
    if (!m_hint || m_pinningHint)
        return;
    m_pinningHint = true;
    // 先让版面排开再读宽度：resizeEvent 里那颗标签的几何还是上一轮的，
    // 拿那个宽度去 heightForWidth 会算出"每行一个字"的假高度（第一次就留过 376px 的空块）。
    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }
    const int w = m_hint->width() > 1 ? m_hint->width() : m_hint->sizeHint().width();
    const QString shown = m_hint->text();
    int need = m_hint->sizeHint().height();
    for (const QString& t : hintArmTexts()) {
        m_hint->setText(t);
        need = qMax(need, m_hint->heightForWidth(w));
    }
    m_hint->setText(shown);
    if (m_hint->minimumHeight() != need)
        m_hint->setFixedHeight(need);
    m_pinningHint = false;
}

void UserAccountPage::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    reserveHintHeight();
}
