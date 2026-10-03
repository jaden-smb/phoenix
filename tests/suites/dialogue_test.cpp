// tests/suites/dialogue_test.cpp — dialogue as data (phx/runtime/dialogue.h, tools/phxpack/dialogue.h):
//   * the .dlg model: expressions, JSON round trip, validation (errors vs warnings), compile;
//   * the bake: a Dialogue asset that mounts, and a corrupt one that is refused;
//   * the runtime DialogueRunner and the Studio's DlgSim stepping the same conversation in lockstep
//     (conditions skip / hide, effects change variables, typewriter reveal, choice selection);
//   * a game flow played from data with scripted input: a "talk" cutscene screen, then a level where
//     the player collects coins, walks to a Talk sign, presses Up, and trades 2 coins for luck —
//     the conversation's effect lands in the flow's totals (what the HUD counts).
// Both scalar tiers (determinism: TIER=gba_sim).
#include "phx/runtime/app.h"
#include "phx/runtime/flow.h"

#include "builders.h"                 // tools/phxpack: the converters' bake logic
#include "editor.h"                   // tools/phxtmap: TmapDoc
#include "ascii_font.h"               // tools/common: the 5x7 font atlas
#include "png_write.h"                // tools/common

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" void phx_null_set_max_frames(uint64_t n);
extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }
void write_file(const char* path, const std::string& s) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
}

const char* kFlow =
"{ \"struct\":\"Screen\","
"  \"fields\":[ {\"name\":\"name\",\"type\":\"str16\"}, {\"name\":\"kind\",\"type\":\"str16\"}, {\"name\":\"map\",\"type\":\"str16\"},"
"              {\"name\":\"text\",\"type\":\"str64\"}, {\"name\":\"dialogue\",\"type\":\"str16\"} ],"
"  \"records\":[ {\"name\":\"intro\",\"kind\":\"talk\",\"text\":\"CHAPTER 1\",\"dialogue\":\"opening\"},"
"               {\"name\":\"one\",\"kind\":\"level\",\"map\":\"d_l1\"},"
"               {\"name\":\"end\",\"kind\":\"end\",\"text\":\"THE END\"} ] }";
const char* kPrefabs =
"{ \"struct\":\"Prefab\","
"  \"fields\":[ {\"name\":\"type\",\"type\":\"str16\"}, {\"name\":\"w\",\"type\":\"u8\"}, {\"name\":\"h\",\"type\":\"u8\"},"
"              {\"name\":\"body\",\"type\":\"u8\"}, {\"name\":\"layer\",\"type\":\"u16\"}, {\"name\":\"mask\",\"type\":\"u16\"},"
"              {\"name\":\"components\",\"type\":\"str64\"}, {\"name\":\"Talk_conversation\",\"type\":\"str16\"} ],"
"  \"records\":[ {\"type\":\"player\",\"w\":6,\"h\":8,\"body\":1,\"layer\":1,\"mask\":6,\"components\":\"PlatformerController\"},"
"               {\"type\":\"coin\",\"w\":4,\"h\":4,\"layer\":2,\"mask\":1,\"components\":\"Pickup\"},"
"               {\"type\":\"sign\",\"w\":8,\"h\":8,\"layer\":4,\"mask\":1,\"components\":\"Talk\",\"Talk_conversation\":\"sign\"} ] }";

// The game's dialogue: a cutscene and the sign (its starter conversation, plus an opening).
phxtool::DlgDoc game_dialogue() {
    phxtool::DlgDoc d = phxtool::dlg_starter();
    phxtool::DlgConversation op;
    op.name = "opening";
    op.nodes = { phxtool::DlgNode{ "", "Narrator", "Long ago...", "", "", "", {} },
                 phxtool::DlgNode{ "", "Narrator", "It begins.", "", "", "", {} } };
    d.convs.push_back(op);
    return d;
}

