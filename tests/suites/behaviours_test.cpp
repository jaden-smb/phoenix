// tests/suites/behaviours_test.cpp — the stock behaviours (phx/runtime/behaviours.h) driven only by
// data: a level whose prefabs list PlatformerController, CameraFollow, Pickup, Patrol, Hazard,
// Checkpoint and Exit in their `components` columns, baked by the real converters and played with
// scripted input. No gameplay code in this test: it only reads back what happened. Runs on both
// scalar tiers (determinism: TIER=gba_sim).
#include "phx/runtime/app.h"
#include "phx/runtime/behaviours.h"

#include "builders.h"                 // tools/phxpack: the converters' bake logic
#include "editor.h"                   // tools/phxtmap: TmapDoc
#include "fixtures/png_fixtures.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void phx_null_set_max_frames(uint64_t n);
extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }
void write_file(const char* path, const void* data, size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, n, f); std::fclose(f); }
}
std::vector<uint8_t> wav(uint32_t frames) {                 // a 16-bit mono square wave
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); };
    auto tag   = [](std::vector<uint8_t>& v, const char* t) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(t[i])); };
    std::vector<uint8_t> w;
    tag(w, "RIFF"); put32(w, 36 + frames * 2); tag(w, "WAVE");
    tag(w, "fmt "); put32(w, 16); put16(w, 1); put16(w, 1); put32(w, 22050); put32(w, 44100); put16(w, 2); put16(w, 16);
    tag(w, "data"); put32(w, frames * 2);
    for (uint32_t i = 0; i < frames; ++i) put16(w, uint16_t((i & 16) ? 6000 : -6000));
    return w;
}

// Every behaviour is a component named in `components` and tuned by a Component_field column.
const char* kPrefabs =
"{ \"struct\":\"Prefab\","
"  \"fields\":[ {\"name\":\"type\",\"type\":\"str16\"}, {\"name\":\"sprite\",\"type\":\"str16\"},"
"              {\"name\":\"w\",\"type\":\"u8\"}, {\"name\":\"h\",\"type\":\"u8\"}, {\"name\":\"body\",\"type\":\"u8\"},"
"              {\"name\":\"layer\",\"type\":\"u16\"}, {\"name\":\"mask\",\"type\":\"u16\"},"
"              {\"name\":\"components\",\"type\":\"str64\"}, {\"name\":\"Pickup_value\",\"type\":\"i16\"},"
"              {\"name\":\"Pickup_sound\",\"type\":\"str16\"}, {\"name\":\"Patrol_range\",\"type\":\"i16\"},"
"              {\"name\":\"Exit_target\",\"type\":\"str16\"} ],"
"  \"records\":[ {\"type\":\"player\",\"sprite\":\"hero\",\"w\":6,\"h\":8,\"body\":1,\"layer\":1,\"mask\":30,"
"                 \"components\":\"PlatformerController CameraFollow\"},"
"               {\"type\":\"coin\",\"w\":4,\"h\":4,\"layer\":2,\"mask\":1,\"components\":\"Pickup\",\"Pickup_value\":3,\"Pickup_sound\":\"blip\"},"
"               {\"type\":\"slime\",\"w\":6,\"h\":8,\"body\":1,\"layer\":4,\"mask\":1,\"components\":\"Patrol Hazard\",\"Patrol_range\":10},"
"               {\"type\":\"flag\",\"w\":4,\"h\":8,\"layer\":8,\"mask\":1,\"components\":\"Checkpoint\"},"
"               {\"type\":\"door\",\"w\":4,\"h\":8,\"layer\":16,\"mask\":1,\"components\":\"Exit\",\"Exit_target\":\"level2\"} ] }";
// The hero's clips form a state machine (trans lines) the controller drives with its events:
// idle -move-> walk -stop-> idle, jump from anywhere, land on touching down, back to idle when the
// one-frame land clip is done.
const char* kSprdef = "sheet b_sheet.png 2 2\nclip idle 0 1 0 0\nclip walk 1 3 8 1\nclip jump 3 1 0 0\n"
                      "clip land 2 1 20 0\ntrans idle walk move\ntrans walk idle stop\ntrans * jump jump\n"
                      "trans * jump fall\ntrans jump land land\ntrans land idle done\n";

