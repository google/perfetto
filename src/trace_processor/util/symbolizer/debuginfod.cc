/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/util/symbolizer/debuginfod.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/atomic_file.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/progress_reporter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"

namespace perfetto::profiling {
namespace {

std::string EnvironmentValue(const char* name) {
  const char* value = getenv(name);
  return value ? value : "";
}

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
bool MakeCacheDirectory(const std::string& path) {
  if (base::DirectoryExists(path))
    return true;
  if (path.empty())
    return false;
  std::string parent = base::Dirname(path);
  if (parent != path && !MakeCacheDirectory(parent))
    return false;
  return base::Mkdir(path, 0700) || base::DirectoryExists(path);
}

class DebuginfodBinaryFinder : public BinaryFinder {
 public:
  DebuginfodBinaryFinder(const DebuginfodConfig& config,
                         bool progress,
                         DebuginfodStats* stats)
      : config_(config), progress_(progress), stats_(stats) {}

  BinaryLookupResult FindBinary(const std::string&,
                                const std::string& build_id) override {
    auto entry = results_.emplace(build_id, BinaryLookupResult{});
    if (!entry.second)
      return entry.first->second;
    auto& result = entry.first->second;
    if (build_id.empty())
      return result;
    const std::string hex = base::ToHex(build_id);
    const std::string directory = config_.cache_path + "/" + hex;
    const std::string path = directory + "/debuginfo";
    BinaryPathError error;
    result.binary = FindBinaryFile(path, build_id, &error);
    if (result.binary) {
      ++stats_->cache_hits;
      result.attempts.push_back({path, BinaryPathError::kOk});
      stats_->details += "  " + hex + ": cache hit " + path + "\n";
      return result;
    }
    if (!MakeCacheDirectory(directory)) {
      ++stats_->failures;
      stats_->details +=
          "  " + hex + ": cannot create cache directory " + directory + "\n";
      return result;
    }
    for (const auto& server : config_.urls) {
      const std::string url = server + "/buildid/" + hex + "/debuginfo";
      base::AtomicFile output(path);
      auto status = output.Open();
      if (!status.ok()) {
        stats_->details += "  " + hex + ": " + status.message() + "\n";
        break;
      }
      base::ProgressReporter progress(progress_);
      progress.Update("Debuginfod: fetching build ID " + hex);
      base::Subprocess curl;
      // Disable curlrc and URL globbing. Keep proxy/TLS environment support,
      // but let the CLI own output, protocols, and timeout policy.
      curl.args.exec_cmd = {"curl",
                            "--disable",
                            "--globoff",
                            "--fail",
                            "--silent",
                            "--show-error",
                            "--location",
                            "--proto",
                            "=http,https",
                            "--proto-redir",
                            "=http,https",
                            "--connect-timeout",
                            std::to_string(config_.connect_timeout_seconds),
                            "--speed-limit",
                            "1",
                            "--speed-time",
                            std::to_string(config_.stall_timeout_seconds),
                            "--output",
                            output.temp_path(),
                            "--url",
                            url};
      curl.args.stdin_mode = base::Subprocess::InputMode::kDevNull;
      curl.args.stdout_mode = base::Subprocess::OutputMode::kDevNull;
      curl.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
      bool ok = curl.Call();
      progress.Clear();
      if (!ok) {
        if (curl.returncode() == 127 || curl.returncode() == 128) {
          stats_->warnings =
              "Cannot run curl; install curl and ensure it is "
              "on PATH.\n";
          break;
        }
        stats_->details += "  " + hex + ": curl exit " +
                           std::to_string(curl.returncode()) + " from " +
                           server + ": " + base::TrimWhitespace(curl.output()) +
                           "\n";
        continue;
      }
      result.binary = FindBinaryFile(output.temp_path(), build_id, &error);
      if (!result.binary) {
        result.attempts.push_back({server, error});
        stats_->details +=
            "  " + hex + ": invalid debug file from " + server + "\n";
        continue;
      }
      status = std::move(output).Commit();
      if (!status.ok()) {
        result.binary.reset();
        stats_->details += "  " + hex + ": " + status.message() + "\n";
        break;
      }
      result.binary->file_name = path;
      result.attempts.push_back({path, BinaryPathError::kOk});
      ++stats_->downloads;
      stats_->details +=
          "  " + hex + ": downloaded from " + server + " to " + path + "\n";
      return result;
    }
    ++stats_->failures;
    return result;
  }

