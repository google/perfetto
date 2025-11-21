/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traceconv/trace_to_profile.h"

#include <cerrno>
#include <cinttypes>
#include <random>
#include <string>
#include <vector>

#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/symbolize_database.h"
#include "src/traceconv/utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/profiling/pprof_builder.h"
#include "src/profiling/symbolizer/symbolizer.h"

namespace {
constexpr const char* kDefaultTmp = "/tmp";

std::string GetTemp() {
  if (auto tmp = getenv("TMPDIR"); tmp)
    return tmp;
  if (auto tmp = getenv("TEMP"); tmp)
    return tmp;
  return kDefaultTmp;
}
}  // namespace

namespace perfetto {
namespace trace_to_text {
namespace {

uint64_t ToConversionFlags(bool annotate_frames) {
  return static_cast<uint64_t>(annotate_frames
                                   ? ConversionFlags::kAnnotateFrames
                                   : ConversionFlags::kNone);
}

std::string GetRandomString(size_t n) {
  std::random_device r;
  auto rng = std::default_random_engine(r());
  std::string result(n, ' ');
  for (size_t i = 0; i < n; ++i) {
    result[i] = 'a' + (rng() % ('z' - 'a'));
  }
  return result;
}

void MaybeSymbolize(trace_processor::TraceProcessor* tp) {
  std::unique_ptr<profiling::Symbolizer> symbolizer =
      profiling::MaybeLocalSymbolizer(profiling::GetPerfettoBinaryPath(), {},
                                      getenv("PERFETTO_SYMBOLIZER_MODE"));
  if (!symbolizer)
    return;
  profiling::SymbolizeDatabase(tp, symbolizer.get(),
                               [tp](const std::string& trace_proto) {
                                 IngestTraceOrDie(tp, trace_proto);
                               });
  tp->Flush();
}

void MaybeDeobfuscate(trace_processor::TraceProcessor* tp) {
  auto maybe_map = profiling::GetPerfettoProguardMapPath();
  if (maybe_map.empty()) {
    return;
  }
  profiling::ReadProguardMapsToDeobfuscationPackets(
      maybe_map, [tp](const std::string& trace_proto) {
        IngestTraceOrDie(tp, trace_proto);
      });
  tp->Flush();
}

// Creates the destination directory.
// If |output_dir| is not empty, it is used as the destination directory.
// Otherwise, a random temporary directory is created using
// |fallback_dirname_prefix|.
std::string GetDestinationDirectory(
    const std::string& output_dir,
    const std::string& fallback_dirname_prefix) {
  std::string dst_dir;
  if (!output_dir.empty()) {
    dst_dir = output_dir;
  } else {
    dst_dir = GetTemp() + "/" + fallback_dirname_prefix +
              base::GetTimeFmt("%y%m%d%H%M%S") + GetRandomString(5);
  }
  if (!base::Mkdir(dst_dir) && errno != EEXIST) {
    PERFETTO_FATAL("Failed to create output directory %s", dst_dir.c_str());
  }
  return dst_dir;
}

// Helper function to detect ConversionMode from trace content
std::optional<ConversionMode> DetectConversionMode(
    trace_processor::TraceProcessor* tp) {
  auto it = tp->ExecuteQuery(R"(
  SELECT
    EXISTS (SELECT 1 FROM heap_profile_allocation LIMIT 1),
    EXISTS (SELECT 1 FROM perf_sample LIMIT 1),
    EXISTS (SELECT 1 FROM __intrinsic_heap_graph_object LIMIT 1)
  )");
  PERFETTO_CHECK(it.Next());

  int64_t alloc_present = it.Get(0).AsLong();
  int64_t perf_present = it.Get(1).AsLong();
  int64_t graph_present = it.Get(2).AsLong();
  
  PERFETTO_LOG("DetectConversionMode: alloc_present=%" PRId64 ", perf_present=%" PRId64 ", graph_present=%" PRId64,
               alloc_present, perf_present, graph_present);
  
  // Count how many booleans are set
  int64_t count = alloc_present + perf_present + graph_present;

  if (count != 1) {
    PERFETTO_LOG("DetectConversionMode: Expected exactly one profile type, but found %" PRId64,
                 count);
    return std::nullopt;
  }
  
  // Derive ConversionMode based on which boolean is set
  ConversionMode mode;
  if (alloc_present) {
    mode = ConversionMode::kHeapProfile;
  } else if (perf_present) {
    mode = ConversionMode::kPerfProfile;
  } else {  // graph_present
    mode = ConversionMode::kJavaHeapProfile;
  }
  
  PERFETTO_LOG("DetectConversionMode: Derived conversion_mode=%d",
               static_cast<int>(mode));

  return mode;
}

}  // namespace

int TraceToProfile(std::istream* input,
                   std::ostream* output,
                   uint64_t pid,
                   const std::vector<uint64_t>& timestamps,
                   bool annotate_frames,
                   const std::string& output_dir,
                   std::optional<ConversionMode> explicit_conversion_mode) {
  // Parse the trace (always needed regardless of mode)
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);
  if (!ReadTraceUnfinalized(tp.get(), input))
    return -1;
  tp->Flush();

