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

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>

#include <functional>
#include <iostream>
#include <vector>

#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/time.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#define PERFETTO_HAS_SIGNAL_H() 1
#else
#define PERFETTO_HAS_SIGNAL_H() 0
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#define PERFETTO_HAS_AIO_H() 1
#else
#define PERFETTO_HAS_AIO_H() 0
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#include <linenoise.h>
#include <pwd.h>
#include <sys/types.h>
#include "perfetto_version.gen.h"
#else
#define PERFETTO_GET_GIT_REVISION() "unknown"
#endif

#if PERFETTO_HAS_SIGNAL_H()
#include <signal.h>
#endif

#if PERFETTO_HAS_AIO_H()
#include <aio.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define ftruncate _chsize
#else
#include <dirent.h>
#include <getopt.h>
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

bool PrintStats() {
  auto it = g_tp->ExecuteQuery(
      "SELECT name, idx, source, value from stats "
      "where severity IN ('error', 'data_loss') and value > 0");

  bool first = true;
  for (uint32_t rows = 0; it.Next(); rows++) {
    if (first) {
      fprintf(stderr, "Error stats for this trace:\n");

      for (uint32_t i = 0; i < it.ColumnCount(); i++)
        fprintf(stderr, "%40s ", it.GetColumName(i).c_str());
      fprintf(stderr, "\n");

      for (uint32_t i = 0; i < it.ColumnCount(); i++)
        fprintf(stderr, "%40s ", "----------------------------------------");
      fprintf(stderr, "\n");

      first = false;
    }

    for (uint32_t c = 0; c < it.ColumnCount(); c++) {
      auto value = it.Get(c);
      switch (value.type) {
        case SqlValue::Type::kNull:
          fprintf(stderr, "%-40.40s", "[NULL]");
          break;
        case SqlValue::Type::kDouble:
          fprintf(stderr, "%40f", value.double_value);
          break;
        case SqlValue::Type::kLong:
          fprintf(stderr, "%40" PRIi64, value.long_value);
          break;
        case SqlValue::Type::kString:
          fprintf(stderr, "%-40.40s", value.string_value);
          break;
        case SqlValue::Type::kBytes:
          printf("%-40.40s", "<raw bytes>");
          break;
      }
      fprintf(stderr, " ");
    }
    fprintf(stderr, "\n");
  }

  util::Status status = it.Status();
  if (!status.ok()) {
    PERFETTO_ELOG("Error while iterating stats %s", status.c_message());
    return false;
  }
  return true;
}

int ExportTraceToDatabase(const std::string& output_name) {
  PERFETTO_CHECK(output_name.find("'") == std::string::npos);
  {
    base::ScopedFile fd(base::OpenFile(output_name, O_CREAT | O_RDWR, 0600));
    if (!fd) {
      PERFETTO_PLOG("Failed to create file: %s", output_name.c_str());
      return 1;
    }
    int res = ftruncate(fd.get(), 0);
    PERFETTO_CHECK(res == 0);
  }

  std::string attach_sql =
      "ATTACH DATABASE '" + output_name + "' AS perfetto_export";
  auto attach_it = g_tp->ExecuteQuery(attach_sql);
  bool attach_has_more = attach_it.Next();
  PERFETTO_DCHECK(!attach_has_more);

  util::Status status = attach_it.Status();
  if (!status.ok()) {
    PERFETTO_ELOG("SQLite error: %s", status.c_message());
    return 1;
  }

  auto tables_it = g_tp->ExecuteQuery(
      "SELECT name FROM perfetto_tables UNION "
      "SELECT name FROM sqlite_master WHERE type='table'");
  for (uint32_t rows = 0; tables_it.Next(); rows++) {
    std::string table_name = tables_it.Get(0).string_value;
    PERFETTO_CHECK(table_name.find("'") == std::string::npos);
    std::string export_sql = "CREATE TABLE perfetto_export." + table_name +
                             " AS SELECT * FROM " + table_name;

    auto export_it = g_tp->ExecuteQuery(export_sql);
    bool export_has_more = export_it.Next();
    PERFETTO_DCHECK(!export_has_more);

    status = export_it.Status();
    if (!status.ok()) {
      PERFETTO_ELOG("SQLite error: %s", status.c_message());
      return 1;
    }
  }
  status = tables_it.Status();
  if (!status.ok()) {
    PERFETTO_ELOG("SQLite error: %s", status.c_message());
    return 1;
  }

  auto detach_it = g_tp->ExecuteQuery("DETACH DATABASE perfetto_export");
  bool detach_has_more = attach_it.Next();
  PERFETTO_DCHECK(!detach_has_more);
  status = detach_it.Status();
  if (!status.ok()) {
    PERFETTO_ELOG("SQLite error: %s", status.c_message());
    return 1;
  }
  return 0;
}

