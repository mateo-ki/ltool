#include "ShellMenu.h"
#include "PluginHost.h"
#include "Utils.h"

#include <QSettings>

namespace {

QString intentPath()
{
    return Utils::dataDir() + QStringLiteral("/config.ini");
}

} // namespace

namespace ShellMenu {

bool installed()
{
    return false;
}

QString install()
{
    return QStringLiteral("非 Windows 平台暂不支持外壳右键集成");
}

QString remove()
{
    return QString();
}

void refreshIfStale()
{
}

QString locationHint()
{
    return QStringLiteral("（非 Windows 平台不适用）");
}

bool providerPresent()
{
    const PluginHost::Meta* m = PluginHost::meta(QStringLiteral("shell"));
    return m && m->ok() && !PluginHost::isDisabled(QStringLiteral("shell"));
}

bool intent()
{
    return QSettings(intentPath(), QSettings::IniFormat)
        .value(QStringLiteral("shell/intent"), false)
        .toBool();
}

void setIntent(bool on)
{
    QSettings st(intentPath(), QSettings::IniFormat);
    st.setValue(QStringLiteral("shell/intent"), on);
    st.sync();
}

bool intentExplicitlySet()
{
    return QSettings(intentPath(), QSettings::IniFormat).contains(QStringLiteral("shell/intent"));
}

QString syncWithPlugin()
{
    return QString();
}

} // namespace ShellMenu
