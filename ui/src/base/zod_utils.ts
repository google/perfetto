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
  | {
      kind: 'nativeEnum';
      entries: ReadonlyArray<{key: string; value: string | number}>;
    }
  | {kind: 'unknown'};

/**
 * Introspects a zod schema and returns information about its type and
 * configuration. This centralizes the logic for determining schema types
 * which is used in multiple places for rendering UI controls.
 *
 * @param schema The zod schema to introspect
 * @returns A discriminated union describing the schema type and its config
 */
export function getZodSchemaInfo(schema: z.ZodTypeAny): ZodSchemaInfo {
  if (schema instanceof z.ZodBoolean) {
    return {kind: 'boolean'};
  }

  if (schema instanceof z.ZodString) {
    return {kind: 'string'};
  }

  if (schema instanceof z.ZodNumber) {
    const minCheck = schema._def.checks.find(
      (c: z.ZodNumberCheck) => c.kind === 'min',
    ) as {kind: 'min'; value: number} | undefined;
    const maxCheck = schema._def.checks.find(
      (c: z.ZodNumberCheck) => c.kind === 'max',
    ) as {kind: 'max'; value: number} | undefined;
    return {
      kind: 'number',
      min: minCheck?.value,
      max: maxCheck?.value,
    };
  }

  if (schema instanceof z.ZodEnum) {
    return {kind: 'enum', options: schema.options};
  }

  if (schema instanceof z.ZodNativeEnum) {
    const entries = Object.entries(schema._def.values)
      .filter(([key]) => typeof key === 'string' && isNaN(Number(key)))
      .map(([key, value]) => ({key, value: value as string | number}));
    return {kind: 'nativeEnum', entries};
  }

  return {kind: 'unknown'};
}