class ErrorPrinter : public google::protobuf::io::ErrorCollector {
  void AddError(int line, int col, const std::string& msg) override {
    PERFETTO_ELOG("%d:%d: %s", line, col, msg.c_str());
  }

  void AddWarning(int line, int col, const std::string& msg) override {
    PERFETTO_ILOG("%d:%d: %s", line, col, msg.c_str());
  }
};

util::Status RegisterMetric(const std::string& register_metric) {
  std::string sql;
  base::ReadFile(register_metric, &sql);

  std::string path = "shell/";
  auto slash_idx = register_metric.rfind('/');
  path += slash_idx == std::string::npos
              ? register_metric
              : register_metric.substr(slash_idx + 1);

  return g_tp->RegisterMetric(path, sql);
}

util::Status ExtendMetricsProto(const std::string& extend_metrics_proto,
                                google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorSet desc_set;

  base::ScopedFile file(base::OpenFile(extend_metrics_proto, O_RDONLY));
  if (file.get() == -1) {
    return util::ErrStatus("Failed to open proto file %s",
                           extend_metrics_proto.c_str());
  }

  google::protobuf::io::FileInputStream stream(file.get());
  ErrorPrinter printer;
  google::protobuf::io::Tokenizer tokenizer(&stream, &printer);

  auto* proto = desc_set.add_file();
  google::protobuf::compiler::Parser parser;
  parser.Parse(&tokenizer, proto);

  auto basename_idx = extend_metrics_proto.rfind('/');
  auto basename = basename_idx == std::string::npos
                      ? extend_metrics_proto
                      : extend_metrics_proto.substr(basename_idx + 1);
  proto->set_name(basename);
  pool->BuildFile(*proto);

  std::vector<uint8_t> metric_proto;
  metric_proto.resize(static_cast<size_t>(desc_set.ByteSize()));
  desc_set.SerializeToArray(metric_proto.data(),
                            static_cast<int>(metric_proto.size()));

  return g_tp->ExtendMetricsProto(metric_proto.data(), metric_proto.size());
}

int RunMetrics(const std::vector<std::string>& metric_names,
               bool metrics_textproto,
               const google::protobuf::DescriptorPool& pool) {
  std::vector<uint8_t> metric_result;
  util::Status status = g_tp->ComputeMetric(metric_names, &metric_result);
  if (!status.ok()) {
    PERFETTO_ELOG("Error when computing metrics: %s", status.c_message());
    return 1;
  }
  if (metrics_textproto) {
    google::protobuf::DynamicMessageFactory factory(&pool);
    auto* descriptor =
        pool.FindMessageTypeByName("perfetto.protos.TraceMetrics");
    std::unique_ptr<google::protobuf::Message> metrics(
        factory.GetPrototype(descriptor)->New());
    metrics->ParseFromArray(metric_result.data(),
                            static_cast<int>(metric_result.size()));
    std::string out;
    google::protobuf::TextFormat::PrintToString(*metrics, &out);
    fwrite(out.c_str(), sizeof(char), out.size(), stdout);
  } else {
    fwrite(metric_result.data(), sizeof(uint8_t), metric_result.size(), stdout);
  }
  return 0;
}

