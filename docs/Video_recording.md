# Sunshine Frame Capture Feature

This document describes the changes that add a frame capture feature to Sunshine's encoding pipeline. It saves frames just before they are passed to the encoder, enabling quantitative evaluation of streaming quality.

## Overview

The frame capture feature captures frames (`device->frame`) inside Sunshine's `encode_run()` loop — after `session->convert()` and before `encode()` — and writes them to a file.

## Output Location

Recordings are saved to the directory specified in the config file (default: `recorded_session/`).
Default values when no config is explicitly set:
- `capture_frames = enabled`
- `capture_format = ffv1` (fallback when `capture_format` is unset or invalid)

Each recording session generates the following files:

| Mode | Generated Files |
|------|----------------|
| `ffv1` | `sunshine_capture_YYYYMMDD_HHMMSS.mkv` + `.meta` + `.frames.csv` |
| `mp4` | `sunshine_capture_YYYYMMDD_HHMMSS.mp4` + `.meta` + `.frames.csv` |
| `raw` | `sunshine_capture_YYYYMMDD_HHMMSS.yuv` + `.meta` + `.frames.csv` |

The `.meta` file records resolution, fps, pixel format, and example conversion commands.

The `.frames.csv` file records per-frame correspondence information:

```csv
local_frame_idx,frame_nr,is_idr
0,1,1
1,2,0
2,3,0
3,4,0
...
```

| Column | Description |
|--------|-------------|
| `local_frame_idx` | Zero-based sequential index within the recording file |
| `frame_nr` | `frame_nr` from Sunshine's `encode_run()` (sequential number passed to the encoder) |
| `is_idr` | 1 if IDR frame, 0 otherwise (derived from `AV_FRAME_FLAG_KEY` on the `AVFrame`) |

## Output Modes

| Mode | Codec | Lossless | Size Estimate (1080p60) | Use Case |
|------|-------|----------|------------------------|----------|
| `ffv1` | FFV1 (.mkv) | ✅ | 35–60 MB/s | Relatively compact data |
| `mp4` | H.264 crf 18 (.mp4) | ❌ | 2–10 MB/s | Smallest data |
| `raw` | Uncompressed (.yuv) | ✅ | ~178 MB/s | No ffmpeg required, minimal frame drops |

> **Note:**
> `ffv1` or `raw` is recommended as a reference for VMAF/PSNR calculation.
> Even when the input is NV12/P010, the saved pixel format is always YUV420P.

## Files Added/Modified

### New Files

#### 1. `src/video_recorder.h`

Header file for the VideoRecorder class.

```cpp
namespace video {

  enum class capture_format_e {
    ffv1,  // FFV1 lossless in MKV container
    mp4,   // H.264 lossy in MP4 container
    raw    // Raw frames, no encoding
  };

  class VideoRecorder {
  public:
    VideoRecorder();
    ~VideoRecorder();

    bool initialize(const std::string &output_dir, int width, int height, int fps,
                    capture_format_e format);
    bool write_frame(AVFrame *frame, int64_t frame_nr,
                     std::optional<steady_clock::time_point> frame_timestamp);
    void finalize();
    bool is_recording() const;
    std::string get_output_path() const;
    int64_t frames_written() const;

  private:
    bool is_supported_format(AVPixelFormat fmt);
    bool start_ffmpeg_pipe(AVPixelFormat fmt);
    bool start_raw_file(AVPixelFormat fmt);
    bool write_planes_to_pipe(AVFrame *sw_frame);
    bool write_planes_to_file(AVFrame *sw_frame);
    void write_meta_file(AVPixelFormat fmt);
    bool validate_path(const std::string &path);
    bool check_ffmpeg_available();
    void abort_recording(const std::string &reason);

    std::atomic<bool> m_recording {false};
    capture_format_e m_format = capture_format_e::ffv1;
    std::string m_output_dir;
    std::string m_output_path;
    FILE *m_ffmpeg_pipe = nullptr;
    std::ofstream m_raw_file;
    std::ofstream m_frame_log;  // CSV frame log
    int m_width = 0, m_height = 0, m_fps = 0;
    int64_t m_frames_written = 0;
    AVPixelFormat m_pix_fmt = AV_PIX_FMT_NONE;
    bool m_output_started = false;
    std::mutex m_mutex;
  };

  capture_format_e capture_format_from_string(const std::string &str);

}  // namespace video
```

Key features:

