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
});
