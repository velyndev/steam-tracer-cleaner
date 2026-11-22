#include "app/task_runner.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace stc::app {

TaskRunner::~TaskRunner() { shutdown(); }

TaskRunner::Completion TaskRunner::error_completion(const std::string& label, const std::string& message,
                                                    std::function<void(const std::string&)> on_error) {
    spdlog::error("Background job '{}' failed: {}", label, message);
    return [on_error = std::move(on_error), message]() {
        if (on_error) {
            on_error(message);
        }
    };
}

JobId TaskRunner::start(const std::shared_ptr<Job>& job, std::move_only_function<Completion(JobContext&)> body) {
    // The lock is held until job->thread is assigned, so the worker cannot publish its result
    // before the job is fully set up.
    std::lock_guard lock(mutex_);
    if (shut_down_) {
        return 0;
    }
    const JobId id = next_id_++;
    job->info.id = id;
    jobs_.push_back(job);
    Job* raw = job.get();  // kept alive by jobs_ until poll() or shutdown() joined the thread
    job->thread = std::jthread(
        [this, raw, body = std::move(body), done = job->done, total = job->total](std::stop_token stop) mutable {
            JobContext ctx(std::move(stop), std::move(done), std::move(total));
            Completion completion = body(ctx);
            std::lock_guard inner(mutex_);
            raw->completion = std::move(completion);
            raw->finished = true;
        });
    spdlog::debug("Job {} started: {}", id, job->info.label);
    return id;
}

void TaskRunner::poll() {
    std::vector<std::shared_ptr<Job>> finished;
    {
        std::lock_guard lock(mutex_);
        auto split = std::stable_partition(jobs_.begin(), jobs_.end(),
                                           [](const std::shared_ptr<Job>& j) { return !j->finished; });
        finished.assign(std::make_move_iterator(split), std::make_move_iterator(jobs_.end()));
        jobs_.erase(split, jobs_.end());
    }
    // Outside the lock: the callbacks may submit new jobs.
    for (auto& job : finished) {
        if (job->thread.joinable()) {
            job->thread.join();  // already past its last statement
        }
        spdlog::debug("Job {} finished: {}", job->info.id, job->info.label);
        if (job->completion) {
            (*job->completion)();
        }
    }
}

bool TaskRunner::is_running(JobKind kind) const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(jobs_, [kind](const auto& j) { return !j->finished && j->info.kind == kind; });
}

bool TaskRunner::any_running() const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(jobs_, [](const auto& j) { return !j->finished; });
}

bool TaskRunner::must_finish_running() const {
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(jobs_, [](const auto& j) { return !j->finished && j->info.must_finish; });
}

bool TaskRunner::idle() const {
    std::lock_guard lock(mutex_);
    return jobs_.empty();
}

std::vector<JobInfo> TaskRunner::running() const {
    std::vector<JobInfo> out;
    std::lock_guard lock(mutex_);
    for (const auto& j : jobs_) {
        if (j->finished) {
            continue;
        }
        JobInfo info = j->info;
        info.done = j->done->load(std::memory_order_relaxed);
        info.total = j->total->load(std::memory_order_relaxed);
        out.push_back(std::move(info));
    }
    return out;
}

void TaskRunner::request_stop_all() {
    std::lock_guard lock(mutex_);
    for (auto& j : jobs_) {
        if (!j->info.must_finish) {
            j->thread.request_stop();
        }
    }
}

void TaskRunner::shutdown() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard lock(mutex_);
        shut_down_ = true;
        for (auto& j : jobs_) {
            if (!j->info.must_finish) {
                j->thread.request_stop();
            }
            if (!j->finished) {
                spdlog::info("Waiting for job '{}' before exit", j->info.label);
            }
        }
        jobs.swap(jobs_);
    }
    for (auto& job : jobs) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

}  // namespace stc::app