- Thread-safe via `std::mutex`
- Automatic GPU→CPU transfer for HW frames (VAAPI / NVENC, etc.) using `av_hwframe_transfer_data()`
- Supported input pixel formats: NV12 / YUV420P / P010
- Automatic stop on unsupported format detection
- FFmpeg pipe for `ffv1`/`mp4` modes, direct file write for `raw` mode

#### 2. `src/video_recorder.cpp`

Implementation of the VideoRecorder class.

Key functionality:

- **HW frame handling**: Detects `frame->hw_frames_ctx` and transfers from GPU to CPU memory via `av_hwframe_transfer_data()`
- **Automatic pixel format detection**: Determines sw_format from the first frame and starts output
- **FFmpeg pipe output** (`ffv1`/`mp4`): Opens a pipe via `popen()` and sends rawvideo to FFmpeg
- **Raw file output** (`raw`): Writes Y/U/V planes sequentially
- **Automatic stop on error**: Safely stops recording via `abort_recording()` on pipe breakage or disk full
- **Metadata file**: Records resolution, fps, format, and example conversion commands in a `.meta` file
- **CSV frame log**: Appends one line to `.frames.csv` after each successful frame write. Can be used for frame alignment during VMAF calculation by matching against the Moonlight-side CSV

### Modified Files

#### 3. `src/video.h`

Changes:

- Added include for `video_recorder.h`
- Added extern declaration for `frame_recorder` and helper functions

```diff
 #include <libswscale/swscale.h>
+#include "video_recorder.h"

 namespace video {
```

```diff
+  extern std::unique_ptr<VideoRecorder> frame_recorder;
+
+  void init_frame_recorder(int width, int height, int fps);
+  void stop_frame_recorder();

 }  // namespace video
```

#### 4. `src/video.cpp`

Changes:

- Added global variable `frame_recorder` and helper functions

**Global variable and helper functions:**

```cpp
std::unique_ptr<VideoRecorder> frame_recorder;

void init_frame_recorder(int width, int height, int fps) {
  if (!config::video.capture_frames) {
    return;
  }

  frame_recorder = std::make_unique<VideoRecorder>();

  std::string output_dir = config::video.capture_output_dir;
  if (output_dir.empty()) {
    output_dir = "recorded_session";
  }

  auto format = capture_format_from_string(config::video.capture_format);

  if (!frame_recorder->initialize(output_dir, width, height, fps, format)) {
    frame_recorder.reset();
  }
}

void stop_frame_recorder() {
  if (frame_recorder) {
    frame_recorder->finalize();
    frame_recorder.reset();
  }
}
```

**Frame capture inserted in `encode_run()`** — after `session->convert()`, before `encode()`:

```diff
      if (session->convert(*img)) {
        BOOST_LOG(error) << "Could not convert image"sv;
        return;
      }

+     // Record frame if enabled.
+     // write_frame() will auto-disable recording on failure.
+     if (frame_recorder && frame_recorder->is_recording()) {
+       if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(session.get())) {
+         frame_recorder->write_frame(avcodec_session->device->frame,
+                                     frame_nr,          // before ++ increment
+                                     frame_timestamp);  // nullopt for duplicates
+       }
+     }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp)) {
```

**Initialization and cleanup in `encode_run()`:**

```diff
    auto session = make_encode_session(...);
    if (!session) { return; }

+   init_frame_recorder(config.width, config.height, config.framerate);

    auto fail_guard = util::fail_guard([&encoder, &session] {
+     stop_frame_recorder();
      if (encoder.flags & ASYNC_TEARDOWN) {
```

#### 5. `src/config.h`

Changes: Added capture config fields to `video_t` struct (`capture_frames` defaults to `true`)

```diff
    int max_bitrate;
    double minimum_fps_target;
+
+   bool capture_frames = true;
+   std::string capture_output_dir;
+   std::string capture_format;   // "ffv1", "mp4", "raw"
  };
```

#### 6. `src/config.cpp`

Changes: Added config parsing

```diff
    double_between_f(vars, "minimum_fps_target", video.minimum_fps_target, {0.0, 1000.0});

+   bool_f(vars, "capture_frames", video.capture_frames);
+   string_f(vars, "capture_output_dir", video.capture_output_dir);
+   string_f(vars, "capture_format", video.capture_format);

    path_f(vars, "pkey", nvhttp.pkey);
```

#### 7. `cmake/compile_definitions/common.cmake`

