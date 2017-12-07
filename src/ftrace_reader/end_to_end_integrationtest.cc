/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <fstream>
#include <sstream>

#include "ftrace_procfs.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/ftrace_reader/ftrace_controller.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/ftrace_reader/test/scattered_stream_delegate_for_testing.h"

#include "protos/ftrace/ftrace_event_bundle.pb.h"
#include "protos/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/ftrace/test_bundle_wrapper.pb.h"
#include "protos/ftrace/test_bundle_wrapper.pbzero.h"

using testing::HasSubstr;
using testing::Not;

namespace perfetto {
namespace {

const size_t kPageSize = 4096;

const char kTracingPath[] = "/sys/kernel/debug/tracing/";

using BundleHandle =
    protozero::ProtoZeroMessageHandle<protos::pbzero::FtraceEventBundle>;

class EndToEndIntegrationTest : public ::testing::Test,
                                public FtraceSink::Delegate {
 public:
  void Finalize(protos::TestBundleWrapper* wrapper) {
    message->set_after("--- Bundle wrapper after ---");
    PERFETTO_CHECK(message);
    size_t msg_size = message->Finalize();
    std::unique_ptr<uint8_t[]> buffer = writer_delegate->StitchChunks(msg_size);
    wrapper->ParseFromArray(buffer.get(), static_cast<int>(msg_size));
    message.reset();
  }

 protected:
  virtual void SetUp() {
    writer_delegate = std::unique_ptr<ScatteredStreamDelegateForTesting>(
        new ScatteredStreamDelegateForTesting(kPageSize * 100));
    writer = std::unique_ptr<protozero::ScatteredStreamWriter>(
        new protozero::ScatteredStreamWriter(writer_delegate.get()));
    writer_delegate->set_writer(writer.get());
    message = std::unique_ptr<protos::pbzero::TestBundleWrapper>(
        new protos::pbzero::TestBundleWrapper);
    message->Reset(writer.get());
    message->set_before("--- Bundle wrapper before ---");
  }

  virtual BundleHandle GetBundleForCpu(size_t cpu) {
    PERFETTO_CHECK(!currently_writing_);
    currently_writing_ = true;
    cpu_being_written_ = cpu;
    return BundleHandle(message->add_bundle());
  }

  virtual void OnBundleComplete(size_t cpu, BundleHandle bundle) {
    PERFETTO_CHECK(currently_writing_);
    currently_writing_ = false;
    EXPECT_EQ(cpu_being_written_, cpu);
    if (!count--)
      runner_.Quit();
  }

  base::UnixTaskRunner* runner() { return &runner_; }

 private:
  size_t count = 3;
  base::UnixTaskRunner runner_;
  bool currently_writing_ = false;
  size_t cpu_being_written_ = 9999;
  std::unique_ptr<ScatteredStreamDelegateForTesting> writer_delegate = nullptr;
  std::unique_ptr<protozero::ScatteredStreamWriter> writer = nullptr;
  std::unique_ptr<protos::pbzero::TestBundleWrapper> message = nullptr;
};

}  // namespace

TEST_F(EndToEndIntegrationTest, SchedSwitchAndPrint) {
  FtraceProcfs procfs(kTracingPath);
  procfs.ClearTrace();
  procfs.WriteTraceMarker("Hello, World!");

  // Create a sink listening for our favorite events:
  std::unique_ptr<FtraceController> ftrace = FtraceController::Create(runner());
  FtraceConfig config(std::set<std::string>({"print", "sched_switch"}));
  std::unique_ptr<FtraceSink> sink = ftrace->CreateSink(config, this);

  // Let some events build up.
  sleep(1);

  // Start processing the tasks (OnBundleComplete will quit the task runner).
  runner()->Run();

  // Disable events.
  sink.reset();

  // Read the output into a full proto so we can use reflection.
  protos::TestBundleWrapper output;
  Finalize(&output);

  // Check we can see the guards:
  EXPECT_THAT(output.before(), HasSubstr("before"));
  EXPECT_THAT(output.after(), HasSubstr("after"));

  std::string output_as_text;
  google::protobuf::TextFormat::PrintToString(output, &output_as_text);
  printf("%s\n", output_as_text.c_str());
}

}  // namespace perfetto