void PrintQueryResultInteractively(TraceProcessor::Iterator* it,
                                   base::TimeNanos t_start) {
  base::TimeNanos t_end = t_start;
  for (uint32_t rows = 0; it->Next(); rows++) {
    if (rows % 32 == 0) {
      if (rows > 0) {
        fprintf(stderr, "...\nType 'q' to stop, Enter for more records: ");
        fflush(stderr);
        char input[32];
        if (!fgets(input, sizeof(input) - 1, stdin))
          exit(0);
        if (input[0] == 'q')
          break;
      } else {
        t_end = base::GetWallTimeNs();
      }
      for (uint32_t i = 0; i < it->ColumnCount(); i++)
        printf("%20s ", it->GetColumName(i).c_str());
      printf("\n");

      for (uint32_t i = 0; i < it->ColumnCount(); i++)
        printf("%20s ", "--------------------");
      printf("\n");
    }

    for (uint32_t c = 0; c < it->ColumnCount(); c++) {
      auto value = it->Get(c);
      switch (value.type) {
        case SqlValue::Type::kNull:
          printf("%-20.20s", "[NULL]");
          break;
        case SqlValue::Type::kDouble:
          printf("%20f", value.double_value);
          break;
        case SqlValue::Type::kLong:
          printf("%20" PRIi64, value.long_value);
          break;
        case SqlValue::Type::kString:
          printf("%-20.20s", value.string_value);
          break;
        case SqlValue::Type::kBytes:
          printf("%-20.20s", "<raw bytes>");
          break;
      }
      printf(" ");
    }
    printf("\n");
  }

  util::Status status = it->Status();
  if (!status.ok()) {
    PERFETTO_ELOG("SQLite error: %s", status.c_message());
  }
  printf("\nQuery executed in %.3f ms\n\n", (t_end - t_start).count() / 1E6);
}

void PrintShellUsage() {
  PERFETTO_ELOG(
      "Available commands:\n"
      ".quit, .q    Exit the shell.\n"
      ".help        This text.\n"
      ".dump FILE   Export the trace as a sqlite database.\n");
}

int StartInteractiveShell() {
  SetupLineEditor();

  for (;;) {
    char* line = GetLine("> ");
    if (!line)
      break;
    if (strcmp(line, "") == 0)
      continue;
    if (line[0] == '.') {
      char command[32] = {};
      char arg[1024] = {};
      sscanf(line + 1, "%31s %1023s", command, arg);
      if (strcmp(command, "quit") == 0 || strcmp(command, "q") == 0) {
        break;
      } else if (strcmp(command, "help") == 0) {
        PrintShellUsage();
      } else if (strcmp(command, "dump") == 0 && strlen(arg)) {
        if (ExportTraceToDatabase(arg) != 0)
          PERFETTO_ELOG("Database export failed");
      } else {
        PrintShellUsage();
      }
      continue;
    }

    base::TimeNanos t_start = base::GetWallTimeNs();
    auto it = g_tp->ExecuteQuery(line);
    PrintQueryResultInteractively(&it, t_start);

    FreeLine(line);
  }
  return 0;
}

void PrintQueryResultAsCsv(TraceProcessor::Iterator* it, FILE* output) {
  for (uint32_t c = 0; c < it->ColumnCount(); c++) {
    if (c > 0)
      fprintf(output, ",");
    fprintf(output, "\"%s\"", it->GetColumName(c).c_str());
  }
  fprintf(output, "\n");

  for (uint32_t rows = 0; it->Next(); rows++) {
    for (uint32_t c = 0; c < it->ColumnCount(); c++) {
      if (c > 0)
        fprintf(output, ",");

      auto value = it->Get(c);
      switch (value.type) {
        case SqlValue::Type::kNull:
          fprintf(output, "\"%s\"", "[NULL]");
          break;
        case SqlValue::Type::kDouble:
          fprintf(output, "%f", value.double_value);
          break;
        case SqlValue::Type::kLong:
          fprintf(output, "%" PRIi64, value.long_value);
          break;
        case SqlValue::Type::kString:
          fprintf(output, "\"%s\"", value.string_value);
          break;
        case SqlValue::Type::kBytes:
          fprintf(output, "\"%s\"", "<raw bytes>");
          break;
      }
    }
    fprintf(output, "\n");
  }
}

bool IsBlankLine(char* buffer) {
  size_t buf_size = strlen(buffer);
  for (size_t i = 0; i < buf_size; ++i) {
    // We can index into buffer[i+1], because strlen does not include the
    // trailing \0, so even if \r is the last character, this is not out
    // of bound.
    if (buffer[i] == '\r') {
      if (buffer[i + 1] != '\n')
        return false;
    } else if (buffer[i] != ' ' && buffer[i] != '\t' && buffer[i] != '\n') {
      return false;
    }
  }
  return true;
}

