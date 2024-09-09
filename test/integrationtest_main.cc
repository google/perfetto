/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "test/integrationtest_initializer.h"

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::integration_tests {

static void (*heapprofd_end_to_end_test_initializer)(void) = nullptr;
int RegisterHeapprofdEndToEndTestInitializer(void (*fn)(void)) {
  PERFETTO_CHECK(heapprofd_end_to_end_test_initializer == nullptr);
  heapprofd_end_to_end_test_initializer = fn;
  return 0;
}

static void (*api_integration_test_initializer)(void) = nullptr;
int RegisterApiIntegrationTestInitializer(void (*fn)(void)) {
  PERFETTO_CHECK(api_integration_test_initializer == nullptr);
  api_integration_test_initializer = fn;
  return 0;
}

}  // namespace perfetto::integration_tests

int main(int argc, char** argv) {
  if (perfetto::integration_tests::heapprofd_end_to_end_test_initializer) {
    perfetto::integration_tests::heapprofd_end_to_end_test_initializer();
  }
  if (perfetto::integration_tests::api_integration_test_initializer) {
    perfetto::integration_tests::api_integration_test_initializer();
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
