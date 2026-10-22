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

#include "src/trace_redaction/merge_process_tree.h"

#include <cstdint>
#include <string>
#include <vector>

#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::gen::TracePacket;
using ProcessTree = protos::gen::ProcessTree;

void AddProcess(ProcessTree* tree,
                int32_t pid,
                int32_t ppid,
                int32_t uid,
                const std::vector<std::string>& cmdlines = {},
                bool is_kthread = false) {
  auto* process = tree->add_processes();
  process->set_pid(pid);
  process->set_ppid(ppid);
  process->set_uid(uid);
  process->set_is_kthread(is_kthread);
  for (const auto& cmd : cmdlines) {
    process->add_cmdline(cmd);
  }
}

void AddThread(ProcessTree* tree,
               int32_t tid,
               int32_t tgid,
               const std::string& name = "") {
  auto* thread = tree->add_threads();
  thread->set_tid(tid);
  thread->set_tgid(tgid);
  if (!name.empty()) {
    thread->set_name(name);
  }
}

const ProcessTree::Process* FindProcessByPid(const ProcessTree& tree,
                                             int32_t pid) {
  for (const auto& process : tree.processes()) {
    if (process.pid() == pid) {
      return &process;
    }
  }
  return nullptr;
}

const ProcessTree::Thread* FindThreadByTid(const ProcessTree& tree,
                                           int32_t tid) {
  for (const auto& thread : tree.threads()) {
    if (thread.tid() == tid) {
      return &thread;
    }
  }
  return nullptr;
}

}  // namespace

class MergeProcessTreeTest : public testing::Test {
 protected:
  void CollectPacket(const TracePacket& packet) {
    std::string buffer = packet.SerializeAsString();
    protos::pbzero::TracePacket::Decoder decoder(buffer);
    ASSERT_OK(collect_.Collect(decoder, &context_));
  }

  Context context_;
  CollectProcessTrees collect_;
  ReduceProcessTrees reduce_;
  AugmentProcessTrees augment_;
};

TEST_F(MergeProcessTreeTest, NoopWhenNoProcessTreeCollected) {
  std::string augmented_str;
  ASSERT_OK(augment_.Augment(context_, &augmented_str));
  ASSERT_TRUE(augmented_str.empty());

  TracePacket packet;
  packet.set_timestamp(100);
  packet.set_trusted_uid(10);
  CollectPacket(packet);

  augmented_str.clear();
  ASSERT_OK(augment_.Augment(context_, &augmented_str));
  ASSERT_TRUE(augmented_str.empty());
}