bool bake() {
    std::vector<uint32_t> atlas(size_t(phxtool::kAsciiFontW) * phxtool::kAsciiFontH);
    phxtool::build_ascii_font(atlas.data());
    if (!phxtool::png_write_file("build/d_font.png", atlas.data(), phxtool::kAsciiFontW, phxtool::kAsciiFontH)) return false;
    write_file("build/d_flow.json", kFlow);
    write_file("build/d_prefabs.json", kPrefabs);
    write_file("build/d_dialogue.dlg", phxtool::dlg_to_json(game_dialogue()));
    phxtool::TmapDoc m = phxtool::TmapDoc::blank(20, 6, 8, 8, "font");
    for (int x = 0; x < 20; ++x) m.set_tile(0, x, 5, 1);
    m.set_tile_flag(1, kTileSolid);
    m.add_spawn("player", 20, 36);
    m.add_spawn("coin", 36, 36);
    m.add_spawn("coin", 52, 36);
    m.add_spawn("sign", 70, 36);
    if (!m.save_file("build/d_l1.tmj")) return false;
    phxtool::BundleWriter w(2);
    return phxtool::build_png(w, "build/d_font.png", "font") && phxtool::build_tmj(w, "build/d_l1.tmj", "d_l1") &&
           phxtool::build_bin(w, "build/d_flow.json", "flow") && phxtool::build_bin(w, "build/d_prefabs.json", "prefabs") &&
           phxtool::build_dialogue(w, "build/d_dialogue.dlg", "dialogue") && w.write("build/d_game.phxp");
}

// ---- the model (host) ----------------------------------------------------------------------
void model_checks() {
    using namespace phxtool;
    DlgOp op{};
    std::vector<DlgOp> ops;
    std::string err;
    check(dlg_parse_cond("coins >= 5", op) && op.var == "coins"_hash && op.op == kDlgGe && op.value == 5 &&
          dlg_parse_cond("key", op) && op.op == kDlgNe && op.value == 0 && dlg_parse_cond("!key", op) && op.op == kDlgEq &&
          dlg_parse_cond("hp<-2", op) && op.op == kDlgLt && op.value == -2 && dlg_parse_cond("", op) && op.op == kDlgNone,
          "if: comparisons, bare / negated flags, empty = always");
    check(!dlg_parse_cond("coins >= lots", op, &err) && !dlg_parse_cond("two words", op) && !dlg_parse_cond("x == 99999", op),
          "if: a bad number, a bad name or an out-of-range value is an error");
    check(dlg_parse_effects("coins -= 5, key = 1, met, hp += 2", ops) && ops.size() == 4 && ops[0].op == kDlgSub &&
          ops[0].value == 5 && ops[1].op == kDlgSet && ops[2].op == kDlgSet && ops[2].value == 1 && ops[3].op == kDlgAdd,
          "do: -=, =, a bare flag (= 1), +=");
    check(!dlg_parse_effects("coins -= many", ops) && !dlg_parse_effects("a b = 1", ops), "do: malformed effects fail");

    const DlgDoc d = game_dialogue();
    DlgDoc r;
    check(dlg_from_json(dlg_to_json(d), r) && r.convs.size() == 2 && r.convs[0].nodes.size() == 4 &&
          r.convs[0].nodes[1].choices.size() == 3 && r.convs[0].nodes[1].choices[1].effects == "coins -= 2, lucky = 1" &&
          r.speakers.size() == 1 && r.convs[1].nodes[0].speaker == "Narrator", "JSON round trip");
    check(!dlg_has_errors(dlg_validate(d)), "the starter + an opening validate");

    DlgDoc bad = d;
    bad.convs[0].nodes[0].next = "nowhere";
    bad.convs[0].nodes[1].choices[0].cond = "coins >=";
    bad.convs[1].name = "sign";
    const auto probs = dlg_validate(bad);
    int errors = 0;
    for (const auto& p : probs) errors += p.error;
    check(errors == 3, "validation: an unknown next, a bad condition, a duplicate conversation name");
    DlgCompiled c;
    check(!dlg_compile(bad, c, &err) && !err.empty(), "the compiler refuses a dialogue with errors");
    DlgDoc warn = d;
    warn.convs[0].nodes.push_back(DlgNode{ "orphan", "", "never said", "", "", "", {} });
    const auto w = dlg_validate(warn);
    check(!dlg_has_errors(w) && !w.empty() && w.back().what.find("never be reached") != std::string::npos,
          "an unreachable line is a warning, not an error");

    DlgDoc ren = d;
    DlgDoc::rename_node(ren.convs[0], 2, "advice");
    check(ren.convs[0].nodes[1].choices[0].next == "advice", "renaming a line id follows its references");

    check(dlg_compile(d, c, &err) && c.convs.size() == 2 && c.nodes.size() == 6 && c.choices.size() == 3 &&
          c.speakers.size() == 2 && c.nodes[0].next == 1 && c.nodes[2].next == kDlgEnd &&
          c.choices[2].next == kDlgEnd && c.nodes[3].cond.op == kDlgNe && c.choices[1].op_count == 2,
          "compile: nexts resolve to node indices (end, following), conditions, effects, auto speakers");

    // the simulator: walking the compiled tables
    DlgSim s;
    check(s.start(c, "sign") && std::string(s.text()).find("Welcome") == 0 && std::string(s.speaker()) == "Sign",
          "sim: the conversation starts at its first line");
    s.advance();
    check(s.visible.size() == 2, "sim: a choice whose condition fails is hidden (no coins yet)");
    s.vars["coins"_hash] = 3;
    s.start(c, "sign"); s.advance();
    check(s.visible.size() == 3, "sim: with 3 coins the trade shows");
    s.advance(1);
    check(s.get("coins"_hash) == 1 && s.get("lucky"_hash) == 1 && std::string(s.text()) == "Luck is with you now.",
          "sim: the picked choice's effects apply, then its next line (whose condition now passes)");
    s.advance();
    check(!s.active(), "sim: 'end' ends it");
}

