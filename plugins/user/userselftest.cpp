// 「Windows 用户名」这一页的判据。
//
// 为什么在插件里而不是 main.cpp：页面类 UserAccountPage 和 UserAccount 那几个拼脚本的函数
// 现在都是这个插件的内部实现，主程序链接不到它们。宿主通过壳里那个 extern "C" 的
// ltoolPluginSelftest 把这段跑一遍（见 src/PluginHost.h 的 selftestEntry）。
#include "UserAccount.h"

#include <QCoreApplication>
#include <QApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QSet>
#include <QString>
#include <QThread>
#include <QWidget>

#include <cstdio>
#include <functional>

int userPageSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    const auto has = [&bad](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++bad;
    };
    using namespace UserAccount;

    // ---- 引号：这段是要提权执行的，逃逸口必须没有 ----
    check("psQuote 把单引号翻倍", psQuote(QStringLiteral("a'b")), QStringLiteral("'a''b'"));
    check("psQuote 包住整串", psQuote(QStringLiteral("x")).left(1) + psQuote(QStringLiteral("x")).right(1),
          QStringLiteral("''"));
    const QString evil = QStringLiteral("x'; Add-LocalGroupMember -Group 'Administrators'; #");
    const QString quoted = psQuote(evil);
    has("psQuote 对注入串：首尾各一个引号、里面没有裸单引号",
        quoted.startsWith(QLatin1Char('\'')) && quoted.endsWith(QLatin1Char('\''))
            && [&quoted] {
                const QString inner = quoted.mid(1, quoted.size() - 2);
                for (int i = 0; i < inner.size(); ++i) {
                    if (inner.at(i) != QLatin1Char('\''))
                        continue;
                    // 单引号只能成对出现；下一格不是引号就是逃逸口
                    if (i + 1 >= inner.size() || inner.at(i + 1) != QLatin1Char('\''))
                        return false;
                    ++i;
                }
                return true;
            }());
    std::printf("INFO 逃逸尝试拼出来的是：%s\n", quoted.toUtf8().constData());

    // ---- 名字合法性 ----
    has("空名被拒", !reject(QStringLiteral("   "), true).isEmpty());
    has("带斜杠被拒", !reject(QStringLiteral("a/b"), true).isEmpty());
    has("带引号被拒", !reject(QStringLiteral("a\"b"), true).isEmpty());
    has("带换行被拒", !reject(QString::fromUtf8("a") + QChar(10) + QString::fromUtf8("b"), true).isEmpty());
    has("账户名 20 字通过", reject(QString(20, QLatin1Char('a')), true).isEmpty());
    has("账户名 21 字被拒", !reject(QString(21, QLatin1Char('a')), true).isEmpty());
    has("显示名可以长一点（40 字通过）", reject(QString(40, QLatin1Char('a')), false).isEmpty());
    has("中文显示名通过", reject(QString::fromUtf8("张三"), false).isEmpty());

    // ---- 拼出去的脚本 ----
    const QString fs = fullNameScript(QStringLiteral("MACH"), QStringLiteral("usr"),
                                      QString::fromUtf8("新名字"));
    has("改显示名：ADSI 路径是拼出来的（单引号不会变成名字的一部分）",
        fs.contains(QStringLiteral("\"WinNT://\" + 'MACH' + '/' + 'usr' + ',user'")));
    has("改显示名：新值作为单引号字面量传进去", fs.contains(QStringLiteral("$u.FullName = '")
                                                         + QString::fromUtf8("新名字") + QStringLiteral("'")));
    has("改显示名：SetInfo 在", fs.contains(QStringLiteral("SetInfo")));
    const QString ns1 = newUserScript(QStringLiteral("newbie"), QString(), QStringLiteral("p'w"), true);
    has("新建用户：New-LocalUser 在", ns1.contains(QStringLiteral("New-LocalUser -Name 'newbie'")));
    has("新建用户：管理员组按 SID 找（不写死组名）", ns1.contains(QStringLiteral("S-1-5-32-544"))
        && ns1.contains(QStringLiteral("Add-LocalGroupMember")));
    has("新建用户：密码里的单引号也翻倍", ns1.contains(QStringLiteral("'p''w'")));
    const QString ns0 = newUserScript(QStringLiteral("newbie"), QString(),
                                      QStringLiteral("pw"), false);
    has("新建用户：不勾管理员就不加组", !ns0.contains(QStringLiteral("Add-LocalGroupMember")));
    has("新建用户：留空显示名时拿账户名顶上", ns0.contains(QStringLiteral("-FullName 'newbie'")));
    has("改账户名：走 Rename-LocalUser",
        renameScript(QStringLiteral("M"), QStringLiteral("old"), QStringLiteral("new"))
            .contains(QStringLiteral("Rename-LocalUser -Name 'old' -NewName 'new'")));

    // ---- 真实读取（只读）----
    const Info me = read();
    has("读得到当前账户", me.readable);
    std::printf("INFO 当前：%s@%s 本地=%d 显示名=[%s] 目录=%s\n", me.name.toUtf8().constData(),
                me.machine.toUtf8().constData(), me.local ? 1 : 0,
                me.fullName.toUtf8().constData(), me.profileDir.toUtf8().constData());
    has("账户名非空", !me.name.isEmpty());
    has("SID 长得对", me.sid.startsWith(QStringLiteral("S-1-")));
    has("用户目录真的在盘上", !me.profileDir.isEmpty() && QFileInfo::exists(me.profileDir));
    const Probe ghost = probe(QStringLiteral("ltool-no-such-user-zz"));
    has("查一个不存在的账户：可读、且不存在", ghost.readable && !ghost.exists);

    // ---- 页面：三颗按钮都得摆在布局里（数 findChild 不等于界面可见，配置页栽过）----
    UserAccountPage page;
    QSet<const QWidget*> laidOut;
    std::function<void(QLayout*)> walk = [&](QLayout* l) {
        if (!l)
            return;
        for (int i = 0; i < l->count(); ++i) {
            QLayoutItem* it = l->itemAt(i);
            if (!it)
                continue;
            if (QWidget* w = it->widget()) {
                laidOut.insert(w);
                walk(w->layout());
            } else if (QLayout* sub = it->layout())
                walk(sub);
        }
    };
    walk(page.layout());
    int onLayout = 0;
    for (const char* objName : { "applyFull", "applyName", "applyCreate" }) {
        QWidget* w = page.findChild<QWidget*>(QString::fromLatin1(objName));
        if (w && laidOut.contains(w))
            ++onLayout;
    }
    check("改显示名/改账户名/新建用户 三颗按钮都在布局里", QString::number(onLayout),
          QStringLiteral("3"));
    // 后台那次读取要真等到回包，别拿"正在读"当状态。
    // 必须按 objectName("accountSummary") 找状态行，并在量 yFull 坐标之前真正等按钮放开：
    // 以前遍历所有 QLabel 找「账户名」三个字，结果 120*20ms=2.4s 在 CI runner 上不够 powershell
    // 跑完时，底部常驻说明 m_hint（「修改账户名不动…」）被误当成状态行假绿混过去，
    // 导致 167 行量 yFull 时 m_state 还是单行的「正在读账户信息…」（yFull=109，页高 343），
    // 等到后面再等按钮放开时 applyInfo 才填上两行真摘要，武装后重排变成 yFull=127（+18px）。
    QLineEdit* nameIn = page.findChild<QLineEdit*>(QString::fromLatin1("nameInput"));
    QPushButton* nameBtn = page.findChild<QPushButton*>(QString::fromLatin1("applyName"));
    QPushButton* fullBtn = page.findChild<QPushButton*>(QString::fromLatin1("applyFull"));
    QLabel* summaryLbl = page.findChild<QLabel*>(QString::fromLatin1("accountSummary"));
    for (int i = 0; i < 600; ++i) {
        QCoreApplication::processEvents();
        if ((fullBtn && fullBtn->isEnabled())
            || (summaryLbl && !summaryLbl->text().contains(QStringLiteral("正在读"))))
            break;
        QThread::msleep(20);
    }
    const bool ready = nameBtn && nameBtn->isEnabled();
    const QString state = summaryLbl ? summaryLbl->text() : QString();
    has("页面读完之后状态行填上了（不是停在「正在读」）",
        state.contains(QStringLiteral("账户名")) && !state.contains(QStringLiteral("正在读")));

    // ---- 形状（他挑的方案 B）：单列到底、动作一律在行尾、第一下不弹框也不把上面挪走 ----
    int rowEndActions = 0;
    for (QHBoxLayout* r : page.findChildren<QHBoxLayout*>()) {
        if (r->count() == 0)
            continue;
        QLayoutItem* end = r->itemAt(r->count() - 1);
        const QPushButton* b = qobject_cast<const QPushButton*>(end ? end->widget() : nullptr);
        if (b && (b->objectName() == QLatin1String("applyFull")
                  || b->objectName() == QLatin1String("applyName")
                  || b->objectName() == QLatin1String("applyCreate")
                  || b->objectName() == QLatin1String("reload")))
            ++rowEndActions;
    }
    check("四个动作都在自己那行的末尾（改显示名/改账户名/新建/重读）",
          QString::number(rowEndActions), QStringLiteral("4"));
    // 只断言"一行摆完"这一件形状上的事：内容里有几个字段是机器相关的（本地/微软账户），
    // 拿它当期望值会在 CI 那台_runner_上红得莫名其妙
    has("摘要行是一行摆完的（方案 B 不竖着列六行）", !state.contains(QChar(10)));

    // 几何断言：先把版面撑开量一遍，再点"改账户名"的第一下，上面那些一颗都不许挪。
    // 这里只点一下（摆出确认），绝不会点第二下 —— 第二下会真去提权弹 UAC，那不可逆。
    page.ensurePolished();
    page.resize(560, 320);
    // 挤压态（这页装不下 320）里所有标签都被压成同一个高度，几何判据就成了空转 ——
    // 量之前先把版面还给它能显示的那块。这个坑是"把钉子拆掉判据却照样绿"那次照出来的。
    if (page.layout() && page.layout()->hasHeightForWidth()) {
        const int needH = page.layout()->totalHeightForWidth(page.width());
        if (needH > page.height())
            page.resize(page.width(), needH);
    }
    if (page.layout()) {
        page.layout()->invalidate();
        page.layout()->activate();
    }
    const auto yOf = [&page](const char* objName) {
        QWidget* w = page.findChild<QWidget*>(QString::fromLatin1(objName));
        return w ? w->y() : -1;
    };
    const int yFull = yOf("applyFull"), yGap = yOf("newName"), yAdmin = yOf("newAdmin");
    std::printf("INFO 量到的行位置：改显示名=%d 新建登录名=%d 管理员那行=%d（页面 %dx%d）\n",
                yFull, yGap, yAdmin, page.width(), page.height());
    // 先证明"版面对我露出过真坐标"：三个 y 必须是互不相同的正数。
    // 不钉这一条的话，布局没排开时三个 y 全是 0，下面那条"没挪走"就在 0==0 上空转（装饰性的绿）
    has("离屏版面真的排开了（三行的 y 是互不相同的正数）",
        yFull > 0 && yGap > yFull && yAdmin > yGap);
    if (!ready) {
        std::printf("SKIP 就地两段确认那 4 条：页面 10 秒内没读到账户信息（按钮还在禁用态），"
                    "对着禁用按钮 click() 量不到任何东西\n");
        check("读不到账户时两颗按钮是禁用态（不许按）",
              nameBtn && !nameBtn->isEnabled() ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
    }
    const bool armed = nameIn && nameBtn && ready;
    if (armed) {
        const QString orig = nameBtn->text();
        // 拿读到的真账户名改一个字母：既要和现在不一样（才不会被"没变"那条挡回来），
        // 又要过得去校验（长度 ≤20、没有那些非法字符）
        nameIn->setText(me.name.left(10) + QStringLiteral("z"));
        nameBtn->click();                                              // 程序内触发，不碰他的鼠标键盘
        check("点第一下只摆出确认（按钮换了字，没动手）",
              nameBtn->text() != orig ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        check("确认说明写在页内那一行上（不是弹出来的框）",
              QApplication::activeModalWidget() == nullptr ? QStringLiteral("yes")
                                                           : QStringLiteral("no"),
              QStringLiteral("yes"));
        // 点了之后必须先让布局重排再读坐标 —— 不重排读到的是旧几何，
        // 那条判据就会在"永远相等"上空转（2026-10-17 我就是这样被骗过一次，
        // 拿"点一下插一块进去"的破坏试验才照出来）
        for (int i = 0; i < 10; ++i) {
            page.layout()->invalidate();
            page.layout()->activate();
            QCoreApplication::processEvents();
        }
        // 前后坐标都打出来：这条以前只报 yes/no，CI 红了一次而本机全绿，
        // 没有数字就看不出"挪走的是哪一行、挪了几像素"，只能再等一轮 50 分钟的流水线。
        const int yFull2 = yOf("applyFull"), yGap2 = yOf("newName"), yAdmin2 = yOf("newAdmin");
        QLabel* hint = page.findChild<QLabel*>(QString::fromLatin1("hintText"));
        std::printf("INFO 武装后：改显示名=%d 新建登录名=%d 管理员那行=%d"
                    "（增量 %+d/%+d/%+d，说明行高=%d，页面 %dx%d）\n",
                    yFull2, yGap2, yAdmin2, yFull2 - yFull, yGap2 - yGap, yAdmin2 - yAdmin,
                    hint ? hint->height() : -1, page.width(), page.height());
        check("第一下不许把上面任何一颗挪走（改显示名/新建/管理员行 y 不变）",
              (yFull2 == yFull && yGap2 == yGap && yAdmin2 == yAdmin)
                  ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        // 武装只留一颗：再点另一颗，前一颗必须收回原字
        QPushButton* fullBtn = page.findChild<QPushButton*>(QString::fromLatin1("applyFull"));
        if (fullBtn) {
            QLineEdit* fullIn = page.findChild<QLineEdit*>(QString::fromLatin1("fullInput"));
            if (fullIn)
                fullIn->setText(QString::fromUtf8("对照名"));
            const QString origFull = fullBtn->text();
            fullBtn->click();
            check("武装只留一颗（点另一颗会把前一颗收回原字）",
                  nameBtn->text() == orig ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            // **绝不允许对同一颗按钮点第二下**：第二下就是"确认"，会真去提权改账户。
            // 2026-10-17 我犯过一次 —— 以为在"收回"，实际把「改显示名」提交了
            //（他账户幸运地以退出码 1 失败、没被改动）。收回只能靠点另一颗：
            // 这里点回 applyName，它把 applyFull 收回、自己武装起来，测试到此为止。
            nameIn->setText(me.name.left(9) + QStringLiteral("zz"));
            nameBtn->click();
            // 收尾这条量的是"同时只有一颗在武装"：点回 applyName 之后 applyFull 收回、
            // applyName 武装 —— 安全靠的是结构（这段里没有一处对同一颗点第二下），
            // 不是靠这条断言，所以它写得很明白，别把它当成防线
            check("收尾：前一颗收回原字、同时只有一颗停在武装态",
                  (fullBtn->text() == origFull && nameBtn->text() != orig)
                      ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
        }
    }
    // ---- 说明行那块高度不随文字长短变（上面那条"第一下不许挪走"的根子就在这儿）----
    // 武装那一句比常驻说明长短不一，而这一行以前把 sizeHint/换行高度交给自己 ——
    // 于是"多折一行"就把上面每一行都挪走。本机版面宽（摘要不许换行时是 1194）
    // 怎么都不跨那一行的边界，CI 那台窄（607）正好跨过去：红在流水线、绿在我桌上。
    // 这两条量的是"决定会不会重排的那个输入"（说明行自己占多高），不是重排的结果。
    QLabel* probe = page.findChild<QLabel*>(QString::fromLatin1("hintText"));
    const int probeW = probe ? probe->width() : 0;
    const QStringList armTexts = page.hintArmTexts();
    if (probe && probeW > 50 && !armTexts.isEmpty()) {
        const QString was = probe->text();
        const auto relayout = [&]() {
            for (int i = 0; i < 6; ++i) {
                page.layout()->invalidate();
                page.layout()->activate();
                QCoreApplication::processEvents();
            }
            return probe->height();
        };
        int h0 = -1, sameAll = 1, needMax = 0;
        for (const QString& t : armTexts) {
            probe->setText(t);
            const int h = relayout();
            needMax = qMax(needMax, probe->heightForWidth(probeW));
            if (h0 < 0)
                h0 = h;
            else if (h != h0)
                sameAll = 0;
        }
        probe->setText(was);
        relayout();
        std::printf("INFO 说明行：宽=%d，量了 %d 句（第一下能摆出来的全部），每句都占=%d，最挤那句要=%d\n",
                    probeW, static_cast<int>(armTexts.size()), h0, needMax);
        check("说明行的高度对「第一下能摆出的每一句」都一样（武装不许重排版面）",
              sameAll ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        check("预留的那块高度装得下其中最挤的一句（不许拿一行糊过去）",
              h0 >= needMax ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
    } else {
        std::printf("SKIP 说明行高度那 2 条：标签宽=%d、句子 %d 句（没排开就没得量）\n",
                    probeW, static_cast<int>(armTexts.size()));
    }
    has("这页已经不再创建任何 QMessageBox（模态框那一类彻底没了）",
        page.findChildren<QMessageBox*>().isEmpty());
    return bad;
}
