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

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_WATCHDOG)

#include "perfetto/ext/base/watchdog_posix.h"

#include <stdio.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(WatchdogPosixTest, ParseProcStat) {
  constexpr const char stat[] =
      "2965981 (zsh) S 2965977 2965981 2965981 34822 2966607 4194304 6632 6697 "
      "0 0 11 6 4 1 20 0 1 0 227163466 15839232 2311 18446744073709551615 "
      "94823961161728 94823961762781 140722993535472 0 0 0 2 3686400 134295555 "
      "0 0 0 17 2 0 0 0 0 0 94823961905904 94823961935208 94823993954304 "
      "140722993543678 140722993543691 140722993543691 140722993545195 0";
  TempFile f = TempFile::CreateUnlinked();
  WriteAll(f.fd(), stat, sizeof(stat));
  ASSERT_NE(lseek(f.fd(), 0, SEEK_SET), -1);
  ProcStat ps;
  ASSERT_TRUE(ReadProcStat(f.fd(), &ps));
  EXPECT_EQ(ps.utime, 11u);
  EXPECT_EQ(ps.stime, 6u);
  EXPECT_EQ(ps.rss_pages, 2311);
}

}  // namespace
}  // namespace base
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_WATCHDOG)
