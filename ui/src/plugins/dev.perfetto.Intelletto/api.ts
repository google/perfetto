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

// Public API for the Intelletto assistant plugin. Other plugins depend on
// IntellettoPlugin and reach this surface via
// `ctx.plugins.getPlugin(IntellettoPlugin)` to contribute their own tools - the
// assistant doesn't need to know a feature exists, only that a tool drives it.

import type {z, ZodObject, ZodRawShape} from 'zod';

// What a plugin provides to register a tool the assistant can call. The model
// decides *whether* to call it (from `description`) and *how* (from `shape`);
// the harness validates args against `shape` before invoking `callback`.
export interface ToolRegistration<S extends ZodRawShape = ZodRawShape> {
  // Stable, unique identifier. Must match ^[a-zA-Z0-9_-]+$ (no dots/spaces).
  readonly name: string;

  // Prose telling the model *when* to call this tool, not just what it does.
  // Load-bearing: describe the triggering situations and any constraints.
  readonly description: string;

  // The argument shape as a zod raw shape, e.g. `{sql: z.string()}`. Add
  // `.describe(...)` to each field - the model reads those too.
  readonly shape: S;

  // True if the tool mutates UI state (selection, navigation, ...).
  readonly mutating?: boolean;

  // The implementation. `args` arrive typed and validated. Return the string
  // fed back to the model (data for reads, a short ack for mutations); throwing
  // becomes a tool-result error the model can recover from.
  readonly callback: (args: z.infer<ZodObject<S>>) => Promise<string>;
}

// What a context provider returns when it has something relevant to say about
// the current UI state. Both halves come from one callback so the chip and the
// model payload cannot drift apart.
export interface ContextSnapshot {
  // Plain-language summary, shown on the chip in the context strip.
  readonly summary: string;
  // JSON-serialisable payload sent to the model with the next prompt (and shown
  // when the chip is expanded). Keep it small - it travels with every turn.
  readonly data: unknown;
}

// What a plugin provides to register a context provider: a live view of some
// piece of UI state the model should know about ("what is the user looking
// at?").
export interface ContextProviderRegistration {
  // Stable, unique id, e.g. 'dev.perfetto.Timeline#selection'.
  readonly id: string;

  // Optional *invariant* explanation of the payload format, folded once into
  // the system prompt. Must not contain anything that changes per turn.
  readonly description?: string;

  // Sampled before each prompt (and to render the context strip). Return
  // undefined when there is nothing relevant right now.
  getContext(): ContextSnapshot | undefined;
}

// The capability IntellettoPlugin exposes to dependent plugins. Obtain it with
// `ctx.plugins.getPlugin(IntellettoPlugin)` in your plugin's onTraceLoad.
export interface IntellettoToolRegistrar {
  // Register a tool the assistant can call. Trace-scoped.
  registerTool<S extends ZodRawShape>(tool: ToolRegistration<S>): void;

  // Register a context provider describing what the user is looking at.
  registerContextProvider(provider: ContextProviderRegistration): void;
}
