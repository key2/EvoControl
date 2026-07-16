// DearTT — TikTok LIVE viewer.
//
//   video (FFmpeg -> OpenGL texture)  |  chat feed (ttlive-cpp)
//
// The main thread owns the window, ImGui and the GL texture. A ttlive client
// thread feeds chat + the stream URL; an FFmpeg thread feeds RGBA frames.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "misc/freetype/imgui_freetype.h"

#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

// Windows' GL/gl.h is OpenGL 1.1; this token was added in 1.2.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Silencing libprotobuf's console spam differs across major versions:
//   - protobuf <= 4.x (22.x): google::protobuf::SetLogHandler() in
//     <google/protobuf/stubs/logging.h>.
//   - protobuf >= 5.x (23+, incl. Homebrew's current build): logging moved to
//     Abseil; the stubs header is gone and severity is controlled globally via
//     absl::SetMinLogLevel / SetStderrThreshold.
#include <google/protobuf/stubs/common.h>
#if defined(GOOGLE_PROTOBUF_VERSION) && GOOGLE_PROTOBUF_VERSION < 4023000
#include <google/protobuf/stubs/logging.h>
#define DEARTT_PROTOBUF_HAS_SETLOGHANDLER 1
#else
#include <absl/log/globals.h>
#endif

#include "event_json.hpp"
#include "event_server.hpp"
#include "icon_cache.hpp"
#include "live_session.hpp"
#include "profile.hpp"
#include "stats.hpp"
#include "video_player.hpp"

// EvoControl subsystems (§10-§14).
#include "avatar_fetch.hpp"  // AvatarFetcher + decodeImageFileRGBA (always)
#include "file_dialog.hpp"
#include "game_engine.hpp"
#include "rest_api.hpp"
#include "store.hpp"
#include "widget_registry.hpp"

#ifdef DEARTT_FACE_RECOGNITION
#include "face_tracker.hpp"
#endif

#ifdef DEARTT_STT
#include <ggml-backend.h>

#include "model_download.hpp"
#include "stt.hpp"
#endif

namespace {

void glfwErrorCb(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

// Load a pan-Unicode base font so chat in non-Latin scripts renders (ImGui
// 1.92 loads glyphs dynamically, no ranges needed), and merge a CJK-capable
// fallback so rare Chinese/Japanese/Korean chat doesn't turn into
// replacement boxes.
//
// The preferred base is the bundled GoNotoKurrent-Regular.ttf
// (https://github.com/satbyy/go-noto-universal, SIL OFL): 80+ merged Noto
// scripts — Latin/Greek/Cyrillic, Arabic, Hebrew, Thai, Indic, CJK
// (Unihan-IICore subset), … — identical rendering on every OS, no system
// font hunting. System fonts remain as fallback when the asset is missing.
// Note: ImGui has no shaping/bidi, so Arabic still renders as unjoined
// letterforms in logical order regardless of the font.
void loadFont(ImGuiIO& io) {
    const char* base[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        // Wine prefixes usually have no TTFs in C:\windows\Fonts; the host
        // filesystem is mapped as Z:\ — harmless (skipped) on real Windows.
        "Z:/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "Z:/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    // Only *scalable* color-emoji fonts (COLR vector layers): bitmap-strike
    // fonts (Noto Color Emoji CBDT, Apple sbix) open fine but can't rasterize
    // at arbitrary sizes with imgui_freetype, leaving replacement glyphs.
    // The bundled Twemoji (COLR) covers Linux/macOS/Wine.
    std::vector<std::string> emoji = {
#if defined(_WIN32)
        "C:/Windows/Fonts/seguiemj.ttf",  // Segoe UI Emoji (COLR)
#endif
    };
    std::string bundled = findResource("fonts/Twemoji.Mozilla.ttf");
    if (!bundled.empty()) emoji.push_back(bundled);
    const char* cjk[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/msyh.ttc",      // Microsoft YaHei (SC)
        "C:/Windows/Fonts/meiryo.ttc",    // JP
        "C:/Windows/Fonts/malgun.ttf",    // KR
        "C:/Windows/Fonts/simsun.ttc",
        // Wine fallback via the Z:\ host mapping (skipped on real Windows).
        "Z:/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "Z:/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
#endif
    };
    // The base fonts (Noto Sans / DejaVu on Linux) have no Arabic coverage, so
    // Arabic chat rendered as replacement glyphs — merge an Arabic-capable
    // font. Note: ImGui has no text shaping/bidi, so Arabic renders as
    // isolated (unjoined) letterforms in logical order — legible, but not
    // typographically correct.
    const char* arabic[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",   // Segoe UI covers Arabic
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf",
        // Wine fallback via the Z:\ host mapping (skipped on real Windows).
        "Z:/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "Z:/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
        "Z:/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/GeezaPro.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // partial Arabic
#endif
    };

    // Preferred base: the bundled pan-Unicode font.
    bool haveUniversal = false;
    std::string universal = findResource("fonts/GoNotoKurrent-Regular.ttf");
    if (!universal.empty() &&
        io.Fonts->AddFontFromFileTTF(universal.c_str(), 17.0f))
        haveUniversal = true;

    bool haveBase = haveUniversal;
    for (const char* path : base) {
        if (haveBase) break;
        if (!fileExists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f)) haveBase = true;
    }
    if (!haveBase) io.Fonts->AddFontDefault();

    // GoNotoKurrent's CJK is the Unihan-IICore subset — merge a full-coverage
    // system CJK font to fill in rare ideographs (and as the only CJK source
    // when falling back to a system base font).
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const char* path : cjk) {
        if (!fileExists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg)) break;
    }

    // Arabic gap-filler — only needed when the bundled universal font (which
    // covers Arabic) is unavailable and the system base font lacks it.
    if (!haveUniversal) {
        ImFontConfig acfg;
        acfg.MergeMode = true;
        for (const char* path : arabic) {
            if (!fileExists(path)) continue;
            if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &acfg)) break;
        }
    }

    // Color emoji fallback — needs the FreeType rasterizer (COLR/CBDT glyph
    // formats); stb_truetype would silently drop every emoji.
    ImFontConfig ecfg;
    ecfg.MergeMode = true;
    ecfg.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
    for (const std::string& path : emoji) {
        if (!fileExists(path.c_str())) continue;
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 17.0f, &ecfg)) break;
    }
}

// Latest-frame video texture (RGBA8).
struct VideoTexture {
    GLuint id = 0;
    int w = 0, h = 0;
    float sar = 1.0f;   ///< pixel aspect ratio; display at (w * sar) x h

    void upload(const std::vector<uint8_t>& rgba, int fw, int fh) {
        if (!id) {
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (fw != w || fh != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba.data());
            w = fw;
            h = fh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA,
                            GL_UNSIGNED_BYTE, rgba.data());
        }
    }

    void destroy() {
        if (id) glDeleteTextures(1, &id);
        id = 0;
        w = h = 0;
        sar = 1.0f;
    }
};

ImVec4 chatColor(ChatItem::Kind k) {
    switch (k) {
        case ChatItem::Kind::Gift:      return ImVec4(1.00f, 0.75f, 0.25f, 1.0f);
        case ChatItem::Kind::Join:      return ImVec4(0.55f, 0.60f, 0.70f, 1.0f);
        case ChatItem::Kind::Follow:
        case ChatItem::Kind::Share:
        case ChatItem::Kind::Subscribe: return ImVec4(0.55f, 0.85f, 1.00f, 1.0f);
        case ChatItem::Kind::System:    return ImVec4(0.95f, 0.45f, 0.55f, 1.0f);
        default:                        return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    }
}

// Wrapped chat text as a layout item. With `shadow`, TikTok-style: a dark
// drop shadow underneath plus a second slightly-offset main pass (faux bold)
// so white text stays readable directly over bright video.
void chatText(const char* s, const ImVec4& col, float size, bool shadow) {
    ImFont* font = ImGui::GetFont();
    float wrapW = ImGui::GetContentRegionAvail().x;
    if (wrapW < 40.0f) wrapW = 40.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, wrapW, s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = ImGui::GetColorU32(col);
    if (shadow) {
        const ImU32 sc = IM_COL32(0, 0, 0, 230);
        dl->AddText(font, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), sc, s,
                    nullptr, wrapW);
        dl->AddText(font, size, ImVec2(pos.x + 0.7f, pos.y), c, s, nullptr,
                    wrapW);  // faux bold
    }
    dl->AddText(font, size, pos, c, s, nullptr, wrapW);
    ImGui::Dummy(sz);
}

// Round avatar inline with text; grey placeholder while it downloads.
void avatarImage(unsigned tex, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex)
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, p,
                            ImVec2(p.x + size, p.y + size), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, size * 0.5f);
    else
        dl->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f),
                            size * 0.5f, IM_COL32(70, 70, 82, 255));
    ImGui::Dummy(ImVec2(size, size));
}

