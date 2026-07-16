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
#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

int64_t ScalarCount(TraceProcessor* tp, const std::string& sql) {
  auto it = tp->ExecuteQuery(sql);
  PERFETTO_CHECK(it.Next());
  int64_t value = it.Get(0).AsLong();
  PERFETTO_CHECK(!it.Next());
  PERFETTO_CHECK(it.Status().ok());
  return value;
}

int64_t ScalarCount(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  PERFETTO_CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
                 SQLITE_OK);
  PERFETTO_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  int64_t value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

// Builds a file: URI that forces the OS-backed VFS, so a memdb-based trace
// processor connection attaches the on-disk file rather than a memory database.
std::string MakeAttachUri(const std::string& path) {
  std::string normalized = path;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  const char* vfs = "win32";
#else
  const char* vfs = "unix";
#endif
  return "file:" + normalized + "?vfs=" + vfs;
}

// Verifies the export produces an on-disk SQLite database that can be read back
// by a standalone SQLite connection and re-attached and queried by a trace
// processor.
TEST(ShellUtilsTest, ExportTraceToDatabaseWritesToDisk) {
  auto tp = TraceProcessor::CreateInstance(Config());

  int64_t expected_tables = ScalarCount(tp.get(),
                                        "SELECT COUNT(*) FROM "
                                        "perfetto_tables");
  int64_t expected_views = ScalarCount(
      tp.get(), "SELECT COUNT(*) FROM sqlite_master WHERE type='view'");

  base::TempDir dir = base::TempDir::Create();
  std::string output = dir.path() + "/export.db";

  ASSERT_TRUE(ExportTraceToDatabase(tp.get(), output).ok());

  std::string contents;
  ASSERT_TRUE(base::ReadFile(output, &contents));
  ASSERT_GT(contents.size(), 0u);
  ASSERT_EQ(contents.rfind("SQLite format 3", 0), 0u);

  // Read back with a standalone connection on the default (on-disk) VFS.
  PERFETTO_CHECK(sqlite3_initialize() == SQLITE_OK);
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(output.c_str(), &db), SQLITE_OK);
  EXPECT_EQ(
      ScalarCount(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'"),
      expected_tables);
  EXPECT_EQ(
      ScalarCount(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='view'"),
      expected_views);
  sqlite3_close(db);

  // Reload the exported database into a fresh trace processor and query it.
  auto reloaded = TraceProcessor::CreateInstance(Config());
  {
    auto it = reloaded->ExecuteQuery("ATTACH DATABASE '" +
                                     MakeAttachUri(output) + "' AS reimported");
    EXPECT_FALSE(it.Next());
    ASSERT_TRUE(it.Status().ok()) << it.Status().c_message();
  }
  std::string table;
  {
    auto it = tp->ExecuteQuery(
        "SELECT name FROM perfetto_tables ORDER BY name LIMIT 1");
    ASSERT_TRUE(it.Next());
    table = it.Get(0).string_value;
    ASSERT_TRUE(it.Status().ok());
  }
  EXPECT_EQ(
      ScalarCount(reloaded.get(), "SELECT COUNT(*) FROM reimported." + table),
      ScalarCount(tp.get(), "SELECT COUNT(*) FROM " + table));
  reloaded.reset();

  base::Unlink(output.c_str());
}

}  // namespace
}  // namespace perfetto::trace_processor
