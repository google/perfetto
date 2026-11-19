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
import {zodToGeminiSchema} from './schema_converter';

// Recursively collect every object key present anywhere in the schema tree, so
// a test can assert a Gemini-rejected key is gone at *all* depths, not just the
// root.
function allKeys(node: unknown): Set<string> {
  const keys = new Set<string>();
  const visit = (n: unknown) => {
    if (Array.isArray(n)) {
      n.forEach(visit);
    } else if (n !== null && typeof n === 'object') {
      for (const [k, v] of Object.entries(n)) {
        keys.add(k);
        visit(v);
      }
    }
  };
  visit(node);
  return keys;
}

describe('zodToGeminiSchema', () => {
  it('extracts object properties (not an empty string schema)', () => {
    const schema = zodToGeminiSchema(
      z.object({
        query: z.string(),
        limit: z.number(),
      }),
    );

    expect(schema.type).toBe('object');
    const properties = schema.properties as Record<string, unknown>;
    expect(Object.keys(properties).sort()).toEqual(['limit', 'query']);
    expect((properties.query as Record<string, unknown>).type).toBe('string');
    expect((properties.limit as Record<string, unknown>).type).toBe('number');
  });

  it('preserves descriptions and required', () => {
    const schema = zodToGeminiSchema(
      z.object({
        name: z.string().describe('The thing to look up'),
        note: z.string().optional(),
      }),
    );

    const properties = schema.properties as Record<string, unknown>;
    expect((properties.name as Record<string, unknown>).description).toBe(
      'The thing to look up',
    );
    // `name` is required, `note` (optional) is not.
    expect(schema.required).toEqual(['name']);
  });

  it('strips keys Gemini rejects at every depth', () => {
    const schema = zodToGeminiSchema(
      z.object({
        outer: z.object({
          inner: z.string(),
        }),
        list: z.array(z.object({item: z.number()})),
      }),
    );

    const keys = allKeys(schema);
    expect(keys.has('$schema')).toBe(false);
    expect(keys.has('additionalProperties')).toBe(false);
    expect(keys.has('$ref')).toBe(false);
    expect(keys.has('$defs')).toBe(false);
  });

  it('inlines nested object schemas rather than referencing them', () => {
    const schema = zodToGeminiSchema(
      z.object({
        nested: z.object({value: z.string()}),
      }),
    );

    const properties = schema.properties as Record<string, unknown>;
    const nested = properties.nested as Record<string, unknown>;
    // Inlined: the nested object's own properties are present in place.
    expect(nested.type).toBe('object');
    const nestedProps = nested.properties as Record<string, unknown>;
    expect((nestedProps.value as Record<string, unknown>).type).toBe('string');
  });

  it('handles an empty object schema', () => {
    const schema = zodToGeminiSchema(z.object({}));
    expect(schema.type).toBe('object');
    expect(allKeys(schema).has('additionalProperties')).toBe(false);
  });

  it('handles enum types', () => {
    const schema = zodToGeminiSchema(
      z.object({
        mode: z.enum(['fast', 'slow']),
      }),
    );

    const properties = schema.properties as Record<string, unknown>;
    expect((properties.mode as Record<string, unknown>).enum).toEqual([
      'fast',
      'slow',
    ]);
  });

  it('strips exclusiveMinimum and exclusiveMaximum while keeping minimum and maximum', () => {
    const schema = zodToGeminiSchema(
      z.object({
        positive: z.number().positive(),
        boundedInt: z.number().int().positive().max(500),
        clamped: z.number().min(0).max(1),
        strictRange: z.number().gt(1).lt(10),
      }),
    );

    const keys = allKeys(schema);
    expect(keys.has('exclusiveMinimum')).toBe(false);
    expect(keys.has('exclusiveMaximum')).toBe(false);

    const properties = schema.properties as Record<
      string,
      Record<string, unknown>
    >;
    expect(properties.positive).toEqual({type: 'number'});
    expect(properties.boundedInt).toEqual({type: 'integer', maximum: 500});
    expect(properties.clamped).toEqual({
      type: 'number',
      minimum: 0,
      maximum: 1,
    });
    expect(properties.strictRange).toEqual({type: 'number'});
  });

  it('strips propertyNames and additionalProperties from z.record()', () => {
    const schema = zodToGeminiSchema(
      z.object({
        criteriaFilters: z
          .record(z.string(), z.array(z.string()))
          .optional()
          .describe('Map of criterion name to option keys.'),
      }),
    );

    const keys = allKeys(schema);
    expect(keys.has('propertyNames')).toBe(false);
    expect(keys.has('additionalProperties')).toBe(false);

    const properties = schema.properties as Record<
      string,
      Record<string, unknown>
    >;
    expect(properties.criteriaFilters).toEqual({
      type: 'object',
      description: 'Map of criterion name to option keys.',
    });
  });

  it('recursively sanitizes anyOf branches', () => {
    const schema = zodToGeminiSchema(
      z.object({
        value: z.union([z.number().positive(), z.string()]),
      }),
    );

    const keys = allKeys(schema);
    expect(keys.has('exclusiveMinimum')).toBe(false);

    const properties = schema.properties as Record<
      string,
      Record<string, unknown>
    >;
    expect(properties.value).toEqual({
      anyOf: [{type: 'number'}, {type: 'string'}],
    });
  });

  it('preserves parameter names that collide with stripped schema keywords', () => {
    const schema = zodToGeminiSchema(
      z.object({
        propertyNames: z.array(z.string()),
        exclusiveMinimum: z.number().positive(),
      }),
    );

    const properties = schema.properties as Record<
      string,
      Record<string, unknown>
    >;
    expect(Object.keys(properties).sort()).toEqual([
      'exclusiveMinimum',
      'propertyNames',
    ]);
    expect(properties.propertyNames).toEqual({
      type: 'array',
      items: {type: 'string'},
    });
    expect(properties.exclusiveMinimum).toEqual({type: 'number'});
  });
});
