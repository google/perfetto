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

const RELEASE_CHANNELS = ['stable', 'canary', 'autopush'] as const;
export type ReleaseChannel = (typeof RELEASE_CHANNELS)[number];

// Separate from perfettoUiChannel so /perfetto and BigTrace don't switch each other.
const CHANNEL_KEY = 'bigtraceUiChannel';

const VERSIONS = JSON.parse(
  document.body.dataset['perfetto_version'] ?? '{}',
) as Record<string, string>;
const saved = localStorage.getItem(CHANNEL_KEY);
const CURRENT: ReleaseChannel =
  RELEASE_CHANNELS.find((c) => c === saved && Boolean(VERSIONS[c])) ?? 'stable';

export function getReleaseChannel(): ReleaseChannel {
  return CURRENT;
}

export function getDeployedReleaseChannels(): ReleaseChannel[] {
  return RELEASE_CHANNELS.filter((c) => Boolean(VERSIONS[c]));
}

export function switchReleaseChannel(channel: ReleaseChannel): void {
  localStorage.setItem(CHANNEL_KEY, channel);
  window.location.reload();
}

export function channelStorageKey(key: string): string {
  return CURRENT === 'stable' ? key : `${key}.${CURRENT}`;
}
