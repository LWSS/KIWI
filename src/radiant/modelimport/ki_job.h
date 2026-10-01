#pragma once

// KIWI model importer - one background job at a time (loading an FBX through Blender,
// building a model, scanning a folder) so the editor UI never freezes. The work
// function runs on its own thread and must only touch the data it was given; the UI
// polls Running()/TakeLog() each frame and collects the result when Finished().

#include "ki_convert.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kimport {

class ImportJob {
public:
    using Work = std::function<bool(const LogFn& log, std::string& error)>;

    ImportJob() = default;
    ImportJob(const ImportJob&) = delete;
    ImportJob& operator=(const ImportJob&) = delete;
    ~ImportJob();

    // False when a job is still running (one at a time).
    bool Start(const std::string& title, Work work);

    bool Running() const { return running_.load(); }
    // True once per finished job (then the result can be collected).
    bool TakeFinished(bool& succeeded, std::string& error);
    // Log lines added since the last call.
    std::vector<std::string> TakeLog();
    const std::string& Title() const { return title_; }

private:
    void Join();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    bool succeeded_ = false;
    std::string error_;
    std::string title_;
    std::mutex mutex_;
    std::vector<std::string> pending_;
};

}  // namespace kimport
