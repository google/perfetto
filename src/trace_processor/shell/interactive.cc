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

#include "src/trace_processor/shell/interactive.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/query.h"
#include "src/trace_processor/shell/result_formatter.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_TP_LINENOISE)
#include <linenoise.h>
#include <pwd.h>
#include <sys/types.h>
#endif

namespace perfetto::trace_processor {

namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_TP_LINENOISE)

bool EnsureDir(const std::string& path) {
  return base::Mkdir(path) || errno == EEXIST;
}

bool EnsureFile(const std::string& path) {
  return base::OpenFile(path, O_RDONLY | O_CREAT, 0644).get() != -1;
}

std::string GetConfigPath() {
  const char* homedir = getenv("HOME");
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  if (homedir == nullptr)
    homedir = getpwuid(getuid())->pw_dir;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  if (homedir == nullptr)
    homedir = getenv("USERPROFILE");
#endif
  if (homedir == nullptr)
    return "";
  return std::string(homedir) + "/.config";
}

std::string GetPerfettoPath() {
  std::string config = GetConfigPath();
  if (config.empty())
    return "";
  return config + "/perfetto";
}

std::string GetHistoryPath() {
  std::string perfetto = GetPerfettoPath();
  if (perfetto.empty())
    return "";
  return perfetto + "/.trace_processor_shell_history";
}

void SetupLineEditor() {
  linenoiseSetMultiLine(true);
  linenoiseHistorySetMaxLen(1000);

  bool success = !GetHistoryPath().empty();
  success = success && EnsureDir(GetConfigPath());
  success = success && EnsureDir(GetPerfettoPath());
  success = success && EnsureFile(GetHistoryPath());
  success = success && linenoiseHistoryLoad(GetHistoryPath().c_str()) != -1;
  if (!success) {
    PERFETTO_PLOG("Could not load history from %s", GetHistoryPath().c_str());
  }
}

struct LineDeleter {
  void operator()(char* p) const {
    linenoiseHistoryAdd(p);
    linenoiseHistorySave(GetHistoryPath().c_str());
    linenoiseFree(p);
  }
};

using ScopedLine = std::unique_ptr<char, LineDeleter>;

ScopedLine GetLine(const char* prompt) {
  errno = 0;
  auto line = ScopedLine(linenoise(prompt));
  // linenoise returns a nullptr both for CTRL-C and CTRL-D, however in the
  // former case it sets errno to EAGAIN.
  // If the user press CTRL-C return "" instead of nullptr. We don't want the
  // main loop to quit in that case as that is inconsistent with the behavior
  // "CTRL-C interrupts the current query" and frustrating when hitting that
  // a split second after the query is done.
  if (!line && errno == EAGAIN)
    return ScopedLine(strdup(""));
  return line;
}

#else

void SetupLineEditor() {}

using ScopedLine = std::unique_ptr<char>;

ScopedLine GetLine(const char* prompt) {
  printf("\r%80s\r%s", "", prompt);
  fflush(stdout);
  ScopedLine line(new char[1024]);
  if (!fgets(line.get(), 1024 - 1, stdin))
    return nullptr;
  if (strlen(line.get()) > 0)
    line.get()[strlen(line.get()) - 1] = 0;
  return line;
}

#endif  // PERFETTO_TP_LINENOISE

// Rows per page: the terminal's height less room for the header and the
// prompt, or 32 if it isn't known.
uint64_t PageRows() {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  struct winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 8) {
    return size.ws_row - 4u;
  }
#endif
  return 32;
}

void RunAndPrint(TraceProcessor* trace_processor,
                 const std::string& sql,
                 bool quiet) {
  base::Status status = RunQueriesAndPrintResult(
      trace_processor, sql, DefaultTableOptions(PageRows()), stdout, quiet,
      nullptr);
  if (!status.ok()) {
    fprintf(stderr, "%s\n", status.c_message());
  }
}

// Commands which no longer work. They are left out of .help but still
// recognized, so that anyone using them is told what to do instead.
struct RetiredCommand {
  const char* name;
  const char* note;
};
constexpr RetiredCommand kRetiredCommands[] = {
    {"dump",
     "was removed: use `trace_processor export sqlite -o FILE TRACE_FILE` "
     "instead"},
    {"load-metrics-sql",
     "was removed with the deprecated v1 metrics workflow: see "
     "https://perfetto.dev/docs/analysis/metrics"},
    {"run-metrics",
     "was removed with the deprecated v1 metrics workflow: see "
     "https://perfetto.dev/docs/analysis/metrics"},
    {"width",
     "is deprecated and does nothing: columns are now sized to their "
     "contents"},
};

void PrintRetiredNote(const char* what, const char* note) {
  PERFETTO_ELOG(
      "%s %s. If this affects you, please file a bug at "
      "https://github.com/google/perfetto/issues",
      what, note);
}

// Prints the note for |command| if it is retired. Returns whether it was.
bool MaybePrintRetiredCommand(const char* command) {
  for (const auto& retired : kRetiredCommands) {
    if (strcmp(command, retired.name) == 0) {
      PrintRetiredNote(("." + std::string(retired.name)).c_str(), retired.note);
      return true;
    }
  }
  return false;
}

void PrintShellUsage() {
  PERFETTO_ELOG(R"(
Available commands:
.quit, .q         Exit the shell.
.help             This text.
.read FILE        Executes the queries in the FILE.
.reset            Destroys all tables/view created by the user.
)");
}

}  // namespace

shell::FlagSpec RetiredWideFlag() {
  return {"wide", 'W', false, nullptr, nullptr, [](const char*) {
            PrintRetiredNote("-W/--wide",
                             "is deprecated and does nothing: columns are now "
                             "sized to their contents");
          }};
}

base::Status StartInteractiveShell(TraceProcessor* trace_processor,
                                   const InteractiveOptions& options) {
  SetupLineEditor();

  for (;;) {
    ScopedLine line = GetLine("> ");
    if (!line)
      break;
    if (strcmp(line.get(), "") == 0) {
      if (!options.quiet)
        printf("If you want to quit either type .q or press CTRL-D (EOF)\n");
      continue;
    }
    if (line.get()[0] == '.') {
      char command[32] = {};
      char arg[1024] = {};
      sscanf(line.get() + 1, "%31s %1023s", command, arg);
      if (strcmp(command, "quit") == 0 || strcmp(command, "q") == 0) {
        break;
      }
      if (MaybePrintRetiredCommand(command)) {
        continue;
      }
      if (strcmp(command, "help") == 0) {
        PrintShellUsage();
      } else if (strcmp(command, "reset") == 0) {
        trace_processor->RestoreInitialTables();
      } else if (strcmp(command, "read") == 0 && strlen(arg)) {
        std::string sql;
        if (!base::ReadFile(arg, &sql)) {
          PERFETTO_ELOG("Unable to read file %s", arg);
          continue;
        }
        RunAndPrint(trace_processor, sql, options.quiet);
      } else {
        PrintShellUsage();
      }
      continue;
    }

    RunAndPrint(trace_processor, line.get(), options.quiet);
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
