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
import {prettyDOM} from '@testing-library/dom';
import type {TreeExplorerMergeSelection} from './tree_explorer_merge';
import {TreeExplorerMergeControls} from './tree_explorer_merge_controls';

// DOM tests for the merge controls: the Merge switch, and while it is off,
// the profile list and the stepper. The controls are rendered into
// `container` (jsdom) with m.render, controlled by `selection`, which the
// tests update from onSelectionChange and re-render, like a host would.

const PROFILES = [
  {key: 'a', label: 'a.pprof'},
  {key: 'b', label: 'b.pprof'},
  {key: 'c', label: 'c.pprof'},
];

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function dumpDom(): string {
  const out = prettyDOM(container);
  return typeof out === 'string' ? out : '';
}

function renderControls(initial: TreeExplorerMergeSelection) {
  const changes: TreeExplorerMergeSelection[] = [];
  let selection = initial;
  const rerender = () =>
    m.render(
      container,
      m(TreeExplorerMergeControls, {
        profiles: PROFILES,
        selection,
        onSelectionChange: (s) => {
          changes.push(s);
          selection = s;
          rerender();
        },
      }),
    );
  rerender();

  const query = <T extends Element>(selector: string): T => {
    const el = container.querySelector<T>(selector);
    expect(el, `${selector} in ${dumpDom()}`).not.toBeNull();
    return el!;
  };
  return {
    changes,
    query,
    has: (selector: string) => container.querySelector(selector) !== null,
    text: () => container.textContent ?? '',
    toggleMerge: () => query<HTMLInputElement>('input[type=checkbox]').click(),
    button: (title: string) =>
      query<HTMLButtonElement>(`button[title="${title}"]`),
    select: () => query<HTMLSelectElement>('select'),
  };
}

describe('TreeExplorerMergeControls', () => {
  test('while merged, shows only the switch and the profile count', () => {
    const c = renderControls({merged: true});
    expect(c.query<HTMLInputElement>('input[type=checkbox]').checked).toBe(
      true,
    );
    expect(c.text()).toContain('3 profiles');
    expect(c.has('select')).toBe(false);
    expect(c.has('button[title="Next profile"]')).toBe(false);
  });

  test('turning merging off shows the first profile', () => {
    const c = renderControls({merged: true});
    c.toggleMerge();
    expect(c.changes).toEqual([{merged: false}]);
    expect(c.select().value).toBe('a');
    expect(c.text()).toContain('1 / 3');
    expect(c.button('Previous profile').disabled).toBe(true);
    expect(c.button('Next profile').disabled).toBe(false);
  });

  test('steps through the profiles in order', () => {
    const c = renderControls({merged: false, profileKey: 'a'});
    c.button('Next profile').click();
    expect(c.select().value).toBe('b');
    expect(c.text()).toContain('2 / 3');
    c.button('Next profile').click();
    expect(c.select().value).toBe('c');
    expect(c.button('Next profile').disabled).toBe(true);
    c.button('Previous profile').click();
    expect(c.changes).toEqual([
      {merged: false, profileKey: 'b'},
      {merged: false, profileKey: 'c'},
      {merged: false, profileKey: 'b'},
    ]);
  });

  test('picks a profile from the list', () => {
    const c = renderControls({merged: false});
    const select = c.select();
    expect(Array.from(select.options, (o) => o.text)).toEqual(
      PROFILES.map((p) => p.label),
    );
    select.value = 'c';
    select.dispatchEvent(new Event('change', {bubbles: true}));
    expect(c.changes).toEqual([{merged: false, profileKey: 'c'}]);
    expect(c.text()).toContain('3 / 3');
  });

  test('merging again remembers the shown profile', () => {
    const c = renderControls({merged: false, profileKey: 'b'});
    c.toggleMerge();
    expect(c.changes).toEqual([{merged: true, profileKey: 'b'}]);
    c.toggleMerge();
    expect(c.select().value).toBe('b');
  });

  test('shows the first profile for an unknown key', () => {
    const c = renderControls({merged: false, profileKey: 'gone'});
    expect(c.select().value).toBe('a');
    expect(c.text()).toContain('1 / 3');
  });
});
