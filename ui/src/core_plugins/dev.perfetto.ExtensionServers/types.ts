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
import {
  CommandInvocation,
  commandInvocationArraySchema,
} from '../../core/command_manager';

// =============================================================================
// Zod Schemas (source of truth)
// =============================================================================
//
// Schema Hierarchy:
//
// ExtensionServersSchema (persisted in Settings)
//   └── ExtensionServerSchema
//
// ManifestSchema (fetched from {base_url}/manifest.json)
//
// Resource schemas (fetched from {base_url}/modules/{module}/...):
//   ├── MacrosSchema           → /macros
//   ├── SqlModulesSchema       → /sql_modules
//   └── ProtoDescriptorsSchema → /proto_descriptors
//         └── ProtoDescriptorSchema
//
// =============================================================================

// Extension server configuration (persisted via Settings).
// Both installation-provided and user-added servers use this schema.
export const ExtensionServerSchema = z.object({
  url: z.string(),
  enabledModules: z.array(z.string()),
  enabled: z.boolean(),
});

// Array of extension servers.
// This is the schema used for the Settings registration.
export const ExtensionServersSchema = z.array(ExtensionServerSchema);

// Manifest file format from {base_url}/manifest.json
// Provides server metadata and available modules.
//
// The `modules` array specifies a set of modules. For each enabled module,
// the client fetches:
//   - {base_url}/modules/{module}/macros       → using MacrosSchema
//   - {base_url}/modules/{module}/sql_modules  → using SqlModulesSchema
//   - {base_url}/modules/{module}/proto_descriptors → using ProtoDescriptorsSchema
export const ManifestSchema = z.object({
  name: z.string(),
  modules: z.array(z.string()),
});

// Macros format from {base_url}/modules/{module}/macros
// Maps macro names to command sequences.
export const MacrosSchema = z.object({
  macros: z.record(commandInvocationArraySchema),
});

// SQL Modules format from {base_url}/modules/{module}/sql_modules
// Maps module paths (e.g., "android.startup") to SQL content.
export const SqlModulesSchema = z.object({
  modules: z.record(z.string()),
});

// Proto Descriptors format from {base_url}/modules/{module}/proto_descriptors
//
// Proto descriptors are binary-encoded protocol buffer schema definitions
// (FileDescriptorSet protos). They allow the UI to decode and display
// custom proto messages embedded in traces without having the .proto files
// compiled into the UI itself. Extension servers can provide descriptors
// for proprietary or custom proto types.
export const ProtoDescriptorsSchema = z.object({
  descriptors: z.array(z.string()),
});

// =============================================================================
// TypeScript Types (derived from Zod schemas)
// =============================================================================

export type ExtensionServer = z.infer<typeof ExtensionServerSchema>;
export type Manifest = z.infer<typeof ManifestSchema>;
export type Macros = z.infer<typeof MacrosSchema>;
export type SqlModules = z.infer<typeof SqlModulesSchema>;
export type ProtoDescriptors = z.infer<typeof ProtoDescriptorsSchema>;

// Runtime state for an extension server.
// Combines persisted config with derived and ephemeral fields.
// Not persisted - computed fresh on each load.
export interface ExtensionServerState {
  // Source configuration
  // Non-canonicalized (e.g. can be github://).
  url: string;
  enabledModules: string[];
  enabled: boolean;

  // Derived fields (computed from url)
  // Canonicalized URL (always https://, never github://).
  canonicalUrl: string;
  serverKey: string;

  // Fetched fields (from manifest)
  displayName: string;
  availableModules: string[];

  // Ephemeral runtime state (not persisted)
  // Absence of lastFetchError indicates success.
  lastFetchError?: string;
}

// Aggregated extensions loaded from all enabled servers/modules.
// Used internally by the extension system.
//
// Key formats:
// - macros: "[server_key module] macro_name"
//     Namespaced to avoid collisions between servers/modules.
//     Example: "[raw.githubusercontent.com/foo/bar default] run_query"
// - sqlModules: module_path
//     Already globally unique (e.g., "android.startup", "chrome.scroll").
export interface AggregatedExtensions {
  macros: Map<string, CommandInvocation[]>;
  sqlModules: Map<string, string>;
  protoDescriptors: ProtoDescriptors;
}
