
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

#include <cstdint>
#include <memory>
#include <string>

#include "src/trace_redaction/prune_package_list.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/packages_list.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {
using PackageList = protos::gen::PackagesList;
using TracePacket = protos::gen::TracePacket;
using ProcessTree = protos::gen::ProcessTree;

constexpr uint64_t kPackageUid = 1037;
constexpr std::string_view kPackageName =
    "com.google.android.networkstack.tethering";

void AddPackage(uint64_t uid, std::string_view name, PackageList* list) {
  auto* package = list->add_packages();
  package->set_uid(uid);
  package->set_name(std::string(name));
}

std::string CreateTestPacket() {
  auto packet = std::make_unique<TracePacket>();

  packet->set_trusted_uid(9999);
  packet->set_trusted_packet_sequence_id(2);
  packet->set_previous_packet_dropped(true);

  auto* packages = packet->mutable_packages_list();
  AddPackage(10205, "com.google.android.uvexposurereporter", packages);
  AddPackage(10007, "com.android.internal.display.cutout.emulation.noCutout",
             packages);
  AddPackage(kPackageUid, kPackageName, packages);
  AddPackage(10367, "com.android.systemui.clocks.metro", packages);

  return packet->SerializeAsString();
}

// packet {
//   process_tree {
//     processes {
//       pid: 23022
//       ppid: 1
//       cmdline: "/vendor/bin/hw/wpa_supplicant"
//       cmdline: "-O/data/vendor/wifi/wpa/sockets"
//       cmdline: "-dd"
//       cmdline: "-g@android:wpa_wlan0"
//       uid: 1010
//     }
//     threads {
//       tid: 6382
//       tgid: 18176
//     }
//     threads {
//      tid: 18419
//       tgid: 18176
//     }
//     threads {
//       tid: 18434
//       tgid: 18176
//     }
//     collection_end_timestamp: 333724398314653
//   }
//   trusted_uid: 9999
//   timestamp: 333724396714922
//   trusted_packet_sequence_id: 3
//   trusted_pid: 1085
// }
std::string CreateNoPackageListPacket() {
  auto packet = std::make_unique<TracePacket>();

  packet->set_trusted_uid(9999);
  packet->set_timestamp(333724396714922);
  packet->set_trusted_packet_sequence_id(3);
  packet->set_trusted_pid(1085);

  ProcessTree* tree = packet->mutable_process_tree();

  auto* p0 = tree->add_processes();
  p0->set_pid(23022);
  p0->set_ppid(1);
  p0->add_cmdline("/vendor/bin/hw/wpa_supplicant");
  p0->add_cmdline("-O/data/vendor/wifi/wpa/sockets");
  p0->add_cmdline("-dd");
  p0->add_cmdline("-g@android:wpa_wlan0");
  p0->set_uid(1010);

  auto* t0 = tree->add_threads();
  t0->set_tid(6382);
  t0->set_tgid(18176);

  auto* t1 = tree->add_threads();
  t1->set_tid(18419);
  t1->set_tgid(18176);

  auto* t2 = tree->add_threads();
  t2->set_tid(18434);
  t2->set_tgid(18176);

  tree->set_collection_end_timestamp(333724398314653);

  return packet->SerializeAsString();
}
}  // namespace

TEST(PrunePackageListTest, ReturnsErrorWhenPackageUidIsMissing) {
  auto before = CreateTestPacket();

  const Context context;
  const PrunePackageList prune;
  ASSERT_FALSE(prune.Transform(context, &before).ok());
}

TEST(PrunePackageListTest, NoopWhenThereIsNoPackageList) {
  Context context;
  context.package_uid.emplace(1037);

  const auto before = CreateNoPackageListPacket();
  auto after = CreateNoPackageListPacket();

  ASSERT_EQ(before, after);

  const PrunePackageList prune;
  ASSERT_TRUE(prune.Transform(context, &after).ok());

  // The buffer should have changed.
  ASSERT_EQ(before, after);
}

// PrunePackageList should not drop packets, instead it should drop individual
// PackageInfo entries.
TEST(PrunePackageListTest, RemovesPackagesInfoFromPackageList) {
  Context context;
  context.package_uid.emplace(1037);

  const auto before = CreateTestPacket();
  auto after = CreateTestPacket();

  ASSERT_EQ(before, after);

  const PrunePackageList prune;
  ASSERT_TRUE(prune.Transform(context, &after).ok());

  // The buffer should have changed.
  ASSERT_NE(before, after);

  protos::gen::TracePacket after_packet;
  after_packet.ParseFromString(after);

  ASSERT_TRUE(after_packet.has_packages_list());
  ASSERT_EQ(1, after_packet.packages_list().packages_size());

  ASSERT_TRUE(after_packet.packages_list().packages().at(0).has_uid());
  ASSERT_EQ(kPackageUid, after_packet.packages_list().packages().at(0).uid());

  ASSERT_TRUE(after_packet.packages_list().packages().at(0).has_name());
  ASSERT_EQ(kPackageName, after_packet.packages_list().packages().at(0).name());
}

}  // namespace perfetto::trace_redaction
