/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_PLUGIN_H_

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::plugins {
class PluginContext;
}  // namespace perfetto::trace_processor::plugins

namespace perfetto::trace_processor::plugins::tree {

// Single entrypoint for the tree algebra plugin.
// Registers all tree-related functions and macros.
class TreePlugin {
 public:
  static base::Status Register(PluginContext& ctx);
};

}  // namespace perfetto::trace_processor::plugins::tree

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_PLUGIN_H_
