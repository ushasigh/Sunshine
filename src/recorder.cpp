#include "recorder.h"

#include <algorithm>
#include <boost/process/v1.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "config.h"
#include "entry_handler.h"
#include "logging.h"
#include "video.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace fs = std::filesystem;
namespace bp = boost::process::v1;

using namespace std::literals;

namespace video::recording {
  namespace {
    enum class output_format_e {
      yuv,
      mp4,
      mkv,
    };

    struct queued_frame_t {
      avcodec_frame_t frame;
      int64_t frame_index;
    };

    bool is_hw_frame(const AVFrame *frame) {
      if (!frame) {
        return false;
      }

      if (frame->hw_frames_ctx) {
        return true;
      }

      auto fmt_desc = av_pix_fmt_desc_get((AVPixelFormat) frame->format);
      return fmt_desc && (fmt_desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
    }

    std::string make_timestamp_string() {
      auto now = std::chrono::system_clock::now();
      auto tt = std::chrono::system_clock::to_time_t(now);

      std::tm tm {};
#ifdef _WIN32
      localtime_s(&tm, &tt);
#else
      localtime_r(&tt, &tm);
#endif

      std::ostringstream oss;
      oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
      return oss.str();
    }

    output_format_e output_format_from_config() {
      if (config::sunshine.recording_format == "mp4"sv) {
        return output_format_e::mp4;
      }
      if (config::sunshine.recording_format == "mkv"sv) {
        return output_format_e::mkv;
      }
      return output_format_e::yuv;
    }

    std::string_view output_format_name(output_format_e format) {
      switch (format) {
        case output_format_e::yuv:
          return "yuv"sv;
        case output_format_e::mp4:
          return "mp4"sv;
        case output_format_e::mkv:
          return "mkv"sv;
      }

      return "unknown"sv;
    }

    std::string_view output_extension(output_format_e format) {
      switch (format) {
        case output_format_e::yuv:
          return ".yuv"sv;
        case output_format_e::mp4:
          return ".mp4"sv;
        case output_format_e::mkv:
          return ".mkv"sv;
      }

      return ""sv;
    }

    avcodec_frame_t clone_frame_for_queue(const AVFrame *src) {
      avcodec_frame_t dst {av_frame_alloc()};
      if (!dst) {
        BOOST_LOG(error) << "recorder: failed to allocate frame clone";
        return {};
      }

      dst->format = src->format;
      dst->width = src->width;
      dst->height = src->height;
      dst->color_range = src->color_range;
      dst->color_primaries = src->color_primaries;
      dst->color_trc = src->color_trc;
      dst->colorspace = src->colorspace;
      dst->chroma_location = src->chroma_location;
      dst->pts = src->pts;

      if (av_frame_copy_props(dst.get(), src) < 0) {
        BOOST_LOG(error) << "recorder: failed to copy frame properties";
        return {};
      }

      if (is_hw_frame(src)) {
        if (src->hw_frames_ctx) {
          dst->hw_frames_ctx = av_buffer_ref(src->hw_frames_ctx);
          if (!dst->hw_frames_ctx) {
            BOOST_LOG(error) << "recorder: failed to reference hw_frames_ctx";
            return {};
          }
        } else {
          BOOST_LOG(error) << "recorder: hardware frame missing hw_frames_ctx";
          return {};
        }

        if (av_hwframe_get_buffer(dst->hw_frames_ctx, dst.get(), 0) < 0) {
          BOOST_LOG(error) << "recorder: failed to allocate hardware recording frame";
          return {};
        }

        if (av_hwframe_transfer_data(dst.get(), const_cast<AVFrame *>(src), 0) < 0) {
          BOOST_LOG(error) << "recorder: failed to copy frame into recording buffer";
          return {};
        }
      } else {
        if (av_frame_get_buffer(dst.get(), 0) < 0) {
          BOOST_LOG(error) << "recorder: failed to allocate software recording frame";
          return {};
        }

        if (av_frame_copy(dst.get(), src) < 0) {
          BOOST_LOG(error) << "recorder: failed to copy software frame";
          return {};
        }
      }

      return dst;
    }

    std::string frame_rate_to_string(AVRational frame_rate, int fallback_fps) {
      if (frame_rate.num > 0 && frame_rate.den > 0) {
        return std::to_string(frame_rate.num) + '/' + std::to_string(frame_rate.den);
      }

      return std::to_string(std::max(fallback_fps, 1));
    }
  }  // namespace

