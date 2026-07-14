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

import type {Trace} from '../../../public/trace';
import type {
  GlobalDmaHeapMetricData,
  MetricData,
  MetricHandler,
} from './metricUtils';

export class PinDMAHeapSizeMetricsHandler implements MetricHandler {
  private readonly matcher = /perfetto_android_dma_heap-(.*)_size_bytes-.*/;

  public match(metricKey: string): GlobalDmaHeapMetricData | undefined {
    if (this.matcher.test(metricKey)) {
      return {};
    }
    return undefined;
  }

  public addMetricTrack(_metricData: MetricData, ctx: Trace) {
    const dmaHeapTracks = ctx.currentWorkspace.flatTracks.filter(
      (t) => t.uri !== undefined && t.name === 'mem.dma_heap',
    );

    const dmaBufferTracks = ctx.currentWorkspace.flatTracks.filter(
      (t) =>
        t.uri !== undefined &&
        // We have many 'mem.dma_buffer' tracks, we only interested in global one
        t.name === 'mem.dma_buffer' &&
        t.parent?.name === 'mem.dma_heap',
    );

    dmaHeapTracks.forEach((t) => t.pin());
    dmaBufferTracks.forEach((t) => t.pin());
  }
}

export const pinDMAHeapSizeMetricsInstance = new PinDMAHeapSizeMetricsHandler();
