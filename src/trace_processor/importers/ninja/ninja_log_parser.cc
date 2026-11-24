/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/importers/ninja/ninja_log_parser.h"

#include <stdio.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/sort.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

using base::StringSplitter;

namespace {
// The range of `# ninja log vN` headers we can read. ninja bumped the version
// twice without changing the layout of a log line: v6 (ninja 1.12) only
// dropped its own support for reading v4 logs, and v7 (ninja 1.13) switched
// the command hash from MurmurHash2 to rapidhash. We never interpret that
// hash, only compare it for equality, so v5, v6 and v7 all parse the same
// here. Anything newer is rejected rather than guessed at.
constexpr uint32_t kOldestSupportedVersion = 5;
constexpr uint32_t kNewestSupportedVersion = 7;

// A recompaction writes the log in the order of a hash map, so the end
// timestamp goes backwards on about half of its lines, against once per build
// in a log that was only appended to: a quarter is far from both.
constexpr size_t kRecompactedBackwardsRatio = 4;

// With fewer lines than this the rate above is noise: a handful of small
// builds of different targets gives the same numbers.
constexpr size_t kMinRecompactedLines = 16;

// The tracks are not tied to a process or a thread: a worker is a lane we
// make up to draw jobs that ran at the same time, not something ninja has.
constexpr auto kWorkerBlueprint = tracks::SliceBlueprint(
    "ninja_worker",
    tracks::DimensionBlueprints(tracks::UintDimensionBlueprint("worker")),
    tracks::FnNameBlueprint([](uint32_t worker) {
      return base::StackString<32>("Worker %u", worker);
    }));

constexpr auto kBuildsBlueprint =
    tracks::SliceBlueprint("ninja_builds",
                           tracks::DimensionBlueprints(),
                           tracks::StaticNameBlueprint("Builds"));

constexpr auto kActiveJobsBlueprint =
    tracks::CounterBlueprint("ninja_active_jobs",
                             tracks::UnknownUnitBlueprint(),
                             tracks::DimensionBlueprints(),
                             tracks::StaticNameBlueprint("Active jobs"));

constexpr int64_t kMsToNs = 1000 * 1000;
}  // namespace

NinjaLogParser::NinjaLogParser(TraceProcessorContext* ctx) : ctx_(ctx) {}
NinjaLogParser::~NinjaLogParser() = default;

base::Status NinjaLogParser::Parse(TraceBlobView blob) {
  // A trace is read in chunks of arbitrary size (for http fetch() pipeliniing),
  // not necessarily aligned on a line boundary.
  // Here we push everything into a vector and, on each call, consume only
  // the leading part until the last \n, keeping the rest for the next call.
  const char* src = reinterpret_cast<const char*>(blob.data());
  log_.insert(log_.end(), src, src + blob.size());

  // Find the last \n.
  size_t valid_size = log_.size();
  for (; valid_size > 0 && log_[valid_size - 1] != '\n'; --valid_size) {
  }

  RETURN_IF_ERROR(ParseLines(valid_size));
  log_.erase(log_.begin(), log_.begin() + static_cast<ssize_t>(valid_size));
  return base::OkStatus();
}

