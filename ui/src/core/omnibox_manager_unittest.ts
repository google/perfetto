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

import {OmniboxManagerImpl} from './omnibox_manager';

describe('OmniboxManagerImpl.setStatusMessage', () => {
  test('sets, updates, and clears status message and progress on dispose', () => {
    const omnibox = new OmniboxManagerImpl();
    expect(omnibox.statusMessage).toBeUndefined();
    expect(omnibox.showStatusProgress).toBe(false);

    const handle = omnibox.setStatusMessage('Downloading…', {
      showProgress: true,
    });
    expect(omnibox.statusMessage).toBe('Downloading…');
    expect(omnibox.showStatusProgress).toBe(true);

    handle.update('Downloading… 50%');
    expect(omnibox.statusMessage).toBe('Downloading… 50%');
    expect(omnibox.showStatusProgress).toBe(true);

    handle[Symbol.dispose]();
    expect(omnibox.statusMessage).toBeUndefined();
    expect(omnibox.showStatusProgress).toBe(false);
  });

  test('disposing or updating a superseded handle does not affect the newer message', () => {
    const omnibox = new OmniboxManagerImpl();
    const first = omnibox.setStatusMessage('First', {showProgress: true});
    const second = omnibox.setStatusMessage('Second');

    first.update('Stale update');
    expect(omnibox.statusMessage).toBe('Second');
    expect(omnibox.showStatusProgress).toBe(false);

    first[Symbol.dispose]();
    expect(omnibox.statusMessage).toBe('Second');

    second[Symbol.dispose]();
    expect(omnibox.statusMessage).toBeUndefined();
  });
});
