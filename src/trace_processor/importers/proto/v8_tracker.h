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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_TRACKER_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/chrome/v8.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

// Keeps track of V8 related objects.
class V8Tracker : public Destructible {
 public:
  static V8Tracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->v8_tracker) {
      context->v8_tracker.reset(new V8Tracker(context));
    }
    return static_cast<V8Tracker*>(context->v8_tracker.get());
  }

  ~V8Tracker() override;

  tables::V8IsolateTable::Id InternIsolate(protozero::ConstBytes bytes);
  tables::V8JsScriptTable::Id InternJsScript(
      protozero::ConstBytes bytes,
      tables::V8IsolateTable::Id isolate_id);
  tables::V8WasmScriptTable::Id InternWasmScript(
      protozero::ConstBytes bytes,
      tables::V8IsolateTable::Id isolate_id);
  tables::V8JsFunctionTable::Id InternJsFunction(
      protozero::ConstBytes bytes,
      StringId name,
      tables::V8JsScriptTable::Id script_id);

  void AddJsCode(int64_t timestamp,
                 tables::V8IsolateTable::Id isolate_id,
                 tables::V8JsFunctionTable::Id function_id,
                 const protos::pbzero::V8JsCode::Decoder& code);

  void AddInternalCode(int64_t timestamp,
                       tables::V8IsolateTable::Id v8_isolate_id,
                       const protos::pbzero::V8InternalCode::Decoder& code);

  void AddWasmCode(int64_t timestamp,
                   tables::V8IsolateTable::Id isolate_id,
                   tables::V8WasmScriptTable::Id script_id,
                   const protos::pbzero::V8WasmCode::Decoder& code);

  void AddRegExpCode(int64_t timestamp,
                     tables::V8IsolateTable::Id v8_isolate_id,
                     const protos::pbzero::V8RegExpCode::Decoder& code);

 private:
  explicit V8Tracker(TraceProcessorContext* context);

  StringId InternV8String(const protos::pbzero::V8String::Decoder& v8_string);

  TraceProcessorContext* const context_;

  struct IsolateIndexHash {
    size_t operator()(const std::pair<UniquePid, int32_t>& v) const {
      return static_cast<size_t>(base::Hasher::Combine(v.first, v.second));
    }
  };
  base::FlatHashMap<std::pair<UniquePid, int32_t>,
                    tables::V8IsolateTable::Id,
                    IsolateIndexHash>
      isolate_index_;

  struct ScriptIndexHash {
    size_t operator()(
        const std::pair<tables::V8IsolateTable::Id, int32_t>& v) const {
      return static_cast<size_t>(
          base::Hasher::Combine(v.first.value, v.second));
    }
  };
  base::FlatHashMap<std::pair<tables::V8IsolateTable::Id, int32_t>,
                    tables::V8JsScriptTable::Id,
                    ScriptIndexHash>
      js_script_index_;
  base::FlatHashMap<std::pair<tables::V8IsolateTable::Id, int32_t>,
                    tables::V8WasmScriptTable::Id,
                    ScriptIndexHash>
      wasm_script_index_;

  struct JsFunctionHash {
    size_t operator()(const tables::V8JsFunctionTable::Row& v) const {
      return static_cast<size_t>(base::Hasher::Combine(
          v.name.raw_id(), v.v8_js_script_id.value, v.is_toplevel,
          v.kind.raw_id(), v.line.value_or(0), v.column.value_or(0)));
    }
  };
  base::FlatHashMap<tables::V8JsFunctionTable::Row,
                    tables::V8JsFunctionTable::Id,
                    JsFunctionHash>
      js_function_index_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_V8_TRACKER_H_
