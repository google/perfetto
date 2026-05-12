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

#include "src/trace_processor/importers/proto/android_binder_parcel_module.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <string>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/android/android_binder_parcel.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

using perfetto::protos::pbzero::AndroidBinderParcelEvent;
using perfetto::protos::pbzero::ParcelFieldAnnotation;
using perfetto::protos::pbzero::TracePacket;

namespace {

const char* DirectionToString(int32_t dir) {
  switch (dir) {
    case AndroidBinderParcelEvent::CLIENT_SEND:
      return "CLIENT_SEND";
    case AndroidBinderParcelEvent::SERVER_RECV:
      return "SERVER_RECV";
    case AndroidBinderParcelEvent::SERVER_REPLY:
      return "SERVER_REPLY";
    case AndroidBinderParcelEvent::CLIENT_RECV:
      return "CLIENT_RECV";
    default:
      return "UNKNOWN";
  }
}

const char* KindToString(int32_t kind) {
  switch (kind) {
    case AndroidBinderParcelEvent::PRIMITIVE:
      return "primitive";
    case AndroidBinderParcelEvent::STRING:
      return "string";
    case AndroidBinderParcelEvent::STRONG_BINDER:
      return "strong_binder";
    case AndroidBinderParcelEvent::FILE_DESCRIPTOR:
      return "file_descriptor";
    case AndroidBinderParcelEvent::PARCELABLE:
      return "parcelable";
    case AndroidBinderParcelEvent::TYPED_OBJECT:
      return "typed_object";
    case AndroidBinderParcelEvent::ARRAY:
      return "array";
    case AndroidBinderParcelEvent::VECTOR:
      return "vector";
    case AndroidBinderParcelEvent::BYTES:
      return "bytes";
    case AndroidBinderParcelEvent::INTERFACE_TOKEN:
      return "interface_token";
    case AndroidBinderParcelEvent::OPAQUE:
      return "opaque";
    default:
      return "unspecified";
  }
}

}  // namespace

AndroidBinderParcelModule::AndroidBinderParcelModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(TracePacket::kAndroidBinderParcelFieldNumber);
}

void AndroidBinderParcelModule::ParseTracePacketData(
    const TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  if (field_id != TracePacket::kAndroidBinderParcelFieldNumber) {
    return;
  }
  ParseAndroidBinderParcel(ts, decoder.android_binder_parcel());
}

