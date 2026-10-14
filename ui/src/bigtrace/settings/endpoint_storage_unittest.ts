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

import {describe, expect, test} from 'vitest';
import {isAutoTrustedEndpoint} from './endpoint_storage';

describe('isAutoTrustedEndpoint', () => {
  const page = 'https://ui.example.com';

  test('trusts loopback servers and the page origin', () => {
    for (const ep of [
      'http://127.0.0.1:5001',
      'http://localhost:5001/v1',
      'http://[::1]:5001',
      `${page}/v1`,
    ]) {
      expect(isAutoTrustedEndpoint(ep, page)).toBe(true);
    }
  });

  test('does not trust other hosts or schemes', () => {
    for (const ep of [
      'https://evil.example.com/v1',
      'http://127.0.0.1.evil.example.com',
      'https://ui.example.com.evil.example.com',
      'javascript:alert(1)',
      'not a url',
    ]) {
      expect(isAutoTrustedEndpoint(ep, page)).toBe(false);
    }
  });
});
