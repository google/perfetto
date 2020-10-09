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

#include "src/traced/probes/ftrace/kallsyms/lazy_kernel_symbolizer.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"
#include "src/traced/probes/ftrace/kallsyms/kernel_symbol_map.h"

namespace perfetto {

LazyKernelSymbolizer::LazyKernelSymbolizer() = default;
LazyKernelSymbolizer::~LazyKernelSymbolizer() = default;

KernelSymbolMap* LazyKernelSymbolizer::GetOrCreateKernelSymbolMap() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (symbol_map_)
    return symbol_map_.get();

  symbol_map_.reset(new KernelSymbolMap());
  std::string kptr_restrict;
  static const char kPath[] = "/proc/sys/kernel/kptr_restrict";
  base::ScopedFile fd = base::OpenFile(kPath, O_RDONLY);

  // If kptr_restrict is set, try temporarily lifting it (it works only if
  // traced_probes is run as a privileged user).
  if (fd && base::ReadFileDescriptor(*fd, &kptr_restrict)) {
    if (kptr_restrict.empty() || kptr_restrict[0] != '0') {
      fd = base::OpenFile(kPath, O_WRONLY);  // Just O_RDWR once doesn't work.
      base::WriteAll(*fd, "0", 1);
      fd.reset();
    }
  }

  symbol_map_->Parse("/proc/kallsyms");

  // Restore kptr_restrict to the old value.
  if (!kptr_restrict.empty()) {
    fd = base::OpenFile(kPath, O_WRONLY);
    base::WriteAll(*fd, kptr_restrict.data(), kptr_restrict.size());
  }

  return symbol_map_.get();
}

void LazyKernelSymbolizer::Destroy() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  symbol_map_.reset();
  base::MaybeReleaseAllocatorMemToOS();  // For Scudo, b/170217718.
}

}  // namespace perfetto
