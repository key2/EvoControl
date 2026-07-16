# DearTT

A cross-platform (Linux / Windows / macOS) **TikTok LIVE viewer** built with Dear
ImGui: live video playback next to real-time chat, gifts, likes and joins,
with a stats panel (viewers, diamonds/min, per-gift breakdown, top gifters)
and an embedded web server that streams every event as JSON.

```
+-------------------------------+------------------+
|                               |  chat / gifts    |
|     live video (FFmpeg)       |  (ttlive-cpp)    |
|                               +------------------+
|                               |  stats / plots   |
+-------------------------------+------------------+
```

- **Video**: FLV/HLS pull via FFmpeg (H.264/HEVC + AAC), audio through miniaudio.
- **Events**: [ttlive-cpp](https://github.com/key2/ttlive-cpp) — signed
  Webcast API + WebSocket push with a Chrome TLS fingerprint (curl-impersonate).
- **Gift accounting**: streakable gift combos are counted exactly once, when
  the combo finishes; non-streakable gifts (which never send a "combo end")
  are counted immediately.
- **Webview**: `http://localhost:8080/` serves a minimal event viewer;
  `ws://localhost:8080/ws` broadcasts 100% of events as JSON (including the
  raw decoded protobuf of every Webcast message).
- **Fonts**: bundled pan-Unicode
  [GoNotoKurrent](https://github.com/satbyy/go-noto-universal) (SIL OFL) +
  Twemoji color emoji — chat renders in any script out of the box.
- **Face recognition** (optional): ONNX Runtime + InsightFace (SCRFD detect,
  ArcFace embed) — name the people in the video; identities are smoothed over
  a configurable time window and shown as overlays.
- **Speech-to-text** (optional): live transcription of the stream audio with
  [voxtral.cpp](third_party/voxtral) (Voxtral-Mini-4B-Realtime, GGUF) on CPU
  or GPU (`-DGGML_VULKAN=ON`). The ~2.7 GB model is **not** shipped: the app
  downloads it on first run (in-app progress bar, resumable) into
  `models/voxtral/` next to the executable.

## Prebuilt releases

The [Releases page](https://github.com/key2/DearTT/releases) has ready-to-run
packages:

- **Windows x64** (`dist-win64.zip`) — self-contained, with face recognition
  and STT (Vulkan GPU + CPU fallback) built in. Unzip and run `deartt.exe`.
  Also runs under **Wine** (GPU STT needs Wine >= 10; older Wine falls back
  to CPU STT automatically).
- **Linux x64** (`dist-linux64.zip`) — links distro libraries at runtime (see
  the Linux build section for the required packages).

## Getting the source

```sh
git clone --recurse-submodules https://github.com/key2/DearTT.git
cd DearTT
# If cloned without --recurse-submodules:
git submodule update --init --recursive
```

## Building — Linux

Build dependencies (Ubuntu/Debian package names):

```sh
sudo apt install build-essential cmake git pkg-config \
    libavformat-dev libavcodec-dev libswscale-dev libswresample-dev libavutil-dev \
    libprotobuf-dev protobuf-compiler libssl-dev zlib1g-dev libfreetype-dev \
    xorg-dev
```

(`xorg-dev` provides the X11 headers GLFW needs; GLFW, Dear ImGui, ImPlot,
civetweb, miniaudio and nlohmann-json are vendored and built from source.
The first configure downloads a prebuilt curl-impersonate.)

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/deartt [@username]
```

Optional features (both auto-detected / opt-in):

- **Face recognition**: install `libonnxruntime-dev` (or pass
  `-DONNXRUNTIME_DIR=<prebuilt onnxruntime root>`); the configure log prints
  `Face recognition: ON`.
- **Speech-to-text**: on by default (`-DDEARTT_STT=ON`); ggml is fetched at
  configure time. Add `-DGGML_VULKAN=ON` for GPU transcription (needs
  Vulkan headers + `glslc` from shaderc at build time). For redistributable
  binaries also pass `-DGGML_NATIVE=OFF` — the default compiles the CPU
  backend with `-march=native`.

Package a release zip (stripped binary + js/web/fonts assets + build-info):

```sh
./scripts/package-linux.sh        # -> dist-linux64/ and dist-linux64.zip
```

The Linux package links distro libraries at runtime; the target machine
needs the runtime equivalents (Ubuntu: `libavformat61 libavcodec61
libswscale8 libswresample5 libprotobuf32 libfreetype6` — i.e. a distro with
FFmpeg 7 era libraries) plus OpenGL drivers.

## Building — macOS

Native build (Apple Silicon or Intel) using Homebrew for the system libraries.
GLFW, Dear ImGui, ImPlot, civetweb, miniaudio and nlohmann-json are vendored
and built from source; the first configure downloads a prebuilt
curl-impersonate for the host arch.

```sh
brew install cmake pkg-config ffmpeg protobuf freetype openssl@3
```

(`ffmpeg` provides avformat/avcodec/swscale/swresample/avutil; `protobuf`
pulls in abseil; `freetype` pulls in libpng for color-emoji bitmaps.)

```sh
cmake -B build-macos -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos -j
./build-macos/deartt [@username]
```

Package a release zip (stripped binary + js/web/fonts assets + build-info):

```sh
./scripts/package-macos.sh        # -> dist-macos/ and dist-macos.zip
```

Like the Linux package, the macOS package links Homebrew libraries at runtime;
the target machine needs the same formulae installed (`brew install ffmpeg
protobuf freetype openssl@3`). The packaged binary matches the build
architecture (arm64 on Apple Silicon, x86_64 on Intel).

## Building — Windows (cross-compiled from Linux)

The Windows build is produced on Linux with MinGW-w64:

```sh
sudo apt install mingw-w64 nasm zip
```

1. **Cross-build the dependencies** (one-time; cached in `win64-deps/`):

   ```sh
   ./scripts/build-win64-deps.sh
   ```

   This produces a **minimal FFmpeg 7.1** (LGPL, ~5.5 MB of DLLs instead of
   the ~140 MB full builds: only FLV/HLS/MPEG-TS/fMP4 demuxers,
   H.264/HEVC/AAC/MP3 decoders, webp/jpeg/png/gif image decoding for gift
   icons and avatars, and https via Windows schannel), a curl-impersonate DLL
   import lib, static OpenSSL + protobuf, and the **Vulkan headers + loader
   import lib** (prebuilt MSYS2 packages) for GPU STT. Downloads are cached
   in `win64-deps/dl/`.

2. **Build the app** — full-featured (face recognition + STT with Vulkan),
   exactly what the published release uses:

   ```sh
   cmake -B build-win64 -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
     -DCMAKE_BUILD_TYPE=Release \
     -DFFMPEG_DIR=$PWD/win64-deps/ffmpeg \
     -DCURL_IMPERSONATE_LOCAL_DIR=$PWD/win64-deps/curl-impersonate \
     -DONNXRUNTIME_DIR=$PWD/win64-deps/onnxruntime \
     -DDEARTT_STT=ON -DGGML_VULKAN=ON -DGGML_NATIVE=OFF
   cmake --build build-win64 -j
   ```

   STT defaults OFF in the cross-build; `-DDEARTT_STT=ON` opts in (ggml is
   fetched at configure time and its Vulkan shader generator is compiled with
   the host compiler automatically; `glslc` must be on the host PATH —
   Ubuntu: `apt install glslc`). Watch the configure log for
   `Speech-to-text: ON` and `Face recognition: ON`.

3. **Package** (strips every binary, verifies all DLL imports are shipped,
   writes `build-info.txt` with per-file sha256, zips):

   ```sh
   ./scripts/package-win64.sh       # -> dist-win64/ and dist-win64.zip
   ```

The result is self-contained: unzip anywhere on a Windows x86_64 machine and
run `deartt.exe`. If a "DLL not found" error ever appears, compare
`build-info.txt` in the extracted folder with the one in the zip — it means
an old extracted copy is being launched.

### Running the Windows build under Wine

The packaged exe runs under Wine, GPU STT included: **Wine >= 10** runs the
ggml Vulkan backend (verified real-time transcription through winevulkan);
on Wine <= 9 the app detects the version and stays on CPU STT (winevulkan
there can't create the compute device). The app also pre-initializes Vulkan
before the OpenGL context exists — required under Wine, where a late host
Vulkan init after a GLX context is live fails inside winevulkan. Face
recognition (ONNX Runtime) works under Wine out of the box.

## Running

```sh
./build/deartt              # enter a @username in the UI
./build/deartt @someuser    # connect immediately
```

| Environment variable | Effect |
|---|---|
| `DEARTT_PORT` | Webview/WebSocket port (default 8080) |
| `DEARTT_COOKIES` | Cookies for the TikTok session, e.g. `sessionid=...` — needed for age/region-restricted rooms |
| `GLFW_PLATFORM` | `x11` or `wayland` to force the Linux backend |
| `DEARTT_VOXTRAL_MODEL` | Path to a Voxtral `.gguf` (overrides the `models/voxtral/` scan) |
| `DEARTT_VOXTRAL_URL` | Alternate URL for the first-run model download |
| `DEARTT_NO_MODEL_DOWNLOAD` | Set to disable the automatic first-run model download |
| `DEARTT_STT_GPU` | STT compute: `auto`/`vulkan` force a GPU attempt (even under old Wine), `none`/`cpu` force CPU (e.g. weak iGPUs where the 2.7 GB weight upload takes minutes) |

The active STT backend is shown in the video stats overlay
(`... | stt Vulkan` / `... | stt CPU`) and logged at startup.

The webview at `http://localhost:8080/` shows every event live; the
`/ws` WebSocket delivers them as JSON (type, user, gift fields incl.
`gift_type`/`gift_streaking`, plus the fully decoded raw protobuf message)
for external tooling.

## Project layout

```
src/
  main.cpp            UI: layout, fonts, stats panel, plots
  live_session.cpp    ttlive client thread -> chat feed + event sink
  stats.cpp           StatsCollector: totals, rates, per-gift/per-gifter
  video_player.cpp    FFmpeg pull + decode -> RGBA frames + audio
  audio_output.cpp    miniaudio playback
  event_server.cpp    civetweb HTTP + WebSocket server (:8080)
  event_json.cpp      Event -> JSON (incl. reflective protobuf decode)
  icon_cache.cpp      Gift icon / avatar fetch + decode + GL textures
  face_*.cpp          Face detect (SCRFD) / embed (ArcFace) / gallery / track
  stt.cpp             Live speech-to-text worker (voxtral.cpp)
  model_download.cpp  First-run Voxtral model download (streaming, resumable)
web/index.html        The event-viewer website
fonts/                GoNotoKurrent (OFL) + Twemoji (CC-BY)
models/               Face detection/recognition ONNX models (committed);
                      models/voxtral/ is filled by the first-run download
imgui/, third_party/  Vendored: imgui, implot, glfw, freetype, civetweb,
                      miniaudio, nlohmann-json, voxtral.cpp
ttlive-cpp/           TikTok LIVE client library (submodule)
cmake/                MinGW-w64 toolchain file
scripts/              Windows dep builder + Linux/Windows/macOS packagers
```

## Notes

- ImGui has no text shaping/bidi: Arabic renders as unjoined letterforms in
  logical order (the webview shapes it correctly in the browser).
- This project is for interoperability/research. Respect TikTok's Terms of
  Service and applicable laws.
