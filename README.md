# MetaDots

MetaDots is a standalone Linux audio metadata editor written in C++20. It uses GTK 4 and TagLib, with no Qt dependency. Open music files, edit one track or a selection, and review changes before saving. Its flat interface uses recolorable symbolic icons and resizes on Wayland.

MetaDots is styled only by its own palette. It never loads your `~/.config/gtk-4.0/gtk.css` or `gtk-dark.css`: at start-up GTK is pointed at a private, per-process view of your config directory that links everything except those two files, and your real files are never modified.

All dialogs, including the file browser, stay inside the main window. Modal cards blur the content behind them. Transparent palette colors retain their opacity; desktop background blur depends on the compositor.

## Build and install

Requirements: CMake 3.22+, a C++20 compiler, GTK 4.18+, TagLib 2+, and nlohmann-json 3.10+ development packages.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
bash scripts/install.sh --standalone --dry-run
bash scripts/install.sh --standalone
```

The user installer places the executable in `~/.local/bin/metadots`, creates a MetaDots desktop launcher, and registers it in the application menu. CMake also supports a conventional prefix installation:

```bash
cmake --install build --prefix "$HOME/.local"
```

Launch `metadots`, pass files or folders as arguments, or drop files into the window. Use Ctrl+O to open, Ctrl+S to review and save, Ctrl+F to filter, and Escape to dismiss a modal. Select several files with Ctrl+click, Shift+click or the Select all button; the Open browser selects the same way, and double-click opens a file or folder. When you edit a file, the shared tags you changed (everything except title and track) are filled in on the next file you open, ready to review and save. A cover you choose or remove is copied the same way. Untick "Copy my last edits to the next file" to turn that off. Mixed fields stay untouched until you edit that field. Emptying a field, or using its clear icon, removes that tag from the selected tracks.

Supported formats include MP3, FLAC, Ogg Vorbis, Opus, MP4/M4A, WAV, AIFF, APE, WavPack and the other formats handled by TagLib. The editor exposes title, artist, album, album artist, track, disc, date, genre, composer, comment and copyright. Other tags and embedded artwork are preserved when saving. Saves use a temporary sibling file and reject conflicting external modifications. Each file is committed separately, and failed edits stay pending. Symlinks and multiply linked files are rejected for writing.

Explicit: the Explicit box under Title adds or removes the boxed E (🅴, U+1F174) that Apple Music shows after explicit titles, at the end of the title of every selected file. Tags can't carry colours, so it is a text character: it draws in whatever colour the player uses for titles, and players without a font for it may show a blank box. It is part of the title text only (no advisory tag is written), it is not copied to the next file, and files with no title are skipped. A title that already ends in the character shows as explicit.

Cover art: the editor shows the selected file's cover, or "Mixed covers" when the selection differs. Choose image… opens a JPEG or PNG from the in-window browser (it starts in the file's folder), or drop an image on the cover. Remove cover stages a removal. Both work on every selected file, are reviewed and saved like other edits, and replace only the front-cover, untyped and "Other" pictures; back covers, artist photos and other typed pictures stay. Covers are stored as ID3 `APIC`, FLAC and Ogg pictures, or MP4 `covr`, and images over 16 MB are rejected.

## CLI theming

The last palette is saved in `$XDG_CONFIG_HOME/metadots/theme.json` (normally `~/.config/metadots/theme.json`). CLI changes validate and atomically replace that JSON file before the running interface reloads. A directory watcher reloads CLI changes immediately, with a 500 ms fallback for followed sources, without restarting or losing edits. Invalid JSON leaves the active palette in place.

```bash
metadots theme show
metadots theme path
metadots theme set accent='#a8c5ee' background='#e00e1e36'
metadots theme set background='Qt.rgba(14 / 255, 30 / 255, 54 / 255, 0.88)'
metadots theme import /absolute/path/palette.json
metadots theme follow file /absolute/path/palette.json
metadots theme sync
metadots theme set accent='#dab6ee' --dry-run
metadots --config /absolute/path/theme.json theme show
```

Theme keys: `background`, `surface`, `surface_alt`, `text`, `muted`, `accent`, `accent_text`, `border`, `selection`, `error`. All colors are configurable. Six-digit colors use `#RRGGBB`. Eight-digit colors use **alpha first**, `#AARRGGBB`: `#e00e1e36` has RGB `#0e1e36` and opacity `224/255`. `Qt.rgba(r, g, b, a)` expressions accept normalized numbers or divisions such as `14 / 255`; expressions are parsed as data and never executed.

An import copies a palette once; `follow file` follows later edits and atomic replacements at that path. Manual `set` commands stop following the previous source. `--dry-run` previews a change without creating or editing files. Generic JSON palettes and color-property files are accepted, including common background/foreground/primary aliases.

Example palette:

```json
{
  "version": 1,
  "name": "Blue ink",
  "colors": {
    "background": "#e00e1e36",
    "surface": "#192b43",
    "surface_alt": "#233851",
    "text": "#e4edf9",
    "muted": "#a7b6cd",
    "accent": "#aac5ef",
    "accent_text": "#142036",
    "border": "#384b65",
    "selection": "#30466a",
    "error": "#efa7ad"
  }
}
```

## Validation

`ctest` covers color parsing, GTK config isolation (hidden `gtk.css`, write-through links, stale-view cleanup), palette persistence, CLI behavior, desktop registration, tag round trips, artwork preservation, and conflicting saves. A real display is needed for the UI checks:

```bash
build/metadots --config /tmp/metadots-preview.json --ui-self-test --screenshots /tmp/metadots-preview
```

For memory and undefined-behavior diagnostics, configure a separate build with `-DMETADOTS_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`.

Inspired by the file-and-tags workflow of [Kid3](https://github.com/KDE/kid3). MetaDots is an independent implementation.
