/**
 * @file src/video_recorder.h
 * @brief Frame recorder with selectable output format.
 *
 * Modes:
 *   ffv1 — FFmpeg pipe → FFV1 lossless (.mkv)
 *   mp4  — FFmpeg pipe → H.264 lossy (.mp4)
 *   raw  — Direct file write, no FFmpeg needed (.yuv)
 *
 * Supported input pixel formats: NV12, YUV420P, P010.
 * Other formats will cause recording to be automatically disabled.
 *
 * This runs synchronously on the encode thread.
 * Intended for short captures (10s–2min) for quality evaluation.
 *
 * Width, height, and fps are determined by the Moonlight client
 * negotiation and passed via config_t at encode_run() start.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace video {

  enum class capture_format_e {
    ffv1,  ///< FFV1 lossless in MKV container
    mp4,   ///< H.264 lossy in MP4 container
    raw    ///< Raw frames, no encoding
  };

  class VideoRecorder {
  public:
    VideoRecorder();
    ~VideoRecorder();

    /**
     * @brief 録画を開始する。
     *        width/height/fps はクライアントネゴシエーションで確定した値を渡す。
     *        ffv1/mp4 モードでは ffmpeg の存在を事前確認する。
     *        ピクセルフォーマットは最初のフレームから自動検出する。
     * @param output_dir 出力ディレクトリ
     * @param width フレーム幅（config_t から）
     * @param height フレーム高さ（config_t から）
     * @param fps フレームレート（config_t から）
     * @param format 出力フォーマット
     * @return 成功時 true
     */
    bool initialize(const std::string &output_dir, int width, int height, int fps,
                    capture_format_e format);

    /**
     * @brief フレームを保存する。
     *        初回呼び出し時にピクセルフォーマットを検出して出力を起動する。
     *        失敗した場合は自動的に録画を停止し、以降の呼び出しは何もしない。
     * @param frame エンコーダの AVFrame
     * @param frame_nr エンコードスレッドの frame カウンタ（インクリメント前の値）
     * @param frame_timestamp キャプチャ時刻（steady_clock）。nullopt なら duplicate 判定に使う
     * @return 成功時 true
     */
    bool write_frame(AVFrame *frame,
                     int64_t frame_nr,
                     std::optional<std::chrono::steady_clock::time_point> frame_timestamp);

    /**
     * @brief 録画を停止する。
     */
    void finalize();

    bool is_recording() const { return m_recording.load(); }
    std::string get_output_path() const { return m_output_path; }
    int64_t frames_written() const { return m_frames_written; }

  private:
    bool is_supported_format(AVPixelFormat fmt);
    bool start_ffmpeg_pipe(AVPixelFormat fmt);
    bool start_raw_file(AVPixelFormat fmt);
    bool write_planes_to_pipe(AVFrame *sw_frame);
    bool write_planes_to_file(AVFrame *sw_frame);
    void write_meta_file(AVPixelFormat fmt);
    bool validate_path(const std::string &path);
    bool check_ffmpeg_available();

    /**
     * @brief 録画失敗時に呼ぶ。ログを出して録画を停止する。
     *        注意: この関数は呼び出し側が m_mutex を保持している前提。
     *        外部から直接呼ばないこと。
     */
    void abort_recording(const std::string &reason);

    std::atomic<bool> m_recording {false};
    capture_format_e m_format = capture_format_e::ffv1;

    std::string m_output_dir;
    std::string m_output_path;

    // FFmpeg pipe (ffv1 / mp4 mode)
    FILE *m_ffmpeg_pipe = nullptr;

    // Raw file (raw mode)
    std::ofstream m_raw_file;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    int64_t m_frames_written = 0;
    AVPixelFormat m_pix_fmt = AV_PIX_FMT_NONE;
    bool m_output_started = false;  ///< FFmpeg パイプまたは raw ファイルが起動済みか
    SwsContext *m_sws_to_yuv420p = nullptr;  ///< Raw保存用の色変換コンテキスト

    // フレームごとのメタログ
    std::ofstream m_frame_log;

    std::mutex m_mutex;
  };

  capture_format_e capture_format_from_string(const std::string &str);

}  // namespace video