  struct recorder_t::impl_t {
    explicit impl_t(const config_t &config):
        width {config.width},
        height {config.height},
        fps {config.framerate > 0 ? config.framerate : 60},
        frame_rate {config.framerateX100 > 0 ? framerateX100_to_rational(config.framerateX100) : AVRational {fps, 1}},
        output_format {output_format_from_config()} {
      if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = AVRational {fps, 1};
      }
    }

    ~impl_t() {
      stop();
    }

    bool init_from_env() {
      output_dir = fs::path("/media/wcsng-32/3053f4a5-5eb1-4b85-b1e4-80fd10e1533b/sunshine_recordings_async");
      std::error_code ec;
      fs::create_directories(output_dir, ec);
      if (ec) {
        BOOST_LOG(error) << "recorder: failed to create output directory ["sv << output_dir.string() << "] "sv << ec.message();
        return false;
      }

      auto stem = "sunshine_recording_"s + make_timestamp_string();
      output_path = output_dir / (stem + std::string {output_extension(output_format)});
      meta_path = output_dir / (stem + ".yuv.meta");
      csv_path = output_dir / (stem + ".csv");

      csv_file.open(csv_path, std::ios::out | std::ios::trunc);
      if (!csv_file.is_open()) {
        BOOST_LOG(error) << "recorder: failed to open csv file ["sv << csv_path.string() << ']';
        return false;
      }
      csv_file << "local_frame_idx,frame_nr\n";

      if (output_format == output_format_e::yuv) {
        raw_file.open(output_path, std::ios::binary | std::ios::out);
        if (!raw_file.is_open()) {
          BOOST_LOG(error) << "recorder: failed to open output file ["sv << output_path.string() << ']';
          return false;
        }
      } else if (!start_ffmpeg_output()) {
        return false;
      }

      converted_frame.reset(av_frame_alloc());
      if (!converted_frame) {
        BOOST_LOG(error) << "recorder: failed to allocate conversion frame";
        return false;
      }

      converted_frame->format = AV_PIX_FMT_YUV420P;
      converted_frame->width = width;
      converted_frame->height = height;

      auto buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
      if (buffer_size <= 0) {
        BOOST_LOG(error) << "recorder: invalid conversion buffer size";
        return false;
      }

      converted_buffer.resize(buffer_size);
      if (av_image_fill_arrays(converted_frame->data, converted_frame->linesize, converted_buffer.data(), AV_PIX_FMT_YUV420P, width, height, 1) < 0) {
        BOOST_LOG(error) << "recorder: failed to setup conversion frame";
        return false;
      }

      if (output_format == output_format_e::yuv) {
        write_metadata();
      }

      worker = std::thread {[this]() {
        worker_loop();
      }};

      BOOST_LOG(info) << "recorder: format=" << output_format_name(output_format);
      BOOST_LOG(info) << "recorder: writing recording to ["sv << output_path.string() << ']';
      initialized = true;
      return true;
    }

    bool enqueue(const AVFrame *frame, int64_t frame_index) {
      auto cloned_frame = clone_frame_for_queue(frame);
      if (!cloned_frame) {
        return false;
      }

      std::lock_guard lg {queue_mutex};
      if (!running) {
        return false;
      }

      if (queue.size() == max_queue_depth) {
        ++dropped_frames;
        BOOST_LOG(warning) << "recorder: queue full, dropping newest frame " << frame_index << "; depth=" << max_queue_depth << '/' << max_queue_depth;
        if (config::sunshine.recording_stop_on_drop) {
          BOOST_LOG(fatal) << "recorder: stopping Sunshine because recording_stop_on_drop is enabled";
          lifetime::exit_sunshine(1, true);
        }
        return false;
      }

      queue.push_back(queued_frame_t {std::move(cloned_frame), frame_index});
      BOOST_LOG(info) << "recorder: enqueued frame " << frame_index << "; depth=" << queue.size() << '/' << max_queue_depth;
      queue_cv.notify_one();
      return true;
    }

    void stop() {
      {
        std::lock_guard lg {queue_mutex};
        if (!running) {
          return;
        }

        running = false;
      }

      queue_cv.notify_all();
      if (worker.joinable()) {
        worker.join();
      }

      if (raw_file.is_open()) {
        raw_file.close();
      }
      if (csv_file.is_open()) {
        csv_file.close();
      }

      finalize_ffmpeg_output();

      if (dropped_frames > 0) {
        BOOST_LOG(warning) << "recorder: dropped " << dropped_frames << " frames due to queue pressure";
      }

      if (initialized) {
        BOOST_LOG(info) << "recorder: finalized recording ["sv << output_path.string() << "] with " << written_frames << " frames";
      }
    }

