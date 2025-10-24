/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/user_list/user_list_data_source.h"

#include <stdio.h>

#include <set>
#include <string>

#include "perfetto/ext/base/pipe.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/android/user_list.gen.h"
#include "protos/perfetto/trace/android/user_list.pbzero.h"
#include "src/traced/probes/user_list/user_list_parser.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(UserListDataSourceTest, ParseLineNonProfileNonDebug) {
  char kLine[] = "SYSTEM 0\n";
  User usr;
  ASSERT_TRUE(ReadUserListLine(kLine, &usr));
  EXPECT_EQ(usr.type, "SYSTEM");
  EXPECT_EQ(usr.uid, 0u);
}

TEST(UserListDataSourceTest, ParseLineProfileNonDebug) {
  char buf[] =
      "SYSTEM 0\n"
      "PROFILE 10\n";
  // Create a stream from |buf|, up to the null byte. Avoid fmemopen as it
  // requires a higher target API (23) than we use for portability.
  auto pipe = base::Pipe::Create();
  PERFETTO_CHECK(write(pipe.wr.get(), buf, sizeof(buf) - 1) == sizeof(buf) - 1);
  pipe.wr.reset();
  auto fs = base::ScopedFstream(fdopen(pipe.rd.get(), "r"));
  pipe.rd.release();  // now owned by |fs|

  protozero::HeapBuffered<protos::pbzero::UserList> user_list;
  std::set<std::string> filter{};

  ASSERT_TRUE(ParseUserListStream(user_list.get(), fs, filter));

  protos::gen::UserList parsed_list;
  parsed_list.ParseFromString(user_list.SerializeAsString());

  EXPECT_FALSE(parsed_list.read_error());
  EXPECT_FALSE(parsed_list.parse_error());
  // all entries
  EXPECT_EQ(parsed_list.users_size(), 2);
  EXPECT_EQ(parsed_list.users()[0].type(), "SYSTEM");
  EXPECT_EQ(parsed_list.users()[0].uid(), 0u);
  EXPECT_EQ(parsed_list.users()[1].type(), "PROFILE");
  EXPECT_EQ(parsed_list.users()[1].uid(), 10u);
}

TEST(UserListDataSourceTest, ParseLineNonProfileDebug) {
  char kLine[] =
      "SYSTEM 0\n"
      "PROFILE 10\n";
  User usr;
  ASSERT_TRUE(ReadUserListLine(kLine, &usr));
  EXPECT_EQ(usr.type, "SYSTEM");
  EXPECT_EQ(usr.uid, 0u);
}

}  // namespace
}  // namespace perfetto
