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

#include "src/trace_redaction/trace_redactor.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "protos/perfetto/trace/android/packages_list.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

TEST(TraceRedactorTest, EmptyTimelineReturnsError) {
  auto input_file = base::TempFile::Create();
  auto output_file = base::TempFile::Create();

  protos::gen::Trace trace;
  auto* packet = trace.add_packet();
  packet->set_trusted_uid(9999);

  auto* packages = packet->mutable_packages_list();
  auto* package = packages->add_packages();
  package->set_uid(1037);
  package->set_name("com.example.package");

  std::string serialized = trace.SerializeAsString();

  ASSERT_EQ(
      base::WriteAll(input_file.fd(), serialized.data(), serialized.size()),
      static_cast<ssize_t>(serialized.size()));

  TraceRedactor::Config config;
  config.verify = false;

  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = "com.example.package";

  auto status =
      redactor->Redact(input_file.path(), output_file.path(), &context);

  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.message(),
            "TraceRedactor: No process timeline found. Are sched_free or "
            "process stats data sources missing");
}

}  // namespace perfetto::trace_redaction
