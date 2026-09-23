// tools/phxstudio/jobs.h — runs the studio's launches (`make <target>`, a game binary, an
// emulator) as child processes on ONE worker thread, one at a time from a FIFO queue, and hands
// their combined stdout/stderr to the GUI thread for the console panel. Host-only (POSIX shell).
//
// Every job runs as `setsid sh -c '<command>'` from the repository root, so the whole process
// tree (make -> g++ -> the test binary, or make -> a game window) sits in its own process group
// and STOP can signal all of it at once. `stdbuf -oL` makes the children line-buffered, so test
// output streams live instead of arriving in one block at exit. Both are optional: without
// setsid STOP is unavailable, without stdbuf output simply arrives in bigger chunks.
//
// The engine's single-threaded contract is about ENGINE state; this worker touches none of it —
// it only moves bytes into a mutex-guarded buffer the GUI thread drains once per frame.
#ifndef PHX_TOOLS_PHXSTUDIO_JOBS_H
#define PHX_TOOLS_PHXSTUDIO_JOBS_H

#include "model.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace phxstudio {

enum class JobState : uint8_t { Idle, Queued, Running, Pass, Fail, Stopped };

struct JobResult {
    JobState state = JobState::Idle;
    int      exit_code = 0;
    double   seconds = 0.0;
};

// Quote `s` for a POSIX shell single-quoted context.
inline std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

class JobRunner {
public:
    explicit JobRunner(std::string root)
        : root_(std::move(root)),
          has_setsid_(tool_available("setsid")),
          has_stdbuf_(tool_available("stdbuf")) {
        thread_ = std::thread([this] { worker(); });
    }

    ~JobRunner() { shutdown(); }

    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    // Queue a launch. A launch already queued or running is not queued twice.
    void enqueue(int id, const std::string& label, const std::string& command) {
        std::lock_guard<std::mutex> lk(mu_);
        const JobState st = results_[id].state;
        if (st == JobState::Queued || (st == JobState::Running && running_id_ == id)) return;
        queue_.push_back(Pending{ id, label, command });
        results_[id].state = JobState::Queued;
        cv_.notify_one();
    }

    // Signal the running job's whole process group (TERM). Queued jobs keep waiting.
    void stop_current() {
        long pg = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pg = pgid_;
            if (running_id_ >= 0) stop_requested_ = true;
        }
        if (pg > 0) signal_group(pg, "TERM");
    }

    // Drop every queued (not yet started) job.
    void cancel_queue() {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Pending& p : queue_) results_[p.id].state = JobState::Idle;
        queue_.clear();
    }

    // Move the output produced since the last call into `log` (GUI thread, once per frame).
    void drain(LogRing& log) {
        std::string bytes;
        {
            std::lock_guard<std::mutex> lk(mu_);
            bytes.swap(out_);
        }
        if (!bytes.empty()) log.feed(bytes.data(), bytes.size());
    }

    JobResult result(int id) const {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = results_.find(id);
        return it == results_.end() ? JobResult{} : it->second;
    }
    int running_id() const { std::lock_guard<std::mutex> lk(mu_); return running_id_; }
    std::string running_label() const { std::lock_guard<std::mutex> lk(mu_); return running_label_; }
    size_t queued() const { std::lock_guard<std::mutex> lk(mu_); return queue_.size(); }
    double running_seconds() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_id_ < 0) return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }
    bool can_stop() const { return has_setsid_; }

    // Stop everything and join the worker (idempotent). A job that ignores TERM for ~2 s is
    // KILLed, so closing the studio never hangs on a stuck child.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (quit_) return;
            quit_ = true;
            queue_.clear();
            cv_.notify_one();
        }
        stop_current();
        for (int i = 0; i < 40 && running_id() >= 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        long pg = 0;
        { std::lock_guard<std::mutex> lk(mu_); pg = pgid_; }
        if (pg > 0) signal_group(pg, "KILL");
        if (thread_.joinable()) {
            if (running_id() >= 0 && pg <= 0) thread_.detach();   // no process group to kill
            else thread_.join();
        }
    }

private:
    struct Pending { int id; std::string label, command; };

    static void signal_group(long pgid, const char* sig) {
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "kill -%s -%ld 2>/dev/null", sig, pgid);   // no "--": dash rejects it
        const int rc = std::system(cmd);
        (void)rc;
    }

    void append(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu_);
        out_ += s;
    }

    void worker() {
        for (;;) {
            Pending job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
                if (quit_) return;
                job = queue_.front();
                queue_.pop_front();
                running_id_ = job.id;
                running_label_ = job.label;
                stop_requested_ = false;
                pgid_ = 0;
                started_ = std::chrono::steady_clock::now();
                results_[job.id].state = JobState::Running;
                out_ += "$ " + job.command + "\n";
            }

            // cd root && [exec setsid [stdbuf -oL -eL]] sh -c 'echo @@PGID $$; <command>' 2>&1 </dev/null
            std::string inner = "echo @@PGID $$; " + job.command;
            std::string sh = "cd " + shell_quote(root_) + " && ";
            if (has_setsid_) sh += "exec setsid ";
            if (has_stdbuf_) sh += "stdbuf -oL -eL ";
            sh += "sh -c " + shell_quote(inner) + " 2>&1 </dev/null";

            int status = -1;
            if (FILE* p = popen(sh.c_str(), "r")) {
                char buf[4096];
                bool first = true;
                while (std::fgets(buf, sizeof(buf), p)) {
                    if (first) {
                        first = false;
                        long pg = 0;
                        if (std::sscanf(buf, "@@PGID %ld", &pg) == 1) {
                            if (has_setsid_) { std::lock_guard<std::mutex> lk(mu_); pgid_ = pg; }
                            continue;
                        }
                    }
                    append(buf);
                }
                status = pclose(p);
            } else {
                append("studio: could not start a shell (popen failed)\n");
            }

            const int code = exit_code_from_status(status);
            std::lock_guard<std::mutex> lk(mu_);
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            JobResult& r = results_[job.id];
            r.exit_code = code;
            r.seconds = secs;
            r.state = stop_requested_ ? JobState::Stopped : (code == 0 ? JobState::Pass : JobState::Fail);
            char tail[160];
            std::snprintf(tail, sizeof(tail), "== %s: %s (exit %d) in %.1f s ==\n\n", job.label.c_str(),
                          r.state == JobState::Pass ? "PASS" : r.state == JobState::Stopped ? "stopped" : "FAIL",
                          code, secs);
            out_ += tail;
            running_id_ = -1;
            running_label_.clear();
            pgid_ = 0;
        }
    }

    std::string root_;
    bool has_setsid_ = false;
    bool has_stdbuf_ = false;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Pending> queue_;
    std::map<int, JobResult> results_;
    std::string out_;                     // produced output not yet drained by the GUI
    int  running_id_ = -1;
    std::string running_label_;
    long pgid_ = 0;
    bool stop_requested_ = false;
    bool quit_ = false;
    std::chrono::steady_clock::time_point started_{};
    std::thread thread_;
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_JOBS_H