bool LoadQueries(FILE* input, std::vector<std::string>* output) {
  char buffer[4096];
  while (!feof(input) && !ferror(input)) {
    std::string sql_query;
    while (fgets(buffer, sizeof(buffer), input)) {
      if (IsBlankLine(buffer))
        break;
      sql_query.append(buffer);
    }
    if (sql_query.back() == '\n')
      sql_query.resize(sql_query.size() - 1);

    // If we have a new line at the end of the file or an extra new line
    // somewhere in the file, we'll end up with an empty query which we should
    // just ignore.
    if (sql_query.empty())
      continue;

    output->push_back(sql_query);
  }
  if (ferror(input)) {
    PERFETTO_ELOG("Error reading query file");
    return false;
  }
  return true;
}

bool RunQueryAndPrintResult(const std::vector<std::string> queries,
                            FILE* output) {
  bool is_first_query = true;
  bool is_query_error = false;
  bool has_output = false;
  for (const auto& sql_query : queries) {
    // Add an extra newline separator between query results.
    if (!is_first_query)
      fprintf(output, "\n");
    is_first_query = false;

    PERFETTO_ILOG("Executing query: %s", sql_query.c_str());

    auto it = g_tp->ExecuteQuery(sql_query);
    util::Status status = it.Status();
    if (!status.ok()) {
      PERFETTO_ELOG("SQLite error: %s", status.c_message());
      is_query_error = true;
      break;
    }
    if (it.ColumnCount() == 0) {
      bool it_has_more = it.Next();
      PERFETTO_DCHECK(!it_has_more);
      continue;
    }

    if (has_output) {
      PERFETTO_ELOG(
          "More than one query generated result rows. This is "
          "unsupported.");
      is_query_error = true;
      break;
    }
    PrintQueryResultAsCsv(&it, output);
    has_output = true;
  }
  return !is_query_error;
}

int MaybePrintPerfFile(const std::string& perf_file_path,
                       base::TimeNanos t_load,
                       base::TimeNanos t_run) {
  if (perf_file_path.empty())
    return 0;

  char buf[128];
  int count = snprintf(buf, sizeof(buf), "%" PRId64 ",%" PRId64,
                       static_cast<int64_t>(t_load.count()),
                       static_cast<int64_t>(t_run.count()));
  if (count < 0) {
    PERFETTO_ELOG("Failed to write perf data");
    return 1;
  }

  auto fd(base::OpenFile(perf_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666));
  if (!fd) {
    PERFETTO_ELOG("Failed to open perf file");
    return 1;
  }
  base::WriteAll(fd.get(), buf, static_cast<size_t>(count));
  return 0;
}

struct CommandLineOptions {
  std::string perf_file_path;
  std::string query_file_path;
  std::string sqlite_file_path;
  std::string metric_names;
  std::string metric_output;
  std::string metric_extra;
  std::string trace_file_path;
  bool launch_shell = false;
};