    void worker_loop() {
      while (true) {
        std::optional<queued_frame_t> frame;
        std::size_t remaining_depth = 0;

        {
          std::unique_lock ul {queue_mutex};
          queue_cv.wait(ul, [&]() {
            return !queue.empty() || !running;
          });

          if (queue.empty()) {
            if (!running) {
              break;
            }
            continue;
          }

          frame.emplace(std::move(queue.front()));
          queue.pop_front();
          remaining_depth = queue.size();
        }

        BOOST_LOG(info) << "recorder: dequeued frame " << frame->frame_index << "; depth=" << remaining_depth << '/' << max_queue_depth;

        if (!write_frame(frame->frame.get(), frame->frame_index)) {
          BOOST_LOG(error) << "recorder: failed to write frame " << frame->frame_index;
        }
      }
    }

    bool write_frame(AVFrame *frame, int64_t frame_index) {
      AVFrame *sw_frame = frame;
      avcodec_frame_t temp_frame;

      if (is_hw_frame(frame)) {
        temp_frame.reset(av_frame_alloc());
        if (!temp_frame) {
          BOOST_LOG(error) << "recorder: failed to allocate readback frame";
          return false;
        }

        if (av_hwframe_transfer_data(temp_frame.get(), frame, 0) < 0) {
          BOOST_LOG(error) << "recorder: failed to transfer frame " << frame_index << " to CPU";
          return false;
        }

        temp_frame->width = frame->width;
        temp_frame->height = frame->height;
        sw_frame = temp_frame.get();
      }

      if (!ensure_sws_context(sw_frame)) {
        return false;
      }

      if (av_frame_make_writable(converted_frame.get()) < 0) {
        BOOST_LOG(error) << "recorder: converted frame is not writable";
        return false;
      }

      if (sws_scale(sws.get(), sw_frame->data, sw_frame->linesize, 0, sw_frame->height, converted_frame->data, converted_frame->linesize) <= 0) {
        BOOST_LOG(error) << "recorder: sws_scale failed for frame " << frame_index;
        return false;
      }

      converted_frame->pts = written_frames;

      bool ok = false;
      if (output_format == output_format_e::yuv) {
        write_plane(converted_frame->data[0], converted_frame->linesize[0], width, height);
        write_plane(converted_frame->data[1], converted_frame->linesize[1], width / 2, height / 2);
        write_plane(converted_frame->data[2], converted_frame->linesize[2], width / 2, height / 2);
        ok = raw_file.good();
      } else {
        write_plane(converted_frame->data[0], converted_frame->linesize[0], width, height);
        write_plane(converted_frame->data[1], converted_frame->linesize[1], width / 2, height / 2);
        write_plane(converted_frame->data[2], converted_frame->linesize[2], width / 2, height / 2);
        ok = ffmpeg_input && ffmpeg_input->good();
        if (ok && ffmpeg_process && !ffmpeg_process->running()) {
          BOOST_LOG(error) << "recorder: ffmpeg exited before frame " << frame_index << " was fully written";
          ok = false;
        }
      }

      if (ok) {
        write_csv_row(written_frames, frame_index);
        ++written_frames;
      }

      return ok;
    }

    bool start_ffmpeg_output() {
      auto ffmpeg_executable = bp::search_path("ffmpeg");
      if (ffmpeg_executable.empty()) {
        BOOST_LOG(error) << "recorder: failed to locate ffmpeg in PATH";
        return false;
      }

      std::vector<std::string> args {
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        "-video_size",
        std::to_string(width) + 'x' + std::to_string(height),
        "-framerate",
        frame_rate_to_string(frame_rate, fps),
        "-i",
        "-",
        "-an",
      };

      if (output_format == output_format_e::mp4) {
        args.insert(args.end(), {
          "-c:v",
          "libx264",
          "-preset",
          "ultrafast",
          "-tune",
          "zerolatency",
          "-movflags",
          "+faststart",
        });
      } else {
        args.insert(args.end(), {
          "-c:v",
          "ffv1",
        });
      }

      args.push_back(output_path.string());

      std::error_code ec;
      ffmpeg_input = std::make_unique<bp::opstream>();
      ffmpeg_process = std::make_unique<bp::child>(
        ffmpeg_executable,
        bp::args(args),
        bp::std_in < *ffmpeg_input,
        bp::std_out > bp::null,
        bp::std_err > bp::null,
        bp::limit_handles,
        ec
      );

      if (ec) {
        BOOST_LOG(error) << "recorder: failed to start ffmpeg ["sv << ffmpeg_executable.string() << "]: "sv << ec.message();
        ffmpeg_input.reset();
        ffmpeg_process.reset();
        return false;
      }

      BOOST_LOG(info) << "recorder: started ffmpeg ["sv << ffmpeg_executable.string() << "] for "sv << output_format_name(output_format);
      return true;
    }

