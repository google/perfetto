/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "perfetto/public/abi/track_event_abi.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>

#include "perfetto/base/compiler.h"
#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/public/compiler.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/basic_types.h"
#include "perfetto/tracing/internal/data_source_internal.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/track.h"
#include "protos/perfetto/common/data_source_descriptor.gen.h"
#include "protos/perfetto/common/track_event_descriptor.pbzero.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/track_event/track_event_config.gen.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/shared_lib/reset_for_testing.h"

struct PerfettoTeCategoryImpl* perfetto_te_any_categories;

PERFETTO_ATOMIC(bool) * perfetto_te_any_categories_enabled;

struct PerfettoTeCategoryImpl {
  std::atomic<bool> flag{false};
  std::atomic<uint8_t> instances{0};
  PerfettoTeCategoryDescriptor* desc = nullptr;
  uint64_t cat_iid = 0;
  PerfettoTeCategoryImplCallback cb = nullptr;
  void* cb_user_arg = nullptr;
};

enum class MatchType { kExact, kPattern };

static bool NameMatchesPattern(const std::string& pattern,
                               const perfetto::base::StringView& name,
                               MatchType match_type) {
  // To avoid pulling in all of std::regex, for now we only support a single "*"
  // wildcard at the end of the pattern.
  size_t i = pattern.find('*');
  if (i != std::string::npos) {
    if (match_type != MatchType::kPattern)
      return false;
    return name.substr(0, i) ==
           perfetto::base::StringView(pattern).substr(0, i);
  }
  return name == perfetto::base::StringView(pattern);
}

static bool NameMatchesPatternList(const std::vector<std::string>& patterns,
                                   const perfetto::base::StringView& name,
                                   MatchType match_type) {
  for (const auto& pattern : patterns) {
    if (NameMatchesPattern(pattern, name, match_type))
      return true;
  }
  return false;
}

static bool IsSingleCategoryEnabled(
    const PerfettoTeCategoryDescriptor& c,
    const perfetto::protos::gen::TrackEventConfig& config) {
  auto has_matching_tag = [&](std::function<bool(const char*)> matcher) {
    for (size_t i = 0; i < c.num_tags; ++i) {
      if (matcher(c.tags[i]))
        return true;
    }
    return false;
  };
  // First try exact matches, then pattern matches.
  const std::array<MatchType, 2> match_types = {
      {MatchType::kExact, MatchType::kPattern}};
  for (auto match_type : match_types) {
    // 1. Enabled categories.
    if (NameMatchesPatternList(config.enabled_categories(), c.name,
                               match_type)) {
      return true;
    }

    // 2. Enabled tags.
    if (has_matching_tag([&](const char* tag) {
          return NameMatchesPatternList(config.enabled_tags(), tag, match_type);
        })) {
      return true;
    }

    // 3. Disabled categories.
    if (NameMatchesPatternList(config.disabled_categories(), c.name,
                               match_type)) {
      return false;
    }

    // 4. Disabled tags.
    if (has_matching_tag([&](const char* tag) {
          return NameMatchesPatternList(config.disabled_tags(), tag,
                                        match_type);
        })) {
      return false;
    }
  }

  // If nothing matched, enable the category by default.
  return true;
}

static bool IsRegisteredCategoryEnabled(
    const PerfettoTeCategoryImpl& cat,
    const perfetto::protos::gen::TrackEventConfig& config) {
  if (!cat.desc) {
    return false;
  }
  return IsSingleCategoryEnabled(*cat.desc, config);
}

static void EnableRegisteredCategory(PerfettoTeCategoryImpl* cat,
                                     uint32_t instance_index) {
  PERFETTO_DCHECK(instance_index < perfetto::internal::kMaxDataSourceInstances);
  // Matches the acquire_load in DataSource::Trace().
  uint8_t old = cat->instances.fetch_or(
      static_cast<uint8_t>(1u << instance_index), std::memory_order_release);
  bool global_state_changed = old == 0;
  if (global_state_changed) {
    cat->flag.store(true, std::memory_order_relaxed);
  }
  if (cat->cb) {
    cat->cb(cat, instance_index, /*created=*/true, global_state_changed,
            cat->cb_user_arg);
  }
}

