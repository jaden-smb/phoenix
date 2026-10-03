// tools/phxstudio/winjob.h — the Windows half of the job runner (jobs.h). A launch runs as
// `sh.exe <script>` (MSYS2 or Git for Windows; cmd.exe can't run the launches' POSIX shell) inside
// a Job Object, so STOP ends the whole tree (make -> g++, or a game window) at once, and a Studio
// that exits or crashes takes its jobs with it. stdout and stderr share one pipe; stdin is NUL.
// Implemented in winjob.cpp, which keeps <windows.h> out of every other translation unit.
// Host-only; declared on every host so winjob.cpp is never an empty translation unit.
#ifndef PHX_TOOLS_PHXSTUDIO_WINJOB_H
#define PHX_TOOLS_PHXSTUDIO_WINJOB_H

#include <cstddef>
#include <string>

namespace phxstudio {

struct WinJob;   // opaque: the process, its Job Object and the read end of its output pipe

// Start `sh script`. nullptr (and `err`) when the process can't be started.
WinJob* winjob_start(const std::string& sh, const std::string& script, std::string* err);
// Block for up to `n` bytes of output; 0 once every process holding the pipe has exited.
size_t winjob_read(WinJob* j, char* buf, size_t n);
// Terminate every process in the job. Safe from another thread while winjob_read blocks.
void winjob_kill(WinJob* j);
// Wait for the shell to exit, free the job, and return the shell's exit code.
int winjob_finish(WinJob* j);
// A temp-folder path for job `id`'s script, unique to this Studio process ('/'-separated).
std::string winjob_script_path(int id);

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_WINJOB_H
