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

import {legacyMacrosConfigSchema} from './index';

describe('legacyMacrosConfigSchema', () => {
  it('parses a valid legacy macros config', () => {
    const input = {
      'my-macro': [
        {id: 'dev.perfetto.RunQuery', args: ['SELECT * FROM slice']},
        {id: 'dev.perfetto.ShowTab', args: ['query_results']},
      ],
      'another-macro': [{id: 'dev.perfetto.PanToTimestamp', args: []}],
    };

    const result = legacyMacrosConfigSchema.parse(input);

    expect(result['my-macro']).toHaveLength(2);
    expect(result['my-macro'][0].id).toBe('dev.perfetto.RunQuery');
    expect(result['my-macro'][0].args).toEqual(['SELECT * FROM slice']);
    expect(result['another-macro']).toHaveLength(1);
  });

  it('accepts empty object', () => {
    const result = legacyMacrosConfigSchema.parse({});

    expect(result).toEqual({});
  });

  it('accepts empty command arrays', () => {
    const input = {
      'empty-macro': [],
    };

    const result = legacyMacrosConfigSchema.parse(input);

    expect(result['empty-macro']).toEqual([]);
  });

  it('rejects invalid command invocations', () => {
    const input = {
      'bad-macro': [{id: 123, args: []}], // id should be string
    };

    expect(() => legacyMacrosConfigSchema.parse(input)).toThrow();
  });

  it('rejects non-array args', () => {
    const input = {
      'bad-macro': [{id: 'cmd', args: 'not-an-array'}],
    };

    expect(() => legacyMacrosConfigSchema.parse(input)).toThrow();
  });
});
