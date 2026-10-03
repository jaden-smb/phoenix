// tools/phxstudio/winjob.cpp — see winjob.h. Compiles to nothing but the declarations off Windows.
#include "winjob.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace phxstudio {

struct WinJob {
    HANDLE job  = nullptr;
    HANDLE proc = nullptr;
    HANDLE out  = nullptr;   // read end of the stdout+stderr pipe
};

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// One argument for a Windows command line, quoted the way the C runtime (and the MSYS runtime)
// splits it back: backslashes are literal except before a '"', which they escape.
std::wstring quote_arg(const std::wstring& a) {
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') o.append(bs * 2 + 1, L'\\');
        else o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}

std::string last_error(const char* what) {
    return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

} // namespace

WinJob* winjob_start(const std::string& sh, const std::string& script, std::string* err) {
    auto fail = [&](const char* what) -> WinJob* { if (err) *err = last_error(what); return nullptr; };

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return fail("CreatePipe");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(rd); CloseHandle(wr);
        return fail("opening NUL");
    }

    // The child inherits exactly the pipe and NUL, nothing else this process has open.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[2] = { wr, nul };
    const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                    sizeof(inherit), nullptr, nullptr);

    // Every process the shell starts stays in the job; closing the job's last handle (winjob_finish,
    // or this process dying) terminates whatever is left.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;
    std::wstring cmd = quote_arg(widen(sh)) + L" " + quote_arg(widen(script));
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL started = job && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                               &si.StartupInfo, &pi);
    std::string why;
    if (!job) why = last_error("CreateJobObject");
    else if (!started) why = last_error("starting the shell");
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);     // the child holds the write end now; EOF arrives when every holder exits
    CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        if (err) *err = why + ": " + sh;
        return nullptr;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        why = last_error("AssignProcessToJobObject");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd); CloseHandle(job);
        if (err) *err = why;
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return new WinJob{ job, pi.hProcess, rd };
}

size_t winjob_read(WinJob* j, char* buf, size_t n) {
    DWORD got = 0;
    if (!ReadFile(j->out, buf, DWORD(n), &got, nullptr)) return 0;   // ERROR_BROKEN_PIPE = done
    return size_t(got);
}

void winjob_kill(WinJob* j) { TerminateJobObject(j->job, 1); }

int winjob_finish(WinJob* j) {
    WaitForSingleObject(j->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(j->proc, &code);
    CloseHandle(j->out);
    CloseHandle(j->proc);
    CloseHandle(j->job);
    delete j;
    return int(code);
}

std::string winjob_script_path(int id) {
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::string dir = narrow(std::wstring(tmp, n));
    for (char& c : dir) if (c == '\\') c = '/';
    if (dir.empty()) dir = "./";
    if (dir.back() != '/') dir += '/';
    return dir + "phxstudio-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(id) + ".sh";
}

} // namespace phxstudio
#endif // _WIN32