std::string fmtCount(int64_t v) {
    char buf[32];
    if (v >= 1000000)
        std::snprintf(buf, sizeof(buf), "%.1fM", v / 1e6);
    else if (v >= 10000)
        std::snprintf(buf, sizeof(buf), "%.1fk", v / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
}

std::string fmtDuration(double sec) {
    int s = (int)sec;
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600,
                      (s / 60) % 60, s % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// One key/value row of the session summary.
void statRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

void drawStatsPanel(StatsCollector& stats) {
    const StatsTotals t = stats.totals();
    const auto& x = stats.tMin();
    const int n = (int)x.size();

    // --- session summary ------------------------------------------------
    if (ImGui::BeginTable("summary", 4,
                          ImGuiTableFlags_SizingStretchProp)) {
        statRow("time", fmtDuration(t.elapsedSec));
        statRow("viewers", fmtCount(t.viewers) + "  (peak " +
                               fmtCount(t.peakViewers) + ")");
        statRow("diamonds", fmtCount(t.diamonds));
        // Rough creator payout: ~USD 0.005 per diamond.
        char usd[32];
        std::snprintf(usd, sizeof(usd), "~$%.2f", t.diamonds * 0.005);
        statRow("est. value", usd);
        statRow("likes", fmtCount(t.likes));
        statRow("comments", fmtCount(t.comments));
        statRow("joins", fmtCount(t.joins));
        statRow("follows", fmtCount(t.follows));
        statRow("shares", fmtCount(t.shares));
        statRow("subs", fmtCount(t.subscribes));
        statRow("chatters", fmtCount(t.uniqueChatters));
        statRow("gifters", fmtCount(t.uniqueGifters));
        ImGui::EndTable();
    }
    ImGui::Separator();

    const ImPlotFlags plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
    const ImPlotAxisFlags axFlags = ImPlotAxisFlags_AutoFit;

    // --- viewers ----------------------------------------------------------
    if (ImPlot::BeginPlot("Viewers", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            ImPlot::PlotShaded(
                "viewers", x.data(), stats.viewersSeries().data(), n, 0.0,
                ImPlotSpec(ImPlotProp_FillAlpha, 0.25f));
            ImPlot::PlotLine("viewers", x.data(),
                             stats.viewersSeries().data(), n);
        }
        ImPlot::EndPlot();
    }

    // --- gift trend (derivative of diamonds) ------------------------------
    if (ImPlot::BeginPlot("Gifts: diamonds/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            const ImVec4 gold(1.0f, 0.75f, 0.25f, 1.0f);
            ImPlot::PlotShaded(
                "diamonds/min", x.data(), stats.diamondsPerMin().data(), n,
                0.0,
                ImPlotSpec(ImPlotProp_FillColor, gold, ImPlotProp_FillAlpha,
                           0.3f));
            ImPlot::PlotLine("diamonds/min", x.data(),
                             stats.diamondsPerMin().data(), n,
                             ImPlotSpec(ImPlotProp_LineColor, gold));
        }
        ImPlot::EndPlot();
    }

    // --- likes -------------------------------------------------------------
    if (ImPlot::BeginPlot("Likes/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            const ImVec4 pink(1.0f, 0.45f, 0.60f, 1.0f);
            ImPlot::PlotShaded(
                "likes/min", x.data(), stats.likesPerMin().data(), n, 0.0,
                ImPlotSpec(ImPlotProp_FillColor, pink, ImPlotProp_FillAlpha,
                           0.3f));
            ImPlot::PlotLine("likes/min", x.data(),
                             stats.likesPerMin().data(), n,
                             ImPlotSpec(ImPlotProp_LineColor, pink));
        }
        ImPlot::EndPlot();
    }

    // --- activity ----------------------------------------------------------
    if (ImPlot::BeginPlot("Comments/min", ImVec2(-1, 130), plotFlags)) {
        ImPlot::SetupAxes("min", nullptr, axFlags,
                          axFlags | ImPlotAxisFlags_LockMin);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
        if (n > 0) {
            ImPlot::PlotLine("comments", x.data(),
                             stats.commentsPerMin().data(), n);
        }
        ImPlot::EndPlot();
    }

    if (n == 0)
        ImGui::TextDisabled("waiting for events...");
}

// Gifts column: Top gifters (1/3) over the per-gift list (2/3), both
// scrollable. The gift list is ordered by units sent (count), not diamonds.
void drawGiftsPanel(StatsCollector& stats, IconCache& icons,
                    IconCache& avatars) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float headerH = ImGui::GetFrameHeightWithSpacing();

    ImGui::SeparatorText("Top gifters");
    ImGui::BeginChild("topgifters",
                      ImVec2(0, avail.y * (1.0f / 3.0f) - headerH));
    {
        auto gifters = stats.topGifters(100);
        int rank = 1;
        for (const auto& g : gifters) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%d.", rank++);
            ImGui::SameLine();
            avatarImage(avatars.texture(g.id), 18.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted(g.name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
            ImGui::TextDisabled("%s dia", fmtCount(g.diamonds).c_str());
        }
        if (gifters.empty()) ImGui::TextDisabled("no gifts yet");
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Gifts (by count)");
    ImGui::BeginChild("giftlist", ImVec2(0, 0));
    {
        auto gifts = stats.giftStats(/*byCount=*/true);
        int64_t totalCount = 0;
        for (const auto& g : gifts) totalCount += g.count;
        if (!gifts.empty() &&
            ImGui::BeginTable(
                "gifts", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed,
                                    26.0f);
            ImGui::TableSetupColumn("gift", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("dia", ImGuiTableColumnFlags_WidthFixed,
                                    52.0f);
            ImGui::TableHeadersRow();
            for (const auto& g : gifts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (unsigned tex = icons.texture(g.id))
                    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(22, 22));
                else
                    ImGui::Dummy(ImVec2(22, 22));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g.name.empty() ? "?" : g.name.c_str());
                if (g.price > 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%d diamonds each", g.price);
                ImGui::TableNextColumn();
                ImGui::Text("%s", fmtCount(g.count).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", totalCount > 0
                                          ? 100.0 * g.count / totalCount
                                          : 0.0);
                ImGui::TableNextColumn();
                ImGui::Text("%s", fmtCount(g.diamonds).c_str());
            }
            ImGui::EndTable();
        }
        if (gifts.empty()) ImGui::TextDisabled("no gifts yet");
    }
    ImGui::EndChild();
}

// The four activity waveforms (viewers, diamonds/min, likes/min, comments/min)
// laid out in a 2x2 grid. Each cell is half the available width; the height is
// split so both rows fit within `totalHeight`.
void drawWaveforms2x2(StatsCollector& stats, float totalHeight) {
    const auto& x = stats.tMin();
    const int n = (int)x.size();
    const ImPlotFlags plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                                  ImPlotFlags_NoLegend;
    const ImPlotAxisFlags axFlags = ImPlotAxisFlags_AutoFit;
    const ImGuiStyle& st = ImGui::GetStyle();
    float cellH = (totalHeight - st.ItemSpacing.y) * 0.5f;
    float cellW = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
    ImVec2 cell(cellW, cellH);

    auto plot = [&](const char* title, const std::vector<double>& y,
                    ImVec4 color, bool shade) {
        if (ImPlot::BeginPlot(title, cell, plotFlags)) {
            ImPlot::SetupAxes("min", nullptr, axFlags,
                              axFlags | ImPlotAxisFlags_LockMin);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 10, ImPlotCond_Once);
            ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0, INFINITY);
            if (n > 0) {
                if (shade)
                    ImPlot::PlotShaded(
                        title, x.data(), y.data(), n, 0.0,
                        ImPlotSpec(ImPlotProp_FillColor, color,
                                   ImPlotProp_FillAlpha, 0.3f));
                ImPlot::PlotLine(title, x.data(), y.data(), n,
                                 ImPlotSpec(ImPlotProp_LineColor, color));
            }
            ImPlot::EndPlot();
        }
    };

    plot("Viewers", stats.viewersSeries(), ImVec4(0.5f, 0.7f, 1.0f, 1.0f), true);
    ImGui::SameLine();
    plot("Diamonds/min", stats.diamondsPerMin(), ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
         true);
    plot("Likes/min", stats.likesPerMin(), ImVec4(1.0f, 0.45f, 0.60f, 1.0f),
         true);
    ImGui::SameLine();
    plot("Comments/min", stats.commentsPerMin(), ImVec4(0.6f, 0.9f, 0.7f, 1.0f),
         true);
}

const char* stateText(LiveSession::State s) {
    switch (s) {
        case LiveSession::State::Idle:         return "idle";
        case LiveSession::State::Connecting:   return "connecting...";
        case LiveSession::State::Connected:    return "LIVE";
        case LiveSession::State::Disconnected: return "disconnected";
        case LiveSession::State::Error:        return "error";
    }
    return "?";
}

// Files dropped onto the window (GLFW drop callback, main thread during
// glfwPollEvents); consumed by the avatar picture picker and the widget
// installer (.evw drag-drop, §8.1).
std::vector<std::string> g_droppedFiles;
// Cursor position (in framebuffer/pixel coords, matching ImGui screen space) at
// the moment of the drop. The OS drag doesn't emit ImGui mouse-move events, so
// ImGui::GetMousePos() is stale during a cross-app drag — we capture the real
// drop location here instead.
float g_dropX = -1.0f, g_dropY = -1.0f;
void dropCb(GLFWwindow* win, int count, const char** paths) {
    // Cursor pos in GLFW window coords, which is the same space ImGui uses for
    // mouse/screen coordinates with the GLFW backend.
    double cx = 0, cy = 0;
    glfwGetCursorPos(win, &cx, &cy);
    g_dropX = (float)cx;
    g_dropY = (float)cy;
    for (int i = 0; i < count; i++) g_droppedFiles.emplace_back(paths[i]);
}

#ifdef DEARTT_FACE_RECOGNITION
// Small GL-texture cache for display-only profile pictures, keyed by person.
struct AvatarTextures {
    struct Entry { std::string path; unsigned tex = 0; };
    std::map<std::string, Entry> map_;

    unsigned get(const std::string& person, const std::string& path) {
        if (path.empty()) return 0;
        auto it = map_.find(person);
        if (it != map_.end() && it->second.path == path) return it->second.tex;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!decodeImageFileRGBA(path, rgba, w, h, 96)) return 0;
        unsigned tex = (it != map_.end()) ? it->second.tex : 0u;
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
        map_[person] = {path, tex};
        return tex;
    }
    void invalidate(const std::string& person) {
        auto it = map_.find(person);
        if (it != map_.end()) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            map_.erase(it);
        }
    }
    void shutdown() {
        for (auto& [k, e] : map_)
            if (e.tex) glDeleteTextures(1, &e.tex);
        map_.clear();
    }
};
#endif

}  // namespace

int main(int argc, char** argv) {
    // The reflective webview JSON decode (event_json.cpp) parses every
    // Webcast message type. TikTok's schema drifts and some auto-generated
    // fields are mislabeled string-vs-bytes, which makes libprotobuf spam
    // "invalid UTF-8" errors to the console — drop its logging entirely
    // (failed parses already fall back to raw_base64 gracefully).
#if defined(DEARTT_PROTOBUF_HAS_SETLOGHANDLER)
    google::protobuf::SetLogHandler(nullptr);
#else
    // protobuf 5.x+: raise the Abseil log threshold above every level protobuf
    // emits so its "invalid UTF-8" warnings never reach stderr.
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfinity);
    absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
#endif

#ifdef DEARTT_STT
    // Bootstrap ggml's backend registry (including its Vulkan instance and
    // device enumeration) BEFORE any OpenGL context exists. Under Wine, the
    // host Vulkan loader fails to initialize once a GLX context is live
    // (winevulkan asserts in loader.c and the worker threads die, leaving
    // STT stuck at "loading model" and the face tracker silent). Doing the
    // first touch here, on the main thread, is fast and harmless on real
    // Windows/Linux.
    ggml_backend_dev_count();
#endif

    glfwSetErrorCallback(glfwErrorCb);
