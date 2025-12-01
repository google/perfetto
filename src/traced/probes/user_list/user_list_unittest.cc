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
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using ::testing::ElementsAre;
using ::testing::Property;

// Helper function to create a ScopedFstream from a string buffer
base::ScopedFstream CreateStreamFromString(const char* buf, size_t size) {
  auto pipe = base::Pipe::Create();
  PERFETTO_CHECK(write(pipe.wr.get(), buf, size) == static_cast<ssize_t>(size));
  pipe.wr.reset();
  auto fs = base::ScopedFstream(fdopen(pipe.rd.get(), "r"));
  pipe.rd.release();  // now owned by |fs|
  return fs;
}

TEST(UserListDataSourceTest, ParseLineSystem) {
  char kLine[] = "android.os.usertype.full.SYSTEM 0\n";
  User usr;
  EXPECT_EQ(ReadUserListLine(kLine, &usr), 0);
  EXPECT_EQ(usr.type, "android.os.usertype.full.SYSTEM");
  EXPECT_EQ(usr.uid, 0);
}

TEST(UserListDataSourceTest, ParseLineProfile) {
  char kLine[] =
      "android.os.usertype.profile.MANAGED 10\n";  // Test a single line
  User usr;
  EXPECT_EQ(ReadUserListLine(kLine, &usr), 0);
  EXPECT_EQ(usr.type, "android.os.usertype.profile.MANAGED");
  EXPECT_EQ(usr.uid, 10);
}

TEST(UserListDataSourceTest, ParseLineWithSpaces) {
  char kLine[] = "android.os.usertype.full.GUEST 11  \n";
  User usr;
  EXPECT_EQ(ReadUserListLine(kLine, &usr), 0);
  EXPECT_EQ(usr.type, "android.os.usertype.full.GUEST");
  EXPECT_EQ(usr.uid, 11);
}

TEST(UserListDataSourceTest, ParseLineIncomplete) {
  char kLine[] = "android.os.usertype.full.SYSTEM\n";
  User usr;
  EXPECT_EQ(ReadUserListLine(kLine, &usr), -1);
}

TEST(UserListDataSourceTest, ParseLineInvalidUid) {
  char kLine[] = "android.os.usertype.full.SYSTEM ABC\n";
  User usr;
  EXPECT_EQ(ReadUserListLine(kLine, &usr), -1);
}

TEST(UserListDataSourceTest, ParseUserListStream) {
  char buf[] =
      "android.os.usertype.full.SYSTEM 0\n"
      "android.os.usertype.profile.MANAGED 10\n";
  // Create a stream from |buf|, up to the null byte. Avoid fmemopen as it
  // requires a higher target API (23) than we use for portability.
  auto pipe = base::Pipe::Create();
  PERFETTO_CHECK(write(pipe.wr.get(), buf, sizeof(buf) - 1) == sizeof(buf) - 1);
  pipe.wr.reset();
  auto fs = base::ScopedFstream(fdopen(pipe.rd.get(), "r"));
  pipe.rd.release();  // now owned by |fs|

  protozero::HeapBuffered<protos::pbzero::AndroidUserList> user_list;
  std::set<std::string> filter{};

  EXPECT_EQ(ParseUserListStream(user_list.get(), fs, filter), 0);

  protos::gen::AndroidUserList parsed_list;
  parsed_list.ParseFromString(user_list.SerializeAsString());

  EXPECT_EQ(parsed_list.error(), 0);
  // all entries
  ASSERT_EQ(parsed_list.users_size(), 2);
  EXPECT_EQ(parsed_list.users()[0].type(), "android.os.usertype.full.SYSTEM");
  EXPECT_EQ(parsed_list.users()[0].uid(), 0);
  EXPECT_EQ(parsed_list.users()[1].type(),
            "android.os.usertype.profile.MANAGED");
  EXPECT_EQ(parsed_list.users()[1].uid(), 10);
}

