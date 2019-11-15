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

#include "src/trace_processor/importers/proto/heap_graph_module.h"

#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#include "protos/perfetto/trace/profiling/heap_graph.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {

const char* HeapGraphRootTypeToString(int32_t type) {
  switch (type) {
    case protos::pbzero::HeapGraphRoot::ROOT_UNKNOWN:
      return "ROOT_UNKNOWN";
    case protos::pbzero::HeapGraphRoot::ROOT_JNI_GLOBAL:
      return "ROOT_JNI_GLOBAL";
    case protos::pbzero::HeapGraphRoot::ROOT_JNI_LOCAL:
      return "ROOT_JNI_LOCAL";
    case protos::pbzero::HeapGraphRoot::ROOT_JAVA_FRAME:
      return "ROOT_JAVA_FRAME";
    case protos::pbzero::HeapGraphRoot::ROOT_NATIVE_STACK:
      return "ROOT_NATIVE_STACK";
    case protos::pbzero::HeapGraphRoot::ROOT_STICKY_CLASS:
      return "ROOT_STICKY_CLASS";
    case protos::pbzero::HeapGraphRoot::ROOT_THREAD_BLOCK:
      return "ROOT_THREAD_BLOCK";
    case protos::pbzero::HeapGraphRoot::ROOT_MONITOR_USED:
      return "ROOT_MONITOR_USED";
    case protos::pbzero::HeapGraphRoot::ROOT_THREAD_OBJECT:
      return "ROOT_THREAD_OBJECT";
    case protos::pbzero::HeapGraphRoot::ROOT_INTERNED_STRING:
      return "ROOT_INTERNED_STRING";
    case protos::pbzero::HeapGraphRoot::ROOT_FINALIZING:
      return "ROOT_FINALIZING";
    case protos::pbzero::HeapGraphRoot::ROOT_DEBUGGER:
      return "ROOT_DEBUGGER";
    case protos::pbzero::HeapGraphRoot::ROOT_REFERENCE_CLEANUP:
      return "ROOT_REFERENCE_CLEANUP";
    case protos::pbzero::HeapGraphRoot::ROOT_VM_INTERNAL:
      return "ROOT_VM_INTERNAL";
    case protos::pbzero::HeapGraphRoot::ROOT_JNI_MONITOR:
      return "ROOT_JNI_MONITOR";
    default:
      return "ROOT_UNKNOWN";
  }
}

}  // namespace

void HeapGraphModule::ParseHeapGraph(int64_t ts, protozero::ConstBytes blob) {
  protos::pbzero::HeapGraph::Decoder heap_graph(blob.data, blob.size);
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(heap_graph.pid()));
  context_->heap_graph_tracker->SetPacketIndex(heap_graph.index());
  for (auto it = heap_graph.objects(); it; ++it) {
    protos::pbzero::HeapGraphObject::Decoder object(*it);
    HeapGraphTracker::SourceObject obj;
    obj.object_id = object.id();
    obj.self_size = object.self_size();
    obj.type_id = object.type_id();
    auto ref_field_ids_it = object.reference_field_id();
    auto ref_object_ids_it = object.reference_object_id();
    for (; ref_field_ids_it && ref_object_ids_it;
         ++ref_field_ids_it, ++ref_object_ids_it) {
      HeapGraphTracker::SourceObject::Reference ref;
      ref.field_name_id = *ref_field_ids_it;
      ref.owned_object_id = *ref_object_ids_it;
      obj.references.emplace_back(std::move(ref));
    }

    if (ref_field_ids_it || ref_object_ids_it) {
      context_->storage->IncrementIndexedStats(stats::heap_graph_missing_packet,
                                               static_cast<int>(upid));
      continue;
    }
    context_->heap_graph_tracker->AddObject(upid, ts, std::move(obj));
  }
  for (auto it = heap_graph.type_names(); it; ++it) {
    protos::pbzero::InternedString::Decoder entry(*it);
    const char* str = reinterpret_cast<const char*>(entry.str().data);
    auto str_view = base::StringView(str, entry.str().size);

    context_->heap_graph_tracker->AddInternedTypeName(
        entry.iid(), context_->storage->InternString(str_view));
  }
  for (auto it = heap_graph.field_names(); it; ++it) {
    protos::pbzero::InternedString::Decoder entry(*it);
    const char* str = reinterpret_cast<const char*>(entry.str().data);
    auto str_view = base::StringView(str, entry.str().size);

    context_->heap_graph_tracker->AddInternedFieldName(
        entry.iid(), context_->storage->InternString(str_view));
  }
  for (auto it = heap_graph.roots(); it; ++it) {
    protos::pbzero::HeapGraphRoot::Decoder entry(*it);
    const char* str = HeapGraphRootTypeToString(entry.root_type());
    auto str_view = base::StringView(str);

    HeapGraphTracker::SourceRoot src_root;
    src_root.root_type = context_->storage->InternString(str_view);
    for (auto obj_it = entry.object_ids(); obj_it; ++obj_it)
      src_root.object_ids.emplace_back(*obj_it);
    context_->heap_graph_tracker->AddRoot(upid, ts, std::move(src_root));
  }
  if (!heap_graph.continued()) {
    context_->heap_graph_tracker->FinalizeProfile();
  }
}

}  // namespace trace_processor
}  // namespace perfetto
