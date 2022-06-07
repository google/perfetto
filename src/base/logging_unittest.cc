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

#include "perfetto/base/logging.h"

#include <stdint.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "perfetto/ext/base/crash_keys.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/log_ring_buffer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

char g_last_line[256];

TEST(LoggingTest, Basic) {
  SetLogMessageCallback(nullptr);
  LogMessage(kLogDebug, "file.cc", 100, "test message %d", 1);

  SetLogMessageCallback(+[](LogMessageCallbackArgs log) {
    base::SprintfTrunc(g_last_line, sizeof(g_last_line), "%d:%s:%d:%s",
                       log.level, log.filename, log.line, log.message);
  });

  g_last_line[0] = 0;
  LogMessage(kLogDebug, "file.cc", 101, "test message %d", 2);
  ASSERT_STREQ(g_last_line, "0:file.cc:101:test message 2");

  g_last_line[0] = 0;
  SetLogMessageCallback(nullptr);
  LogMessage(kLogDebug, "file.cc", 102, "test message %d", 3);
  ASSERT_STREQ(g_last_line, "");
}

TEST(LogRingBufferTest, SimpleCases) {
  char buf[4096];
  memset(buf, 'x', sizeof(buf));  // Deliberately not 0-initialized.

  LogRingBuffer lrb;
  EXPECT_EQ(0u, lrb.Read(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "");

  // Append one entry and read back.
  lrb.Append("tstamp1,", "src1.cc", "message1");
  EXPECT_EQ(25u, lrb.Read(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "tstamp1,src1.cc message1\n");

  lrb.Append("tstamp2,", "src2.cc", "message2");
  EXPECT_EQ(50u, lrb.Read(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "tstamp1,src1.cc message1\ntstamp2,src2.cc message2\n");
}

TEST(LogRingBufferTest, Truncation) {
  // Append a long entry that overflows the event slot.
  std::string long_msg;
  long_msg.resize(kLogRingBufMsgLen * 2);
  for (size_t i = 0; i < long_msg.size(); i++)
    long_msg[i] = static_cast<char>('a' + (i % 27));
  LogRingBuffer lrb;
  lrb.Append("A", "B", StringView(long_msg));

  // Check that it gets truncated with no side effects.
  char buf[4096];
  memset(buf, 'x', sizeof(buf));  // Deliberately not 0-initialized.
  auto expected = "AB " + long_msg.substr(0, kLogRingBufMsgLen - 4) + "\n";
  EXPECT_EQ(expected.size(), lrb.Read(buf, sizeof(buf)));
  EXPECT_EQ(buf, expected);

  // Append a short message and check everything still works.
  lrb.Append("X", "Y", "foo");
  EXPECT_EQ(expected.size() + 7, lrb.Read(buf, sizeof(buf)));
  EXPECT_EQ(buf, expected + "XY foo\n");
}

TEST(LogRingBufferTest, Wrapping) {
  LogRingBuffer lrb;

  std::vector<std::string> expected_logs;
  for (uint32_t i = 0; i < 128; i++) {
    std::string id = std::to_string(i);
    std::string tstamp = "tstamp" + id + ",";
    std::string src = "src";
    std::string msg;
    msg.resize(1 + (i % 16));
    for (size_t c = 0; c < msg.size(); c++)
      msg[c] = static_cast<char>('a' + c);
    lrb.Append(StringView(tstamp), StringView(src), StringView(msg));
    auto expected_log =
        (tstamp + src + " " + msg).substr(0, kLogRingBufMsgLen) + "\n";
    expected_logs.emplace_back(expected_log);
  }

  std::string expected;
  for (size_t i = expected_logs.size() - kLogRingBufEntries;
       i < expected_logs.size(); i++) {
    expected += expected_logs[i];
  }

  char buf[kLogRingBufMsgLen * kLogRingBufEntries];
  memset(buf, 'x', sizeof(buf));  // Deliberately not 0-initialized.
  lrb.Read(buf, sizeof(buf));
  EXPECT_EQ(buf, expected);

  // Do a partial readback which will cause output truncation.
  lrb.Read(buf, 127);
  EXPECT_EQ(buf, expected.substr(0, 127 - 1));  // - 1 for the NUL terminator.
}

// Writes concurrently into the ring buffer and check that all the events are
// seen in some order.
TEST(LogRingBufferTest, MultiThreadedWrites) {
  LogRingBuffer lrb;

  std::vector<std::thread> threads;
  const size_t kNumThreads = 8;

  std::mutex mutex;
  std::condition_variable cond;
  bool sync_start = false;

  auto thread_main = [&](size_t thread_idx) {
    std::unique_lock<std::mutex> lock(mutex);
    cond.wait(lock, [&] { return sync_start; });

    std::string tstamp = "ts" + std::to_string(thread_idx) + ",";
    std::string src = "src";
    std::string msg(thread_idx + 1, '.');  // A variable number of dots.
    lrb.Append(StringView(tstamp), StringView(src), StringView(msg));
  };

  std::vector<std::string> expected_events;
  for (size_t i = 0; i < kNumThreads; i++) {
    threads.emplace_back(thread_main, i);
    std::string id = std::to_string(i);
    expected_events.emplace_back("ts" + id + ",src " + std::string(i + 1, '.'));
  }

  // Unlock all the threads as close as possible to maximize races.
  {
    std::unique_lock<std::mutex> lock(mutex);
    sync_start = true;
    cond.notify_all();
  }

  for (auto& thread : threads)
    thread.join();

  char buf[kLogRingBufEntries * 40];
  memset(buf, 'x', sizeof(buf));  // Deliberately not 0-initialized.
  lrb.Read(buf, sizeof(buf));

  std::vector<std::string> actual_events = SplitString(buf, "\n");
  EXPECT_THAT(actual_events,
              testing::UnorderedElementsAreArray(expected_events));
}

TEST(CrashKeysTest, SetClearAndLongKeys) {
  UnregisterAllCrashKeysForTesting();

  char buf[1024];
  memset(buf, 'x', sizeof(buf));
  EXPECT_EQ(0u, SerializeCrashKeys(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "");

  CrashKey k1("key1");
  CrashKey k2("key2");
  CrashKey k3("key3");
  CrashKey k4("key4");

  k1.Set(0);
  k1.Clear();

  k2.Set(42);

  k3.Set("xx");
  k3.Clear();

  k4.Set("value");

  EXPECT_EQ(21u, SerializeCrashKeys(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "key2: 42\nkey4: value\n");

  EXPECT_EQ(0u, SerializeCrashKeys(buf, 0));

  EXPECT_EQ(0u, SerializeCrashKeys(buf, 1));
  EXPECT_STREQ(buf, "");

  // Test truncated output.
  EXPECT_EQ(5u, SerializeCrashKeys(buf, 5 + 1));
  EXPECT_STREQ(buf, "key2:");

  k2.Clear();

  std::string long_str(1024, 'x');
  k4.Set(StringView(long_str));

  EXPECT_EQ(6 + kCrashKeyMaxStrSize, SerializeCrashKeys(buf, sizeof(buf)));
  std::string expected =
      "key4: " + long_str.substr(0, kCrashKeyMaxStrSize - 1) + "\n";
  EXPECT_EQ(buf, expected);

  UnregisterAllCrashKeysForTesting();
}

TEST(CrashKeysTest, ScopedSet) {
  UnregisterAllCrashKeysForTesting();

  char buf[1024];
  memset(buf, 'x', sizeof(buf));

  CrashKey k1("key1");
  CrashKey k2("key2");

  auto scoped_key = k1.SetScoped(42);
  EXPECT_GT(SerializeCrashKeys(buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "key1: 42\n");

  {
    auto scoped_key2 = k2.SetScoped("foo");
    EXPECT_GT(SerializeCrashKeys(buf, sizeof(buf)), 0u);
    EXPECT_STREQ(buf, "key1: 42\nkey2: foo\n");
  }

  EXPECT_GT(SerializeCrashKeys(buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "key1: 42\n");

  k1.Clear();
  EXPECT_EQ(0u, SerializeCrashKeys(buf, sizeof(buf)));
  EXPECT_STREQ(buf, "");

  UnregisterAllCrashKeysForTesting();
}

}  // namespace
}  // namespace base
}  // namespace perfetto