void AndroidBinderParcelModule::ParseAndroidBinderParcel(
    int64_t ts,
    protozero::ConstBytes blob) {
  AndroidBinderParcelEvent::Decoder ev(blob);

  // Compose slice name = "<interface>::<method>" (fallback: "...::code#<N>").
  std::string slice_name;
  slice_name.reserve(ev.interface_name().size + ev.method_name().size + 8);
  slice_name.append(ev.interface_name().data, ev.interface_name().size);
  slice_name.append("::");
  if (ev.method_name().size > 0) {
    slice_name.append(ev.method_name().data, ev.method_name().size);
  } else {
    slice_name.append("code#");
    slice_name.append(std::to_string(ev.code()));
  }

  UniqueTid utid = context_->process_tracker->UpdateThread(ev.tid(), ev.pid());
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  StringId cat_id = context_->storage->InternString("aidl");
  StringId name_id =
      context_->storage->InternString(base::StringView(slice_name));
  int64_t dur = static_cast<int64_t>(ev.duration_ns());

  context_->slice_tracker->Scoped(
      ts, track_id, cat_id, name_id, dur,
      [this, &ev](ArgsTracker::BoundInserter* inserter) {
        auto* storage = context_->storage.get();

        auto add_str = [&](const char* k, protozero::ConstChars v) {
          if (v.size == 0)
            return;
          inserter->AddArg(storage->InternString(k),
                           Variadic::String(storage->InternString(v)));
        };
        auto add_uint = [&](const char* k, uint64_t v) {
          inserter->AddArg(storage->InternString(k),
                           Variadic::UnsignedInteger(v));
        };
        auto add_int = [&](const char* k, int64_t v) {
          inserter->AddArg(storage->InternString(k), Variadic::Integer(v));
        };
        auto add_bool = [&](const char* k) {
          inserter->AddArg(storage->InternString(k), Variadic::Boolean(true));
        };

        add_str("binder.interface_name", ev.interface_name());
        add_uint("binder.code", ev.code());
        add_uint("binder.flags", ev.flags());
        const char* dir_str = DirectionToString(ev.direction());
        inserter->AddArg(
            storage->InternString("binder.direction"),
            Variadic::String(storage->InternString(
                protozero::ConstChars{dir_str, std::strlen(dir_str)})));
        add_uint("binder.txn_id", ev.txn_id());
        add_int("binder.returned_status", ev.returned_status());
        if (ev.has_oneway() && ev.oneway())
          add_bool("binder.oneway");
        add_str("binder.thread_name", ev.thread_name());

        if (ev.has_metadata()) {
          AndroidBinderParcelEvent::ParcelMetadata::Decoder md(ev.metadata());
          auto md_uint = [&](const char* k, bool has, uint64_t v) {
            if (has && v > 0)
              add_uint(k, v);
          };
          md_uint("binder.data_size_bytes", md.has_total_data_size(),
                  md.total_data_size());
          md_uint("binder.object_count", md.has_object_count(),
                  md.object_count());
          md_uint("binder.binder_count", md.has_binder_count(),
                  md.binder_count());
          md_uint("binder.fd_count", md.has_file_descriptor_count(),
                  md.file_descriptor_count());
          md_uint("binder.fd_array_count", md.has_file_descriptor_array_count(),
                  md.file_descriptor_array_count());
          md_uint("binder.pointer_count", md.has_pointer_count(),
                  md.pointer_count());
          md_uint("binder.shared_memory_bytes", md.has_shared_memory_bytes(),
                  md.shared_memory_bytes());
          if (md.has_sensitive() && md.sensitive())
            add_bool("binder.sensitive");
          if (md.has_is_rpc() && md.is_rpc())
            add_bool("binder.is_rpc");
          if (md.has_strict_mode_policy()) {
            add_uint("binder.strict_mode_policy", md.strict_mode_policy());
          }
          if (md.has_work_source_uid()) {
            add_int("binder.work_source_uid", md.work_source_uid());
          }
        }

        // Per-annotation args live under their own "aidl.<field>.*"
        // subtree, distinct from the "binder.*" metadata namespace.
        // This avoids two args-to-JSON collisions in the UI:
        //   * scalar vs dict at the same key (e.g. aidl type string
        //     at "aidl.<field>" alongside "aidl.<field>.value")
        //   * an AIDL argument named "code"/"flags"/... colliding
        //     with the "binder.code" / "binder.flags" metadata scalar.
        // Keys:
        //   "aidl.<field>.type"   (aidl type string)
        //   "aidl.<field>.value"  (typed value)
        //   "aidl.<field>.kind"   (kind enum)
        // plus a handful of attribute sub-keys.
        for (auto it = ev.annotation(); it; ++it) {
          ParcelFieldAnnotation::Decoder a(*it);
          std::string prefix = "aidl.";
          if (a.field_name().size > 0) {
            prefix.append(a.field_name().data, a.field_name().size);
          } else {
            prefix.append("arg");
          }

          auto sub_key = [&](const char* suffix) {
            std::string k = prefix;
            k.append(suffix);
            return storage->InternString(base::StringView(k));
          };

          if (a.aidl_type().size > 0) {
            inserter->AddArg(
                sub_key(".type"),
                Variadic::String(storage->InternString(a.aidl_type())));
          }

          if (a.has_int_preview()) {
            inserter->AddArg(sub_key(".value"),
                             Variadic::Integer(a.int_preview()));
          } else if (a.has_double_preview()) {
            inserter->AddArg(sub_key(".value"),
                             Variadic::Real(a.double_preview()));
          } else if (a.has_string_preview()) {
            inserter->AddArg(
                sub_key(".value"),
                Variadic::String(storage->InternString(a.string_preview())));
          } else if (a.has_binder_descriptor_preview()) {
            inserter->AddArg(sub_key(".descriptor"),
                             Variadic::String(storage->InternString(
                                 a.binder_descriptor_preview())));
          } else if (a.has_bytes_preview()) {
            protozero::ConstBytes bp = a.bytes_preview();
            base::StringView sv(reinterpret_cast<const char*>(bp.data),
                                bp.size);
            inserter->AddArg(sub_key(".value"),
                             Variadic::String(storage->InternString(sv)));
          }

          if (a.kind()) {
            const char* k = KindToString(a.kind());
            inserter->AddArg(sub_key(".kind"),
                             Variadic::String(storage->InternString(
                                 protozero::ConstChars{k, std::strlen(k)})));
          }
          if (a.byte_length()) {
            inserter->AddArg(sub_key(".byte_length"),
                             Variadic::UnsignedInteger(a.byte_length()));
          }
          if (a.has_nullable() && a.nullable()) {
            inserter->AddArg(sub_key(".nullable"), Variadic::Boolean(true));
          }
          if (a.has_is_null() && a.is_null()) {
            inserter->AddArg(sub_key(".is_null"), Variadic::Boolean(true));
          }
          if (a.has_element_count() && a.element_count() > 0) {
            inserter->AddArg(sub_key(".element_count"),
                             Variadic::UnsignedInteger(a.element_count()));
          }
          if (a.has_fd_size_bytes()) {
            inserter->AddArg(sub_key(".fd_size_bytes"),
                             Variadic::UnsignedInteger(a.fd_size_bytes()));
          }
          if (a.has_fd_kind()) {
            inserter->AddArg(
                sub_key(".fd_kind"),
                Variadic::String(storage->InternString(a.fd_kind())));
          }
          if (a.has_sensitive() && a.sensitive()) {
            inserter->AddArg(sub_key(".sensitive"), Variadic::Boolean(true));
          }
        }
      });
}

}  // namespace perfetto::trace_processor
