#!/usr/bin/env bash
set -euo pipefail

project_directory="$(cd -- "${BASH_SOURCE[0]%/*}/.." && pwd)"
build_directory="$project_directory/build"
install_prefix="$HOME/.local"
config_directory="${XDG_CONFIG_HOME:-$HOME/.config}/metadots"
apps_list="${XDG_CONFIG_HOME:-$HOME/.config}/apps.list"
theme_hook="$HOME/bin/theme-apply"
follow_desktop=1
dry_run=0
desktop_id=io.github.metadots

usage() {
    printf '%s\n' 'Usage: bash scripts/install.sh [--dry-run] [--build-dir DIR] [--prefix DIR]' \
        '       [--config-dir DIR] [--apps-list FILE] [--theme-hook FILE]' \
        '       [--no-theme-hook] [--standalone]'
}

fail() { printf 'install: %s\n' "$*" >&2; exit 1; }

parse_options() {
    while (( $# )); do
        case "$1" in
            --help|-h) usage; exit 0 ;;
            --dry-run) dry_run=1; shift ;;
            --no-theme-hook) theme_hook=""; shift ;;
            --standalone) follow_desktop=0; theme_hook=""; shift ;;
            --build-dir|--prefix|--config-dir|--apps-list|--theme-hook)
                (( $# >= 2 )) || fail "Missing value for $1"
                [[ "$2" == /* && "$2" != / && "$2" != *$'\n'* && "$2" != *'|'* && "$2" != *'"'* && "$2" != *'%'* && "$2" != *'\\'* && "$2" != *'$'* && "$2" != *'`'* ]] || fail "Expected a safe absolute path for $1"
                case "$1" in
                    --build-dir) build_directory="${2%/}" ;;
                    --prefix) install_prefix="${2%/}" ;;
                    --config-dir) config_directory="${2%/}" ;;
                    --apps-list) apps_list="$2" ;;
                    --theme-hook) theme_hook="$2" ;;
                esac
                shift 2 ;;
            *) fail "Unknown option: $1" ;;
        esac
    done
}

backup_file() {
    [[ -f "$1" ]] || return 0
    /usr/bin/cp -p -- "$1" "$1.metadots-backup.$(/usr/bin/date +%Y%m%dT%H%M%S).$$"
}

validate_installation() {
    [[ -x "$build_directory/metadots" ]] || fail "Build MetaDots first: $build_directory/metadots"
    [[ ! -L "$install_prefix/bin/metadots" ]] || fail 'Executable is an unmanaged symlink'
    [[ ! -L "$install_prefix/bin/applications/$desktop_id.desktop" ]] || fail 'Desktop entry is an unmanaged symlink'
    [[ ! -L "$install_prefix/share/icons/hicolor/symbolic/apps/$desktop_id-symbolic.svg" ]] || fail 'Icon is an unmanaged symlink'
    [[ ! -e "$apps_list" || -f "$apps_list" ]] || fail "App list is not a regular file: $apps_list"
    [[ ! -L "$apps_list" ]] || apps_list="$(/usr/bin/readlink -f -- "$apps_list")"
    if [[ -n "$theme_hook" && -f "$theme_hook" ]]; then
        theme_hook="$(/usr/bin/readlink -f -- "$theme_hook")"
        /usr/bin/bash -n "$theme_hook"
        /usr/bin/grep -q '^reload_applications() {' "$theme_hook" || fail "Unsupported theme coordinator: $theme_hook"
    else
        theme_hook=""
    fi
    if (( follow_desktop )); then
        "$build_directory/metadots" --config "$config_directory/theme.json" theme follow desktop --dry-run >/dev/null
    fi
    local registered="$install_prefix/share/applications/$desktop_id.desktop"
    [[ ! -e "$registered" && ! -L "$registered" || -L "$registered" && "$(/usr/bin/readlink -- "$registered")" == "$install_prefix/bin/applications/$desktop_id.desktop" ]] || fail "Desktop registration is unmanaged: $registered"
}

install_executable() {
    /usr/bin/mkdir -p -- "$install_prefix/bin"
    backup_file "$install_prefix/bin/metadots"
    local temporary
    temporary="$(/usr/bin/mktemp "$install_prefix/bin/.metadots.XXXXXX")"
    /usr/bin/install -m755 -- "$build_directory/metadots" "$temporary"
    /usr/bin/mv -T -- "$temporary" "$install_prefix/bin/metadots"
}

install_desktop() {
    local desktop="$install_prefix/bin/applications/$desktop_id.desktop"
    local registered="$install_prefix/share/applications/$desktop_id.desktop"
    local icon="$install_prefix/share/icons/hicolor/symbolic/apps/$desktop_id-symbolic.svg"
    /usr/bin/install -Dm644 -- "$project_directory/assets/$desktop_id-symbolic.svg" "$icon"
    /usr/bin/mkdir -p -- "${desktop%/*}" "${registered%/*}"
    backup_file "$desktop"
    local temporary
    temporary="$(/usr/bin/mktemp "${desktop%/*}/.metadots-desktop.XXXXXX")"
    /usr/bin/awk -v executable="$install_prefix/bin/metadots" -v config="$config_directory/theme.json" \
        '/^Exec=/ { print "Exec=\"" executable "\" --config \"" config "\" %F"; next } { print }' \
        "$project_directory/assets/$desktop_id.desktop" > "$temporary"
    /usr/bin/chmod 644 -- "$temporary"
    /usr/bin/mv -T -- "$temporary" "$desktop"
    [[ -L "$registered" ]] || /usr/bin/ln -s -- "$desktop" "$registered"
    if [[ -x /usr/bin/update-desktop-database ]]; then /usr/bin/update-desktop-database "${registered%/*}"; fi
}

register_app() {
    /usr/bin/mkdir -p -- "${apps_list%/*}"
    backup_file "$apps_list"
    local temporary
    temporary="$(/usr/bin/mktemp "${apps_list%/*}/.metadots-apps.XXXXXX")"
    local entry="$desktop_id.desktop|$install_prefix/share/icons/hicolor/symbolic/apps/$desktop_id-symbolic.svg|MetaDots"
    if [[ -f "$apps_list" ]]; then
        /usr/bin/awk -F '|' -v id="$desktop_id.desktop" -v entry="$entry" \
            '$1 == id { if (!found++) print entry; next } { print } END { if (!found) print entry }' "$apps_list" > "$temporary"
        /usr/bin/chmod --reference="$apps_list" "$temporary"
    else
        printf '%s\n' "$entry" > "$temporary"
    fi
    /usr/bin/mv -T -- "$temporary" "$apps_list"
}

configure_theme() {
    [[ ! -e "$config_directory/theme.json" ]] || return 0
    if (( follow_desktop )); then
        "$install_prefix/bin/metadots" --config "$config_directory/theme.json" theme follow desktop >/dev/null
    else
        "$install_prefix/bin/metadots" --config "$config_directory/theme.json" theme set accent='#a8c5ee' >/dev/null
    fi
}

install_theme_hook() {
    [[ -n "$theme_hook" ]] || return 0
    local marker='    # MetaDots palette sync'
    local command="    queue_reload metadots \"$install_prefix/bin/metadots\" --config \"$config_directory/theme.json\" theme sync"
    if /usr/bin/grep -qFx "$marker" "$theme_hook" && /usr/bin/grep -qFx "$command" "$theme_hook"; then return 0; fi
    local temporary
    temporary="$(/usr/bin/mktemp "${theme_hook%/*}/.metadots-hook.XXXXXX")"
    /usr/bin/awk -v marker="$marker" -v command="$command" \
        '$0 == marker { if (getline <= 0 || $0 !~ /^    queue_reload metadots /) exit 1; next }
         { print } /^reload_applications\(\) \{/ { print marker; print command }' \
        "$theme_hook" > "$temporary"
    /usr/bin/bash -n "$temporary"
    backup_file "$theme_hook"
    # The coordinator is hard-linked into dotfiles; preserve its inode.
    /usr/bin/cat -- "$temporary" > "$theme_hook"
    /usr/bin/unlink -- "$temporary"
}

main() {
    parse_options "$@"
    validate_installation
    if (( dry_run )); then
        printf 'Executable: %s/bin/metadots\nDesktop: %s/bin/applications/%s.desktop\nDesktop registration: %s/share/applications/%s.desktop\nApp list: %s\nTheme JSON: %s/theme.json\nTheme hook: %s\n' \
            "$install_prefix" "$install_prefix" "$desktop_id" "$install_prefix" "$desktop_id" "$apps_list" "$config_directory" "${theme_hook:-none}"
        return
    fi
    install_executable
    install_desktop
    register_app
    configure_theme
    install_theme_hook
    printf 'Installed MetaDots.\n'
}

main "$@"