 private:
  DebuginfodConfig config_;
  bool progress_;
  DebuginfodStats* stats_;
  std::map<std::string, BinaryLookupResult> results_;
};
#endif

}  // namespace

base::Status ResolveDebuginfodOptions(const DebuginfodOptions& options,
                                      DebuginfodConfig* config,
                                      std::string* warnings) {
  if (!EnvironmentValue("LLVM_SYMBOLIZER_OPTS").empty())
    *warnings +=
        "LLVM_SYMBOLIZER_OPTS is ignored; Perfetto controls "
        "llvm-symbolizer options.\n";
  const std::string env_urls = EnvironmentValue("DEBUGINFOD_URLS");
  if (!options.enabled) {
    if (!env_urls.empty() || options.urls.has_value()) {
      *warnings += options.urls ? "--debuginfod-urls is set but ignored; "
                                : "DEBUGINFOD_URLS is set but ignored; ";
      *warnings += "pass --debuginfod to enable downloads.\n";
    }
    return base::OkStatus();
  }
#if !PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  return base::ErrStatus(
      "this build does not support debuginfod symbolization");
#endif
  std::istringstream urls(options.urls.value_or(env_urls));
  for (std::string url; urls >> url;) {
    while (!url.empty() && url.back() == '/')
      url.pop_back();
    if ((!base::StartsWith(url, "https://") &&
         !base::StartsWith(url, "http://")) ||
        url.find_first_of("?#") != std::string::npos)
      return base::ErrStatus("debuginfod URLs must be HTTP(S) server roots");
    config->urls.push_back(std::move(url));
  }
  if (config->urls.empty())
    return base::ErrStatus(
        "--debuginfod requires --debuginfod-urls or "
        "DEBUGINFOD_URLS");
  auto connect = base::StringToUInt32(options.connect_timeout);
  auto stall = base::StringToUInt32(options.stall_timeout);
  if (!connect || !*connect || !stall || !*stall ||
      options.connect_timeout.find_first_not_of("0123456789") !=
          std::string::npos ||
      options.stall_timeout.find_first_not_of("0123456789") !=
          std::string::npos)
    return base::ErrStatus(
        "debuginfod timeouts must be positive whole seconds");
  config->connect_timeout_seconds = *connect;
  config->stall_timeout_seconds = *stall;
  config->cache_path =
      options.cache_path.value_or(EnvironmentValue("DEBUGINFOD_CACHE_PATH"));
  if (!options.cache_path && config->cache_path.empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    std::string root = EnvironmentValue("LOCALAPPDATA");
    if (!root.empty())
      config->cache_path = root + "/debuginfod_client";
#else
    std::string root = EnvironmentValue("XDG_CACHE_HOME");
    if (root.empty()) {
      root = EnvironmentValue("HOME");
      if (!root.empty())
        root += "/.cache";
    }
    if (!root.empty())
      config->cache_path = root + "/debuginfod_client";
#endif
  }
  if (config->cache_path.empty())
    return base::ErrStatus(
        "cannot determine debuginfod cache directory; "
        "pass --debuginfod-cache-path");
  return base::OkStatus();
}

std::unique_ptr<Symbolizer> CreateDebuginfodSymbolizer(
    const DebuginfodConfig& config,
    bool progress,
    DebuginfodStats* stats) {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  if (!CanRunLlvmSymbolizer()) {
    stats->warnings =
        "Cannot run llvm-symbolizer; install it and ensure it "
        "is on PATH to use debuginfod.\n";
    return nullptr;
  }
  return std::make_unique<LocalSymbolizer>(
      std::make_unique<DebuginfodBinaryFinder>(config, progress, stats),
      /*use_kernel_paths=*/false);
#else
  (void)config;
  (void)progress;
  (void)stats;
  return nullptr;
#endif
}

}  // namespace perfetto::profiling
