#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QTimer;
class QFutureWatcherBase;

// 账户那一层的读/写全在这个 namespace 里，页面只是壳。
// 拆开的理由和别处一样：判据是纯函数，`--username` 能在离屏把它们逐条断言一遍，
// 而**一次都不会真的去改账户**（改账户是不可逆级别的动作，只能在界面上由人按下）。
namespace UserAccount {

struct Info {
    bool readable = false;
    bool local = false;         // SAM（Get-LocalUser）里查得到 = 本地账户
    bool msaHint = false;       // 读到微软账户登录痕迹；只用来出提示，不据此禁用按钮
    QString machine, name, fullName, sid, profileDir, error;
};

Info read();                                        // 跑一次 powershell（纯读）

// 查某个账户名现在存不存在、在不在管理员组里（纯读，给"新建用户"回读验证用）
struct Probe {
    bool readable = false;
    bool exists = false;
    bool admin = false;
    QString fullName;
    QString error;
};
Probe probe(const QString& name);

QString psQuote(const QString& s);                  // PowerShell 单引号串：' → ''
// 名字能不能用。空表通过；forAccountName=true 时按账户名的限制（长度 20、不许那些字符）
QStringList reject(const QString& proposed, bool forAccountName);
QString fullNameScript(const QString& machine, const QString& name, const QString& newFull);
QString renameScript(const QString& machine, const QString& name, const QString& newName);
// 新建本地用户。密码是明文传进来的，只活在那段临时脚本里（跑完立刻删文件）——
// 这一点要在界面上写明白，别让人以为密码没落过盘。
QString newUserScript(const QString& name, const QString& fullName, const QString& password,
                      bool asAdmin);
// 落成临时 .ps1（带 BOM，否则 PS5.1 把中文读成乱码）→ runas 提权 → 等退出码
bool runElevatedScript(const QString& script, QString* err);

} // namespace UserAccount

// 工具 → Windows 用户名：看清自己现在叫什么，然后改显示名或账户名。
class UserAccountPage : public QWidget {
    Q_OBJECT
public:
    explicit UserAccountPage(QWidget* parent = nullptr);
    ~UserAccountPage() override;

    // 「第一下能摆出来的每一句说明」：常驻那句 + 三颗按钮各自的风险说明（含管理员那两档）。
    // 公开是为了自测拿**同一批**句子去量"高度不随文字变" —— 它在页面里另数一份就成了平行实现。
    // 这颗也是预留高度唯一的来源：页面上任何一句只要在这批里，武装/收回就不许换版面。
    QStringList hintArmTexts() const;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void refresh();          // 后台跑 powershell（读一次要半秒多，不能卡在开页面上）
    void applyInfo(const UserAccount::Info& got);
    void applyFullName();
    void applyAccountName();
    void createNewUser();
    // 就地两段确认：第一下把同一颗按钮变成「确认…」并亮 6 秒，第二下才动手（返回 true）。
    // 不再用模态框 —— 这页从启动器里叫出来，启动器一失焦就把自己藏起来，
    // 挂在它里面的模态框看不见也答不了，实测三颗按钮按下后哪儿也没去。
    bool armOrRun(QPushButton* btn, const QString& confirmText, const QString& risk);
    void disarm();
    void refused(const QString& action, const QStringList& why);
    // 三句风险说明只写一份：armOrRun 摆出来用它，hintArmTexts 预留高度也用它。
    // 名字那两格是**参数**：界面拿真那串，预留拿最长的可能值占位（账户名上限 20 是
    // reject() 定的规矩，按下之前就先过那一关，所以 20 是个有界的数）。
    QString riskFullText() const;
    QString riskNameText(const QString& who) const;
    QString riskCreateText(const QString& machine, const QString& who, bool admin) const;
    // 把说明行钉成"最挤的那一句"那么高：文字换来换去都不许改这一行占的垂直空间。
    void reserveHintHeight();

    QLabel* m_state = nullptr;
    QLabel* m_hint = nullptr;
    QLineEdit* m_full = nullptr;
    QLineEdit* m_name = nullptr;
    QPushButton* m_applyFull = nullptr;
    QPushButton* m_applyName = nullptr;
    QLineEdit* m_newName = nullptr;
    QLineEdit* m_newFull = nullptr;
    QLineEdit* m_newPass = nullptr;
    QCheckBox* m_newAdmin = nullptr;
    QPushButton* m_create = nullptr;
    UserAccount::Info m_info;
    QFutureWatcherBase* m_watcher = nullptr;
    QPushButton* m_armed = nullptr;      // 现在处于"等你第二下"的那颗（最多一颗）
    bool m_pinningHint = false;          // 预留高度时的闸门：setFixedHeight 会再触发一次 resize
    QString m_armedText;                 // 它原来的文字，收回时换回去
    QTimer* m_armTimer = nullptr;
};
