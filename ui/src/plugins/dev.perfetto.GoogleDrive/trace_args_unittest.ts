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

import {
  GDRIVE_PLUGIN_ID,
  gdriveTraceArgs,
  parseGDriveTraceArgs,
} from './trace_args';

describe('parseGDriveTraceArgs', () => {
  test('returns fileId for valid args', () => {
    expect(parseGDriveTraceArgs({fileId: 'abc123'})).toBe('abc123');
    expect(parseGDriveTraceArgs({fileId: '0-aZ_9'})).toBe('0-aZ_9');
  });

  test('round-trips gdriveTraceArgs', () => {
    const pluginArgs = gdriveTraceArgs('file_123');
    expect(parseGDriveTraceArgs(pluginArgs[GDRIVE_PLUGIN_ID])).toBe('file_123');
  });

  test('returns undefined for non-object args', () => {
    expect(parseGDriveTraceArgs(undefined)).toBeUndefined();
    expect(parseGDriveTraceArgs(null)).toBeUndefined();
    expect(parseGDriveTraceArgs('abc123')).toBeUndefined();
    expect(parseGDriveTraceArgs(123)).toBeUndefined();
    expect(parseGDriveTraceArgs([])).toBeUndefined();
  });

  test('returns undefined when fileId is missing or not a string', () => {
    expect(parseGDriveTraceArgs({})).toBeUndefined();
    expect(parseGDriveTraceArgs({fileId: 123})).toBeUndefined();
    expect(parseGDriveTraceArgs({fileId: null})).toBeUndefined();
  });

  test('returns undefined for invalid fileId', () => {
    expect(parseGDriveTraceArgs({fileId: ''})).toBeUndefined();
    expect(parseGDriveTraceArgs({fileId: '../secret'})).toBeUndefined();
    expect(parseGDriveTraceArgs({fileId: 'abc?fields=*'})).toBeUndefined();
    expect(
      parseGDriveTraceArgs({fileId: 'abc\r\nX-Injected: true'}),
    ).toBeUndefined();
  });
});
