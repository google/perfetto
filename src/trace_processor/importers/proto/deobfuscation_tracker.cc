#include "src/trace_processor/importers/proto/deobfuscation_tracker.h"
#include "perfetto/protozero/field.h"

namespace perfetto::trace_processor {

DeobfuscationTracker::DeobfuscationTracker() = default;

DeobfuscationTracker::~DeobfuscationTracker() = default;

void DeobfuscationTracker::AddDeobfuscationPacket(protozero::ConstBytes data) {
  packets_.emplace_back(TraceBlob::CopyFrom(data.data, data.size));
}

}  // namespace perfetto::trace_processor
