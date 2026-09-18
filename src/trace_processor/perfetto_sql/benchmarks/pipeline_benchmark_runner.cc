// Copyright (C) 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "benchmarks/pipeline/protocol.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace {
using namespace perfetto::trace_processor;
using pipeline_benchmark::Sink;
using Clock = std::chrono::steady_clock;
using Statement = SqliteConnection::PreparedStatement;

[[noreturn]] void Fail(const std::string& message) {
  fprintf(stderr, "%s\n", message.c_str());
  exit(1);
}

std::string Read(const std::string& path) {
  std::ifstream stream(path);
  if (!stream)
    Fail("Cannot read " + path);
  return {std::istreambuf_iterator<char>(stream), {}};
}
uint32_t Number(const std::string& value) {
  char* end = nullptr;
  auto result = strtoull(value.c_str(), &end, 10);
  if (value.empty() || *end || result > 1000000 || value[0] == '-')
    Fail("Invalid count: " + value);
  return static_cast<uint32_t>(result);
}
int64_t Elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              start)
      .count();
}

Statement Prepare(PerfettoSqlConnection& connection,
                  const std::string& backend,
                  const std::string& query) {
  auto sql = SqlSource::FromExecuteQuery(query);
  if (backend == "pipeline" || backend == "pipeline_sql_scan") {
    auto result = connection.ExecuteUntilLastStatement(std::move(sql));
    if (!result.ok())
      Fail(result.status().message());
    return std::move(result->stmt);
  }
  // Ordinary and dataframe SQLite use the same bundled SQLite prepare/step
  // path. Pipeline additionally pays for its frontend and adapter construction.
  auto stmt = connection.sqlite_connection()->PrepareStatement(std::move(sql));
  if (!stmt.status().ok())
    Fail(stmt.status().message());
  stmt.Step();
  if (!stmt.status().ok())
    Fail(stmt.status().message());
  return stmt;
}

void Drain(Statement& stmt, Sink& sink) {
  sqlite3_stmt* raw = stmt.sqlite_stmt();
  int columns = sqlite3_column_count(raw);
  for (bool more = !stmt.IsDone(); more; more = stmt.Step()) {
    for (int column = 0; column < columns; ++column) {
      switch (sqlite3_column_type(raw, column)) {
        case SQLITE_NULL:
          sink.Null();
          break;
        case SQLITE_INTEGER:
          sink.Integer(sqlite3_column_int64(raw, column));
          break;
        case SQLITE_FLOAT:
          sink.Double(sqlite3_column_double(raw, column));
          break;
        case SQLITE_TEXT: {
          const char* data =
              reinterpret_cast<const char*>(sqlite3_column_text(raw, column));
          sink.String(data,
                      static_cast<size_t>(sqlite3_column_bytes(raw, column)));
          break;
        }
        default:
          Fail(
              "Unsupported result cell (BLOB); cannot silently compare it as "
              "text");
      }
    }
    sink.EndRow();
  }
  if (!stmt.status().ok())
    Fail(stmt.status().message());
}

}  // namespace

