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
import {withOwner} from './owner_attribute';

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

describe('withOwner', () => {
  test('wraps children in an element that names their owner', () => {
    m.render(container, withOwner('com.example.A', m('.child')));
    const wrapper = container.querySelector('.child')?.parentElement;
    expect(wrapper?.getAttribute('data-plugin')).toBe('com.example.A');
    expect(wrapper?.style.display).toBe('contents');
  });

  test('writes no label when there is no owner', () => {
    m.render(container, withOwner(undefined, m('.child')));
    const wrapper = container.querySelector('.child')?.parentElement;
    expect(wrapper?.hasAttribute('data-plugin')).toBe(false);
  });
});