Changes: Registered new source files in the build system

```diff
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.cpp"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.h"
+       "${CMAKE_SOURCE_DIR}/src/video_recorder.cpp"
+       "${CMAKE_SOURCE_DIR}/src/video_recorder.h"
        "${CMAKE_SOURCE_DIR}/src/input.cpp"
```

## Configuration

Settings can be overridden in `sunshine.conf` (default: `~/.config/sunshine/sunshine.conf`).
Defaults when unset: `capture_frames=enabled`, `capture_format=ffv1`, `capture_output_dir=recorded_session`.
You can also create your own config directory and point to it.
Example: `XDG_CONFIG_HOME=~/config-yoshi ./build-yoshi/sunshine`

```ini
capture_frames = enabled            # enabled by default
capture_output_dir = recorded_session
capture_format = raw                # example: switch to raw mode
```

## Converting Raw YUV to MP4

After recording, convert using the parameters listed in the `.meta` file:

```bash
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1920x1080 -r 60 \
    -i "recorded_session/sunshine_capture_YYYYMMDD_HHMMSS.yuv" \
    -c:v libx264 -pix_fmt yuv420p -crf 18 \
    "output.mp4"
```

Parameters (check the `.meta` file for exact values):

- `-pix_fmt` — Source pixel format (`raw`/`ffv1`/`mp4` all save as YUV420P)
- `-s WIDTHxHEIGHT` — Video resolution
- `-r FPS` — Frame rate
- `-crf 18` — Quality (lower = better; 18 is high quality)

## Storage Requirements

`raw` mode writes uncompressed frames, requiring large amounts of storage:

| Resolution | Size/sec | Size/min |
|------------|----------|----------|
| 720p60 | ~80 MB/s | ~4.8 GB/min |
| 1080p60 | ~178 MB/s | ~10.7 GB/min |
| 4K60 | ~720 MB/s | ~43 GB/min |

`ffv1` mode compresses to 35–60 MB/s at 1080p60; `mp4` mode compresses to 2–10 MB/s.

## Building

After making changes, rebuild using the standard Sunshine build procedure:

```bash
cd ~/sunshine
rm -rf build
cmake -B build -G Ninja -S . \
  -DCMAKE_INSTALL_PREFIX=$PWD/build \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF
ninja -C build
```

## How to use

After building, run the following command.
Create a separate configuration directory (e.g., config-yoshi) to avoid conflicts with other Sunshine instances.
The directory name does not have to be config-yoshi; it is used here as an example.

```bash
mkdir config-yoshi
XDG_CONFIG_HOME=~/config-yoshi ./build-yoshi/sunshine
```



No additional external dependencies are required. `ffv1`/`mp4` modes require `ffmpeg` to be installed on the host (`raw` mode does not).

## Disabling Recording

Set the following in `sunshine.conf`:

```ini
capture_frames = disabled
```

## Assumptions and Limitations

- **Runs synchronously on the encode loop.** Recording cost adds to streaming latency. `raw` mode is recommended (additional cost: 1–2.5 ms). `ffv1` mode may cause 5–15 ms of blocking.
- **Only avcodec-based sessions are supported.** Requires sessions where `device->frame` is accessible as an AVFrame. Sessions that do not directly expose an AVFrame (e.g., `nvenc_encode_session_t`) will have recording skipped.
- **Supported input pixel formats:** NV12 / YUV420P / P010 only. Recording is automatically stopped if any other format is detected.
- **Output pixel format:** Even when the input is NV12/P010, all modes (`raw`/`ffv1`/`mp4`) convert and save as YUV420P (8-bit). P010 is downconverted to 8-bit.
- **`ffv1`/`mp4` modes require `ffmpeg` on the host.** If not found, recording is disabled at `initialize()` time.

## Changed Files Summary

| File | Operation | Description |
|------|-----------|-------------|
| `src/video_recorder.h` | **New** | VideoRecorder class (3 modes + error handling) |
| `src/video_recorder.cpp` | **New** | FFmpeg pipe + raw write + CSV frame log + safety measures |
| `src/video.h` | Modified | `#include` and `extern` declarations |
| `src/video.cpp` | Modified | Frame capture insertion in `encode_run()` + init/cleanup |
| `src/config.h` | Modified | 3 fields added to `video_t` |
| `src/config.cpp` | Modified | 3 lines added for config parsing |
| `cmake/compile_definitions/common.cmake` | Modified | Source file registration |
