// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Standalone native runner: build with build_duckdb_runner.py. DuckDB is an
// optional external benchmark dependency, not a Perfetto build dependency.
#include <duckdb.h>

#include "protocol.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using pipeline_benchmark::Sink;

using Clock = std::chrono::steady_clock;

std::string ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Cannot open " + path);
  std::ostringstream result;
  result << stream.rdbuf();
  if (stream.bad())
    throw std::runtime_error("Cannot read " + path);
  return result.str();
}

uint64_t Elapsed(Clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

struct Options {
  std::string database = ":memory:";
  std::string setup;
  std::string query;
  std::string output;
  std::string mode = "end_to_end";
  unsigned repeat = 5;
  unsigned warmup = 1;
  unsigned threads = 1;
};

unsigned ParseUnsigned(const std::string& value) {
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Expected a nonnegative integer: " + value);
  size_t consumed = 0;
  auto number = std::stoull(value, &consumed);
  if (consumed != value.size() || number > std::numeric_limits<unsigned>::max())
    throw std::runtime_error("Integer out of range: " + value);
  return static_cast<unsigned>(number);
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (i + 1 == argc)
      throw std::runtime_error("Missing value for " + key);
    std::string value = argv[++i];
    if (key == "--database")
      options.database = value;
    else if (key == "--setup")
      options.setup = value;
    else if (key == "--query")
      options.query = value;
    else if (key == "--output")
      options.output = value;
    else if (key == "--mode")
      options.mode = value;
    else if (key == "--repeat")
      options.repeat = ParseUnsigned(value);
    else if (key == "--warmup")
      options.warmup = ParseUnsigned(value);
    else if (key == "--threads")
      options.threads = ParseUnsigned(value);
    else
      throw std::runtime_error("Unknown option: " + key);
  }
  if (options.query.empty() || options.output.empty() || options.threads == 0)
    throw std::runtime_error(
        "Require --query FILE --output PREFIX and threads > 0");
  if (options.mode != "end_to_end" && options.mode != "prepared")
    throw std::runtime_error("--mode must be end_to_end or prepared");
  if (options.repeat == 0)
    options.warmup = 0;
  return options;
}

struct Database {
  duckdb_database database = nullptr;
  duckdb_connection connection = nullptr;
  duckdb_prepared_statement prepared = nullptr;
  ~Database() {
    if (prepared)
      duckdb_destroy_prepare(&prepared);
    if (connection)
      duckdb_disconnect(&connection);
    if (database)
      duckdb_close(&database);
  }
};

struct Result {
  duckdb_result value{};
  ~Result() { duckdb_destroy_result(&value); }
};

struct Chunk {
  duckdb_data_chunk value = nullptr;
  ~Chunk() {
    if (value)
      duckdb_destroy_data_chunk(&value);
  }
};

void Query(duckdb_connection connection,
           const std::string& sql,
           Result& result) {
  if (duckdb_query(connection, sql.c_str(), &result.value) != DuckDBSuccess) {
    const char* error = duckdb_result_error(&result.value);
    throw std::runtime_error(error ? error : "DuckDB query failed");
  }
}

struct Summary {
  uint64_t rows;
  uint64_t checksum;
  uint64_t columns;
};

struct Column {
  duckdb_type type;
  void* data;
  uint64_t* validity;
};

template <typename T>
T Value(const Column& column, idx_t row) {
  return static_cast<T*>(column.data)[row];
}

void Unsigned(uint64_t value, Sink& sink) {
  if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    throw std::runtime_error("Unsigned result does not fit SQLite's int64");
  sink.Integer(static_cast<int64_t>(value));
}

