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

#include "src/trace_processor/forwarding_trace_parser.h"

#include "src/trace_processor/importers/common/builtin_trace_importers.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/util/trace_type.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

// Registers the builtin importers exercised below so detection runs through the
// importer registry, matching production. Each importer's id is captured for
// comparison since the importer classes are anonymous.
class GuessTraceTypeTest : public ::testing::Test {
 protected:
  GuessTraceTypeTest() {
    fuchsia_ = registry_.Register(CreateFuchsiaImporter());
    json_ = registry_.Register(CreateJsonImporter());
    ninja_ = registry_.Register(CreateNinjaLogImporter());
    systrace_ = registry_.Register(CreateSystraceImporter());
    proto_ = registry_.Register(CreateProtoImporter());
  }

  TraceImporterId Guess(const uint8_t* data, size_t size) {
    return registry_.Guess(data, size);
  }

  TraceImporterRegistry registry_;
  TraceImporterId fuchsia_;
  TraceImporterId json_;
  TraceImporterId ninja_;
  TraceImporterId systrace_;
  TraceImporterId proto_;
};

TEST_F(GuessTraceTypeTest, Empty) {
  const uint8_t prefix[] = "";
  EXPECT_EQ(TraceImporterId(), Guess(prefix, 0));
}

TEST_F(GuessTraceTypeTest, Json) {
  const uint8_t prefix[] = "{\"traceEvents\":[";
  EXPECT_EQ(json_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, Ninja) {
  const uint8_t prefix[] = "# ninja log v5\n";
  EXPECT_EQ(ninja_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, JsonWithSpaces) {
  const uint8_t prefix[] = "\n{ \"traceEvents\": [";
  EXPECT_EQ(json_, Guess(prefix, sizeof(prefix)));
}

// Some Android build traces do not contain the wrapper. See b/118826940
TEST_F(GuessTraceTypeTest, JsonMissingTraceEvents) {
  const uint8_t prefix[] = "[{\"";
  EXPECT_EQ(json_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, DoctypeHtmlUppercase) {
  const uint8_t prefix[] = "<!DOCTYPE HTML>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, DoctypeHtml) {
  const uint8_t prefix[] = "<!doctype html>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, DoctypeHtmlMixed) {
  const uint8_t prefix[] = "<!DoCTyPe HtMl>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, Html) {
  const uint8_t prefix[] = "<html>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, HtmlUpper) {
  const uint8_t prefix[] = "<HTML>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, HtmlMixed) {
  const uint8_t prefix[] = "<htmL>";
  EXPECT_EQ(systrace_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, Proto) {
  const uint8_t prefix[] = {0x0a, 0x00};  // An empty TracePacket.
  EXPECT_EQ(proto_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, Fuchsia) {
  const uint8_t prefix[] = {0x10, 0x00, 0x04, 0x46, 0x78, 0x54, 0x16, 0x00};
  EXPECT_EQ(fuchsia_, Guess(prefix, sizeof(prefix)));
}

TEST_F(GuessTraceTypeTest, Bmp) {
  const uint8_t prefix[] = {0x42, 0x4d, 0x1e, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00,
                            0x0c, 0x00, 0x00, 0x00, 0x01, 0x00};
  EXPECT_EQ(TraceImporterId(), Guess(prefix, sizeof(prefix)));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
