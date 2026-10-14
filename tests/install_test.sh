#!/usr/bin/env bash
set -euo pipefail
binary="$(/usr/bin/readlink -f -- "$1")"
project_directory="$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd)"
fixture="$(/usr/bin/mktemp -d /tmp/metadots-install.XXXXXX)"
export HOME="$fixture/home"
export XDG_CONFIG_HOME="$fixture/config"
/usr/bin/mkdir -p -- "$HOME/bin" "$XDG_CONFIG_HOME/themes/one"
/usr/bin/cat > "$HOME/bin/theme-apply" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
queue_reload() { shift; "$@" >/dev/null; }
reload_applications() {
    :
}
reload_applications
EOF
/usr/bin/ln -- "$HOME/bin/theme-apply" "$HOME/bin/coordinator-hardlink"
printf '%s\n' '{"background":"#142033","foreground":"#e2e7f2","primary":"#9bbded"}' > "$XDG_CONFIG_HOME/themes/one/palette.json"
/usr/bin/cat > "$XDG_CONFIG_HOME/themes/one/Colors.qml" <<'EOF'
property color background: "#e00e1e36"
property color foreground: "#f4b999"
property color primary: "#9279aa"
EOF
/usr/bin/ln -s one "$XDG_CONFIG_HOME/themes/current"
/usr/bin/ln -s current "$XDG_CONFIG_HOME/themes/.caelestia-use"
printf 'icon-override = true\nexisting.desktop|symbolic|Existing\n' > "$XDG_CONFIG_HOME/apps.list"
options=(--build-dir "${binary%/*}" --prefix "$HOME/.local")
/usr/bin/bash "$project_directory/scripts/install.sh" "${options[@]}" --dry-run >/dev/null
[[ ! -e "$HOME/.local" ]]
/usr/bin/bash "$project_directory/scripts/install.sh" "${options[@]}" >/dev/null
printf '%s\n' 'trailing.desktop|symbolic|Trailing' >> "$XDG_CONFIG_HOME/apps.list"
/usr/bin/bash "$project_directory/scripts/install.sh" "${options[@]}" >/dev/null
/usr/bin/cmp -- "$binary" "$HOME/.local/bin/metadots"
[[ -L "$HOME/.local/share/applications/io.github.metadots.desktop" ]]
[[ "$(/usr/bin/grep -c '^io.github.metadots.desktop|' "$XDG_CONFIG_HOME/apps.list")" == 1 ]]
/usr/bin/grep -q '^existing.desktop|' "$XDG_CONFIG_HOME/apps.list"
[[ "$(/usr/bin/tail -n 1 "$XDG_CONFIG_HOME/apps.list")" == 'trailing.desktop|symbolic|Trailing' ]]
[[ "$(/usr/bin/grep -c '# MetaDots palette sync' "$HOME/bin/theme-apply")" == 1 ]]
/usr/bin/cmp -- "$HOME/bin/theme-apply" "$HOME/bin/coordinator-hardlink"
printf '%s\n' 'property color background: "#e00e1e36"' 'property color foreground: "#ffffff"' 'property color primary: "#123456"' > "$XDG_CONFIG_HOME/themes/one/Colors.qml"
/usr/bin/bash "$HOME/bin/theme-apply"
"$binary" --config "$XDG_CONFIG_HOME/metadots/theme.json" theme show | /usr/bin/grep -q '#123456'
if [[ -x /usr/bin/desktop-file-validate ]]; then /usr/bin/desktop-file-validate "$HOME/.local/bin/applications/io.github.metadots.desktop"; fi
/usr/bin/bash "$project_directory/scripts/install.sh" "${options[@]}" --config-dir "$XDG_CONFIG_HOME/alternate config" >/dev/null
/usr/bin/grep -qF "$XDG_CONFIG_HOME/alternate config/theme.json" "$HOME/bin/theme-apply"
/usr/bin/bash "$HOME/bin/theme-apply"
/usr/bin/bash "$project_directory/scripts/install.sh" --build-dir "${binary%/*}" --prefix "$HOME/standalone prefix" \
    --config-dir "$XDG_CONFIG_HOME/standalone config" --apps-list "$XDG_CONFIG_HOME/standalone apps.list" --standalone >/dev/null
/usr/bin/env PATH=/usr/bin:/bin "$HOME/standalone prefix/bin/metadots" --config "$XDG_CONFIG_HOME/standalone config/theme.json" theme show >/dev/null
if [[ -x /usr/bin/desktop-file-validate ]]; then /usr/bin/desktop-file-validate "$HOME/standalone prefix/bin/applications/io.github.metadots.desktop"; fi
printf 'Installation checks passed\n'
