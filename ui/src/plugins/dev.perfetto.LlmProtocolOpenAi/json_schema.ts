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

// Convert a zod schema (how tools author their input shape) into the JSON
// Schema that an OpenAI-compatible `tools[].function.parameters` expects.
//
// We delegate to zod 4's built-in `z.toJSONSchema()` rather than hand-rolling
// the conversion. The previous hand-rolled walker keyed off `_def.typeName`,
// which is a zod *3* internal that doesn't exist in zod 4 - so every node fell
// through to a `string` fallback and tools were sent with no `properties` at
// all. That left grammar-constrained backends (e.g. llama.cpp) with nothing to
// fill, so the model emitted empty `{}` args. The native converter produces
// correct, complete schemas (properties, required, descriptions, nesting).

import {z} from 'zod';

export function zodToJsonSchema(schema: z.ZodType): Record<string, unknown> {
  const json = z.toJSONSchema(schema, {
    // Inline everything: tool parameter schemas must be self-contained, not
    // carry $defs/$ref that some servers reject.
    target: 'draft-2020-12',
  }) as Record<string, unknown>;
  // Strip the $schema declaration - it's noise in a tool parameters object and
  // some servers are picky about extra keys.
  delete json.$schema;
  return json;
}
