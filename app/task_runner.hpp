#pragma once

// Background jobs for the UI.
//
// Long operations (plan building with directory size walks, cleaning, the audit report, backup
// listing, restore, the update check) run here instead of on the UI thread, so the window keeps
// drawing while they work. Each job runs on its own std::jthread.
//
// Rules (enforced by the shape of the API, and binding for every caller):
//   1. A worker lambda never touches AppState or any other UI-thread object. It captures copies
//      of what it needs (paths, the ignore list, the selected targets) by value and returns its
//      result by value.
//   2. The completion callback runs on the UI thread, from poll(), which the shell calls once per
//      frame. That is the only place a result is written into AppState.
//   3. Every job has a kind and a label. The title bar shows the label of the running job, and
//      pages ask is_running(kind) to disable a button or show a loading state instead of starting
//      a second job of the same kind.
//   4. A worker checks ctx.stop_requested() at safe points if it can stop early. Jobs submitted
//      with must_finish (a clean, a restore) are never asked to stop: shutdown() waits for them.
//   5. Progress is optional: ctx.set_total(n) and ctx.advance() write atomics the UI reads.
//
// Example (a page):
//
//   auto install = *state.install;                // copies, captured by value
//   auto ignore = state.ignore_list;
//   state.tasks.submit<stc::core::Plan>(
//       JobKind::PlanBuild, "Building the cleanup plan",
//       [install, ignore](stc::app::JobContext& ctx) { return build_the_plan(install, ignore, ctx); },
//       [&state](stc::core::Plan plan) { state.last_plan = std::move(plan); },
//       [&state](const std::string& error) { state.plan_error = error; });
//
// The callbacks may capture `state` by reference because they run on the UI thread while the
// AppState is alive (the runner is a member of AppState and is shut down first).

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stc::app {

enum class JobKind {
    PlanBuild,      // cleanup plan with size walks
    Clean,          // executes a plan (must finish)
    Restore,        // restores a backup (must finish)
    BackupList,     // lists backups and their sizes
    BackupDelete,   // deletes a backup (must finish)
    AuditReport,    // the read-only audit
    ConfigInstall,  // installs config files (must finish)
    UpdateCheck,    // the release check on launch
    Other,
};

using JobId = std::uint64_t;

// Handed to the worker. Lives as long as the job.
class JobContext {
public:
    bool stop_requested() const noexcept { return stop_.stop_requested(); }
    std::stop_token stop_token() const noexcept { return stop_; }

    void set_total(std::uint64_t total) noexcept { total_->store(total, std::memory_order_relaxed); }
    void set_done(std::uint64_t done) noexcept { done_->store(done, std::memory_order_relaxed); }
    void advance(std::uint64_t by = 1) noexcept { done_->fetch_add(by, std::memory_order_relaxed); }

private:
    friend class TaskRunner;
    JobContext(std::stop_token stop, std::shared_ptr<std::atomic<std::uint64_t>> done,
               std::shared_ptr<std::atomic<std::uint64_t>> total)
        : stop_(std::move(stop)), done_(std::move(done)), total_(std::move(total)) {}

    std::stop_token stop_;
    std::shared_ptr<std::atomic<std::uint64_t>> done_;
    std::shared_ptr<std::atomic<std::uint64_t>> total_;
};

// A snapshot of a running job, for the title bar's activity indicator and the pages.
struct JobInfo {
    JobId id = 0;
    JobKind kind = JobKind::Other;
    std::string label;
    std::uint64_t done = 0;
    std::uint64_t total = 0;  // 0 when the job reports no progress
    bool must_finish = false;
};

struct SubmitOptions {
    bool must_finish = false;  // never asked to stop; shutdown() waits for it
};

class TaskRunner {
public:
    TaskRunner() = default;
    ~TaskRunner();
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

    // Starts `work` on a worker thread. `on_done(result)` runs on the UI thread in a later poll().
    // If `work` throws, `on_error(message)` runs there instead (when given; the error is logged
    // either way). Returns the job id. UI thread only.
    template <class R, class Work, class Done>
    JobId submit(JobKind kind, std::string label, Work work, Done on_done,
                 std::function<void(const std::string&)> on_error = {}, SubmitOptions options = {}) {
        auto job = std::make_shared<Job>();
        job->info.kind = kind;
        job->info.label = std::move(label);
        job->info.must_finish = options.must_finish;
        return start(job, [label = job->info.label, work = std::move(work), on_done = std::move(on_done),
                           on_error = std::move(on_error)](JobContext& ctx) mutable -> Completion {
            try {
                R result = work(ctx);
                return [on_done = std::move(on_done), result = std::move(result)]() mutable {
                    on_done(std::move(result));
                };
            } catch (const std::exception& e) {
                return error_completion(label, e.what(), std::move(on_error));
            } catch (...) {
                return error_completion(label, "unknown error", std::move(on_error));
            }
        });
    }

    // Runs the completion callbacks of finished jobs and joins their threads. UI thread, once per
    // frame.
    void poll();

    bool is_running(JobKind kind) const;
    bool any_running() const;
    // True while a must_finish job runs (the app must not exit yet).
    bool must_finish_running() const;
    // No job running and no completion waiting for poll().
    bool idle() const;
    // The running jobs, oldest first.
    std::vector<JobInfo> running() const;

    // Asks every job that may stop to stop.
    void request_stop_all();
    // Asks stoppable jobs to stop, waits for every thread (must_finish jobs run to the end) and
    // drops the completions that were not polled. Called on exit before AppState goes away.
    void shutdown();

private:
    using Completion = std::move_only_function<void()>;

    struct Job {
        JobInfo info;
        std::shared_ptr<std::atomic<std::uint64_t>> done = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::shared_ptr<std::atomic<std::uint64_t>> total = std::make_shared<std::atomic<std::uint64_t>>(0);
        std::jthread thread;
        bool finished = false;          // guarded by mutex_
        std::optional<Completion> completion;  // guarded by mutex_
    };

    static Completion error_completion(const std::string& label, const std::string& message,
                                       std::function<void(const std::string&)> on_error);
    JobId start(const std::shared_ptr<Job>& job, std::move_only_function<Completion(JobContext&)> body);

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Job>> jobs_;  // running and finished-but-not-polled
    JobId next_id_ = 1;
    bool shut_down_ = false;
};

}  // namespace stc::app
