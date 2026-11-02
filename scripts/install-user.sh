#!/usr/bin/env bash
# Copy already-built VST Host executables into this user's desktop locations.
# Never configure, compile, download dependencies, or require sudo.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$root/build"
prefix="$HOME/.local"

usage() {
    cat <<'HELP'
Usage: bash scripts/install-user.sh [--build-dir DIR] [--prefix ABSOLUTE_DIR]

Requires a previously compiled GUI and CLI in build/ (or --build-dir).
Copies only vst-host-gui and vst-host into PREFIX/bin, and creates the
application-menu launcher in PREFIX/share/applications. Never builds code.
Default prefix: ~/.local. No sudo, no system dependency installation.

To uninstall, run: bash scripts/uninstall-user.sh
HELP
}

while (($#)); do
    case "$1" in
        --build-dir|--prefix)
            if (($# < 2)); then echo "Missing value for $1" >&2; exit 2; fi
            if [[ "$1" == "--build-dir" ]]; then build_dir="$2"; else prefix="$2"; fi
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        --uninstall)
            echo "Use the separate script: bash scripts/uninstall-user.sh" >&2
            exit 2 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if (( EUID == 0 )); then
    echo "Do not run the per-user installer as root or with sudo." >&2
    exit 2
fi
if [[ "$prefix" != /* || "$prefix" == "/" ]]; then
    echo "Prefix must be an absolute directory other than /." >&2
    exit 2
fi
if [[ "$build_dir" != /* ]]; then build_dir="$root/$build_dir"; fi
if [[ ! -f "$build_dir/vst-host-gui" || ! -x "$build_dir/vst-host-gui" ||
      ! -f "$build_dir/vst-host" || ! -x "$build_dir/vst-host" ]]; then
    echo "Missing compiled vst-host-gui or vst-host in: $build_dir" >&2
    echo "Compile first, e.g.:" >&2
    echo "  cmake -S . -B build -DVST_HOST_ENABLE_VST3=ON -DVST_HOST_BUILD_GUI=ON" >&2
    echo "  cmake --build build --parallel 2" >&2
    echo "Then run this script again (or specify --build-dir)." >&2
    exit 1
fi

# Freedesktop Exec is not a shell command; quote one absolute filename.
escape_exec_arg() {
    local value="$1"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//\$/\\\$}
    value=${value//`/\\`}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

bin_dir="$prefix/bin"
desktop_dir="$prefix/share/applications"
desktop_entry="$desktop_dir/vst-host-experiment.desktop"
gui_binary="$bin_dir/vst-host-gui"
expected_exec="Exec=$(escape_exec_arg "$gui_binary")"

# Never replace an existing application or launcher that this installer did
# not create. This also protects manually installed unrelated executables.
managed=false
if [[ -e "$desktop_entry" || -L "$desktop_entry" ]]; then
    if [[ -L "$desktop_entry" || ! -f "$desktop_entry" ]] ||
       ! grep -Fxq 'X-VSTHostExperiment-Managed=true' "$desktop_entry" ||
       ! grep -Fxq "$expected_exec" "$desktop_entry"; then
        echo "Refusing to replace an unrelated desktop entry: $desktop_entry" >&2
        exit 1
    fi
    managed=true
fi
if [[ "$managed" == false ]] &&
   { [[ -e "$gui_binary" || -L "$gui_binary" ]] ||
     [[ -e "$bin_dir/vst-host" || -L "$bin_dir/vst-host" ]]; }; then
    echo "Refusing to overwrite existing executables without our launcher in $bin_dir." >&2
    exit 1
fi
if [[ -L "$gui_binary" || -L "$bin_dir/vst-host" ]]; then
    echo "Refusing to replace symlinked executable paths." >&2
    exit 1
fi

mkdir -p -- "$bin_dir" "$desktop_dir"

# Stage each new file in its destination directory, then replace atomically.
# Existing running executables are not overwritten in place.
tmp_gui=$(mktemp "$bin_dir/.vst-host-gui.XXXXXX")
tmp_cli=$(mktemp "$bin_dir/.vst-host.XXXXXX")
tmp_desktop=$(mktemp "$desktop_dir/.vst-host-experiment.XXXXXX")
trap 'rm -f -- "$tmp_gui" "$tmp_cli" "$tmp_desktop"' EXIT
install -m 755 -- "$build_dir/vst-host-gui" "$tmp_gui"
install -m 755 -- "$build_dir/vst-host" "$tmp_cli"
cat > "$tmp_desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=VST Host Experiment
GenericName=Virtual microphone effects host
Comment=Apply VST effects to a PipeWire microphone
$expected_exec
Icon=audio-input-microphone
Terminal=false
Categories=AudioVideo;Audio;
StartupNotify=true
X-VSTHostExperiment-Managed=true
DESKTOP
chmod 644 "$tmp_desktop"
mv -f -- "$tmp_gui" "$gui_binary"
mv -f -- "$tmp_cli" "$bin_dir/vst-host"
mv -f -- "$tmp_desktop" "$desktop_entry"
trap - EXIT

echo "Copied already-built VST Host files (no build performed):"
echo "  $gui_binary"
echo "  $bin_dir/vst-host"
echo "  $desktop_entry"
echo "Existing profiles and configuration were not changed."