// Parses the first |size| bytes of |log_|, which must end with a \n.
base::Status NinjaLogParser::ParseLines(size_t size) {
  for (StringSplitter line(log_.data(), size, '\n'); line.Next();) {
    // A log that went through a Windows checkout or editor ends its lines
    // with \r\n. The \r belongs to neither the header nor the command hash.
    char* str = line.cur_token();
    size_t len = line.cur_token_size();
    if (len > 0 && str[len - 1] == '\r')
      str[--len] = '\0';
    if (len == 0)
      continue;

    static const char kHeader[] = "# ninja log v";
    if (!header_parsed_) {
      if (!base::StartsWith(str, kHeader))
        return base::ErrStatus("Failed to parse ninja log header");
      header_parsed_ = true;
      auto version = base::CStringToUInt32(str + strlen(kHeader));
      if (!version || *version < kOldestSupportedVersion ||
          *version > kNewestSupportedVersion) {
        return base::ErrStatus("Unsupported ninja log version: %s",
                               str + strlen(kHeader));
      }
      continue;
    }

    // Any other line starting with # is a comment, not a job that failed to
    // parse. ninja only writes the header this way, but the header shows up
    // again when two logs are concatenated.
    if (str[0] == '#')
      continue;

    // Each line in the ninja log looks like this:
    // 4 12  1579224178  ui/assets/modal.scss  832a958a9e234dfa
    // Where:
    // - [4, 12] are the timestamps in ms of [start, end] of the job, measured
    //     from the beginning of the build.
    // - 1579224178 is the "restat" (ignored).
    // - ui/assets/modal.scss is the name of the output file being built.
    // - 832a958a9e234dfa is a hash of the compiler invocation.
    // In most cases, each hash should be unique per ninja invocation (because
    // two rules shouln't generate the same output). However, in rare
    // circumstances the same hash can show up more than once. Examples:
    // - A GN action generates > 1 output per invocation (e.g., protos). In this
    //   case all items will have the same [start, end] timestamp. In this case
    //   we want to merge all the output names into one build step, because from
    //   the build system viewpoint, that was the same compiler/tool invocation.
    // - A subtle script that generates different outputs without taking a
    //   --output=filename argument (e.g. via env vars or similar). Note that
    //   this happens in the perfetto codebase itself (goto.google.com/nigew).
    //   In this case we want to treat the two entries as two distinct jobs.
    //
    // In summary the deduping logic here is: if both the hash and the
    // timestamps match -> merge, if not, keep distinct.
    StringSplitter tok(&line, '\t');
    auto t_start = base::CStringToInt64(tok.Next() ? tok.cur_token() : "");
    auto t_end = base::CStringToInt64(tok.Next() ? tok.cur_token() : "");
    tok.Next();  // Ignore restat.
    const char* name = tok.Next() ? tok.cur_token() : nullptr;
    auto cmdhash = base::CStringToUInt64(tok.Next() ? tok.cur_token() : "", 16);

    if (!t_start || !t_end || !name || !cmdhash) {
      ctx_->stats_tracker->IncrementStats(stats::ninja_parse_errors);
      continue;
    }

    jobs_.emplace_back(*t_start, *t_end, *cmdhash, name);
  }
  return base::OkStatus();
}

// Returns how many lines at the start of |jobs_| were written by a
// recompaction of the log rather than by a build: all of them if the log was
// not appended to since.
// ninja rewrites the log (on its own once the log is three times larger than
// the set of outputs, or on `ninja -t recompact`) keeping only the last entry
// of each output, and writes those in the order of its hash map. The lines it
// writes are told apart from the lines of a build by two things: no output
// shows up twice, and the end timestamp goes backwards on about half of the
// lines, while within a build it never does.
size_t NinjaLogParser::CountRecompactedLines() const {
  const size_t size = jobs_.size();

  // The lines written by a recompaction stop, at the latest, where an output
  // shows up for the second time.
  size_t unique_end = size;
  std::unordered_set<std::string_view> outputs;
  for (size_t i = 0; i < size; ++i) {
    if (!outputs.insert(jobs_[i].names).second) {
      unique_end = i;
      break;
    }
  }

  // They stop where a build starts, that is where the end timestamp goes
  // backwards, or at |unique_end|. Take the last such place for which the
  // lines before it look recompacted.
  size_t recompacted = 0;
  size_t backwards = 0;
  for (size_t i = 1; i <= unique_end; ++i) {
    const bool goes_backwards =
        i < size && jobs_[i].end_ms < jobs_[i - 1].end_ms;
    if ((goes_backwards || i == unique_end) && i >= kMinRecompactedLines &&
        backwards * kRecompactedBackwardsRatio >= i - 1) {
      recompacted = i;
    }
    backwards += goes_backwards;
  }
  return recompacted;
}