TEST_F(MergeProcessTreeTest, CollectsAndAugmentsProcessesAndThreads) {
  TracePacket packet;
  packet.set_timestamp(1000);
  packet.set_trusted_uid(9999);

  auto* process_tree = packet.mutable_process_tree();
  AddProcess(process_tree, 10, 1, 1000, {"/bin/foo", "--arg"}, false);
  AddProcess(process_tree, 20, 2, 2000, {"/bin/bar"}, true);
  AddProcess(process_tree, 30, 3, 3000, {}, false);
  AddThread(process_tree, 11, 10, "thread_a");
  AddThread(process_tree, 21, 20, "thread_b");
  AddThread(process_tree, 31, 30, "");

  CollectPacket(packet);

  std::string augmented_str;
  ASSERT_OK(augment_.Augment(context_, &augmented_str));
  ASSERT_FALSE(augmented_str.empty());

  TracePacket augmented;
  ASSERT_TRUE(augmented.ParseFromString(augmented_str));

  ASSERT_TRUE(augmented.has_timestamp());
  ASSERT_EQ(augmented.timestamp(), 1000u);
  ASSERT_TRUE(augmented.has_trusted_uid());
  ASSERT_EQ(augmented.trusted_uid(), 9999);

  ASSERT_TRUE(augmented.has_process_tree());
  const auto& tree = augmented.process_tree();
  ASSERT_EQ(tree.processes_size(), 3);
  ASSERT_EQ(tree.threads_size(), 3);

  const auto* p10 = FindProcessByPid(tree, 10);
  ASSERT_NE(p10, nullptr);
  ASSERT_EQ(p10->ppid(), 1);
  ASSERT_EQ(p10->uid(), 1000);
  ASSERT_FALSE(p10->is_kthread());
  ASSERT_EQ(p10->cmdline_size(), 2);
  ASSERT_EQ(p10->cmdline()[0], "/bin/foo");
  ASSERT_EQ(p10->cmdline()[1], "--arg");

  const auto* p20 = FindProcessByPid(tree, 20);
  ASSERT_NE(p20, nullptr);
  ASSERT_EQ(p20->ppid(), 2);
  ASSERT_EQ(p20->uid(), 2000);
  ASSERT_TRUE(p20->is_kthread());
  ASSERT_EQ(p20->cmdline_size(), 1);
  ASSERT_EQ(p20->cmdline()[0], "/bin/bar");

  const auto* p30 = FindProcessByPid(tree, 30);
  ASSERT_NE(p30, nullptr);
  ASSERT_EQ(p30->ppid(), 3);
  ASSERT_EQ(p30->uid(), 3000);
  ASSERT_FALSE(p30->is_kthread());
  ASSERT_EQ(p30->cmdline_size(), 0);

  const auto* t11 = FindThreadByTid(tree, 11);
  ASSERT_NE(t11, nullptr);
  ASSERT_EQ(t11->tgid(), 10);
  ASSERT_EQ(t11->name(), "thread_a");

  const auto* t21 = FindThreadByTid(tree, 21);
  ASSERT_NE(t21, nullptr);
  ASSERT_EQ(t21->tgid(), 20);
  ASSERT_EQ(t21->name(), "thread_b");

  const auto* t31 = FindThreadByTid(tree, 31);
  ASSERT_NE(t31, nullptr);
  ASSERT_EQ(t31->tgid(), 30);
  ASSERT_FALSE(t31->has_name());
  ASSERT_TRUE(t31->name().empty());

  // A second call to Augment() should leave packet empty since all
  // collected processes and threads have been emitted.
  std::string second_str;
  ASSERT_OK(augment_.Augment(context_, &second_str));
  ASSERT_TRUE(second_str.empty());
}

TEST_F(MergeProcessTreeTest, DeduplicatesProcessesByPid) {
  int32_t pid1 = 100;
  int32_t tid1 = 200;
  int32_t tid2 = 201;
  TracePacket packet1;

  packet1.set_timestamp(1000);
  packet1.set_trusted_uid(9999);
  auto* tree1 = packet1.mutable_process_tree();
  AddProcess(tree1, pid1, 1, 1000, {"first_cmd"}, false);
  AddThread(tree1, tid1, pid1, "thread_1");
  CollectPacket(packet1);

  TracePacket packet2;
  packet2.set_timestamp(2000);
  packet2.set_trusted_uid(8888);
  auto* tree2 = packet2.mutable_process_tree();
  AddProcess(tree2, pid1, 99, 9999, {"second_cmd"}, true);
  AddThread(tree2, tid2, pid1, "thread_2");
  CollectPacket(packet2);

  std::string augmented_str;
  ASSERT_OK(augment_.Augment(context_, &augmented_str));

  // Make sure any subsequent call to Augment() leaves packet empty since
  // we should only emit a single merged process tree.
  std::string second_str;
  ASSERT_OK(augment_.Augment(context_, &second_str));
  ASSERT_TRUE(second_str.empty());

  // Verify that we generated a merged process tree given
  // that we collected 2 process tree packets.
  ASSERT_FALSE(augmented_str.empty());
  TracePacket augmented;
  ASSERT_TRUE(augmented.ParseFromString(augmented_str));
  ASSERT_TRUE(augmented.has_process_tree());
  const auto& tree = augmented.process_tree();

  // Verify all the processes and threads are present exactly once
  // pid1 was a dupe, so it should have been removed.
  ASSERT_EQ(tree.processes_size(), 1);
  ASSERT_EQ(tree.threads_size(), 2);

  const auto* process1 = FindProcessByPid(tree, pid1);
  ASSERT_NE(process1, nullptr);
  ASSERT_EQ(process1->ppid(), 1);
  ASSERT_EQ(process1->uid(), 1000);
  ASSERT_FALSE(process1->is_kthread());
  ASSERT_EQ(process1->cmdline_size(), 1);
  ASSERT_EQ(process1->cmdline()[0], "first_cmd");
}

