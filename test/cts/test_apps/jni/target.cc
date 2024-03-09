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

#include <jni.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fstream>
#include <limits>

namespace {

// Must be kept in sync with heapprofd_test_cts.cc
constexpr int kIndividualAllocSz = 4153;
constexpr int kAllocationIntervalUs = 10 * 1000;

// Increments a value in the text file `path`. The file is read by the CTS test
// to observe the app progress.
void ReportCycle(const char* path) {
  int64_t value = 0;
  {
    // Read the previous value from the file (it might be from a separate
    // execution of this app).
    std::ifstream ifs(path);
    if (ifs) {
      ifs >> value;
    }
  }

  std::string tmppath = std::string(path) + std::string(".tmp");
  std::ofstream ofs(tmppath, std::ios::trunc);
  if (value == std::numeric_limits<int64_t>::max()) {
    value = std::numeric_limits<int64_t>::min();
  } else {
    value++;
  }
  ofs << value;
  ofs.close();
  if (!ofs) {
    abort();
  }
  rename(tmppath.c_str(), path);
}

__attribute__((noreturn)) void perfetto_test_allocations(
    const char* report_cycle_path) {
  for (;;) {
    for (size_t j = 0; j < 20; j++) {
      // volatile & use avoids builtin malloc optimizations
      volatile char* x = static_cast<char*>(malloc(kIndividualAllocSz));
      if (x) {
        x[0] = '\0';
        free(const_cast<char*>(x));
      }
      usleep(kAllocationIntervalUs);
    }
    ReportCycle(report_cycle_path);
  }
}

// Runs continuously as a target for the sampling perf profiler tests.
__attribute__((noreturn)) void perfetto_busy_wait() {
  for (volatile unsigned i = 0;; i++) {
  }
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_android_perfetto_cts_app_MainActivity_runNative(
    JNIEnv* env,
    jclass,
    jstring jreport_cycle_path) {
  const char* path = env->GetStringUTFChars(jreport_cycle_path, NULL);
  perfetto_test_allocations(path);
  env->ReleaseStringUTFChars(jreport_cycle_path, NULL);
}

extern "C" JNIEXPORT void JNICALL
Java_android_perfetto_cts_app_BusyWaitActivity_runNativeBusyWait(JNIEnv*,
                                                                 jclass) {
  perfetto_busy_wait();
}
