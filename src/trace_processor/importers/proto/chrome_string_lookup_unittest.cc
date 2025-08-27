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

#include "src/trace_processor/importers/proto/chrome_string_lookup.h"

#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

#include "protos/third_party/chromium/chrome_enums.pbzero.h"

namespace perfetto::trace_processor {
namespace {

namespace chrome_enums = ::perfetto::protos::chrome_enums::pbzero;

class ChromeStringLookupTest : public ::testing::Test {
 protected:
  // Converts the result of GetString to a C string that can be compared with
  // EXPECT_STREQ.
  const char* LookupString(StringId string_id) const {
    return storage_.GetString(string_id).c_str();
  }

  TraceStorage storage_;
};

TEST_F(ChromeStringLookupTest, UnspecifiedStrings) {
  ChromeStringLookup strings(&storage_);
  EXPECT_EQ(strings.GetProcessName(chrome_enums::PROCESS_UNSPECIFIED),
            kNullStringId);
  EXPECT_EQ(strings.GetThreadName(chrome_enums::THREAD_UNSPECIFIED),
            kNullStringId);
}

TEST_F(ChromeStringLookupTest, PredefinedStrings) {
  ChromeStringLookup strings(&storage_);
  EXPECT_STREQ(
      LookupString(strings.GetProcessName(chrome_enums::PROCESS_BROWSER)),
      "Browser");
  EXPECT_STREQ(
      LookupString(strings.GetThreadName(chrome_enums::THREAD_BROWSER_MAIN)),
      "CrBrowserMain");
}

TEST_F(ChromeStringLookupTest, GeneratedStrings) {
  ChromeStringLookup strings(&storage_,
                             /*ignore_predefined_strings_for_testing=*/true);
  EXPECT_STREQ(
      LookupString(strings.GetProcessName(chrome_enums::PROCESS_BROWSER)),
      "PROCESS_BROWSER");
  EXPECT_STREQ(
      LookupString(strings.GetThreadName(chrome_enums::THREAD_BROWSER_MAIN)),
      "THREAD_BROWSER_MAIN");
}

TEST_F(ChromeStringLookupTest, UnknownStrings) {
  ChromeStringLookup strings(&storage_);
  EXPECT_EQ(strings.GetProcessName(chrome_enums::ProcessType_MIN - 1),
            kNullStringId);
  EXPECT_EQ(strings.GetProcessName(chrome_enums::ProcessType_MAX + 1),
            kNullStringId);
  EXPECT_EQ(strings.GetThreadName(chrome_enums::ThreadType_MIN - 1),
            kNullStringId);
  EXPECT_EQ(strings.GetThreadName(chrome_enums::ThreadType_MAX + 1),
            kNullStringId);
}

}  // namespace
}  // namespace perfetto::trace_processor
