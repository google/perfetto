/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_IMPORTER_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_IMPORTER_MODULE_H_

#include "perfetto/ext/base/optional.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/trace_blob_view.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class TraceConfig_Decoder;
class TracePacket_Decoder;
}  // namespace pbzero
}  // namespace protos

namespace trace_processor {

class PacketSequenceState;
struct TimestampedTracePiece;
class TraceProcessorContext;

// This file contains helper and base class templates for
// ProtoTraceTokenizer/Parser modules. A module implements support for a subset
// of features of the TracePacket proto format. Modules inherit from
// ProtoImporterModuleBase, and should be instantiated using the
// ProtoImporterModule<> wrapper template in trace_processor_context.h.
//
// To add and integrate a new module:
// (1) Add MyModule as a subclass of ProtoImporterModuleBase<IsEnabled>,
//     defining the TokenizePacket() and/or ParsePacket() methods.
//     Typically, a build-time macro will inform the value of IsEnabled.
//     See ftrace_module.h for an example.
// (2) Add a member of type std::unique_ptr<ProtoImporterModule<MyModule>> to
//     TraceProcessorContext (trace_processor_context.h) and init it from
//     TraceProcessorImpl() and appropriate tests.
// (3) Add an include of my_module.h and calls to your module's TokenizePacket /
//     ParsePacket methods in ProtoTraceTokenizer and/or ProtoTraceParser
//     (proxying via the wrapper).

class ModuleResult {
 public:
  // Allow auto conversion from util::Status to Handled / Error result.
  ModuleResult(util::Status status)
      : ignored_(false),
        error_(status.ok() ? base::nullopt
                           : base::make_optional(status.message())) {}

  // Constructs a result that indicates the module ignored the packet and is
  // deferring the handling of the packet to other modules.
  static ModuleResult Ignored() { return ModuleResult(true); }

  // Constructs a result that indicates the module handled the packet. Other
  // modules will not be notified about the packet.
  static ModuleResult Handled() { return ModuleResult(false); }

  // Constructs a result that indicates an error condition while handling the
  // packet. Other modules will not be notified about the packet.
  static ModuleResult Error(const std::string& message) {
    return ModuleResult(message);
  }

  bool ignored() const { return ignored_; }
  bool ok() const { return !error_.has_value(); }
  const std::string& message() const { return *error_; }

  util::Status ToStatus() const {
    PERFETTO_DCHECK(!ignored_);
    if (error_)
      return util::Status(*error_);
    return util::OkStatus();
  }

 private:
  explicit ModuleResult(bool ignored) : ignored_(ignored) {}
  explicit ModuleResult(const std::string& error)
      : ignored_(false), error_(error) {}

  bool ignored_;
  base::Optional<std::string> error_;
};

// Wrapper class for a module. This wrapper allows modules to be disabled
// disabled at compile time to remove support for its features from the trace
// processor.
//
// The trace processor will instantiate enabled modules for each
// TraceProcessorContext. The tokenizer and parser notify individual modules
// about trace data by calling their respective methods via the wrapper class.
// If the module is enabled, the wrapper will forward the call to the module
// implementation. This way, we avoid virtual methods, so that calling any of
// the module's methods is zero overhead - they can be inlined by the compiler
// at callsites directly.
template <class ModuleType>
class ProtoImporterModule {
 public:
  ProtoImporterModule(TraceProcessorContext* context) {
    if (ModuleType::kEnabled)
      impl_.reset(new ModuleType(context));
  }

  // ModuleType may specify methods with the signatures below.
  // ProtoImporterModule<ModuleType> acts as a wrapper for these methods.
  // ModuleType only needs to specify the methods that
  // ProtoTraceParser/Tokenizer actually calls on the respective module.

  // Wraps ModuleType::TokenizePacket(). If the module is disabled, compiles
  // into a noop in optimized builds. Called by ProtoTraceTokenizer for each
  // TracePacket during the tokenization stage, i.e. before sorting. If this
  // returns a result other than ModuleResult::Ignored(), tokenization of the
  // packet will be aborted after the module.
  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket_Decoder& decoder,
      TraceBlobView* packet,
      int64_t packet_timestamp,
      PacketSequenceState* state) {
    if (ModuleType::kEnabled) {
      return impl_->TokenizePacket(decoder, packet, packet_timestamp, state);
    }
    return ModuleResult::Ignored();
  }

  // Wraps ModuleType::ParsePacket(). If the module is disabled, compiles into a
  // noop in optimized builds. Called by ProtoTraceParser for each non-ftrace
  // TracePacket after the sorting stage. If this returns a result other than
  // ModuleResult::Ignored(), parsing of the packet will be aborted after the
  // module.
  ModuleResult ParsePacket(const protos::pbzero::TracePacket_Decoder& decoder,
                           const TimestampedTracePiece& ttp) {
    if (ModuleType::kEnabled)
      return impl_->ParsePacket(decoder, ttp);
    return ModuleResult::Ignored();
  }

  // Wraps ModuleType::ParseTraceConfig(). If the module is disabled, compiles
  // into a noop in optimized builds. Called by ProtoTraceParser for trace
  // config packets after the sorting stage.
  ModuleResult ParseTraceConfig(
      const protos::pbzero::TraceConfig_Decoder& decoder) {
    if (ModuleType::kEnabled)
      return impl_->ParseTraceConfig(decoder);
    return ModuleResult::Ignored();
  }

  // For FtraceModule only. Wraps ModuleType::ParseFtracePacket(). If the module
  // is disabled, compiles into a noop in optimized builds. Called by
  // ProtoTraceParser for each ftrace TracePacket after the sorting stage.
  // Ftrace packets are handled specially here because they are sorted in
  // separate queues per CPU. If this returns a result other than
  // ModuleResult::Ignored(), parsing of the packet will be aborted after the
  // module.
  ModuleResult ParseFtracePacket(uint32_t cpu,
                                 const TimestampedTracePiece& ttp) {
    if (ModuleType::kEnabled)
      return impl_->ParseFtracePacket(cpu, ttp);
    return ModuleResult::Ignored();
  }

 private:
  // Only initialized if the module is enabled.
  std::unique_ptr<ModuleType> impl_;
};

// Base class for a proto trace module that can be disabled at compile time.
// Typically, a build-time macro will inform the value of IsEnabled.
template <int IsEnabled>
class ProtoImporterModuleBase {
 public:
  static constexpr bool kEnabled = static_cast<bool>(IsEnabled);

  explicit ProtoImporterModuleBase(TraceProcessorContext* context)
      : context_(context) {}
  ~ProtoImporterModuleBase() {}

  // See ProtoTraceModule<> for the public methods subclasses may implement.

 protected:
  TraceProcessorContext* context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_IMPORTER_MODULE_H_