static void DisableRegisteredCategory(PerfettoTeCategoryImpl* cat,
                                      uint32_t instance_index) {
  PERFETTO_DCHECK(instance_index < perfetto::internal::kMaxDataSourceInstances);
  // Matches the acquire_load in DataSource::Trace().
  cat->instances.fetch_and(static_cast<uint8_t>(~(1u << instance_index)),
                           std::memory_order_release);
  bool global_state_changed = false;
  if (!cat->instances.load(std::memory_order_relaxed)) {
    cat->flag.store(false, std::memory_order_relaxed);
    global_state_changed = true;
  }
  if (cat->cb) {
    cat->cb(cat, instance_index, /*created=*/false, global_state_changed,
            cat->cb_user_arg);
  }
}

static void SerializeCategory(
    const PerfettoTeCategoryDescriptor& desc,
    perfetto::protos::pbzero::TrackEventDescriptor* ted) {
  auto* c = ted->add_available_categories();
  c->set_name(desc.name);
  if (desc.desc)
    c->set_description(desc.desc);
  for (size_t j = 0; j < desc.num_tags; ++j) {
    c->add_tags(desc.tags[j]);
  }
}

namespace perfetto {
namespace shlib {

class TrackEvent
    : public perfetto::DataSource<TrackEvent, DefaultDataSourceTraits> {
 public:
  ~TrackEvent() override;
  void OnSetup(const DataSourceBase::SetupArgs& args) override {
    const std::string& config_raw = args.config->track_event_config_raw();
    bool ok = config_.ParseFromArray(config_raw.data(), config_raw.size());
    if (!ok) {
      PERFETTO_LOG("Failed to parse config");
    }
    inst_id_ = args.internal_instance_index;
  }

  void OnStart(const DataSourceBase::StartArgs&) override {
    GlobalState::Instance().OnStart(config_, inst_id_);
  }

  void OnStop(const DataSourceBase::StopArgs&) override {
    GlobalState::Instance().OnStop(inst_id_);
  }

  const perfetto::protos::gen::TrackEventConfig& GetConfig() const {
    return config_;
  }

  static void Init() {
    DataSourceDescriptor dsd =
        GlobalState::Instance().GenerateDescriptorFromCategories();
    Register(dsd);
  }

  static void RegisterCategory(PerfettoTeCategoryImpl* cat) {
    GlobalState::Instance().RegisterCategory(cat);
  }

  static void UpdateDescriptorFromCategories() {
    DataSourceDescriptor dsd =
        GlobalState::Instance().GenerateDescriptorFromCategories();
    UpdateDescriptor(dsd);
  }

  static void UnregisterCategory(PerfettoTeCategoryImpl* cat) {
    GlobalState::Instance().UnregisterCategory(cat);
  }

  static void CategorySetCallback(struct PerfettoTeCategoryImpl* cat,
                                  PerfettoTeCategoryImplCallback cb,
                                  void* user_arg) {
    GlobalState::Instance().CategorySetCallback(cat, cb, user_arg);
  }

  static internal::DataSourceType* GetType();

  static internal::DataSourceThreadLocalState** GetTlsState();

 private:
  struct GlobalState {
    static GlobalState& Instance() {
      static GlobalState* instance = new GlobalState();
      return *instance;
    }

    void OnStart(const perfetto::protos::gen::TrackEventConfig& config,
                 uint32_t instance_id) {
      std::lock_guard<std::mutex> lock(mu_);
      EnableRegisteredCategory(perfetto_te_any_categories, instance_id);
      for (PerfettoTeCategoryImpl* cat : categories_) {
        if (IsRegisteredCategoryEnabled(*cat, config)) {
          EnableRegisteredCategory(cat, instance_id);
        }
      }
    }

    void OnStop(uint32_t instance_id) {
      std::lock_guard<std::mutex> lock(GlobalState::Instance().mu_);
      for (PerfettoTeCategoryImpl* cat : GlobalState::Instance().categories_) {
        DisableRegisteredCategory(cat, instance_id);
      }
      DisableRegisteredCategory(perfetto_te_any_categories, instance_id);
    }

    void RegisterCategory(PerfettoTeCategoryImpl* cat) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        Trace([cat](TraceContext ctx) {
          auto ds = ctx.GetDataSourceLocked();

          if (IsRegisteredCategoryEnabled(*cat, ds->GetConfig())) {
            EnableRegisteredCategory(cat, ds->inst_id_);
          }
        });
        categories_.push_back(cat);
        cat->cat_iid = ++GlobalState::Instance().interned_categories_;
      }
    }

