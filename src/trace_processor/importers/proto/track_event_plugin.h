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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"

namespace perfetto::trace_processor {

// Field-id-keyed dispatch for TrackEvent extension parsers. Empty registry
// costs one bool check per packet.
class TrackEventPluginRegistry {
 public:
  using FieldHandler =
      std::function<void(protozero::ConstBytes data, int64_t ts)>;

  class Plugin {
   public:
    virtual ~Plugin();
  };

  void RegisterFieldHandler(uint32_t field_id, FieldHandler handler) {
    handlers_.Insert(field_id, std::move(handler));
  }

  void RegisterPlugin(std::unique_ptr<Plugin> plugin) {
    plugins_.push_back(std::move(plugin));
  }

  void Dispatch(protozero::ConstBytes event_bytes, int64_t ts) const {
    if (handlers_.size() == 0)
      return;
    protozero::ProtoDecoder decoder(event_bytes);
    for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
      if (auto* h = handlers_.Find(f.id())) {
        (*h)(f.as_bytes(), ts);
      }
    }
  }

 private:
  base::FlatHashMap<uint32_t, FieldHandler> handlers_;
  std::vector<std::unique_ptr<Plugin>> plugins_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
