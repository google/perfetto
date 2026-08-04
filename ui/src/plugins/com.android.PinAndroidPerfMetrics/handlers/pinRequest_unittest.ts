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

import {PinIntentKind, ProcessMemoryType} from './metricUtils';
import {
  normalizePinIntent,
  parsePinRequests,
  type PinRequestsCommandArg,
} from './pinRequest';

describe('normalizePinIntent & parsePinRequests', () => {
  describe('Shorthand strings & wildcard expansion', () => {
    it('parses shorthand string literals for parameterless tracks', () => {
      expect(parsePinRequests('allJankCujs')).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
      expect(normalizePinIntent('allLatencyCujs')).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
      expect(normalizePinIntent('globalDmaHeap')).toEqual([
        {kind: PinIntentKind.GlobalDmaHeap},
      ]);
    });

    it('expands wildcard "*" string into wildcard cuj intent', () => {
      expect(normalizePinIntent('*')).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });
  });

  describe('Benchmark metric key regex matching', () => {
    it('matches raw CUJ counter metric strings to cuj and cuj_scoped_jank intents', () => {
      const metric =
        'perfetto_cuj_com.android.systemui-RECENTS_SCROLLING-counter_metrics-missed_app_frames-mean';
      expect(normalizePinIntent(metric)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: 'RECENTS_SCROLLING'},
        {
          kind: PinIntentKind.CujScopedJank,
          cujName: 'RECENTS_SCROLLING',
          process: 'com.android.systemui',
          jankType: 'app_frames',
          isWeighted: false,
        },
      ]);
    });

    it('matches raw full-trace metric strings', () => {
      const metric = 'perfetto_ft_com.android.systemui-missed_sf_frames-mean';
      expect(normalizePinIntent(metric)).toEqual([
        {
          kind: PinIntentKind.FullTraceJank,
          process: 'com.android.systemui',
          jankType: 'sf_frames',
          isWeighted: false,
        },
      ]);
    });

    it('matches blocking call and notification blocking call strings', () => {
      const bcMetric =
        'perfetto_android_blocking_call-cuj-name-com.android.systemui-name-SHADE_EXPAND-blocking_calls-name-input-mean_dur_per_frame_ns-max';
      expect(normalizePinIntent(bcMetric)).toEqual([
        {
          kind: PinIntentKind.CujBlockingCall,
          process: 'com.android.systemui',
          cujName: 'SHADE_EXPAND',
          blockingCallName: 'input',
          aggregation: 'mean_dur_per_frame_ns-max',
        },
      ]);

      const notifMetric =
        'perfetto_android_notifications_blocking_call-blocking_calls-name-NotificationStackScrollLayout-cnt';
      expect(normalizePinIntent(notifMetric)).toEqual([
        {
          kind: PinIntentKind.NotificationBlockingCall,
          notificationName: 'NotificationStackScrollLayout',
          aggregation: 'cnt',
        },
      ]);
    });

    it('matches process memory metric keys', () => {
      const memMetric =
        'perfetto_android_mem-com.android.systemui-total_counters-java_heap-mean';
      expect(normalizePinIntent(memMetric)).toEqual([
        {
          kind: PinIntentKind.ProcessMemory,
          process: 'com.android.systemui',
          memoryType: ProcessMemoryType.HeapSize,
        },
      ]);
    });
  });

  describe('Parameterless & wildcard dictionary records', () => {
    it('parses wildcard CUJ dictionary "{ cuj: \'*\' }" into wildcard cuj intent', () => {
      expect(normalizePinIntent({cuj: '*'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });

    it('parses boolean flag dictionaries for global features', () => {
      expect(normalizePinIntent({globalDmaHeap: 'true'})).toEqual([
        {kind: PinIntentKind.GlobalDmaHeap},
      ]);
      expect(normalizePinIntent({allJankCujs: 'true'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
      expect(normalizePinIntent({allLatencyCujs: 'false'})).toEqual([]);
    });
  });

  describe('Specific dictionary records & field alias normalization', () => {
    it('parses single CUJ dictionary record', () => {
      expect(normalizePinIntent({cuj: 'RECENTS_SCROLLING'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: 'RECENTS_SCROLLING'},
        {
          kind: PinIntentKind.CujScopedJank,
          process: 'com.android.systemui',
          cujName: 'RECENTS_SCROLLING',
          jankType: 'frames',
          isWeighted: false,
        },
      ]);
    });

    it('parses missed frames during CUJ with alias normalization', () => {
      const input = {
        pkg: 'com.android.systemui',
        CUJ: 'RECENTS_SCROLLING',
        frameType: 'sf_frames',
        weighted: 'true',
      };
      expect(normalizePinIntent(input)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: 'RECENTS_SCROLLING'},
        {
          kind: PinIntentKind.CujScopedJank,
          process: 'com.android.systemui',
          cujName: 'RECENTS_SCROLLING',
          jankType: 'sf_frames',
          isWeighted: true,
        },
      ]);
    });

    it('parses blocking calls and notification blocking calls', () => {
      const cujBlocking = {
        cuj: 'SHADE_EXPAND',
        blockingCall: 'input',
        agg: 'mean_dur_per_frame_ns-max',
      };
      expect(normalizePinIntent(cujBlocking)).toEqual([
        {
          kind: PinIntentKind.CujBlockingCall,
          process: 'com.android.systemui',
          cujName: 'SHADE_EXPAND',
          blockingCallName: 'input',
          aggregation: 'mean_dur_per_frame_ns-max',
        },
      ]);

      const notifBlocking = {
        notification: 'NotificationStackScrollLayout',
        agg: 'cnt',
      };
      expect(normalizePinIntent(notifBlocking)).toEqual([
        {
          kind: PinIntentKind.NotificationBlockingCall,
          notificationName: 'NotificationStackScrollLayout',
          aggregation: 'cnt',
        },
      ]);
    });

    it('parses process memory track requests', () => {
      expect(
        normalizePinIntent({
          process: 'com.android.systemui',
          memory: 'heapSize',
        }),
      ).toEqual([
        {
          kind: PinIntentKind.ProcessMemory,
          process: 'com.android.systemui',
          memoryType: ProcessMemoryType.HeapSize,
        },
      ]);
    });
  });

  describe('Priority & Mutual exclusion enforcement', () => {
    it('prioritizes cuj_blocking_call when blockingCall is present with cuj', () => {
      const input = {cuj: 'RECENTS_SCROLLING', blockingCall: 'input'};
      expect(normalizePinIntent(input)).toEqual([
        {
          kind: PinIntentKind.CujBlockingCall,
          process: 'com.android.systemui',
          cujName: 'RECENTS_SCROLLING',
          blockingCallName: 'input',
          aggregation: 'mean_dur_per_frame_ns-max',
        },
      ]);
    });
  });

  describe('Deduplication across mixed & redundant inputs', () => {
    it('deduplicates identical requests from mixed string and dictionary sources', () => {
      const input = ['allJankCujs', {allJankCujs: 'true'}, 'all_jank_cujs'];
      expect(normalizePinIntent(input)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });

    it('preserves distinct parameterless handlers', () => {
      const input = ['allJankCujs', 'globalDmaHeap'];
      expect(normalizePinIntent(input)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
        {kind: PinIntentKind.GlobalDmaHeap},
      ]);
    });
  });

  describe('Malformed & unrecognized inputs', () => {
    it('returns empty array for null, undefined, primitives, or empty objects', () => {
      expect(
        normalizePinIntent(null as unknown as PinRequestsCommandArg),
      ).toEqual([]);
      expect(normalizePinIntent(undefined)).toEqual([]);
      expect(
        normalizePinIntent(12345 as unknown as PinRequestsCommandArg),
      ).toEqual([]);
      expect(normalizePinIntent({})).toEqual([]);
    });

    it('returns empty array for unrecognized filter dictionaries or invalid metric strings', () => {
      expect(normalizePinIntent({unknownField: 'someValue'})).toEqual([]);
      expect(normalizePinIntent('perfetto_unknown_metric_format-mean')).toEqual(
        [],
      );
    });
  });
});