void Cell(const Column& column, idx_t row, Sink& sink) {
  if ((column.validity &&
       !(column.validity[row / 64] & (UINT64_C(1) << (row % 64)))) ||
      column.type == DUCKDB_TYPE_SQLNULL) {
    sink.Null();
    return;
  }
  switch (column.type) {
    case DUCKDB_TYPE_BOOLEAN:
      return sink.Integer(Value<bool>(column, row) ? 1 : 0);
    case DUCKDB_TYPE_TINYINT:
      return sink.Integer(Value<int8_t>(column, row));
    case DUCKDB_TYPE_SMALLINT:
      return sink.Integer(Value<int16_t>(column, row));
    case DUCKDB_TYPE_INTEGER:
      return sink.Integer(Value<int32_t>(column, row));
    case DUCKDB_TYPE_BIGINT:
      return sink.Integer(Value<int64_t>(column, row));
    case DUCKDB_TYPE_UTINYINT:
      return Unsigned(Value<uint8_t>(column, row), sink);
    case DUCKDB_TYPE_USMALLINT:
      return Unsigned(Value<uint16_t>(column, row), sink);
    case DUCKDB_TYPE_UINTEGER:
      return Unsigned(Value<uint32_t>(column, row), sink);
    case DUCKDB_TYPE_UBIGINT:
      return Unsigned(Value<uint64_t>(column, row), sink);
    case DUCKDB_TYPE_HUGEINT: {
      const auto value = Value<duckdb_hugeint>(column, row);
      const bool negative = (value.lower >> 63) != 0;
      if (value.upper != (negative ? -1 : 0))
        throw std::runtime_error("HUGEINT result does not fit SQLite's int64");
      int64_t signed_value;
      std::memcpy(&signed_value, &value.lower, sizeof(signed_value));
      return sink.Integer(signed_value);
    }
    case DUCKDB_TYPE_FLOAT:
      return sink.Double(Value<float>(column, row));
    case DUCKDB_TYPE_DOUBLE:
      return sink.Double(Value<double>(column, row));
    case DUCKDB_TYPE_VARCHAR: {
      auto value = Value<duckdb_string_t>(column, row);
      return sink.String(duckdb_string_t_data(&value),
                         duckdb_string_t_length(value));
    }
    default:
      throw std::runtime_error("Unsupported benchmark result type " +
                               std::to_string(column.type) +
                               "; explicitly cast the query result");
  }
}

Summary Consume(Result& result, std::ostream* output) {
  const auto column_count = duckdb_column_count(&result.value);
  if (column_count == 0)
    throw std::runtime_error("Benchmark query must return result columns");
  Sink sink(output);
  std::vector<Column> columns;
  while (true) {
    Chunk chunk{duckdb_fetch_chunk(result.value)};
    if (!chunk.value)
      break;
    const auto column_count = duckdb_data_chunk_get_column_count(chunk.value);
    columns.clear();
    for (idx_t c = 0; c < column_count; ++c) {
      auto vector = duckdb_data_chunk_get_vector(chunk.value, c);
      auto type = duckdb_vector_get_column_type(vector);
      columns.push_back({duckdb_get_type_id(type),
                         duckdb_vector_get_data(vector),
                         duckdb_vector_get_validity(vector)});
      duckdb_destroy_logical_type(&type);
    }
    const auto rows = duckdb_data_chunk_get_size(chunk.value);
    for (idx_t r = 0; r < rows; ++r) {
      for (idx_t c = 0; c < column_count; ++c) {
        Cell(columns[c], r, sink);
      }
      sink.EndRow();
    }
  }
  const char* error = duckdb_result_error(&result.value);
  if (error)
    throw std::runtime_error(error);
  return {sink.rows(), sink.checksum(), column_count};
}

Summary Run(Database& database,
            const std::string& query,
            std::ostream* output = nullptr) {
  Result result;
  if (database.prepared) {
    if (duckdb_execute_prepared(database.prepared, &result.value) !=
        DuckDBSuccess) {
      const char* error = duckdb_result_error(&result.value);
      throw std::runtime_error(error ? error : "DuckDB execution failed");
    }
  } else {
    Query(database.connection, query, result);
  }
  return Consume(result, output);
}

