// phx/core/log.h — leveled logging. The sink is platform-provided (stdout / mGBA / PSP).
// Below the compile-time floor PHX_LOG_FLOOR the macros expand to nothing, so format
// strings cost zero ROM bytes — important on GBA.
#ifndef PHX_CORE_LOG_H
#define PHX_CORE_LOG_H

#include "phx/core/types.h"

namespace phx {

enum class LogLevel : uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

using LogSink = void (*)(LogLevel, const char* msg);

void log_set_sink(LogSink sink);     // wired to the platform's log function at boot
void log_set_level(LogLevel level);  // runtime floor (in addition to the compile floor)
void log_emit(LogLevel level, const char* fmt, ...);   // formats into a fixed stack buffer

} // namespace phx

// Compile-time floor: levels below this vanish entirely (no code, no strings).
#ifndef PHX_LOG_FLOOR
    #if defined(PHX_BUILD_RELEASE)
        #define PHX_LOG_FLOOR 2   /* Info */
    #else
        #define PHX_LOG_FLOOR 0   /* Trace */
    #endif
#endif

#define PHX_LOG_(lvl, intlvl, fmt, ...) \
    do { if ((intlvl) >= PHX_LOG_FLOOR) ::phx::log_emit(::phx::LogLevel::lvl, fmt, ##__VA_ARGS__); } while (0)

#define PHX_LOG_TRACE(fmt, ...) PHX_LOG_(Trace, 0, fmt, ##__VA_ARGS__)
#define PHX_LOG_DEBUG(fmt, ...) PHX_LOG_(Debug, 1, fmt, ##__VA_ARGS__)
#define PHX_LOG_INFO(fmt, ...)  PHX_LOG_(Info,  2, fmt, ##__VA_ARGS__)
#define PHX_LOG_WARN(fmt, ...)  PHX_LOG_(Warn,  3, fmt, ##__VA_ARGS__)
#define PHX_LOG_ERROR(fmt, ...) PHX_LOG_(Error, 4, fmt, ##__VA_ARGS__)

#endif // PHX_CORE_LOG_H
