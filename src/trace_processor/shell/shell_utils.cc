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

#include "src/trace_processor/shell/shell_utils.h"

#include <sqlite3.h>
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <unistd.h>
#else
#include <io.h>
#define ftruncate _chsize
#endif

namespace perfetto::trace_processor {

bool StderrSupportsColors() {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) &&  \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM) && \
    !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
  static const bool use_colors = isatty(STDERR_FILENO);
  return use_colors;
#else
  return false;
#endif
}

namespace {

// Builds a SQLite "file:" URI for `path`, percent-encoding characters that are
// not safe in a URI path.
//
// The main trace processor connection is opened on the in-memory "memdb" VFS.
// ATTACH DATABASE inherits the connection's VFS, so a plain path would create
// the export database in memory too. Pin the default VFS instead: it is the
// OS-backed one, under a platform-dependent name ("unix", "win32", ...).
std::string MakeExportFileUri(const std::string& path) {
  std::string normalized = path;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
#endif
  std::string uri = "file:";
  for (char c : normalized) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                      c == '_' || c == '~' || c == '/' || c == ':';
    if (unreserved) {
      uri.push_back(c);
    } else {
      base::StackString<4> escaped("%%%02X", static_cast<uint8_t>(c));
      uri.append(escaped.c_str());
    }
  }
  // Lookup default VFS.
  sqlite3_vfs* vfs = sqlite3_vfs_find(nullptr);
  PERFETTO_CHECK(vfs);
  uri.append("?vfs=");
  uri.append(vfs->zName);
  return uri;
}

base::Status PrintStatsSection(TraceProcessor* tp,
                               const char* header,
                               const char* color,
                               const char* query) {
  const bool colors = StderrSupportsColors();
  auto it = tp->ExecuteQuery(query);
  bool first = true;
  while (it.Next()) {
    if (first) {
      base::StackString<256> line("  %s%s%s", colors ? color : "", header,
                                  colors ? "\x1b[0m" : "");
      fprintf(stderr, "%s\n", line.c_str());
      first = false;
    }
    // Columns: name, idx, value, description.
    const char* name = it.Get(0).string_value;
    SqlValue idx = it.Get(1);
    int64_t value = it.Get(2).long_value;
    const char* description = it.Get(3).string_value;

    if (idx.type == SqlValue::Type::kNull) {
      base::StackString<512> line("    %s: %" PRIi64, name, value);
      fprintf(stderr, "%s", line.c_str());
    } else {
      base::StackString<512> line("    %s[%" PRIi64 "]: %" PRIi64, name,
                                  idx.long_value, value);
      fprintf(stderr, "%s", line.c_str());
    }
    if (description && description[0] != '\0') {
      base::StackString<512> desc(" | %s", description);
      fprintf(stderr, "%s", desc.c_str());
    }
    fprintf(stderr, "\n");
  }
  if (!first) {
    fprintf(stderr, "\n");
  }
  base::Status status = it.Status();
  if (!status.ok()) {
    return base::ErrStatus("Error while iterating stats (%s)",
                           status.c_message());
  }
  return base::OkStatus();
}

}  // namespace

base::Status PrintStats(TraceProcessor* tp) {
  // Quick check: are there any issues at all?
  auto check = tp->ExecuteQuery(
      "SELECT 1 FROM stats "
      "WHERE severity IN ('error', 'data_loss') AND value > 0 LIMIT 1");
  bool has_issues = check.Next();
  {
    base::Status s = check.Status();
    if (!s.ok())
      return s;
  }
  if (!has_issues)
    return base::OkStatus();

  const bool colors = StderrSupportsColors();
  base::StackString<64> title("\n%sTrace health issues:%s\n",
                              colors ? "\x1b[1;33m" : "",
                              colors ? "\x1b[0m" : "");
  fprintf(stderr, "\n%s\n", title.c_str());

  RETURN_IF_ERROR(PrintStatsSection(
      tp, "Trace errors", "\x1b[1;31m",
      "SELECT name, idx, value, description FROM stats "
      "WHERE severity = 'error' AND source = 'trace' AND value > 0 "
      "ORDER BY name, idx"));

  RETURN_IF_ERROR(PrintStatsSection(
      tp, "Import errors", "\x1b[1;31m",
      "SELECT name, idx, value, description FROM stats "
      "WHERE severity = 'error' AND source = 'analysis' AND value > 0 "
      "ORDER BY name, idx"));

  RETURN_IF_ERROR(
      PrintStatsSection(tp, "Data losses", "\x1b[1;33m",
                        "SELECT name, idx, value, description FROM stats "
                        "WHERE severity = 'data_loss' AND value > 0 "
                        "ORDER BY name, idx"));

  return base::OkStatus();
}

