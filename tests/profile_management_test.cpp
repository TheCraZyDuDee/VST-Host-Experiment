#include "gui/profile_management.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    assert(temp.isValid());
    const auto root = temp.path();
    QSettings settings(root + QStringLiteral("/test-settings.ini"), QSettings::IniFormat);
    const QString id = QStringLiteral("3cd3ad32-24a0-44f1-818b-a8f00e3a062d");

    assert(vst_host::gui::valid_profile_id(id));
    assert(!vst_host::gui::valid_profile_id(QStringLiteral("../other/secret")));
    assert(vst_host::gui::profile_file(root, QStringLiteral("../bad")).isEmpty());
    assert(vst_host::gui::saved_profiles(settings, root).isEmpty());

    const auto file = vst_host::gui::profile_file(root, id);
    assert(QDir().mkpath(QFileInfo(file).absolutePath()));
    {
        QFile out(file);
        assert(out.open(QIODevice::WriteOnly));
        assert(out.write("test data") == 9);
    }
    assert(vst_host::gui::record_profile(settings, id, QStringLiteral("Studio")));
    assert(vst_host::gui::record_profile(settings, id, QStringLiteral("Updated")));
    const auto list = vst_host::gui::saved_profiles(settings, root);
    assert(list.size() == 1);
    assert(list.front().id == id);
    assert(list.front().name == QStringLiteral("Updated"));

    settings.setValue(QStringLiteral("autostart/profileId"), id);
    settings.setValue(QStringLiteral("autostart/startAudio"), true);
    vst_host::gui::unrecord_profile(settings, id);
    assert(settings.value(QStringLiteral("autostart/profileId")).toString().isEmpty());
    assert(!settings.value(QStringLiteral("autostart/startAudio")).toBool());

    // One Exec argument can contain whitespace, quotes, dollars or literal
    // desktop field codes without being mistaken for multiple arguments.
    const auto quoted = vst_host::gui::desktop_exec_argument(
        QStringLiteral("/home/test user/app$%\""));
    assert(quoted.startsWith(QLatin1Char('"')));
    assert(quoted.endsWith(QLatin1Char('"')));
    assert(quoted.contains(QStringLiteral("\\$")));
    assert(quoted.contains(QStringLiteral("%%")));
    assert(quoted.contains(QStringLiteral("\\\"")));

    QString error;
    const auto exe = QCoreApplication::applicationFilePath();
    assert(vst_host::gui::set_login_autostart(root, exe, true, error));
    assert(error.isEmpty());
    const auto desktop = vst_host::gui::autostart_file(root);
    assert(vst_host::gui::is_managed_autostart(desktop));
    {
        QFile in(desktop);
        assert(in.open(QIODevice::ReadOnly));
        const auto text = in.readAll();
        assert(text.contains(" --autostart\n"));
        assert(text.contains("X-VSTHostExperiment-Managed=true\n"));
    }

    assert(vst_host::gui::set_login_autostart(root, exe, false, error));
    assert(!QFileInfo::exists(desktop));
    // Never overwrite a manually maintained file of the same name.
    {
        QFile unknown(desktop);
        assert(unknown.open(QIODevice::WriteOnly));
        assert(unknown.write("[Desktop Entry]\nName=Other\n") > 0);
    }
    assert(!vst_host::gui::set_login_autostart(root, exe, true, error));
    assert(!error.isEmpty());
    assert(!vst_host::gui::set_login_autostart(root, exe, false, error));
    assert(QFileInfo::exists(desktop));

    return 0;
}
