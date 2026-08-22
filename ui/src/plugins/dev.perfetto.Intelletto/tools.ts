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

import {z, type ZodObject, type ZodRawShape} from 'zod';
import type {ToolRegistration} from './api';

/**
 * A registered tool, normalised from a ToolRegistration: the args shape is
 * wrapped in a ZodObject and the callback takes unknown args (validated on
 * call).
 */
export interface ToolDef {
  readonly name: string;
  readonly description: string;
  readonly inputSchema: ZodObject<ZodRawShape>;
  /** Whether the tool mutates UI state (vs. read-only). */
  readonly mutating: boolean;
  readonly callback: (args: unknown) => Promise<string>;
}

/**
 * The concrete registry behind IntellettoToolRegistrar.registerTool. Core and
 * plugin-contributed tools both land here; the agent reads it to expose tools
 * to the model.
 */
export class ToolRegistry {
  private readonly tools = new Map<string, ToolDef>();

  /** Register a tool. Throws if the name is already registered. */
  registerTool<S extends ZodRawShape>(opts: ToolRegistration<S>): void {
    if (this.tools.has(opts.name)) {
      throw new Error(`Tool "${opts.name}" already registered`);
    }
    const inputSchema = z.object(opts.shape);
    this.tools.set(opts.name, {
      name: opts.name,
      description: opts.description,
      inputSchema: inputSchema as unknown as ZodObject<ZodRawShape>,
      mutating: opts.mutating ?? false,
      callback: opts.callback as (args: unknown) => Promise<string>,
    });
  }

  /** Look up a tool by name, or undefined if not registered. */
  get(name: string): ToolDef | undefined {
    return this.tools.get(name);
  }

  /** All registered tools, in registration order. */
  list(): readonly ToolDef[] {
    return Array.from(this.tools.values());
  }

  /**
   * Validate args against the tool's schema, then invoke. Throws on unknown
   * tool or invalid args so the agent loop can fold it back as a tool-result
   * error.
   */
  async call(name: string, rawArgs: unknown): Promise<string> {
    const tool = this.tools.get(name);
    if (tool === undefined) throw new Error(`Unknown tool "${name}"`);
    const parsed = tool.inputSchema.safeParse(rawArgs ?? {});
    if (!parsed.success) {
      throw new Error(
        `Invalid arguments for "${name}": ${parsed.error.message}`,
      );
    }
    return tool.callback(parsed.data);
  }
}
