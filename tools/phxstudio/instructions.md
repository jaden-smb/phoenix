# phxstudio — Phoenix Studio, the editor for the engine

## What it is for

One window for **making a game** with Phoenix: write its code, paint its sprites, lay out its
tilemaps and edit its data tables, see its baked bundle the way each console receives it, and
build and run it with a click.

The Studio works on **one game project at a time**, and a project can only change **its own
files** (see [Projects](#projects)). The engine's public API can be read for reference but not
changed, and the engine's own sources are out of reach. Engine maintainers start the Studio with
**`--engine-dev`** to get the whole checkout, the module graph and every gate and suite.

| View | What it does |
|---|---|
| **Editor** (Ctrl+1 in a project; Ctrl+3 with `--engine-dev`) | An **Explorer** over the project (the whole checkout with `--engine-dev`) and tabs of open documents, each in its own editor: **Code**, **Sprite / Pixel**, **Tilemap**, **Data table**. New sprites, maps, tables, images and code files from templates. **Quick open** (Ctrl+P) finds any project file or engine API header. |
| **Overview** (`--engine-dev` only) | The module dependency graph as built (layers from `tools/common/depcheck.py`, edges from the real `#include`s). Click a module for its summary, headers, backends, size and edges. The GBA / PSP / PC capability tiers from `caps.h` sit below. |
| **Assets** (Ctrl+2) | The project's bundles (every `.phxp` with `--engine-dev`), validated the way `ResourceCache::mount()` validates it. Live previews: textures re-encoded per render tier, animated sprite clips, tilemaps through the real parallax path, sounds on the real mixer, spawn tables and blob hex. **Edit source** opens the file an asset was baked from, and **bake** rebakes the project and reloads. |
| **Run** (Ctrl+3 in a project) | The project's launches from its `phxproject.json`: Play, Build, Bake, tests, console builds. With `--engine-dev`: the engine's games, editors, gates (`check`, `determinism`, `sanitize`, ...), every `make check` suite as a pass/fail chip, and its console builds. Output streams into a colour-coded console. **Compiler errors are links**: click one to open the file at that line. |

Everything the editors write is an ordinary **author file** that the bake reads: `.png`, `.sprdef`
or sprite `.json`, Tiled `.tmj`, phxbin `.json`, and source code. The Studio never writes a
`.phxp`; `phxpack` stays the only writer of engine blobs (docs/08 §1).

## Projects

A **project** is a folder with a `phxproject.json`: a game and its assets. The examples in this
checkout are projects (`examples/emberwing`, `examples/platformer`, `examples/miracle-player`,
`examples/tinyllm`), and **File > New project** creates one from a template. The template is a small
complete game (`src/main.cpp`), a hero sprite, a tileset and a level, all ready to Play. Start the
Studio with `--project DIR`, or pick one from the project picker it shows otherwise (recent
projects + the examples). **File > Open / Close project** switches.

**What the Studio can touch while a project is open** (enforced in one place,
`projectdoc.h: AccessPolicy`, and tested in `make editors`):

| | |
|---|---|
| **The project folder** | read + write: code, assets, `phxproject.json`, everything in it |
| **The engine's public API**: `engine/<module>/include/**.h` | **read-only**, under **ENGINE API** in the Explorer and in quick open. The editor shows a lock and refuses every edit, paste, undo and save |
| **The engine's documentation**: `docs/*.md`, `README.md`, each tool's `instructions.md` | read-only (Help > Studio guide) |
| **Everything else**: the engine's `.cpp` sources, its tools, tests, Makefile, other projects | **not opened at all**: not from the Explorer, quick open, `--open`, a dropped file, a compiler-error link, or a sprite sheet or tileset that points outside the project |

Paths are canonicalised before the check, so `../` and symlinks can't step outside the folder.
A project can't live inside the engine's own folders (`engine/`, `tools/`, `tests/`, `docs/`,
`cmake/`) and can't contain the engine. The project's launch commands are the one exception by
design: they are the project's own build configuration and run whatever they say.

**`phxproject.json`**:

```json
{ "name": "My Game",
  "description": "A Phoenix game",
  "source":  ["src"],
  "assets":  ["assets"],
  "bundles": ["build/my_game.phxp"],
  "launches": [
    { "label": "Play", "group": "play",
      "command": "make -C \"$PHX_ROOT\" -s play PROJECT=\"$PHX_PROJECT\"",
      "blurb": "Build, bake and run it", "needs": ["sdl2-config"], "windowed": true } ] }
```

| Key | Meaning |
|---|---|
| `name`, `description` | shown in the Studio; the name's slug (`my_game`) names the binary and the bundle |
| `source`, `assets` | code folders, and the folders `make game-assets` bakes (default `["src"]`, `["assets"]`) |
| `bundles` | extra `.phxp` files for the Assets view, relative to the project (it always lists the project's own `*.phxp` and `build/*.phxp`) |
| `launches[]` | the Run view: `label`, `command`, `group` (`play`, `build`, `test`, `console`, `tool`), `blurb` (status-bar help), `needs` (tools that must be installed; the launch is greyed out without them), `windowed` (it opens its own window) |

Launch commands run **from the project folder** with `$PHX_ROOT` (the Phoenix checkout) and
`$PHX_PROJECT` (the project) exported. A new project uses the engine's generic rules, so it needs
no edit to the engine's Makefile:

```bash
make game        PROJECT=path/to/project        # src/*.cpp against the PUBLIC headers -> build/<name>
make game-assets PROJECT=path/to/project [TIER=0|1|2]   # assets/ -> build/<name>.phxp (TIER 0 = GBA: <name>.t0.phxp)
make play        PROJECT=path/to/project        # both, then run it from the project folder
```

`make game-assets` runs `tools/common/bake_project.py`. It sends `.sprdef` and sprite `.json` to
`phxsprite`, `.tmj` to `phxtile`, `.wav` to `phxsnd` and phxbin tables to `phxbin` (the headers go
to `build/gen/`, which is on the game's include path), then `phxpack` merges them with the
remaining PNGs. Assets are named by their file stem (`"hero"_hash` is `assets/hero.sprdef`).

## Build & run

Needs SDL2 and a display. It is not part of `make check`, but its models and widget kit are, in the
`editors` and `pipeline` suites.

```bash
make studio                    # -> build/phxstudio (incremental object build)
./build/phxstudio              # the project picker (run from anywhere inside the repo)
./build/phxstudio --project examples/emberwing   # open a game project
./build/phxstudio --engine-dev # work on the engine itself (the whole checkout)
```

Options:

```bash
./build/phxstudio --project mygame --open src/main.cpp --open assets/hero.sprdef   # open documents
./build/phxstudio --tab assets                  # start on another view (editor|assets|run; overview: --engine-dev)
./build/phxstudio --fresh                       # don't reopen the last session's documents
./build/phxstudio --scale 3                     # UI scale (window pixels per canvas pixel)
./build/phxstudio --engine-dev --tab assets --bundle build/emberwing.phxp --asset sprite:hero
./build/phxstudio --project mygame --run Build   # queue a launch at startup by label (repeatable)
./build/phxstudio --engine-dev --run check       # an engine gate (make target or label)
./build/phxstudio --shot out.ppm                # render 45 frames, save the window as PPM, quit
./build/phxstudio --project mygame --script "open assets/level.tmj; wait 10; shot a.ppm; quit"
```

The window is **resizable**. The canvas follows the window at the integer UI scale:
Ctrl+= / Ctrl+- change it, and the Studio opens at 800×450 canvas pixels when the display has
room. The open documents, recent files and Explorer width are restored on the next start. They
are kept per project in `~/.config/phxstudio/session-*.txt`, and the recent projects in
`~/.config/phxstudio/projects.txt`.

## The Editor workspace

- **Explorer** (Ctrl+B toggles it; drag its edge to resize), titled with the project's name. Click
  a folder to expand it and a file to open it. Below the project, **ENGINE API** (read-only) lists
  every module's public headers. Type in the filter box to fuzzy-find across the whole project. Right-click an
  entry for Open, Open as text, New … here, and Copy path. The Explorer hides `.git` and
  per-config object directories. Files already open show in orange.
- **Tabs**: a dot marks unsaved edits. Click × or middle-click to close; closing an unsaved
  document asks first, and so does quitting. Ctrl+Tab / Ctrl+Shift+Tab switch tabs.
- **New** (File menu, the Explorer's + button, or the welcome page):
  - **Sprite**: a sheet PNG with N frames laid out left to right, plus a `.sprdef` (or `.json`)
    with an `idle` clip.
  - **Tilemap**: a `.tmj` of W×H tiles, optionally linked to a tileset PNG.
  - **Data table**: a phxbin `.json` from a `name:type, …` schema. A `str` field called
    `type` makes it a prefab table.
  - **Image / tileset**: a blank PNG.
  - **Code file**: from a template; `.h` files get an include guard.
- **Reload on change**: when a file changes on disk and you have no unsaved edits, it reloads.
  If you do, the Studio warns and keeps your version.
- **Save**: Ctrl+S saves the document; Ctrl+Shift+S saves every document. Saving an unchanged
  document does nothing, so files are never rewritten just because they were opened.

### Changing existing assets

Everything you make stays editable. Open it from the Explorer, or select it in the **Assets**
view and click **edit source** (or double-click the asset) to open the file it was baked from:

| Asset | Opens |
|---|---|
| texture / tileset | the PNG in the pixel editor, in [tile mode](#sprite--pixel-editor) |
| sprite | its `.sprdef` / sprite `.json`: sheet pixels, frame grid and clips |
| tilemap, spawns | the `.tmj` in the map editor |
| blob | the phxbin table `.json` |
| sound | no sound editor yet; the Studio names the `.wav` |

The bake names each asset after its file (`hero` is `assets/hero.sprdef`), which is how the
Studio finds the source. After you save, **bake** (next to **scan**, in a project) runs the
project's `game-assets` launch and reloads the bundle, so the preview shows the change. In the
map editor, double-click a palette tile to paint that tile.

### Code editor

A syntax-highlighted text editor for C/C++, JSON, Makefile, shell, Python, CMake, Markdown and
`.sprdef`. It shows line numbers, the current line, bracket matching, and a caret that keeps
UTF-8 text intact (em dashes and arrows edit as single characters).

| Keys | Action |
|---|---|
| typing, Enter, Backspace | auto-indent (one level more after `{ [ ( :`); `{` Enter `}` opens the block; `}` on a blank line dedents |
| arrows, Home/End, PgUp/PgDn, Ctrl+Home/End | move; add Shift to select; Ctrl+←/→ moves by word; Home toggles indent/column 0 |
| mouse | click, drag, Shift+click; double-click selects a word and triple-click a line; click the gutter to select lines; the wheel scrolls |
| Ctrl+A / C / X / V | select all / copy / cut / paste. With no selection, copy and cut take the whole line |
| Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) | undo / redo. Typing undoes a word at a time; an undo that returns to the saved text clears the unsaved dot |
| Tab / Shift+Tab, Ctrl+] / Ctrl+[ | indent / outdent (a multi-line selection shifts every line) |
| Ctrl+/ | toggle line comments (`//` or `#` by language) |
| Ctrl+D, Ctrl+Shift+K, Alt+↑/↓ | duplicate lines, delete lines, move lines |
| Ctrl+F, Ctrl+H, F3 / Shift+F3 | find (live, match count, case / whole-word toggles), replace (one or all, as one undo step), next / previous match |
| Ctrl+G | go to line (`line` or `line:col`) |

When a build in the Run view prints `file:line:col: error: …`, the file's gutter shows a red (or
yellow for warnings) marker with a squiggle under the column. Hover it for the message.

### Sprite / pixel editor

Opens a `.png` to paint it, or a sprite definition (`.sprdef` / sprite `.json`) to paint its sheet
**and** edit its frame grid and named clips.

| Keys | Tool |
|---|---|
| B / E / G / I | pencil / eraser / fill (Shift+click: every pixel of that colour) / picker |
| L / R / O | line / rectangle / ellipse (hold Shift for filled) |
| M | marquee: Ctrl+C / X / V, Delete, drag inside to move, arrows nudge, Enter/Esc to drop |
| H or middle-drag | pan. Ctrl+wheel zooms about the pointer; F fits |
| right button, Alt+click, X | paint with the secondary colour, pick a colour, swap the two |
| [ / ] | brush size |
| , / . and P | previous / next frame (or tile), play / pause the clip preview |

- **Toolbar toggles**: pixel grid, frame grid, onion skin (the previous frame shows beneath the
  current one), mirror painting, **GBA colours** (every painted colour snaps to BGR555, what the
  GBA can show), and the **8×8 tile check** (tiles over the 4bpp limit of 15 colours are
  outlined red).
- **Colour tab**: hex field, RGB sliders, hue strip, and palettes: this image's colours, DB16,
  PICO-8, Game Boy or Ember. Right-click a swatch to set the secondary colour.
- **Clips tab** (sprite defs): frame size and the clip list (add, delete, reorder). For each clip:
  name, first frame, count, fps and loop. Warnings flag anything the bake would reject
  (a clip past the last frame, fps 0, a sheet that isn't a whole number of frames).
- **Frame strip + preview**: thumbnails of every frame (the selected clip's frames are outlined)
  and the selected clip playing at its fps.
- **Tile mode** (plain PNGs such as tilesets): a grid of W×H tiles, each labelled with its **GID**
  (tile index + 1, the number a map stores). The tile size comes from a map or sprite def in the
  same folder that uses the image, or 8×8, and can be changed in the Image tab. The strip lists
  every tile; click one to zoom onto it. The map editor's **Edit tile** opens the tileset here,
  zoomed on that tile.
- **Image tab**: size and colour count, the GBA tile budget, the tile size and count, resize
  canvas (with an anchor), scale ×2, snap to GBA colours, append a frame (sprites) or **add a row
  of tiles** (images), and make a `.sprdef` for a plain PNG.
- **Saving** writes the PNG (indexed when it has ≤ 256 colours) and the def. `phxsprite` bakes
  them; the editors suite re-reads every saved def through the bake's own loaders.

### Tilemap editor

Edits Tiled `.tmj` maps: tile layers drawn with the **real tileset art**, per-tile collision,
entity spawns and parallax. It is the same panel as `phxtmap`
([instructions](../phxtmap/instructions.md)).

| Keys | Tool |
|---|---|
| B / E / G / R | brush (the tile or a multi-tile stamp) / eraser (right-drag with any tool too) / fill / rectangle |
| I / S | picker (drag a box to pick a stamp) / select (Ctrl+C turns the box into a stamp; Delete clears it) |
| T | spawns: click to place or select, drag to move (Shift = no snapping), Delete removes |
| Tab / Shift+Tab, V | next / previous layer, cycle the brush tile's collision flag |

- **Tiles tab**: the tileset palette. Click for a tile and drag for a stamp. **Double-click a
  tile** (or right-click > Edit tile, or **this tile**) to paint it in the pixel editor, zoomed on
  that tile. **tileset** opens the whole image. Right-click also sets the tile's collision, as do
  the none / solid / one-way / hazard buttons. Open maps redraw with the new art as soon as the
  tileset PNG is saved.
- **A map without a tileset image** draws swatch colours. **create tileset…** (Tiles or Map tab)
  writes a PNG of cols × rows tiles next to the map, optionally starting from the swatch colours,
  links it to the map, and opens it for painting.
- **Layers tab**: visibility (editor-only), add, delete, reorder, rename, and parallax factors.
  The **last layer is the gameplay layer** that the game's physics collides with.
- **Spawns tab**: the type to place comes from every prefab table's name column in the project, plus
  player/coin/enemy/spike and the map's own types. It lists the spawns and has an inspector for
  name, type, x/y and w/h.
- **Map tab**: size (resize with an anchor; spawns move with it), the tileset name (the texture the
  bake references), and the tileset image (saved as Tiled's `image`, so Tiled opens the map with
  its art too).
- **Toolbar toggles**: grid, the collision overlay (white = solid, yellow = one-way, red = hazard),
  spawns, dim the other layers, and the **parallax preview** (each layer scrolls by its factor as
  you pan).

### Data table editor

A spreadsheet over a phxbin record table (stats, prefabs, items, dialogue …). It is the same panel
as `phxentity` ([instructions](../phxentity/instructions.md)).

| Keys | Action |
|---|---|
| arrows, Tab / Shift+Tab, Home/End, PgUp/PgDn | move the cell cursor |
| Enter or F2 or double-click; or just type | edit the cell. Enter commits and moves down, Tab moves right, Esc cancels |
| + / − (Shift: ±10), Delete | step a number, clear the cell |
| Ctrl+D, Ctrl+Enter | duplicate the record, insert a record above |
| right-click a header / row number | rename / change type / move / delete a field; insert / duplicate / move / delete a record |

Every value you can enter is one the baked struct can hold. Integers **clamp** to their type
(`u8` … `i32`, and the Studio says so), `f32` rounds to float, and `strN` clips to N−1
characters (text already too long shows red). Field and struct names must be C identifiers.
The **Record** inspector on the right edits the selected record with typed fields. **Table**
shows the prefab vocabulary (the `type`/`name` column), duplicate names, and the baked size.

## How it works

It **dogfoods the engine**: one `phx::Game` on the App loop, the SDL window, and the **software
golden renderer**, the same pixels the consoles are diffed against. There is no separate UI
toolkit (docs/gui-editor-feasibility.md, Option A). The layers:

- **`phx/platform/desktop.h`** is the desktop-only extension of the platform seam. It adds a key
  event stream with modifiers, typed text, right/middle buttons, the wheel, the clipboard, a
  resizable window with an integer UI scale, cursors, window titles, dropped files, and a vetoable
  close. Only the `sdl` and `null` backends implement it. The null backend has a **scripted
  queue** (`phx_null_desktop_push`), so tool logic is testable headlessly. No console backend and
  no game ever sees it.
- **`tools/common/twk.h`** is the tool widget kit: an immediate-mode GUI over `phx::UI`'s
  rect/image primitives. It clips without a scissor (images trim their source rect, texel-exact at
  integer zooms) and draws on planes (base / popup / modal / tooltip) so menus block what is
  beneath them. It provides buttons, text fields (caret, selection, clipboard), number fields
  (type, wheel, drag-scrub), dropdowns, sliders, scrollbars, splitters, tabs, menus, context menus,
  modals and tooltips. The soft renderer alpha-*tests*, so "translucent" overlays are stippled.
  Icons are pixel art drawn from ASCII grids (`twk_icons.h`).
- **`projectdoc.h`**: the project file, the new-project template, and the **access policy** every
  open, save and New-file passes through (`Host::access`).
- **Headless document models**, all unit-tested in `make editors` (and `make pipeline`):
  - `textdoc.h`: text, undo, find
  - `syntax.h`: lexers
  - `pixeldoc.h`: pixels, sprite defs, colour math
  - `tools/common/png_write.h`: the PNG encoder
  - `project.h`: file kinds, tree, fuzzy match, diagnostics
  - `tools/phxtmap/editor.h`: maps
  - `tools/phxentity/editor.h`: tables
  - `model.h`: engine map, bundles, launches
- **Editor panels** (`ed_code.cpp`, `ed_sprite.cpp`, `ed_map.cpp`, `ed_table.cpp`) are
  `DocView`s behind the **`Host`** interface (`host.h`). The Studio is one host (`workspace.cpp`
  plus `main.cpp`). `solo.h` is another: a one-document window, which is what `phxtmap` and
  `phxentity` now are.
- **`jobs.h`** runs launches one at a time as `setsid sh -c '…'` process groups, so Stop signals
  the whole tree.

Engine facts the shell works around (the next tool will hit them too):

- **Sprites are camera-relative**, including `phx::ui` draws. The Assets tilemap preview moves the
  camera, so every chrome draw adds the camera back.
- **Tilemaps draw beneath every sprite** and there is no scissor, so the Assets preview shows the
  map through a "window" of opaque covers. The map *editor* doesn't use tilemap slots at all: each
  layer is composited on the CPU into one RGBA image that updates cell by cell, then drawn as one
  clipped, zoomed sprite.
- **Sprites sort by (layer, z, texture)**, so content made of several textures that must stack in
  order (a map's layers) takes one sub-layer each (`kSubImage + i`).
- **Textures are zero-copy**: the painted buffer *is* the texture, so strokes show on the next
  frame. A texture is re-uploaded only when its buffer moves (resize, undo).
- **Tilemap slots can't be freed**, so the Assets view caches one upload per bundle and asset.

## Scripting (repeatable checks)

`--script` takes commands inline (separated by `;`) or from a file (one per line; `#` comments).
Coordinates are canvas pixels. A script fixes the window at 640×360 and skips the saved session.

| Command | Effect |
|---|---|
| `click X Y`, `rclick X Y`, `dclick X Y`, `move X Y` | the pointer (a click is press then release on the next frame) |
| `drag X0 Y0 X1 Y1 [FRAMES]` | press, move over N frames (default 8), release |
| `wheel X Y N` | wheel notches at a point (+ = up) |
| `key [ctrl+][shift+][alt+]NAME` | a key: a letter, `enter`, `tab`, `esc`, `backspace`, `delete`, arrows, `home`, `end`, `pgup`, `pgdn`, `space`, `f1`… |
| `type TEXT` | typed text (the rest of the command) |
| `open PATH`, `tab NAME`, `project DIR` | open a document (relative to the project), switch the view, open a project |
| `wait N`, `shot FILE.ppm`, `quit` | idle N frames, capture the window, exit |

This is how the views were verified, including under ASan:

```bash
make studio BUILD=build/asan EXTRA_CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1"
ASAN_OPTIONS=detect_leaks=0 ./build/asan/phxstudio --project mygame --script "open assets/level.tmj; drag 250 150 300 170; key ctrl+z; quit"
```

## Notes & limits

- Games and editors opened from **Run** get their own windows. Their job ends when you close them,
  and only one job runs at a time; others queue.
- The Run view shells out through `sh`, and **Stop** needs `setsid` (util-linux). Both are
  Linux/macOS.
- The fonts are ASCII 5×7 on a 6-pixel pitch. Other UTF-8 characters display as a look-alike
  (— as -, → as >) or as a box, but are kept byte-for-byte on save.
- The code editor has no language server: highlighting is lexical, and errors come from real
  builds.
- The Assets view's texture tier previews re-encode in memory; the Studio never writes a bundle.
