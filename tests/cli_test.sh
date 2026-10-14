#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -x "$1" ]]; then
    printf 'Usage: cli_test.sh METADOTS_BIN\n' >&2
    exit 2
fi

metadots_bin="$1"
test_root="$(mktemp -d)"
trap 'rm -rf -- "$test_root"' EXIT
export HOME="$test_root/home"
export XDG_CONFIG_HOME="$HOME/.config"
mkdir -p -- "$XDG_CONFIG_HOME/themes/first" "$XDG_CONFIG_HOME/themes/second"
config="$test_root/theme.json"

"$metadots_bin" --config "$config" theme show >/dev/null
[[ ! -e "$config" ]]

"$metadots_bin" theme set --dry-run --config "$config" background=#e00e1e36 >/dev/null
[[ ! -e "$config" ]]

cat > "$XDG_CONFIG_HOME/themes/first/Colors.qml" <<'QML'
Singleton {
    readonly property color background: "#101318"
    readonly property color foreground: "#e7eaf0"
    readonly property color primary: "#e37b66"
}
QML
cat > "$XDG_CONFIG_HOME/themes/second/Colors.qml" <<'QML'
Singleton {
    readonly property color background: "#20242a"
    readonly property color foreground: "#e7eaf0"
    readonly property color primary: "#8fb8f5"
}
QML
ln -s first "$XDG_CONFIG_HOME/themes/.caelestia-use"

"$metadots_bin" theme follow desktop --config "$config" >/dev/null
rg -q '"source_kind": "desktop"' "$config"
rg -q '"accent": "#e37b66"' "$config"
ln -s second "$XDG_CONFIG_HOME/themes/.selector-next"
mv -Tf -- "$XDG_CONFIG_HOME/themes/.selector-next" "$XDG_CONFIG_HOME/themes/.caelestia-use"
"$metadots_bin" theme sync --config "$config" >/dev/null
rg -q '"accent": "#8fb8f5"' "$config"

cat > "$test_root/alpha.qml" <<'QML'
Singleton {
    readonly property color background: Qt.rgba(14 / 255, 30 / 255, 54 / 255, 0.88)
    readonly property color foreground: "#f0e4d1"
    readonly property color primary: "#8eb1dd"
}
QML
"$metadots_bin" theme import "$test_root/alpha.qml" --config "$test_root/alpha-theme.json" --dry-run > "$test_root/import.json"
rg -q '"background": "#e00e1e36"' "$test_root/import.json"
[[ ! -e "$test_root/alpha-theme.json" ]]

before="$(sha256sum "$config" | cut -d ' ' -f1)"
if "$metadots_bin" theme set --config "$config" accent=invalid >/dev/null 2>&1; then
    printf 'invalid color unexpectedly succeeded\n' >&2
    exit 1
fi
after="$(sha256sum "$config" | cut -d ' ' -f1)"
[[ "$before" == "$after" ]]

concurrent_config="$test_root/concurrent-theme.json"
assignments=(
    background=#110000
    surface=#220000
    surface_alt=#330000
    text=#440000
    muted=#550000
    accent=#660000
    accent_text=#770000
    border=#880000
    selection=#990000
    error=#aa0000
)
pids=()
for index in "${!assignments[@]}"; do
    "$metadots_bin" theme set --config "$concurrent_config" "${assignments[$index]}" \
        > "$test_root/concurrent-$index.json" &
    pids+=("$!")
done
for pid in "${pids[@]}"; do
    wait "$pid"
done
for assignment in "${assignments[@]}"; do
    key="${assignment%%=*}"
    value="${assignment#*=}"
    rg -q "\"$key\": \"$value\"" "$concurrent_config"
done
