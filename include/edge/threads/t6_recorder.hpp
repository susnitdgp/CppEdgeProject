#pragma once
// Thread 6: recorder. Writes raw market-data events exactly as XTS sent them, plus our receive time,
// to one gzip-compressed JSON-lines file per instrument and IST day:
//   <dir>/<symbol>/<YYYY-MM-DD>.jsonl.gz      one line per event:
//   {"rx":<unix ns, our receive time>,"ev":"<event name>","d":<XTS payload | socket status string>}
// Nothing in trading waits for it: thread 1 only enqueues; if this thread falls behind, thread 1 drops
// records (counted and logged) instead of blocking. Restarting on the same day appends a new gzip member
// to the same file (zcat / gzip -dc read it as one stream). Settings: engine-config.json "recorder".
#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <stop_token>
#include <string>

#include "edge/core/config.hpp"
#include "edge/core/log.hpp"
#include "edge/core/types.hpp"

namespace edge {

class RecorderThread {
 public:
  RecorderThread(const Config& cfg, Queue<RawEvent>& in) : cfg_(cfg), in_(in) {}

  void operator()(std::stop_token st) {
    set_thread_name("edge-recorder");
    const auto& rc = cfg_.engine.recorder;
    if (!rc.enabled) return;
    try {
      constexpr std::size_t kBatch = 256;
      RawEvent batch[kBatch];
      auto last_flush = now_ms();
      while (!st.stop_requested()) {
        const auto n = in_.wait_dequeue_bulk_timed(batch, kBatch, cfg_.engine.queue_poll());
        for (std::size_t i = 0; i < n; ++i) write(batch[i]);
        if (now_ms() - last_flush >= rc.flush_interval_ms) {
          if (file_) gzflush(file_, Z_SYNC_FLUSH);
          last_flush = now_ms();
        }
      }
      for (std::size_t n; (n = in_.try_dequeue_bulk(batch, kBatch)) > 0;)  // drain what thread 1 left
        for (std::size_t i = 0; i < n; ++i) write(batch[i]);
      close();
    } catch (const std::exception& e) {
      ELOG_ERROR("recorder stopped: {}", e.what());
      close();
    }
  }

 private:
  static std::string ist_date(std::int64_t rx_ns) {
    using namespace std::chrono;
    return std::format("{:%Y-%m-%d}", sys_seconds{seconds{rx_ns / 1'000'000'000 + 19'800}});
  }

  void write(const RawEvent& e) {
    if (const auto day = ist_date(e.rx_ns); day != day_) open(day);
    line_.clear();
    line_.append("{\"rx\":").append(std::to_string(e.rx_ns)).append(",\"ev\":\"").append(e.name);
    line_.append("\",\"d\":").append(e.data).append("}\n");
    if (gzwrite(file_, line_.data(), static_cast<unsigned>(line_.size())) <= 0)
      throw std::runtime_error("gzwrite failed for " + path_);
    ++events_;
    bytes_ += line_.size();
  }

  void open(const std::string& day) {
    close();
    const auto dir = std::filesystem::path(cfg_.engine.recorder.dir) / cfg_.instrument.symbol;
    std::filesystem::create_directories(dir);
    path_ = (dir / (day + ".jsonl.gz")).string();
    file_ = gzopen(path_.c_str(), "ab6");
    if (!file_) throw std::runtime_error("cannot open " + path_);
    gzbuffer(file_, 1 << 20);
    day_ = day;
    ELOG_INFO("recorder: writing {}", path_);
  }

  void close() {
    if (!file_) return;
    gzclose(file_);
    file_ = nullptr;
    ELOG_INFO("recorder: closed {} ({} events, {:.1f} MB before compression)", path_, events_,
              static_cast<double>(bytes_) / 1e6);
    events_ = bytes_ = 0;
  }

  const Config& cfg_;
  Queue<RawEvent>& in_;
  gzFile file_ = nullptr;
  std::string day_, path_, line_;
  std::uint64_t events_ = 0, bytes_ = 0;
};

}  // namespace edge
