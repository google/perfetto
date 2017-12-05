/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_PROTOZERO_PROTOC_PLUGIN_PROTOZERO_GENERATOR_H_
#define SRC_PROTOZERO_PROTOC_PLUGIN_PROTOZERO_GENERATOR_H_

#include <string>

#include "google/protobuf/compiler/code_generator.h"

namespace protozero {

class ProtoZeroGenerator : public ::google::protobuf::compiler::CodeGenerator {
 public:
  explicit ProtoZeroGenerator();
  ~ProtoZeroGenerator() override;

  // CodeGenerator implementation
  bool Generate(const google::protobuf::FileDescriptor* file,
                const std::string& options,
                google::protobuf::compiler::GeneratorContext* context,
                std::string* error) const override;
};

}  // namespace protozero

#endif  // SRC_PROTOZERO_PROTOC_PLUGIN_PROTOZERO_GENERATOR_H_
