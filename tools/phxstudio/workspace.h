// tools/phxstudio/workspace.h — the Studio's EDITOR workspace: an Explorer over the repository,
// a tab strip of open documents (each a DocView from host.h: code, sprite/pixel, map, table),
// a welcome page, the New-asset dialogs (sprite, image/tileset, map, table, code file), quick
// open, save / close-with-confirm, and reload-on-external-change. Host-only.
#ifndef PHX_TOOLS_PHXSTUDIO_WORKSPACE_H
#define PHX_TOOLS_PHXSTUDIO_WORKSPACE_H

#include "host.h"

#include <memory>
#include <string>
#include <vector>

namespace phxstudio {

class Workspace {
public:
    FileTree tree;
    std::vector<std::unique_ptr<DocView>> docs;
    int active = -1;
    int side_w = 158;
    bool side_open = true;
    std::vector<std::string> recent;          // most recent first (absolute paths)

    void init(Host& h);
    void draw(Host& h, const Rect& area);

    // Open (or focus) a document. line/col (1-based) jump there in text editors.
    bool open(Host& h, const std::string& abs, bool as_text = false, int line = 0, int col = 0);
    // Open an image in the pixel editor focused on one tile (the map editor's "Edit tile").
    bool open_tile(Host& h, const std::string& abs, int index, int tw, int th);
    DocView* current() { return active >= 0 && active < int(docs.size()) ? docs[size_t(active)].get() : nullptr; }
    bool save(Host& h, int i);
    bool save_all(Host& h);
    void close(Host& h, int i);                // asks when dirty
    void close_now(Host& h, int i);
    bool any_dirty() const;
    int  dirty_count() const;
    void next_tab(int dir);

    void quick_open(Host& h);
    // New-document dialogs (dir = folder relative to the root, "" = the root).
    void new_sprite(Host& h, const std::string& dir);
    void new_image(Host& h, const std::string& dir);
    void new_map(Host& h, const std::string& dir);
    void new_table(Host& h, const std::string& dir);
    void new_code(Host& h, const std::string& dir);
    void new_font(Host& h, const std::string& dir);
    void new_sfx(Host& h, const std::string& dir);
    void new_song(Host& h, const std::string& dir);
    void new_dialogue(Host& h, const std::string& dir);
    std::string selected_dir() const { return sel_dir_; }

    void poll_disk(Host& h);                   // external changes (call ~once a second)
    void refresh_tree() { tree.refresh(); files_cache_.clear(); }
    const std::vector<std::string>& all_files();

    // session (open docs + recents), one file per workspace root in ~/.config/phxstudio/
    void load_session(Host& h, bool reopen);
    void save_session() const;
    void note_recent(const std::string& abs);
    // Close every document without asking (the caller already offered to save).
    void close_all(Host& h);

private:
    void draw_explorer(Host& h, const Rect& r);
    void draw_welcome(Host& h, const Rect& r);
    void explorer_context(Host& h);
    std::string rel(Host& h, const std::string& abs) const;

    // The engine's public API, browsable read-only below the project (project mode).
    struct ApiMod { std::string name, dir; std::vector<std::string> files; bool expanded = false, loaded = false; };
    std::vector<ApiMod> api_;
    bool api_open_ = false;
    void load_api_files(ApiMod& m);
    struct QuickItem { std::string display, abs; bool api = false; };
    std::vector<QuickItem> quick_items();
    std::string session_file() const;

    std::string filter_;
    int tree_scroll_ = 0;
    std::string sel_path_, sel_dir_;          // Explorer selection (relative)
    std::string ctx_path_;                     // right-clicked entry
    bool ctx_dir_ = false;
    std::vector<std::string> files_cache_;
    uint64_t files_tick_ = 0;
    std::string root_;
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_WORKSPACE_H
