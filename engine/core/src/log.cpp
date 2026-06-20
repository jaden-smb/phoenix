// phx/core/src/log.cpp — formatting + sink dispatch. No allocation: formats into a
// fixed stack buffer. A default sink writes to stderr until the platform overrides it.
#include "phx/core/log.h"

#include <cstdarg>
#include <cstdio>

namespace phx {
namespace {

void default_sink(LogLevel level, const char* msg) {
    static const char* kTag[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "OFF  " };
    std::fprintf(stderr, "[phx][%s] %s\n", kTag[int(level)], msg);
}

LogSink  g_sink  = default_sink;
LogLevel g_level = LogLevel::Trace;

} // namespace

void log_set_sink(LogSink sink)     { g_sink = sink ? sink : default_sink; }
void log_set_level(LogLevel level)  { g_level = level; }

void log_emit(LogLevel level, const char* fmt, ...) {
    if (level < g_level) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_sink(level, buf);
}

} // namespace phx
