// ltool 工具插件：Windows 用户名
//
// 页面本体是 UserAccountPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "UserAccount.h"

#include <QObject>
#include <QtPlugin>

class UserPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("user"); }
    QString name() const override { return QStringLiteral("Windows 用户名"); }
    QString desc() const override { return QStringLiteral("系统用户账户与权限治理：查看账户属性、改显示名/登录名或新建本地用户"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("用户名"), QStringLiteral("账户"), QStringLiteral("改名"), QStringLiteral("user"), QStringLiteral("username"), QStringLiteral("account"), QStringLiteral("fullname") };
    }
    QWidget* createPage(QWidget* parent) override { return new UserAccountPage(parent); }
};

// 这个工具自己的判据。主程序不再链接页面代码，所以测试跟着页一起搬进来，
// 由 --plugintest 通过 QPluginLoader::resolve 找到这个符号来跑。
int userPageSelftest();   // 判据本体在同目录 userselftest.cpp
extern "C" Q_DECL_EXPORT int ltoolPluginSelftest()
{
    return userPageSelftest();
}
#include "userplugin.moc"
