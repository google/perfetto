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
import {renderTrackDimensions} from './track_dimensions';

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

describe('renderTrackDimensions', () => {
  test('renders nothing without dimensions', () => {
    expect(renderTrackDimensions([])).toBeUndefined();
  });

  test('renders values with their display names', () => {
    m.render(
      container,
      renderTrackDimensions([
        {name: 'process', value: '100', displayName: 'proc'},
        {name: 'rank', value: '7', displayName: ''},
        {name: 'max', value: '9223372036854775807', displayName: 'largest'},
        {name: 'empty', value: ''},
      ]),
    );
    const cells = (selector: string) =>
      Array.from(container.querySelectorAll(selector), (e) => e.textContent);
    expect(container.textContent).toContain('Dimensions');
    expect(cells('.pf-tree-left')).toEqual(['process', 'rank', 'max', 'empty']);
    expect(cells('.pf-tree-right')).toEqual([
      '100 (proc)',
      '7',
      '9223372036854775807 (largest)',
      '',
    ]);
  });
});