    void finalize_ffmpeg_output() {
      if (!ffmpeg_process) {
        return;
      }

      if (ffmpeg_input) {
        ffmpeg_input->flush();
        ffmpeg_input->pipe().close();
        ffmpeg_input.reset();
      }

      std::error_code ec;
      ffmpeg_process->wait(ec);
      if (ec) {
        BOOST_LOG(error) << "recorder: failed while waiting for ffmpeg shutdown: " << ec.message();
      } else if (ffmpeg_process->exit_code() != 0) {
        BOOST_LOG(error) << "recorder: ffmpeg exited with code " << ffmpeg_process->exit_code();
      }

      ffmpeg_process.reset();
    }

    bool ensure_sws_context(const AVFrame *frame) {
      if (sws && frame->format == last_input_format && frame->width == last_input_width && frame->height == last_input_height) {
        return true;
      }

      sws.reset(sws_getContext(
        frame->width,
        frame->height,
        (AVPixelFormat) frame->format,
        width,
        height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
      ));

      if (!sws) {
        BOOST_LOG(error) << "recorder: failed to create swscale context for input format " << frame->format;
        return false;
      }

      last_input_format = frame->format;
      last_input_width = frame->width;
      last_input_height = frame->height;
      return true;
    }

    void write_plane(const uint8_t *data, int linesize, int plane_width, int plane_height) {
      for (int y = 0; y < plane_height; ++y) {
        if (output_format == output_format_e::yuv) {
          raw_file.write((const char *) (data + y * linesize), plane_width);
        } else if (ffmpeg_input) {
          ffmpeg_input->write((const char *) (data + y * linesize), plane_width);
        }
      }
    }

    void write_metadata() {
      std::ofstream meta {meta_path, std::ios::out | std::ios::trunc};
      if (!meta.is_open()) {
        BOOST_LOG(warning) << "recorder: failed to open metadata file ["sv << meta_path.string() << ']';
        return;
      }

      meta << "width=" << width << '\n';
      meta << "height=" << height << '\n';
      meta << "fps=" << fps << '\n';
      meta << "format=yuv420p\n";
      meta << "raw_path=" << output_path.string() << '\n';
      meta << "csv_path=" << csv_path.string() << '\n';
    }

    void write_csv_row(int64_t local_frame_idx, int64_t frame_nr) {
      if (!csv_file.is_open()) {
        return;
      }

      csv_file << local_frame_idx
               << ',' << frame_nr
               << '\n';
    }

    fs::path output_dir;
    fs::path output_path;
    fs::path meta_path;
    fs::path csv_path;

    std::ofstream raw_file;
    std::ofstream csv_file;
    std::unique_ptr<bp::opstream> ffmpeg_input;
    std::unique_ptr<bp::child> ffmpeg_process;
    sws_t sws;
    avcodec_frame_t converted_frame;
    std::vector<uint8_t> converted_buffer;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<queued_frame_t> queue;
    std::thread worker;

    bool initialized {false};
    bool running {true};
    int width {0};
    int height {0};
    int fps {0};
    AVRational frame_rate {0, 1};
    output_format_e output_format {output_format_e::yuv};
    int last_input_format {AV_PIX_FMT_NONE};
    int last_input_width {0};
    int last_input_height {0};
    int64_t written_frames {0};
    int64_t dropped_frames {0};

    static constexpr std::size_t max_queue_depth = 100;
  };

  recorder_t::~recorder_t() = default;

  std::shared_ptr<recorder_t> recorder_t::create_from_env(const config_t &config) {
    auto recorder = std::shared_ptr<recorder_t>(new recorder_t {});
    recorder->impl = std::make_unique<impl_t>(config);
    if (!recorder->impl->init_from_env()) {
      return nullptr;
    }

    return recorder;
  }

  bool recorder_t::enqueue_frame(const AVFrame *frame, int64_t frame_index) {
    if (!impl || !frame) {
      return false;
    }

    return impl->enqueue(frame, frame_index);
  }
}  // namespace video::recording
