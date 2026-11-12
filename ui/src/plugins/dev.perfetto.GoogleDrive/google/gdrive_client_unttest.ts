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

import {isValidDriveId} from './gdrive_client';

describe('isValidDriveId', () => {
  test('accepts valid drive IDs and resource keys', () => {
    expect(isValidDriveId('abc123')).toBe(true);
    expect(isValidDriveId('0-aZ_9')).toBe(true);
    expect(isValidDriveId('1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs74OgvE2upms')).toBe(
      true,
    );
  });

  test('rejects empty string', () => {
    expect(isValidDriveId('')).toBe(false);
  });

  test('rejects path traversal', () => {
    expect(isValidDriveId('..')).toBe(false);
    expect(isValidDriveId('../other')).toBe(false);
    expect(isValidDriveId('abc/def')).toBe(false);
    expect(isValidDriveId('abc\\def')).toBe(false);
    expect(isValidDriveId('..%2Fother')).toBe(false);
  });

  test('rejects query and fragment injection', () => {
    expect(isValidDriveId('abc?fields=*')).toBe(false);
    expect(isValidDriveId('abc&alt=media')).toBe(false);
    expect(isValidDriveId('abc#fragment')).toBe(false);
  });

  test('rejects header injection and whitespace', () => {
    expect(isValidDriveId('abc\r\nX-Injected: true')).toBe(false);
    expect(isValidDriveId('abc\ndef')).toBe(false);
    expect(isValidDriveId('abc def')).toBe(false);
  });
});