// This is called after the last Parse() call. At this point |jobs_| holds
// every line of the log but possibly the last one.
base::Status NinjaLogParser::OnPushDataToSorter() {
  // What is left in |log_| is the last line of a log that does not end with
  // \n. It is a job like any other.
  if (!log_.empty()) {
    log_.push_back('\n');
    RETURN_IF_ERROR(ParseLines(log_.size()));
    log_.clear();
  }

  // The lines of a recompacted log carry the timestamps of the builds that
  // wrote them, but nothing says which build that was. If builds were appended
  // after the recompaction we show those and drop the recompacted lines, which
  // would otherwise be read as thousands of one-line builds. If the whole log
  // is recompacted there is nothing else to show, so we keep the lines where
  // they are, as if they were one build.
  const size_t recompacted = CountRecompactedLines();
  const bool whole_log_recompacted = recompacted == jobs_.size();
  const size_t skipped = whole_log_recompacted ? 0 : recompacted;
  ctx_->stats_tracker->SetStats(stats::ninja_recompacted_entries_skipped,
                                static_cast<int64_t>(skipped));

  // The same log file usually contains several ninja invocations, appended
  // one after the other. Each invocation restarts its timestamps from zero,
  // and within an invocation ninja appends lines as jobs complete, so the
  // end timestamps only ever grow. An end timestamp that goes backwards is
  // therefore the start of a new build.
  // Those builds must not be laid on top of each other: the packing below
  // infers parallelism from overlapping timestamps, so superimposed builds
  // look like one build with several times the real number of workers. We
  // shift each build past the end of the previous one instead, which is what
  // ninjatracing does with --showall.
  std::vector<Job> jobs;
  jobs.reserve(jobs_.size() - skipped);
  struct Build {
    int64_t start_ms;
    int64_t end_ms;
  };
  std::vector<Build> builds;
  int64_t last_end_seen = 0;
  for (size_t i = skipped; i < jobs_.size(); ++i) {
    Job& job = jobs_[i];
    if (builds.empty() ||
        (!whole_log_recompacted && job.end_ms < last_end_seen)) {
      const int64_t start_ms = builds.empty() ? 0 : builds.back().end_ms;
      builds.push_back(Build{start_ms, start_ms});
    }
    Build& build = builds.back();
    last_end_seen = job.end_ms;
    job.start_ms += build.start_ms;
    job.end_ms += build.start_ms;
    build.end_ms = std::max(build.end_ms, job.end_ms);

    // If more hashes show up back-to-back with the same timestamps, merge them
    // together as they identify multiple outputs for the same build rule.
    // TODO(lalitm): this merging should really happen across the whole build
    // rather than between consecutive lines only. However, this needs some
    // non-significant rework of this class so it's not been found to be worth
    // implementing yet.
    if (!jobs.empty() && job.hash == jobs.back().hash &&
        job.start_ms == jobs.back().start_ms &&
        job.end_ms == jobs.back().end_ms) {
      jobs.back().names.append(", ");
      jobs.back().names.append(job.names);
      continue;
    }
    jobs.emplace_back(std::move(job));
  }
  jobs_.clear();

  SortByKey(jobs.begin(), jobs.end(),
            [](const Job& job) { return job.start_ms; });

  // Now we need to work out the job parallelism. There's no direct indication
  // of that in the ninja logs, so it must be inferred by observing overlapping
  // of timestamps. In this context a "Worker" is an inferred sequence of jobs
  // that happened concurrently with other sequences.
  // Here we pack jobs according the following heuristic, for the sake of making
  // the graph nicer to read to humans. Consider the initial situation:
  // 1: [  job 1 ]
  // 2:   [   job 2   ]
  // 3: [   job 3   ]
  //    T=0              | T=6
  // Assume that a new job starts at T=6. It's very likely that job4 was started
  // as a consequence of job2 completion (otherwise it could have been started
  // earlier, soon after job 1 or Job 3). It seems to make more sense to draw
  // it next in the 2nd worker, i.e. next to job 2.
  struct Worker {
    int64_t busy_until;
    TrackId track_id;
  };
  std::vector<Worker> workers;
  size_t builds_written = 0;
  for (const auto& job : jobs) {
    // One slice per build, from the moment ninja started (which is where its
    // timestamps count from) to the end of its last job. The slice table is
    // sorted by timestamp, so it has to go in before the jobs of that build.
    for (; builds_written < builds.size() &&
           builds[builds_written].start_ms <= job.start_ms;
         ++builds_written) {
      const Build& build = builds[builds_written];
      base::StackString<32> name("Build %zu", builds_written + 1);
      ctx_->slice_tracker->Scoped(
          build.start_ms * kMsToNs,
          ctx_->track_tracker->InternTrack(kBuildsBlueprint), StringId::Null(),
          ctx_->storage->InternString(name.string_view()),
          (build.end_ms - build.start_ms) * kMsToNs);
    }

    Worker* worker = nullptr;
    for (Worker& cur : workers) {
      // Pick the worker which has the greatest end time (busy_until) <= the
      // job's start time.
      if (cur.busy_until <= job.start_ms) {
        if (!worker || worker->busy_until < cur.busy_until)
          worker = &cur;
      }
    }
    if (worker) {
      // Update the worker's end time with the newly assigned job.
      worker->busy_until = job.end_ms;
    } else {
      // All workers are busy, allocate a new one.
      const auto worker_id = static_cast<uint32_t>(workers.size()) + 1;
      TrackId track_id = ctx_->track_tracker->InternTrack(
          kWorkerBlueprint, tracks::Dimensions(worker_id));
      workers.emplace_back(Worker{/*busy_until=*/job.end_ms, track_id});
      worker = &workers.back();
    }

    const int64_t start_ns = job.start_ms * kMsToNs;
    const int64_t dur_ns = (job.end_ms - job.start_ms) * kMsToNs;
    StringId name_id = ctx_->storage->InternString(base::StringView(job.names));
    ctx_->slice_tracker->Scoped(start_ns, worker->track_id, StringId::Null(),
                                name_id, dur_ns);
  }

  // The number of jobs running at each moment. Jobs that end at a timestamp
  // are counted out before the ones starting at it are counted in, and only
  // the outcome is written.
  std::vector<std::pair<int64_t, int32_t>> edges;
  edges.reserve(jobs.size() * 2);
  for (const auto& job : jobs) {
    edges.emplace_back(job.start_ms, 1);
    edges.emplace_back(job.end_ms, -1);
  }
  std::sort(edges.begin(), edges.end());
  int32_t active_jobs = 0;
  for (size_t i = 0; i < edges.size(); ++i) {
    active_jobs += edges[i].second;
    if (i + 1 < edges.size() && edges[i + 1].first == edges[i].first)
      continue;
    ctx_->event_tracker->PushCounter(
        edges[i].first * kMsToNs, static_cast<double>(active_jobs),
        ctx_->track_tracker->InternTrack(kActiveJobsBlueprint));
  }
  return base::OkStatus();
}

