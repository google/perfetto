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

// memscope: records the memory of one Android app and opens it in the
// Memscope UI. See products/memscope/README.md.

#include <stdio.h>
#include <stdlib.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/http/http_server.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/utils.h"
#include "products/memscope/cli/report.h"
#include "products/memscope/configs/record_config.h"

namespace perfetto::memscope {
namespace {

constexpr char kDefaultUi[] = "https://memscope.perfetto.dev";
constexpr char kDeviceTracePath[] =
    "/data/misc/perfetto-traces/memscope.pftrace";
// The only port the UI's CSP allows fetching traces from.
constexpr int kHandoffPort = 9001;

struct Options {
  std::string command;
  std::string app;
  std::string serial;
  std::string output;
  std::string ui = kDefaultUi;
  uint32_t duration_ms = 30000;
  uint32_t java_dumps = 2;
  std::optional<int64_t> pid;
  bool open_browser = true;
  std::vector<std::string> positional;
};

void PrintUsage() {
  fprintf(stderr, R"(Usage: memscope <command> [options]

Commands:
  record --app <package>   Record the memory of one Android app, then open it.
  open <trace>             Open an existing trace in the Memscope UI.
  report <trace>           Print the memory summary of one process.
  doctor [--app <package>] Check the device and app can be profiled.

Options:
  --app <package>       The app to profile, e.g. com.example.myapp.
  --duration <N>[s|m]   How long to record for (default 30s).
  --java-dumps <N>      Java heap dumps to take, spread over the run (default 2).
  --serial <serial>     Device serial, when more than one is connected.
  -o <file>             Where to save the trace (default <package>.pftrace).
  --ui <url>            UI to open (default %s).
  --no-open             Print the URL instead of opening a browser.
  --pid <pid>           report: the process to summarise (default: --app, or
                        the process with the most memory data).
)",
          kDefaultUi);
}

std::optional<uint32_t> ParseDurationMs(const std::string& s) {
  if (s.empty())
    return std::nullopt;
  uint32_t mult = 1000;
  std::string num = s;
  if (base::EndsWith(s, "ms")) {
    mult = 1;
    num = s.substr(0, s.size() - 2);
  } else if (base::EndsWith(s, "s")) {
    num = s.substr(0, s.size() - 1);
  } else if (base::EndsWith(s, "m")) {
    mult = 60000;
    num = s.substr(0, s.size() - 1);
  }
  std::optional<uint32_t> v = base::StringToUInt32(num);
  if (!v || *v == 0)
    return std::nullopt;
  return *v * mult;
}

std::optional<Options> ParseArgs(int argc, char** argv) {
  if (argc < 2)
    return std::nullopt;
  Options opts;
  opts.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) {
        PERFETTO_ELOG("%s requires a value", arg.c_str());
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };
    if (arg == "--app" || arg == "--serial" || arg == "-o" || arg == "--ui" ||
        arg == "--duration" || arg == "--java-dumps" || arg == "--pid") {
      std::optional<std::string> v = next();
      if (!v)
        return std::nullopt;
      if (arg == "--app") {
        opts.app = *v;
      } else if (arg == "--serial") {
        opts.serial = *v;
      } else if (arg == "-o") {
        opts.output = *v;
      } else if (arg == "--ui") {
        opts.ui = *v;
      } else if (arg == "--duration") {
        std::optional<uint32_t> ms = ParseDurationMs(*v);
        if (!ms) {
          PERFETTO_ELOG("Invalid --duration: %s", v->c_str());
          return std::nullopt;
        }
        opts.duration_ms = *ms;
      } else if (arg == "--pid") {
        opts.pid = base::StringToInt64(*v);
        if (!opts.pid) {
          PERFETTO_ELOG("Invalid --pid: %s", v->c_str());
          return std::nullopt;
        }
      } else {
        std::optional<uint32_t> n = base::StringToUInt32(*v);
        if (!n) {
          PERFETTO_ELOG("Invalid --java-dumps: %s", v->c_str());
          return std::nullopt;
        }
        opts.java_dumps = *n;
      }
    } else if (arg == "--no-open") {
      opts.open_browser = false;
    } else if (arg == "-h" || arg == "--help") {
      return std::nullopt;
    } else if (!arg.empty() && arg[0] == '-') {
      PERFETTO_ELOG("Unknown option: %s", arg.c_str());
      return std::nullopt;
    } else {
      opts.positional.push_back(arg);
    }
  }
  return opts;
}

// Runs adb with |args|, feeding |input| on stdin. Returns stdout+stderr, or
// nullopt if adb could not be run or exited with an error.
std::optional<std::string> Adb(const Options& opts,
                               std::vector<std::string> args,
                               const std::string& input = "",
                               bool inherit_output = false,
                               bool log_errors = true) {
  const char* adb_env = getenv("ADB");
  base::Subprocess p({adb_env ? adb_env : "adb"});
  if (!opts.serial.empty()) {
    p.args.exec_cmd.push_back("-s");
    p.args.exec_cmd.push_back(opts.serial);
  }
  for (std::string& a : args)
    p.args.exec_cmd.push_back(std::move(a));
  p.args.input = input;
  if (!inherit_output) {
    p.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
    p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  }
  if (!p.Call() || p.returncode() != 0) {
    if (!inherit_output && log_errors)
      PERFETTO_ELOG("`%s` failed: %s", p.args.GetCmdString().c_str(),
                    base::TrimWhitespace(p.output()).c_str());
    return std::nullopt;
  }
  return p.output();
}

