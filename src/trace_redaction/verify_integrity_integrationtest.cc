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

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_integration_fixture.h"
#include "src/trace_redaction/trace_redactor.h"
#include "src/trace_redaction/verify_integrity.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

class VerifyIntegrityIntegrationTest : public testing::Test,
                                       public TraceRedactionIntegrationFixure {
};

// Because the test trace has failed patches, if we verify failed patches (i.e.
// equal 0), redaction will failed. To cover this, these one without verifying
// that field and a second time verifying that field.

TEST_F(VerifyIntegrityIntegrationTest, VerifiesWithoutVerifyingFailedPatches) {
  trace_redactor()->emplace_collect<VerifyIntegrity>();

  context()->verify_config.verify_failed_patches = false;
  ASSERT_OK(Redact());
}

TEST_F(VerifyIntegrityIntegrationTest, VerifiesWithVerifyingFailedPatches) {
  trace_redactor()->emplace_collect<VerifyIntegrity>();

  // By default context() has verify_trace_config.verify_failed_patches = true.
  ASSERT_FALSE(Redact().ok());
}

}  // namespace perfetto::trace_redaction