#if PERFETTO_HAS_AIO_H()
uint64_t ReadTrace(TraceProcessor* tp, int file_descriptor) {
  // Load the trace in chunks using async IO. We create a simple pipeline where,
  // at each iteration, we parse the current chunk and asynchronously start
  // reading the next chunk.

  // 1MB chunk size seems the best tradeoff on a MacBook Pro 2013 - i7 2.8 GHz.
  constexpr size_t kChunkSize = 1024 * 1024;
  uint64_t file_size = 0;

  struct aiocb cb {};
  cb.aio_nbytes = kChunkSize;
  cb.aio_fildes = file_descriptor;

  std::unique_ptr<uint8_t[]> aio_buf(new uint8_t[kChunkSize]);
#if defined(MEMORY_SANITIZER)
  // Just initialize the memory to make the memory sanitizer happy as it
  // cannot track aio calls below.
  memset(aio_buf.get(), 0, kChunkSize);
#endif  // defined(MEMORY_SANITIZER)
  cb.aio_buf = aio_buf.get();

  PERFETTO_CHECK(aio_read(&cb) == 0);
  struct aiocb* aio_list[1] = {&cb};

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
#if defined(MEMORY_SANITIZER)
    // Just initialize the memory to make the memory sanitizer happy as it
    // cannot track aio calls below.
    memset(aio_buf.get(), 0, kChunkSize);
#endif  // defined(MEMORY_SANITIZER)
    cb.aio_buf = aio_buf.get();
    cb.aio_offset += rsize;
    PERFETTO_CHECK(aio_read(&cb) == 0);

    // Parse the completed buffer while the async read is in-flight.
    util::Status status = tp->Parse(std::move(buf), static_cast<size_t>(rsize));
    if (PERFETTO_UNLIKELY(!status.ok())) {
      PERFETTO_ELOG("Fatal error while parsing trace: %s", status.c_message());
      exit(1);
    }
  }
  tp->NotifyEndOfFile();
  return file_size;
}
#else   // PERFETTO_HAS_AIO_H()
uint64_t ReadTrace(TraceProcessor* tp, int file_descriptor) {
  // Load the trace in chunks using ordinary read().
  // This version is used on Windows, since there's no aio library.

  constexpr size_t kChunkSize = 1024 * 1024;
  uint64_t file_size = 0;

  for (int i = 0;; i++) {
    if (i % 128 == 0)
      fprintf(stderr, "\rLoading trace: %.2f MB\r", file_size / 1E6);

    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunkSize]);
    auto rsize = read(file_descriptor, buf.get(), kChunkSize);
    if (rsize <= 0)
      break;
    file_size += static_cast<uint64_t>(rsize);

    util::Status status = tp->Parse(std::move(buf), static_cast<size_t>(rsize));
    if (PERFETTO_UNLIKELY(!status.ok())) {
      PERFETTO_ELOG("Fatal error while parsing trace: %s", status.c_message());
      exit(1);
    }
  }
  tp->NotifyEndOfFile();
  return file_size;
}
#endif  // PERFETTO_HAS_AIO_H()

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
void PrintUsage(char** argv) {
  PERFETTO_ELOG(
      "Interactive trace processor shell.\n"
      "Usage: %s [-q query_file] trace_file.pb",
      argv[0]);
}

CommandLineOptions ParseCommandLineOptions(int argc, char** argv) {
  CommandLineOptions command_line_options;

  if (argc == 2) {
    command_line_options.trace_file_path = argv[1];
  } else if (argc == 4) {
    if (strcmp(argv[1], "-q") != 0) {
      PrintUsage(argv);
      exit(1);
    }
    command_line_options.query_file_path = argv[2];
    command_line_options.trace_file_path = argv[3];
  } else {
    PrintUsage(argv);
    exit(1);
  }

  return command_line_options;
}

util::Status RegisterExtraMetrics(const std::string&, const std::string&) {
  return util::ErrStatus("RegisterExtraMetrics not implemented on Windows");
}

#else  // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

void PrintUsage(char** argv) {
  PERFETTO_ELOG(R"(
Interactive trace processor shell.
Usage: %s [OPTIONS] trace_file.pb

Options:
 -h, --help                      Prints this guide.
 -v, --version                   Prints the version of trace processor.
 -d, --debug                     Enable virtual table debugging.
 -p, --perf-file FILE            Writes the time taken to ingest the trace and
                                 execute the queries to the given file. Only
                                 valid with -q or --run-metrics and the file
                                 will only be written if the execution
                                 is successful.
 -q, --query-file FILE           Read and execute an SQL query from a file.
 -i, --interactive               Starts interactive mode even after a query file
                                 is specified with -q or --run-metrics.
 -e, --export FILE               Export the trace into a SQLite database.
 --run-metrics x,y,z             Runs a comma separated list of metrics and
                                 prints the result as a TraceMetrics proto to
                                 stdout. The specified can either be in-built
                                 metrics or SQL/proto files of extension
                                 metrics.
 --metrics-output=[binary|text]  Allows the output of --run-metrics to be
                                 specified in either proto binary or proto
                                 text format (default: text).
 --extra-metrics PATH            Registers all SQL files at the given path to
                                 the trace processor and extends the builtin
                                 metrics proto with $PATH/metrics-ext.proto.)",
                argv[0]);
}

