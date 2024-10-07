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

import {Time} from '../base/time';
import {createEmptyRecordConfig} from '../controller/record_config_types';
import {featureFlags} from '../core/feature_flags';
import {
  autosaveConfigStore,
  recordTargetStore,
} from '../frontend/record_config';
import {State, STATE_VERSION} from './state';

const AUTOLOAD_STARTED_CONFIG_FLAG = featureFlags.register({
  id: 'autoloadStartedConfig',
  name: 'Auto-load last used recording config',
  description:
    'Starting a recording automatically saves its configuration. ' +
    'This flag controls whether this config is automatically loaded.',
  defaultValue: true,
});

export function keyedMap<T>(
  keyFn: (key: T) => string,
  ...values: T[]
): Map<string, T> {
  const result = new Map<string, T>();

  for (const value of values) {
    result.set(keyFn(value), value);
  }

  return result;
}

export function createEmptyState(): State {
  return {
    version: STATE_VERSION,
    nextId: '-1',

    recordConfig: AUTOLOAD_STARTED_CONFIG_FLAG.get()
      ? autosaveConfigStore.get()
      : createEmptyRecordConfig(),
    displayConfigAsPbtxt: false,
    lastLoadedConfig: {type: 'NONE'},

    perfDebug: false,
    sidebarVisible: true,
    hoveredUtid: -1,
    hoveredPid: -1,
    hoveredNoteTimestamp: Time.INVALID,
    highlightedSliceId: -1,

    recordingInProgress: false,
    recordingCancelled: false,
    extensionInstalled: false,
    flamegraphModalDismissed: false,
    recordingTarget: recordTargetStore.getValidTarget(),
    availableAdbDevices: [],

    fetchChromeCategories: false,
    chromeCategories: undefined,

    trackFilterTerm: undefined,
    forceRunControllers: 0,
  };
}
