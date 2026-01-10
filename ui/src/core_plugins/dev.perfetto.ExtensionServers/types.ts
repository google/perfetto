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
import {macroSchema} from '../../core/command_manager';

// =============================================================================
// Zod Schemas (source of truth)
// =============================================================================
//
// Schema Hierarchy:
//
// extensionServersSchema (persisted in Settings)
//
// manifestSchema (fetched from {base_url}/manifest.json)
//
// Resource schemas (fetched from {base_url}/modules/{module}/...):
//   ├── macrosSchema           → /macros
//   ├── sqlModulesSchema       → /sql_modules
//   └── protoDescriptorsSchema → /proto_descriptors
//
// =============================================================================

// Extension server configuration (persisted via Settings).
// Both installation-provided and user-added servers use this schema.
export const extensionServerSchema = z.object({
  url: z.string(),
  enabledModules: z.array(z.string()),
  enabled: z.boolean(),
});

// Array of extension servers.
// This is the schema used for the Settings registration.
export const extensionServersSchema = z.array(extensionServerSchema);

// Manifest file format from {base_url}/manifest.json
// Provides server metadata and available modules.
//
// The `modules` array specifies a set of modules. For each enabled module,
// the client fetches:
//   - {base_url}/modules/{module}/macros       → using MacrosSchema
//   - {base_url}/modules/{module}/sql_modules  → using SqlModulesSchema
//   - {base_url}/modules/{module}/proto_descriptors → using ProtoDescriptorsSchema
export const manifestSchema = z.object({
  name: z.string(),
  namespace: z.string(),
  features: z.array(z.string()),
  modules: z.array(z.string()),
});

// Macros format from {base_url}/modules/{module}/macros
export const macrosSchema = z.object({
  macros: z.array(macroSchema),
});

// SQL Modules format from {base_url}/modules/{module}/sql_modules.
const sqlModuleSchema = z.object({
  name: z.string(),
  sql: z.string(),
});
export const sqlModulesSchema = z.object({
  sqlModules: z.array(sqlModuleSchema),
});

// Proto Descriptors format from {base_url}/modules/{module}/proto_descriptors
//
// Proto descriptors are binary-encoded protocol buffer schema definitions
// (FileDescriptorSet protos). They allow the UI to decode and display
// custom proto messages embedded in traces without having the .proto files
// compiled into the UI itself. Extension servers can provide descriptors
// for proprietary or custom proto types.
const protoDescriptorSchema = z.string();
export const protoDescriptorsSchema = z.object({
  descriptors: z.array(protoDescriptorSchema),
});

// =============================================================================
// TypeScript Types (derived from Zod schemas)
// =============================================================================

export type ExtensionServer = z.infer<typeof extensionServerSchema>;
export type Manifest = z.infer<typeof manifestSchema>;
export type SqlModule = z.infer<typeof sqlModuleSchema>;
export type ProtoDescriptor = z.infer<typeof protoDescriptorSchema>;
