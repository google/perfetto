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

#include "src/profiling/memory/page_idle_checker.h"
#include "perfetto/ext/base/utils.h"
#include "src/profiling/memory/utils.h"

#include <vector>

namespace perfetto {
namespace profiling {
namespace {

constexpr uint64_t kIsInRam = 1ULL << 63;
constexpr uint64_t kRamPhysicalPageMask = ~(~0ULL << 55);

constexpr uint64_t kPhysPageReferenced = 1ULL << 2;

}  // namespace

int64_t PageIdleChecker::OnIdlePage(uint64_t addr, size_t size) {
  uint64_t page_nr = addr / base::kPageSize;
  uint64_t page_aligned_addr = page_nr * base::kPageSize;
  uint64_t end_page_nr = (addr + size) / base::kPageSize;
  // The trailing division will have rounded down, unless the end is at a page
  // boundary. Add one page if we rounded down.
  if (addr + size % base::kPageSize != 0)
    end_page_nr++;
  uint64_t page_aligned_end_addr = base::kPageSize * end_page_nr;

  size_t pages = (page_aligned_end_addr - page_aligned_addr) / base::kPageSize;
  std::vector<uint64_t> virt_page_infos(pages);

  off64_t virt_off = static_cast<off64_t>(page_nr * sizeof(virt_page_infos[0]));
  size_t virt_rd_size = pages * sizeof(virt_page_infos[0]);
  if (ReadAtOffsetClobberSeekPos(*pagemap_fd_, &(virt_page_infos[0]),
                                 virt_rd_size, virt_off) !=
      static_cast<ssize_t>(virt_rd_size)) {
    return -1;
  }

  int64_t idle_mem = 0;

  for (size_t i = 0; i < pages; ++i) {
    if (!(virt_page_infos[i] & kIsInRam))
      continue;
    uint64_t phys_page_nr = virt_page_infos[i] & kRamPhysicalPageMask;
    uint64_t phys_page_info;
    off64_t phys_off =
        static_cast<off64_t>(phys_page_nr * sizeof(phys_page_info));
    if (ReadAtOffsetClobberSeekPos(*kpageflags_fd_, &phys_page_info,
                                   sizeof(phys_page_info),
                                   phys_off) != sizeof(phys_page_info)) {
      return -1;
    }
    if (!(phys_page_info & kPhysPageReferenced)) {
      if (i == 0)
        idle_mem += GetFirstPageShare(addr, size);
      else if (i == pages - 1)
        idle_mem += GetLastPageShare(addr, size);
      else
        idle_mem += base::kPageSize;
    }
  }
  return idle_mem;
}

uint64_t GetFirstPageShare(uint64_t addr, size_t size) {
  // Our allocation is xxxx in this illustration:
  //         +----------------------------------------------+
  //         |             xxxxxxxxxx|xxxxxx                |
  //         |             xxxxxxxxxx|xxxxxx                |
  //         |             xxxxxxxxxx|xxxxxx                |
  //         +-------------+---------------+----------------+
  //         ^             ^         ^     ^
  //         +             +         +     +
  // page_aligned_addr  addr        end    addr + size
  uint64_t page_aligned_addr = (addr / base::kPageSize) * base::kPageSize;
  uint64_t end = page_aligned_addr + base::kPageSize;
  if (end > addr + size) {
    // The whole allocation is on the first page.
    return size;
  }

  return base::kPageSize - (addr - page_aligned_addr);
}

uint64_t GetLastPageShare(uint64_t addr, size_t size) {
  uint64_t last_page_size = (addr + size) % base::kPageSize;
  if (last_page_size == 0) {
    // Address ends at a page boundary, the whole last page is idle.
    return base::kPageSize;
  } else {
    // Address does not end at a page boundary, only a subset of the last
    // page should be attributed to this allocation.
    return last_page_size;
  }
}

}  // namespace profiling
}  // namespace perfetto
