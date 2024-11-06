/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_H_

#include <array>
#include <cstddef>

namespace perfetto::trace_processor::tracks {

// The classification of a track indicates the "type of data" the track
// contains.
//
// Every track is uniquely identified by the the combination of the
// classification and a set of dimensions: classifications allow identifying a
// set of tracks with the same type of data within the whole universe of tracks
// while dimensions allow distinguishing between different tracks in that set.
#define PERFETTO_TP_TRACKS(F)                    \
  F(android_energy_estimation_breakdown_per_uid) \
  F(android_energy_estimation_breakdown)         \
  F(android_gpu_work_period)                     \
  F(android_lmk)                                 \
  F(chrome_process_instant)                      \
  F(cpu_capacity)                                \
  F(cpu_frequency_throttle)                      \
  F(cpu_frequency)                               \
  F(cpu_funcgraph)                               \
  F(cpu_idle_state)                              \
  F(cpu_idle)                                    \
  F(cpu_irq)                                     \
  F(cpu_nr_running)                              \
  F(cpu_mali_irq)                                \
  F(cpu_max_frequency_limit)                     \
  F(cpu_min_frequency_limit)                     \
  F(cpu_napi_gro)                                \
  F(cpu_softirq)                                 \
  F(cpu_stat)                                    \
  F(cpu_utilization)                             \
  F(gpu_frequency)                               \
  F(interconnect_events)                         \
  F(irq_counter)                                 \
  F(legacy_chrome_global_instants)               \
  F(linux_device_frequency)                      \
  F(linux_rpm)                                   \
  F(pkvm_hypervisor)                             \
  F(softirq_counter)                             \
  F(thread)                                      \
  F(track_event)                                 \
  F(triggers)                                    \
  F(unknown)

#define PERFETTO_TP_TRACKS_CLASSIFICATION_ENUM(name) name,
enum TrackClassification : size_t {
  PERFETTO_TP_TRACKS(PERFETTO_TP_TRACKS_CLASSIFICATION_ENUM)
};

#define PERFETTO_TP_TRACKS_CLASSIFICATION_STR(name) #name,
constexpr std::array kTrackClassificationStr{
    PERFETTO_TP_TRACKS(PERFETTO_TP_TRACKS_CLASSIFICATION_STR)};

constexpr const char* ToString(TrackClassification c) {
  return kTrackClassificationStr[c];
}

}  // namespace perfetto::trace_processor::tracks

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_H_
