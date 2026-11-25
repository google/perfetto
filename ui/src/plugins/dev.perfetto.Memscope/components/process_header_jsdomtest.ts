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
import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import {ProcessHeader, type ProcessOption} from './process_header';

const OPTIONS: ProcessOption[] = [
  {key: '1', name: 'com.example.app', pid: 1001, detail: '42 MiB'},
  {key: '2', name: 'system_server', pid: 500, detail: '180 MiB'},
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

function buttons(): HTMLButtonElement[] {
  return Array.from(container.querySelectorAll('button'));
}

describe('ProcessHeader', () => {
  test('shows All processes back button when focused and clears on click', () => {
    const onClear = vi.fn();
    m.render(
      container,
      m(ProcessHeader, {
        title: 'Memscope',
        options: OPTIONS,
        selectedKey: '1',
        onSelect: () => {},
        onClear,
      }),
    );

    const backBtn = buttons().find((b) =>
      b.textContent?.includes('All processes'),
    );
    expect(backBtn).toBeDefined();
    backBtn!.click();
    expect(onClear).toHaveBeenCalledTimes(1);
  });

  test('hides back button on system view and shows All processes label', () => {
    m.render(
      container,
      m(ProcessHeader, {
        title: 'Memscope',
        options: OPTIONS,
        selectedKey: undefined,
        onSelect: () => {},
        onClear: () => {},
        unselectedLabel: 'All processes',
      }),
    );

    expect(buttons()).toHaveLength(1);
    expect(buttons()[0]!.textContent).toContain('All processes');
  });

  test('shows back button when canClear is true while waiting for a process', () => {
    const onClear = vi.fn();
    m.render(
      container,
      m(ProcessHeader, {
        title: 'Memscope',
        options: OPTIONS,
        selectedKey: undefined,
        onSelect: () => {},
        onClear,
        canClear: true,
        note: 'Waiting for com.missing to start…',
      }),
    );

    const backBtn = buttons().find((b) =>
      b.textContent?.includes('All processes'),
    );
    expect(backBtn).toBeDefined();
    backBtn!.click();
    expect(onClear).toHaveBeenCalledTimes(1);
  });

  test('omits back button when onClear is not provided', () => {
    m.render(
      container,
      m(ProcessHeader, {
        title: 'Memory Explorer',
        options: OPTIONS,
        selectedKey: '1',
        onSelect: () => {},
      }),
    );

    expect(
      buttons().find((b) => b.textContent?.includes('All processes')),
    ).toBeUndefined();
  });

  test('renders static badge without dropdown when only one process exists', () => {
    m.render(
      container,
      m(ProcessHeader, {
        title: 'Memory Explorer',
        options: [OPTIONS[0]!],
        selectedKey: '1',
        onSelect: () => {},
      }),
    );

    expect(buttons()).toHaveLength(0);
    expect(container.textContent).toContain('com.example.app');
    expect(container.textContent).toContain('PID 1001');
  });
});