namespace {

// The ninja build log trace type (.ninja_log).
class NinjaLogImporter : public TraceImporter<NinjaLogImporter> {
 public:
  NinjaLogImporter() : TraceImporter(MakeDescriptor()) {}
  ~NinjaLogImporter() override;

  SniffResult Sniff(const uint8_t* data, size_t size) const override {
    static constexpr char kMagic[] = "# ninja log";
    size_t n = sizeof(kMagic) - 1;
    return {size >= n && memcmp(data, kMagic, n) == 0};
  }

  base::StatusOr<std::unique_ptr<ChunkedTraceReader>> CreateReader(
      TraceProcessorContext* context,
      uint32_t) const override {
    return std::unique_ptr<ChunkedTraceReader>(
        std::make_unique<NinjaLogParser>(context));
  }

 private:
  static TraceTypeDescriptor MakeDescriptor() {
    TraceTypeDescriptor d;
    d.name = "ninja_log";
    d.detection_priority = 183;
    // Ninja logs cannot be parsed nested inside another trace format.
    d.supports_nesting = false;
    return d;
  }
};

NinjaLogImporter::~NinjaLogImporter() = default;

}  // namespace

std::unique_ptr<TraceImporterBase> CreateNinjaLogImporter() {
  return std::make_unique<NinjaLogImporter>();
}

}  // namespace perfetto::trace_processor
