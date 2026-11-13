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
import {AppImpl} from '../core/app_impl';
import {createFakeTraceImpl} from '../core/fake_trace_impl';
import {setOwner} from '../core/ownership';
import type {OmniboxModeDescriptor} from '../public/omnibox';
import {Omnibox} from './omnibox';

let container: HTMLElement;

beforeEach(() => {
  createFakeTraceImpl();
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function inputOwner(): string | null | undefined {
  m.render(container, m(Omnibox));
  return container
    .querySelector('input')
    ?.closest('[data-plugin]')
    ?.getAttribute('data-plugin');
}

describe('Omnibox', () => {
  test("labels a registered mode with the mode's owner", () => {
    const omnibox = AppImpl.instance.omnibox;
    const mode: OmniboxModeDescriptor = {
      trigger: '?',
      placeholder: 'Ask',
      onSubmit: () => {},
    };
    setOwner(mode, 'com.example.Owner');
    omnibox.registerMode(mode);

    expect(inputOwner()).toBeUndefined();

    omnibox.activateRegisteredMode('?');
    expect(inputOwner()).toBe('com.example.Owner');
  });
});
