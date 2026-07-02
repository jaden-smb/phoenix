# phxtile — the tilemap converter

## What it is for

Bakes a **Tiled map** (`.tmj`, the JSON export of the open [Tiled](https://www.mapeditor.org/)
editor — also what our own `phxtmap` GUI editor writes) into a `.phxtmap` intermediate holding
two assets that share one name: a **Tilemap** (tile-index layers + per-layer parallax factors)
and a **Spawns** table (the object groups, as typed spawn points the game maps onto its ECS).
`phxpack` merges the `.phxtmap` into the final bundle.

## How it works

- **Tile layers** → `uint16` index layers (0 = empty; Tiled GIDs pass through when
  `firstgid == 1`, flip/rotate bits are stripped). All layers of the map stack into one
  Tilemap asset, drawn per layer at runtime. Convention: the **last** tile layer is the
  gameplay/solid one (the platformer's physics reads it; backdrop layers sit before it).
- **Per-layer parallax** → Tiled's native `parallaxx`/`parallaxy` layer properties are carried
  into the blob as Q16 factors; the runtime feeds them to `set_tilemap_parallax` (½ = classic
  half-speed background, 0 = screen-fixed sky).
- **Object groups** → a flat spawn table: `{type-hash, x, y, w, h}` per object (the object's
  `type`/`class`, falling back to its name). The game resolves `"player"_hash` etc. at spawn.
- The first tileset's name becomes the tileset-texture reference (bake the matching sheet PNG
  under that name).

Requirements: CSV-encoded tile layers (Tiled's default), `firstgid` 1.

## Build & run

```bash
make check              # builds build/phxtile along the way (or: make tools)

./build/phxtile --out FILE.phxtmap [--name N] [--target 0|1|2] <map.tmj>
```

The asset name defaults to the map file's stem; the game looks the map and its spawns up by
that same name (`res->tilemap("level"_hash)`, `res->spawns("level"_hash)`).

## Example

```bash
./build/phxtile --out level.phxtmap --name level level.tmj
./build/phxpack --out assets.phxp tiles.png level.phxtmap
```

Try it on the fixture `make check` drops: `./build/phxtile --out /tmp/l.phxtmap build/p_level.tmj`.
Verified by `make tiled` (parse → bake → mount → render + spawn resolution + parallax
round-trip) and `make pipeline`.