base::Status ExportTraceToDatabase(TraceProcessor* trace_processor,
                                   const std::string& output_name) {
  PERFETTO_CHECK(output_name.find('\'') == std::string::npos);
  {
    base::ScopedFile fd(base::OpenFile(output_name, O_CREAT | O_RDWR, 0600));
    if (!fd)
      return base::ErrStatus("Failed to create file: %s", output_name.c_str());
    int res = ftruncate(fd.get(), 0);
    PERFETTO_CHECK(res == 0);
  }

  std::string attach_sql = "ATTACH DATABASE '" +
                           MakeExportFileUri(output_name) +
                           "' AS perfetto_export";
  auto attach_it = trace_processor->ExecuteQuery(attach_sql);
  bool attach_has_more = attach_it.Next();
  PERFETTO_DCHECK(!attach_has_more);

  RETURN_IF_ERROR(attach_it.Status());

  // The export database is written once and regenerated from the trace on
  // failure, so trade durability for speed and lower I/O.
  for (const char* pragma : {"PRAGMA perfetto_export.journal_mode=OFF",
                             "PRAGMA perfetto_export.synchronous=OFF"}) {
    auto pragma_it = trace_processor->ExecuteQuery(pragma);
    while (pragma_it.Next()) {
    }
    RETURN_IF_ERROR(pragma_it.Status());
  }

  // Export real and virtual tables.
  auto tables_it =
      trace_processor->ExecuteQuery("SELECT name FROM perfetto_tables");
  while (tables_it.Next()) {
    std::string table_name = tables_it.Get(0).string_value;
    PERFETTO_CHECK(!base::Contains(table_name, '\''));
    std::string export_sql = "CREATE TABLE perfetto_export." + table_name +
                             " AS SELECT * FROM " + table_name;

    auto export_it = trace_processor->ExecuteQuery(export_sql);
    bool export_has_more = export_it.Next();
    PERFETTO_DCHECK(!export_has_more);
    RETURN_IF_ERROR(export_it.Status());
  }
  RETURN_IF_ERROR(tables_it.Status());

  // Export views.
  auto views_it = trace_processor->ExecuteQuery(
      "SELECT sql FROM sqlite_master WHERE type='view'");
  while (views_it.Next()) {
    std::string sql = views_it.Get(0).string_value;
    // View statements are of the form "CREATE VIEW name AS stmt". We need to
    // rewrite name to point to the exported db.
    const std::string kPrefix = "CREATE VIEW ";
    PERFETTO_CHECK(sql.find(kPrefix) == 0);
    sql = sql.substr(0, kPrefix.size()) + "perfetto_export." +
          sql.substr(kPrefix.size());

    auto export_it = trace_processor->ExecuteQuery(sql);
    bool export_has_more = export_it.Next();
    PERFETTO_DCHECK(!export_has_more);
    RETURN_IF_ERROR(export_it.Status());
  }
  RETURN_IF_ERROR(views_it.Status());

  auto detach_it =
      trace_processor->ExecuteQuery("DETACH DATABASE perfetto_export");
  bool detach_has_more = detach_it.Next();
  PERFETTO_DCHECK(!detach_has_more);
  return detach_it.Status();
}

}  // namespace perfetto::trace_processor