bool bake() {
    write_file("build/b_sheet.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/b_tiles.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/b_hero.sprdef", kSprdef, std::strlen(kSprdef));
    write_file("build/b_prefabs.json", kPrefabs, std::strlen(kPrefabs));
    const std::vector<uint8_t> j = wav(400), b = wav(300);
    write_file("build/b_jump.wav", j.data(), j.size());
    write_file("build/b_blip.wav", b.data(), b.size());
    // 40x6 tiles: a floor on row 5, one hazard tile (gid 2) on row 4 at column 30
    phxtool::TmapDoc d = phxtool::TmapDoc::blank(40, 6, 8, 8, "tiles");
    for (int x = 0; x < 40; ++x) d.set_tile(0, x, 5, 1);
    d.set_tile(0, 30, 4, 2);
    d.set_tile_flag(1, kTileSolid);
    d.set_tile_flag(2, kTileHazard);
    d.add_spawn("player", 20, 36);
    d.add_spawn("coin", 70, 36);                              // past where the opening jump lands
    d.add_spawn("flag", 80, 36);
    d.add_spawn("slime", 110, 36);
    d.add_spawn("door", 200, 36);
    if (!d.save_file("build/b_level.tmj")) return false;
    phxtool::BundleWriter w(2);
    return phxtool::build_sprite(w, "build/b_hero.sprdef", "hero") && phxtool::build_png(w, "build/b_tiles.png", "tiles") &&
           phxtool::build_tmj(w, "build/b_level.tmj", "level") && phxtool::build_bin(w, "build/b_prefabs.json", "prefabs") &&
           phxtool::build_wav(w, "build/b_jump.wav", "jump") && phxtool::build_wav(w, "build/b_blip.wav", "blip") &&
           w.write("build/b_level.phxp");
}

constexpr uint32_t kA = 1u << 4, kRight = 1u << 3;
uint32_t g_script[200];

struct BehaviourGame final : Game {
    ResourceCache* res = nullptr;
    Level          level;
    PhysicsWorld   physics;
    Behaviours     beh;
    uint32_t       frame = 0;
    ecs::Entity    slime = ecs::kInvalid;
    vec2           start{}, flag{};
    bool           started_ok = false, jumped = false, walked = false, camera_moved = false;
    bool           idle_first = true, jump_clip = false, land_clip = false, land_done = false, walk_after = false;
    int            slime_min = 1 << 20, slime_max = -(1 << 20);
    NameHash       exit_seen = 0;
    int32_t        deaths_before_tile = -1, deaths_after_tile = -1;

    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        check(res->mount(app.platform(), "build/b_level.phxp") == Status::Ok, "mount the baked level");
        check(level.load(app, *res, &physics) == Status::Ok, "load it");
        physics.set_gravity(vec2{ s_from_int(0), s_from_int(400) });
        beh.start(app, level, *res, physics);
        ecs::World& w = app.world();
        slime = level.find(w, "slime"_hash);
        flag = w.get<Transform>(level.find(w, "flag"_hash))->pos;
        start = beh.player() != ecs::kInvalid ? w.get<Transform>(beh.player())->pos : vec2{};
        started_ok = beh.player() == level.find(w, "player"_hash) && beh.respawn_point().x == start.x &&
                     start.x == s_from_int(24) &&                 // the start override (Play from here)
                     w.has<PlatformerController>(beh.player()) && w.has<CameraFollow>(beh.player()) &&
                     w.has<Patrol>(slime) && w.has<Hazard>(slime) && w.get<Patrol>(slime)->range == 10;
    }
    void on_fixed_update(App& app, scalar dt) override {
        ecs::World& w = app.world();
        const ecs::Entity p = beh.player();
        ++frame;
        if (frame == 150) w.get<Transform>(p)->pos = vec2{ s_from_int(200), s_from_int(36) };     // onto the door
        if (frame == 170) { deaths_before_tile = beh.counter("deaths"_hash);
                            w.get<Transform>(p)->pos = vec2{ s_from_int(244), s_from_int(34) }; } // onto the hazard tile
        beh.update(app, dt);
        if (frame == 170) deaths_after_tile = beh.counter("deaths"_hash);
        if (const Body* b = w.get<Body>(p); b && frame >= 11 && frame <= 14 && b->vel.y < s_from_int(0)) jumped = true;
        if (const Animator* an = w.get<Animator>(p); an && frame > 30 && frame < 120 && an->clip == 1) walked = true;
        if (const Animator* an = w.get<Animator>(p)) {             // the state machine's path
            if (frame < 9 && an->clip != 0) idle_first = false;
            if (frame >= 11 && frame <= 14 && an->clip == 2) jump_clip = true;
            if (jump_clip && an->clip == 3) land_clip = true;
            if (land_clip && !land_done && an->clip == 0) land_done = true;
            if (land_done && an->clip == 1) walk_after = true;
        }
        if (beh.camera().pos.x > s_from_int(0)) camera_moved = true;
        if (w.is_alive(slime)) {
            const int x = s_to_int(w.get<Transform>(slime)->pos.x);
            slime_min = x < slime_min ? x : slime_min;
            slime_max = x > slime_max ? x : slime_max;
        }
        if (beh.exit()) { exit_seen = beh.exit(); beh.clear_exit(); }
    }
    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        r.begin_frame(beh.camera());
        level.draw(r);
        draw_sprites(app.world(), r);
        r.end_frame();
    }
};
} // namespace

