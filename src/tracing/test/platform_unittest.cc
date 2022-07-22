/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/tracing/platform.h"

#include <atomic>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/tracing/internal/tracing_tls.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

internal::TracingTLS* GetTLS() {
  return static_cast<internal::TracingTLS*>(
      Platform::GetDefaultPlatform()->GetOrCreateThreadLocalObject());
}

// We use this class only as a listener to detect thread-local destruction.
class FakeTraceWriter : public TraceWriterBase {
 public:
  std::atomic<bool>* destroyed_flag;

  ~FakeTraceWriter() override { *destroyed_flag = true; }
  protozero::MessageHandle<protos::pbzero::TracePacket> NewTracePacket()
      override {
    PERFETTO_CHECK(false);
  }
  void FinishTracePacket() override { PERFETTO_CHECK(false); }
  void Flush(std::function<void()>) override {}
  uint64_t written() const override { return 0; }
};

// This test mainly checks that the thread at-exit logic works properly and
// destroys the TracingTLS when a thread exits.
TEST(PlatformUnittest, ThreadingAndTLSDtor) {
  auto* platform = Platform::GetDefaultPlatform();
  if (!platform)
    GTEST_SKIP() << "Platform::GetDefaultPlatform() not implemented";

  auto proc_name = platform->GetCurrentProcessName();
  EXPECT_FALSE(proc_name.empty());

  // Create two threads.

  Platform::CreateTaskRunnerArgs tr_args{};
  auto thread1 = platform->CreateTaskRunner(tr_args);
  ASSERT_TRUE(thread1);

  auto thread2 = platform->CreateTaskRunner(tr_args);
  ASSERT_TRUE(thread2);

  // Check that the TLS is actually thread-local.

  thread1->PostTask([] { GetTLS()->generation = 101; });
  thread2->PostTask([] { GetTLS()->generation = 102; });
  std::atomic<bool> thread1_destroyed{};
  std::atomic<bool> thread2_destroyed{};

  // Now post another task on each thread. The task will:
  // 1. Check that the generation matches what previously set.
  // 2. Create a FakeTraceWriter and wire up a destruction event.
  base::WaitableEvent evt1;
  thread1->PostTask([&] {
    EXPECT_EQ(GetTLS()->generation, 101u);
    GetTLS()->data_sources_tls[0].per_instance[0].Reset();
    std::unique_ptr<FakeTraceWriter> tw(new FakeTraceWriter());
    tw->destroyed_flag = &thread1_destroyed;
    GetTLS()->data_sources_tls[0].per_instance[0].trace_writer = std::move(tw);
    evt1.Notify();
  });
  evt1.Wait();

  base::WaitableEvent evt2;
  thread2->PostTask([&] {
    EXPECT_EQ(GetTLS()->generation, 102u);
    GetTLS()->data_sources_tls[0].per_instance[0].Reset();
    std::unique_ptr<FakeTraceWriter> tw(new FakeTraceWriter());
    tw->destroyed_flag = &thread2_destroyed;
    GetTLS()->data_sources_tls[0].per_instance[0].trace_writer = std::move(tw);
    evt2.Notify();
  });
  evt2.Wait();

  EXPECT_FALSE(thread1_destroyed);
  EXPECT_FALSE(thread2_destroyed);

  thread1.reset();
  EXPECT_TRUE(thread1_destroyed);
  EXPECT_FALSE(thread2_destroyed);

  thread2.reset();
  EXPECT_TRUE(thread2_destroyed);
}

}  // namespace
}  // namespace perfetto
