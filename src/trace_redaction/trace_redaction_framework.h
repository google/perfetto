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

#ifndef SRC_TRACE_REDACTION_TRACE_REDACTION_FRAMEWORK_H_
#define SRC_TRACE_REDACTION_TRACE_REDACTION_FRAMEWORK_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/status_or.h"

#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Multiple packages can share the same name. This is common when a device has
// multiple users. When this happens, each instance shares the 5 least
// significant digits.
constexpr uint64_t NormalizeUid(uint64_t uid) {
  return uid % 1000000;
}

// Primitives should be stateless. All state should be stored in the context.
// Primitives should depend on data in the context, not the origin of the data.
// This allows primitives to be swapped out or work together to populate data
// needed by another primitive.
//
// For this to work, primitives are divided into three types:
//
//  `CollectPrimitive` :  Reads data from trace packets and saves low-level data
//                        in the context.
//
//  `BuildPrimitive` :    Reads low-level data from the context and builds
//                        high-level (read-optimized) data structures.
//
//  `TransformPrimitive`: Reads high-level data from the context and modifies
//                        trace packets.
class Context {
 public:
  // The package that should not be redacted. This must be populated before
  // running any primitives.
  std::string package_name;

  // The package list maps a package name to a uid. It is possible for multiple
  // package names to map to the same uid, for example:
  //
  //    packages {
  //      name: "com.google.android.gms"
  //      uid: 10113
  //      debuggable: false
  //      profileable_from_shell: false
  //      version_code: 235013038
  //    }
  //    packages {
  //      name: "com.google.android.gsf"
  //      uid: 10113
  //      debuggable: false
  //      profileable_from_shell: false
  //      version_code: 34
  //    }
  //
  // The process tree maps processes to packages via the uid value. However
  // multiple processes can map to the same uid, only differed by some multiple
  // of 100000, for example:
  //
  //    processes {
  //      pid: 18176
  //      ppid: 904
  //      cmdline: "com.google.android.gms.persistent"
  //      uid: 10113
  //    }
  //    processes {
  //      pid: 21388
  //      ppid: 904
  //      cmdline: "com.google.android.gms.persistent"
  //      uid: 1010113
  //    }
  std::optional<uint64_t> package_uid;

  // Trace packets contain a "one of" entry called "data". This field can be
  // thought of as the message. A track packet with have other fields along
  // side "data" (e.g. "timestamp"). These fields can be thought of as metadata.
  //
  // A message should be removed if:
  //
  //  ...we know it contains too much sensitive information
  //
  //  ...we know it contains sensitive information and we know how to remove
  //        the sensitive information, but don't have the resources to do it
  //        right now
  //
  //  ...we know it provide little value
  //
  // "trace_packet_allow_list" contains the field ids of trace packets we want
  // to pass onto later transformations. Examples are:
  //
  //    - protos::pbzero::TracePacket::kProcessTreeFieldNumber
  //    - protos::pbzero::TracePacket::kProcessStatsFieldNumber
  //    - protos::pbzero::TracePacket::kClockSnapshotFieldNumber
  //
  // Because "data" is a "one of", if no field in "trace_packet_allow_list" can
  // be found, it packet should be removed.
  base::FlatSet<uint32_t> trace_packet_allow_list;
};

// Responsible for extracting low-level data from the trace and storing it in
// the context.
class CollectPrimitive {
 public:
  // When a collect primitive has collected all necessary information, it can
  // stop processing packets by returning kRetire. If the primitives wants to
  // continue processing packets, it will return kNextPacket.
  //
  // If a collector encounters an unrecoverable error, base::ErrStatus() is
  // returned.
  enum class ContinueCollection : bool { kRetire = false, kNextPacket = true };

  virtual ~CollectPrimitive();

  // Processes a packet and writes low-level data to the context. Returns
  // kContinue if the primitive wants more data (i.e. next packet).
  virtual base::StatusOr<ContinueCollection> Collect(
      const protos::pbzero::TracePacket::Decoder& packet,
      Context* context) const = 0;
};

// Responsible for converting low-level data from the context and storing it in
// the context (high-level data).
class BuildPrimitive {
 public:
  virtual ~BuildPrimitive();

  // Reads low-level data from the context and writes high-level data to the
  // context.
  virtual base::Status Build(Context* context) const = 0;
};

// Responsible for modifying trace packets using data from the context.
class TransformPrimitive {
 public:
  virtual ~TransformPrimitive();

  // Modifies a packet using data from the context.
  virtual base::Status Transform(const Context& context,
                                 std::string* packet) const = 0;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_TRACE_REDACTION_FRAMEWORK_H_
