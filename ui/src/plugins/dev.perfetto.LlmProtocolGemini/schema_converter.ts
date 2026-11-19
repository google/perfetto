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

/**
 * Convert a zod schema into the subset of JSON Schema that Gemini's
 * `functionDeclarations[].parameters` accepts.
 *
 * Emits base JSON Schema via zod 4's `z.toJSONSchema()` with subschemas
 * inlined, then keeps only the fields defined on Gemini's `Schema` proto
 * (dropping unsupported JSON Schema keywords such as `$schema`,
 * `additionalProperties`, `$defs`, `$ref`, `exclusiveMinimum`,
 * `exclusiveMaximum`, and `propertyNames`).
 *
 * @param schema The tool's input schema, authored in zod.
 * @returns A plain JSON-Schema object ready to drop into a Gemini function
 *   declaration's `parameters`.
 */
export function zodToGeminiSchema(schema: z.ZodType): Record<string, unknown> {
  const json = z.toJSONSchema(schema, {
    // Inline subschemas: Gemini does not accept $ref/$defs.
    target: 'draft-2020-12',
  }) as Record<string, unknown>;
  return sanitizeSchema(json);
}

// Scalar/non-recursive fields accepted by Gemini's v1beta `Schema` proto
// (https://ai.google.dev/api/caching#Schema). Recursive fields (`properties`,
// `items`, `anyOf`) are handled explicitly in `sanitizeSchema`.
const ALLOWED_SCALAR_KEYS: ReadonlySet<string> = new Set([
  'type',
  'format',
  'title',
  'description',
  'nullable',
  'enum',
  'maxItems',
  'minItems',
  'required',
  'minProperties',
  'maxProperties',
  'minLength',
  'maxLength',
  'pattern',
  'example',
  'propertyOrdering',
  'default',
  'minimum',
  'maximum',
]);

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

// Recursively strip any JSON Schema keys not present on Gemini's `Schema`
// proto, while preserving arbitrary parameter names inside `properties`.
function sanitizeSchema(
  node: Record<string, unknown>,
): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(node)) {
    if (key === 'properties' && isRecord(value)) {
      const props: Record<string, unknown> = {};
      for (const [propName, propSchema] of Object.entries(value)) {
        props[propName] = isRecord(propSchema)
          ? sanitizeSchema(propSchema)
          : propSchema;
      }
      out.properties = props;
    } else if (key === 'items' && isRecord(value)) {
      out.items = sanitizeSchema(value);
    } else if (key === 'anyOf' && Array.isArray(value)) {
      out.anyOf = value.map((item) =>
        isRecord(item) ? sanitizeSchema(item) : item,
      );
    } else if (ALLOWED_SCALAR_KEYS.has(key)) {
      out[key] = value;
    }
  }
  return out;
}
