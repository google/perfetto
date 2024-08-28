
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
#include <string>

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/find_package_uid.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/packages_list.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {

// packet {
//   trusted_uid: 9999
//   trusted_packet_sequence_id: 2
//   previous_packet_dropped: true
//   packages_list {
//     packages {
//       name: "com.shannon.qualifiednetworksservice"
//       uid: 10201
//       debuggable: false
//       profileable_from_shell: false
//       version_code: 131
//     }
//     packages {
//       name: "com.google.android.uvexposurereporter"
//       uid: 10205
//       debuggable: false
//       profileable_from_shell: false
//       version_code: 34
//     }
//     packages {
//       name: "com.android.internal.display.cutout.emulation.noCutout"
//       uid: 10007
//       debuggable: false
//       profileable_from_shell: false
//       version_code: 1
//     }
//     packages {
//       name: "com.google.android.networkstack.tethering"
//       uid: 1073
//       debuggable: false
//       profileable_from_shell: false
//       version_code: 34
//     }
//     packages {
//       name: "com.amazon.mShop.android.shopping"
//       uid: 10303
//       debuggable: false
//       profileable_from_shell: false
//       version_code: 1241261011
//     }
//   }
//   trusted_pid: 1085
//   first_packet_on_sequence: true
// }
//
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

void AddPackage(const std::string& name,
                uint64_t uid,
                int version_code,
                protos::gen::PackagesList* list) {
  auto* pkg = list->add_packages();
  pkg->set_name(name);
  pkg->set_uid(uid);
  pkg->set_version_code(version_code);
}

void AddProcess(protos::gen::ProcessTree* process_tree) {
  auto* p0 = process_tree->add_processes();
  p0->set_pid(23022);
  p0->set_ppid(1);
  p0->add_cmdline("/vendor/bin/hw/wpa_supplicant");
  p0->add_cmdline("-O/data/vendor/wifi/wpa/sockets");
  p0->add_cmdline("-dd");
  p0->add_cmdline("-g@android:wpa_wlan0");
  p0->set_uid(1010);
}

void AddThread(int32_t tid,
               int32_t ttid,
               protos::gen::ProcessTree* process_tree) {
  auto* thread = process_tree->add_threads();
  thread->set_tid(tid);
  thread->set_tgid(ttid);
}

std::string CreatePackageListPacket() {
  protos::gen::TracePacket packet;
  packet.set_trusted_uid(9999);
  packet.set_trusted_packet_sequence_id(2);
  packet.set_previous_packet_dropped(true);
  packet.set_trusted_pid(1085);
  packet.set_first_packet_on_sequence(true);

  auto* package_list = packet.mutable_packages_list();

  AddPackage("com.shannon.qualifiednetworksservice", 10201, 131, package_list);
  AddPackage("com.google.android.uvexposurereporter", 10205, 34, package_list);
  AddPackage("com.android.internal.display.cutout.emulation.noCutout", 10007, 1,
             package_list);
  AddPackage("com.google.android.networkstack.tethering", 1073, 34,
             package_list);
  AddPackage("com.amazon.mShop.android.shopping", 10303, 1241261011,
             package_list);

  return packet.SerializeAsString();
}

std::string CreateProcessTreePacket() {
  protos::gen::TracePacket packet;
  packet.set_trusted_uid(9999);
  packet.set_timestamp(333724396714922);
  packet.set_trusted_packet_sequence_id(3);
  packet.set_trusted_pid(1085);

  auto* tree = packet.mutable_process_tree();
  tree->set_collection_end_timestamp(333724398314653);

  AddProcess(tree);
  AddThread(6382, 18176, tree);
  AddThread(18419, 18176, tree);
  AddThread(18434, 18176, tree);

  return packet.SerializeAsString();
}
}  // namespace

TEST(FindPackageUidTest, FindsUidInPackageList) {
  const auto packet = CreatePackageListPacket();

  Context context;
  context.package_name = "com.google.android.uvexposurereporter";

  const FindPackageUid find;

  const auto decoder = protos::pbzero::TracePacket::Decoder(packet);

  base::Status status = find.Begin(&context);
  ASSERT_OK(status) << status.message();

  status = find.Collect(decoder, &context);
  ASSERT_OK(status) << status.message();

  status = find.End(&context);
  ASSERT_OK(status) << status.message();

  ASSERT_TRUE(context.package_uid.has_value());

  // context.package_uid should have been normalized already.
  ASSERT_EQ(context.package_uid.value(), NormalizeUid(10205));
}

TEST(FindPackageUidTest, ContinuesOverNonPackageList) {
  const auto packet = CreateProcessTreePacket();

  Context context;
  context.package_name = "com.google.android.uvexposurereporter";

  const FindPackageUid find;

  const auto decoder = protos::pbzero::TracePacket::Decoder(packet);

  base::Status status = find.Begin(&context);
  ASSERT_OK(status) << status.message();

  status = find.Collect(decoder, &context);
  ASSERT_OK(status) << status.message();

  // The should not have been found; End() should return an error.
  status = find.End(&context);
  ASSERT_FALSE(status.ok()) << status.message();

  ASSERT_FALSE(context.package_uid.has_value());
}

TEST(FindPackageUidTest, ContinuesOverPackageListWithOutPackageName) {
  const auto packet = CreatePackageListPacket();

  Context context;
  context.package_name = "com.not.a.packagename";

  const FindPackageUid find;

  const auto decoder = protos::pbzero::TracePacket::Decoder(packet);

  base::Status status = find.Begin(&context);
  ASSERT_OK(status) << status.message();

  status = find.Collect(decoder, &context);
  ASSERT_OK(status) << status.message();

  // The should not have been found; End() should return an error.
  status = find.End(&context);
  ASSERT_FALSE(status.ok()) << status.message();

  ASSERT_FALSE(context.package_uid.has_value());
}

TEST(FindPackageUidTest, MissingPackageNameReturnsError) {
  const auto packet = CreatePackageListPacket();

  Context context;

  const FindPackageUid find;

  const auto decoder = protos::pbzero::TracePacket::Decoder(packet);

  base::Status status = find.Begin(&context);
  ASSERT_FALSE(status.ok()) << status.message();
}

TEST(FindPackageUidTest, FailsIfUidStartsInitialized) {
  const auto packet = CreatePackageListPacket();

  Context context;
  context.package_name = "com.google.android.uvexposurereporter";
  context.package_uid = 1000;

  const FindPackageUid find;

  const auto decoder = protos::pbzero::TracePacket::Decoder(packet);

  base::Status status = find.Begin(&context);
  ASSERT_FALSE(status.ok()) << status.message();
}

}  // namespace perfetto::trace_redaction
