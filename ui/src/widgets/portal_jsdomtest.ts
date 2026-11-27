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
import {Portal} from './portal';

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

// The owner of the moved content, found the way the owner lookup does it:
// the nearest data-plugin above the element.
function ownerOf(selector: string): string | null | undefined {
  return document
    .querySelector(selector)
    ?.closest('[data-plugin]')
    ?.getAttribute('data-plugin');
}

describe('Portal', () => {
  test('gives moved content the owner of the area that opened it', () => {
    m.render(
      container,
      m('', {'data-plugin': 'com.example.A'}, m(Portal, m('.popup'))),
    );
    // The content lives outside the labeled area...
    expect(container.querySelector('.popup')).toBeNull();
    // ...but still gets its owner.
    expect(ownerOf('.popup')).toBe('com.example.A');
  });

  test('passes the owner through nested portals', () => {
    m.render(
      container,
      m(
        '',
        {'data-plugin': 'com.example.A'},
        m(Portal, m('.outer', m(Portal, m('.inner')))),
      ),
    );
    expect(ownerOf('.inner')).toBe('com.example.A');
  });

  test('follows the owner when the area around it changes', () => {
    const render = (owner: string | undefined) =>
      m.render(
        container,
        m('', {'data-plugin': owner}, m(Portal, m('.popup'))),
      );

    render('com.example.A');
    expect(ownerOf('.popup')).toBe('com.example.A');

    render('com.example.B');
    expect(ownerOf('.popup')).toBe('com.example.B');

    render(undefined);
    expect(ownerOf('.popup')).toBeUndefined();
  });
});
