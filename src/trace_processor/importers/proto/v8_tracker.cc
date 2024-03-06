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

#include "src/trace_processor/importers/proto/v8_tracker.h"

#include <cstdint>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/chrome/v8.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/string_encoding_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::perfetto::protos::pbzero::InternedV8Isolate;
using ::perfetto::protos::pbzero::InternedV8JsFunction;
using ::perfetto::protos::pbzero::InternedV8JsScript;
using ::perfetto::protos::pbzero::InternedV8WasmScript;
using ::perfetto::protos::pbzero::V8InternalCode;
using ::perfetto::protos::pbzero::V8JsCode;
using ::perfetto::protos::pbzero::V8String;
using ::perfetto::protos::pbzero::V8WasmCode;

base::StringView JsScriptTypeToString(int32_t type) {
  if (type < protos::pbzero::InternedV8JsScript_Type_MIN ||
      type > protos::pbzero::InternedV8JsScript_Type_MAX) {
    return "UNKNOWN";
  }
  base::StringView name =
      InternedV8JsScript::Type_Name(InternedV8JsScript::Type(type));
  // Remove the "TYPE_" prefix
  return name.substr(5);
}

base::StringView JsFunctionKindToString(int32_t kind) {
  if (kind < protos::pbzero::InternedV8JsFunction_Kind_MIN ||
      kind > protos::pbzero::InternedV8JsFunction_Kind_MAX) {
    return "UNKNOWN";
  }
  base::StringView name =
      InternedV8JsFunction::Kind_Name(InternedV8JsFunction::Kind(kind));
  // Remove the "KIND_" prefix
  return name.substr(5);
}

}  // namespace

V8Tracker::V8Tracker(TraceProcessorContext* context) : context_(context) {}

V8Tracker::~V8Tracker() = default;

tables::V8IsolateTable::Id V8Tracker::InternIsolate(
    protozero::ConstBytes bytes) {
  InternedV8Isolate::Decoder isolate(bytes);
  const UniquePid upid =
      context_->process_tracker->GetOrCreateProcess(isolate.pid());

  if (auto* id =
          isolate_index_.Find(std::make_pair(upid, isolate.isolate_id()));
      id) {
    return *id;
  }

  // TODO(carlscab): Implement support for no code range
  PERFETTO_CHECK(isolate.has_code_range());

  InternedV8Isolate::CodeRange::Decoder code_range(isolate.code_range());

  auto v8_isolate_id =
      context_->storage->mutable_v8_isolate_table()
          ->Insert(
              {upid, isolate.isolate_id(),
               static_cast<int64_t>(isolate.embedded_blob_code_start_address()),
               static_cast<int64_t>(isolate.embedded_blob_code_size()),
               static_cast<int64_t>(code_range.base_address()),
               static_cast<int64_t>(code_range.size()),
               code_range.is_process_wide(),
               code_range.has_embedded_blob_code_copy_start_address()
                   ? std::make_optional(static_cast<int64_t>(
                         code_range.embedded_blob_code_copy_start_address()))
                   : std::nullopt

              })
          .id;
  isolate_index_.Insert(std::make_pair(upid, isolate.isolate_id()),
                        v8_isolate_id);
  return v8_isolate_id;
}

tables::V8JsScriptTable::Id V8Tracker::InternJsScript(
    protozero::ConstBytes bytes,
    tables::V8IsolateTable::Id isolate_id) {
  InternedV8JsScript::Decoder script(bytes);

  if (auto* id =
          js_script_index_.Find(std::make_pair(isolate_id, script.script_id()));
      id) {
    return *id;
  }

  tables::V8JsScriptTable::Row row;
  row.v8_isolate_id = isolate_id;
  row.internal_script_id = script.script_id();
  row.script_type =
      context_->storage->InternString(JsScriptTypeToString(script.type()));
  row.name = InternV8String(V8String::Decoder(script.name()));
  row.source = InternV8String(V8String::Decoder(script.source()));

  tables::V8JsScriptTable::Id script_id =
      context_->storage->mutable_v8_js_script_table()->Insert(row).id;
  js_script_index_.Insert(std::make_pair(isolate_id, script.script_id()),
                          script_id);
  return script_id;
}

tables::V8WasmScriptTable::Id V8Tracker::InternWasmScript(
    protozero::ConstBytes bytes,
    tables::V8IsolateTable::Id isolate_id) {
  InternedV8WasmScript::Decoder script(bytes);

  if (auto* id = wasm_script_index_.Find(
          std::make_pair(isolate_id, script.script_id()));
      id) {
    return *id;
  }

  tables::V8WasmScriptTable::Row row;
  row.v8_isolate_id = isolate_id;
  row.internal_script_id = script.script_id();
  row.url = context_->storage->InternString(script.url());

  tables::V8WasmScriptTable::Id script_id =
      context_->storage->mutable_v8_wasm_script_table()->Insert(row).id;
  wasm_script_index_.Insert(std::make_pair(isolate_id, script.script_id()),
                            script_id);
  return script_id;
}

tables::V8JsFunctionTable::Id V8Tracker::InternJsFunction(
    protozero::ConstBytes bytes,
    StringId name,
    tables::V8JsScriptTable::Id script_id) {
  InternedV8JsFunction::Decoder function(bytes);

  tables::V8JsFunctionTable::Row row;
  row.name = name;
  row.v8_js_script_id = script_id;
  row.is_toplevel = function.is_toplevel();
  row.kind =
      context_->storage->InternString(JsFunctionKindToString(function.kind()));
  // TODO(carlscab): Row and line are hard. Offset is in bytes, row and line are
  // in characters and we potentially have a multi byte encoding (UTF16). Good
  // luck!

  if (auto* id = js_function_index_.Find(row); id) {
    return *id;
  }

  tables::V8JsFunctionTable::Id function_id =
      context_->storage->mutable_v8_js_function_table()->Insert(row).id;
  js_function_index_.Insert(row, function_id);
  return function_id;
}

void V8Tracker::AddJsCode(int64_t,
                          tables::V8IsolateTable::Id,
                          tables::V8JsFunctionTable::Id,
                          const protos::pbzero::V8JsCode::Decoder&) {
  // TODO(carlscab): Implement
}

void V8Tracker::AddInternalCode(
    int64_t,
    tables::V8IsolateTable::Id,
    const protos::pbzero::V8InternalCode::Decoder&) {
  // TODO(carlscab): Implement
}

void V8Tracker::AddWasmCode(int64_t,
                            tables::V8IsolateTable::Id,
                            tables::V8WasmScriptTable::Id,
                            const protos::pbzero::V8WasmCode::Decoder&) {
  // TODO(carlscab): Implement
}

void V8Tracker::AddRegExpCode(int64_t,
                              tables::V8IsolateTable::Id,
                              const protos::pbzero::V8RegExpCode::Decoder&) {
  // TODO(carlscab): Implement
}

StringId V8Tracker::InternV8String(
    const protos::pbzero::V8String::Decoder& v8_string) {
  auto& storage = *context_->storage;
  if (v8_string.has_latin1()) {
    return storage.InternString(
        base::StringView(ConvertLatin1ToUtf8(v8_string.latin1())));
  }

  if (v8_string.has_utf16_le()) {
    return storage.InternString(
        base::StringView(ConvertUtf16LeToUtf8(v8_string.latin1())));
  }

  if (v8_string.has_utf16_be()) {
    return storage.InternString(
        base::StringView(ConvertUtf16BeToUtf8(v8_string.latin1())));
  }
  return storage.InternString("");
}

}  // namespace trace_processor
}  // namespace perfetto
