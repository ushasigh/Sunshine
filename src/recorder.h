#pragma once

#include <memory>

struct AVFrame;

namespace video {
  struct config_t;

  namespace recording {
    class recorder_t {
    public:
      recorder_t(const recorder_t &) = delete;
      recorder_t &operator=(const recorder_t &) = delete;

      ~recorder_t();

      static std::shared_ptr<recorder_t> create_from_env(const config_t &config);

      bool enqueue_frame(const AVFrame *frame, int64_t frame_index);

    private:
      recorder_t() = default;

      struct impl_t;
      std::unique_ptr<impl_t> impl;
    };
  }  // namespace recording
}  // namespace video
