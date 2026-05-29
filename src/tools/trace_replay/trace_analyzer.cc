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

#include "src/tools/trace_replay/trace_analyzer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/util/gzip_utils.h"

#include "protos/perfetto/common/trace_stats.gen.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

namespace perfetto {
namespace trace_replay {

namespace {

// First pass over the raw bytes — visits every TracePacket, transparently
// expanding any TracePacket that contains a `compressed_packets` field.
//
// A compressed_packets payload is a zlib-deflated stream of length-delimited
// Trace.packet records (see src/tracing/service/zlib_compressor.cc), so once
// inflated we can feed the bytes back through the same Trace::Decoder.
template <typename Visit>
bool IteratePackets(const uint8_t* data, size_t size, Visit&& visit) {
  protos::pbzero::Trace::Decoder td(data, size);
  for (auto p = td.packet(); p; ++p) {
    auto bytes = p->as_bytes();
    protos::pbzero::TracePacket::Decoder tp(bytes.data, bytes.size);
    if (tp.has_compressed_packets()) {
      auto blob = tp.compressed_packets();
      auto inflated = trace_processor::util::GzipDecompressor::DecompressFully(
          blob.data, blob.size);
      if (inflated.empty()) {
        PERFETTO_ELOG("Failed to inflate compressed_packets (%zu bytes in)",
                      blob.size);
        return false;
      }
      if (!IteratePackets(inflated.data(), inflated.size(), visit))
        return false;
    } else {
      visit(bytes.data, bytes.size, tp);
    }
  }
  return true;
}

// Field IDs that traced injects on the consumption path and that the
// PacketStreamValidator rejects when present at top-level on the producer
// side (see src/tracing/service/packet_stream_validator.cc). When we replay
// the captured TracePacket bytes, we must strip these or the entire packet
// gets dropped.
bool IsReservedTopLevelField(uint32_t id) {
  using TP = protos::pbzero::TracePacket;
  return id == TP::kTrustedUidFieldNumber ||
         id == TP::kTrustedPacketSequenceIdFieldNumber ||
         id == TP::kTraceConfigFieldNumber ||
         id == TP::kTraceStatsFieldNumber ||
         id == TP::kCompressedPacketsFieldNumber ||
         id == TP::kSynchronizationMarkerFieldNumber ||
         id == TP::kTrustedPidFieldNumber || id == TP::kMachineIdFieldNumber ||
         id == TP::kServiceEventFieldNumber ||
         id == TP::kTraceProvenanceFieldNumber ||
         id == TP::kProtovmsFieldNumber;
}

// Walks the inner TracePacket bytes and copies fields into `out`, skipping
// reserved fields (which would cause traced to drop the replayed packet).
// Returns the resulting byte vector.
std::vector<uint8_t> StripReservedFields(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size);
  protozero::ProtoDecoder dec(data, size);
  for (auto fld = dec.ReadField(); fld.valid(); fld = dec.ReadField()) {
    if (IsReservedTopLevelField(fld.id()))
      continue;
    // Compute the byte range covered by this field (preamble + payload). The
    // decoder advanced past it; the byte range starts at `data + start_off`
    // and ends at `data + read_off`. We track `start_off` ourselves by
    // sampling before the next ReadField — but ProtoDecoder doesn't expose a
    // "field byte range" directly, so reconstruct from the Field handle.
    //
    // Re-encode the field: preamble (tag) + payload (with length for LEN).
    using WT = protozero::proto_utils::ProtoWireType;
    WT wt = fld.type();
    uint8_t buf[16];
    uint8_t* p = buf;
    uint64_t tag =
        (static_cast<uint64_t>(fld.id()) << 3) | static_cast<uint64_t>(wt);
    p = protozero::proto_utils::WriteVarInt(tag, p);
    if (wt == WT::kLengthDelimited) {
      auto bytes = fld.as_bytes();
      p = protozero::proto_utils::WriteVarInt(static_cast<uint64_t>(bytes.size),
                                              p);
      out.insert(out.end(), buf, p);
      out.insert(out.end(), bytes.data, bytes.data + bytes.size);
    } else if (wt == WT::kVarInt) {
      // raw_int_value() preserves the encoded varint bytes.
      uint64_t v = fld.raw_int_value();
      p = protozero::proto_utils::WriteVarInt(v, p);
      out.insert(out.end(), buf, p);
    } else if (wt == WT::kFixed32) {
      out.insert(out.end(), buf, p);
      uint32_t v = fld.as_uint32();
      const uint8_t* vp = reinterpret_cast<const uint8_t*>(&v);
      out.insert(out.end(), vp, vp + 4);
    } else if (wt == WT::kFixed64) {
      out.insert(out.end(), buf, p);
      uint64_t v = fld.as_uint64();
      const uint8_t* vp = reinterpret_cast<const uint8_t*>(&v);
      out.insert(out.end(), vp, vp + 8);
    }
    // Unknown / unsupported types: drop (shouldn't happen in well-formed
    // input — ProtoDecoder would set ReadField()->valid() to false).
  }
  return out;
}

// Heuristic: identify the canonical data source name from the payload field
// present in a TracePacket. Returns nullptr if the packet doesn't clearly
// belong to a single recognizable data source.
const char* GuessDataSourceName(
    const protos::pbzero::TracePacket::Decoder& tp) {
  if (tp.has_ftrace_events())
    return "linux.ftrace";
  if (tp.has_process_tree() || tp.has_process_stats())
    return "linux.process_stats";
  if (tp.has_sys_stats())
    return "linux.sys_stats";
  if (tp.has_system_info() || tp.has_cpu_info())
    return "linux.system_info";
  if (tp.has_track_event() || tp.has_track_descriptor() ||
      tp.has_thread_descriptor() || tp.has_process_descriptor())
    return "track_event";
  if (tp.has_gpu_counter_event())
    return "gpu.counters";
  if (tp.has_gpu_render_stage_event())
    return "gpu.renderstages";
  if (tp.has_vulkan_memory_event())
    return "android.gpu.memory";
  if (tp.has_frame_timeline_event())
    return "android.surfaceflinger.frametimeline";
  if (tp.has_power_rails())
    return "android.power";
  if (tp.has_perf_sample())
    return "linux.perf";
  if (tp.has_chrome_events())
    return "org.chromium.trace_event";
  if (tp.has_profile_packet() || tp.has_streaming_profile_packet())
    return "android.heapprofd";  // or linux.perf; default to heapprofd
  if (tp.has_smaps_packet())
    return "android.smaps";
  if (tp.has_android_log())
    return "android.log";
  (void)tp;
  return nullptr;
}

// From a TraceConfig, build a name -> target_buffer map. If a data source is
// listed multiple times with the same name (possible with producer filters),
// the lowest target_buffer wins (arbitrary; matches the lookup intent).
std::unordered_map<std::string, uint32_t> BuildNameToBufferMap(
    const protos::gen::TraceConfig& cfg) {
  std::unordered_map<std::string, uint32_t> out;
  for (const auto& ds : cfg.data_sources()) {
    const std::string& name = ds.config().name();
    uint32_t buf = ds.config().target_buffer();
    auto it = out.find(name);
    if (it == out.end() || buf < it->second)
      out[name] = buf;
  }
  return out;
}

}  // namespace

base::Status AnalyzeTraceFile(const std::string& path,
                              const AnalyzeOptions& opts,
                              TraceAnalysis* out) {
  base::ScopedMmap mmap = base::ReadMmapWholeFile(path);
  if (!mmap.IsValid())
    return base::ErrStatus("Cannot mmap %s", path.c_str());

  const auto* data = static_cast<const uint8_t*>(mmap.data());
  const size_t size = mmap.length();

  bool config_found = false;
  bool have_trace_stats = false;
  uint64_t min_ts = std::numeric_limits<uint64_t>::max();

  constexpr uint32_t kClockBoot = 6;  // BUILTIN_CLOCK_BOOTTIME

  // Trace-wide default clock. Defaults to BOOTTIME per builtin_clock.proto;
  // can be overridden by the original config's primary_trace_clock or by the
  // first ClockSnapshot.primary_trace_clock.
  uint32_t default_clock_id = kClockBoot;

  // Per-sequence clock override from TracePacketDefaults.timestamp_clock_id.
  std::map<uint32_t, uint32_t> seq_default_clock;

  // Mini clock tracker. Built from the FIRST ClockSnapshot packet only — we
  // intentionally don't track subsequent snapshots or compensate for drift
  // (suspended time, frequency adjustments). For each clock id seen in that
  // snapshot, we record the offset to the reference (= default_clock_id)
  // such that:  ref_ts_ns = local_ts_ns + clock_offset[id].
  // Any clock id not in this map (most commonly clock_id=64, the
  // sequence-local incremental clock used by track_event) is treated as
  // "untranslatable" and its packets fire immediately (rel_ts_ns=0).
  std::map<uint32_t, int64_t> clock_offset;
  bool have_clock_snapshot = false;

  // Per-packet captured data. `ts_set` distinguishes "timestamp 0 because the
  // producer set it to 0" from "timestamp 0 because the field was absent" —
  // the former participates in min_ts, the latter inherits the prior packet's
  // timestamp on the same sequence. `clock_id` is the effective clock as
  // determined by per-packet override / sequence default / trace default. It
  // is used in a second pass to translate to BOOT-normalised ns once the
  // MONO offset is known.
  struct Raw {
    int32_t pid;
    uint32_t seq_id;
    uint64_t ts;
    bool ts_set;
    uint32_t clock_id;
    const uint8_t* p;
    size_t len;
    const char* inferred_ds_name;  // const-string literal, no ownership.
  };
  std::vector<Raw> records;
  records.reserve(1u << 16);

  if (!IteratePackets(
          data, size,
          [&](const uint8_t* pkt_data, size_t pkt_size,
              protos::pbzero::TracePacket::Decoder& tp) {
            if (tp.has_trace_config() && !config_found) {
              auto cfg_bytes = tp.trace_config();
              if (!out->original_config.ParseFromArray(cfg_bytes.data,
                                                       cfg_bytes.size)) {
                PERFETTO_ELOG("Failed to parse embedded TraceConfig");
              } else {
                config_found = true;
                // Honour primary_trace_clock if the trace overrides BOOT.
                if (out->original_config.builtin_data_sources()
                        .has_primary_trace_clock()) {
                  default_clock_id = static_cast<uint32_t>(
                      out->original_config.builtin_data_sources()
                          .primary_trace_clock());
                }
              }
            }
            if (tp.has_trace_stats()) {
              protos::gen::TraceStats stats;
              auto sbytes = tp.trace_stats();
              if (stats.ParseFromArray(sbytes.data, sbytes.size)) {
                for (const auto& ws : stats.writer_stats()) {
                  out->seq_to_buf[ws.sequence_id()] = ws.buffer();
                }
                if (!stats.writer_stats().empty())
                  have_trace_stats = true;
              }
            }

            // Capture the first ClockSnapshot. We index every clock id it
            // contains and compute its offset to the trace-default clock —
            // no further drift compensation, no later snapshots consulted.
            if (!have_clock_snapshot && tp.has_clock_snapshot()) {
              auto cs_bytes = tp.clock_snapshot();
              protos::pbzero::ClockSnapshot::Decoder cs(cs_bytes.data,
                                                        cs_bytes.size);
              // Honour ClockSnapshot.primary_trace_clock if set.
              if (cs.has_primary_trace_clock()) {
                default_clock_id =
                    static_cast<uint32_t>(cs.primary_trace_clock());
              }
              std::map<uint32_t, uint64_t> snap_ts;
              for (auto c = cs.clocks(); c; ++c) {
                protos::pbzero::ClockSnapshot::Clock::Decoder ck(c->as_bytes());
                snap_ts[ck.clock_id()] = ck.timestamp();
              }
              auto rit = snap_ts.find(default_clock_id);
              if (rit != snap_ts.end()) {
                const int64_t ref = static_cast<int64_t>(rit->second);
                for (auto& kv : snap_ts) {
                  clock_offset[kv.first] =
                      ref - static_cast<int64_t>(kv.second);
                }
                have_clock_snapshot = true;
              }
            }

            uint32_t seq_id = tp.trusted_packet_sequence_id();
            if (seq_id == 0)
              return;
            if (seq_id == 1) {
              out->skipped_service_packets++;
              return;
            }
            if (!tp.has_trusted_pid()) {
              out->skipped_no_pid_packets++;
              return;
            }
            // Update per-sequence default from TracePacketDefaults (sent
            // typically in the first packet of a sequence).
            if (tp.has_trace_packet_defaults()) {
              auto def_bytes = tp.trace_packet_defaults();
              protos::pbzero::TracePacketDefaults::Decoder defs(def_bytes.data,
                                                                def_bytes.size);
              if (defs.has_timestamp_clock_id()) {
                uint32_t ck = defs.timestamp_clock_id();
                if (ck != 0)
                  seq_default_clock[seq_id] = ck;
              }
            }

            int32_t pid = tp.trusted_pid();
            uint64_t ts = tp.timestamp();
            bool ts_set = tp.has_timestamp() && ts != 0;

            // Effective clock id: per-packet > sequence default > trace
            // default. 0 means "unspecified" in the wire format.
            uint32_t eff_clock = default_clock_id;
            auto sit = seq_default_clock.find(seq_id);
            if (sit != seq_default_clock.end())
              eff_clock = sit->second;
            if (tp.has_timestamp_clock_id() && tp.timestamp_clock_id() != 0) {
              eff_clock = tp.timestamp_clock_id();
            }

            // min_ts is only meaningful for translatable timestamps; we
            // compute it in the second pass once the clock tracker is built.

            const char* name = GuessDataSourceName(tp);
            records.push_back(
                {pid, seq_id, ts, ts_set, eff_clock, pkt_data, pkt_size, name});
          })) {
    return base::ErrStatus("Failed to iterate packets in %s", path.c_str());
  }

  if (!config_found)
    return base::ErrStatus(
        "No TraceConfig packet in input trace; cannot derive replay config");

  if (!have_trace_stats) {
    PERFETTO_ELOG(
        "Input trace has no usable trace_stats.writer_stats. The "
        "sequence_id->buffer mapping will be inferred from packet content "
        "(by data source name) using the original TraceConfig.");
  }

  // No strict-mode check anymore: packets in clocks not present in the
  // first ClockSnapshot are silently treated as "fire immediately" in the
  // second pass below.

  // Build the content-based fallback map.
  const auto name_to_buf = BuildNameToBufferMap(out->original_config);

  // For each sequence_id without a writer_stats mapping, pick the most-popular
  // inferred data-source name across its packets, then look up the buffer.
  std::map<uint32_t, std::unordered_map<const char*, uint32_t>> seq_name_votes;
  for (const auto& r : records) {
    if (out->seq_to_buf.count(r.seq_id))
      continue;
    if (r.inferred_ds_name)
      seq_name_votes[r.seq_id][r.inferred_ds_name]++;
  }
  // seq_id -> (buffer, source) where source is 'stats'|'content'|'default'.
  enum class Source { kStats, kContent, kDefault };
  std::map<uint32_t, std::pair<uint32_t, Source>> resolved;
  for (const auto& kv : out->seq_to_buf)
    resolved[static_cast<uint32_t>(kv.first)] = {kv.second, Source::kStats};

  uint64_t seq_resolved_by_content = 0;
  uint64_t seq_defaulted = 0;
  std::set<uint32_t> orphan_unresolved;
  for (auto& kv : seq_name_votes) {
    if (kv.second.empty())
      continue;
    const char* best_name = nullptr;
    uint32_t best_votes = 0;
    for (auto& vk : kv.second) {
      if (vk.second > best_votes) {
        best_votes = vk.second;
        best_name = vk.first;
      }
    }
    auto it = name_to_buf.find(std::string(best_name));
    if (it != name_to_buf.end()) {
      resolved[kv.first] = {it->second, Source::kContent};
      seq_resolved_by_content++;
    }
  }
  // Any seq_id not in `resolved` after the above is still unmapped.
  for (const auto& r : records) {
    if (resolved.count(r.seq_id) == 0)
      orphan_unresolved.insert(r.seq_id);
  }

  // Default-to-buf-0 fallback (unless ignore_orphan_writers and we drop).
  if (!orphan_unresolved.empty()) {
    if (opts.ignore_orphan_writers) {
      PERFETTO_ELOG(
          "%zu sequence_id(s) could not be mapped to a buffer (no stats, no "
          "content match); dropping their packets as requested.",
          orphan_unresolved.size());
    } else {
      PERFETTO_ELOG(
          "%zu sequence_id(s) could not be mapped to a buffer (no stats, no "
          "content match); defaulting them to buffer 0.",
          orphan_unresolved.size());
      for (uint32_t s : orphan_unresolved) {
        resolved[s] = {0, Source::kDefault};
        seq_defaulted++;
      }
    }
  }

  if (records.empty()) {
    out->min_ts_ns = 0;
    out->max_rel_ts_ns = 0;
    return base::OkStatus();
  }

  // First pass: translate every (ts, clock) into reference-clock-equivalent
  // nanoseconds using the offset map built from the first ClockSnapshot.
  // Packets in clocks absent from the snapshot get boot_ts_set=false, which
  // causes them to inherit the prior packet's timestamp on the same
  // sequence (or rel_ts=0 if there is none yet) in the emission loop.
  uint64_t min_boot_ts = std::numeric_limits<uint64_t>::max();
  std::vector<uint64_t> boot_ts(records.size(), 0);
  std::vector<bool> boot_ts_set(records.size(), false);
  uint64_t untranslatable_packets = 0;
  for (size_t i = 0; i < records.size(); i++) {
    const Raw& r = records[i];
    if (!r.ts_set)
      continue;
    int64_t off = 0;
    if (r.clock_id == default_clock_id) {
      off = 0;
    } else {
      auto oit = clock_offset.find(r.clock_id);
      if (oit == clock_offset.end()) {
        untranslatable_packets++;
        continue;
      }
      off = oit->second;
    }
    int64_t v = static_cast<int64_t>(r.ts) + off;
    if (v < 0)
      v = 0;
    uint64_t bts = static_cast<uint64_t>(v);
    boot_ts[i] = bts;
    boot_ts_set[i] = true;
    min_boot_ts = std::min(min_boot_ts, bts);
  }
  out->min_ts_ns =
      min_boot_ts == std::numeric_limits<uint64_t>::max() ? 0 : min_boot_ts;
  (void)min_ts;  // Pre-translation min was only used for diagnostics.

  // For each sequence_id, remember the last *translated-to-BOOT* timestamp
  // so packets with no timestamp set (or with an unsupported clock) can
  // inherit it (they emit right after the prior packet on the same writer).
  // If a sequence's very first packets lack a usable timestamp, default to t0.
  std::map<uint32_t, uint64_t> last_seq_ts;

  for (size_t i = 0; i < records.size(); i++) {
    const Raw& r = records[i];
    auto it = resolved.find(r.seq_id);
    if (it == resolved.end()) {
      out->mapping_stats.packets_dropped_orphan++;
      continue;
    }
    uint64_t ts;
    if (boot_ts_set[i]) {
      ts = boot_ts[i];
      last_seq_ts[r.seq_id] = ts;
    } else {
      auto lt = last_seq_ts.find(r.seq_id);
      ts = lt == last_seq_ts.end() ? out->min_ts_ns : lt->second;
    }
    uint64_t rel = ts >= out->min_ts_ns ? ts - out->min_ts_ns : 0;
    if (opts.zero_delay)
      rel = 0;  // Skip pacing — fire every packet ASAP.
    ReplayRecord rec;
    rec.rel_ts_ns = rel;
    rec.orig_seq_id = r.seq_id;
    rec.buffer_idx = it->second.first;
    rec.bytes = StripReservedFields(r.p, r.len);
    out->max_rel_ts_ns = std::max(out->max_rel_ts_ns, rel);
    out->total_packets++;
    switch (it->second.second) {
      case Source::kStats:
        out->mapping_stats.packets_resolved_by_stats++;
        break;
      case Source::kContent:
        out->mapping_stats.packets_resolved_by_content++;
        break;
      case Source::kDefault:
        out->mapping_stats.packets_defaulted_to_buf0++;
        break;
    }
    out->records_by_pid[r.pid].push_back(std::move(rec));
  }

  for (auto& kv : out->records_by_pid) {
    std::stable_sort(kv.second.begin(), kv.second.end(),
                     [](const ReplayRecord& a, const ReplayRecord& b) {
                       return a.rel_ts_ns < b.rel_ts_ns;
                     });
  }

  PERFETTO_LOG("seq mapping: %zu by writer_stats, %" PRIu64
               " by content, %" PRIu64
               " defaulted to buf0 (totals are seq counts)",
               out->seq_to_buf.size(), seq_resolved_by_content, seq_defaulted);
  if (have_clock_snapshot) {
    std::string parts;
    for (auto& kv : clock_offset) {
      if (!parts.empty())
        parts += ", ";
      parts += std::to_string(kv.first) + "->" + std::to_string(kv.second);
    }
    PERFETTO_LOG("clock tracker: ref=%u, offsets (ns): %s", default_clock_id,
                 parts.c_str());
  }
  if (untranslatable_packets > 0) {
    PERFETTO_LOG("clock tracker: %" PRIu64
                 " packet(s) had timestamps in clocks not present in the first "
                 "ClockSnapshot; they will fire immediately.",
                 untranslatable_packets);
  }

  return base::OkStatus();
}

}  // namespace trace_replay
}  // namespace perfetto
