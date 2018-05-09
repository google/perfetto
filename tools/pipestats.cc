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

// Print timestamps when pages were read to stdout.
// This data is useful to compute rate of events from the kernel.

#include <thread>

#include <fcntl.h>  // splice
#include <inttypes.h>
#include <stdint.h>
#include <unistd.h>  // pipe

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/time.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace {

void SetBlocking(int fd, bool is_blocking) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags = (is_blocking) ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  PERFETTO_CHECK(fcntl(fd, F_SETFL, flags) == 0);
}

__attribute__((__noreturn__)) void ReadLoop(int fd) {
  char buf[4096];
  while (true) {
    base::ignore_result(read(fd, &buf, sizeof(buf)));
  }
}

int PipestatsMain(int argc, char** argv) {
  PERFETTO_CHECK(argc == 2);
  base::ScopedFile trace_fd(open(argv[1], O_RDONLY));
  PERFETTO_CHECK(trace_fd);
  std::thread reader(ReadLoop, trace_fd.get());

  int pipe_fds[2];
  PERFETTO_CHECK(pipe(&pipe_fds[0]) == 0);
  base::ScopedFile staging_read_fd(pipe_fds[0]);
  base::ScopedFile staging_write_fd(pipe_fds[1]);

  // Make reads from the raw pipe blocking so that splice() can sleep.
  SetBlocking(*trace_fd, true);

  // Reads from the staging pipe are always non-blocking.
  SetBlocking(*staging_read_fd, false);

  // Note: O_NONBLOCK seems to be ignored by splice() on the target pipe. The
  // blocking vs non-blocking behavior is controlled solely by the
  // SPLICE_F_NONBLOCK flag passed to splice().
  SetBlocking(*staging_write_fd, false);

  while (true) {
    ssize_t splice_res = splice(*trace_fd, nullptr, *staging_write_fd, nullptr,
                                base::kPageSize, SPLICE_F_MOVE);
    if (splice_res > 0) {
      auto cur = base::GetWallTimeNs();
      printf("%" PRId64 "\n", int64_t(cur.count()));
    }
  }
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::PipestatsMain(argc, argv);
}
