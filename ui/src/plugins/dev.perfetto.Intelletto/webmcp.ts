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

// Registers Intelletto's tools with Chrome's experimental WebMCP API
// (document.modelContext.registerTool). Instead of duplicating definitions,
// it reads the already-populated ToolRegistry and exposes each tool.
// Gracefully no-ops when the API is not available.

import {z} from 'zod';
import type {ToolDef, ToolRegistry} from './tools';

// --- WebMCP type declarations ---
// Chrome's experimental API; not in lib.dom.d.ts yet.

interface WebMCPToolDefinition {
  readonly name: string;
  readonly description: string;
  readonly inputSchema: Record<string, unknown>;
  readonly execute: (args: Record<string, unknown>) => Promise<string>;
  readonly annotations?: {
    readonly readOnlyHint?: boolean;
    readonly untrustedContentHint?: boolean;
  };
}

interface WebMCPRegisterOptions {
  readonly signal?: AbortSignal;
  readonly exposedTo?: string[];
}

interface WebMCPModelContext {
  registerTool(
    tool: WebMCPToolDefinition,
    options?: WebMCPRegisterOptions,
  ): Promise<void>;
}

interface DocumentWithWebMCP extends Document {
  readonly modelContext: WebMCPModelContext;
}

// --- Helpers ---

function hasWebMCP(document: Document): document is DocumentWithWebMCP {
  return 'modelContext' in document && document.modelContext !== undefined;
}

/**
 * Convert a Zod schema to a JSON Schema object suitable for WebMCP's
 * inputSchema.
 */
function zodToJSONSchema(schema: z.ZodType<unknown>): Record<string, unknown> {
  return {...z.toJSONSchema(schema)};
}

// --- Registration ---

/**
 * Register all Intelletto tools with WebMCP by reading the shared registry.
 * Returns an AbortController whose .abort() method unregisters all tools.
 * No-ops if WebMCP is not available.
 */
export function registerWebMCPTools(
  registry: ToolRegistry,
): AbortController | null {
  if (!hasWebMCP(document)) {
    return null;
  }

  const controller = new AbortController();
  const mc = document.modelContext;

  for (const tool of registry.list()) {
    registerTool(mc, tool, controller.signal);
  }

  const listener = registry.addToolRegisteredListener((tool) => {
    registerTool(mc, tool, controller.signal);
  });

  controller.signal.addEventListener(
    'abort',
    () => listener[Symbol.dispose](),
    {once: true},
  );

  return controller;
}

/** Register a single ToolDef with WebMCP. */
async function registerTool(
  mc: WebMCPModelContext,
  tool: ToolDef,
  signal: AbortSignal,
): Promise<void> {
  try {
    return await mc.registerTool(
      {
        name: `perfetto_${tool.name}`,
        description: tool.description,
        inputSchema: zodToJSONSchema(tool.inputSchema),
        execute: async (rawArgs: Record<string, unknown>): Promise<string> => {
          // WebMCP passes raw args; delegate to the original callback.
          return await tool.callback(rawArgs);
        },
        annotations: {readOnlyHint: !tool.mutating},
      },
      {signal},
    );
  } catch {}
}
