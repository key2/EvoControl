#include "widget_registry.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include "miniz.h"
#include "store.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace evo {

WidgetRegistry::WidgetRegistry(Store& store, std::string widgetsDir)
    : store_(store), widgetsDir_(std::move(widgetsDir)) {
    std::error_code ec;
    fs::create_directories(widgetsDir_, ec);
}

WidgetManifest WidgetRegistry::parseManifest(const std::string& text) {
    WidgetManifest m;
    m.raw = text;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return m;
    m.type = j.value("type", "");
    m.version = j.value("version", "1.0.0");
    m.apiVersion = j.value("apiVersion", 1);
    m.name = j.value("name", m.type);
    m.description = j.value("description", "");
    m.icon = j.value("icon", "icon.png");
    if (j.contains("defaultSize") && j["defaultSize"].is_object()) {
        m.defaultW = j["defaultSize"].value("w", 1080.0);
        m.defaultH = j["defaultSize"].value("h", 1920.0);
    }
    if (j.contains("config")) m.configJson = j["config"].dump();
    if (j.contains("subscribes") && j["subscribes"].is_array())
        for (const auto& s : j["subscribes"])
            if (s.is_string()) m.subscribes.push_back(s.get<std::string>());
    if (j.contains("capabilities") && j["capabilities"].is_array())
        for (const auto& s : j["capabilities"])
            if (s.is_string()) m.capabilities.push_back(s.get<std::string>());
    m.valid = !m.type.empty();
    return m;
}

std::optional<WidgetManifest> WidgetRegistry::installFromZip(
    const std::vector<uint8_t>& zip, std::string& err) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0)) {
        err = "not a valid zip/.evw archive";
        return std::nullopt;
    }

    // Locate manifest.json (top level or under a single root dir).
    auto findEntry = [&](const std::string& name) -> int {
        int n = (int)mz_zip_reader_get_num_files(&za);
        for (int i = 0; i < n; i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&za, i, &st)) continue;
            std::string fn = st.m_filename;
            // match exact or "<root>/name"
            if (fn == name || (fn.size() > name.size() &&
                               fn.substr(fn.size() - name.size() - 1) ==
                                   "/" + name))
                return i;
        }
        return -1;
    };
    int mi = findEntry("manifest.json");
    if (mi < 0) {
        mz_zip_reader_end(&za);
        err = "manifest.json missing from bundle";
        return std::nullopt;
    }
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&za, mi, &sz, 0);
    if (!p) {
        mz_zip_reader_end(&za);
        err = "failed to read manifest.json";
        return std::nullopt;
    }
    std::string manifestText((const char*)p, sz);
    mz_free(p);
    WidgetManifest m = parseManifest(manifestText);
    if (!m.valid) {
        mz_zip_reader_end(&za);
        err = "manifest.json missing required 'type'";
        return std::nullopt;
    }

    // Determine the archive root prefix (if the manifest was under one).
    mz_zip_archive_file_stat mstat;
    mz_zip_reader_file_stat(&za, mi, &mstat);
    std::string manifestName = mstat.m_filename;
    std::string rootPrefix;
    {
        auto pos = manifestName.rfind("manifest.json");
        rootPrefix = manifestName.substr(0, pos);  // "" or "<root>/"
    }

    // Extract every file into widgets/<type>/<version>/, stripping rootPrefix.
    fs::path dest = fs::path(widgetsDir_) / m.type / m.version;
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    int n = (int)mz_zip_reader_get_num_files(&za);
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) continue;
        std::string fn = st.m_filename;
        if (!rootPrefix.empty() && fn.rfind(rootPrefix, 0) == 0)
            fn = fn.substr(rootPrefix.size());
        if (fn.empty()) continue;
        // Prevent path traversal.
        if (fn.find("..") != std::string::npos) continue;
        fs::path outPath = dest / fn;
        if (mz_zip_reader_is_file_a_directory(&za, i)) {
            fs::create_directories(outPath, ec);
            continue;
        }
        fs::create_directories(outPath.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&za, i, outPath.string().c_str(),
                                           0)) {
            // best-effort; skip unreadable entries
        }
    }
    mz_zip_reader_end(&za);

    // Record in the registry.
    WidgetBundle b;
    b.type = m.type;
    b.version = m.version;
    b.path = dest.string();
    b.manifestJson = m.raw;
    b.installedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    store_.upsertWidgetBundle(b);
    return m;
}

std::optional<WidgetManifest> WidgetRegistry::installFromFile(
    const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open file: " + path;
        return std::nullopt;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return installFromZip(buf, err);
}

std::string WidgetRegistry::resolveFile(const std::string& type,
                                        const std::string& version,
                                        const std::string& rel) {
    std::string ver = version;
    if (ver.empty()) {
        auto b = store_.latestBundle(type);
        if (b) ver = b->version;
    }
    if (ver.empty()) return {};
    // Prevent traversal.
    if (rel.find("..") != std::string::npos) return {};
    fs::path p = fs::path(widgetsDir_) / type / ver / rel;
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) return p.string();
    return {};
}

std::optional<WidgetManifest> WidgetRegistry::manifest(
    const std::string& type) {
    auto b = store_.latestBundle(type);
    if (!b) return std::nullopt;
    return parseManifest(b->manifestJson);
}

std::vector<uint8_t> WidgetRegistry::readBundleFile(const std::string& type,
                                                    const std::string& version,
                                                    const std::string& rel) {
    std::string path = resolveFile(type, version, rel);
    std::vector<uint8_t> out;
    if (path.empty()) return out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    return out;
}

std::string WidgetRegistry::appJsPath(const std::string& type,
                                      const std::string& version) {
    return resolveFile(type, version, "app.js");
}

std::string WidgetRegistry::registryJson() {
    json arr = json::array();
    auto bundles = store_.widgetBundles();
    // Only the latest version of each type in the palette.
    std::map<std::string, WidgetBundle> latest;
    for (const auto& b : bundles) {
        auto it = latest.find(b.type);
        if (it == latest.end() || b.installedAt > it->second.installedAt)
            latest[b.type] = b;
    }
    for (const auto& [type, b] : latest) {
        WidgetManifest m = parseManifest(b.manifestJson);
        json e;
        e["type"] = m.type;
        e["version"] = m.version;
        e["name"] = m.name;
        e["description"] = m.description;
        e["icon"] = "/widgets/" + m.type + "/" + m.version + "/" + m.icon;
        e["defaultSize"] = {{"w", m.defaultW}, {"h", m.defaultH}};
        e["config"] = m.configJson.empty()
                          ? json::array()
                          : json::parse(m.configJson, nullptr, false);
        e["subscribes"] = m.subscribes;
        e["capabilities"] = m.capabilities;
        arr.push_back(e);
    }
    return arr.dump();
}

}  // namespace evo
