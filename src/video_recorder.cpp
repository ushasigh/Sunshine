/**
 * @file src/video_recorder.cpp
 * @brief Frame recorder implementation.
 */
#include "video_recorder.h"

#include <boost/log/trivial.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

namespace video {

  capture_format_e capture_format_from_string(const std::string &str) {
    if (str == "mp4") return capture_format_e::mp4;
    if (str == "raw") return capture_format_e::raw;
    return capture_format_e::ffv1;
  }

  VideoRecorder::VideoRecorder() = default;

  VideoRecorder::~VideoRecorder() {
    finalize();
  }

  // ============================================================
  //  ユーティリティ
  // ============================================================

  bool VideoRecorder::is_supported_format(AVPixelFormat fmt) {
    return fmt == AV_PIX_FMT_NV12 ||
           fmt == AV_PIX_FMT_YUV420P ||
           fmt == AV_PIX_FMT_P010;
  }

  bool VideoRecorder::validate_path(const std::string &path) {
    for (char c : path) {
      if (c == '"' || c == '\'' || c == ';' || c == '`' ||
          c == '$' || c == '|' || c == '&' || c == '\n') {
        BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Invalid character in path: " << path;
        return false;
      }
    }
    return true;
  }

  bool VideoRecorder::check_ffmpeg_available() {
    int ret = std::system("which ffmpeg > /dev/null 2>&1");
    if (ret != 0) {
      BOOST_LOG_TRIVIAL(error) << "VideoRecorder: FFmpeg executable not found. "
                       << "Capture disabled. "
                       << "Output mode ffv1/mp4 requires ffmpeg in PATH. "
                       << "Install with: sudo apt install ffmpeg";
      return false;
    }
    return true;
  }

  // 注意: この関数は呼び出し側が m_mutex を保持している前提。
  // 外部から直接呼ばないこと。
  void VideoRecorder::abort_recording(const std::string &reason) {
    BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Aborting recording — " << reason;
    m_recording = false;

    if (m_ffmpeg_pipe) {
      pclose(m_ffmpeg_pipe);
      m_ffmpeg_pipe = nullptr;
    }
    if (m_raw_file.is_open()) {
      m_raw_file.close();
    }
    if (m_frame_log.is_open()) {
      m_frame_log.flush();
      m_frame_log.close();
    }

    BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Recording aborted after " << m_frames_written
                            << " frames. Output (possibly incomplete): " << m_output_path;
  }

  // ============================================================
  //  initialize()
  // ============================================================

