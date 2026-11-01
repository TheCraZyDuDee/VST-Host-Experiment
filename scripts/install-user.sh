#!/usr/bin/env bash
# Install/uninstall the built desktop app for the current user. No sudo.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
build_dir="$root/build"
prefix="$HOME/.local"
skip_build=false
uninstall=false

usage() {
    cat <<'HELP'
Usage: ./scripts/install-user.sh [--build-dir DIR] [--prefix DIR] [--no-build] [--uninstall]

Installs into ~/.local by default (no root), including a desktop launcher.
Existing CMake build settings (including optional VST2 SDK paths) are reused.
If the build directory is new, the GUI and VST3 host are configured automatically.
--no-build: install an already built GUI and CLI (useful for CI).
--uninstall: remove installed executables and launcher, not settings/profiles.
HELP
}

while (($#)); do
    case "$1" in
        --build-dir|--prefix)
            if (($# < 2)); then echo "Missing argument to $1" >&2; exit 2; fi
            if [[ "$1" == "--build-dir" ]]; then build_dir="$2"; else prefix="$2"; fi
            shift 2 ;;
        --no-build) skip_build=true; shift ;;
        --uninstall) uninstall=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$prefix" != /* || "$prefix" == "/" ]]; then
    echo "Installation prefix must be an absolute directory other than /." >&2
    exit 2
fi
mkdir -p -- "$prefix"
prefix=$(cd -- "$prefix" && pwd -P)
if [[ "$build_dir" != /* ]]; then build_dir="$root/$build_dir"; fi

desktop_entry="$prefix/share/applications/vst-host-experiment.desktop"
gui_binary="$prefix/bin/vst-host-gui"

# Encode an absolute executable path as one Desktop Entry Exec argument.
# Quoting protects spaces; escape characters required by the specification.
escape_exec_arg() {
    local value="$1"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//\$/\\\$}
    value=${value//\`/\\\`}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

if "$uninstall"; then
    if [[ -f "$desktop_entry" ]] &&
       grep -Fxq 'X-VSTHostExperiment-Managed=true' "$desktop_entry"; then
        rm -f -- "$desktop_entry"
    fi
    rm -f -- "$prefix/bin/vst-host-gui" "$prefix/bin/vst-host"
    autostart_entry="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/vst-host-experiment.desktop"
    if [[ -f "$autostart_entry" ]] &&
       grep -Fxq 'X-VSTHostExperiment-Managed=true' "$autostart_entry" &&
       grep -Fxq "Exec=$(escape_exec_arg "$gui_binary") --autostart" "$autostart_entry"; then
        rm -f -- "$autostart_entry"
    fi
    echo "Uninstalled executables and desktop launcher. Saved profiles and preferences are preserved."
    exit 0
fi

if ! "$skip_build"; then
    if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
        cmake -S "$root" -B "$build_dir" -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON
    fi
    cmake --build "$build_dir" --parallel 2
fi

if [[ ! -x "$build_dir/vst-host-gui" || ! -x "$build_dir/vst-host" ]]; then
    echo "Expected both GUI and CLI binaries in $build_dir." >&2
    echo "Configure with -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON and build first." >&2
    exit 1
fi

cmake --install "$build_dir" --prefix "$prefix"
mkdir -p -- "$(dirname -- "$desktop_entry")"
cat > "$desktop_entry" <<DESKTOP
[Desktop Entry]
Type=Application
Name=VST Host Experiment
GenericName=Virtual microphone effects host
Comment=Apply VST effects to a PipeWire microphone
Exec=$(escape_exec_arg "$gui_binary")
TryExec=$gui_binary
Icon=audio-input-microphone
Terminal=false
Categories=AudioVideo;Audio;
StartupNotify=true
X-VSTHostExperiment-Managed=true
DESKTOP

echo "Installed VST Host Experiment for the current user:"
echo "  GUI:     $gui_binary"
echo "  CLI:     $prefix/bin/vst-host"
echo "  Menu:    $desktop_entry"
echo "You can now launch VST Host Experiment from your desktop app menu."
