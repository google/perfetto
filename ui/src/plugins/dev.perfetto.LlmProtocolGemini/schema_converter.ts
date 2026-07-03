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
 * inlined, then strips the meta/structural keys Gemini rejects ($schema,
 * additionalProperties, $ref, $defs).
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
  return sanitize(json) as Record<string, unknown>;
}

// Recursively remove JSON Schema keys Gemini's parameters validator rejects.
function sanitize(node: unknown): unknown {
  if (Array.isArray(node)) {
    return node.map(sanitize);
  }
  if (node !== null && typeof node === 'object') {
    const out: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(node)) {
      // Gemini rejects these meta/structural keys.
      if (
        key === '$schema' ||
        key === 'additionalProperties' ||
        key === '$defs' ||
        key === '$ref'
      ) {
        continue;
      }
      out[key] = sanitize(value);
    }
    return out;
  }
  return node;
}