  bool VideoRecorder::initialize(const std::string &output_dir, int width, int height, int fps,
                                  capture_format_e format) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_recording) {
      BOOST_LOG_TRIVIAL(warning) << "VideoRecorder: Already recording";
      return false;
    }

    if (!validate_path(output_dir)) {
      return false;
    }

    // ffv1/mp4 モードでは ffmpeg の存在を事前確認
    if (format != capture_format_e::raw) {
      if (!check_ffmpeg_available()) {
        return false;
      }
    }

    m_output_dir = output_dir;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_format = format;
    m_frames_written = 0;
    m_pix_fmt = AV_PIX_FMT_NONE;
    m_output_started = false;

    std::filesystem::create_directories(output_dir);

    // SIGPIPE を無視する設定。
    // FFmpeg パイプが先に閉じた場合に Sunshine プロセスが
    // 終了するのを防ぐ。fwrite() の戻り値で書き込み失敗を検出する。
    // 注: プロセス全体に影響する。研究用途では許容。
    std::signal(SIGPIPE, SIG_IGN);

    m_recording = true;

    const char *mode_str = (format == capture_format_e::ffv1) ? "ffv1" :
                           (format == capture_format_e::mp4)  ? "mp4"  : "raw";
    BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Initialized (" << width << "x" << height
                            << " @ " << fps << "fps, mode=" << mode_str
                            << "). Waiting for first frame to detect pixel format.";
    return true;
  }

  // ============================================================
  //  FFmpeg パイプ起動 (ffv1 / mp4)
  // ============================================================

  bool VideoRecorder::start_ffmpeg_pipe(AVPixelFormat fmt) {
    m_pix_fmt = fmt;

    const char *pix_fmt_name = av_get_pix_fmt_name(fmt);
    if (!pix_fmt_name) {
      BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Unknown pixel format: " << fmt;
      return false;
    }

    // タイムスタンプ付きファイル名（スレッド安全な localtime_r を使用）
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");

    std::stringstream cmd;
    cmd << "ffmpeg -y -hide_banner -loglevel warning"
        << " -f rawvideo"
        << " -pix_fmt " << pix_fmt_name
        << " -s " << m_width << "x" << m_height
        << " -r " << m_fps
        << " -i pipe:0";

    if (m_format == capture_format_e::ffv1) {
      m_output_path = m_output_dir + "/sunshine_capture_" + ss.str() + ".mkv";
      if (!validate_path(m_output_path)) return false;
      cmd << " -c:v ffv1"
          << " -level 3"
          << " -slicecrc 1"
          << " -threads 2"
          << " \"" << m_output_path << "\"";
    } else {
      // mp4 mode
      // 注意: FFmpeg 側で NV12→YUV420P 等の色変換が自動的に行われる場合がある。
      m_output_path = m_output_dir + "/sunshine_capture_" + ss.str() + ".mp4";
      if (!validate_path(m_output_path)) return false;
      cmd << " -c:v libx264"
          << " -preset ultrafast"
          << " -crf 18"
          << " -pix_fmt yuv420p"
          << " \"" << m_output_path << "\"";
    }

    m_ffmpeg_pipe = popen(cmd.str().c_str(), "w");
    if (!m_ffmpeg_pipe) {
      BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Could not start FFmpeg: " << cmd.str();
      return false;
    }

    write_meta_file(fmt);

    // フレームログ開始
    m_frame_log.open(m_output_path + ".frames.csv");
    if (m_frame_log.is_open()) {
      m_frame_log << "local_frame_idx,frame_nr,is_idr\n";
      m_frame_log.flush();
      BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Frame log opened at " << m_output_path << ".frames.csv";
    } else {
      BOOST_LOG_TRIVIAL(warning) << "VideoRecorder: Could not open frame log: " << m_output_path << ".frames.csv";
    }

    const char *mode_str = (m_format == capture_format_e::ffv1) ? "FFV1 lossless" : "H.264 (crf 18)";
    BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Recording → " << m_output_path
                            << " (" << mode_str
                            << ", " << m_width << "x" << m_height << " @ " << m_fps << "fps"
                            << ", source: " << pix_fmt_name << ")";
    return true;
  }

  // ============================================================
  //  Raw ファイル起動
  // ============================================================

  bool VideoRecorder::start_raw_file(AVPixelFormat fmt) {
    m_pix_fmt = fmt;

    const char *pix_fmt_name = av_get_pix_fmt_name(fmt);
    if (!pix_fmt_name) {
      BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Unknown pixel format: " << fmt;
      return false;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    m_output_path = m_output_dir + "/sunshine_capture_" + ss.str() + ".yuv";

    if (!validate_path(m_output_path)) return false;

    m_raw_file.open(m_output_path, std::ios::binary);
    if (!m_raw_file.is_open()) {
      BOOST_LOG_TRIVIAL(error) << "VideoRecorder: Could not open: " << m_output_path;
      return false;
    }

    write_meta_file(fmt);

    // フレームログ開始
    m_frame_log.open(m_output_path + ".frames.csv");
    if (m_frame_log.is_open()) {
      m_frame_log << "local_frame_idx,frame_nr,is_idr\n";
      m_frame_log.flush();
      BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Frame log opened at " << m_output_path << ".frames.csv";
    } else {
      BOOST_LOG_TRIVIAL(warning) << "VideoRecorder: Could not open frame log: " << m_output_path << ".frames.csv";
    }

    BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Recording raw → " << m_output_path
                            << " (" << m_width << "x" << m_height << " @ " << m_fps << "fps"
                            << ", source: " << pix_fmt_name << ")";
    return true;
  }

  // ============================================================
  //  メタデータファイル
  // ============================================================

  void VideoRecorder::write_meta_file(AVPixelFormat fmt) {
    const char *pix_fmt_name = av_get_pix_fmt_name(fmt);
    std::ofstream meta(m_output_path + ".meta");
    if (!meta.is_open()) return;

    meta << "width=" << m_width << "\n"
         << "height=" << m_height << "\n"
         << "fps=" << m_fps << "\n"
         << "source_pix_fmt=" << (pix_fmt_name ? pix_fmt_name : "unknown") << "\n";

    if (m_format == capture_format_e::ffv1) {
      meta << "codec=ffv1\n"
           << "lossless=true\n"
           << "\n"
           << "# Extract to raw YUV420P:\n"
           << "# ffmpeg -i \"" << m_output_path << "\""
           << " -pix_fmt yuv420p -f rawvideo output.yuv\n"
           << "\n"
           << "# VMAF comparison:\n"
           << "# ffmpeg -i \"" << m_output_path << "\""
           << " -f rawvideo -pix_fmt yuv420p"
           << " -s " << m_width << "x" << m_height << " -r " << m_fps
           << " -i moonlight_recording.yuv"
           << " -lavfi libvmaf -f null -\n";
    } else if (m_format == capture_format_e::mp4) {
      meta << "codec=h264\n"
           << "lossless=false\n"
           << "crf=18\n"
           << "note=FFmpeg may apply color conversion internally\n";
    } else {
      meta << "codec=rawvideo\n"
           << "lossless=true\n"
           << "\n"
           << "# Convert to MP4:\n"
           << "# ffmpeg -f rawvideo -pix_fmt " << (pix_fmt_name ? pix_fmt_name : "nv12")
           << " -s " << m_width << "x" << m_height
           << " -r " << m_fps
           << " -i \"" << m_output_path << "\""
           << " -c:v libx264 -pix_fmt yuv420p -crf 18 output.mp4\n";
    }
  }

  // ============================================================
  //  write_frame()
  // ============================================================

  bool VideoRecorder::write_frame(AVFrame *frame,
                                  int64_t frame_nr,
                                  std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);

    (void) frame_timestamp;  // timestampは現在CSVに出力していない

    if (!m_recording || !frame) {
      return false;
    }

    AVFrame *sw_frame = frame;
    AVFrame *temp_frame = nullptr;
    AVFrame *converted_frame = nullptr;

    // HW フレームの場合、CPU メモリに転送
    if (frame->hw_frames_ctx) {
      temp_frame = av_frame_alloc();
      if (!temp_frame) {
        abort_recording("Could not allocate temp frame for GPU transfer");
        return false;
      }

      int ret = av_hwframe_transfer_data(temp_frame, frame, 0);
      if (ret < 0) {
        char errstr[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errstr, sizeof(errstr));
        av_frame_free(&temp_frame);
        abort_recording(std::string("GPU→CPU transfer failed: ") + errstr);
        return false;
      }

      temp_frame->width = frame->width;
      temp_frame->height = frame->height;
      sw_frame = temp_frame;
    }

    // 初回フレームでピクセルフォーマットを確定して出力を起動
    if (!m_output_started) {
      auto fmt = (AVPixelFormat) sw_frame->format;
      auto output_fmt = fmt;

      // raw/ffv1/mp4 すべて YUV420P に寄せる場合の出力フォーマット決定
      if ((fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010) &&
          (m_format == capture_format_e::raw || m_format == capture_format_e::ffv1 || m_format == capture_format_e::mp4)) {
        output_fmt = AV_PIX_FMT_YUV420P;
      }

      if (!is_supported_format(fmt)) {
        const char *name = av_get_pix_fmt_name(fmt);
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording(std::string("Unsupported pixel format: ") + (name ? name : "unknown")
                        + ". Supported: NV12, YUV420P, P010.");
        return false;
      }

      bool ok;
      if (m_format == capture_format_e::raw) {
        ok = start_raw_file(output_fmt);
      } else {
        ok = start_ffmpeg_pipe(output_fmt);
      }
      if (!ok) {
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording("Failed to start output");
        return false;
      }
      m_output_started = true;
    }

    // 書き出し
    auto src_fmt = (AVPixelFormat) sw_frame->format;
    auto dst_fmt = m_pix_fmt;

    // NV12/P010 → YUV420P 変換（raw, ffv1, mp4 共通）
    if ((src_fmt == AV_PIX_FMT_NV12 || src_fmt == AV_PIX_FMT_P010) &&
        dst_fmt == AV_PIX_FMT_YUV420P) {
      converted_frame = av_frame_alloc();
      if (!converted_frame) {
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording("Could not allocate convert frame");
        return false;
      }
      converted_frame->format = dst_fmt;
      converted_frame->width = sw_frame->width;
      converted_frame->height = sw_frame->height;
      if (av_frame_get_buffer(converted_frame, 32) < 0) {
        av_frame_free(&converted_frame);
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording("Could not allocate buffer for convert frame");
        return false;
      }

      m_sws_to_yuv420p = sws_getCachedContext(
        m_sws_to_yuv420p,
        sw_frame->width, sw_frame->height, src_fmt,
        sw_frame->width, sw_frame->height, dst_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!m_sws_to_yuv420p) {
        av_frame_free(&converted_frame);
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording("Could not create swscale context for conversion");
        return false;
      }

      int ret = sws_scale_frame(m_sws_to_yuv420p, converted_frame, sw_frame);
      if (ret < 0) {
        char errstr[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errstr, sizeof(errstr));
        av_frame_free(&converted_frame);
        if (temp_frame) av_frame_free(&temp_frame);
        abort_recording(std::string("Conversion failed: ") + errstr);
        return false;
      }
      sw_frame = converted_frame;
    }

    bool ok;
    if (m_format == capture_format_e::raw) {
      ok = write_planes_to_file(sw_frame);
    } else {
      ok = write_planes_to_pipe(sw_frame);
    }

    if (temp_frame) {
      av_frame_free(&temp_frame);
    }
    if (converted_frame) {
      av_frame_free(&converted_frame);
    }

    if (!ok) {
      abort_recording("Write failed (pipe broken or disk full?)");
      return false;
    }

    // フレームメタログ追記（フレームごとに直接 flush）
    if (m_frame_log.is_open()) {
      bool is_idr = (frame->flags & AV_FRAME_FLAG_KEY) != 0;

      m_frame_log
        << m_frames_written << ","
        << frame_nr << ","
        << (is_idr ? 1 : 0) << "\n";
      m_frame_log.flush();
    } else {
      BOOST_LOG_TRIVIAL(debug) << "VideoRecorder: Frame log not open; skipping metadata write";
    }

    m_frames_written++;
    return true;
  }

  // ============================================================
  //  プレーン書き出し
  // ============================================================

  bool VideoRecorder::write_planes_to_pipe(AVFrame *sw_frame) {
    if (!m_ffmpeg_pipe) return false;

    auto fmt = (AVPixelFormat) sw_frame->format;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    if (!desc) return false;

    int num_planes = av_pix_fmt_count_planes(fmt);
    int bytes_per_component = (desc->comp[0].depth > 8) ? 2 : 1;

    for (int plane = 0; plane < num_planes; plane++) {
      int plane_h = m_height;
      int row_bytes;

      if (plane == 0) {
        row_bytes = m_width * bytes_per_component;
      } else {
        plane_h >>= desc->log2_chroma_h;
        if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010) {
          row_bytes = m_width * bytes_per_component;
        } else {
          row_bytes = (m_width >> desc->log2_chroma_w) * bytes_per_component;
        }
      }

      for (int y = 0; y < plane_h; y++) {
        size_t written = fwrite(
          sw_frame->data[plane] + y * sw_frame->linesize[plane],
          1, row_bytes, m_ffmpeg_pipe
        );
        if (written != (size_t) row_bytes) {
          return false;
        }
      }
    }

    return true;
  }

  bool VideoRecorder::write_planes_to_file(AVFrame *sw_frame) {
    if (!m_raw_file.is_open()) return false;

    auto fmt = (AVPixelFormat) sw_frame->format;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    if (!desc) return false;

    int num_planes = av_pix_fmt_count_planes(fmt);
    int bytes_per_component = (desc->comp[0].depth > 8) ? 2 : 1;

    for (int plane = 0; plane < num_planes; plane++) {
      int plane_h = m_height;
      int row_bytes;

      if (plane == 0) {
        row_bytes = m_width * bytes_per_component;
      } else {
        plane_h >>= desc->log2_chroma_h;
        if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010) {
          row_bytes = m_width * bytes_per_component;
        } else {
          row_bytes = (m_width >> desc->log2_chroma_w) * bytes_per_component;
        }
      }

      for (int y = 0; y < plane_h; y++) {
        m_raw_file.write(
          reinterpret_cast<const char *>(sw_frame->data[plane] + y * sw_frame->linesize[plane]),
          row_bytes
        );
      }
    }

    return m_raw_file.good();
  }

  // ============================================================
  //  finalize()
  // ============================================================

  void VideoRecorder::finalize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_recording) {
      return;
    }

    m_recording = false;

    if (m_ffmpeg_pipe) {
      pclose(m_ffmpeg_pipe);
      m_ffmpeg_pipe = nullptr;
    }

    if (m_raw_file.is_open()) {
      m_raw_file.flush();
      m_raw_file.close();
    }

    if (m_frame_log.is_open()) {
      m_frame_log.flush();
      m_frame_log.close();
    }

    if (m_sws_to_yuv420p) {
      sws_freeContext(m_sws_to_yuv420p);
      m_sws_to_yuv420p = nullptr;
    }

    BOOST_LOG_TRIVIAL(info) << "VideoRecorder: Stopped. Frames: " << m_frames_written
                            << ", Output: " << m_output_path;
  }

}  // namespace video