// Runs adb for a doctor check, where failure is reported by the check itself.
std::optional<std::string> AdbQuiet(const Options& opts,
                                    std::vector<std::string> args) {
  return Adb(opts, std::move(args), "", false, /*log_errors=*/false);
}

std::string GetProp(const Options& opts, const std::string& prop) {
  std::optional<std::string> out = AdbQuiet(opts, {"shell", "getprop", prop});
  return out ? base::TrimWhitespace(*out) : "";
}

// Removes the "# BEGIN <name>" ... "# END <name>" block from |cfg|.
void DropBlock(std::string* cfg, const std::string& name) {
  std::string begin = "# BEGIN " + name + "\n";
  std::string end = "# END " + name + "\n";
  size_t b = cfg->find(begin);
  size_t e = cfg->find(end);
  PERFETTO_CHECK(b != std::string::npos && e != std::string::npos && b < e);
  cfg->erase(b, e + end.size() - b);
}

// Fills the shared products/memscope/configs/record.pbtx template.
std::string BuildTraceConfig(const Options& opts) {
  std::string cfg(reinterpret_cast<const char*>(kRecordConfig.data()),
                  kRecordConfig.size());
  // Leave time at either end so the first dump sees the app settled and the
  // last one finishes before the session stops.
  const uint32_t phase_ms = 2000;
  if (opts.java_dumps == 0) {
    DropBlock(&cfg, "java_hprof");
  } else if (opts.java_dumps > 1 && opts.duration_ms > 2 * phase_ms) {
    uint32_t interval =
        (opts.duration_ms - 2 * phase_ms) / (opts.java_dumps - 1);
    cfg = base::ReplaceAll(cfg, "{{DUMP_PHASE_MS}}", std::to_string(phase_ms));
    cfg =
        base::ReplaceAll(cfg, "{{DUMP_INTERVAL_MS}}", std::to_string(interval));
  } else {
    DropBlock(&cfg, "java_hprof_continuous");
  }
  cfg = base::ReplaceAll(cfg, "{{APP}}", opts.app);
  return base::ReplaceAll(cfg, "{{DURATION_MS}}",
                          std::to_string(opts.duration_ms));
}

// Serves |path| once on 127.0.0.1:kHandoffPort to |origin|, then returns.
class OneShotHandoff : public base::HttpRequestHandler {
 public:
  OneShotHandoff(base::UnixTaskRunner* runner,
                 std::string name,
                 std::string data)
      : runner_(runner), name_(std::move(name)), data_(std::move(data)) {}

  uint8_t* OnHttpRequestBody(const base::HttpRequest&, size_t) override {
    return nullptr;
  }
  uint8_t* OnWebsocketPayload(base::HttpServerConnection*, size_t) override {
    return nullptr;
  }
  void OnHttpRequest(const base::HttpRequest& req) override {
    if (req.method.ToStdString() != "GET" ||
        req.uri.ToStdString() != "/" + name_) {
      req.conn->SendResponseAndClose("404 Not Found");
      return;
    }
    req.conn->SendResponse(
        "200 OK",
        {"Content-Type: application/octet-stream", "Cache-Control: no-cache"},
        base::StringView(data_), /*force_close=*/true);
    runner_->PostTask([this] { runner_->Quit(); });
  }

 private:
  base::UnixTaskRunner* runner_;
  std::string name_;
  std::string data_;
};

// Returns scheme://host[:port] of |url|.
std::string OriginOf(const std::string& url) {
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    return url;
  size_t path = url.find_first_of("/?#", scheme_end + 3);
  return path == std::string::npos ? url : url.substr(0, path);
}

