// Copyright (C) 2024 The Android Open Source Project
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

/**
 * A single proto descriptor entry.
 */
export const ProtoDescriptorSchema = z.object({
  name: z.string(),
  description: z.string().optional(),
  descriptor: z.string(),
});

/**
 * Extension server configuration (persisted via Settings).
 * Both installation-provided and user-added servers use this schema.
 */
export const ExtensionServerSchema = z.object({
  url: z.string(),
  selectedModules: z.array(z.string()),
  enabled: z.boolean(),
});

/**
 * Array of extension servers.
 * This is the schema used for the Settings registration.
 */
export const ExtensionServersSchema = z.array(ExtensionServerSchema);

/**
 * Manifest file format from {base_url}/manifest.json
 * Provides server metadata and available modules.
 */
export const ManifestSchema = z.object({
  name: z.string(),
  modules: z.array(z.string()),
  csp_allow: z.array(z.string()).optional(),
});

/**
 * Macros format from {base_url}/modules/{module}/macros
 * Maps macro names to command sequences.
 */
export const MacrosSchema = z.record(commandInvocationArraySchema);

/**
 * SQL Modules format from {base_url}/modules/{module}/sql_modules
 * Maps module paths (e.g., "android.startup") to SQL content.
 */
export const SqlModulesSchema = z.record(z.string());

/**
 * Proto Descriptors format from {base_url}/modules/{module}/proto_descriptors
 * Maps descriptor IDs to descriptor entries.
 */
export const ProtoDescriptorsSchema = z.record(ProtoDescriptorSchema);

// =============================================================================
// TypeScript Types (derived from Zod schemas)
// =============================================================================

export type ProtoDescriptor = z.infer<typeof ProtoDescriptorSchema>;

export type ExtensionServer = z.infer<typeof ExtensionServerSchema>;
export type Manifest = z.infer<typeof ManifestSchema>;
export type Macros = z.infer<typeof MacrosSchema>;
export type SqlModules = z.infer<typeof SqlModulesSchema>;
export type ProtoDescriptors = z.infer<typeof ProtoDescriptorsSchema>;

/**
 * Runtime state for an extension server.
 * Combines persisted config with derived and ephemeral fields.
 * Not persisted - computed fresh on each load.
 */
export interface ExtensionServerState {
  // Source configuration
  url: string;
  selectedModules: string[];
  enabled: boolean;

  // Derived fields (computed from url)
  resolvedUrl: string;
  serverKey: string;

  // Fetched fields (from manifest)
  displayName: string;
  availableModules: string[];
  cspAllowUrls: string[]; // From manifest.csp_allow

  // Ephemeral runtime state (not persisted)
  lastFetchStatus?: 'success' | 'error';
  lastFetchError?: string;
}

/**
 * Aggregated extensions loaded from all enabled servers/modules.
 * Used internally by the extension system.
 */
export interface AggregatedExtensions {
  macros: Map<string, CommandInvocation[]>; // Key: [server_key module] macro_name
  sqlModules: Map<string, string>; // Key: module_path
  protoDescriptors: Map<string, ProtoDescriptor>; // Key: descriptor_id
}
