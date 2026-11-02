#!/usr/bin/env bash
# Uninstall only this user's VST Host files; configuration removal is opt-in.
set -euo pipefail

prefix="$HOME/.local"
remove_config=false

usage() {
    cat <<'HELP'
Usage: bash scripts/uninstall-user.sh [--prefix ABSOLUTE_DIR] [--remove-config]

Removes this installer's GUI, CLI, application-menu entry and matching
VST Host login autostart entry. No sudo; does not touch shared libraries,
other applications, other desktop entries, or external VST/Wine plugins.

By default, preserves all user profiles and settings.
--remove-config ALSO removes the app's own Qt settings file and UUID-named
saved .vsthost profiles under the app-specific XDG configuration folder.
Unrelated files (even in the same directory) are left untouched.
HELP
}

while (($#)); do
    case "$1" in
        --prefix)
            if (($# < 2)); then echo "Missing value for --prefix" >&2; exit 2; fi
            prefix="$2"; shift 2 ;;
        --remove-config|--purge-config) remove_config=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if (( EUID == 0 )); then
    echo "Do not run the per-user uninstaller as root or with sudo." >&2
    exit 2
fi
if [[ "$prefix" != /* || "$prefix" == "/" ]]; then
    echo "Prefix must be an absolute directory other than /." >&2
    exit 2
fi

config_root="${XDG_CONFIG_HOME:-$HOME/.config}"
if [[ "$config_root" != /* || "$config_root" == "/" ]]; then
    echo "XDG_CONFIG_HOME must be an absolute user directory." >&2
    exit 2
fi

escape_exec_arg() {
    local value="$1"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//\$/\\\$}
    local tick=
    value=${value//%/%%}
    printf '"%s"' "$value"
}

gui_binary="$prefix/bin/vst-host-gui"
cli_binary="$prefix/bin/vst-host"
desktop_entry="$prefix/share/applications/vst-host-experiment.desktop"
autostart_entry="$config_root/autostart/vst-host-experiment.desktop"
expected_exec="Exec=$(escape_exec_arg "$gui_binary")"

# A matching managed launcher proves that this installer owns these exact
# executable paths. Never delete binaries on filename alone.
if [[ -f "$desktop_entry" && ! -L "$desktop_entry" ]] &&
   grep -Fxq 'X-VSTHostExperiment-Managed=true' "$desktop_entry" &&
   grep -Fxq "$expected_exec" "$desktop_entry"; then
    rm -f -- "$desktop_entry" "$gui_binary" "$cli_binary"
    echo "Removed managed application binaries and desktop launcher."
else
    echo "No matching managed launcher found; existing executables were left untouched."
fi

# This app's GUI creates a managed XDG autostart entry in Preferences.
# Remove only the entry that targets this installed binary, not other users'
# or applications' login entries.
if [[ -f "$autostart_entry" && ! -L "$autostart_entry" ]] &&
   grep -Fxq 'X-VSTHostExperiment-Managed=true' "$autostart_entry" &&
   grep -Fxq "$expected_exec --autostart" "$autostart_entry"; then
    rm -f -- "$autostart_entry"
    echo "Removed VST Host login autostart entry."
fi

if "$remove_config"; then
    # QSettings("VSTHostExperiment", "VSTHostExperiment") on Linux writes
    # this single INI-style .conf file. Never remove the parent directory
    # unless empty, since it might contain files owned by other software.
    qt_dir="$config_root/VSTHostExperiment"
    if [[ -d "$qt_dir" && ! -L "$qt_dir" ]]; then
        rm -f -- "$qt_dir/VSTHostExperiment.conf"
        rmdir -- "$qt_dir" 2>/dev/null || true
    fi

    # Named presets are UUID.vsthost files created by the app. Preserve
    # everything else, including exported .vsthost files elsewhere,
    # unrelated files and subdirectories even under this config folder.
    app_dir="$config_root/vst-host-experiment"
    profiles_dir="$app_dir/profiles"
    if [[ -d "$app_dir" && ! -L "$app_dir" &&
          -d "$profiles_dir" && ! -L "$profiles_dir" ]]; then
        for profile in "$profiles_dir"/*.vsthost; do
            [[ -f "$profile" || -L "$profile" ]] || continue
            filename=${profile##*/}
            if [[ "$filename" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.vsthost$ ]]; then
                rm -f -- "$profile"
            fi
        done
        rmdir -- "$profiles_dir" 2>/dev/null || true
        rmdir -- "$app_dir" 2>/dev/null || true
    fi
    echo "Removed application-owned settings and UUID-named profiles where present."
    echo "Other files, imported/exported profiles elsewhere and plugin folders were preserved."
else
    echo "Saved settings and profiles were preserved (use --remove-config to delete app-owned ones)."
fi
\x60'
    value=${value//"$tick"/\\$tick}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

gui_binary="$prefix/bin/vst-host-gui"
cli_binary="$prefix/bin/vst-host"
desktop_entry="$prefix/share/applications/vst-host-experiment.desktop"
autostart_entry="$config_root/autostart/vst-host-experiment.desktop"
expected_exec="Exec=$(escape_exec_arg "$gui_binary")"

# A matching managed launcher proves that this installer owns these exact
# executable paths. Never delete binaries on filename alone.
if [[ -f "$desktop_entry" && ! -L "$desktop_entry" ]] &&
   grep -Fxq 'X-VSTHostExperiment-Managed=true' "$desktop_entry" &&
   grep -Fxq "$expected_exec" "$desktop_entry"; then
    rm -f -- "$desktop_entry" "$gui_binary" "$cli_binary"
    echo "Removed managed application binaries and desktop launcher."
else
    echo "No matching managed launcher found; existing executables were left untouched."
fi

# This app's GUI creates a managed XDG autostart entry in Preferences.
# Remove only the entry that targets this installed binary, not other users'
# or applications' login entries.
if [[ -f "$autostart_entry" && ! -L "$autostart_entry" ]] &&
   grep -Fxq 'X-VSTHostExperiment-Managed=true' "$autostart_entry" &&
   grep -Fxq "$expected_exec --autostart" "$autostart_entry"; then
    rm -f -- "$autostart_entry"
    echo "Removed VST Host login autostart entry."
fi

if "$remove_config"; then
    # QSettings("VSTHostExperiment", "VSTHostExperiment") on Linux writes
    # this single INI-style .conf file. Never remove the parent directory
    # unless empty, since it might contain files owned by other software.
    qt_dir="$config_root/VSTHostExperiment"
    if [[ -d "$qt_dir" && ! -L "$qt_dir" ]]; then
        rm -f -- "$qt_dir/VSTHostExperiment.conf"
        rmdir -- "$qt_dir" 2>/dev/null || true
    fi

    # Named presets are UUID.vsthost files created by the app. Preserve
    # everything else, including exported .vsthost files elsewhere,
    # unrelated files and subdirectories even under this config folder.
    app_dir="$config_root/vst-host-experiment"
    profiles_dir="$app_dir/profiles"
    if [[ -d "$app_dir" && ! -L "$app_dir" &&
          -d "$profiles_dir" && ! -L "$profiles_dir" ]]; then
        for profile in "$profiles_dir"/*.vsthost; do
            [[ -f "$profile" || -L "$profile" ]] || continue
            filename=${profile##*/}
            if [[ "$filename" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.vsthost$ ]]; then
                rm -f -- "$profile"
            fi
        done
        rmdir -- "$profiles_dir" 2>/dev/null || true
        rmdir -- "$app_dir" 2>/dev/null || true
    fi
    echo "Removed application-owned settings and UUID-named profiles where present."
    echo "Other files, imported/exported profiles elsewhere and plugin folders were preserved."
else
    echo "Saved settings and profiles were preserved (use --remove-config to delete app-owned ones)."
fi