int OpenInUi(const Options& opts, const std::string& path) {
  std::string data;
  if (!base::ReadFile(path, &data)) {
    PERFETTO_ELOG("Could not read %s", path.c_str());
    return 1;
  }
  std::string name = path.substr(path.find_last_of('/') + 1);
  std::string ui = opts.ui;
  if (ui.find('?') == std::string::npos && !base::EndsWith(ui, "/"))
    ui += "/";
  std::string url = ui +
                    "#!/?url=http://127.0.0.1:" + std::to_string(kHandoffPort) +
                    "/" + name + "&referrer=memscope";

  base::UnixTaskRunner runner;
  OneShotHandoff handler(&runner, name, std::move(data));
  base::HttpServer server(&runner, &handler);
  server.AddAllowedOrigin(OriginOf(opts.ui));
  server.SetQuiet(true);
  server.Start("127.0.0.1", kHandoffPort);

  if (getenv("SSH_CONNECTION")) {
    printf(
        "This looks like an SSH session. On the machine with your browser, "
        "run:\n  ssh -L %d:127.0.0.1:%d <this host>\nthen open:\n  %s\n",
        kHandoffPort, kHandoffPort, url.c_str());
  } else if (!opts.open_browser) {
    printf("Open this URL in your browser:\n  %s\n", url.c_str());
  } else {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
    base::Subprocess browser({"open", url});
#else
    base::Subprocess browser({"xdg-open", url});
#endif
    browser.args.stdout_mode = base::Subprocess::OutputMode::kDevNull;
    browser.args.stderr_mode = base::Subprocess::OutputMode::kDevNull;
    if (!browser.Call() || browser.returncode() != 0)
      printf("Open this URL in your browser:\n  %s\n", url.c_str());
  }
  printf("Waiting for the UI to load the trace (Ctrl-C to stop)...\n");
  fflush(stdout);
  runner.Run();
  return 0;
}

int Doctor(const Options& opts) {
  int problems = 0;
  auto check = [&](bool ok, const std::string& what, const std::string& fix) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) {
      printf("         %s\n", fix.c_str());
      ++problems;
    }
    return ok;
  };
  printf("memscope doctor\n");
  if (!check(AdbQuiet(opts, {"version"}).has_value(), "adb is installed",
             "Install the Android platform tools, or set $ADB."))
    return 1;
  if (!check(AdbQuiet(opts, {"get-state"}).has_value(), "A device is connected",
             "Connect a device with USB debugging on, or pass --serial."))
    return 1;
  std::optional<uint32_t> sdk =
      base::StringToUInt32(GetProp(opts, "ro.build.version.sdk"));
  check(sdk && *sdk >= 29, "Android 10 or newer",
        "Heap profiling needs Android 10 (API 29) or newer.");
  bool debuggable_build = GetProp(opts, "ro.debuggable") == "1";
  printf("  [info] %s build\n", debuggable_build ? "userdebug/eng" : "user");
  if (!opts.app.empty()) {
    std::optional<std::string> path =
        AdbQuiet(opts, {"shell", "pm", "path", opts.app});
    if (check(path.has_value() && !path->empty(), opts.app + " is installed",
              "Install the app, or check the package name.") &&
        !debuggable_build) {
      std::optional<std::string> dump =
          AdbQuiet(opts, {"shell", "dumpsys", "package", opts.app});
      bool profileable =
          dump && (dump->find("PROFILEABLE") != std::string::npos ||
                   dump->find("DEBUGGABLE") != std::string::npos);
      check(profileable, opts.app + " is profileable",
            "On user builds the app needs <profileable android:shell=\"true\"/>"
            " in its manifest, or android:debuggable=\"true\".");
    }
  }
  printf(problems ? "\n%d problem(s) found.\n" : "\nReady to record.\n",
         problems);
  return problems ? 1 : 0;
}

int Report(const Options& opts, const std::string& trace) {
  ReportArgs args;
  args.trace_path = trace;
  args.pid = opts.pid;
  args.app = opts.app;
  return PrintReport(args);
}

int Record(Options opts) {
  if (opts.app.empty()) {
    PERFETTO_ELOG("record needs --app <package>");
    return 1;
  }
  if (opts.output.empty())
    opts.output = opts.app + ".pftrace";

  printf("Recording %s for %u s. Use the app now.\n", opts.app.c_str(),
         opts.duration_ms / 1000);
  if (!Adb(opts,
           {"shell", "perfetto", "--txt", "-c", "-", "-o", kDeviceTracePath},
           BuildTraceConfig(opts), /*inherit_output=*/true)) {
    PERFETTO_ELOG("Recording failed. Run `memscope doctor --app %s`.",
                  opts.app.c_str());
    return 1;
  }
  if (!Adb(opts, {"pull", kDeviceTracePath, opts.output}))
    return 1;
  Adb(opts, {"shell", "rm", "-f", kDeviceTracePath});
  printf("Saved %s\n\n", opts.output.c_str());
  Report(opts, opts.output);
  printf("\n");
  return OpenInUi(opts, opts.output);
}

int Main(int argc, char** argv) {
  std::optional<Options> opts = ParseArgs(argc, argv);
  if (!opts) {
    PrintUsage();
    return 1;
  }
  if (opts->command == "record")
    return Record(std::move(*opts));
  if (opts->command == "doctor")
    return Doctor(*opts);
  if (opts->command == "open" || opts->command == "report") {
    if (opts->positional.size() != 1) {
      PrintUsage();
      return 1;
    }
    if (opts->command == "report")
      return Report(*opts, opts->positional[0]);
    return OpenInUi(*opts, opts->positional[0]);
  }
  PrintUsage();
  return 1;
}

}  // namespace
}  // namespace perfetto::memscope

int main(int argc, char** argv) {
  return perfetto::memscope::Main(argc, argv);
}
