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
import type {JsonSchemaObject, JsonSchemaProperty} from './types';

function propertyToZod(prop: JsonSchemaProperty): z.ZodTypeAny {
  let schema: z.ZodTypeAny;
  if (prop.type === 'string') {
    if (prop.enum && prop.enum.length > 0) {
      schema = z.enum([prop.enum[0]!, ...prop.enum.slice(1)]);
    } else {
      schema = z.string();
    }
  } else if (prop.type === 'number') {
    schema = z.number();
  } else if (prop.type === 'integer') {
    schema = z.number().int();
  } else if (prop.type === 'boolean') {
    schema = z.boolean();
  } else if (prop.type === 'array') {
    const item = prop.items ? propertyToZod(prop.items) : z.unknown();
    schema = z.array(item);
  } else {
    schema = z.record(z.string(), z.unknown());
  }
  if (prop.description) {
    schema = schema.describe(prop.description);
  }
  return schema;
}

// Converts a declarative JSON Schema from products/*/tools/tools.json into a
// Zod object schema compatible with dev.perfetto.Llm's ToolDef.
export function jsonSchemaToZod(schema: JsonSchemaObject): z.ZodTypeAny {
  const props = schema.properties ?? {};
  const required = new Set(schema.required ?? []);
  const shape: Record<string, z.ZodTypeAny> = {};
  for (const [key, prop] of Object.entries(props)) {
    const s = propertyToZod(prop);
    shape[key] = required.has(key) ? s : s.optional();
  }
  return z.object(shape);
}
