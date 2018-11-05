/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <aio.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <iostream>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/time.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "perfetto/trace_processor/raw_query.pb.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#define PERFETTO_HAS_SIGNAL_H() 1
#else
#define PERFETTO_HAS_SIGNAL_H() 0
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#include <linenoise.h>
#include <pwd.h>
#include <sys/types.h>
#endif

#if PERFETTO_HAS_SIGNAL_H()
#include <signal.h>
#endif

namespace perfetto {
namespace trace_processor {

namespace {
TraceProcessor* g_tp;

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)

bool EnsureDir(const std::string& path) {
  return mkdir(path.c_str(), 0755) != -1 || errno == EEXIST;
}

bool EnsureFile(const std::string& path) {
  return base::OpenFile(path, O_RDONLY | O_CREAT, 0644).get() != -1;
}

std::string GetConfigPath() {
  const char* homedir = getenv("HOME");
  if (homedir == nullptr)
    homedir = getpwuid(getuid())->pw_dir;
  if (homedir == nullptr)
    return "";
  return std::string(homedir) + "/.config";
}

std::string GetPerfettoPath() {
  std::string config = GetConfigPath();
  if (config == "")
    return "";
  return config + "/perfetto";
}

std::string GetHistoryPath() {
  std::string perfetto = GetPerfettoPath();
  if (perfetto == "")
    return "";
  return perfetto + "/.trace_processor_shell_history";
}

void SetupLineEditor() {
  linenoiseSetMultiLine(true);
  linenoiseHistorySetMaxLen(1000);

  bool success = GetHistoryPath() != "";
  success = success && EnsureDir(GetConfigPath());
  success = success && EnsureDir(GetPerfettoPath());
  success = success && EnsureFile(GetHistoryPath());
  success = success && linenoiseHistoryLoad(GetHistoryPath().c_str()) != -1;
  if (!success) {
    PERFETTO_PLOG("Could not load history from %s", GetHistoryPath().c_str());
  }
}

void FreeLine(char* line) {
  linenoiseHistoryAdd(line);
  linenoiseHistorySave(GetHistoryPath().c_str());
  linenoiseFree(line);
}

char* GetLine(const char* prompt) {
  return linenoise(prompt);
}

#else

void SetupLineEditor() {}

void FreeLine(char* line) {
  delete[] line;
}

char* GetLine(const char* prompt) {
  printf("\r%80s\r%s", "", prompt);
  fflush(stdout);
  char* line = new char[1024];
  if (!fgets(line, 1024 - 1, stdin)) {
    FreeLine(line);
    return nullptr;
  }
  if (strlen(line) > 0)
    line[strlen(line) - 1] = 0;
  return line;
}

#endif

void PrintQueryResultInteractively(base::TimeNanos t_start,
                                   const protos::RawQueryResult& res) {
  if (res.has_error()) {
    PERFETTO_ELOG("SQLite error: %s", res.error().c_str());
    return;
  }
  PERFETTO_CHECK(res.columns_size() == res.column_descriptors_size());

  base::TimeNanos t_end = base::GetWallTimeNs();

  for (int r = 0; r < static_cast<int>(res.num_records()); r++) {
    if (r % 32 == 0) {
      if (r > 0) {
        fprintf(stderr, "...\nType 'q' to stop, Enter for more records: ");
        fflush(stderr);
        char input[32];
        if (!fgets(input, sizeof(input) - 1, stdin))
          exit(0);
        if (input[0] == 'q')
          break;
      }
      for (const auto& col : res.column_descriptors())
        printf("%20s ", col.name().c_str());
      printf("\n");

      for (int i = 0; i < res.columns_size(); i++)
        printf("%20s ", "--------------------");
      printf("\n");
    }

    for (int c = 0; c < res.columns_size(); c++) {
      switch (res.column_descriptors(c).type()) {
        case protos::RawQueryResult_ColumnDesc_Type_STRING:
          printf("%-20.20s", res.columns(c).string_values(r).c_str());
          break;
        case protos::RawQueryResult_ColumnDesc_Type_DOUBLE:
          printf("%20f", res.columns(c).double_values(r));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_LONG: {
          auto value = res.columns(c).long_values(r);
          printf((value < 0xffffffll) ? "%20lld" : "%20llx", value);
          break;
        }
      }
      printf(" ");
    }
    printf("\n");
  }
  printf("\nQuery executed in %.3f ms\n\n", (t_end - t_start).count() / 1E6);
}
int StartInteractiveShell() {
  SetupLineEditor();

  for (;;) {
    char* line = GetLine("> ");
    if (!line || strcmp(line, "q\n") == 0)
      break;
    if (strcmp(line, "") == 0)
      continue;
    protos::RawQueryArgs query;
    query.set_sql_query(line);
    base::TimeNanos t_start = base::GetWallTimeNs();
    g_tp->ExecuteQuery(query, [t_start](const protos::RawQueryResult& res) {
      PrintQueryResultInteractively(t_start, res);
    });

    FreeLine(line);
  }
  return 0;
}

void PrintQueryResultAsCsv(const protos::RawQueryResult& res, FILE* output) {
  PERFETTO_CHECK(res.columns_size() == res.column_descriptors_size());

  for (int r = 0; r < static_cast<int>(res.num_records()); r++) {
    if (r == 0) {
      for (int c = 0; c < res.column_descriptors_size(); c++) {
        const auto& col = res.column_descriptors(c);
        if (c > 0)
          fprintf(output, ",");
        fprintf(output, "\"%s\"", col.name().c_str());
      }
      fprintf(output, "\n");
    }

    for (int c = 0; c < res.columns_size(); c++) {
      if (c > 0)
        fprintf(output, ",");
      switch (res.column_descriptors(c).type()) {
        case protos::RawQueryResult_ColumnDesc_Type_STRING:
          fprintf(output, "\"%s\"", res.columns(c).string_values(r).c_str());
          break;
        case protos::RawQueryResult_ColumnDesc_Type_DOUBLE:
          fprintf(output, "%f", res.columns(c).double_values(r));
          break;
        case protos::RawQueryResult_ColumnDesc_Type_LONG: {
          auto value = res.columns(c).long_values(r);
          fprintf(output, "%lld", value);
          break;
        }
      }
    }
    printf("\n");
  }
}

int RunQueryAndPrintResult(FILE* input, FILE* output) {
  char buffer[4096];
  bool is_first_query = true;
  bool is_query_error = false;
  bool has_output_printed = false;
  while (!feof(input) && !ferror(input) && !is_query_error) {
    // Add an extra newline separator between query results.
    if (!is_first_query)
      fprintf(output, "\n");
    is_first_query = false;

    std::string sql_query;
    while (fgets(buffer, sizeof(buffer), input)) {
      if (strncmp(buffer, "\n", sizeof(buffer)) == 0)
        break;
      sql_query.append(buffer);
    }
    if (sql_query.back() == '\n')
      sql_query.resize(sql_query.size() - 1);
    PERFETTO_ILOG("Executing query: %s", sql_query.c_str());

    protos::RawQueryArgs query;
    query.set_sql_query(sql_query);
    g_tp->ExecuteQuery(query, [output, &is_query_error, &has_output_printed](
                                  const protos::RawQueryResult& res) {
      if (res.has_error()) {
        PERFETTO_ELOG("SQLite error: %s", res.error().c_str());
        is_query_error = true;
        return;
      } else if (res.num_records() != 0) {
        if (has_output_printed) {
          PERFETTO_ELOG(
              "More than one query generated result rows. This is "
              "unsupported.");
          is_query_error = true;
          return;
        }
        has_output_printed = true;
      }
      PrintQueryResultAsCsv(res, output);
    });
  }
  return is_query_error ? 1 : 0;
}

void PrintUsage(char** argv) {
  PERFETTO_ELOG("Usage: %s [-d] [-q query.sql] trace_file.pb", argv[0]);
}

int TraceProcessorMain(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv);
    return 1;
  }
  const char* trace_file_path = nullptr;
  const char* query_file_path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0) {
      EnableSQLiteVtableDebugging();
      continue;
    }
    if (strcmp(argv[i], "-q") == 0) {
      if (++i == argc) {
        PrintUsage(argv);
        return 1;
      }
      query_file_path = argv[i];
      continue;
    }
    trace_file_path = argv[i];
  }

  if (trace_file_path == nullptr) {
    PrintUsage(argv);
    return 1;
  }

  // Load the trace file into the trace processor.
  Config config;
  config.optimization_mode = OptimizationMode::kMaxBandwidth;
  std::unique_ptr<TraceProcessor> tp = TraceProcessor::CreateInstance(config);
  base::ScopedFile fd(base::OpenFile(trace_file_path, O_RDONLY));
  PERFETTO_CHECK(fd);

  // Load the trace in chunks using async IO. We create a simple pipeline where,
  // at each iteration, we parse the current chunk and asynchronously start
  // reading the next chunk.

  // 1MB chunk size seems the best tradeoff on a MacBook Pro 2013 - i7 2.8 GHz.
  constexpr size_t kChunkSize = 1024 * 1024;
  struct aiocb cb {};
  cb.aio_nbytes = kChunkSize;
  cb.aio_fildes = *fd;

  std::unique_ptr<uint8_t[]> aio_buf(new uint8_t[kChunkSize]);
  cb.aio_buf = aio_buf.get();

  PERFETTO_CHECK(aio_read(&cb) == 0);
  struct aiocb* aio_list[1] = {&cb};

  uint64_t file_size = 0;
  auto t_load_start = base::GetWallTimeMs();
  for (int i = 0;; i++) {
    if (i % 128 == 0)
      fprintf(stderr, "\rLoading trace: %.2f MB\r", file_size / 1E6);

    // Block waiting for the pending read to complete.
    PERFETTO_CHECK(aio_suspend(aio_list, 1, nullptr) == 0);
    auto rsize = aio_return(&cb);
    if (rsize <= 0)
      break;
    file_size += static_cast<uint64_t>(rsize);

    // Take ownership of the completed buffer and enqueue a new async read
    // with a fresh buffer.
    std::unique_ptr<uint8_t[]> buf(std::move(aio_buf));
    aio_buf.reset(new uint8_t[kChunkSize]);
    cb.aio_buf = aio_buf.get();
    cb.aio_offset += rsize;
    PERFETTO_CHECK(aio_read(&cb) == 0);

    // Parse the completed buffer while the async read is in-flight.
    tp->Parse(std::move(buf), static_cast<size_t>(rsize));
  }
  tp->NotifyEndOfFile();
  double t_load = (base::GetWallTimeMs() - t_load_start).count() / 1E3;
  double size_mb = file_size / 1E6;
  PERFETTO_ILOG("Trace loaded: %.2f MB (%.1f MB/s)", size_mb, size_mb / t_load);
  g_tp = tp.get();

#if PERFETTO_HAS_SIGNAL_H()
  signal(SIGINT, [](int) { g_tp->InterruptQuery(); });
#endif

  // If there is no query file, start a shell.
  if (query_file_path == nullptr) {
    return StartInteractiveShell();
  }

  // Otherwise run the queries and print the results.
  base::ScopedFstream file(fopen(query_file_path, "r"));
  return RunQueryAndPrintResult(file.get(), stdout);
}

}  // namespace

}  // namespace trace_processor
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::trace_processor::TraceProcessorMain(argc, argv);
}
