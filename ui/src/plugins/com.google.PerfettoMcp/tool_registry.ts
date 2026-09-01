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

import {z, type ZodRawShape, type ZodTypeAny} from 'zod';

// What a tool's handler returns. Matches the shape used by the previous
// @modelcontextprotocol/sdk McpServer.tool() so that the contents of
// tracetools.ts / uitools.ts don't need to change.
export interface ToolResult {
  readonly content: ReadonlyArray<{
    readonly type: 'text';
    readonly text: string;
  }>;
}

interface RegisteredTool {
  readonly name: string;
  readonly description: string;
  readonly schema: z.ZodObject<ZodRawShape>;
  readonly handler: (args: unknown) => Promise<ToolResult>;
}

// Local replacement for the McpServer / Client / InMemoryTransport trio.
// Everything ran in-process anyway; this is just a typed dispatcher.
export class ToolRegistry {
  private readonly tools = new Map<string, RegisteredTool>();

  // Mirrors McpServer.tool(name, description, shape, handler). `shape` is a
  // zod raw shape (e.g. {query: z.string()}); we wrap it into a ZodObject so
  // we can both validate at call time and emit a JSON Schema for the LLM.
  tool<S extends ZodRawShape>(
    name: string,
    description: string,
    shape: S,
    handler: (args: z.infer<z.ZodObject<S>>) => Promise<ToolResult>,
  ): void {
    const schema = z.object(shape);
    this.tools.set(name, {
      name,
      description,
      schema: schema as unknown as z.ZodObject<ZodRawShape>,
      handler: handler as (args: unknown) => Promise<ToolResult>,
    });
  }

  list(): ReadonlyArray<RegisteredTool> {
    return Array.from(this.tools.values());
  }

  async call(name: string, rawArgs: unknown): Promise<ToolResult> {
    const tool = this.tools.get(name);
    if (!tool) throw new Error(`Unknown tool "${name}"`);
    const parsed = tool.schema.parse(rawArgs ?? {});
    return tool.handler(parsed);
  }
}

// Convert a zod object schema into a JSON-Schema-ish object suitable for
// Gemini's `functionDeclarations[].parameters`. Gemini accepts a tight subset
// of JSON Schema; we cover what the existing tools use (strings, numbers,
// booleans, objects, optional fields). Anything more exotic gets stringified.
// Minimal view of zod's internal `_def` field. zod doesn't expose this in its
// public types but the shape is stable across the 3.x line we depend on.
interface ZodInternalDef {
  readonly typeName: string;
  readonly innerType?: ZodTypeAny;
  readonly shape?: () => Record<string, ZodTypeAny>;
}

function defOf(schema: ZodTypeAny): ZodInternalDef {
  return (schema as unknown as {_def: ZodInternalDef})._def;
}

export function zodToGeminiSchema(schema: ZodTypeAny): Record<string, unknown> {
  const def = defOf(schema);
  switch (def.typeName) {
    case 'ZodString':
      return {type: 'string'};
    case 'ZodNumber':
      return {type: 'number'};
    case 'ZodBoolean':
      return {type: 'boolean'};
    case 'ZodOptional':
      return zodToGeminiSchema(def.innerType!);
    case 'ZodObject': {
      const shape = def.shape!();
      const properties: Record<string, unknown> = {};
      const required: string[] = [];
      for (const [key, value] of Object.entries(shape)) {
        properties[key] = zodToGeminiSchema(value);
        if (defOf(value).typeName !== 'ZodOptional') required.push(key);
      }
      const out: Record<string, unknown> = {type: 'object', properties};
      if (required.length > 0) out.required = required;
      return out;
    }
    default:
      // Fallback: accept anything as a string. Good enough for the current
      // tool set; revisit if a tool needs arrays or unions.
      return {type: 'string'};
  }
}
