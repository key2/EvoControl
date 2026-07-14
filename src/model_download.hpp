#pragma once

// ModelDownloader — background download of a large file (the Voxtral STT
// model, ~2.7 GB) so it doesn't have to ship inside the distributable.
//
// The download streams straight to ``<dest>.part`` (never buffered in RAM),
// resumes from a partial file if a previous run was interrupted, and renames
// to ``<dest>`` only when complete. Progress is exposed through atomics the
// UI thread reads every frame.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class ModelDownloader {
public:
    ModelDownloader() = default;
    ~ModelDownloader();

    ModelDownloader(const ModelDownloader&) = delete;
    ModelDownloader& operator=(const ModelDownloader&) = delete;

    /// Start downloading url -> destPath (parent dirs are created). Returns
    /// false if a download is already running.
    bool start(const std::string& url, const std::string& destPath);

    /// Ask the worker to stop. The partial file is kept for a later resume.
    void cancel() { cancel_.store(true); }

    bool active() const { return active_.load(); }
    /// True once the file has been fully downloaded and renamed into place.
    bool finished() const { return done_.load(); }

    uint64_t downloaded() const { return down_.load(); }
    /// Total size in bytes, or 0 while unknown.
    uint64_t total() const { return total_.load(); }

    std::string error() const;

    /// Internal (called from the libcurl progress callback). Returns false to
    /// abort the transfer.
    bool onProgress(uint64_t dlTotal, uint64_t dlNow);

private:
    void worker(std::string url, std::string dest);
    void setError(const std::string& msg);

    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> down_{0};
    std::atomic<uint64_t> total_{0};
    uint64_t resumeOff_ = 0;  // written before the thread starts polling

    mutable std::mutex mutex_;
    std::string error_;
};
