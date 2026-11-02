#!/usr/bin/env bash
# Execute with: bash tests/user_install_test.sh ABSOLUTE_COMPILED_BUILD_DIR
# All changes are confined to a fresh temporary HOME/XDG_CONFIG_HOME.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="${1:?Please supply the absolute path to a built GUI/CLI directory}"
[[ "$build_dir" == /* ]] || { echo "Build path must be absolute" >&2; exit 2; }
[[ -x "$build_dir/vst-host-gui" && -x "$build_dir/vst-host" ]] ||
    { echo "Binaries missing in $build_dir" >&2; exit 2; }

test_root=$(mktemp -d)
trap 'rm -rf -- "$test_root"' EXIT
export HOME="$test_root/home"
export XDG_CONFIG_HOME="$HOME/.config"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"
prefix="$HOME/.local app with spaces"

# Prove that installer never calls CMake even when on PATH.
mkdir -p "$test_root/tools"
cat > "$test_root/tools/cmake" <<'SCRIPT'
#!/usr/bin/env sh
echo "Installer attempted to invoke CMake!" >&2
exit 91
SCRIPT
chmod 755 "$test_root/tools/cmake"
export PATH="$test_root/tools:$PATH"

bash -n "$root/scripts/install-user.sh"
bash -n "$root/scripts/uninstall-user.sh"

# Existing unowned executable or desktop files cannot be overwritten.
mkdir -p "$prefix/bin" "$prefix/share/applications"
printf 'user file\n' > "$prefix/bin/vst-host"
if bash "$root/scripts/install-user.sh" --build-dir "$build_dir" --prefix "$prefix"; then
    echo "Installer unexpectedly replaced a foreign executable" >&2; exit 1
fi
grep -Fxq 'user file' "$prefix/bin/vst-host"
rm -f -- "$prefix/bin/vst-host"
printf '[Desktop Entry]\nName=Another program\n' \
    > "$prefix/share/applications/vst-host-experiment.desktop"
if bash "$root/scripts/install-user.sh" --build-dir "$build_dir" --prefix "$prefix"; then
    echo "Installer unexpectedly replaced a foreign desktop file" >&2; exit 1
fi
grep -Fxq 'Name=Another program' \
    "$prefix/share/applications/vst-host-experiment.desktop"
rm -f -- "$prefix/share/applications/vst-host-experiment.desktop"

bash "$root/scripts/install-user.sh" --build-dir "$build_dir" --prefix "$prefix"
cmp -s -- "$build_dir/vst-host-gui" "$prefix/bin/vst-host-gui"
cmp -s -- "$build_dir/vst-host" "$prefix/bin/vst-host"
launcher="$prefix/share/applications/vst-host-experiment.desktop"
test -f "$launcher"
grep -Fxq 'X-VSTHostExperiment-Managed=true' "$launcher"
grep -Fq 'Exec="' "$launcher"
# Ensure a second copy-only installation remains safe.
bash "$root/scripts/install-user.sh" --build-dir "$build_dir" --prefix "$prefix"

# Prepare real app-owned configuration beside unrelated data that MUST survive.
mkdir -p "$XDG_CONFIG_HOME/VSTHostExperiment" \
         "$XDG_CONFIG_HOME/vst-host-experiment/profiles" \
         "$XDG_CONFIG_HOME/autostart" \
         "$XDG_CONFIG_HOME/other-software"
qt_config="$XDG_CONFIG_HOME/VSTHostExperiment/VSTHostExperiment.conf"
uuid_profile="$XDG_CONFIG_HOME/vst-host-experiment/profiles/3cd3ad32-24a0-44f1-818b-a8f00e3a062d.vsthost"
printf 'settings\n' > "$qt_config"
printf 'profile\n' > "$uuid_profile"
printf 'do not delete\n' > "$XDG_CONFIG_HOME/VSTHostExperiment/other-file.txt"
printf 'do not delete\n' > "$XDG_CONFIG_HOME/vst-host-experiment/profiles/custom.vsthost"
printf 'do not delete\n' > "$XDG_CONFIG_HOME/other-software/notes"
printf 'do not delete\n' > "$XDG_CONFIG_HOME/autostart/other-software.desktop"
printf 'do not delete\n' > "$HOME/external-export.vsthost"

# Simulate the managed XDG autostart entry created by app Preferences.
{
    printf '[Desktop Entry]\nType=Application\n'
    grep '^Exec=' "$launcher" | sed 's/$/ --autostart/'
    printf 'X-VSTHostExperiment-Managed=true\n'
} > "$XDG_CONFIG_HOME/autostart/vst-host-experiment.desktop"

# Plain uninstall keeps ALL configuration but removes the exact managed
# binaries, launcher and matching autostart entry.
bash "$root/scripts/uninstall-user.sh" --prefix "$prefix"
test ! -e "$prefix/bin/vst-host-gui"
test ! -e "$prefix/bin/vst-host"
test ! -e "$launcher"
test ! -e "$XDG_CONFIG_HOME/autostart/vst-host-experiment.desktop"
test -f "$qt_config"
test -f "$uuid_profile"
test -f "$XDG_CONFIG_HOME/autostart/other-software.desktop"

# The explicit purge flag is the only path that removes app-specific config.
bash "$root/scripts/install-user.sh" --build-dir "$build_dir" --prefix "$prefix"
bash "$root/scripts/uninstall-user.sh" --prefix "$prefix" --remove-config
test ! -e "$qt_config"
test ! -e "$uuid_profile"
test -f "$XDG_CONFIG_HOME/VSTHostExperiment/other-file.txt"
test -f "$XDG_CONFIG_HOME/vst-host-experiment/profiles/custom.vsthost"
test -f "$XDG_CONFIG_HOME/other-software/notes"
test -f "$XDG_CONFIG_HOME/autostart/other-software.desktop"
test -f "$HOME/external-export.vsthost"

# A foreign launcher's presence must NEVER authorize deleting other binaries.
printf 'user binary\n' > "$prefix/bin/vst-host"
printf 'Name=Foreign\n' > "$launcher"
bash "$root/scripts/uninstall-user.sh" --prefix "$prefix"
grep -Fxq 'user binary' "$prefix/bin/vst-host"
grep -Fxq 'Name=Foreign' "$launcher"

echo "Copy-only install and selective uninstall checks passed."
