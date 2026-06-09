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

// What a plugin provides to register a tool. The model decides *whether* to
// call it (from `description`) and *how* (from `shape`); the harness validates
// the args against `shape` before invoking `callback`, so a malformed call
// becomes a tool-result error the model self-corrects from rather than an
// exception in your plugin.
export interface ToolRegistration<S extends ZodRawShape = ZodRawShape> {
  // Stable identifier. Model APIs constrain tool names to roughly
  // ^[a-zA-Z0-9_-]+$, so avoid dots/spaces. Must be unique.
  readonly name: string;

  // Prose telling the model *when* to call this tool, not just what it does.
  // This is load-bearing: "Run a query" is weak; describe the situations that
  // should trigger it and any constraints (e.g. "prefer aggregation").
  readonly description: string;

  // The argument shape as a zod raw shape, e.g. `{sql: z.string()}`. Add a
  // `.describe(...)` to each field - the model reads those too. This buys typed
  // callback args, the JSON Schema sent on the wire, and runtime validation
  // from one declaration.
  readonly shape: S;

  // True if the tool mutates UI state (selection, navigation, ...) vs. being
  // read-only. Not gated on in Phase 1, but recorded for a future consent hook.
  readonly mutating?: boolean;

  // The implementation. `args` arrive typed (inferred from `shape`) and already
  // validated. Return the string fed back to the model: data for read tools, a
  // short ack (e.g. 'OK') for mutating ones. Throwing turns into a tool-result
  // error the model can recover from.
  readonly callback: (args: z.infer<ZodObject<S>>) => Promise<string>;
}

// The capability IntellettoPlugin exposes to dependent plugins. Obtain it with
// `ctx.plugins.getPlugin(IntellettoPlugin)` in your plugin's onTraceLoad.
export interface IntellettoToolRegistrar {
  // Register a tool the assistant can call. Tools are trace-scoped: register in
  // onTraceLoad; they live for the lifetime of the loaded trace.
  registerTool<S extends ZodRawShape>(tool: ToolRegistration<S>): void;
}
