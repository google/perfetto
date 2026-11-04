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
import type {MinimapMode} from '../public/minimap';
import {
  DEFAULT_MINIMAP_HEIGHT_PX,
  MAX_MINIMAP_HEIGHT_PX,
  MIN_MINIMAP_HEIGHT_PX,
  MinimapManagerImpl,
} from './minimap_manager';

function makeMode(
  id: string,
  priority: number,
  heightPx?: number,
): MinimapMode {
  return {id, name: id, priority, heightPx, render: () => {}};
}

describe('MinimapManagerImpl', () => {
  test('has no active mode until one is registered', () => {
    const manager = new MinimapManagerImpl();
    expect(manager.activeMode).toBeUndefined();
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);
  });

  test('activates the highest priority mode by default', () => {
    const manager = new MinimapManagerImpl();
    const a = makeMode('a', 1, 90);
    manager.registerMode(makeMode('activity', 0));
    manager.registerMode(a);
    manager.registerMode(makeMode('b', 1));

    expect(manager.modes.map((m) => m.id)).toEqual(['a', 'b', 'activity']);
    expect(manager.activeMode).toBe(a);
    expect(manager.heightPx).toBe(90);
  });

  test('rejects duplicate mode ids', () => {
    const manager = new MinimapManagerImpl();
    manager.registerMode(makeMode('a', 0));
    expect(() => manager.registerMode(makeMode('a', 1))).toThrow();
  });

  test('switches modes', () => {
    const manager = new MinimapManagerImpl();
    const activity = makeMode('activity', 0, DEFAULT_MINIMAP_HEIGHT_PX);
    const video = makeMode('video', 1, 90);
    manager.registerMode(activity);
    manager.registerMode(video);

    manager.selectMode('activity');
    expect(manager.activeMode).toBe(activity);
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);

    // Unknown ids fall back to the default mode.
    manager.selectMode('unknown');
    expect(manager.activeMode).toBe(video);
  });

  test('clamps and resets user height', () => {
    const manager = new MinimapManagerImpl();
    manager.registerMode(makeMode('activity', 0, DEFAULT_MINIMAP_HEIGHT_PX));
    manager.registerMode(makeMode('video', 1, 90));

    manager.heightPx = 150;
    expect(manager.heightPx).toBe(150);

    // The user height is kept when switching modes.
    manager.selectMode('activity');
    expect(manager.heightPx).toBe(150);

    manager.heightPx = 1;
    expect(manager.heightPx).toBe(MIN_MINIMAP_HEIGHT_PX);
    manager.heightPx = 10000;
    expect(manager.heightPx).toBe(MAX_MINIMAP_HEIGHT_PX);

    manager.resetHeight();
    expect(manager.heightPx).toBe(DEFAULT_MINIMAP_HEIGHT_PX);
  });
});
