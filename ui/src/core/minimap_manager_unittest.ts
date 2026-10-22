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

import {describe, expect, test} from 'vitest';
import {Duration, Time} from '../base/time';
import type {MinimapMode} from '../public/minimap';
import {
  ACTIVITY_MODE,
  DEFAULT_MINIMAP_HEIGHT_PX,
  MinimapManagerImpl,
} from './minimap_manager';

function makeMode(
  id: string,
  priority: number,
  extra: Partial<MinimapMode> = {},
): MinimapMode {
  return {id, name: id, priority, render: () => {}, ...extra};
}

describe('MinimapManagerImpl', () => {
  test('defaults to the activity overview', () => {
    const manager = new MinimapManagerImpl();
    expect(manager.modes.map((m) => m.id)).toEqual([ACTIVITY_MODE.id]);
    expect(manager.activeModeId).toBe(ACTIVITY_MODE.id);
    expect(manager.activeMode).toBeUndefined();
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);
  });

  test('loads rows from the highest priority content provider', async () => {
    const manager = new MinimapManagerImpl();
    const row = (load: number) => [
      {ts: Time.fromRaw(100n), dur: Duration.fromRaw(50n), load},
    ];
    manager.registerContentProvider({
      priority: 1,
      getData: async () => [row(1)],
    });
    manager.registerContentProvider({
      priority: 2,
      getData: async () => [row(2)],
    });

    await manager.load(Time.fromRaw(0n), Time.fromRaw(1000n));

    expect(manager.getLoad()).toEqual([row(2)]);
  });

  test('selects the highest priority mode by default', () => {
    const manager = new MinimapManagerImpl();
    const a = makeMode('a', 1, {heightPx: 90});
    const b = makeMode('b', 1);
    manager.registerMode(a);
    manager.registerMode(b);
    manager.registerMode(makeMode('low', -1));

    expect(manager.modes.map((m) => m.id)).toEqual([
      'a',
      'b',
      'activity',
      'low',
    ]);
    expect(manager.activeMode).toBe(a);
    expect(manager.heightPx).toBe(90);
  });

  test('switches modes', () => {
    const manager = new MinimapManagerImpl();
    const video = makeMode('video', 1, {heightPx: 90});
    manager.registerMode(video);

    manager.activeModeId = ACTIVITY_MODE.id;
    expect(manager.activeMode).toBeUndefined();
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);

    manager.activeModeId = 'video';
    expect(manager.activeMode).toBe(video);
    expect(manager.heightPx).toBe(90);

    // Unknown ids fall back to the default mode.
    manager.activeModeId = 'unknown';
    expect(manager.activeMode).toBe(video);
  });

  test('clamps and resets user height', () => {
    const manager = new MinimapManagerImpl();
    manager.registerMode(makeMode('video', 1, {heightPx: 90}));

    manager.heightPx = 150;
    expect(manager.heightPx).toBe(150);

    // The user height is kept when switching modes.
    manager.activeModeId = ACTIVITY_MODE.id;
    expect(manager.heightPx).toBe(150);

    manager.heightPx = 1;
    expect(manager.heightPx).toBe(40);
    manager.heightPx = 10000;
    expect(manager.heightPx).toBe(400);

    manager.resetHeight();
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);
  });
});
