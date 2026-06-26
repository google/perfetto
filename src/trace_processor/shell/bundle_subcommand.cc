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

#include "src/trace_processor/shell/bundle_subcommand.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/traceconv/trace_to_bundle.h"

namespace perfetto::trace_processor::shell {

const char* BundleSubcommand::name() const {
  return "bundle";
}

const char* BundleSubcommand::description() const {
  return "Bundle a trace with symbols and deobfuscation data.";
}

const char* BundleSubcommand::usage_args() const {
  return "<input> <output>";
}

const char* BundleSubcommand::detailed_help() const {
  return R"(Create a self-contained bundle from a trace.

Outputs a TAR containing the trace plus the symbols and deobfuscation
mappings needed to make it self-contained. Both <input> and <output> must be
real file paths (stdin/stdout are not supported).

Options:
  --symbol-paths PATH1,PATH2,...   Additional paths to search for symbols.
  --no-auto-symbol-paths           Disable automatic symbol path discovery.
  --proguard-map [pkg=]PATH        ProGuard/R8 mapping.txt for Java/Kotlin
                                   deobfuscation (may be repeated). The pkg=
                                   prefix scopes the map to a package.
  --no-auto-proguard-maps          Disable automatic ProGuard/R8 mapping
                                   discovery (e.g. Gradle project layout).
  --verbose                        Print more detailed output.)";
}

std::vector<FlagSpec> BundleSubcommand::GetFlags() {
  return {
      StringFlag("symbol-paths", '\0', "PATH1,PATH2,...",
                 "Additional paths to search for symbols.", &symbol_paths_),
      BoolFlag("no-auto-symbol-paths", '\0',
               "Disable automatic symbol path discovery.",
               &no_auto_symbol_paths_),
      FlagSpec{"proguard-map", '\0', true, "[pkg=]PATH",
               "ProGuard/R8 mapping.txt for deobfuscation (may be repeated).",
               [this](const char* v) { proguard_maps_.emplace_back(v); }},
      BoolFlag("no-auto-proguard-maps", '\0',
               "Disable automatic ProGuard/R8 mapping discovery.",
               &no_auto_proguard_maps_),
      BoolFlag("verbose", '\0', "Print more detailed output.", &verbose_),
  };
}

base::Status BundleSubcommand::Run(const SubcommandContext& ctx) {
  if (ctx.positional_args.size() < 2) {
    return base::ErrStatus(
        "bundle requires both an input and an output file path.");
  }
  const std::string& input_file = ctx.positional_args[0];
  const std::string& output_file = ctx.positional_args[1];

  if (input_file == "-") {
    return base::ErrStatus(
        "bundle does not support stdin input; provide a file path.");
  }
  if (output_file == "-") {
    return base::ErrStatus(
        "bundle does not support stdout output; provide a file path.");
  }
  if (!base::FileExists(input_file)) {
    return base::ErrStatus("Input file does not exist: %s", input_file.c_str());
  }

  trace_to_text::BundleContext context;
  if (!symbol_paths_.empty())
    context.symbol_paths = base::SplitString(symbol_paths_, ",");
  for (const std::string& map : proguard_maps_) {
    trace_to_text::ProguardMapSpec spec;
    size_t eq = map.find('=');
    if (eq == std::string::npos) {
      spec.path = map;
    } else {
      spec.package = map.substr(0, eq);
      spec.path = map.substr(eq + 1);
    }
    context.proguard_maps.push_back(std::move(spec));
  }
  context.no_auto_symbol_paths = no_auto_symbol_paths_;
  context.no_auto_proguard_maps = no_auto_proguard_maps_;
  context.verbose = verbose_;
  if (const char* val = getenv("ANDROID_PRODUCT_OUT"))
    context.android_product_out = val;
  if (const char* val = getenv("HOME"))
    context.home_dir = val;
  context.root_dir = "/";

  if (trace_to_text::TraceToBundle(input_file, output_file, context) != 0)
    return base::ErrStatus("bundle: failed to create bundle.");
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
