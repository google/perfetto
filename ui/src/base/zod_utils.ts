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
 * Discriminated union representing the introspected type of a zod schema.
 * Used for rendering appropriate UI controls based on the schema type.
 */
export type ZodSchemaInfo =
  | {kind: 'boolean'}
  | {kind: 'string'}
  | {kind: 'number'; min?: number; max?: number}
  | {kind: 'enum'; options: readonly string[]}
  | {kind: 'unknown'};

// JSON Schema type definition for the subset we care about
interface JSONSchema {
  type?: string;
  enum?: readonly (string | number)[];
  minimum?: number;
  maximum?: number;
}

/**
 * Introspects a zod schema and returns information about its type and
 * configuration. Uses toJSONSchema() for reliable introspection rather than
 * accessing internal zod properties.
 *
 * @param schema The zod schema to introspect
 * @returns A discriminated union describing the schema type and its config
 */
export function getZodSchemaInfo(schema: z.ZodTypeAny): ZodSchemaInfo {
  const jsonSchema = schema.toJSONSchema() as JSONSchema;

  // Check for enum first - enums have the 'enum' property
  if (jsonSchema.enum !== undefined) {
    // Only support string enums for now (most common case)
    if (jsonSchema.enum.every((v) => typeof v === 'string')) {
      return {kind: 'enum', options: jsonSchema.enum as readonly string[]};
    }
    // Mixed or numeric enums fall through to unknown
    return {kind: 'unknown'};
  }

  switch (jsonSchema.type) {
    case 'boolean':
      return {kind: 'boolean'};

    case 'string':
      return {kind: 'string'};

    case 'number':
    case 'integer':
      return {
        kind: 'number',
        min: jsonSchema.minimum,
        max: jsonSchema.maximum,
      };

    default:
      return {kind: 'unknown'};
  }
}