  // Detect conversion mode if not explicitly provided
  ConversionMode conversion_mode;
  if (explicit_conversion_mode.has_value()) {
    conversion_mode = explicit_conversion_mode.value();
    PERFETTO_LOG("TraceToProfile: Using explicit conversion_mode=%d",
                 static_cast<int>(conversion_mode));
  } else {
    auto detected_mode = DetectConversionMode(tp.get());
    if (!detected_mode.has_value()) {
      return -1;
    }
    conversion_mode = detected_mode.value();
  }

  // Set up filename function and directory prefix based on conversion mode
  int file_idx = 0;
  std::function<std::string(const SerializedProfile&)> filename_fn;
  std::string dir_prefix;
  
  switch (conversion_mode) {
    case ConversionMode::kHeapProfile:
      filename_fn = [&file_idx](const SerializedProfile& profile) {
        return "heap_dump." + std::to_string(++file_idx) + "." +
               std::to_string(profile.pid) + "." + profile.heap_name + ".pb";
      };
      dir_prefix = "heap_profile-";
      break;
    case ConversionMode::kPerfProfile:
      filename_fn = [&file_idx](const SerializedProfile& profile) {
        return "profile." + std::to_string(++file_idx) + ".pid." +
               std::to_string(profile.pid) + ".pb";
      };
      dir_prefix = "perf_profile-";
      break;
    case ConversionMode::kJavaHeapProfile:
      filename_fn = [&file_idx](const SerializedProfile& profile) {
        return "java_heap_dump." + std::to_string(++file_idx) + "." +
               std::to_string(profile.pid) + ".pb";
      };
      dir_prefix = "heap_profile-";
      break;
  }

  std::string dst_dir = GetDestinationDirectory(output_dir, dir_prefix);
  
  // Symbolize and deobfuscate
  MaybeSymbolize(tp.get());
  MaybeDeobfuscate(tp.get());
  if (auto status = tp->NotifyEndOfFile(); !status.ok()) {
    return -1;
  }
  
  // Generate profiles
  std::vector<SerializedProfile> profiles;
  TraceToPprof(tp.get(), &profiles, conversion_mode,
               ToConversionFlags(annotate_frames), pid, timestamps);
  if (profiles.empty()) {
    return 0;
  }

  // Write profiles to files
  for (const auto& profile : profiles) {
    std::string filename = dst_dir + "/" + filename_fn(profile);
    base::ScopedFile fd(base::OpenFile(filename, O_CREAT | O_WRONLY, 0700));
    if (!fd)
      PERFETTO_FATAL("Failed to open %s", filename.c_str());
    PERFETTO_CHECK(base::WriteAll(*fd, profile.serialized.c_str(),
                                  profile.serialized.size()) ==
                   static_cast<ssize_t>(profile.serialized.size()));
  }
  *output << "Wrote profiles to " << dst_dir << '\n';
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
