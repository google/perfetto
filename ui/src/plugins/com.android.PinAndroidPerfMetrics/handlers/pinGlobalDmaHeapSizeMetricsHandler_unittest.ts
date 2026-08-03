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

import {vi} from 'vitest';
import type {Trace} from '../../../public/trace';
import {PinRequestType} from './pinRequest';
import {
  execGlobalDmaHeap,
  translateGlobalDmaHeap,
} from './pinGlobalDmaHeapSizeMetricsHandler';

describe('translateGlobalDmaHeap', () => {
  it('parses valid metrics', () => {
    expect(
      translateGlobalDmaHeap('perfetto_android_dma_heap-avg_size_bytes-p95'),
    ).toEqual([{type: PinRequestType.GlobalDmaHeap}]);
    expect(
      translateGlobalDmaHeap('perfetto_android_dma_heap-max_size_bytes-mean'),
    ).toEqual([{type: PinRequestType.GlobalDmaHeap}]);
    expect(
      translateGlobalDmaHeap(
        'perfetto_android_dma_heap-total_alloc_size_bytes-anything',
      ),
    ).toEqual([{type: PinRequestType.GlobalDmaHeap}]);
  });
  it('returns empty array for invalid metrics', () => {
    expect(
      translateGlobalDmaHeap('perfetto_android_dma_heap-avg_size_bytes'),
    ).toEqual([]);
    expect(
      translateGlobalDmaHeap(
        'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
      ),
    ).toEqual([]);
  });
});

describe('execGlobalDmaHeap', () => {
  it('pins mem.dma_heap and mem.dma_buffer tracks', async () => {
    const createMockTrack = (
      uri: string,
      name: string,
      parent?: {name: string},
    ) => ({
      uri,
      name,
      parent,
      pin: vi.fn(),
    });

    const dmaHeapTrack = createMockTrack('uri1', 'mem.dma_heap'); // Valid heap
    const dmaBufferTrack = createMockTrack(
      'uri2',
      'mem.dma_buffer',
      dmaHeapTrack,
    ); // Valid buffer
    const invalidBufferTrack = createMockTrack('uri3', 'mem.dma_buffer', {
      name: 'other',
    }); // Invalid buffer (wrong parent)
    const otherTrack = createMockTrack('uri4', 'other_track'); // Invalid track

    const mockTrace = {
      currentWorkspace: {
        flatTracks: [
          dmaHeapTrack,
          dmaBufferTrack,
          invalidBufferTrack,
          otherTrack,
        ],
      },
    } as unknown as Trace;

    await execGlobalDmaHeap(mockTrace, {type: PinRequestType.GlobalDmaHeap});

    expect(dmaHeapTrack.pin).toHaveBeenCalledTimes(1);
    expect(dmaBufferTrack.pin).toHaveBeenCalledTimes(1);
    expect(invalidBufferTrack.pin).not.toHaveBeenCalled();
    expect(otherTrack.pin).not.toHaveBeenCalled();
  });
});
