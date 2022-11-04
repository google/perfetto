/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "test/gtest_and_gmock.h"

#include "perfetto/tracing.h"

namespace {

class PerfettoApiEnvironment : public ::testing::Environment {
 public:
  void TearDown() override {
    // Test shutting down Perfetto only when all other tests have been run and
    // no more tracing code will be executed.
    PERFETTO_CHECK(!perfetto::Tracing::IsInitialized());
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::Tracing::Shutdown();
    PERFETTO_CHECK(!perfetto::Tracing::IsInitialized());
    // Shutting down again is a no-op.
    perfetto::Tracing::Shutdown();
    PERFETTO_CHECK(!perfetto::Tracing::IsInitialized());
  }
};

}  // namespace

int main(int argc, char** argv) {
  ::testing::AddGlobalTestEnvironment(new PerfettoApiEnvironment);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
