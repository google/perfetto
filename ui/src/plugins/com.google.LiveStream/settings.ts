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

import {z} from 'zod';
import type {Setting, SettingsManager} from '../../public/settings';

/** Choices of the update interval, in seconds. */
export const UPDATE_INTERVALS = ['0.25', '0.5', '1', '2'] as const;

/** Choices of the buffer delay, in seconds. */
export const BUFFER_DELAYS = ['0', '1', '2', '5', '10', '15'] as const;

/** Choices of the history kept in the browser, in minutes. */
export const RETENTIONS = ['2', '10', '60', '240'] as const;

/** The user's settings of live streams. */
export interface LiveSettings {
  readonly updateInterval: Setting<(typeof UPDATE_INTERVALS)[number]>;
  readonly bufferDelay: Setting<(typeof BUFFER_DELAYS)[number]>;
  readonly retention: Setting<(typeof RETENTIONS)[number]>;
}

/** Registers the settings, for the page's lifetime. */
export function registerLiveSettings(settings: SettingsManager): LiveSettings {
  return {
    updateInterval: settings.register({
      id: 'com.google.LiveStreamUpdateInterval',
      name: 'Live stream update interval (s)',
      description: 'Seconds between updates of live streams.',
      schema: z.enum(UPDATE_INTERVALS),
      defaultValue: '0.25',
    }),
    bufferDelay: settings.register({
      id: 'com.google.LiveStreamBufferDelay',
      name: 'Live stream buffer delay (s)',
      description:
        'Seconds the followed playhead trails the latest data, so that ' +
        'late data shows up before it scrolls into view.',
      schema: z.enum(BUFFER_DELAYS),
      defaultValue: '5',
    }),
    retention: settings.register({
      id: 'com.google.LiveStreamRetention',
      name: 'Live stream history (min)',
      description:
        'Minutes of history that live streams keep in the browser. Errors ' +
        'are kept regardless, up to 10,000.',
      schema: z.enum(RETENTIONS),
      defaultValue: '60',
    }),
  };
}
