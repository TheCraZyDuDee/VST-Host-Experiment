#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace vst_host::gui {

struct SavedProfile {
    QString id;
    QString name;
};

// Human-visible names are metadata, never filesystem paths. Profile filenames
// are opaque UUIDs so names such as "../foo" cannot escape the profile folder.
[[nodiscard]] inline bool valid_profile_id(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(id).hasMatch();
}

[[nodiscard]] inline QString user_config_root() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

[[nodiscard]] inline QString profiles_directory(const QString& configRoot) {
    return QDir(configRoot).filePath(QStringLiteral("vst-host-experiment/profiles"));
}

[[nodiscard]] inline QString profile_file(const QString& configRoot, const QString& id) {
    if (!valid_profile_id(id)) return {};
    return QDir(profiles_directory(configRoot)).filePath(id + QStringLiteral(".vsthost"));
}

[[nodiscard]] inline QList<SavedProfile> saved_profiles(QSettings& settings,
                                                         const QString& configRoot) {
    QList<SavedProfile> result;
    const auto ids = settings.value(QStringLiteral("profiles/ids")).toStringList();
    for (const auto& id : ids) {
        if (!valid_profile_id(id) || !QFileInfo(profile_file(configRoot, id)).isFile())
            continue;
        const auto name = settings.value(QStringLiteral("profiles/names/") + id).toString();
        if (!name.trimmed().isEmpty())
            result.push_back(SavedProfile{id, name});
    }
    return result;
}

[[nodiscard]] inline bool record_profile(QSettings& settings,
                                          const QString& id, const QString& name) {
    if (!valid_profile_id(id) || name.trimmed().isEmpty()) return false;
    auto ids = settings.value(QStringLiteral("profiles/ids")).toStringList();
    if (!ids.contains(id)) ids.append(id);
    settings.setValue(QStringLiteral("profiles/ids"), ids);
    settings.setValue(QStringLiteral("profiles/names/") + id, name.trimmed());
    settings.sync();
    return settings.status() == QSettings::NoError;
}

inline void unrecord_profile(QSettings& settings, const QString& id) {
    if (!valid_profile_id(id)) return;
    auto ids = settings.value(QStringLiteral("profiles/ids")).toStringList();
    ids.removeAll(id);
    settings.setValue(QStringLiteral("profiles/ids"), ids);
    settings.remove(QStringLiteral("profiles/names/") + id);
    // A deleted profile may never silently fall back to the last workspace
    // and start audio on login.
    if (settings.value(QStringLiteral("autostart/profileId")).toString() == id) {
        settings.setValue(QStringLiteral("autostart/profileId"), QString());
        settings.setValue(QStringLiteral("autostart/startAudio"), false);
    }
    settings.sync();
}

// Freedesktop Desktop Entry Exec is NOT a shell command. Quote one argument,
// escaping special characters and literal percent signs (field-code syntax).
[[nodiscard]] inline QString desktop_exec_argument(QString value) {
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    value.replace(QStringLiteral("$"), QStringLiteral("\\$"));
    value.replace(QChar(0x60), QString(QChar(0x5c)) + QChar(0x60));
    value.replace(QStringLiteral("%"), QStringLiteral("%%"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

[[nodiscard]] inline QString autostart_file(const QString& configRoot) {
    return QDir(configRoot).filePath(QStringLiteral(
        "autostart/vst-host-experiment.desktop"));
}

[[nodiscard]] inline bool is_managed_autostart(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    if (file.size() > 64 * 1024) return false;
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')).contains(
        QStringLiteral("X-VSTHostExperiment-Managed=true"));
}

// XDG autostart is opt-in and user-scoped. Never replace/remove a desktop file
// with this name unless our marker is present. Use an atomic write to avoid
// leaving a half-written entry after disk errors or a crash.
[[nodiscard]] inline bool set_login_autostart(const QString& configRoot,
                                              const QString& executable,
                                              bool enabled, QString& error) {
    error.clear();
    const auto path = autostart_file(configRoot);
    if (QFileInfo::exists(path) && !is_managed_autostart(path)) {
        error = QStringLiteral(
            "An existing autostart entry at %1 is not managed by VST Host. "
            "No changes were made.").arg(path);
        return false;
    }

    if (!enabled) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            error = QStringLiteral("Could not remove autostart entry: %1").arg(path);
            return false;
        }
        return true;
    }

    if (!QFileInfo(executable).isAbsolute() || !QFileInfo(executable).isExecutable()) {
        error = QStringLiteral(
            "The GUI executable is missing or not executable: %1").arg(executable);
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("Could not create autostart directory: %1").arg(path);
        return false;
    }

    const QString content =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=VST Host Experiment\n"
                       "Comment=Start the saved microphone profile at login\n"
                       "Exec=") + desktop_exec_argument(executable) +
        QStringLiteral(" --autostart\n"
                       "Icon=audio-input-microphone\n"
                       "Terminal=false\n"
                       "Categories=AudioVideo;Audio;\n"
                       "X-VSTHostExperiment-Managed=true\n");
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = out.errorString();
        return false;
    }
    const auto utf8 = content.toUtf8();
    if (out.write(utf8) != utf8.size() || !out.commit()) {
        error = out.errorString();
        return false;
    }
    return true;
}

} // namespace vst_host::gui
