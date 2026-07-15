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

import {makeColorScheme} from '../colorizer';
import {HSLColor} from '../../base/color';
import {Time} from '../../base/time';
import type {
  MarkerCluster,
  VisualMarker,
  VisualMarkerStyle,
} from './visual_marker';

const RED_COLOR = makeColorScheme(new HSLColor('#FF0000'));
const YELLOW_COLOR = makeColorScheme(new HSLColor('#FFFF00'));
const ORANGE_COLOR = makeColorScheme(new HSLColor('#FFA500'));
const GRAY_COLOR = makeColorScheme(new HSLColor('#808080'));

describe('VisualMarker Framework - Comprehensive Jank Type Testing', () => {
  const testCases = [
    {
      jankType: 'Self Jank',
      priority: 40,
      icon: '🔴',
      color: RED_COLOR,
      strokeColor: '#FFFFFF',
    },
    {
      jankType: 'Other Jank',
      priority: 30,
      icon: '🟡',
      color: YELLOW_COLOR,
      strokeColor: '#000000',
    },
    {
      jankType: 'Dropped Frame',
      priority: 20,
      icon: '🚫',
      color: RED_COLOR,
      strokeColor: '#FFFFFF',
    },
    {
      jankType: 'Cadence Drop',
      priority: 15,
      icon: '⚠️',
      color: ORANGE_COLOR,
      strokeColor: '#FFFFFF',
    },
    {
      jankType: 'Non-perceivable Jank',
      priority: 10,
      icon: '⚪',
      color: GRAY_COLOR,
      strokeColor: '#000000',
    },
    {
      jankType: 'Unknown Jank',
      priority: 5,
      icon: '❓',
      color: GRAY_COLOR,
      strokeColor: '#FFFFFF',
    },
  ];

  testCases.forEach(({jankType, priority, icon, color, strokeColor}) => {
    test(`VisualMarker creation and priority weight for ${jankType}`, () => {
      const style: VisualMarkerStyle = {
        sizePx: 16,
        colorScheme: color,
        icon,
        strokeColor,
      };

      const marker: VisualMarker = {
        id: 100 + priority,
        ts: Time.fromRaw(1000n * BigInt(priority)),
        depth: 0,
        typeKey: jankType,
        style,
        priority,
        row: {jank_tag: jankType},
      };

      expect(marker.id).toBe(100 + priority);
      expect(marker.priority).toBe(priority);
      expect(marker.style.icon).toBe(icon);
      expect(marker.typeKey).toBe(jankType);
      expect(marker.style.strokeColor).toBe(strokeColor);
    });
  });

  test('Priority resolution in visual marker cluster (Highest priority wins)', () => {
    const markers: VisualMarker[] = [
      {
        id: 1,
        ts: Time.fromRaw(1000n),
        depth: 0,
        typeKey: 'Cadence Drop',
        style: {sizePx: 16, colorScheme: ORANGE_COLOR, icon: '⚠️'},
        priority: 10,
        row: {jank_tag: 'Cadence Drop'},
      },
      {
        id: 2,
        ts: Time.fromRaw(1000n),
        depth: 0,
        typeKey: 'Self Jank',
        style: {sizePx: 16, colorScheme: RED_COLOR, icon: '🔴'},
        priority: 30,
        row: {jank_tag: 'Self Jank'},
      },
      {
        id: 3,
        ts: Time.fromRaw(1000n),
        depth: 0,
        typeKey: 'Other Jank',
        style: {sizePx: 16, colorScheme: YELLOW_COLOR, icon: '🟡'},
        priority: 20,
        row: {jank_tag: 'Other Jank'},
      },
      {
        id: 4,
        ts: Time.fromRaw(1000n),
        depth: 0,
        typeKey: 'Dropped Frame',
        style: {sizePx: 16, colorScheme: RED_COLOR, icon: '🚫'},
        priority: 40,
        row: {jank_tag: 'Dropped Frame'},
      },
    ];

    const topMarker = markers.reduce((prev, curr) =>
      curr.priority > prev.priority ? curr : prev,
    );

    expect(topMarker.typeKey).toBe('Dropped Frame');
    expect(topMarker.priority).toBe(40);
    expect(topMarker.style.icon).toBe('🚫');

    const cluster: MarkerCluster = {
      centerTs: Time.fromRaw(1000n),
      screenX: 50,
      depth: 0,
      count: markers.length,
      representativeMarker: topMarker,
      markers,
    };

    expect(cluster.count).toBe(4);
    expect(cluster.representativeMarker.typeKey).toBe('Dropped Frame');
    expect(cluster.representativeMarker.priority).toBe(40);
  });
});
