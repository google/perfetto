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

import {describe, expect, test, vi} from 'vitest';
import m from 'mithril';
import {renderSetting} from './settings_widgets';
import type {Setting} from './settings_types';

function fakeStringSetting(initial: string) {
  let value = initial;
  const setSpy = vi.fn((v: unknown) => {
    value = v as string;
  });
  const setting = {
    id: 's',
    name: 'S',
    description: '',
    type: 'string',
    defaultValue: '',
    get: () => value,
    set: setSpy,
    reset: () => {},
    isDisabled: () => false,
    setDisabled: () => {},
    isDefault: false,
    [Symbol.dispose]: () => {},
  } as unknown as Setting<unknown>;
  return {setting, setSpy};
}

describe('renderSetting string input (deferred commit)', () => {
  test('keeps in-progress typing across a redraw and commits only on blur', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const {setting, setSpy} = fakeStringSetting('old');
    m.mount(root, {view: () => renderSetting(setting)});
    const input = root.querySelector('input')!;
    expect(input.value).toBe('old');

    // Typing updates the field but doesn't commit yet.
    input.value = 'newpath';
    input.dispatchEvent(new Event('input', {bubbles: true}));
    expect(setSpy).not.toHaveBeenCalled();

    // Regression guard: an unrelated redraw must not reset the field.
    m.redraw.sync();
    expect(input.value).toBe('newpath');

    // Blur/Enter commits the buffered value.
    input.dispatchEvent(new Event('change', {bubbles: true}));
    expect(setSpy).toHaveBeenCalledWith('newpath');

    m.mount(root, null);
    document.body.removeChild(root);
  });
});
