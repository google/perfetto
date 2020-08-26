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
import {validateRecordConfig} from './validate_config';

test('validateRecordConfig test valid keys config', () => {
  const config: RecordConfig = createEmptyRecordConfig();
  const validationResult = validateRecordConfig(config);

  expect('errorMessage' in validationResult).toEqual(false);

  for (const key of Object.keys(validationResult.config)) {
    expect(key in config).toEqual(true);
  }
});

test('validateRecordConfig test no key config', () => {
  const emptyRecord: RecordConfig = createEmptyRecordConfig();
  const validationResult = validateRecordConfig({});

  expect('errorMessage' in validationResult).toEqual(true);

  for (const key of Object.keys(emptyRecord)) {
    expect(key in validationResult.config).toEqual(true);
  }
});

test('validateRecordConfig test some valid key config', () => {
  const emptyRecord: RecordConfig = createEmptyRecordConfig();
  const validationResult = validateRecordConfig({
    'durationMs': 5.0,
    'cpuSched': true,
  });

  expect('errorMessage' in validationResult).toEqual(true);

  expect(validationResult.config.durationMs).toEqual(5.0);
  expect(validationResult.config.cpuSched).toEqual(true);

  for (const key of Object.keys(emptyRecord)) {
    if (['durationMs', 'cpuSched'].includes(key) === false) {
      expect(validationResult.config[key]).toEqual(emptyRecord[key]);
    }
    expect(key in validationResult.config).toEqual(true);
  }
});

test('validateRecordConfig test some invalid key config', () => {
  const emptyRecord: RecordConfig = createEmptyRecordConfig();
  const validationResult = validateRecordConfig({
    'durationMs': 5.0,
    'invalidKey': 0,
    'cpuSched': true,
    'anotherInvalidKey': 'foobar',
  });

  expect('errorMessage' in validationResult).toEqual(true);

  expect(validationResult.config.durationMs).toEqual(5.0);
  expect(validationResult.config.cpuSched).toEqual(true);
  expect('invalidKey' in validationResult.config).toEqual(false);
  expect('anotherInvalidKey' in validationResult.config).toEqual(false);

  for (const key of Object.keys(emptyRecord)) {
    if (['durationMs', 'cpuSched'].includes(key) === false) {
      expect(validationResult.config[key]).toEqual(emptyRecord[key]);
    }
    expect(key in validationResult.config).toEqual(true);
  }
});

test('validateRecordConfig test only invalid key config', () => {
  const emptyRecord: RecordConfig = createEmptyRecordConfig();
  const validationResult = validateRecordConfig({
    'invalidKey': 0,
    'anotherInvalidKey': 'foobar',
  });

  expect('errorMessage' in validationResult).toEqual(true);

  expect('invalidKey' in validationResult.config).toEqual(false);
  expect('anotherInvalidKey' in validationResult.config).toEqual(false);

  for (const key of Object.keys(emptyRecord)) {
    expect(validationResult.config[key]).toEqual(emptyRecord[key]);
    expect(key in validationResult.config).toEqual(true);
  }
});
