# OpenAVX for OBS Studio (`openavx-obs`)

OBS Studio plugin integrating the [OpenAVX](https://github.com/saifmukhtar/openavx) (`libopenavx`) IP audio/video transport stack into OBS Studio as a receiver source (`openavx_source`), dual program/preview outputs (`openavx_output` / `openavx_video_output`), and per-source ISO output filters (`openavx_filter` / `openavx_audio_filter`).

---

## Features

### OpenAVX Source (`openavx_source`)

* **Sectioned Properties UI**: Organized via a top **Settings Section** switcher (`General`, `Video & Network`, `Failover`, `PTZ & Remote Recording`, `Sync Cloud Tally Light`, or `Show All Sections`) with hover tooltips on technical options.
* **Non-blocking Discovery & Connection**: Shared background mDNS watcher updates the stream list (`Refresh Stream List`) without blocking the OBS UI thread; supports stream names, `Discovery Groups`, direct `ip:port` endpoints, and `openavx://` relay URLs.
* **Honest Status & Secret Redaction**: Reports `Waiting for media...` until frames arrive, `Connected (<W>x<H>, <Mbps>, RTT: <ms>)` when video is flowing, `Connected (Audio Only, ...)` in audio-only mode, and masks `key=...` query secrets in status text.
* **Sync & Visibility Lifecycle**:
  * **Lowest Latency**: Anchors incoming frame PTS to `os_gettime_ns()`.
  * **Synchronised (`FrameSync`)**: Uses `oavx_framesync` paced to OBS's canvas framerate and audio sample rate.
  * Configurable **Bandwidth** (`Highest`, `Lowest`, `Audio Only`), **Inactive Behaviour** (`Keep connection alive` / `Disconnect when not visible`), and `Clear video frame on disconnect`.
* **Video, HDR & Audio**:
  * Supports `UYVY`, `UYVA` (converted to `BGRA` with alpha), `BGRA`, `BGRX`, `RGBA`, `RGBX`, `NV12`, `I420`, `YV12`, `P216`, SpeedHQ, and compressed H.264 / HEVC / AV1 streams.
  * Parses `<oavx_color_info>` HDR metadata for `BT.2100 PQ` and `BT.2100 HLG` streams via `obs_source_output_video2`.
  * Outputs multi-channel `FLTP` audio and synchronizes Program/Preview tally.
* **Failover & Auto-Revert**: Configurable `Failover Backup Stream`, stall timeout (`200`–`30000 ms`), and stability-window auto-revert (`500`–`60000 ms`).
* **PTZ & Remote Recording**: Pan, tilt, zoom, and focus sliders (`Send PTZ Command`), plus remote recording start/stop with optional filename hint.
* **Cloud Tally Sync**: Optional synchronization of OBS Program/Preview state to an OpenAVX Cloud Studio room (`Cloud Tally Endpoint URL`, `Studio Room Slug`, `Stream Key`, `Camera Slot Index`).
* **Transport & Encryption**: Selectable `Transport Protocol` (`TCP`, `RUDP`, `UDP`) and ChaCha20-Poly1305 encryption passphrase.

### OpenAVX Output (`openavx_output` / `openavx_video_output`) & Tools Dialog

* **3-Tab `Tools -> OpenAVX Output Settings` Dialog** (`ENABLE_FRONTEND_API=ON`, `ENABLE_QT=ON`):
  * **General**: Enable/configure **Main Program Output** (`obs_get_video` / `obs_get_audio`) and **Preview Output** (`obs_view` tracking Studio Mode preview scenes, with optional audio), color-coded live status (`Active`, `Stopped`, `Error`, bytes, frames, dropped, connected clients, congestion, `[PGM]` / `[PVW]` tally), and shared **Encryption Passphrase**.
  * **Encoding**: Video Compression (**AV1**, **HEVC / H.265**, **H.264** with hardware auto-detect NVENC/QSV/AMF or software fallback, bitrate `1000`–`50000 kbps`, GOP `15`–`300 frames`; **SpeedHQ** `SHQ2`/`SHQ3` quality `20`–`98`; **Uncompressed** including `P216` HDR), Colour Format, and Audio Compression (`Uncompressed`, `Adaptive`, `Always Compressed` Opus/ADPCM).
  * **Servers & Network**: Built-in **HTTP `fMP4` Web Preview** (`:8080`) and **RTSP 1.0 Server** (`:8554`) with live copyable URLs, `Main` / `Preview` Discovery Groups, `Transport Protocol` (`TCP`, `RUDP`, `UDP`), `Bind IP / Network Interface`, and `Failover Backup Target`.
* **Persistence & Lifecycle**: Saves settings in OBS user config (`[OpenAVX]` in `user.ini`), auto-starts enabled outputs after OBS finishes loading, and shuts down cleanly on exit.

### Dedicated ISO Output Filters (`openavx_filter` / `openavx_audio_filter`)

* **Dedicated OpenAVX ISO Output** (video + audio) and **OpenAVX Audio ISO Output** (audio-only) source filters for isolated multi-camera/audio feeds.
* Organized into **General**, **Encoding**, and **Network** groups with live status readout, independent stream names, discovery groups, encryption passphrases, bind IP, failover target, codec settings, and transport selection.

---

## Building

### Prerequisites

* CMake `>= 3.28`, Ninja (recommended on Linux), and a C/C++ compiler (GCC/Clang/MSVC/Xcode).
* OBS Studio development headers, `libobs`, and `obs-frontend-api` (`obs-studio` 30+), plus Qt6 (`Core` and `Widgets`) for the `Tools -> OpenAVX Output Settings` UI (pass `-DENABLE_QT=OFF -DENABLE_FRONTEND_API=OFF` for headless backend-only builds).
* `openavx` (`libopenavx`), resolved by [`CMakeLists.txt`](CMakeLists.txt) in this order:
  1. Pre-installed CMake package (`find_package(openavx CONFIG QUIET)`, e.g., via `-DCMAKE_PREFIX_PATH=...`).
  2. Local core checkout at `OPENAVX_DIR` (defaults to `../openavx`, built via `add_subdirectory`).
  3. Automatic Git `FetchContent` from `https://github.com/saifmukhtar/openavx.git` when `OPENAVX_FETCH_GIT=ON` (default `ON`, `OPENAVX_GIT_TAG=v0.2.2`).

### Linux Build & Local Plugin Install Example

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOPENAVX_DIR=../openavx
cmake --build build -j
```

To install into your user OBS Studio plugin directory on Linux:

```bash
mkdir -p ~/.config/obs-studio/plugins/openavx-obs/bin/64bit \
         ~/.config/obs-studio/plugins/openavx-obs/data/locale
cp build/openavx-obs.so ~/.config/obs-studio/plugins/openavx-obs/bin/64bit/
cp build/rundir/RelWithDebInfo/bin/64bit/libopenavx.so* ~/.config/obs-studio/plugins/openavx-obs/bin/64bit/
cp data/locale/*.ini ~/.config/obs-studio/plugins/openavx-obs/data/locale/
```

When `OPENAVX_BUNDLE_RUNTIME=ON` (default), CMake bundles `libopenavx` alongside `openavx-obs` in `rundir/<Config>` and install targets, configured with portable runtime search paths (`$ORIGIN` on Linux, `@loader_path` on macOS).

Cross-platform CMake presets are defined in [`CMakePresets.json`](CMakePresets.json) (`ubuntu-x86_64`, `windows-x64`, `macos`), and CI workflows are configured in [`.github/workflows/`](.github/workflows/).

---

## License

This project is licensed under the GNU General Public License v2.0 or later — see [`LICENSE`](LICENSE).
