// phx/ui/ui.h — an immediate-mode UI that emits DrawSprites into the renderer's sprite path
// (text = bitmap-font atlas; bars/panels = a tinted, scaled 1x1 white quad). No per-frame
// widget-tree allocations. Menus navigate by a focus ring (D-pad/buttons), because consoles
// have no pointer. Sized for GBA/PSP limits — everything batches into the same sprite budget.
// See docs/10-gameplay-systems.md §6.
//
// Depends on core + render + input (it legitimately draws and reads buttons). L3 over L2.
#ifndef PHX_UI_UI_H
#define PHX_UI_UI_H

#include "phx/core/math.h"
#include "phx/core/pixel.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"

namespace phx {

// A bitmap font: an atlas of fixed-size glyph cells in a `cols`-wide grid. Cell 0 is ASCII
// `first_char` (usually 32 = space). Fixed-width advance keeps GBA tile budgets predictable.
struct BitmapFont {
    TextureId tex        = kNoTexture;
    uint8_t   glyph_w    = 8;
    uint8_t   glyph_h    = 8;
    uint8_t   cols       = 16;
    uint8_t   first_char = 32;
    uint8_t   advance    = 8;     // horizontal pixels per glyph
    uint8_t   line_h     = 8;     // vertical pixels per '\n'
};

struct UIRect { vec2 pos; vec2 size; };

class UI {
public:
    // Begin a frame: bind the renderer + this frame's input, lazily create the white quad,
    // and apply pending focus navigation (Up/Down move the focus ring, wrapping).
    void begin(Renderer&, const InputState&);

    void text(vec2 pos, const BitmapFont&, const char* str,
              Rgba tint = rgba(255, 255, 255), uint8_t layer = kLayer);
    void image(const UIRect& dst, TextureId, int sx, int sy, int sw, int sh,
               Rgba tint = rgba(255, 255, 255), uint8_t layer = kLayer);
    void rect(const UIRect&, Rgba color, uint8_t layer = kLayer);
    void bar(const UIRect&, scalar t, Rgba fg, Rgba bg, uint8_t layer = kLayer);  // HUD bar, t∈[0,1]

    // A focus-ring menu button: draws a panel (highlighted when focused) + centered label.
    // Returns true on the frame it is focused AND the confirm button (A) is pressed.
    bool button(const UIRect&, const BitmapFont&, const char* label);

    void end();

    int  focus() const     { return focus_; }
    void set_focus(int f)   { focus_ = f; }

    // Focus-ring step with wraparound. Pure (no renderer) so it's unit-testable directly.
    static int ring_step(int focus, int count, int delta) {
        if (count <= 0) return 0;
        return ((focus + delta) % count + count) % count;
    }

    // theming (public so games can tweak; sensible defaults)
    Rgba panel       = rgba(50, 50, 70);
    Rgba panel_focus = rgba(90, 90, 140);
    Rgba label       = rgba(230, 230, 240);

private:
    static constexpr uint8_t kLayer = 200;   // UI draws above gameplay

    Renderer*         r_   = nullptr;
    const InputState* in_  = nullptr;
    TextureId         white_ = kNoTexture;
    int focus_          = 0;
    int btn_count_      = 0;   // buttons issued this frame
    int prev_btn_count_ = 0;   // last frame's count (for nav wrap)
};

} // namespace phx
#endif // PHX_UI_UI_H
