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

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/atomic_file.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/progress_reporter.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"

namespace perfetto::profiling {
namespace {

std::string EnvironmentValue(const char* name) {
  const char* value = getenv(name);
  return value ? value : "";
}

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
// Positive whole seconds that fit the config field, whatever the width of the
// platform's unsigned long.
std::optional<uint32_t> ParseSeconds(const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    return std::nullopt;
  auto value = base::StringToUInt64(text);
  if (!value || !*value || *value > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return static_cast<uint32_t>(*value);
}

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

// curl reports connection, resolution, TLS and timeout failures with these
// exit codes; anything else means the server answered.
bool IsUnreachableExit(int code) {
  return code == 6 || code == 7 || code == 28 || code == 35;
}

// Strips curl's "curl: (N) " prefix so only the human-readable cause is kept.
std::string CurlMessage(const std::string& output) {
  std::string message = base::TrimWhitespace(output);
  if (base::StartsWith(message, "curl: (")) {
    size_t end = message.find(") ");
    if (end != std::string::npos)
      message = message.substr(end + 2);
  }
  return message;
}

// With --fail, curl exits 22 and names the HTTP status in its message.
std::optional<int> HttpStatus(const std::string& message) {
  const char kPrefix[] = "returned error: ";
  size_t pos = message.find(kPrefix);
  if (pos == std::string::npos)
    return std::nullopt;
  return base::StringToInt32(message.substr(pos + sizeof(kPrefix) - 1, 3));
}

class DebuginfodBinaryFinder : public BinaryFinder {
 public:
  DebuginfodBinaryFinder(const DebuginfodConfig& config, DebuginfodStats* stats)
      : config_(config), stats_(stats) {}

  BinaryLookupResult FindBinary(const std::string& abspath,
                                const std::string& build_id) override {
    auto [result_ptr, inserted] =
        results_.Insert(build_id, BinaryLookupResult{});
    if (!inserted)
      return *result_ptr;
    auto& result = *result_ptr;
    if (build_id.empty())
      return result;
    const std::string hex = base::ToHex(build_id);
    const std::string directory = config_.cache_path + "/" + hex;
    const std::string path = directory + "/debuginfo";
    BinaryPathError error;
    result.binary = FindBinaryFile(path, build_id, &error);
    if (result.binary) {
      ++stats_->cache_hits;
      result.attempts.push_back({path, BinaryPathError::kOk, "from cache"});
      return result;
    }
    if (!MakeCacheDirectory(directory)) {
      ++stats_->failed;
      result.attempts.push_back({directory, BinaryPathError::kDownloadFailed,
                                 "cannot create cache directory"});
      return result;
    }
    bool answered_only_not_found = true;
    for (const auto& server : config_.urls) {
      const std::string url = server + "/buildid/" + hex + "/debuginfo";
      base::AtomicFile output(path);
      auto status = output.Open();
      if (!status.ok()) {
        answered_only_not_found = false;
        result.attempts.push_back(
            {path, BinaryPathError::kDownloadFailed, status.message()});
        break;
      }
      auto& progress = base::ProgressReporter::GetInstance();
      progress.Update("Debuginfod: fetching " + base::Basename(abspath) +
                      " from " + server);
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
          answered_only_not_found = false;
          break;
        }
        std::string message = CurlMessage(curl.output());
        if (HttpStatus(message) == 404) {
          result.attempts.push_back(
              {server, BinaryPathError::kNotOnServer, {}});
          continue;
        }
        answered_only_not_found = false;
        if (IsUnreachableExit(curl.returncode())) {
          result.attempts.push_back(
              {server, BinaryPathError::kServerUnreachable, message});
          auto& servers = stats_->unreachable_servers;
          if (std::find(servers.begin(), servers.end(), server) ==
              servers.end())
            servers.push_back(server);
          continue;
        }
        result.attempts.push_back(
            {server, BinaryPathError::kDownloadFailed, message});
        continue;
      }
      result.binary = FindBinaryFile(output.temp_path(), build_id, &error);
      if (!result.binary) {
        answered_only_not_found = false;
        result.attempts.push_back({server, error, "invalid debug file"});
        continue;
      }
      status = std::move(output).Commit();
      if (!status.ok()) {
        answered_only_not_found = false;
        result.binary.reset();
        result.attempts.push_back(
            {path, BinaryPathError::kDownloadFailed, status.message()});
        break;
      }
      result.binary->file_name = path;
      result.attempts.push_back(
          {path, BinaryPathError::kOk, "downloaded from " + server});
      ++stats_->downloads;
      return result;
    }
    if (answered_only_not_found)
      ++stats_->not_found;
    else
      ++stats_->failed;
    return result;
  }

 private:
  DebuginfodConfig config_;
  DebuginfodStats* stats_;
  base::FlatHashMapV2<std::string, BinaryLookupResult> results_;
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
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  // Servers are separated by any whitespace, as debuginfod clients expect.
  std::string list = options.urls.value_or(env_urls);
  std::replace_if(
      list.begin(), list.end(),
      [](char c) { return isspace(static_cast<unsigned char>(c)); }, ' ');
  for (base::StringSplitter urls(std::move(list), ' '); urls.Next();) {
    std::string url = urls.cur_token();
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
  auto connect = ParseSeconds(options.connect_timeout);
  auto stall = ParseSeconds(options.stall_timeout);
  if (!connect || !stall)
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
#else
  base::ignore_result(config);
  return base::ErrStatus(
      "this build does not support debuginfod symbolization");
#endif
}

std::unique_ptr<Symbolizer> CreateDebuginfodSymbolizer(
    const DebuginfodConfig& config,
    DebuginfodStats* stats) {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  if (!CanRunLlvmSymbolizer()) {
    stats->warnings =
        "Cannot run llvm-symbolizer; install it and ensure it "
        "is on PATH to use debuginfod.\n";
    return nullptr;
  }
  return std::make_unique<LocalSymbolizer>(
      std::make_unique<DebuginfodBinaryFinder>(config, stats),
      /*use_kernel_paths=*/false);
#else
  base::ignore_result(config);
  base::ignore_result(stats);
  return nullptr;
#endif
}

}  // namespace perfetto::profiling
