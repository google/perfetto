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
#include <memory>
#include <optional>
#include <string>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/status.h"
#include "src/trace_redaction/process_thread_timeline.h"

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

  // Ftrace packets contain a "one of" entry called "event". Within the scope of
  // a ftrace event, the event can be considered the payload and other other
  // values can be considered metadata (e.g. timestamp and pid).
  //
  // A ftrace event should be removed if:
  //
  //  ... we know it contains too much sensitive information
  //
  //  ... we know it contains sensitive information and we have some ideas on
  //      to remove it, but don't have the resources to do it right now (e.g.
  //      print).
  //
  //  ... we don't see value in including it
  //
  // "ftrace_packet_allow_list" contains field ids of ftrace packets that we
  // want to pass onto later transformations. An example would be:
  //
  //  ... kSchedWakingFieldNumber because it contains cpu activity information
  //
  // Compared against track days, the rules around removing ftrace packets are
  // complicated because...
  //
  //  packet {
  //    ftrace_packets {  <-- ONE-OF    (1)
  //      event {         <-- REPEATED  (2)
  //        cpu_idle { }  <-- ONE-OF    (3)
  //      }
  //      event { ... }
  //    }
  //  }
  //
  //  1.  A ftrace packet will populate the one-of slot in the trace packet.
  //
  //  2.  A ftrace packet can have multiple events
  //
  //  3.  In this example, a cpu_idle event populates the one-of slot in the
  //      ftrace event
  base::FlatSet<uint32_t> ftrace_packet_allow_list;

  //  message SuspendResumeFtraceEvent {
  //    optional string action = 1 [(datapol.semantic_type) = ST_NOT_REQUIRED];
  //    optional int32 val = 2;
  //    optional uint32 start = 3 [(datapol.semantic_type) = ST_NOT_REQUIRED];
  //  }
  //
  // The "action" in SuspendResumeFtraceEvent is a free-form string. There are
  // some know and expected values. Those values are stored here and all events
  // who's action value is not found here, the ftrace event will be dropped.
  base::FlatSet<std::string> suspend_result_allow_list;

  // The timeline is a query-focused data structure that connects a pid to a
  // uid at specific point in time.
  //
  // A timeline has two modes:
  //
  //    1. write-only
  //    2. read-only
  //
  // Attempting to use the timeline incorrectly results in undefined behaviour.
  //
  // To use a timeline, the primitive needs to be "built" (add events) and then
  // "sealed" (transition to read-only).
  //
  // A timeline must have Sort() called to change from write-only to read-only.
  // After Sort(), Flatten() and Reduce() can be called (optional) to improve
  // the practical look-up times (compared to theoretical look-up times).
  std::unique_ptr<ProcessThreadTimeline> timeline;
};

// Extracts low-level data from the trace and writes it into the context. The
// life cycle of a collect primitive is:
//
//  primitive.Begin(&context);
//
//  for (auto& packet : packets) {
//    primitive.Collect(packet, &context);
//  }
//
//  primitive.End(&context);
class CollectPrimitive {
 public:
  virtual ~CollectPrimitive();

  // Called once before the first call to Collect(...).
  virtual base::Status Begin(Context*) const;

  // Reads a trace packet and updates the context.
  virtual base::Status Collect(const protos::pbzero::TracePacket::Decoder&,
                               Context*) const = 0;

  // Called once after the last call to Collect(...).
  virtual base::Status End(Context*) const;
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
