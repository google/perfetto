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

import {expect, test, vi} from 'vitest';

// The module reads the saved channel and the page's version map at import.
async function importWith(saved: string, versions: object) {
  vi.resetModules();
  localStorage.setItem('bigtraceUiChannel', saved);
  document.body.dataset['perfetto_version'] = JSON.stringify(versions);
  return import('./release_channel');
}

test('runs the saved channel when the page deploys it', async () => {
  const channels = await importWith('canary', {
    stable: 'v1',
    canary: 'v2',
    autopush: 'v3',
  });
  expect(channels.getReleaseChannel()).toBe('canary');
  expect(channels.getDeployedReleaseChannels()).toEqual([
    'stable',
    'canary',
    'autopush',
  ]);
  expect(channels.channelStorageKey('tabs')).toBe('tabs.canary');
});

test('falls back to stable, like the loader, without that channel', async () => {
  const channels = await importWith('autopush', {stable: 'v1', canary: 'v2'});
  expect(channels.getReleaseChannel()).toBe('stable');
  expect(channels.getDeployedReleaseChannels()).toEqual(['stable', 'canary']);
  expect(channels.channelStorageKey('tabs')).toBe('tabs');
});
