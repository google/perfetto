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

import {translateHeapSize} from './pinHeapSizeMetricsHandler';
import {translateBitmap} from './pinBitmapMetricsHandler';
import {translateDirtyMemory} from './pinDirtyMemoryMetricsHandler';
import {translateGpuMemory} from './pinGPUMemoryMetricsHandler';
import {translateActivityOrBinderLeaks} from './pinActivityOrBinderLeaksMetricsHandler';
import {translateHardwareBufferMemory} from './pinHardwareBufferMemoryMetricsHandler';

describe('SimpleProcessMetric translators', () => {
  describe('translateHeapSize', () => {
    it('parses valid metrics', () => {
      expect(
        translateHeapSize(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateHeapSize(
          'perfetto_java_heap_class_stats-instance_stats-name-com.android.systemui-samples-type_count-type_name-android.graphics.Bitmap-obj_count-p95',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateHeapSize('perfetto_ft_launcher-missed_sf_frames-mean'),
      ).toEqual([]);
    });
  });

  describe('translateBitmap', () => {
    it('parses valid metrics', () => {
      expect(
        translateBitmap(
          'perfetto_android_bitmap_metric_max_val-com.android.systemui',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateBitmap('perfetto_android_bitmap_metric_max_val-systemui'),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateBitmap(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toEqual([]);
    });
  });

  describe('translateDirtyMemory', () => {
    it('parses valid metrics', () => {
      expect(
        translateDirtyMemory(
          'perfetto_android_mem-com.android.systemui-total_counters-anon_and_swap-max-mean',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateDirtyMemory(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toEqual([]);
    });
  });

  describe('translateGpuMemory', () => {
    it('parses valid metrics', () => {
      expect(
        translateGpuMemory(
          'perfetto_android_gpu-com.android.systemui-mem_max-max',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateGpuMemory(
          'perfetto_android_gpu-/system/bin/surfaceflinger-mem_avg-mean',
        ),
      ).toEqual([
        expect.objectContaining({process: '/system/bin/surfaceflinger'}),
      ]);

      expect(
        translateGpuMemory(
          'perfetto_android_gpu-/vendor/bin/hw/surfaceflinger-mem_avg-mean',
        ),
      ).toEqual([
        expect.objectContaining({process: '/system/bin/surfaceflinger'}),
      ]);

      expect(
        translateGpuMemory(
          'perfetto_android_gpu-/system/bin/otherprocess-mem_avg-mean',
        ),
      ).toEqual([
        expect.objectContaining({process: '/system/bin/otherprocess'}),
      ]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateGpuMemory(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toEqual([]);
    });
  });

  describe('translateActivityOrBinderLeaks', () => {
    it('parses valid metrics', () => {
      expect(
        translateActivityOrBinderLeaks(
          'com.android.systemui_Activities-last-first-diff',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateActivityOrBinderLeaks('systemui_View-last-first-diff'),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateActivityOrBinderLeaks(
          'com.android.systemui_Activities-last-first',
        ),
      ).toEqual([]);
    });
  });

  describe('translateHardwareBufferMemory', () => {
    it('parses valid metrics', () => {
      expect(
        translateHardwareBufferMemory(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui-p95',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateHardwareBufferMemory(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui-mean',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);

      expect(
        translateHardwareBufferMemory(
          'perfetto_android_dmabuf_per_process_metric_max_val-systemui-p95',
        ),
      ).toEqual([expect.objectContaining({process: 'com.android.systemui'})]);
    });
    it('returns empty array for invalid metrics', () => {
      expect(
        translateHardwareBufferMemory(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui',
        ),
      ).toEqual([]);
    });
  });
});