CommandLineOptions ParseCommandLineOptions(int argc, char** argv) {
  CommandLineOptions command_line_options;
  enum LongOption {
    OPT_RUN_METRICS = 1000,
    OPT_METRICS_OUTPUT,
    OPT_EXTRA_METRICS,
  };

  static const struct option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      {"interactive", no_argument, nullptr, 'i'},
      {"debug", no_argument, nullptr, 'd'},
      {"perf-file", required_argument, nullptr, 'p'},
      {"query-file", required_argument, nullptr, 'q'},
      {"export", required_argument, nullptr, 'e'},
      {"run-metrics", required_argument, nullptr, OPT_RUN_METRICS},
      {"metrics-output", required_argument, nullptr, OPT_METRICS_OUTPUT},
      {"extra-metrics", required_argument, nullptr, OPT_EXTRA_METRICS},
      {nullptr, 0, nullptr, 0}};

  bool explicit_interactive = false;
  int option_index = 0;
  for (;;) {
    int option =
        getopt_long(argc, argv, "hvidp:q:e:", long_options, &option_index);

    if (option == -1)
      break;  // EOF.

    if (option == 'v') {
      printf("%s\n", PERFETTO_GET_GIT_REVISION());
      exit(0);
    }

    if (option == 'i') {
      explicit_interactive = true;
      continue;
    }

    if (option == 'd') {
      EnableSQLiteVtableDebugging();
      continue;
    }

    if (option == 'p') {
      command_line_options.perf_file_path = optarg;
      continue;
    }

    if (option == 'q') {
      command_line_options.query_file_path = optarg;
      continue;
    }

    if (option == 'e') {
      command_line_options.sqlite_file_path = optarg;
      continue;
    }

    if (option == OPT_RUN_METRICS) {
      command_line_options.metric_names = optarg;
      continue;
    }

    if (option == OPT_METRICS_OUTPUT) {
      command_line_options.metric_output = optarg;
      continue;
    }

    if (option == OPT_EXTRA_METRICS) {
      command_line_options.metric_extra = optarg;
      continue;
    }

    PrintUsage(argv);
    exit(option == 'h' ? 0 : 1);
  }

  command_line_options.launch_shell =
      explicit_interactive || (command_line_options.metric_names.empty() &&
                               command_line_options.query_file_path.empty());

  // Only allow non-interactive queries to emit perf data.
  if (!command_line_options.perf_file_path.empty() &&
      command_line_options.launch_shell) {
    PrintUsage(argv);
    exit(1);
  }

  // Ensure that we have the tracefile argument only at the end.
  if (optind != argc - 1 || argv[optind] == nullptr) {
    PrintUsage(argv);
    exit(1);
  }

  command_line_options.trace_file_path = argv[optind];

  return command_line_options;
}

util::Status RegisterExtraMetric(const std::string& parent_path,
                                 const std::string& path) {
  // Silently ignore any non-SQL files.
  if (path.find(".sql") == std::string::npos)
    return util::OkStatus();

  std::string sql;
  base::ReadFile(parent_path + "/" + path, &sql);
  return g_tp->RegisterMetric(path, sql);
}

