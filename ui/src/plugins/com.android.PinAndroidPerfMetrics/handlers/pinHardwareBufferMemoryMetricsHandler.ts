// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {SimpleProcessMetricHandler} from './simpleProcessMetricHandler';

export const pinHardwareBufferMemoryMetricsInstance =
  new SimpleProcessMetricHandler(
    [
      /perfetto_android_dmabuf_per_process_metric_max_val-(?<processName>.*)-p95/,
      /perfetto_android_gralloc_buffers_per_process_metric_max_val-(?<processName>.*)-p95/,
    ],
    ['dmabuf allocs', 'mem.gralloc.allocations', 'mem.gralloc.buffers'],
  );
