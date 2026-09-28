// engine/runtime/src/budget.cpp — the budget report (phx/runtime/budget.h). Host-only.
#include "phx/runtime/budget.h"
#include "phx/core/caps.h"
#include "phx/core/log.h"

#include <cstdio>

namespace phx {

bool write_budget_report(App& app, const TargetProfile& target, const char* path, uint32_t host_only) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const Config& c = app.config();
    const RuntimePeaks& p = app.peaks();
    const Caps k = caps();
    const uint32_t max_ents = c.max_entities ? c.max_entities : k.max_entities;
    const uint32_t max_sprites = target.max_sprites ? target.max_sprites : k.max_sprites;
    const uint32_t channels = target.audio_channels ? target.audio_channels : k.audio_channels;
    char title[96];                                       // the title as a JSON string body
    size_t n = 0;
    for (const char* s = c.title ? c.title : ""; *s && n + 2 < sizeof(title); ++s) {
        if (*s == '"' || *s == '\\') title[n++] = '\\';
        title[n++] = (unsigned char)*s < 32 ? ' ' : *s;
    }
    title[n] = 0;
    std::fprintf(f,
        "{ \"budget\": 1, \"target\": \"%s\", \"title\": \"%s\", \"frames\": %llu, \"width\": %d, \"height\": %d,\n"
        "  \"arena\": { \"used\": %llu, \"capacity\": %llu },\n"
        "  \"frame_scratch\": { \"peak\": %u, \"capacity\": %u },\n"
        "  \"entities\": { \"peak\": %u, \"max\": %u },\n"
        "  \"sprites\": { \"peak\": %u, \"max\": %u, \"dropped\": %u },\n"
        "  \"tiles_peak\": %u, \"batches_peak\": %u,\n"
        "  \"audio\": { \"sounds\": %u, \"peak\": %d, \"channels\": %u },\n"
        "  \"log\": { \"warnings\": %u, \"errors\": %u } }\n",
        target.name ? target.name : "", title, (unsigned long long)app.frame(), int(c.width), int(c.height),
        (unsigned long long)(p.arena_used > host_only ? p.arena_used - host_only : 0),
        (unsigned long long)(p.arena_capacity > host_only ? p.arena_capacity - host_only : 0),
        p.frame_scratch, unsigned(c.frame_scratch),
        p.entities, max_ents,
        p.sprites, unsigned(max_sprites), p.sprites_dropped,
        p.tiles, p.batches,
        unsigned(app.audio().plays()), int(app.audio().peak()), unsigned(channels),
        log_count(LogLevel::Warn), log_count(LogLevel::Error));
    return std::fclose(f) == 0;
}

} // namespace phx
