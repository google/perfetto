/*
 * Copyright (C) 2019 The Android Open Source Project
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
#include "src/traced/probes/ftrace/atrace_hal_wrapper.h"

#include "src/android_internal/atrace_hal.h"
#include "src/android_internal/lazy_library_loader.h"

namespace perfetto {

namespace {
constexpr size_t kMaxNumCategories = 64;
}

struct AtraceHalWrapper::DynamicLibLoader {
  PERFETTO_LAZY_LOAD(android_internal::GetCategories, get_categories_);

  std::vector<android_internal::TracingVendorCategory> GetCategories() {
    if (!get_categories_)
      return std::vector<android_internal::TracingVendorCategory>();

    std::vector<android_internal::TracingVendorCategory> categories(
        kMaxNumCategories);
    size_t num_cat = categories.size();
    get_categories_(&categories[0], &num_cat);
    categories.resize(num_cat);
    return categories;
  }
};

AtraceHalWrapper::AtraceHalWrapper() {
  lib_.reset(new DynamicLibLoader());
}

AtraceHalWrapper::~AtraceHalWrapper() = default;

std::vector<AtraceHalWrapper::TracingVendorCategory>
AtraceHalWrapper::GetAvailableCategories() {
  auto details = lib_->GetCategories();
  std::vector<AtraceHalWrapper::TracingVendorCategory> result;
  for (size_t i = 0; i < details.size(); i++) {
    AtraceHalWrapper::TracingVendorCategory cat;
    cat.name = details[i].name;
    cat.description = details[i].description;
    result.emplace_back(cat);
  }
  return result;
}

}  // namespace perfetto
