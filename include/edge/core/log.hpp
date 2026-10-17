#pragma once
// quill: one async logger to console + a dated file. Use the ELOG_* macros everywhere.
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <filesystem>
#include <string>

namespace edge::log {

inline quill::Logger* g = nullptr;

inline void init(const std::string& dir, const std::string& level) {
  quill::Backend::start();
  std::filesystem::create_directories(dir);
  auto console = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
  auto file = quill::Frontend::create_or_get_sink<quill::FileSink>(
      dir + "/edge.log",
      [] {
        quill::FileSinkConfig c;
        c.set_open_mode('a');
        c.set_filename_append_option(quill::FilenameAppendOption::StartDate);
        return c;
      }(),
      quill::FileEventNotifier{});
  g = quill::Frontend::create_or_get_logger("edge", {std::move(console), std::move(file)});
  g->set_log_level(quill::loglevel_from_string(level));
}

}  // namespace edge::log

#define ELOG_DEBUG(...) LOG_DEBUG(::edge::log::g, __VA_ARGS__)
#define ELOG_INFO(...) LOG_INFO(::edge::log::g, __VA_ARGS__)
#define ELOG_WARN(...) LOG_WARNING(::edge::log::g, __VA_ARGS__)
#define ELOG_ERROR(...) LOG_ERROR(::edge::log::g, __VA_ARGS__)