TEST(UserListDataSourceTest, ParseUserListStreamWithFilter) {
  char buf[] =
      "android.os.usertype.full.SYSTEM 0\n"
      "android.os.usertype.full.SECONDARY 10\n"
      "android.os.usertype.profile.MANAGED 11\n"
      "android.os.usertype.full.GUEST 12\n";
  auto fs = CreateStreamFromString(buf, sizeof(buf) - 1);

  protozero::HeapBuffered<protos::pbzero::AndroidUserList> user_list;
  std::set<std::string> filter{"android.os.usertype.full.SYSTEM",
                               "android.os.usertype.profile.MANAGED"};

  EXPECT_EQ(ParseUserListStream(user_list.get(), fs, filter), 0);

  protos::gen::AndroidUserList parsed_list;
  parsed_list.ParseFromString(user_list.SerializeAsString());

  EXPECT_EQ(parsed_list.error(), 0);
  ASSERT_EQ(parsed_list.users_size(), 4);
  EXPECT_EQ(parsed_list.users()[0].type(), "android.os.usertype.full.SYSTEM");
  EXPECT_EQ(parsed_list.users()[0].uid(), 0);
  EXPECT_EQ(parsed_list.users()[1].type(),
            "android.os.usertype.FILTERED");  // Was SECONDARY
  EXPECT_EQ(parsed_list.users()[1].uid(), 10);
  EXPECT_EQ(parsed_list.users()[2].type(),
            "android.os.usertype.profile.MANAGED");
  EXPECT_EQ(parsed_list.users()[2].uid(), 11);
  EXPECT_EQ(parsed_list.users()[3].type(),
            "android.os.usertype.FILTERED");  // Was GUEST
  EXPECT_EQ(parsed_list.users()[3].uid(), 12);
}

TEST(UserListDataSourceTest, ParseUserListStreamWithFilterNotPresentOnly) {
  char buf[] =
      "android.os.usertype.full.SECONDARY 10\n"
      "android.os.usertype.full.GUEST 11\n";
  auto fs = CreateStreamFromString(buf, sizeof(buf) - 1);

  protozero::HeapBuffered<protos::pbzero::AndroidUserList> user_list;
  std::set<std::string> filter{"android.os.usertype.full.SYSTEM"};

  EXPECT_EQ(ParseUserListStream(user_list.get(), fs, filter), 0);

  protos::gen::AndroidUserList parsed_list;
  parsed_list.ParseFromString(user_list.SerializeAsString());

  EXPECT_EQ(parsed_list.error(), 0);
  ASSERT_EQ(parsed_list.users_size(), 2);
  EXPECT_EQ(parsed_list.users()[0].type(), "android.os.usertype.FILTERED");
  EXPECT_EQ(parsed_list.users()[0].uid(), 10);
  EXPECT_EQ(parsed_list.users()[1].type(), "android.os.usertype.FILTERED");
  EXPECT_EQ(parsed_list.users()[1].uid(), 11);
}

TEST(UserListDataSourceTest, ParseUserListStreamWithFilterAllMatch) {
  char buf[] =
      "android.os.usertype.full.SYSTEM 0\n"
      "android.os.usertype.system.HEADLESS 1\n";
  auto fs = CreateStreamFromString(buf, sizeof(buf) - 1);

  protozero::HeapBuffered<protos::pbzero::AndroidUserList> user_list;
  std::set<std::string> filter{"android.os.usertype.full.SYSTEM",
                               "android.os.usertype.system.HEADLESS"};

  EXPECT_EQ(ParseUserListStream(user_list.get(), fs, filter), 0);

  protos::gen::AndroidUserList parsed_list;
  parsed_list.ParseFromString(user_list.SerializeAsString());

  EXPECT_EQ(parsed_list.error(), 0);
  ASSERT_EQ(parsed_list.users_size(), 2);
  EXPECT_EQ(parsed_list.users()[0].type(), "android.os.usertype.full.SYSTEM");
  EXPECT_EQ(parsed_list.users()[0].uid(), 0);
  EXPECT_EQ(parsed_list.users()[1].type(),
            "android.os.usertype.system.HEADLESS");
  EXPECT_EQ(parsed_list.users()[1].uid(), 1);
}

}  // namespace
}  // namespace perfetto
