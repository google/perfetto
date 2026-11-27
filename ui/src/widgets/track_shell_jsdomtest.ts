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

import m from 'mithril';
import {afterEach, beforeEach, describe, expect, test} from 'vitest';
import {TrackShell} from './track_shell';

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

describe('TrackShell', () => {
  test('labels the header, not its children, with the owner', () => {
    m.render(
      container,
      m(
        TrackShell,
        {
          title: 'Parent',
          collapsed: false,
          heightPx: 20,
          pluginId: 'com.example.Parent',
        },
        m('.child-track'),
      ),
    );

    const header = container.querySelector('.pf-track__header');
    expect(header?.getAttribute('data-plugin')).toBe('com.example.Parent');
    // Children may belong to other plugins, so they must not inherit the
    // parent's label.
    expect(
      container.querySelector('.child-track')?.closest('[data-plugin]'),
    ).toBeNull();
  });
});
