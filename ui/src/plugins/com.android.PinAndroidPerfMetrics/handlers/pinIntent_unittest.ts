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

import {parsePinIntents, PinIntentKind} from './pinIntent';

describe('parsePinIntents', () => {
  describe('Dictionary inputs', () => {
    it('parses wildcard CUJ dictionary "{ cuj: \'*\' }"', () => {
      expect(parsePinIntents({cuj: '*'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });

    it('parses single CUJ dictionary record', () => {
      expect(parsePinIntents({cuj: 'RECENTS_SCROLLING'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: 'RECENTS_SCROLLING'},
      ]);
    });

    it('parses allJankCujs and allLatencyCujs flags', () => {
      expect(parsePinIntents({allJankCujs: 'true'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
      expect(parsePinIntents({allLatencyCujs: 'true'})).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });

    it('parses missed frames during CUJ', () => {
      const input = {
        process: 'systemui',
        cuj: 'RECENTS_SCROLLING',
        jankType: 'sf_frames',
        isWeighted: 'true',
      };
      expect(parsePinIntents(input)).toEqual([
        {
          kind: PinIntentKind.CujScopedJank,
          process: 'com.android.systemui',
          cujName: 'RECENTS_SCROLLING',
          jankType: 'sf_frames',
          isWeighted: true,
        },
      ]);
    });

    it('parses blocking calls', () => {
      const cujBlocking = {
        process: 'com.android.systemui',
        cuj: 'SHADE_EXPAND',
        blockingCall: 'input',
        aggregation: 'mean_dur_per_frame_ns-max',
      };
      expect(parsePinIntents(cujBlocking)).toEqual([
        {
          kind: PinIntentKind.CujBlockingCall,
          process: 'com.android.systemui',
          cujName: 'SHADE_EXPAND',
          blockingCallName: 'input',
          aggregation: 'mean_dur_per_frame_ns-max',
        },
      ]);
    });

    it('parses full trace jank dictionaries', () => {
      expect(
        parsePinIntents({
          process: 'systemui',
          fullTrace: 'true',
          jankType: 'app_frames',
        }),
      ).toEqual([
        {
          kind: PinIntentKind.FullTraceJank,
          process: 'com.android.systemui',
          jankType: 'app_frames',
          isWeighted: false,
        },
      ]);
    });
  });

  describe('Deduplication', () => {
    it('deduplicates identical requests based on typed intent key', () => {
      const input = [{allJankCujs: 'true'}, {cuj: '*'}, {cuj: '*'}];
      expect(parsePinIntents(input)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: '*'},
      ]);
    });

    it('deduplicates identical cuj scoped jank requests', () => {
      const item = {
        process: 'systemui',
        cuj: 'SHADE_EXPAND',
        jankType: 'sf_frames',
        isWeighted: 'true',
      };
      expect(parsePinIntents([item, item])).toEqual([
        {
          kind: PinIntentKind.CujScopedJank,
          process: 'com.android.systemui',
          cujName: 'SHADE_EXPAND',
          jankType: 'sf_frames',
          isWeighted: true,
        },
      ]);
    });

    it('preserves distinct requests', () => {
      const input = [{cuj: 'CUJ_A'}, {cuj: 'CUJ_B'}];
      expect(parsePinIntents(input)).toEqual([
        {kind: PinIntentKind.Cuj, cujName: 'CUJ_A'},
        {kind: PinIntentKind.Cuj, cujName: 'CUJ_B'},
      ]);
    });
  });

  describe('Robustness and error handling', () => {
    it('handles undefined, null, empty arrays, and empty objects gracefully', () => {
      expect(parsePinIntents(undefined)).toEqual([]);
      expect(parsePinIntents(null)).toEqual([]);
      expect(parsePinIntents([])).toEqual([]);
      expect(parsePinIntents({})).toEqual([]);
      expect(parsePinIntents([{}])).toEqual([]);
    });
  });
});
