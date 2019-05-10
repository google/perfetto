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

#ifndef SRC_TRACE_PROCESSOR_METRICS_DESCRIPTORS_H_
#define SRC_TRACE_PROCESSOR_METRICS_DESCRIPTORS_H_

#include <algorithm>
#include <string>
#include <vector>

#include "perfetto/base/optional.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

class FieldDescriptor {
 public:
  FieldDescriptor(std::string name,
                  uint32_t number,
                  uint32_t type,
                  std::string raw_type_name,
                  bool is_repeated);

  const std::string& name() const { return name_; }
  uint32_t number() const { return number_; }
  uint32_t type() const { return type_; }
  const std::string& raw_type_name() const { return raw_type_name_; }
  bool is_repeated() const { return is_repeated_; }

  void set_message_type_idx(uint32_t idx) { message_type_idx_ = idx; }

 private:
  std::string name_;
  uint32_t number_;
  uint32_t type_;
  std::string raw_type_name_;
  bool is_repeated_;

  base::Optional<uint32_t> message_type_idx_;
};

class ProtoDescriptor {
 public:
  ProtoDescriptor(std::string package_name,
                  std::string full_name,
                  base::Optional<uint32_t> parent_id);

  void AddField(FieldDescriptor descriptor) {
    fields_.emplace_back(std::move(descriptor));
  }

  base::Optional<uint32_t> FindFieldIdx(const std::string& name) const {
    auto it = std::find_if(
        fields_.begin(), fields_.end(),
        [name](const FieldDescriptor& desc) { return desc.name() == name; });
    auto idx = static_cast<uint32_t>(std::distance(fields_.begin(), it));
    return idx < fields_.size() ? base::Optional<uint32_t>(idx) : base::nullopt;
  }

  const std::string& package_name() const { return package_name_; }

  const std::string& full_name() const { return full_name_; }

  const std::vector<FieldDescriptor>& fields() const { return fields_; }
  std::vector<FieldDescriptor>* mutable_fields() { return &fields_; }

 private:
  std::string package_name_;
  std::string full_name_;
  base::Optional<uint32_t> parent_id_;
  std::vector<FieldDescriptor> fields_;
};

class DescriptorPool {
 public:
  void AddFromFileDescriptorSet(const uint8_t* file_descriptor_set_proto,
                                size_t size);

  base::Optional<uint32_t> FindDescriptorIdx(
      const std::string& full_name) const;

  const std::vector<ProtoDescriptor>& descriptors() const {
    return descriptors_;
  }

 private:
  void AddNestedProtoDescriptors(const std::string& package_name,
                                 base::Optional<uint32_t> parent_idx,
                                 const uint8_t* descriptor_proto,
                                 size_t size);

  std::vector<ProtoDescriptor> descriptors_;
};

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_METRICS_DESCRIPTORS_H_