int main() {
    check(bake(), "bake the level with the real converters");
    for (uint32_t f = 0; f < 200; ++f) g_script[f] = f < 9 ? 0u : f == 9 ? kA : f < 140 ? kRight : 0u;
    set_start_override(24, 36);                               // "Play from here": 4 px right of the spawn
    phx_null_set_button_script(g_script, 200);
    phx_null_set_max_frames(190);

    Config cfg = Config::from_defaults();
    cfg.title = "behaviours_test"; cfg.width = 160; cfg.height = 48;
    cfg.total_ram = 8u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;
    App app(cfg);
    static BehaviourGame g;
    check(app.run(&g) == 0, "the App runs the level");

    check(g.started_ok, "start(): the player is the PlatformerController, its spawn the respawn point; components from data");
    check(g.jumped, "PlatformerController: A on the ground jumps");
    check(app.audio().plays() >= 2, "the jump sound and the pickup sound play (by name, from the bundle)");
    check(g.walked, "PlatformerController: the 'walk' clip plays while running");
    check(g.idle_first && g.jump_clip, "state machine: idle until the 'jump' event takes '* -> jump'");
    check(g.land_clip, "state machine: touching down sends 'land' (jump -> land)");
    check(g.land_done, "state machine: the non-looping land clip ends and 'done' returns to idle");
    check(g.walk_after, "state machine: running on the ground sends 'move' (idle -> walk)");
    check(g.beh.counter("coins"_hash) == 3, "Pickup: the coin added its value (3) to 'coins' and vanished");
    check(g.beh.respawn_point().x == g.flag.x, "Checkpoint: touching the flag made it the respawn point");
    check(g.beh.counter("deaths"_hash) >= 1, "Hazard: the patrolling slime sent the player back");
    check(g.slime_min >= 98 && g.slime_max <= 122 && g.slime_max - g.slime_min >= 14,
          "Patrol: the slime walks its range (110 +- 10) both ways");
    check(g.camera_moved, "CameraFollow: the camera follows the player along the level");
    check(g.exit_seen == "level2"_hash, "Exit: touching the door reports its target");
    check(g.deaths_after_tile == g.deaths_before_tile + 1, "a hazard TILE sends the player back too");

    std::printf("behaviours_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail ? "BEHAVIOURS FAIL\n\n" : "BEHAVIOURS PASS\n\n");
    return g_fail ? 1 : 0;
}