#if defined(GLFW_PLATFORM) && !defined(_WIN32) && !defined(__APPLE__)
    // Allow forcing the windowing backend on Linux: GLFW_PLATFORM=x11|wayland
    if (const char* plat = std::getenv("GLFW_PLATFORM")) {
        if (!std::strcmp(plat, "x11"))
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        else if (!std::strcmp(plat, "wayland"))
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }
#endif
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window =
        glfwCreateWindow(1280, 760,
                         "EvoControl - TikTok LIVE show control", nullptr,
                         nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync
    // Drag & drop: profile pictures (face recog) + widget .evw bundles (§8.1).
    glfwSetDropCallback(window, dropCb);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    loadFont(io);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // Event forwarder: website on http://localhost:<port>/, every TikTok
    // event as JSON on ws://localhost:<port>/ws. Declared before the session
    // so it outlives the client thread that broadcasts into it.
    EventServer eventServer;
    int wsPort = 8080;
    if (const char* p = std::getenv("EVOCONTROL_PORT")) {
        int v = std::atoi(p);
        if (v > 0 && v < 65536) wsPort = v;
    } else if (const char* p2 = std::getenv("DEARTT_PORT")) {
        int v = std::atoi(p2);
        if (v > 0 && v < 65536) wsPort = v;
    }

    // --- EvoControl core: SQLite store (WAL) + widget registry + game engine +
    // REST API, all wired into the server (§5/§10/§11). The store is the
    // salary source of truth; totals are rebuilt from the ledger on open.
    evo::Store store;
    {
        std::string dbPath =
            (std::filesystem::path(resourceRootDir()) / "evocontrol.db")
                .string();
        if (!store.open(dbPath))
            std::fprintf(stderr, "[evo] db open failed: %s\n",
                         store.error().c_str());
    }
    std::string widgetsDir =
        (std::filesystem::path(resourceRootDir()) / "widgets").string();
    evo::WidgetRegistry widgetRegistry(store, widgetsDir);
    // Auto-install bundled reference widgets (§21 phase 6) so the palette is
    // populated on first run. Any .evw next to the exe / source under
    // widgets-bundled/ is (re)installed; already-installed versions are just
    // refreshed. Operators install more by dragging .evw onto the app.
    {
        std::string bundled = findResource("widgets-bundled");
        std::error_code ec;
        if (!bundled.empty() && std::filesystem::is_directory(bundled, ec)) {
            for (const auto& e :
                 std::filesystem::directory_iterator(bundled, ec)) {
                if (e.path().extension() == ".evw") {
                    std::string err;
                    widgetRegistry.installFromFile(e.path().string(), err);
                }
            }
        }
    }
    evo::GameEngine gameEngine(store, widgetRegistry);
    evo::RestApi restApi(store, widgetRegistry, gameEngine);

    // The engine broadcasts confirmed state / shared plane / navigate to views.
    gameEngine.setBroadcaster([&eventServer](const std::string& scope,
                                             int64_t inst,
                                             const std::string& topic,
                                             const std::string& data) {
        eventServer.broadcastEnvelope(scope, inst, topic, data);
    });
    gameEngine.start();

    eventServer.setApi(&restApi);
    eventServer.setEngine(&gameEngine);
    eventServer.setRegistry(&widgetRegistry);
    if (auto pw = store.getSetting("control_password"))
        if (!pw->empty()) eventServer.setControlPassword(*pw);

    eventServer.start(wsPort, findWebDir());

    // Native app runtime state mirrored into the store.runtime singleton.
    int64_t evoAccountId = 0;   // active account (0 = none)
    int64_t evoSelectedScene = 0;  // scene selected for load/edit
    char newAccName[128] = "";     // "New account" modal fields (top bar)
    char newAccStream[128] = "";

    // --- Manage account window (Step 2) state --------------------------------
    // A working copy is loaded when the window opens; edits are only committed
    // to the SQLite store on Save (Cancel discards).
    bool showManageAccount = false;
    char manageStream[128] = "";        // editable @stream
    char manageNewPerformer[128] = "";  // "add performer" field
    // A performer row in the working copy. id==0 => newly added (not yet in DB).
    struct ManagePerformer {
        int64_t id = 0;             // DB id (0 for new rows)
        std::string name;           // original name (for change detection)
        std::string avatarPath;     // current saved avatar (for the thumbnail)
        bool deleted = false;       // marked for deletion on Save
        char nameBuf[128] = "";     // editable name
        char usernameBuf[128] = ""; // editable @username (TikTok fetch source)
        bool hasFace = false;       // has a captured face landmark
    };
    std::vector<ManagePerformer> managePerformers;
    // Background fetcher for TikTok @username profile pictures (Manage window).
    AvatarFetcher manageFetcher;
    // Small GL-texture cache for staged performer avatars (path -> tex).
    std::map<std::string, unsigned> manageAvatarTex;
    // Performer-avatar GL textures keyed by performer id (for the gift table),
    // loaded on demand from the store's avatar_path.
    std::map<int64_t, unsigned> performerAvatarTex;
    // Width of the resizable gift-table column (0 = initialize to video width).
    float giftTableW = 0.0f;
    // gift id -> icon URL (from the room's gift list), so we can fetch a gift's
    // icon on demand even for gifts that weren't pre-requested at connect time.
    std::map<int64_t, std::string> giftIconUrls;

    // Per-account avatars directory (used as the AvatarFetcher "profileDir").
    auto accountAvatarsDir = [&](int64_t accId) {
        return (std::filesystem::path(resourceRootDir()) / "avatars" /
                ("account_" + std::to_string(accId)))
            .string();
    };

    // Load the working copy from the store for the given account.
    auto openManageAccount = [&](int64_t accId) {
        auto acc = store.account(accId);
        if (!acc) return;
        std::snprintf(manageStream, sizeof(manageStream), "%s",
                      acc->stream.c_str());
        managePerformers.clear();
        for (const auto& p : store.performers(accId)) {
            ManagePerformer mp;
            mp.id = p.id;
            mp.name = p.name;
            mp.avatarPath = p.avatarPath;
            mp.hasFace = p.hasFace();
            std::snprintf(mp.nameBuf, sizeof(mp.nameBuf), "%s", p.name.c_str());
            std::snprintf(mp.usernameBuf, sizeof(mp.usernameBuf), "%s",
                          p.tiktokUser.c_str());
            managePerformers.push_back(mp);
        }
        manageNewPerformer[0] = '\0';
        showManageAccount = true;
    };

    // Ensure a working-copy performer exists in the DB (so its avatar can be
    // keyed by id) and return its id. Used when a live picture action happens
    // on a not-yet-saved row.
    auto ensurePerformerId = [&](ManagePerformer& mp) -> int64_t {
        if (mp.id) return mp.id;
        std::string name = mp.nameBuf;
        if (name.empty()) name = "performer";
        auto np = store.createPerformer(evoAccountId, name);
        if (np) {
            mp.id = np->id;
            mp.name = np->name;
        }
        return mp.id;
    };

    // Apply a picture immediately (the LAST action wins): fetch from a picked
    // file or the @username right now and attach to the performer. Async; the
    // frame-loop poll writes avatar_path + refreshes the thumbnail.
    auto applyPerformerPicture = [&](ManagePerformer& mp,
                                     AvatarFetcher::Source src,
                                     const std::string& value) {
        int64_t id = ensurePerformerId(mp);
        if (!id) return;
        manageFetcher.request(accountAvatarsDir(evoAccountId),
                              std::to_string(id), src, value);
    };

    // Commit name/username/stream/deletions to the store. Pictures are applied
    // live (above), not here.
    auto saveManageAccount = [&](int64_t accId) {
        auto acc = store.account(accId);
        if (acc) {
            std::string s = manageStream;
            if (!s.empty() && s[0] == '@') s.erase(0, 1);
            acc->stream = s;
            store.updateAccount(*acc);
        }
        for (auto& mp : managePerformers) {
            std::string name = mp.nameBuf;
            std::string user = mp.usernameBuf;
            if (!user.empty() && user[0] == '@') user.erase(0, 1);
            if (mp.deleted) {
                if (mp.id) store.deletePerformer(mp.id);
                continue;
            }
            int64_t id = mp.id;
            if (id == 0) {
                if (name.empty()) continue;
                auto np = store.createPerformer(accId, name);
                if (!np) continue;
                id = np->id;
                mp.id = id;
            } else if (name != mp.name) {
                store.renamePerformer(id, name);
            }
            store.setPerformerTiktokUser(id, user);
        }
        // Refresh the engine's in-memory roster (shared plane).
        gameEngine.postLoadAccount(accId);
    };

    LiveSession session;
    VideoPlayer player;
    VideoTexture videoTex;
    StatsCollector stats;
    IconCache giftIcons;
    IconCache avatars;

#ifdef DEARTT_FACE_RECOGNITION
    FaceTracker faceTracker;
    {
        std::string mdir = findResource("models");
        if (!mdir.empty()) faceTracker.start(mdir);
    }
    int64_t facesAccountId = 0;  // account whose roster the tracker holds
    // Point the FaceTracker at an account-specific gallery dir and seed its
    // roster with the account's performer names (§12/§15: landmarks are matched
    // by performer name; the engine maps recognized names back to performer ids).
    auto selectAccountFaces = [&](int64_t accId) {
        if (accId == 0 || accId == facesAccountId) return;
        facesAccountId = accId;
        std::error_code ec;
        std::filesystem::path dir =
            std::filesystem::path(resourceRootDir()) / "faces" /
            ("account_" + std::to_string(accId));
        std::filesystem::create_directories(dir, ec);
        faceTracker.selectProfile(dir.string());
        for (const auto& p : store.performers(accId)) {
            faceTracker.addPerson(p.name);
            // Seed the tracker's gallery from the SQLite landmark so matching
            // works immediately after account load (§12/§15).
            if (p.hasFace())
                faceTracker.assignEmbedding(p.name, p.faceEmbedding,
                                            p.faceQuality);
        }
    };
#endif

#ifdef DEARTT_STT
    SttEngine stt;
    ModelDownloader sttDownload;
    bool sttEnabled = false;
    bool sttAvailable = false;
    std::string sttModelPath;
    {
        // Resolve the Voxtral GGUF: $DEARTT_VOXTRAL_MODEL, else the first
        // .gguf under models/voxtral/.
        if (const char* m = std::getenv("DEARTT_VOXTRAL_MODEL"))
            sttModelPath = m;
        if (sttModelPath.empty()) {
            std::string dir = findResource("models/voxtral");
            std::error_code ec;
            if (!dir.empty())
                for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                    if (e.path().extension() == ".gguf") {
                        sttModelPath = e.path().string();
                        break;
                    }
        }
        if (!sttModelPath.empty()) {
            sttAvailable = true;
            stt.start(sttModelPath);
        } else if (!std::getenv("DEARTT_NO_MODEL_DOWNLOAD")) {
            // First run: the model is not shipped in the distributable
            // (~2.7 GB). Fetch it next to the executable in the background;
            // STT lights up when the download finishes.
            const char* u = std::getenv("DEARTT_VOXTRAL_URL");
            std::string url =
                u ? u
                  : "https://huggingface.co/andrijdavid/"
                    "Voxtral-Mini-4B-Realtime-2602-GGUF/resolve/main/"
                    "Q4_K_M.gguf";
            sttModelPath = (std::filesystem::path(resourceRootDir()) /
                            "models" / "voxtral" / "Q4_K_M.gguf")
                               .string();
            sttDownload.start(url, sttModelPath);
        }
        // Feed the decoded audio to STT (resampled to 16 kHz mono inside).
        // Always tapped: pushAudio is a no-op until the engine is ready, and
        // the model may only become available after the download.
        player.setAudioTap([&stt](const float* pcm, int frames, int ch,
                                  int rate) {
            stt.pushAudio(pcm, frames, ch, rate);
        });
    }
#endif

    session.setEventSink(
        [&eventServer, &stats, &avatars, &gameEngine](const ttlive::Event& e) {
            stats.record(e);
            // Fetch the avatar of anyone we might display (chat, top
            // gifters); IconCache dedupes and caps by itself.
            avatars.request(e.user.id, e.user.avatar_url);
            // Feed the game engine: it dedups streaks and credits the ledger.
            gameEngine.postEvent(e);
            eventServer.broadcast(eventToJson(e));
        });

    // REST-driven runtime hooks: connecting the ttlive client when an account
    // is loaded from the Control view / native console.
    restApi.resolveUserAvatar = [&avatars](int64_t) -> std::string {
        return {};  // avatars are GL textures here; served path TBD
    };
    restApi.facesJson = [&gameEngine]() -> std::string {
        return gameEngine.facesJsonArray();
    };

    // "GDI Generic" (Windows) or "llvmpipe" (Linux) here means software
    // rendering — the whole UI will be slow regardless of decoding.
    const char* glRenderer = (const char*)glGetString(GL_RENDERER);
    std::string renderer = glRenderer ? glRenderer : "?";

    // --- profiles: a profile pairs a stream with the people expected on it.
    std::vector<std::string> profileList = profiles::list();
    int profileIdx = -1;
    Profile curProfile;
    bool profileLoaded = false;
    bool showProfileMgr = false;
    char newProfName[128] = "";
    char newProfStream[128] = "";
    char newPersonName[128] = "";
    int assignTrackId = 0;          // face clicked for naming (for highlight)
    char assignNewName[128] = "";
    std::vector<float> assignEmb;   // embedding snapshotted at click time
    float assignArea = 0.0f;
#ifdef DEARTT_FACE_RECOGNITION
    AvatarFetcher avatarFetcher;
    AvatarTextures avatarTex;
    std::string avatarPicTarget;    // person the picture popup is editing
    std::string avatarDropTarget;   // person to receive a dropped image now
    char avatarTT[128] = "";        // TikTok @username input
    char avatarURL[256] = "";       // image URL input
#endif

    auto selectProfile = [&](int idx) {
        profileIdx = idx;
        profileLoaded = (idx >= 0 && idx < (int)profileList.size() &&
                         profiles::load(profileList[idx], curProfile));
#ifdef DEARTT_FACE_RECOGNITION
        if (profileLoaded) faceTracker.selectProfile(curProfile.dir);
#endif
    };

    // Optional dev shortcut: `deartt <username>` still connects directly.
    char userBuf[128] = "";
    if (argc > 1) {
        std::snprintf(userBuf, sizeof(userBuf), "%s",
                      argv[1][0] == '@' ? argv[1] + 1 : argv[1]);
        session.start(userBuf);
    }

    std::vector<ChatItem> chat;
    bool chatAutoScroll = true;
    bool chatOnly = true;    // §16.3 default: comments only
    bool showStats = true;
    bool showEvoConsole = false;  // EvoControl master console window (kept for
                                  // later steps; hidden while we rebuild the UI)
    enum { kChatRight = 0, kChatOverlay = 1, kChatOff = 2 };
    int chatMode = kChatOverlay;  // §16.3 default: overlay
    size_t chatAdded = 0;    // items appended this frame (for autoscroll)
    bool playerStarted = false;
    bool noStreamUrl = false;   // connected, but room gave us no video URLs
    std::vector<uint8_t> frameRgba;
    ttlive::StreamInfo streamInfo;   // qualities of the connected room
    std::string currentQuality;     // active quality key (e.g. "HD1")

    // Chat feed items + autoscroll; rendered either into the right-side pane
    // (shadow=false) or over the video (shadow=true, TikTok-style shadowed
    // bold text so it stays readable without a background panel).
    auto drawChatFeed = [&](bool shadow) {
        const float chatAvatarSz = 36.0f;                    // 2x
        const float chatFontSz = ImGui::GetFontSize() * 1.5f;
        const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
        const ImVec4 nameBlue(0.55f, 0.75f, 1.0f, 1.0f);
        for (const ChatItem& item : chat) {
            if (chatOnly && item.kind != ChatItem::Kind::Comment) continue;
            if (item.kind == ChatItem::Kind::Comment) {
                // [avatar]  Name
                //           message  (aligned with the name, not the icon)
                avatarImage(avatars.texture(item.userId), chatAvatarSz);
                ImGui::SameLine();
                ImGui::BeginGroup();  // lines below start at the name's x
                chatText(item.author.c_str(), nameBlue, chatFontSz, shadow);
                chatText(item.text.c_str(), white, chatFontSz, shadow);
                ImGui::EndGroup();
            } else if (item.author.empty()) {
                chatText(item.text.c_str(), chatColor(item.kind),
                         ImGui::GetFontSize(), shadow);
            } else {
                if (item.userId) {
                    avatarImage(avatars.texture(item.userId), chatAvatarSz);
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                std::string line = item.author + " " + item.text;
                chatText(line.c_str(), chatColor(item.kind), chatFontSz,
                         shadow);
                ImGui::EndGroup();
            }
            ImGui::Spacing();
        }
        if (chatAutoScroll && chatAdded > 0) ImGui::SetScrollHereY(1.0f);
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // --- session -> player handoff -------------------------------------
        if (session.state() == LiveSession::State::Connected && !playerStarted &&
            !noStreamUrl) {
            streamInfo = session.streamInfo();
            // Start with the stream's default quality (or whatever exists).
            currentQuality.clear();
            // Prefer 720p (HD1) when the room offers it; fall back to the
            // stream's default quality otherwise.
            std::string url = streamUrlFor(streamInfo, "HD1");
            if (url.empty()) url = streamUrlFor(streamInfo);
            for (const auto& q : streamInfo.qualities)
                if (!q.hls_url.empty() || !q.flv_url.empty()) {
                    if (url == q.hls_url || url == q.flv_url) {
                        currentQuality = q.quality;
                        break;
                    }
                }
            if (!url.empty()) {
                player.open(url);
                playerStarted = true;
            } else {
                // Room info returned no URLs at all: typically an age-/
                // audience-restricted LIVE, which TikTok only serves to
                // logged-in sessions (status_code 4003110).
                noStreamUrl = true;
            }
            // Publish the gift gallery to the engine's shared plane (§17).
            gameEngine.postGiftGallery(session.giftList());
        }
        if (session.state() != LiveSession::State::Connected &&
            session.state() != LiveSession::State::Connecting) {
            if (playerStarted) {
                player.close();
                playerStarted = false;
            }
            noStreamUrl = false;
        }

        // --- pull chat + video frame ---------------------------------------
        chatAdded = session.drainChat(chat);
        if (chat.size() > 5000)
            chat.erase(chat.begin(), chat.begin() + (chat.size() - 5000));

        int fw = 0, fh = 0;
        float fsar = 1.0f;
        if (player.takeFrame(frameRgba, fw, fh, fsar)) {
            videoTex.upload(frameRgba, fw, fh);
            videoTex.sar = fsar;
#ifdef DEARTT_FACE_RECOGNITION
            if (faceTracker.running())
                faceTracker.submitFrame(frameRgba.data(), fw, fh);
#endif
        }

#ifdef DEARTT_FACE_RECOGNITION
        // Push normalized face positions into the engine so ctx.faces() /
        // GET /api/faces reflect the tracker (§9/§15). Throttled to ~5 Hz.
        {
            static double lastFacePush = 0.0;
            double t = ImGui::GetTime();
            if (faceTracker.running() && videoTex.w > 0 && videoTex.h > 0 &&
                t - lastFacePush > 0.2) {
                lastFacePush = t;
                std::vector<evo::EngineFace> ef;
                for (const auto& tf : faceTracker.faces()) {
                    if (tf.name.empty()) continue;
                    evo::EngineFace f;
                    f.name = tf.name;
                    f.x = tf.box.x / (double)videoTex.w;
                    f.y = tf.box.y / (double)videoTex.h;
                    f.w = tf.box.w / (double)videoTex.w;
                    f.h = tf.box.h / (double)videoTex.h;
                    f.similarity = tf.similarity;
                    ef.push_back(f);
                }
                gameEngine.postFaces(ef);
            }
        }
#endif

        // Manage-account picture fetches: persist finished avatars (person key
        // is the performer id) and invalidate the thumbnail cache so it redraws.
        for (const auto& r : manageFetcher.poll()) {
            if (!r.ok) continue;
            int64_t pid = 0;
            try { pid = std::stoll(r.person); } catch (...) {}
            if (pid) store.setPerformerAvatar(pid, r.path);
            manageAvatarTex.erase(r.path);
            performerAvatarTex.erase(pid);  // refresh the gift-table thumbnail
            for (auto& mp : managePerformers)
                if (mp.id == pid) mp.avatarPath = r.path;
            if (evoAccountId) gameEngine.postLoadAccount(evoAccountId);
        }

        // Keep the gift id -> icon URL map fresh from the room's gift list
        // (it may arrive/refresh after connect), and request the icon for every
        // gift id that shows up in the live monitor — including gifts that were
        // not in the initial list. IconCache dedupes, so this is cheap.
        {
            static bool dbgIcons = std::getenv("EVO_DEBUG_ICONS") != nullptr;
            static bool dumpedList = false;
            for (const auto& g : session.giftList())
                if (!g.icon_url.empty()) {
                    auto& u = giftIconUrls[g.id];
                    if (u.empty()) {
                        u = g.icon_url;
                        giftIcons.request(g.id, g.icon_url);
                    }
                }
            // One-time dump of the full gift list (id, name, url) so we can see
            // whether a specific gift (e.g. "Hand Heart") is present and what
            // its icon URL is.
            if (dbgIcons && !dumpedList && !session.giftList().empty()) {
                dumpedList = true;
                std::fprintf(stderr, "[icon] gift list has %zu entries:\n",
                             session.giftList().size());
                for (const auto& g : session.giftList())
                    std::fprintf(stderr, "[icon]   id=%lld name='%s' url=%s\n",
                                 (long long)g.id, g.name.c_str(),
                                 g.icon_url.empty() ? "(none)"
                                                    : g.icon_url.c_str());
            }
            for (const auto& r : gameEngine.giftMonitor()) {
                if (!r.giftId) continue;
                auto it = giftIconUrls.find(r.giftId);
                if (it != giftIconUrls.end()) {
                    giftIcons.request(r.giftId, it->second);
                } else if (!r.giftIconUrl.empty()) {
                    // Not in the room's gift list (e.g. basic/universal gifts
                    // like "Hand Heart") — use the icon URL carried by the gift
                    // event itself. Cache it so we only request once.
                    giftIconUrls[r.giftId] = r.giftIconUrl;
                    giftIcons.request(r.giftId, r.giftIconUrl);
                } else if (dbgIcons) {
                    static std::set<int64_t> warned;
                    if (warned.insert(r.giftId).second)
                        std::fprintf(stderr,
                                     "[icon] MONITOR gift id=%lld name='%s' no "
                                     "icon URL in list OR event\n",
                                     (long long)r.giftId, r.giftName.c_str());
                }
            }
        }

        stats.sample();      // 1 Hz time series point (throttled internally)
        giftIcons.upload();  // decoded icons -> GL textures
        avatars.upload();

#ifdef DEARTT_STT
        // First-run model download finished -> bring STT up.
        if (!sttAvailable && sttDownload.finished()) {
            sttAvailable = true;
            stt.start(sttModelPath);
        }
#endif

#ifdef DEARTT_FACE_RECOGNITION
        // Apply finished profile-picture downloads.
        for (const auto& r : avatarFetcher.poll())
            if (r.ok) {
                faceTracker.setAvatar(r.person, r.path);
                avatarTex.invalidate(r.person);
            }
        avatarDropTarget.clear();  // set again this frame if the picker is open
#endif

        // --- UI --------------------------------------------------------------
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);

        // Top bar (EvoControl — Step 1): Account picker + New / Manage account /
        // Connect. Accounts are the SQLite-backed accounts (the evolved Profile,
        // §13); the old profile combo/manager is retained below but hidden while
        // we rebuild the console UI incrementally.
        {
            bool busy = session.state() == LiveSession::State::Connecting ||
                        session.state() == LiveSession::State::Connected;

            // Account combo — "Select Account".
            std::vector<evo::Account> accountList = store.accounts();
            evo::Account* curAccount = nullptr;
            for (auto& a : accountList)
                if (a.id == evoAccountId) curAccount = &a;
            const char* accPreview =
                curAccount ? curAccount->name.c_str() : "Select Account";
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##account", accPreview)) {
                for (const auto& a : accountList) {
                    bool sel = a.id == evoAccountId;
                    if (ImGui::Selectable(a.name.c_str(), sel) && !busy) {
                        evoAccountId = a.id;
                        gameEngine.postLoadAccount(a.id);
#ifdef DEARTT_FACE_RECOGNITION
                        selectAccountFaces(a.id);
#endif
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // New account.
            ImGui::SameLine();
            if (ImGui::Button("New")) {
                newAccName[0] = newAccStream[0] = '\0';
                ImGui::OpenPopup("New account");
            }

            // Manage account (disabled until an account is selected).
            ImGui::SameLine();
            ImGui::BeginDisabled(evoAccountId == 0);
            if (ImGui::Button("Manage account"))
                openManageAccount(evoAccountId);
            ImGui::EndDisabled();

            // New-account modal.
            if (ImGui::BeginPopupModal("New account", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputTextWithHint("name", "e.g. FridayShow", newAccName,
                                         sizeof(newAccName));
                ImGui::InputTextWithHint("stream", "@username", newAccStream,
                                         sizeof(newAccStream));
                if (ImGui::Button("Create") && newAccName[0]) {
                    std::string s = newAccStream;
                    if (!s.empty() && s[0] == '@') s.erase(0, 1);
                    auto a = store.createAccount(newAccName, s);
                    if (a) {
                        evoAccountId = a->id;
                        gameEngine.postLoadAccount(a->id);
#ifdef DEARTT_FACE_RECOGNITION
                        selectAccountFaces(a->id);
#endif
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // Connect (disabled until an account is selected).
            ImGui::SameLine();
            if (!busy) {
                bool canConnect = curAccount && !curAccount->stream.empty();
                ImGui::BeginDisabled(!canConnect);
                if (ImGui::Button("Connect") && canConnect) {
                    chat.clear();
                    videoTex.destroy();
                    playerStarted = false;
                    session.start(curAccount->stream);
                }
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Disconnect")) {
                    session.stop();
                    player.close();
                    playerStarted = false;
                }
            }

            // --- everything below is retained for later UI steps but hidden
            // now so Step 1 shows ONLY: account combo, New, Manage, Connect. ---
            if (/*Step-1 minimal UI*/ false) {
            ImGui::SameLine();
            LiveSession::State st = session.state();
            ImVec4 stCol = st == LiveSession::State::Connected
                               ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
                           : st == LiveSession::State::Error
                               ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                               : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            ImGui::TextColored(stCol, "[%s]", stateText(st));
            if (st == LiveSession::State::Error) {
                ImGui::SameLine();
                ImGui::TextWrapped("%s", session.error().c_str());
            }
            if (st == LiveSession::State::Connected) {
                ImGui::SameLine();
                ImGui::Text("@%s   %lld viewers   %lld likes",
                            session.roomTitleUser().c_str(),
                            (long long)session.viewerCount(),
                            (long long)session.totalLikes());

                // Audio: mute toggle + volume slider.
                if (playerStarted) {
                    AudioOutput& au = player.audio();
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 320);
                    bool muted = au.muted();
                    if (ImGui::Checkbox("mute", &muted)) au.setMuted(muted);
                    ImGui::SameLine();
                    float vol = au.volume();
                    ImGui::SetNextItemWidth(120);
                    ImGui::BeginDisabled(muted);
                    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "vol %.2f"))
                        au.setVolume(vol);
                    ImGui::EndDisabled();
                }

                // Quality switch (only qualities that actually have a URL).
                if (playerStarted && streamInfo.qualities.size() > 1) {
                    const ttlive::StreamQuality* cur = nullptr;
                    for (const auto& q : streamInfo.qualities)
                        if (q.quality == currentQuality) cur = &q;
                    std::string curLabel =
                        cur ? (cur->label.empty() ? cur->quality : cur->label)
                            : "auto";
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
                    ImGui::SetNextItemWidth(88);
                    if (ImGui::BeginCombo("##quality", curLabel.c_str())) {
                        for (const auto& q : streamInfo.qualities) {
                            if (q.hls_url.empty() && q.flv_url.empty())
                                continue;
                            std::string label =
                                q.label.empty() ? q.quality : q.label;
                            bool selected = q.quality == currentQuality;
                            if (ImGui::Selectable(label.c_str(), selected) &&
                                !selected) {
                                std::string url =
                                    streamUrlFor(streamInfo, q.quality);
                                if (!url.empty()) {
                                    currentQuality = q.quality;
                                    player.open(url);  // open() closes first
                                }
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }
            }  // end Step-1-hidden status/audio/quality block
        }

        // --- Step 3: connected view ----------------------------------------
        // A left column, no wider than the video, with three stacked parts:
        //   1) a thin control bar (mute, volume, "chat only"),
        //   2) the 1080x1920 (9:16) video with text overlay + face detection,
        //   3) a 2x2 grid of the four activity waveforms.
        if (session.state() == LiveSession::State::Connected) {
            ImGui::Separator();
            ImVec2 avail = ImGui::GetContentRegionAvail();

            // The video is 9:16. Size the left column so the video fits the
            // available height after the control bar + waveforms; the column is
            // never wider than the video.
            const float barH = ImGui::GetFrameHeightWithSpacing();
            const float waveH = 240.0f;
            float videoBoxH = avail.y - barH - waveH -
                              ImGui::GetStyle().ItemSpacing.y * 2.0f;
            if (videoBoxH < 100.0f) videoBoxH = 100.0f;
            float videoW = videoBoxH * 9.0f / 16.0f;   // 9:16
            if (videoW > avail.x) {                     // clamp to width
                videoW = avail.x;
                videoBoxH = videoW * 16.0f / 9.0f;
            }
            float colW = videoW;

            ImGui::BeginChild("leftcol", ImVec2(colW, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);

            // (1) thin control bar: mute / volume / chat only.
            {
                if (playerStarted) {
                    AudioOutput& au = player.audio();
                    bool muted = au.muted();
                    if (ImGui::Checkbox("mute", &muted)) au.setMuted(muted);
                    ImGui::SameLine();
                    float vol = au.volume();
                    ImGui::SetNextItemWidth(140);
                    ImGui::BeginDisabled(muted);
                    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "vol %.2f"))
                        au.setVolume(vol);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                }
                ImGui::Checkbox("chat only", &chatOnly);
            }

            // (2) the 9:16 video with overlay text + face detection.
            ImGui::BeginChild("videobox", ImVec2(colW, videoBoxH),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar);
            {
                ImVec2 area = ImGui::GetContentRegionAvail();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(p0, ImVec2(p0.x + area.x, p0.y + area.y),
                                  IM_COL32(8, 8, 10, 255));
                ImVec2 vidPos = p0, vidSize = area;

                if (videoTex.id && videoTex.w > 0) {
                    float dispW = videoTex.w * videoTex.sar;
                    float scale =
                        std::min(area.x / dispW, area.y / videoTex.h);
                    ImVec2 sz(dispW * scale, videoTex.h * scale);
                    ImVec2 off(p0.x + (area.x - sz.x) * 0.5f,
                               p0.y + (area.y - sz.y) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)videoTex.id, off,
                                 ImVec2(off.x + sz.x, off.y + sz.y));
                    vidPos = off;
                    vidSize = sz;

#ifdef DEARTT_FACE_RECOGNITION
                    // Face detection boxes + identity labels. Click a box to
                    // assign that face to a performer (captures the embedding).
                    if (faceTracker.running()) {
                        float fx = sz.x / (videoTex.w * videoTex.sar);
                        float fy = sz.y / (float)videoTex.h;
                        ImVec2 mouse = ImGui::GetMousePos();
                        bool clicked =
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                            ImGui::IsWindowHovered();
                        for (const auto& tf : faceTracker.faces()) {
                            ImVec2 bmin(off.x + tf.box.x * fx,
                                        off.y + tf.box.y * fy);
                            ImVec2 bmax(bmin.x + tf.box.w * fx,
                                        bmin.y + tf.box.h * fy);
                            int a = tf.coasting ? 110 : 220;
                            ImU32 col = tf.name.empty()
                                            ? IM_COL32(100, 200, 255, a)
                                            : IM_COL32(80, 255, 120, a);
                            dl->AddRect(bmin, bmax, col, 4.0f, 0, 2.0f);

                            // Click inside the box -> snapshot this face's
                            // embedding now, open the assign popup.
                            bool inside = mouse.x >= bmin.x &&
                                          mouse.x <= bmax.x &&
                                          mouse.y >= bmin.y && mouse.y <= bmax.y;
                            if (inside)
                                dl->AddRect(ImVec2(bmin.x - 2, bmin.y - 2),
                                            ImVec2(bmax.x + 2, bmax.y + 2),
                                            IM_COL32(255, 230, 90, 255), 5.0f, 0,
                                            2.0f);
                            if (clicked && inside && !tf.embedding.empty()) {
                                assignTrackId = tf.trackId;
                                assignEmb = tf.embedding;
                                assignArea = tf.box.w * tf.box.h;
                                ImGui::OpenPopup("AssignFaceVid");
                            }

                            if (!tf.name.empty()) {
                                char label[128];
                                std::snprintf(label, sizeof(label),
                                              "%s (%.0f%%)", tf.name.c_str(),
                                              tf.similarity * 100.0f);
                                ImVec2 ts = ImGui::CalcTextSize(label);
                                ImVec2 lp(bmin.x, bmin.y - ts.y - 4);
                                dl->AddRectFilled(
                                    ImVec2(lp.x - 2, lp.y - 1),
                                    ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 2),
                                    IM_COL32(0, 0, 0, 180), 3.0f);
                                dl->AddText(lp, col, label);
                            }
                        }

                        // Assign popup: pick a performer for the snapshotted
                        // face. Assigning to a performer that already has a
                        // landmark simply updates it (assign is unconditional).
                        if (ImGui::BeginPopup("AssignFaceVid")) {
                            ImGui::TextDisabled("Assign this face to:");
                            ImGui::Separator();
                            for (const auto& p : store.performers(evoAccountId)) {
                                std::string lbl =
                                    p.name + (p.hasFace() ? "  *" : "");
                                if (ImGui::Selectable(lbl.c_str())) {
                                    faceTracker.assignEmbedding(
                                        p.name, assignEmb, assignArea);
                                    // Persist the embedding to SQLite too.
                                    store.setPerformerFace(p.id, assignEmb,
                                                           assignArea);
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            if (store.performers(evoAccountId).empty())
                                ImGui::TextDisabled(
                                    "no performers — add some in Manage");
                            ImGui::EndPopup();
                        }
                    }
#endif

                    // Chat text overlay over the bottom of the video.
                    const float margin = 12.0f;
                    float oh = vidSize.y * 0.4f - margin;
                    float ow = vidSize.x - 2.0f * margin;
                    ImVec2 op(vidPos.x + margin,
                              vidPos.y + vidSize.y - oh - margin);
                    ImGui::SetCursorScreenPos(op);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                          ImVec4(0, 0, 0, 0));
                    ImGui::BeginChild("chatoverlay3", ImVec2(ow, oh),
                                      ImGuiChildFlags_None,
                                      ImGuiWindowFlags_NoScrollbar);
                    drawChatFeed(true);  // shadowed text over the video
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                } else {
                    const char* msg = "connecting / buffering...";
                    if (noStreamUrl) msg = "no stream URL for this room";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    dl->AddText(ImVec2(p0.x + (area.x - ts.x) * 0.5f,
                                       p0.y + (area.y - ts.y) * 0.5f),
                                IM_COL32(160, 160, 170, 255), msg);
                }
            }
            ImGui::EndChild();

            // (3) 2x2 waveforms, no wider than the video.
            drawWaveforms2x2(stats, waveH);

            ImGui::EndChild();  // leftcol

            // --- Step 4: gift column (fixed at 4/5 of the video width) ------
            // Top 2/3: the live inline gift feed. Bottom 1/3: two side-by-side
            // panels, "Gifts (by count)" and "Top Gifter".
            giftTableW = colW * 0.8f;  // 4/5 of the video width (not resizable)
            ImGui::SameLine();
            ImGui::BeginChild("giftcolumn", ImVec2(giftTableW, 0),
                              ImGuiChildFlags_None);
            {
                // Load a performer avatar texture on demand (by performer id).
                auto performerTex = [&](int64_t pid) -> unsigned {
                    if (!pid) return 0;
                    auto it = performerAvatarTex.find(pid);
                    if (it != performerAvatarTex.end()) return it->second;
                    unsigned tex = 0;
                    auto p = store.performer(pid);
                    if (p && !p->avatarPath.empty()) {
                        std::vector<uint8_t> rgba;
                        int w = 0, h = 0;
                        if (decodeImageFileRGBA(p->avatarPath, rgba, w, h, 64)) {
                            glGenTextures(1, &tex);
                            glBindTexture(GL_TEXTURE_2D, tex);
                            glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                        }
                    }
                    performerAvatarTex[pid] = tex;  // cache (even 0 = no avatar)
                    return tex;
                };

                // Top 2/3: the live inline gift feed (scrollable).
                ImVec2 giftAvail = ImGui::GetContentRegionAvail();
                float feedH = giftAvail.y * (2.0f / 3.0f);
                ImGui::BeginChild("giftfeed", ImVec2(0, feedH),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar);

                // One inline line per gift, e.g.:
                //   (pic) (@user): -> Rose (icon) x5 = 5  for  (pic) Alice
                // Match the video chat overlay exactly: same font size, same
                // colors, same shadowed/faux-bold text (chatText, shadow=true).
                //   line 1:  @username                 (name blue, like chat)
                //   line 2:  Rose (gift pic) x2 = 2(red) for (pic) Alice(blue)
                const float chatFontSz = ImGui::GetFontSize() * 1.5f;
                const float avatarSz = 36.0f;               // like the overlay
                const float giftIco = chatFontSz * 2.0f;    // gift icon 2x
                const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
                const ImVec4 nameBlue(0.55f, 0.75f, 1.0f, 1.0f);
                const ImVec4 red(1.0f, 0.35f, 0.35f, 1.0f);

                // Inline shadowed text piece (SameLine after).
                auto ptxt = [&](const std::string& s, const ImVec4& col) {
                    chatText(s.c_str(), col, chatFontSz, true);
                    ImGui::SameLine();
                };
                // Inline image centered on the chat line, then SameLine.
                auto inlineImg = [&](unsigned tex, bool circle, float sz) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float dy = (chatFontSz - sz) * 0.5f;
                    ImVec2 tl(p.x, p.y + dy);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (tex) {
                        if (circle)
                            dl->AddImageRounded(
                                (ImTextureID)(intptr_t)tex, tl,
                                ImVec2(tl.x + sz, tl.y + sz), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, sz * 0.5f);
                        else
                            dl->AddImage((ImTextureID)(intptr_t)tex, tl,
                                         ImVec2(tl.x + sz, tl.y + sz));
                    } else if (circle) {
                        dl->AddCircleFilled(
                            ImVec2(tl.x + sz * 0.5f, tl.y + sz * 0.5f),
                            sz * 0.5f, IM_COL32(70, 70, 82, 255));
                    }
                    ImGui::Dummy(ImVec2(sz, sz > chatFontSz ? sz : chatFontSz));
                    ImGui::SameLine();
                };

                for (const auto& r : gameEngine.giftMonitor()) {
                    // Gifter picture on the left (overlay avatar size).
                    avatarImage(avatars.texture(r.gifterId), avatarSz);
                    ImGui::SameLine();

                    // Two stacked lines to the right, aligned like chat.
                    ImGui::BeginGroup();
                    ptxt("@" + r.gifterName, nameBlue);
                    ImGui::NewLine();
                    ptxt(r.giftName, white);
                    inlineImg(giftIcons.texture(r.giftId), false, giftIco);
                    char lead[48];
                    std::snprintf(lead, sizeof(lead), "x%lld =",
                                  (long long)r.amount);
                    ptxt(lead, white);
                    ptxt(std::to_string((long long)r.diamonds), red);
                    if (r.performerId) {
                        ptxt("for", white);
                        inlineImg(performerTex(r.performerId), true, chatFontSz);
                        chatText(r.performerName.c_str(), nameBlue, chatFontSz,
                                 true);
                    } else {
                        ImGui::NewLine();
                    }
                    ImGui::EndGroup();
                    // Visible divider between each gift entry.
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }
                ImGui::EndChild();  // giftfeed (top 2/3)

                // --- Bottom 1/3: "Gifts (by count)" | "Top Gifter" ----------
                ImGui::Spacing();
                float halfW =
                    (ImGui::GetContentRegionAvail().x -
                     ImGui::GetStyle().ItemSpacing.x) * 0.5f;

                // Left: gift counts.
                ImGui::BeginChild("giftcounts", ImVec2(halfW, 0),
                                  ImGuiChildFlags_Borders);
                {
                    ImGui::SeparatorText("Gifts (by count)");
                    auto gifts = stats.giftStats(/*byCount=*/true);
                    for (const auto& g : gifts) {
                        if (unsigned tex = giftIcons.texture(g.id))
                            ImGui::Image((ImTextureID)(intptr_t)tex,
                                         ImVec2(20, 20));
                        else
                            ImGui::Dummy(ImVec2(20, 20));
                        ImGui::SameLine();
                        ImGui::TextUnformatted(g.name.empty() ? "?"
                                                              : g.name.c_str());
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44);
                        ImGui::Text("x%s", fmtCount(g.count).c_str());
                    }
                    if (gifts.empty()) ImGui::TextDisabled("no gifts yet");
                }
                ImGui::EndChild();

                ImGui::SameLine();

                // Right: top gifters.
                ImGui::BeginChild("topgifter", ImVec2(0, 0),
                                  ImGuiChildFlags_Borders);
                {
                    ImGui::SeparatorText("Top Gifter");
                    auto gifters = stats.topGifters(100);
                    int rank = 1;
                    for (const auto& g : gifters) {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                           "%d.", rank++);
                        ImGui::SameLine();
                        avatarImage(avatars.texture(g.id), 18.0f);
                        ImGui::SameLine();
                        ImGui::TextUnformatted(g.name.c_str());
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
                        ImGui::TextDisabled("%s", fmtCount(g.diamonds).c_str());
                    }
                    if (gifters.empty()) ImGui::TextDisabled("no gifts yet");
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();  // giftcolumn

            // Right side (control panel) — intentionally empty for now.
            ImGui::SameLine();
            ImGui::BeginChild("controlpanel", ImVec2(0, 0),
                              ImGuiChildFlags_None);
            ImGui::EndChild();
        }

        // --- Step 1: the rest of the main-window body (web-server line, chat
        // radios, stats/STT/face controls, and the video/gifts/stats/chat
        // columns) is retained but hidden. Flip kStep1MinimalUi to restore. ---
        const bool kStep1MinimalUi = true;
        if (!kStep1MinimalUi) {
        if (eventServer.running())
            ImGui::TextDisabled(
                "web http://localhost:%d  ·  ws://localhost:%d/ws  ·  %d "
                "client%s",
                eventServer.port(), eventServer.port(),
                eventServer.clientCount(),
                eventServer.clientCount() == 1 ? "" : "s");
        else
            ImGui::TextDisabled("web server not running (%s)",
                                eventServer.error().c_str());
        // In overlay mode the chat pane header doesn't exist, so its two
        // toggles move up here instead of floating over the video.
        float ctrlsW = 356.0f + (chatMode == kChatOverlay ? 208.0f : 0.0f);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ctrlsW);
        ImGui::TextDisabled("chat:");
        ImGui::SameLine();
        ImGui::RadioButton("right", &chatMode, kChatRight);
        ImGui::SameLine();
        ImGui::RadioButton("overlay", &chatMode, kChatOverlay);
        ImGui::SameLine();
        ImGui::RadioButton("off", &chatMode, kChatOff);
        if (chatMode == kChatOverlay) {
            ImGui::SameLine();
            ImGui::Checkbox("chat only", &chatOnly);
            ImGui::SameLine();
            ImGui::Checkbox("autoscroll", &chatAutoScroll);
        }
        ImGui::SameLine();
        ImGui::Checkbox("stats", &showStats);
#ifdef DEARTT_STT
        ImGui::SameLine();
        if (!sttAvailable) ImGui::BeginDisabled();
        if (ImGui::Checkbox("STT", &sttEnabled)) stt.setEnabled(sttEnabled);
        if (!sttAvailable) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            if (sttAvailable)
                ImGui::SetTooltip(
                    "Speech-to-text (Voxtral): transcribe the stream audio\n"
                    "into the panel next to the video.");
            else if (sttDownload.active())
                ImGui::SetTooltip(
                    "Downloading the Voxtral model — STT unlocks when the\n"
                    "download finishes.");
            else if (!sttDownload.error().empty())
                ImGui::SetTooltip("Model download failed: %s\n"
                                  "Restart the app to retry (it resumes).",
                                  sttDownload.error().c_str());
            else
                ImGui::SetTooltip(
                    "No Voxtral model found (models/voxtral/*.gguf or\n"
                    "$DEARTT_VOXTRAL_MODEL).");
        }
#endif
#ifdef DEARTT_FACE_RECOGNITION
        if (faceTracker.running()) {
            // Identity smoothing window (temporal vote + coast duration).
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            float win = faceTracker.identityWindow();
            if (ImGui::SliderFloat("##idwin", &win, 1.0f, 10.0f, "id %.0fs"))
                faceTracker.setIdentityWindow(win);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Identity window: names are the majority vote over the\n"
                    "last N seconds, and a face that briefly turns away is\n"
                    "kept for this long. Longer = steadier, slower to switch.");
        }
#endif
        ImGui::Separator();

        // Split: video (left, stretch) | STT | gifts | stats | chat (fixed
        // panes; chat only in "right" mode — "overlay" draws over the video).
        const float chatWidth = chatMode == kChatRight ? 340.0f : 0.0f;
        const float giftsWidth = showStats ? 300.0f : 0.0f;
        const float statsWidth = showStats ? 330.0f : 0.0f;
        float sttWidth = 0.0f;
#ifdef DEARTT_STT
        if (sttEnabled) sttWidth = 320.0f;
#endif
        int sidePanes = (showStats ? 2 : 0) + (chatMode == kChatRight ? 1 : 0) +
                        (sttWidth > 0 ? 1 : 0);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float videoWidth = avail.x - chatWidth - giftsWidth - statsWidth -
                           sttWidth - 8.0f * (float)sidePanes;

        ImGui::BeginChild("video", ImVec2(videoWidth, 0),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        {
            ImVec2 area = ImGui::GetContentRegionAvail();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p0, ImVec2(p0.x + area.x, p0.y + area.y),
                              IM_COL32(8, 8, 10, 255));

            // Rectangle actually covered by the video after aspect-fit
            // (the child is letterboxed); the chat overlay anchors to it.
            ImVec2 vidPos = p0, vidSize = area;

            if (videoTex.id && videoTex.w > 0) {
                // Aspect-fit on the DISPLAY size: anamorphic streams have
                // non-square pixels (sar != 1), so the presented width is
                // w * sar — using raw pixel dimensions would distort them.
                float dispW = videoTex.w * videoTex.sar;
                float scale = std::min(area.x / dispW, area.y / videoTex.h);
                ImVec2 sz(dispW * scale, videoTex.h * scale);
                ImVec2 off(p0.x + (area.x - sz.x) * 0.5f,
                           p0.y + (area.y - sz.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)videoTex.id, off,
                             ImVec2(off.x + sz.x, off.y + sz.y));
                vidPos = off;
                vidSize = sz;

#ifdef DEARTT_FACE_RECOGNITION
                // Face bounding boxes + identity labels; click a face to name.
                if (faceTracker.running()) {
                    // Map from frame pixels to display pixels.
                    float fx = sz.x / (videoTex.w * videoTex.sar);
                    float fy = sz.y / (float)videoTex.h;
                    ImVec2 mouse = ImGui::GetMousePos();
                    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                   ImGui::IsWindowHovered();
                    auto trackedFaces = faceTracker.faces();
                    // name -> display-picture path, for the label thumbnails.
                    std::map<std::string, std::string> avByName;
                    std::map<std::string, int> idxByName;  // roster position
                    {
                        int i = 0;
                        for (const auto& p : faceTracker.roster()) {
                            if (!p.avatar.empty()) avByName[p.name] = p.avatar;
                            idxByName[p.name] = ++i;  // 1..N, stable per person
                        }
                    }
                    for (const auto& tf : trackedFaces) {
                        ImVec2 bmin(off.x + tf.box.x * fx,
                                    off.y + tf.box.y * fy);
                        ImVec2 bmax(bmin.x + tf.box.w * fx,
                                    bmin.y + tf.box.h * fy);
                        // Green = named, blue = unknown; dimmed while coasting.
                        int a = tf.coasting ? 110 : 220;
                        ImU32 col = tf.name.empty()
                                        ? IM_COL32(100, 200, 255, a)
                                        : IM_COL32(80, 255, 120, a);
                        dl->AddRect(bmin, bmax, col, 4.0f, 0, 2.0f);

                        // Click inside the box -> snapshot THIS face's
                        // embedding now, then open the naming popup. The
                        // assignment later uses this fixed snapshot, so it
                        // can't drift to another face while the popup is open.
                        if (clicked && mouse.x >= bmin.x && mouse.x <= bmax.x &&
                            mouse.y >= bmin.y && mouse.y <= bmax.y &&
                            !tf.embedding.empty()) {
                            assignTrackId = tf.trackId;
                            assignEmb = tf.embedding;
                            assignArea = tf.box.w * tf.box.h;
                            assignNewName[0] = '\0';
                            ImGui::OpenPopup("AssignFace");
                        }

                        // Highlight the face currently being named.
                        if (tf.trackId == assignTrackId &&
                            ImGui::IsPopupOpen("AssignFace"))
                            dl->AddRect(ImVec2(bmin.x - 2, bmin.y - 2),
                                        ImVec2(bmax.x + 2, bmax.y + 2),
                                        IM_COL32(255, 230, 90, 255), 5.0f, 0,
                                        3.0f);

                        // Badge anchored to the roster, not the track: a known
                        // person shows their fixed number (1..N, never more
                        // than the people in the profile); an unknown face
                        // shows "?" (click it to name). No runaway counter.
                        char idxBuf[8];
                        auto ii = idxByName.find(tf.name);
                        if (!tf.name.empty() && ii != idxByName.end())
                            std::snprintf(idxBuf, sizeof(idxBuf), "#%d",
                                          ii->second);
                        else
                            std::snprintf(idxBuf, sizeof(idxBuf), "?");
                        ImVec2 is = ImGui::CalcTextSize(idxBuf);
                        dl->AddRectFilled(
                            bmin,
                            ImVec2(bmin.x + is.x + 6, bmin.y + is.y + 4),
                            IM_COL32(0, 0, 0, 200), 3.0f);
                        dl->AddText(ImVec2(bmin.x + 3, bmin.y + 2),
                                    IM_COL32(255, 255, 255, 255), idxBuf);

                        // Identity label above the box: name + match quality
                        // (cosine similarity, not vote agreement — a weak
                        // match now reads low instead of a misleading 100%).
                        if (!tf.name.empty()) {
                            char label[128];
                            std::snprintf(label, sizeof(label), "%s (%.0f%%)",
                                          tf.name.c_str(),
                                          tf.similarity * 100.0f);
                            ImVec2 ts = ImGui::CalcTextSize(label);
                            // Optional display-picture thumbnail before the name.
                            unsigned av = 0;
                            auto ai = avByName.find(tf.name);
                            if (ai != avByName.end())
                                av = avatarTex.get(tf.name, ai->second);
                            float avSz = av ? ts.y + 4 : 0.0f;
                            ImVec2 lp(bmin.x, bmin.y - ts.y - 4);
                            dl->AddRectFilled(
                                ImVec2(lp.x - 2, lp.y - 1),
                                ImVec2(lp.x + avSz + ts.x + 4, lp.y + ts.y + 2),
                                IM_COL32(0, 0, 0, 180), 3.0f);
                            if (av) {
                                dl->AddImageRounded(
                                    (ImTextureID)(intptr_t)av,
                                    ImVec2(lp.x, lp.y),
                                    ImVec2(lp.x + avSz - 2, lp.y + avSz - 2),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                                    (avSz - 2) * 0.5f);
                            }
                            dl->AddText(ImVec2(lp.x + avSz, lp.y), col, label);
                        }
                    }

                    // Naming popup: assigns the embedding snapshotted at
                    // click (assignEmb), so the target face is locked in.
                    if (ImGui::BeginPopup("AssignFace")) {
                        ImGui::TextDisabled("Assign this face to:");
                        ImGui::Separator();
                        auto roster = faceTracker.roster();
                        for (const auto& p : roster) {
                            std::string lbl = p.name + (p.hasFace ? "  *" : "");
                            if (ImGui::Selectable(lbl.c_str())) {
                                faceTracker.assignEmbedding(p.name, assignEmb,
                                                            assignArea);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        if (roster.empty())
                            ImGui::TextDisabled("no people yet");
                        ImGui::Separator();
                        ImGui::SetNextItemWidth(160);
                        bool go = ImGui::InputTextWithHint(
                            "##newassign", "new name", assignNewName,
                            sizeof(assignNewName),
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        ImGui::SameLine();
                        if ((ImGui::Button("Add") || go) && assignNewName[0]) {
                            faceTracker.addPerson(assignNewName);
                            faceTracker.assignEmbedding(assignNewName, assignEmb,
                                                        assignArea);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }
#endif

                // Stats overlay: decode fps vs UI fps + GL renderer + STT
                // compute backend.
                char sarTxt[32] = "";
                if (videoTex.sar < 0.999f || videoTex.sar > 1.001f)
                    std::snprintf(sarTxt, sizeof(sarTxt), " (sar %.3f)",
                                  videoTex.sar);
                char sttTxt[48] = "";
#ifdef DEARTT_STT
                if (stt.ready())
                    std::snprintf(sttTxt, sizeof(sttTxt), " | stt %s",
                                  stt.backend().c_str());
#endif
                char stats[256];
                std::snprintf(stats, sizeof(stats),
                              "%dx%d%s @ %.1f fps | ui %.0f fps | %s%s",
                              videoTex.w, videoTex.h, sarTxt, player.fps(),
                              io.Framerate, renderer.c_str(), sttTxt);
                ImVec2 tp(p0.x + 6, p0.y + 4);
                dl->AddRectFilled(
                    ImVec2(tp.x - 3, tp.y - 2),
                    ImVec2(tp.x + ImGui::CalcTextSize(stats).x + 3,
                           tp.y + ImGui::GetTextLineHeight() + 2),
                    IM_COL32(0, 0, 0, 140));
                dl->AddText(tp, IM_COL32(180, 220, 180, 255), stats);
            } else {
                const char* msg = "no video";
                switch (player.state()) {
                    case VideoPlayer::State::Opening: msg = "opening stream..."; break;
                    case VideoPlayer::State::Playing: msg = "buffering..."; break;
                    case VideoPlayer::State::Ended:   msg = "stream ended"; break;
                    case VideoPlayer::State::Error:   msg = "video error"; break;
                    default: break;
                }
                if (noStreamUrl)
                    msg = "TikTok returned no stream URL for this room.\n"
                          "This LIVE is likely age-restricted, and TikTok\n"
                          "only serves its video to logged-in sessions.\n"
                          "\n"
                          "Restart with DEARTT_COOKIES=\"sessionid=<value>\"\n"
                          "(cookie from a logged-in tiktok.com browser tab).";
                ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText(ImVec2(std::max(p0.x + 12.0f,
                                            p0.x + (area.x - ts.x) * 0.5f),
                                   p0.y + (area.y - ts.y) * 0.5f),
                            IM_COL32(160, 160, 170, 255), msg);
                if (player.state() == VideoPlayer::State::Error) {
                    std::string err = player.error();
                    ImVec2 es = ImGui::CalcTextSize(err.c_str());
                    dl->AddText(ImVec2(p0.x + (area.x - es.x) * 0.5f,
                                       p0.y + (area.y - ts.y) * 0.5f + 22),
                                IM_COL32(220, 90, 90, 255), err.c_str());
                }
            }

            // Chat overlay: a column over the bottom 2/5 of the VIDEO
            // rectangle (not the letterboxed child), anchored to the
            // video's left edge with a small margin. Fully transparent.
            if (chatMode == kChatOverlay) {
                const float margin = 14.0f;
                float oh = vidSize.y * 0.4f - margin;
                float ow = vidSize.x - 2.0f * margin;  // full video width
                ImVec2 op(vidPos.x + margin,
                          vidPos.y + vidSize.y - oh - margin);

                ImGui::SetCursorScreenPos(op);
                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                      ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::BeginChild("chatoverlay", ImVec2(ow, oh),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar);
                drawChatFeed(true);  // shadowed text over the video
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

#ifdef DEARTT_STT
        // Speech-to-text column, right next to the video.
        if (sttWidth > 0) {
            ImGui::SameLine();
            ImGui::BeginChild("sttpane", ImVec2(sttWidth, 0),
                              ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Speech-to-text");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44);
            if (ImGui::SmallButton("clear")) stt.clearTranscript();
            ImGui::TextDisabled("%s", stt.status().c_str());
            ImGui::Separator();
            ImGui::BeginChild("sttlog", ImVec2(0, 0), ImGuiChildFlags_None);
            std::string tr = stt.transcript();
            if (tr.empty())
                ImGui::TextDisabled(stt.ready() ? "listening..."
                                                : "loading model...");
            else {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(tr.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
                    ImGui::SetScrollHereY(1.0f);  // autoscroll when at bottom
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }
#endif

        if (showStats) {
            ImGui::SameLine();
            ImGui::BeginChild("giftspane", ImVec2(giftsWidth, 0),
                              ImGuiChildFlags_None);
            drawGiftsPanel(stats, giftIcons, avatars);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("statspane", ImVec2(statsWidth, 0),
                              ImGuiChildFlags_None);
            drawStatsPanel(stats);
            ImGui::EndChild();
        }

        if (chatMode == kChatRight) {
            ImGui::SameLine();
            ImGui::BeginChild("chatpane", ImVec2(0, 0), ImGuiChildFlags_None);
            ImGui::TextUnformatted("Chat");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
            ImGui::Checkbox("chat only", &chatOnly);
            ImGui::SameLine();
            ImGui::Checkbox("autoscroll", &chatAutoScroll);
            ImGui::Separator();

            ImGui::BeginChild("chatlog", ImVec2(0, 0),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            drawChatFeed(false);
            ImGui::EndChild();
            ImGui::EndChild();
        }
        }  // end !kStep1MinimalUi (hidden main-window body)

        ImGui::End();

        // --- Manage account modal (Step 2) ---------------------------------
        // A MODAL so nothing behind it (New / Connect / combo) is clickable
        // until it's closed. Edits the account's @stream and its performer
        // roster (add / delete / rename / profile picture); staged in the
        // working copy and committed to SQLite only on Save (Cancel discards).
        if (showManageAccount && !ImGui::IsPopupOpen("###manageaccount"))
            ImGui::OpenPopup("###manageaccount");
        if (showManageAccount) {
            // Load a staged avatar image into a GL texture (cached by path).
            auto avatarTexFor = [&](const std::string& path) -> unsigned {
                if (path.empty()) return 0;
                auto it = manageAvatarTex.find(path);
                if (it != manageAvatarTex.end()) return it->second;
                std::vector<uint8_t> rgba;
                int w = 0, h = 0;
                if (!decodeImageFileRGBA(path, rgba, w, h, 96)) {
                    manageAvatarTex[path] = 0;
                    return 0;
                }
                unsigned tex = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba.data());
                manageAvatarTex[path] = tex;
                return tex;
            };

            auto acc = store.account(evoAccountId);
            std::string title =
                "Manage account: " + (acc ? acc->name : std::string("?"));
            // Fixed, non-resizable, centered modal.
            const ImGuiViewport* mvp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(mvp->GetCenter().x, mvp->GetCenter().y),
                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_Always);
            if (ImGui::BeginPopupModal(
                    (title + "###manageaccount").c_str(), &showManageAccount,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoMove)) {
                ImGui::Text("Account: %s", acc ? acc->name.c_str() : "?");

                ImGui::TextDisabled("Stream");
                ImGui::SetNextItemWidth(260);
                ImGui::InputTextWithHint("##stream", "@username", manageStream,
                                         sizeof(manageStream));

                ImGui::SeparatorText("Performers");

                // Add a performer to the working copy.
                ImGui::SetNextItemWidth(220);
                bool addP = ImGui::InputTextWithHint(
                    "##newperf", "new performer name", manageNewPerformer,
                    sizeof(manageNewPerformer),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Add") || addP) && manageNewPerformer[0]) {
                    ManagePerformer mp;
                    mp.id = 0;
                    mp.name = manageNewPerformer;
                    std::snprintf(mp.nameBuf, sizeof(mp.nameBuf), "%s",
                                  manageNewPerformer);
                    managePerformers.push_back(mp);
                    manageNewPerformer[0] = '\0';
                }
                ImGui::TextDisabled(
                    "Press Picture to load a file, or type @username and press "
                    "Enter to fetch. The last action sets the picture.");

                // Reserve space for the Save/Cancel row so the list never
                // pushes them out of the fixed-size window.
                float footer = ImGui::GetFrameHeightWithSpacing() +
                               ImGui::GetStyle().ItemSpacing.y +
                               ImGui::GetStyle().FramePadding.y * 2.0f;
                ImGui::BeginChild("perflist", ImVec2(0, -footer), true);
                for (size_t i = 0; i < managePerformers.size(); i++) {
                    ManagePerformer& mp = managePerformers[i];
                    if (mp.deleted) continue;
                    ImGui::PushID((int)i);

                    // Thumbnail: the live saved avatar (updated the moment a
                    // Picture is loaded or a @username is fetched).
                    unsigned tex = avatarTexFor(mp.avatarPath);
                    const float sz = 44.0f;
                    ImVec2 cur = ImGui::GetCursorScreenPos();
                    if (tex)
                        ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
                    else {
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(cur.x + sz * 0.5f, cur.y + sz * 0.5f),
                            sz * 0.5f, IM_COL32(70, 70, 82, 255));
                        ImGui::Dummy(ImVec2(sz, sz));
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    // Name + @username on the first line. Pressing Enter in the
                    // username field fetches the picture immediately.
                    ImGui::SetNextItemWidth(150);
                    ImGui::InputTextWithHint("##name", "name", mp.nameBuf,
                                             sizeof(mp.nameBuf));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150);
                    bool userEnter = ImGui::InputTextWithHint(
                        "##user", "@username  (Enter to fetch)", mp.usernameBuf,
                        sizeof(mp.usernameBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    if (userEnter && mp.usernameBuf[0]) {
                        std::string user = mp.usernameBuf;
                        if (!user.empty() && user[0] == '@') user.erase(0, 1);
                        if (mp.id) store.setPerformerTiktokUser(mp.id, user);
                        applyPerformerPicture(mp, AvatarFetcher::Source::TikTok,
                                              user);
                    }
                    // Picture (file dialog) — loads & applies immediately.
                    if (ImGui::SmallButton("Picture")) {
                        std::string f = evo::openImageFileDialog(
                            "Picture for " + std::string(mp.nameBuf));
                        if (!f.empty())
                            applyPerformerPicture(
                                mp, AvatarFetcher::Source::File, f);
                    }
                    ImGui::SameLine();
                    // Face landmark status + delete (applies immediately).
                    if (mp.hasFace) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f),
                                           "* face");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Delete landmark")) {
                            if (mp.id) store.resetPerformerFace(mp.id);
#ifdef DEARTT_FACE_RECOGNITION
                            faceTracker.resetPerson(mp.nameBuf);
#endif
                            mp.hasFace = false;
                        }
                        ImGui::SameLine();
                    } else {
                        ImGui::TextDisabled("no face");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("Delete")) mp.deleted = true;
                    ImGui::EndGroup();
                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (managePerformers.empty())
                    ImGui::TextDisabled("no performers yet — add one above");
                ImGui::EndChild();

                 ImGui::Separator();
                if (ImGui::Button("Save")) {
                    saveManageAccount(evoAccountId);
#ifdef DEARTT_FACE_RECOGNITION
                    // Keep the face-tracker roster in sync with performers.
                    for (const auto& p : store.performers(evoAccountId))
                        faceTracker.addPerson(p.name);
#endif
                    showManageAccount = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    showManageAccount = false;
                    ImGui::CloseCurrentPopup();
                }
                if (!manageFetcher.status().empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", manageFetcher.status().c_str());
                }
                ImGui::EndPopup();
            }
        }

        // --- EvoControl master console (§1/§14/§16) -------------------------
        // Accounts, scenes, Start/Stop/New Shift/Load, the big total-coins
        // headline, the live gift monitor, and Detect Faces. This is the
        // operator's authoritative control surface; nothing here is streamed.
        if (showEvoConsole) {
            ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("EvoControl", &showEvoConsole)) {
                // Big total-coins headline for the current shift (§16.1),
                // including unattributed gifts.
                int64_t coins = gameEngine.shiftCoins();
                ImGui::TextDisabled("SHIFT COINS");
                ImGui::PushFont(ImGui::GetFont());
                float big = ImGui::GetFontSize() * 2.6f;
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "%lld", (long long)coins);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddText(
                        ImGui::GetFont(), big, p,
                        IM_COL32(255, 210, 90, 255), buf);
                    ImGui::Dummy(ImVec2(0, big + 4));
                }
                ImGui::PopFont();
                ImGui::Separator();

                // Account picker (accounts are the evolved profiles, §13).
                static char newAccName[128] = "";
                static char newAccStream[128] = "";
                auto accounts = store.accounts();
                std::string accPreview = "(select account)";
                for (const auto& a : accounts)
                    if (a.id == evoAccountId) accPreview = a.name;
                ImGui::SetNextItemWidth(180);
                if (ImGui::BeginCombo("account", accPreview.c_str())) {
                    for (const auto& a : accounts) {
                        bool sel = a.id == evoAccountId;
                        if (ImGui::Selectable(a.name.c_str(), sel)) {
                            evoAccountId = a.id;
                            evoSelectedScene = 0;
                            gameEngine.postLoadAccount(a.id);
#ifdef DEARTT_FACE_RECOGNITION
                            // Load this account's roster into the tracker too.
#endif
                            // Connect the ttlive client to the account stream.
                            if (!a.stream.empty()) {
                                chat.clear();
                                videoTex.destroy();
                                playerStarted = false;
                                session.start(a.stream);
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("New account"))
                    ImGui::OpenPopup("NewAccount");
                if (ImGui::BeginPopup("NewAccount")) {
                    ImGui::InputTextWithHint("name", "e.g. FridayShow",
                                             newAccName, sizeof(newAccName));
                    ImGui::InputTextWithHint("stream", "@username", newAccStream,
                                             sizeof(newAccStream));
                    if (ImGui::Button("Create") && newAccName[0]) {
                        std::string s = newAccStream;
                        if (!s.empty() && s[0] == '@') s.erase(0, 1);
                        auto a = store.createAccount(newAccName, s);
                        if (a) evoAccountId = a->id;
                        newAccName[0] = newAccStream[0] = '\0';
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                if (evoAccountId) {
                    // Shift controls.
                    ImGui::SeparatorText("Shift");
                    if (ImGui::Button("New Shift")) {
                        gameEngine.postNewShift();  // resets displays, keeps
                                                    // history (§13)
                    }
                    ImGui::SameLine();
                    if (auto sh = store.currentShift(evoAccountId)) {
                        if (ImGui::Button("Export report")) {
                            std::string js = store.shiftReportJson(sh->id);
                            std::string dir =
                                (std::filesystem::path(resourceRootDir()) /
                                 "reports")
                                    .string();
                            std::error_code ec;
                            std::filesystem::create_directories(dir, ec);
                            std::string path =
                                dir + "/shift-" + std::to_string(sh->id) +
                                ".json";
                            std::ofstream(path) << js;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("shift #%lld", (long long)sh->id);
                    }

                    // Scenes: pick, load (=new run), new scene, Start/Stop.
                    ImGui::SeparatorText("Scene");
                    static char newSceneName[128] = "";
                    auto scenes = store.scenes(evoAccountId);
                    std::string scPreview = "(select scene)";
                    for (const auto& s : scenes)
                        if (s.id == evoSelectedScene) scPreview = s.name;
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::BeginCombo("scene", scPreview.c_str())) {
                        for (const auto& s : scenes) {
                            bool sel = s.id == evoSelectedScene;
                            if (ImGui::Selectable(s.name.c_str(), sel))
                                evoSelectedScene = s.id;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("New scene"))
                        ImGui::OpenPopup("NewScene");
                    if (ImGui::BeginPopup("NewScene")) {
                        ImGui::InputTextWithHint("##sc", "scene name",
                                                 newSceneName,
                                                 sizeof(newSceneName));
                        if (ImGui::Button("Create") && newSceneName[0]) {
                            auto s = store.createScene(evoAccountId,
                                                       newSceneName);
                            if (s) {
                                evoSelectedScene = s->id;
                                // Force the Control view into the editor and
                                // OBS blank (§14): load the empty scene.
                                gameEngine.postLoadScene(s->id, false);
                            }
                            newSceneName[0] = '\0';
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }

                    ImGui::BeginDisabled(evoSelectedScene == 0);
                    if (ImGui::Button("Load scene"))
                        gameEngine.postLoadScene(evoSelectedScene, false);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    bool run = gameEngine.running();
                    ImGui::BeginDisabled(gameEngine.activeSceneRunId() == 0);
                    if (!run) {
                        if (ImGui::Button("Start"))  // begins a round (§14)
                            gameEngine.postStart();
                    } else {
                        if (ImGui::Button("Stop")) gameEngine.postStop();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
#ifdef DEARTT_FACE_RECOGNITION
                    if (ImGui::Button("Detect Faces")) {
                        // Face positions already stream into the engine; this
                        // button is a hint to widgets — broadcast a shared
                        // "faces" nudge so control.js can trigger slot mapping.
                        eventServer.broadcastEnvelope(
                            "control", 0, "shared",
                            "{\"topic\":\"detect-faces\"}");
                    }
#endif
                }

                // Live gift monitor table (§16.2).
                ImGui::SeparatorText("Gift monitor");
                if (ImGui::BeginTable("giftmon", 6,
                                      ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_BordersInnerH,
                                      ImVec2(0, 220))) {
                    ImGui::TableSetupColumn("@user");
                    ImGui::TableSetupColumn("gift");
                    ImGui::TableSetupColumn("x",
                                            ImGuiTableColumnFlags_WidthFixed,
                                            40);
                    ImGui::TableSetupColumn("dia",
                                            ImGuiTableColumnFlags_WidthFixed,
                                            60);
                    ImGui::TableSetupColumn("-> performer");
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                            1);
                    ImGui::TableHeadersRow();
                    for (const auto& r : gameEngine.giftMonitor()) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.gifterName.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r.giftName.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("x%lld", (long long)r.amount);
                        ImGui::TableNextColumn();
                        ImGui::Text("%lld", (long long)r.diamonds);
                        ImGui::TableNextColumn();
                        if (r.performerId)
                            ImGui::TextUnformatted(r.performerName.c_str());
                        else
                            ImGui::TextDisabled("- (show)");
                        ImGui::TableNextColumn();
                    }
                    ImGui::EndTable();
                }

                ImGui::TextDisabled(
                    "OBS overlay:  http://localhost:%d/obs", eventServer.port());
                ImGui::TextDisabled(
                    "Control view: http://localhost:%d/control",
                    eventServer.port());
            }
            ImGui::End();
        }

        // --- profile manager: stream + roster (add / delete / reset) --------
        if (showProfileMgr && profileLoaded) {
            ImGui::SetNextWindowSize(ImVec2(380, 440), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Manage profile", &showProfileMgr)) {
                ImGui::Text("Profile: %s", curProfile.name.c_str());

                // Editable stream.
                static char streamBuf[128];
                std::snprintf(streamBuf, sizeof(streamBuf), "%s",
                              curProfile.stream.c_str());
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputTextWithHint(
                        "##stream", "@username", streamBuf, sizeof(streamBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    profiles::setStream(curProfile.name, streamBuf);
                    profiles::load(curProfile.name, curProfile);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(stream)");
                ImGui::Separator();

                // Add a person to the roster.
                ImGui::SetNextItemWidth(200);
                bool addP = ImGui::InputTextWithHint(
                    "##person", "new person name", newPersonName,
                    sizeof(newPersonName), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Add") || addP) && newPersonName[0]) {
#ifdef DEARTT_FACE_RECOGNITION
                    faceTracker.addPerson(newPersonName);
#endif
                    newPersonName[0] = '\0';
                }
                ImGui::TextDisabled(
                    "Click a face in the video to name it. * = face captured.");

                // Roster list with per-person Reset / Delete.
                ImGui::BeginChild("people", ImVec2(0, 0), true);
#ifdef DEARTT_FACE_RECOGNITION
                auto roster = faceTracker.roster();
                bool openAvatarPopup = false;  // deferred: OpenPopup must run
                                               // outside the per-person PushID
                for (const auto& p : roster) {
                    ImGui::PushID(p.name.c_str());
                    // Display picture thumbnail (or placeholder).
                    unsigned tex = avatarTex.get(p.name, p.avatar);
                    ImVec2 cur = ImGui::GetCursorScreenPos();
                    if (tex)
                        ImGui::Image((ImTextureID)(intptr_t)tex,
                                     ImVec2(34, 34));
                    else {
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(cur.x + 17, cur.y + 17), 17,
                            IM_COL32(70, 70, 82, 255));
                        ImGui::Dummy(ImVec2(34, 34));
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::TextColored(p.hasFace
                                           ? ImVec4(0.5f, 1.0f, 0.6f, 1.0f)
                                           : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "%s%s", p.name.c_str(),
                                       p.hasFace ? "  * face" : "");
                    if (ImGui::SmallButton("Picture")) {
                        avatarPicTarget = p.name;
                        avatarTT[0] = avatarURL[0] = '\0';
                        openAvatarPopup = true;  // opened after PopID below
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset landmark"))
                        faceTracker.resetPerson(p.name);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete"))
                        faceTracker.removePerson(p.name);
                    ImGui::EndGroup();
                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (roster.empty())
                    ImGui::TextDisabled("no people yet — add one above");

                // Open here (same ID scope as BeginPopup, i.e. no PushID).
                if (openAvatarPopup) ImGui::OpenPopup("AvatarSrc");

                // Shared "set picture" popup (3 sources; all save locally).
                if (ImGui::BeginPopup("AvatarSrc")) {
                    ImGui::Text("Picture for %s", avatarPicTarget.c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled("Drag an image onto the window,");
                    ImGui::TextDisabled("or use one of these:");
                    ImGui::SetNextItemWidth(180);
                    bool ttGo = ImGui::InputTextWithHint(
                        "##tt", "@tiktok username", avatarTT, sizeof(avatarTT),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Fetch##tt") || ttGo) && avatarTT[0]) {
                        avatarFetcher.request(curProfile.dir, avatarPicTarget,
                                              AvatarFetcher::Source::TikTok,
                                              avatarTT);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetNextItemWidth(180);
                    bool urlGo = ImGui::InputTextWithHint(
                        "##url", "https://image.url", avatarURL,
                        sizeof(avatarURL),
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Fetch##url") || urlGo) && avatarURL[0]) {
                        avatarFetcher.request(curProfile.dir, avatarPicTarget,
                                              AvatarFetcher::Source::Url,
                                              avatarURL);
                        ImGui::CloseCurrentPopup();
                    }
                    // While this popup is open, dropped images go to this
                    // person (drained after the frame).
                    avatarDropTarget = avatarPicTarget;
                    ImGui::EndPopup();
                }

                if (!avatarFetcher.status().empty())
                    ImGui::TextDisabled("%s", avatarFetcher.status().c_str());
#else
                ImGui::TextDisabled("face recognition not built in");
#endif
                ImGui::EndChild();
            }
            ImGui::End();
        }
        // Dropped files: install .evw widget bundles (§8.1); route images to
        // the avatar picker if it's open.
        if (!g_droppedFiles.empty()) {
            std::vector<std::string> remaining;
            for (const auto& f : g_droppedFiles) {
                if (f.size() > 4 &&
                    f.compare(f.size() - 4, 4, ".evw") == 0) {
                    std::string err;
                    auto m = widgetRegistry.installFromFile(f, err);
                    if (!m)
                        std::fprintf(stderr, "[evo] install %s failed: %s\n",
                                     f.c_str(), err.c_str());
                } else {
                    remaining.push_back(f);
                }
            }
            g_droppedFiles.swap(remaining);
        }
#ifdef DEARTT_FACE_RECOGNITION
        // Route dropped image files to the person whose picture popup is open.
        if (!g_droppedFiles.empty()) {
            if (!avatarDropTarget.empty() && profileLoaded) {
                for (const auto& f : g_droppedFiles)
                    avatarFetcher.request(curProfile.dir, avatarDropTarget,
                                          AvatarFetcher::Source::File, f);
            }
            g_droppedFiles.clear();
        }
#endif
        // Any non-image drops we didn't consume are dropped.
        g_droppedFiles.clear();

#ifdef DEARTT_STT
        // First-run STT model download: floating progress overlay
        // (bottom-center, on top of everything).
        if (sttDownload.active()) {
            const ImGuiViewport* v = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(
                ImVec2(v->WorkPos.x + v->WorkSize.x * 0.5f,
                       v->WorkPos.y + v->WorkSize.y - 16.0f),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::Begin("##sttdownload", nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav);
            ImGui::TextUnformatted("Downloading STT models");
            uint64_t got = sttDownload.downloaded();
            uint64_t tot = sttDownload.total();
            char overlay[64];
            if (tot > 0) {
                std::snprintf(overlay, sizeof(overlay), "%.2f / %.2f GB",
                              got / 1e9, tot / 1e9);
                ImGui::ProgressBar((float)((double)got / (double)tot),
                                   ImVec2(340, 0), overlay);
            } else {
                std::snprintf(overlay, sizeof(overlay), "%.2f GB", got / 1e9);
                // Total unknown yet: indeterminate animation.
                ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(),
                                   ImVec2(340, 0), overlay);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) sttDownload.cancel();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Stop the download. The partial file is kept and the\n"
                    "download resumes on the next launch.");
            ImGui::End();
        }
#endif

        // --- render ---------------------------------------------------------
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

#ifdef DEARTT_FACE_RECOGNITION
    faceTracker.stop();
    avatarTex.shutdown();
#endif
    gameEngine.stop();
    store.close();
    session.stop();
    player.close();   // joins the decode thread -> no more audio-tap calls
#ifdef DEARTT_STT
    stt.stop();
#endif
    videoTex.destroy();
    giftIcons.shutdown();
    avatars.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
