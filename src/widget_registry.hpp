#pragma once

// WidgetRegistry — installs and serves EvoControl widget bundles (§8.1/§19).
//
// A bundle is a `.evw` (zip) containing manifest.json, icon.png (required),
// app.js (QuickJS game logic), obs.js + control.js (Lit elements), and
// assets/. On install the zip is extracted (miniz) into
//   <widgetsDir>/<type>/<version>/…
// and recorded in the widget_bundle table (Store). Installed widgets are fully
// trusted (no signing, §4).
//
// The registry also resolves bundle files for the HTTP server
// (GET /widgets/<type>/<version>/…) and provides the palette / registry list
// for the control host.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model.hpp"

namespace evo {

class Store;

struct WidgetManifest {
    std::string type;
    std::string version;
    int apiVersion = 1;
    std::string name;
    std::string description;
    std::string icon;  // relative path (e.g. "icon.png")
    double defaultW = 1080, defaultH = 1920;
    std::string configJson;      // the raw "config" array
    std::vector<std::string> subscribes;
    std::vector<std::string> capabilities;
    std::string raw;             // the full manifest JSON text
    bool valid = false;
};

class WidgetRegistry {
public:
    WidgetRegistry(Store& store, std::string widgetsDir);

    /// Directory containing installed widget bundles.
    const std::string& dir() const { return widgetsDir_; }

    /// Install a `.evw`/zip from an in-memory buffer. On success the bundle is
    /// extracted and recorded; returns the manifest. `err` gets a message on
    /// failure.
    std::optional<WidgetManifest> installFromZip(const std::vector<uint8_t>& zip,
                                                 std::string& err);
    /// Install from a file path (drag-drop of a .evw file).
    std::optional<WidgetManifest> installFromFile(const std::string& path,
                                                  std::string& err);

    /// Resolve a bundle file on disk: widgets/<type>/<version>/<rel>.
    /// If version is empty, the latest installed version is used.
    std::string resolveFile(const std::string& type, const std::string& version,
                            const std::string& rel);

    /// The latest installed manifest for a type (parsed).
    std::optional<WidgetManifest> manifest(const std::string& type);

    /// Registry listing for the control palette (JSON array). Each entry:
    /// {type, version, name, description, icon (url), defaultSize, config,
    /// subscribes, capabilities}.
    std::string registryJson();

    /// Read a file's bytes from an installed bundle (e.g. app.js for the
    /// runtime). Empty vector if not found.
    std::vector<uint8_t> readBundleFile(const std::string& type,
                                        const std::string& version,
                                        const std::string& rel);

    /// Path to app.js for the given type/version (latest if version empty).
    std::string appJsPath(const std::string& type, const std::string& version);

    static WidgetManifest parseManifest(const std::string& json);

private:
    Store& store_;
    std::string widgetsDir_;
};

}  // namespace evo
