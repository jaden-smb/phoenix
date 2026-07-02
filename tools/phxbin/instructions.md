# phxbin — the data-table converter

## What it is for

Bakes a **JSON record table** (item stats, enemy parameters, dialogue lines, prefab data —
anything tabular) into a `.phxbin` intermediate holding one binary blob asset, and optionally
emits a **generated C header** with the matching POD struct + typed accessors. The game reads
the table zero-copy from the bundle with no parsing at runtime — JSON never ships. Edit these
tables interactively with the `phxentity` GUI editor. `phxpack` merges the `.phxbin` into the
final bundle.

## How it works

Input schema:

```json
{ "struct": "ItemRecord",
  "fields": [ {"name":"id","type":"u16"}, {"name":"price","type":"u32"}, {"name":"atk","type":"i16"} ],
  "records": [ {"id":1,"price":100,"atk":5}, {"id":2,"price":250,"atk":-3} ] }
```

Field types: `u8 i8 u16 i16 u32 i32 f32`. Records are packed little-endian at **natural C
alignment**, so the generated struct matches the blob by construction (a `static_assert` on the
stride in the generated header guards it). Blob layout: `[u32 count][u32 stride][records…]`.

With `--header`, a `.gen.h` is written containing the `struct ItemRecord { … }`, the
static_assert, and an accessor for indexing the mounted blob.

## Build & run

```bash
make check              # builds build/phxbin along the way (or: make tools)

./build/phxbin --out FILE.phxbin [--name N] [--header FILE.gen.h] [--target 0|1|2] <data.json>
```

The asset name defaults to the JSON's stem; the game fetches it as a blob
(`res->blob("items"_hash)`) and casts through the generated struct.

## Example

```bash
./build/phxbin  --out items.phxbin --name items --header src/items.gen.h items.json
./build/phxpack --out assets.phxp items.phxbin
```

Try it on the fixture `make check` drops:
`./build/phxbin --out /tmp/i.phxbin --header /tmp/i.gen.h build/p_items.json`.
Verified by `make pipeline` (bake → mount → field-exact readback + header emit, plus the
`phxentity` editor round-trip re-baking through this same builder).
