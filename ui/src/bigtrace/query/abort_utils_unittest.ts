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

import {forwardAbort} from './abort_utils';

describe('forwardAbort', () => {
  test('aborts child when parent aborts', () => {
    const parent = new AbortController();
    const child = new AbortController();
    forwardAbort(parent.signal, child);

    expect(child.signal.aborted).toBe(false);
    parent.abort();
    expect(child.signal.aborted).toBe(true);
  });

  test('aborts child immediately if parent is already aborted', () => {
    const parent = new AbortController();
    parent.abort();
    const child = new AbortController();
    forwardAbort(parent.signal, child);
    expect(child.signal.aborted).toBe(true);
  });

  test('detacher prevents future child aborts on parent abort', () => {
    const parent = new AbortController();
    const child = new AbortController();
    const detach = forwardAbort(parent.signal, child);

    detach();
    parent.abort();
    expect(child.signal.aborted).toBe(false);
  });

  test('aborting child does not affect parent', () => {
    const parent = new AbortController();
    const child = new AbortController();
    forwardAbort(parent.signal, child);

    child.abort();
    expect(parent.signal.aborted).toBe(false);
  });
});
