// Copyright (C) 2020 The Android Open Source Project
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

import {createEmptyRecordConfig, RecordConfig} from '../common/state';

interface RecordConfigValidationResult {
  config: RecordConfig;
  errorMessage?: string;
}

export function validateRecordConfig(
    config: {[key: string]: string|number|boolean|string[]|null}):
    RecordConfigValidationResult {
  // Remove the keys that are not in both createEmptyRecordConfig and
  // config.
  const newConfig: RecordConfig = createEmptyRecordConfig();
  const ignoredKeys: string[] = [];
  // TODO(bsebastien): Also check that types of properties match.
  Object.entries(newConfig).forEach(([key, value]) => {
    if (key in config && typeof value === typeof config[key]) {
      newConfig[key] = config[key];
    } else {
      ignoredKeys.push(key);
    }
  });

  // Check if config has additional keys that are not in
  // createEmptyRecordConfig().
  for (const key of Object.keys(config)) {
    if (!(key in newConfig)) {
      ignoredKeys.push(key);
    }
  }

  if (ignoredKeys.length > 0) {
    // At least return an empty RecordConfig if nothing match.
    return {
      errorMessage: 'Warning: Loaded config contains incompatible keys.\n\
        It may have been created with an older version of the UI.\n\
        Ignored keys: ' +
          ignoredKeys.join(' '),
      config: newConfig,
    };
  }
  return {config: newConfig};
}
