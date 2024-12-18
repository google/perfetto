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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_INTERNAL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::tracks {

template <typename... T>
using DimensionsT = std::tuple<T...>;

struct DimensionBlueprintBase {
  std::string_view name;
};

template <typename T>
struct DimensionBlueprintT : DimensionBlueprintBase {
  using type = T;
};

struct NameBlueprintT {
  struct Auto {
    using name_t = nullptr_t;
  };
  struct Static {
    using name_t = nullptr_t;
    const char* name;
  };
  struct Dynamic {
    using name_t = StringPool::Id;
  };
  struct FnBase {
    using name_t = nullptr_t;
  };
  template <typename F>
  struct Fn : FnBase {
    F fn;
  };
};

struct BlueprintBase {
  std::string_view event_type;
  std::string_view classification;
  base::Hasher hasher;
  std::array<DimensionBlueprintBase, 8> dimension_blueprints;
};

template <typename NB, typename UB, typename... DB>
struct BlueprintT : BlueprintBase {
  using name_blueprint_t = NB;
  using unit_blueprint_t = UB;
  using name_t = typename NB::name_t;
  using unit_t = typename UB::unit_t;
  using dimension_blueprints_t = std::tuple<DB...>;
  using dimensions_t = DimensionsT<typename DB::type...>;
  name_blueprint_t name_blueprint;
  unit_blueprint_t unit_blueprint;
};

template <typename... T>
using DimensionBlueprintsT = std::tuple<T...>;

struct UnitBlueprintT {
  struct Unknown {
    using unit_t = nullptr_t;
  };
  struct Static {
    using unit_t = const char*;
    const char* name;
  };
  struct Dynamic {
    using unit_t = StringPool::Id;
  };
};

template <typename BlueprintT, typename Dims>
constexpr uint64_t HashFromBlueprintAndDimensions(const BlueprintT& bp,
                                                  const Dims& dims) {
  base::Hasher hasher(bp.hasher);
  std::apply([&](auto&&... args) { ((hasher.Update(args)), ...); }, dims);
  return hasher.digest();
}

#define PERFETTO_TP_TRACKS(F)                    \
  F(android_energy_estimation_breakdown_per_uid) \
  F(android_energy_estimation_breakdown)         \
  F(android_gpu_work_period)                     \
  F(android_lmk)                                 \
  F(block_io)                                    \
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
  F(pixel_cpm_trace)                             \
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

namespace internal {

#define PERFETTO_TP_TRACKS_CLASSIFICATION_STR(name) #name,
constexpr std::array kTrackClassificationStr{
    PERFETTO_TP_TRACKS(PERFETTO_TP_TRACKS_CLASSIFICATION_STR)};

}  // namespace internal

constexpr const char* ToString(TrackClassification c) {
  return internal::kTrackClassificationStr[c];
}

}  // namespace perfetto::trace_processor::tracks

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACKS_INTERNAL_H_
