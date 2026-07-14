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

import {pinHeapSizeMetricsInstance} from './pinHeapSizeMetricsHandler';
import {pinBitmapMetricsInstance} from './pinBitmapMetricsHandler';
import {pinDirtyMemoryMetricsInstance} from './pinDirtyMemoryMetricsHandler';
import {pinGPUMemoryMetricsInstance} from './pinGPUMemoryMetricsHandler';
import {pinActivityOrBinderLeaksMetricsInstance} from './pinActivityOrBinderLeaksMetricsHandler';
import {pinHardwareBufferMemoryMetricsInstance} from './pinHardwareBufferMemoryMetricsHandler';

describe('SimpleProcessMetricHandler subclasses', () => {
  describe('PinHeapSizeMetricsHandler', () => {
    const tester = pinHeapSizeMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toEqual({process: 'com.android.systemui'});

      expect(
        tester.match(
          'perfetto_java_heap_class_stats-instance_stats-name-com.android.systemui-samples-type_count-type_name-android.graphics.Bitmap-obj_count-p95',
        ),
      ).toEqual({process: 'com.android.systemui'});
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match('perfetto_ft_launcher-missed_sf_frames-mean'),
      ).toBeUndefined();
    });
  });

  describe('PinBitmapMetricsHandler', () => {
    const tester = pinBitmapMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_bitmap_metric_max_val-com.android.systemui',
        ),
      ).toEqual({process: 'com.android.systemui'});

      expect(
        tester.match('perfetto_android_bitmap_metric_max_val-systemui'),
      ).toEqual({process: 'com.android.systemui'});
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toBeUndefined();
    });
  });

  describe('PinDirtyMemoryMetricsHandler', () => {
    const tester = pinDirtyMemoryMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_mem-com.android.systemui-total_counters-anon_and_swap-max-mean',
        ),
      ).toEqual({process: 'com.android.systemui'});
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toBeUndefined();
    });
  });

  describe('PinGPUMemoryMetricsHandler', () => {
    const tester = pinGPUMemoryMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match('perfetto_android_gpu-com.android.systemui-mem_max-max'),
      ).toEqual({process: 'com.android.systemui'});

      expect(
        tester.match(
          'perfetto_android_gpu-/system/bin/surfaceflinger-mem_avg-mean',
        ),
      ).toEqual({process: '/system/bin/surfaceflinger'});

      expect(
        tester.match(
          'perfetto_android_gpu-/vendor/bin/hw/surfaceflinger-mem_avg-mean',
        ),
      ).toEqual({process: '/system/bin/surfaceflinger'});

      expect(
        tester.match(
          'perfetto_android_gpu-/system/bin/otherprocess-mem_avg-mean',
        ),
      ).toEqual({process: '/system/bin/otherprocess'});
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_mem-com.android.systemui-total_counters-java_heap-max-mean',
        ),
      ).toBeUndefined();
    });
  });

  describe('PinActivityOrBinderLeaksMetricsHandler', () => {
    const tester = pinActivityOrBinderLeaksMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match('com.android.systemui_Activities-last-first-diff'),
      ).toEqual({process: 'com.android.systemui'});

      expect(tester.match('systemui_View-last-first-diff')).toEqual({
        process: 'com.android.systemui',
      });
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match('com.android.systemui_Activities-last-first'),
      ).toBeUndefined();
    });
  });

  describe('PinHardwareBufferMemoryMetricsHandler', () => {
    const tester = pinHardwareBufferMemoryMetricsInstance;
    it('parses valid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui-p95',
        ),
      ).toEqual({process: 'com.android.systemui'});

      expect(
        tester.match(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui-mean',
        ),
      ).toEqual({process: 'com.android.systemui'});

      expect(
        tester.match(
          'perfetto_android_dmabuf_per_process_metric_max_val-systemui-p95',
        ),
      ).toEqual({process: 'com.android.systemui'});
    });
    it('returns undefined for invalid metrics', () => {
      expect(
        tester.match(
          'perfetto_android_dmabuf_per_process_metric_max_val-com.android.systemui',
        ),
      ).toBeUndefined();
    });
  });
});
