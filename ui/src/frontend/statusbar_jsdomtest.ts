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
import {createFakeTraceImpl} from '../core/fake_trace_impl';
import {setOwner} from '../core/ownership';
import {renderStatusBar} from './statusbar';

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

describe('renderStatusBar', () => {
  test('labels each item with its owner', () => {
    const trace = createFakeTraceImpl();
    const owned = {renderItem: () => ({label: 'Owned'})};
    setOwner(owned, 'com.example.Owner');
    trace.statusbar.registerItem(owned);
    trace.statusbar.registerItem({renderItem: () => ({label: 'Unowned'})});

    m.render(container, renderStatusBar(trace.app));

    const ownerOf = (label: string) =>
      Array.from(container.querySelectorAll('button'))
        .find((el) => el.textContent?.includes(label))
        // The item's wrapper is the button's parent.
        ?.parentElement?.getAttribute('data-plugin');
    expect(ownerOf('Owned')).toBe('com.example.Owner');
    expect(ownerOf('Unowned')).toBeNull();
  });
});
