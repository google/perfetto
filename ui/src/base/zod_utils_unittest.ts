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
import {getZodSchemaInfo} from './zod_utils';

describe('getZodSchemaInfo', () => {
  it('identifies boolean schema', () => {
    const schema = z.boolean();

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'boolean'});
  });

  it('identifies string schema', () => {
    const schema = z.string();

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'string'});
  });

  it('identifies number schema without constraints', () => {
    const schema = z.number();

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'number', min: undefined, max: undefined});
  });

  it('identifies number schema with min constraint', () => {
    const schema = z.number().min(0);

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'number', min: 0, max: undefined});
  });

  it('identifies number schema with max constraint', () => {
    const schema = z.number().max(100);

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'number', min: undefined, max: 100});
  });

  it('identifies number schema with min and max constraints', () => {
    const schema = z.number().min(0).max(100);

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'number', min: 0, max: 100});
  });

  it('identifies enum schema', () => {
    const schema = z.enum(['option1', 'option2', 'option3']);

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({
      kind: 'enum',
      options: ['option1', 'option2', 'option3'],
    });
  });

  it('identifies native enum schema with string values as enum', () => {
    enum StringEnum {
      A = 'a',
      B = 'b',
      C = 'c',
    }
    const schema = z.enum(StringEnum);

    const result = getZodSchemaInfo(schema);

    // Native string enums are treated as regular enums
    expect(result).toEqual({
      kind: 'enum',
      options: ['a', 'b', 'c'],
    });
  });

  it('returns unknown for native enum with numeric values', () => {
    enum NumericEnum {
      First = 0,
      Second = 1,
      Third = 2,
    }
    const schema = z.enum(NumericEnum);

    const result = getZodSchemaInfo(schema);

    // Numeric enums are not supported
    expect(result).toEqual({kind: 'unknown'});
  });

  it('returns unknown for object schema', () => {
    const schema = z.object({foo: z.string()});

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'unknown'});
  });

  it('returns unknown for array schema', () => {
    const schema = z.array(z.string());

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'unknown'});
  });

  it('returns unknown for record schema', () => {
    const schema = z.record(z.string(), z.string());

    const result = getZodSchemaInfo(schema);

    expect(result).toEqual({kind: 'unknown'});
  });

  it('returns number for int() mofidier type', () => {
    const schema = z.number().int();

    const result = getZodSchemaInfo(schema);

    expect(result).toMatchObject({kind: 'number'});
  });

  it('returns number for int() type', () => {
    const schema = z.int();

    const result = getZodSchemaInfo(schema);

    expect(result).toMatchObject({kind: 'number'});
  });
});