int Main(int argc, char** argv) {
  const auto options = ParseOptions(argc, argv);
  const auto query = ReadFile(options.query);
  Database database;
  duckdb_config config = nullptr;
  if (duckdb_create_config(&config) != DuckDBSuccess)
    throw std::runtime_error("Cannot create DuckDB configuration");
  const auto threads = std::to_string(options.threads);
  const auto configured = duckdb_set_config(config, "threads", threads.c_str());
  if (configured != DuckDBSuccess) {
    duckdb_destroy_config(&config);
    throw std::runtime_error("Cannot configure DuckDB threads");
  }
  char* open_error = nullptr;
  const auto opened = duckdb_open_ext(options.database.c_str(),
                                      &database.database, config, &open_error);
  duckdb_destroy_config(&config);
  if (opened != DuckDBSuccess) {
    const std::string message = open_error ? open_error : "Cannot open DuckDB";
    duckdb_free(open_error);
    throw std::runtime_error(message);
  }
  if (duckdb_connect(database.database, &database.connection) != DuckDBSuccess)
    throw std::runtime_error("Cannot connect to DuckDB");
  uint64_t setup_ns = 0;
  if (!options.setup.empty()) {
    const auto setup = ReadFile(options.setup);
    const auto start = Clock::now();
    {
      Result result;
      Query(database.connection, setup, result);
    }
    setup_ns = Elapsed(start);
  }
  // All runners validate before warmup, so their cache-warming protocol agrees.
  // Opening output files and serialization are deliberately outside timing.
  std::ofstream timings(options.output + ".timings.csv");
  std::ofstream rows(options.output + ".rows.tsv", std::ios::binary);
  std::ofstream metadata(options.output + ".metadata.json");
  if (!timings || !rows || !metadata)
    throw std::runtime_error("Cannot open output files for " + options.output);
  const auto reference = Run(database, query, &rows);
  uint64_t prepare_ns = 0;
  if (options.mode == "prepared" && options.repeat != 0) {
    const auto start = Clock::now();
    if (duckdb_prepare(database.connection, query.c_str(),
                       &database.prepared) != DuckDBSuccess) {
      const char* error = duckdb_prepare_error(database.prepared);
      throw std::runtime_error(error ? error : "Cannot prepare query");
    }
    prepare_ns = Elapsed(start);
  }
  auto verify = [&](const Summary& summary) {
    if (summary.rows != reference.rows ||
        summary.checksum != reference.checksum ||
        summary.columns != reference.columns)
      throw std::runtime_error(
          "Query results changed across repetitions; "
          "benchmark queries must be deterministic");
  };
  for (unsigned i = 0; i < options.warmup; ++i)
    verify(Run(database, query));
  timings << "repetition,elapsed_ns,rows,checksum\n";
  for (unsigned i = 0; i < options.repeat; ++i) {
    const auto start = Clock::now();
    const auto summary = Run(database, query);
    const auto elapsed = Elapsed(start);
    verify(summary);
    timings << i << ',' << elapsed << ',' << summary.rows << ','
            << summary.checksum << '\n';
  }
  metadata << "{\"engine\":\"duckdb\",\"version\":\""
           << duckdb_library_version() << "\",\"threads\":" << options.threads
           << ",\"mode\":\"" << options.mode << "\",\"setup_ns\":" << setup_ns
           << ",\"prepare_ns\":" << prepare_ns
           << ",\"warmup\":" << options.warmup
           << ",\"repeat\":" << options.repeat
           << ",\"validation_columns\":" << reference.columns
           << ",\"validation_rows\":" << reference.rows
           << ",\"validation_checksum\":" << reference.checksum << "}\n";
  timings.close();
  rows.close();
  metadata.close();
  if (!timings || !rows || !metadata)
    throw std::runtime_error("Failed writing benchmark output");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "--query FILE --output PREFIX [--setup FILE] "
                 "[--database :memory:] [--repeat 5] [--warmup 1] "
                 "[--mode end_to_end|prepared] [--threads 1]\n"
                 "Use --repeat 0 for validation only.\n";
    return 0;
  }
  try {
    return Main(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "duckdb_runner: " << error.what() << '\n';
    return 1;
  }
}
