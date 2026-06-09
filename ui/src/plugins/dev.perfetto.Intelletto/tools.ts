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

// The tool surface: how the model gets *hands* to drive the UI and query the
// trace. A tool is a name + a description (telling the model *when* to call it)
// + a zod input schema (validated before the callback runs) + a callback. The
// registry validates the model's args up front, turning malformed args into a
// clean tool-result error the model can self-correct from rather than an
// exception in plugin code.

import {z, type ZodObject, type ZodRawShape} from 'zod';
import type {IntellettoToolRegistrar, ToolRegistration} from './api';

// A tool's callback returns a string (the tool result fed back to the model).
// Read tools return data; mutating tools just ack ('OK') or throw.
export interface ToolDef {
  readonly name: string;
  readonly description: string;
  readonly inputSchema: ZodObject<ZodRawShape>;
  // Whether the tool mutates UI state (vs. read-only). The harness doesn't gate
  // on this in Phase 1 (no consent model - tools are non-destructive and
  // session-scoped), but it's the natural hook to add a confirmation later.
  readonly mutating: boolean;
  readonly callback: (args: unknown) => Promise<string>;
}

// The concrete registry behind the public IntellettoToolRegistrar. Core tools
// and plugin-contributed tools both land here; the agent reads it to expose
// tools to the model.
export class ToolRegistry implements IntellettoToolRegistrar {
  private readonly tools = new Map<string, ToolDef>();

  // Register a tool. `shape` is a zod raw shape (e.g. {sql: z.string()}); we
  // wrap it in a ZodObject so we can both validate at call time and emit JSON
  // Schema for the model.
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

  get(name: string): ToolDef | undefined {
    return this.tools.get(name);
  }

  list(): ReadonlyArray<ToolDef> {
    return Array.from(this.tools.values());
  }

  // Validate args against the tool's schema, then invoke. Validation failures
  // are returned as a thrown Error so the agent loop can fold them back into
  // the conversation as a tool-result error (the model self-corrects).
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
