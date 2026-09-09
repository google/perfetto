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

#include "src/traceconv/trace_to_bundle.h"

#include <cstdio>
#include <string>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/atomic_file.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/progress_reporter.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/tar_writer.h"
#include "src/trace_processor/util/trace_enrichment/trace_enrichment.h"

namespace perfetto::trace_to_text {
namespace {
base::Status AddBundleEntries(
    trace_processor::util::TarWriter* tar,
    const std::string& input_file_path,
    const trace_processor::util::EnrichmentResult& enrich_result) {
  RETURN_IF_ERROR(tar->AddFileFromPath("trace.perfetto", input_file_path));
  // Add symbols if available.
  if (!enrich_result.native_symbols.empty()) {
    auto add_status = tar->AddFile("symbols.pb", enrich_result.native_symbols);
    if (!add_status.ok()) {
      return base::ErrStatus("failed to add symbols to bundle: %s",
                             add_status.c_message());
    }
  }

  // Add deobfuscation data if available.
  if (!enrich_result.deobfuscation_data.empty()) {
    auto add_status =
        tar->AddFile("deobfuscation.pb", enrich_result.deobfuscation_data);
    if (!add_status.ok()) {
      return base::ErrStatus("failed to add deobfuscation data to bundle: %s",
                             add_status.c_message());
    }
  }

  return base::OkStatus();
}
}  // namespace

base::Status TraceToBundle(const std::string& input_file_path,
                           const std::string& output_file_path,
                           const BundleContext& context) {
  if (!base::IsRegularFile(input_file_path))
    return base::ErrStatus("bundle: input must be a regular file: '%s'",
                           input_file_path.c_str());
  if (base::IsSameFile(input_file_path, output_file_path))
    return base::ErrStatus("bundle: input and output refer to the same file");
  base::AtomicFile output(output_file_path);
  RETURN_IF_ERROR(output.Open());
  base::ProgressReporter progress(!context.no_progress && !context.quiet);
  auto tp = trace_processor::TraceProcessor::CreateInstance({});

  double loaded_mb = 0;
  auto status = trace_processor::ReadTrace(
      tp.get(), input_file_path.c_str(),
      [&loaded_mb, &progress](uint64_t parsed_size) {
        loaded_mb = static_cast<double>(parsed_size) / 1E6;
        progress.Update(
            base::StackString<128>("Reading trace: %.2f MB", loaded_mb)
                .ToStdString());
      });
  progress.Clear();
  if (!status.ok())
    return base::ErrStatus("failed to read trace: %s", status.c_message());
  if (!context.quiet)
    fprintf(stderr, "Read trace: %.2f MB.\n", loaded_mb);

  // Build enrichment configuration from context.
  trace_processor::util::EnrichmentConfig enrich_config;
  enrich_config.symbol_paths = context.symbol_paths;
  enrich_config.no_auto_symbol_paths = context.no_auto_symbol_paths;
  enrich_config.no_auto_proguard_maps = context.no_auto_proguard_maps;
  enrich_config.verbose = context.verbose && !context.quiet;
  enrich_config.android_product_out = context.android_product_out;
  enrich_config.home_dir = context.home_dir;
  enrich_config.working_dir = context.working_dir;
  enrich_config.root_dir = context.root_dir;
  enrich_config.colorize = base::StderrSupportsColor();

  // Add explicit ProGuard maps from context.
  for (const auto& map_spec : context.proguard_maps) {
    enrich_config.proguard_maps.push_back({map_spec.package, map_spec.path});
  }

  // Perform trace enrichment (symbolization + deobfuscation).
  if (!context.quiet)
    fprintf(stderr, "Symbolizing and deobfuscating...\n");
  auto enrich_result =
      trace_processor::util::EnrichTrace(tp.get(), enrich_config);
  if (!context.quiet)
    fprintf(stderr, "Enrichment done.\n");

  // Log any issues to stderr (without PERFETTO_LOG noise).
  if (!context.quiet && !enrich_result.details.empty()) {
    fprintf(stderr, "%s", enrich_result.details.c_str());
  }

  if (!enrich_result.warnings.empty())
    fprintf(stderr, "%s", enrich_result.warnings.c_str());

  // Explicitly-provided resources that fail to load are the only hard
  // errors: the user asked for them, so silently producing a bundle without
  // them would hide the problem. Everything else (no matching symbols found,
  // kernel addresses that cannot be symbolized offline, ...) is advisory and
  // has already been printed above; the bundle is still produced with the
  // trace and whatever enrichment was possible.
  if (enrich_result.error ==
      trace_processor::util::EnrichmentError::kExplicitMapsFailed) {
    return base::ErrStatus(
        "bundle: one or more explicitly-provided ProGuard/R8 maps could not "
        "be read; see the details above. Refusing to produce a bundle without "
        "the requested deobfuscation data.");
  }

  if (!context.quiet)
    fprintf(stderr, "Adding trace to bundle...\n");
  {
    auto fd = output.DuplicateFD();
    if (!fd)
      return base::ErrStatus("bundle: failed to duplicate output descriptor");
    trace_processor::util::TarWriter tar(std::move(fd));
    auto write_status = AddBundleEntries(&tar, input_file_path, enrich_result);
    // Always finalize explicitly: the destructor otherwise crashes on an
    // end-of-archive write failure (e.g. disk full).
    auto finalize_status = tar.Finalize();
    RETURN_IF_ERROR(write_status);
    RETURN_IF_ERROR(finalize_status);
  }
  return std::move(output).Commit();
}

}  // namespace perfetto::trace_to_text
