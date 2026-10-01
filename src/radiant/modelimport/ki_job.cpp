// KIWI model importer - background job. See ki_job.h.

#include "ki_job.h"

#include <exception>
#include <utility>

namespace kimport {

ImportJob::~ImportJob() {
    Join();
}

void ImportJob::Join() {
    if (thread_.joinable()) thread_.join();
}

bool ImportJob::Start(const std::string& title, Work work) {
    if (running_.load()) return false;
    Join();
    title_ = title;
    finished_ = false;
    succeeded_ = false;
    error_.clear();
    running_ = true;
    thread_ = std::thread([this, work = std::move(work)]() {
        const LogFn log = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(line);
        };
        bool ok = false;
        std::string error;
        try {
            ok = work(log, error);
        } catch (const std::exception& exception) {
            error = std::string("internal error: ") + exception.what();
        } catch (...) {
            error = "internal error";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            succeeded_ = ok;
            error_ = ok ? std::string() : error;
        }
        finished_ = true;
        running_ = false;
    });
    return true;
}

bool ImportJob::TakeFinished(bool& succeeded, std::string& error) {
    if (!finished_.load()) return false;
    Join();
    finished_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    succeeded = succeeded_;
    error = error_;
    return true;
}

std::vector<std::string> ImportJob::TakeLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(pending_);
    return out;
}

}  // namespace kimport
