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
import {BigtraceHttpError} from './bigtrace_query_client';
import {describeLoadError} from './column_values_loader';

const RPC_503 =
  '{"error": {"code": 503, "message": "The service is currently ' +
  'unavailable.", "status": "UNAVAILABLE"}}';

describe('value load errors', () => {
  test('describeLoadError: backend message first, full text as detail', () => {
    const e = new BigtraceHttpError(503, RPC_503);
    const {summary, detail} = describeLoadError(e);
    expect(summary).toBe('The service is currently unavailable. (HTTP 503)');
    expect(detail).toBe(e.message);
  });

  test('describeLoadError: non-JSON body and network errors', () => {
    expect(
      describeLoadError(new BigtraceHttpError(502, 'Bad Gateway')),
    ).toEqual({
      summary: 'Bad Gateway (HTTP 502)',
      detail: 'HTTP 502: Bad Gateway',
    });
    expect(describeLoadError(new TypeError('Failed to fetch')).summary).toBe(
      'Could not reach the backend.',
    );
    expect(describeLoadError('weird')).toEqual({
      summary: 'weird',
      detail: 'weird',
    });
  });
});