// ---- runtime vs simulator, and the flow ---------------------------------------------------------
constexpr uint32_t kUp = 1u << 0, kDown = 1u << 1, kRight = 1u << 3, kA = 1u << 4;
uint32_t g_script[200];

struct DialogueGame final : Game {
    ResourceCache* res = nullptr;
    GameFlow flow;
    uint32_t frame = 0;
    bool lockstep_ok = true, lockstep_ran = false, corrupt_refused = false, reveal_ok = false;
    bool intro_seen = false, intro_speaker = false, level_after_intro = false, could_talk = false;
    bool talk_started = false, three_choices = false, frozen = true, talk_ended = false;
    int32_t coins_before = -1, coins_after = -1, lucky = -1;
    scalar talk_x{};

    // Drive a DialogueRunner and a DlgSim through the same inputs; they must agree every step.
    void lockstep(App& app) {
        phxtool::DlgCompiled c;
        if (!phxtool::dlg_compile(game_dialogue(), c)) { lockstep_ok = false; return; }
        DialogueRunner run;
        run.chars_per_sec = 30;
        DialogueVarTable vars;
        vars.set("coins"_hash, 3);
        if (!run.load(app.render(), *res, "dialogue"_hash)) { lockstep_ok = false; return; }
        phxtool::DlgSim sim;
        sim.vars["coins"_hash] = 3;
        lockstep_ran = run.start("sign"_hash, vars) && sim.start(c, "sign");
        // A (reveal) A (next) | wait for the reveal | Down A (trade) | A A (end)
        std::vector<uint32_t> presses = { kA, kA };
        presses.insert(presses.end(), 40, 0u);                 // the question types out (17 chars at 30/s)
        presses.insert(presses.end(), { kDown, kA, kA, kA });
        bool revealed_before_a = false;
        int step_pick = 0;
        for (uint32_t p : presses) {
            InputState in{};
            in.pressed = p;
            const bool was_revealed = run.revealed();
            if (p & kDown) step_pick = 1;
            run.update(in, vars);
            // the runtime's rule, in the simulator: A advances only a line that was fully shown
            if ((p & kA) && was_revealed) sim.advance(step_pick);
            if (!was_revealed && (p & kA) && run.revealed()) revealed_before_a = true;
            if (run.active() != sim.active() || (run.active() && std::strcmp(run.text(), sim.text()) != 0) ||
                (run.active() && run.revealed() && run.choice_count() != sim.visible.size()))
                lockstep_ok = false;
        }
        reveal_ok = revealed_before_a;
        lockstep_ok = lockstep_ok && !run.active() && vars.get("coins"_hash) == 1 && vars.get("lucky"_hash) == 1 &&
                      sim.get("coins"_hash) == 1 && run.lines_shown() == 3;

        // a corrupt asset (a node pointing past the table) is refused at load
        phxtool::DlgCompiled cc = c;
        cc.nodes[0].next = uint16_t(cc.nodes.size() + 5);
        phxtool::BundleWriter w(2);
        w.add_dialogue("bad", cc.blob());
        w.write("build/d_bad.phxp");
        ResourceCache* rb = ResourceCache::create(app.mem().persistent()).unwrap();
        corrupt_refused = rb->mount(app.platform(), "build/d_bad.phxp") == Status::Ok && !rb->dialogue("bad"_hash).ok();
    }

    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        check(res->mount(app.platform(), "build/d_game.phxp") == Status::Ok, "mount the baked game");
        lockstep(app);
        check(flow.start(app, *res) == Status::Ok, "the flow starts");
    }
    void on_fixed_update(App& app, scalar dt) override {
        ++frame;
        flow.update(app, dt);
        DialogueRunner& d = flow.dialogue();
        if (flow.screen() == "intro"_hash && d.active()) {
            intro_seen = true;
            if (std::strcmp(d.speaker(), "Narrator") == 0) intro_speaker = true;
        }
        if (flow.screen() == "one"_hash && flow.in_level()) {
            level_after_intro = true;
            if (flow.behaviours().can_talk()) could_talk = true;
            const Transform* t = app.world().get<Transform>(flow.behaviours().player());
            if (d.active()) {
                if (!talk_started) { talk_started = true; coins_before = flow.total("coins"_hash); if (t) talk_x = t->pos.x; }
                if (d.revealed() && d.choice_count() == 3) three_choices = true;
                if (t && t->pos.x != talk_x) frozen = false;
            } else if (talk_started && !talk_ended) {
                talk_ended = true;
                coins_after = flow.total("coins"_hash);
                lucky = flow.total("lucky"_hash);
            }
        }
    }
    void on_render(App& app, scalar) override { flow.render(app); }
};
} // namespace

