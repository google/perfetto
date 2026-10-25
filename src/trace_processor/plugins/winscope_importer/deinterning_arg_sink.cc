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

#include "src/trace_processor/plugins/winscope_importer/deinterning_arg_sink.h"

#include <cstdint>
#include <utility>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor::winscope {

namespace {

constexpr char kIidSuffix[] = "_iid";
constexpr size_t kIidSuffixSize = sizeof(kIidSuffix) - 1;
constexpr char kDeinternError[] = "STRING DE-INTERNING ERROR";

StringId StripIidSuffix(StringPool& pool, StringId id) {
  NullTermStringView str = pool.Get(id);
  return pool.InternString(
      base::StringView(str.data(), str.size() - kIidSuffixSize));
}

}  // namespace

DeinterningArgSink::DeinterningArgSink(ArgSink& inner,
                                       StringPool& pool,
                                       Lookup lookup)
    : inner_(inner), pool_(pool), lookup_(std::move(lookup)) {}

DeinterningArgSink::~DeinterningArgSink() = default;

void DeinterningArgSink::Add(ArgKey key, Variadic value) {
  if ((value.type != Variadic::kInt && value.type != Variadic::kUint) ||
      !pool_.Get(key.key).EndsWith(kIidSuffix)) {
    inner_.Add(key, value);
    return;
  }
  ArgKey deinterned_key{StripIidSuffix(pool_, key.flat_key),
                        StripIidSuffix(pool_, key.key)};
  uint64_t iid = value.type == Variadic::kInt
                     ? static_cast<uint64_t>(value.int_value)
                     : value.uint_value;
  if (std::optional<base::StringView> str = lookup_(key, iid)) {
    inner_.Add(deinterned_key, Variadic::String(pool_.InternString(*str)));
    return;
  }
  inner_.Add(deinterned_key,
             Variadic::String(pool_.InternString(kDeinternError)));
  inner_.Add(key, value);
}

}  // namespace perfetto::trace_processor::winscope