    void UnregisterCategory(PerfettoTeCategoryImpl* cat) {
      std::lock_guard<std::mutex> lock(mu_);
      categories_.erase(
          std::remove(categories_.begin(), categories_.end(), cat),
          categories_.end());
    }

    void CategorySetCallback(struct PerfettoTeCategoryImpl* cat,
                             PerfettoTeCategoryImplCallback cb,
                             void* user_arg) {
      std::lock_guard<std::mutex> lock(mu_);
      cat->cb = cb;
      cat->cb_user_arg = user_arg;
      if (!cat->cb) {
        return;
      }

      bool first = true;
      uint8_t active_instances = cat->instances.load(std::memory_order_relaxed);
      for (PerfettoDsInstanceIndex i = 0; i < internal::kMaxDataSourceInstances;
           i++) {
        if ((active_instances & (1 << i)) == 0) {
          continue;
        }
        cb(cat, i, true, first, user_arg);
        first = false;
      }
    }

    DataSourceDescriptor GenerateDescriptorFromCategories() const {
      DataSourceDescriptor dsd;
      dsd.set_name("track_event");

      protozero::HeapBuffered<perfetto::protos::pbzero::TrackEventDescriptor>
          ted;
      for (PerfettoTeCategoryImpl* cat : categories_) {
        SerializeCategory(*cat->desc, ted.get());
      }
      dsd.set_track_event_descriptor_raw(ted.SerializeAsString());
      return dsd;
    }

   private:
    GlobalState() : interned_categories_(0) {
      perfetto_te_any_categories = new PerfettoTeCategoryImpl;
      perfetto_te_any_categories_enabled = &perfetto_te_any_categories->flag;
    }

    // Guards categories and interned_categories;
    std::mutex mu_;
    std::vector<PerfettoTeCategoryImpl*> categories_;
    uint64_t interned_categories_;
  };

  uint32_t inst_id_;
  perfetto::protos::gen::TrackEventConfig config_;
};

TrackEvent::~TrackEvent() = default;

void ResetTrackEventTls() {
  *TrackEvent::GetTlsState() = nullptr;
}

struct TracePointTraits {
  struct TracePointData {
    struct PerfettoTeCategoryImpl* enabled;
  };
  static constexpr std::atomic<uint8_t>* GetActiveInstances(
      TracePointData data) {
    return &data.enabled->instances;
  }
};

}  // namespace shlib
}  // namespace perfetto

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(perfetto::shlib::TrackEvent);

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(perfetto::shlib::TrackEvent);

perfetto::internal::DataSourceType* perfetto::shlib::TrackEvent::GetType() {
  return &perfetto::shlib::TrackEvent::Helper::type();
}

perfetto::internal::DataSourceThreadLocalState**
perfetto::shlib::TrackEvent::GetTlsState() {
  return &tls_state_;
}

struct PerfettoTeCategoryImpl* PerfettoTeCategoryImplCreate(
    struct PerfettoTeCategoryDescriptor* desc) {
  auto* cat = new PerfettoTeCategoryImpl;
  cat->desc = desc;
  perfetto::shlib::TrackEvent::RegisterCategory(cat);
  return cat;
}

void PerfettoTePublishCategories() {
  perfetto::shlib::TrackEvent::UpdateDescriptorFromCategories();
}

void PerfettoTeCategoryImplSetCallback(struct PerfettoTeCategoryImpl* cat,
                                       PerfettoTeCategoryImplCallback cb,
                                       void* user_arg) {
  perfetto::shlib::TrackEvent::CategorySetCallback(cat, cb, user_arg);
}

PERFETTO_ATOMIC(bool) *
    PerfettoTeCategoryImplGetEnabled(struct PerfettoTeCategoryImpl* cat) {
  return &cat->flag;
}

uint64_t PerfettoTeCategoryImplGetIid(struct PerfettoTeCategoryImpl* cat) {
  return cat->cat_iid;
}

void PerfettoTeCategoryImplDestroy(struct PerfettoTeCategoryImpl* cat) {
  perfetto::shlib::TrackEvent::UnregisterCategory(cat);
  delete cat;
}

void PerfettoTeInit(void) {
  perfetto::shlib::TrackEvent::Init();
}