int main() {
    model_checks();
    check(bake(), "bake the flow, map, prefabs, font and dialogue with the real converters");

    // the opening: A to reveal / advance each of its 2 lines; then the level: run right over both
    // coins onto the sign, Up to talk, A (reveal) A (advance) A (reveal the question), Down to the
    // trade, A to take it, A A through the last line.
    for (uint32_t f = 0; f < 200; ++f) {
        uint32_t m = 0;
        if (f == 4 || f == 6 || f == 8 || f == 10) m |= kA;
        if (f >= 25 && f < 65) m |= kRight;
        if (f == 80) m |= kUp;
        if (f == 84 || f == 86 || f == 88 || f == 92 || f == 94 || f == 96) m |= kA;
        if (f == 90) m |= kDown;
        g_script[f] = m;
    }
    phx_null_set_button_script(g_script, 200);
    phx_null_set_max_frames(130);

    Config cfg = Config::from_defaults();
    cfg.title = "dialogue_test"; cfg.width = 160; cfg.height = 48;
    cfg.total_ram = 8u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;
    App app(cfg);
    static DialogueGame g;
    check(app.run(&g) == 0, "the App runs the game");

    check(g.lockstep_ran && g.lockstep_ok, "DialogueRunner and the Studio's DlgSim agree step for step (texts, choices, variables)");
    check(g.reveal_ok, "A during the typewriter shows the whole line (and does not advance)");
    check(g.corrupt_refused, "a Dialogue asset with an out-of-range index is refused");
    check(g.intro_seen && g.intro_speaker, "a talk screen plays its conversation (the opening, by the Narrator)");
    check(g.level_after_intro, "the talk screen goes on to the next screen when the conversation ends");
    check(g.could_talk, "touching the sign: can_talk() (the flow shows 'UP: TALK')");
    check(g.talk_started && g.coins_before == 2, "Up at the sign starts its conversation (2 coins collected)");
    check(g.three_choices, "the trade shows once the player has 2 coins");
    check(g.frozen, "the level waits while the conversation plays");
    check(g.talk_ended && g.coins_after == 0 && g.lucky == 1,
          "the trade's effects land in the flow's totals: coins 2 -> 0, lucky = 1");

    std::printf("dialogue_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail ? "DIALOGUE FAIL\n\n" : "DIALOGUE PASS\n\n");
    return g_fail ? 1 : 0;
}
