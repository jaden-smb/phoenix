# phxentity — the GUI entity/prefab table editor

## What it is for

A keyboard-driven editor for the **phxbin author JSON** — the typed record tables games use
for entity/prefab parameters, item stats, tuning values, and similar data. It edits the author
format (never engine blobs, docs/08 §1); `phxbin`/`phxpack` bake what it saves, and the
pipeline suite proves an edited table still bakes through the real `phxbin` builder.

## How it works

It **dogfoods the engine**: the same App loop, SDL window, software renderer, and
immediate-mode UI the games use. The document model (`editor.h: BinDoc`) is separated from
this GUI shell and unit-tested headlessly (load → step/clamp → clone/delete → save →
re-load). Values are stepped, never typed: each field **clamps to its declared type's range**
(`u8 i8 u16 i16 u32 i32`), so an edit can never produce a value the baked struct cannot hold.

The table on screen: one row per record (row index on the left), one column per field
(header row shows the field names; the selected column highlights), the struct name at the
top, and a help line at the bottom.

## Build & run

Needs SDL2 and a display (not part of `make check`).

```bash
make entity                                # -> build/phxentity

./build/phxentity items.json               # edit (saves in place)
./build/phxentity --out copy.json items.json
./build/phxentity --new Enemy --fields hp:u16,atk:i8,speed:u8 --out enemies.json
```

`--new NAME --fields a:type,b:type` starts a **fresh table** from a schema — no input file or
hand-written JSON needed (types: `u8 i8 u16 i16 u32 i32`; it opens with one zeroed record).
An existing input must have the phxbin shape (`struct` + `fields` + `records`); a malformed
file is refused with a `line L, col C` parse error. For reference, the shape is:

```json
{ "struct": "ItemRecord",
  "fields": [ {"name":"id","type":"u16"}, {"name":"price","type":"u32"}, {"name":"atk","type":"i16"} ],
  "records": [ {"id":1,"price":100,"atk":5} ] }
```

`PHX_MAX_FRAMES=60 ./build/phxentity …` runs a bounded smoke (boots, runs, exits) for scripts.

## Controls

| Input | Action |
|-------|--------|
| **arrows / WASD** | move the cell cursor (record row / field column) |
| **Z** | +1 on the selected cell |
| **X** | −1 on the selected cell |
| **Q** / **E** | −10 / +10 |
| **C** | NEW record (a clone of the cursor row) |
| **V** | DELETE the cursor row |
| **Enter** | **save** to `--out` (a `*` next to the struct name = unsaved edits) |
| window close | quit |

The grid scrolls automatically to keep the cursor row visible.

## Typical workflow

```bash
make entity
./build/phxentity items.json                       # tune values, C to add records, Enter
./build/phxbin  --out items.phxbin --header src/items.gen.h items.json
./build/phxpack --out assets.phxp items.phxbin
```
