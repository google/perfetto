// Copyright (C) 2021 The Android Open Source Project
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

import {raf} from './raf_scheduler';

export const DEFAULT_CHANNEL = 'stable';
const CHANNEL_KEY = 'perfettoUiChannel';

let currentChannel: string | undefined = undefined;
let nextChannel: string | undefined = undefined;

// This is the channel the UI is currently running. It doesn't change once the
// UI has been loaded.
export function getCurrentChannel(): string {
  if (currentChannel === undefined) {
    currentChannel = localStorage.getItem(CHANNEL_KEY) ?? DEFAULT_CHANNEL;
  }
  return currentChannel;
}

// This is the channel that will be applied on reload.
export function getNextChannel(): string {
  if (nextChannel !== undefined) {
    return nextChannel;
  }
  return getCurrentChannel();
}

export function channelChanged(): boolean {
  return getCurrentChannel() !== getNextChannel();
}

export function setChannel(channel: string): void {
  getCurrentChannel(); // Cache the current channel before mangling next one.
  nextChannel = channel;
  localStorage.setItem(CHANNEL_KEY, channel);
  raf.scheduleFullRedraw();
}
