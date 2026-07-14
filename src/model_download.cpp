#include "model_download.hpp"

#include <cstdio>
#include <filesystem>

#include <curl/curl.h>

#include "web_defaults.hpp"  // ttlive (vendored): UA + CA bundle discovery

namespace fs = std::filesystem;

namespace {

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return std::fwrite(ptr, 1, size * nmemb, (FILE*)userdata);
}

int progressCb(void* userdata, curl_off_t dlTotal, curl_off_t dlNow,
               curl_off_t, curl_off_t) {
    auto* self = (ModelDownloader*)userdata;
    return self->onProgress((uint64_t)dlTotal, (uint64_t)dlNow) ? 0 : 1;
}

}  // namespace

ModelDownloader::~ModelDownloader() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
}

std::string ModelDownloader::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

void ModelDownloader::setError(const std::string& msg) {
    if (!msg.empty()) std::fprintf(stderr, "[model-download] %s\n", msg.c_str());
    std::lock_guard<std::mutex> lk(mutex_);
    error_ = msg;
}

bool ModelDownloader::onProgress(uint64_t dlTotal, uint64_t dlNow) {
    down_.store(resumeOff_ + dlNow);
    if (dlTotal > 0) total_.store(resumeOff_ + dlTotal);
    return !cancel_.load();
}

bool ModelDownloader::start(const std::string& url,
                            const std::string& destPath) {
    if (active_.load()) return false;
    if (thread_.joinable()) thread_.join();  // reap a finished worker

    // libcurl global state; ttlive does the same lazily, but the downloader
    // can run before any of its clients exist. Refcounted, so both are fine.
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_ALL); });

    cancel_.store(false);
    done_.store(false);
    down_.store(0);
    total_.store(0);
    setError("");

    active_.store(true);
    thread_ = std::thread(&ModelDownloader::worker, this, url, destPath);
    return true;
}

void ModelDownloader::worker(std::string url, std::string dest) {
    const std::string part = dest + ".part";
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);

    // Resume where a previous (interrupted) run left off.
    resumeOff_ = fs::exists(part, ec) ? (uint64_t)fs::file_size(part, ec) : 0;
    down_.store(resumeOff_);

    FILE* f = std::fopen(part.c_str(), resumeOff_ ? "ab" : "wb");
    if (!f) {
        setError("cannot write " + part);
        active_.store(false);
        return;
    }

    CURL* c = curl_easy_init();
    if (!c) {
        std::fclose(f);
        setError("curl init failed");
        active_.store(false);
        return;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ttlive::web_defaults::kUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    // Abort transfers that stall for a minute (network gone, server hung).
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    if (resumeOff_ > 0)
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE,
                         (curl_off_t)resumeOff_);
    // The curl-impersonate DLL on Windows has no OS trust store; reuse the
    // same CA bundle discovery the rest of the app uses.
    const std::string& ca = ttlive::web_defaults::ca_bundle_path();
    if (!ca.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, ca.c_str());

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    std::fclose(f);

    // 416 on resume: the .part already contains the whole file (a finished
    // range can't be extended) — treat as success.
    if (rc == CURLE_HTTP_RETURNED_ERROR && http == 416 && resumeOff_ > 0)
        rc = CURLE_OK;

    if (cancel_.load()) {  // keep .part so the next run resumes
        active_.store(false);
        return;
    }
    if (rc != CURLE_OK) {
        std::string msg = curl_easy_strerror(rc);
        if (http > 0 && rc == CURLE_HTTP_RETURNED_ERROR)
            msg = "HTTP " + std::to_string(http);
        setError("download failed: " + msg);
        active_.store(false);
        return;
    }

    fs::rename(part, dest, ec);
    if (ec) {
        setError("cannot rename " + part);
        active_.store(false);
        return;
    }
    done_.store(true);
    active_.store(false);
}
