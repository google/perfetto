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

import {RecordConfig} from '../common/state';

import {
  createEmptyRecordConfig,
  runParser,
  validateNamedRecordConfig,
  validateRecordConfig,
  ValidationResult
} from './validate_config';

test('validateRecordConfig does not keep invalid keys', () => {
  const key = 'Invalid key';
  const config: ValidationResult<RecordConfig> =
      runParser(validateRecordConfig, {[key]: 'Some random value'});

  expect((config.result as object).hasOwnProperty(key)).toEqual(false);

  // Information about an extra key is available in validation result.
  expect(config.extraKeys.includes(key)).toEqual(true);
});

test('validateRecordConfig keeps provided values', () => {
  const value = 31337;
  const config: ValidationResult<RecordConfig> =
      runParser(validateRecordConfig, {'durationMs': value});

  expect(config.result.durationMs).toEqual(value);

  // Check that the valid keys do not show as extra keys in validation result.
  expect(config.extraKeys.includes('durationMs')).toEqual(false);
});

test(
    'validateRecordConfig tracks invalid keys while using default values',
    () => {
      const config: ValidationResult<RecordConfig> = runParser(
          validateRecordConfig,
          {'durationMs': 'a string, this should not be a string'});
      const defaultConfig = createEmptyRecordConfig();

      expect(config.result.durationMs).toEqual(defaultConfig.durationMs);
      expect(config.invalidKeys.includes('durationMs')).toEqual(true);
    });

test(
    'validateNamedRecordConfig throws exception on required field missing',
    () => {
      const unparsedConfig = {
        title: 'Invalid config'
        // Key is missing
      };

      let thrown = false;
      try {
        runParser(validateNamedRecordConfig, unparsedConfig);
      } catch {
        thrown = true;
      }

      expect(thrown).toBeTruthy();
    });