util::Status RegisterExtraMetrics(const std::string& path,
                                  const std::string& group) {
  std::string full_path = path + "/" + group;
  DIR* dir = opendir(full_path.c_str());
  if (dir == nullptr) {
    return util::ErrStatus(
        "Failed to open directory %s to register extra metrics",
        full_path.c_str());
  }

  for (auto* dirent = readdir(dir); dirent != nullptr; dirent = readdir(dir)) {
    util::Status status = util::OkStatus();
    if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
      continue;

    if (dirent->d_type == DT_DIR) {
      status = RegisterExtraMetrics(path, group + dirent->d_name + "/");
    } else if (dirent->d_type == DT_REG) {
      status = RegisterExtraMetric(path, group + dirent->d_name);
    }
    if (!status.ok())
      return status;
  }
  return util::OkStatus();
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

int TraceProcessorMain(int argc, char** argv) {
  CommandLineOptions options = ParseCommandLineOptions(argc, argv);

  // Load the trace file into the trace processor.
  Config config;
  std::unique_ptr<TraceProcessor> tp = TraceProcessor::CreateInstance(config);
  base::ScopedFile fd(base::OpenFile(options.trace_file_path, O_RDONLY));
  if (!fd) {
    PERFETTO_ELOG("Could not open trace file (path: %s)",
                  options.trace_file_path.c_str());
    return 1;
  }

  auto t_load_start = base::GetWallTimeNs();
  uint64_t file_size = ReadTrace(tp.get(), *fd);
  auto t_load = base::GetWallTimeNs() - t_load_start;
  double t_load_s = t_load.count() / 1E9;
  double size_mb = file_size / 1E6;
  PERFETTO_ILOG("Trace loaded: %.2f MB (%.1f MB/s)", size_mb,
                size_mb / t_load_s);
  g_tp = tp.get();

#if PERFETTO_HAS_SIGNAL_H()
  signal(SIGINT, [](int) { g_tp->InterruptQuery(); });
#endif

  // Print out the stats to stderr for the trace.
  if (!PrintStats()) {
    return 1;
  }

  auto t_run_start = base::GetWallTimeNs();

  // Descriptor pool used for printing output as textproto.
  google::protobuf::DescriptorPool pool;
  google::protobuf::FileDescriptorSet root_desc_set;
  root_desc_set.ParseFromArray(kMetricsDescriptor.data(),
                               kMetricsDescriptor.size());
  for (const auto& desc : root_desc_set.file()) {
    pool.BuildFile(desc);
  }

  if (!options.metric_extra.empty()) {
    util::Status status = RegisterExtraMetrics(options.metric_extra, "");
    if (!status.ok()) {
      PERFETTO_ELOG("Failed to register extra metrics: %s", status.c_message());
      return 1;
    }

    auto ext_proto = options.metric_extra + "/metrics-ext.proto";
    // Check if the file exists
    base::ScopedFile file(base::OpenFile(ext_proto, O_RDONLY));
    if (file.get() != -1) {
      status = ExtendMetricsProto(ext_proto, &pool);
      if (!status.ok()) {
        PERFETTO_ELOG("Failed to extend metrics proto: %s", status.c_message());
        return 1;
      }
    }
  }

  if (!options.metric_names.empty()) {
    std::vector<std::string> metrics;
    for (base::StringSplitter ss(options.metric_names, ','); ss.Next();) {
      metrics.emplace_back(ss.cur_token());
    }

    // For all metrics which are files, register them and extend the metrics
    // proto.
    for (size_t i = 0; i < metrics.size(); ++i) {
      const std::string& metric_or_path = metrics[i];

      // If there is no extension, we assume it is a builtin metric.
      auto ext_idx = metric_or_path.rfind(".");
      if (ext_idx == std::string::npos)
        continue;

      std::string no_ext_name = metric_or_path.substr(0, ext_idx);
      util::Status status = RegisterMetric(no_ext_name + ".sql");
      if (!status.ok()) {
        PERFETTO_ELOG("Unable to register metric %s: %s",
                      metric_or_path.c_str(), status.c_message());
        return 1;
      }

      status = ExtendMetricsProto(no_ext_name + ".proto", &pool);
      if (!status.ok()) {
        PERFETTO_ELOG("Unable to extend metrics proto %s: %s",
                      metric_or_path.c_str(), status.c_message());
        return 1;
      }

      auto slash_idx = no_ext_name.rfind('/');
      std::string basename = slash_idx == std::string::npos
                                 ? no_ext_name
                                 : no_ext_name.substr(slash_idx + 1);
      metrics[i] = basename;
    }

    bool metrics_textproto = options.metric_output != "binary";
    int ret = RunMetrics(std::move(metrics), metrics_textproto, pool);
    if (!ret) {
      auto t_query = base::GetWallTimeNs() - t_run_start;
      ret = MaybePrintPerfFile(options.perf_file_path, t_load, t_query);
    }
    if (ret)
      return ret;
  } else {
    // If we were given a query file, load contents
    std::vector<std::string> queries;
    if (!options.query_file_path.empty()) {
      base::ScopedFstream file(fopen(options.query_file_path.c_str(), "r"));
      if (!file) {
        PERFETTO_ELOG("Could not open query file (path: %s)",
                      options.query_file_path.c_str());
        return 1;
      }
      if (!LoadQueries(file.get(), &queries)) {
        return 1;
      }
    }

    if (!RunQueryAndPrintResult(queries, stdout)) {
      return 1;
    }
  }

  if (!options.sqlite_file_path.empty()) {
    return ExportTraceToDatabase(options.sqlite_file_path);
  }

  if (!options.launch_shell) {
    auto t_query = base::GetWallTimeNs() - t_run_start;
    return MaybePrintPerfFile(options.perf_file_path, t_load, t_query);
  }

  return StartInteractiveShell();
}

}  // namespace

}  // namespace trace_processor
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::trace_processor::TraceProcessorMain(argc, argv);
}