TEST_F(MergeProcessTreeTest, DeduplicatesThreadsByTid) {
  int32_t tid1 = 101;
  int32_t pid1 = 10;
  int32_t pid2 = 20;
  TracePacket packet1;
  packet1.set_timestamp(1000);
  packet1.set_trusted_uid(9999);
  auto* tree1 = packet1.mutable_process_tree();
  AddProcess(tree1, pid1, 1, 1000, {"cmd1"}, false);
  AddThread(tree1, tid1, pid1, "first_name");
  CollectPacket(packet1);

  TracePacket packet2;
  packet2.set_timestamp(2000);
  packet2.set_trusted_uid(8888);
  auto* tree2 = packet2.mutable_process_tree();
  AddProcess(tree2, pid2, 1, 2000, {"cmd2"}, false);
  AddThread(tree2, tid1, pid1, "first_name");
  CollectPacket(packet2);

  std::string augmented_str;
  ASSERT_OK(augment_.Augment(context_, &augmented_str));

  // Verify that we generated a merged process tree given
  // that we collected 2 process tree packets.
  ASSERT_FALSE(augmented_str.empty());

  // Make sure any subsequent call to Augment() leaves packet empty since
  // we should only emit a single merged process tree.
  std::string second_str;
  ASSERT_OK(augment_.Augment(context_, &second_str));
  ASSERT_TRUE(second_str.empty());

  TracePacket augmented;
  ASSERT_TRUE(augmented.ParseFromString(augmented_str));
  ASSERT_TRUE(augmented.has_process_tree());
  const auto& tree = augmented.process_tree();

  // Verify that that threads exist only once.
  ASSERT_EQ(tree.processes_size(), 2);
  ASSERT_EQ(tree.threads_size(), 1);

  const auto* thread1 = FindThreadByTid(tree, tid1);
  ASSERT_NE(thread1, nullptr);
  ASSERT_EQ(thread1->tgid(), pid1);
  ASSERT_EQ(thread1->name(), "first_name");

  ASSERT_EQ(augmented.timestamp(), 1000u);
  ASSERT_EQ(augmented.trusted_uid(), 9999);
}

TEST_F(MergeProcessTreeTest, ReduceClearsProcessTreePacket) {
  TracePacket packet;
  packet.set_timestamp(100);
  packet.set_trusted_uid(10);
  auto* tree = packet.mutable_process_tree();
  AddProcess(tree, 10, 1, 1000, {"cmd"}, false);

  std::string packet_str = packet.SerializeAsString();
  ASSERT_FALSE(packet_str.empty());

  ASSERT_OK(reduce_.Transform(context_, &packet_str));
  ASSERT_TRUE(packet_str.empty());
}

TEST_F(MergeProcessTreeTest, ReduceLeavesOtherPacketsUnchanged) {
  TracePacket packet;
  packet.set_timestamp(200);
  packet.set_trusted_uid(20);

  std::string packet_str = packet.SerializeAsString();
  ASSERT_FALSE(packet_str.empty());

  ASSERT_OK(reduce_.Transform(context_, &packet_str));
  ASSERT_FALSE(packet_str.empty());

  TracePacket after;
  ASSERT_TRUE(after.ParseFromString(packet_str));
  ASSERT_EQ(after.timestamp(), 200u);
  ASSERT_EQ(after.trusted_uid(), 20);
  ASSERT_FALSE(after.has_process_tree());
}

}  // namespace perfetto::trace_redaction