int main(int argc, char** argv) {
  std::string setup_path, query_path, output, backend, mode = "end_to_end";
  uint32_t repeat = 7, warmup = 2;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--help") {
      printf(
          "--backend sqlite|dataframe|pipeline|pipeline_sql_scan --setup FILE "
          "--query FILE "
          "--output PREFIX [--repeat 7] [--warmup 2] [--mode "
          "end_to_end|prepared]\n");
      return 0;
    }
    if (++i == argc)
      Fail("Missing value for " + key);
    std::string value = argv[i];
    if (key == "--setup")
      setup_path = value;
    else if (key == "--query")
      query_path = value;
    else if (key == "--output")
      output = value;
    else if (key == "--backend")
      backend = value;
    else if (key == "--mode")
      mode = value;
    else if (key == "--repeat")
      repeat = Number(value);
    else if (key == "--warmup")
      warmup = Number(value);
    else if (key == "--database" && value == ":memory:") {
    } else if (key == "--threads" && value == "1") {
    } else
      Fail("Unsupported option: " + key + " " + value);
  }
  if (setup_path.empty() || query_path.empty() || output.empty() ||
      (backend != "sqlite" && backend != "dataframe" && backend != "pipeline" &&
       backend != "pipeline_sql_scan") ||
      (mode != "end_to_end" && mode != "prepared"))
    Fail("Invalid options; see --help");
  if (repeat == 0)
    warmup = 0;
  std::string setup = Read(setup_path), query = Read(query_path);
  StringPool pool;
  auto connection =
      PerfettoSqlConnection::CreateConnectionToNewDatabase(&pool, false);
  auto setup_start = Clock::now();
  auto setup_result = connection->Execute(SqlSource::FromExecuteQuery(setup));
  if (!setup_result.ok())
    Fail(setup_result.status().message());
  int64_t setup_ns = Elapsed(setup_start);

  std::ofstream rows_file(output + ".rows.tsv");
  std::ofstream timings(output + ".timings.csv");
  std::ofstream metadata(output + ".metadata.json");
  if (!rows_file || !timings || !metadata)
    Fail("Cannot create output files");
  uint64_t validation_rows, validation_checksum;
  int validation_columns;
  {
    auto stmt = Prepare(*connection, backend, query);
    validation_columns = sqlite3_column_count(stmt.sqlite_stmt());
    Sink validation(&rows_file);
    Drain(stmt, validation);
    validation_rows = validation.rows();
    validation_checksum = validation.checksum();
  }
  rows_file.close();
  if (!rows_file)
    Fail("Failed writing validation rows");

  // Prepared mode retains one plan/statement and rewinds it each iteration.
  // End-to-end mode includes parse, planning, execution and statement teardown.
  std::unique_ptr<Statement> prepared;
  int64_t prepare_ns = 0;
  if (mode == "prepared" && repeat != 0) {
    auto prepare_start = Clock::now();
    prepared =
        std::make_unique<Statement>(Prepare(*connection, backend, query));
    prepare_ns = Elapsed(prepare_start);
    // Pipeline's public entry point steps once. This initial execution is
    // outside timings and is discarded before every measured execution.
  }
  timings << "repetition,elapsed_ns,rows,checksum\n";
  for (uint32_t iteration = 0; iteration < warmup + repeat; ++iteration) {
    Sink sink;
    auto start = Clock::now();
    if (prepared) {
      int rc = sqlite3_reset(prepared->sqlite_stmt());
      if (rc != SQLITE_OK)
        Fail(sqlite3_errstr(rc));
      prepared->Step();
      Drain(*prepared, sink);
    } else {
      auto stmt = Prepare(*connection, backend, query);
      Drain(stmt, sink);
    }
    int64_t elapsed = Elapsed(start);
    if (sink.rows() != validation_rows ||
        sink.checksum() != validation_checksum)
      Fail("Timed result differs from validation");
    if (iteration >= warmup) {
      timings << iteration - warmup << ',' << elapsed << ',' << sink.rows()
              << ',' << sink.checksum() << '\n';
    }
  }
  metadata << "{\"engine\":\"" << backend << "\",\"version\":\""
           << sqlite3_libversion() << "\",\"threads\":1,\"mode\":\"" << mode
           << "\",\"setup_ns\":" << setup_ns
           << ",\"prepare_and_first_step_ns\":" << prepare_ns
           << ",\"debug_build\":"
           << (PIPELINE_BENCHMARK_IS_DEBUG ? "true" : "false")
           << ",\"validation_rows\":" << validation_rows
           << ",\"validation_checksum\":" << validation_checksum
           << ",\"validation_columns\":" << validation_columns << "}\n";
  timings.close();
  metadata.close();
  if (!timings || !metadata)
    Fail("Failed writing measurements");
  return 0;
}
