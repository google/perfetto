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

#include "src/trace_processor/util/auto_symbolizer.h"

#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

class AutoSymbolizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_ = TraceProcessor::CreateInstance({});
  }

  std::unique_ptr<TraceProcessor> tp_;
};

TEST_F(AutoSymbolizerTest, NoMappingsReturnsNoMappingsError) {
  // Empty trace has no mappings
  SymbolizerConfig config;
  auto result = Symbolize(tp_.get(), config);

  EXPECT_EQ(result.error, SymbolizerError::kNoMappingsToSymbolize);
  EXPECT_FALSE(result.error_details.empty());
  EXPECT_TRUE(result.symbols.empty());
}

TEST_F(AutoSymbolizerTest, ConfigPassesSymbolPaths) {
  SymbolizerConfig config;
  config.symbol_paths = {"/path/to/symbols", "/another/path"};
  config.no_auto_symbol_paths = true;

  // Still returns no mappings error since trace is empty,
  // but this verifies the config is accepted
  auto result = Symbolize(tp_.get(), config);
  EXPECT_EQ(result.error, SymbolizerError::kNoMappingsToSymbolize);
}

TEST_F(AutoSymbolizerTest, NoAutoSymbolPathsFlag) {
  SymbolizerConfig config;
  config.no_auto_symbol_paths = true;

  // Empty trace, so returns no mappings
  auto result = Symbolize(tp_.get(), config);
  EXPECT_EQ(result.error, SymbolizerError::kNoMappingsToSymbolize);
}

}  // namespace
}  // namespace perfetto::trace_processor::util
