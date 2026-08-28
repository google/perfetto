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

#include "src/trace_processor/sqlite/file_system_vfs.h"

#include <sqlite3.h>

#include <memory>
#include <string>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/trace_processor/io.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/local_file_system.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(SqliteFileSystemVfsTest, CreatesReadableDatabase) {
  // Ensure SQLite is initialized before registering another VFS.
  auto initialization_connection =
      SqliteConnection::CreateConnectionToNewDatabase();
  ASSERT_NE(initialization_connection, nullptr);

  base::TempFile file = base::TempFile::Create();
  const std::string& path = file.path();
  auto file_system = io::CreateLocalFileSystem();
  ASSERT_OK_AND_ASSIGN(auto vfs, SqliteFileSystemVfs::Create(file_system));

  sqlite3* raw_db = nullptr;
  ASSERT_EQ(
      sqlite3_open_v2(path.c_str(), &raw_db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs->name()),
      SQLITE_OK);
  ScopedDb db(raw_db);
  ASSERT_EQ(sqlite3_exec(db.get(),
                         "CREATE TABLE data(value INTEGER);"
                         "INSERT INTO data VALUES(42);",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  db.reset();
  vfs.reset();

  std::string contents;
  ASSERT_TRUE(base::ReadFile(path, &contents));
  ASSERT_GE(contents.size(), 16u);
  EXPECT_EQ(contents.substr(0, 16), std::string("SQLite format 3\0", 16));

  ASSERT_OK_AND_ASSIGN(vfs, SqliteFileSystemVfs::Create(file_system));
  raw_db = nullptr;
  ASSERT_EQ(
      sqlite3_open_v2(path.c_str(), &raw_db, SQLITE_OPEN_READONLY, vfs->name()),
      SQLITE_OK);
  db.reset(raw_db);
  sqlite3_stmt* raw_stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db.get(), "SELECT value FROM data", -1,
                               &raw_stmt, nullptr),
            SQLITE_OK);
  ScopedStmt stmt(raw_stmt);
  ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(stmt.get(), 0), 42);
  stmt.reset();
  db.reset();
}

TEST(SqliteFileSystemVfsTest, NoopFileSystemCannotOpenDatabase) {
  auto initialization_connection =
      SqliteConnection::CreateConnectionToNewDatabase();
  ASSERT_NE(initialization_connection, nullptr);
  auto noop_file_system = io::CreateNoopFileSystem();
  ASSERT_OK_AND_ASSIGN(auto vfs, SqliteFileSystemVfs::Create(noop_file_system));
  sqlite3* raw_db = nullptr;
  EXPECT_NE(
      sqlite3_open_v2("database", &raw_db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs->name()),
      SQLITE_OK);
  if (raw_db) {
    sqlite3_close(raw_db);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
