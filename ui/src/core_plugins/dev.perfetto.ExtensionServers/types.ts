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
// manifestSchema (fetched from {base_url}/manifest)
//
// Resource schemas (fetched from {base_url}/modules/{module}/...):
//   ├── macrosSchema           → /macros
//   ├── sqlModulesSchema       → /sql_modules
//   └── protoDescriptorsSchema → /proto_descriptors
//
// =============================================================================

// Auth schemas for each server type. These are discriminated unions so that
// secret fields (like PAT) only exist in variants that need them.
// Fields containing secrets should use .meta({secret: true}) so that any
// future settings export feature can identify and strip them.
const githubAuthSchema = z.discriminatedUnion('type', [
  z.object({type: z.literal('none')}),
  z.object({
    type: z.literal('github_pat'),
    pat: z.string().meta({secret: true}).default(''),
  }),
]);

const httpsAuthSchema = z.discriminatedUnion('type', [
  z.object({type: z.literal('none')}),
]);

// Extension server configuration (persisted via Settings).
// Discriminated union: GitHub servers store repo+ref, HTTPS servers store a URL.
// Auth is constrained per server type via nested discriminated unions.
export const extensionServerSchema = z.discriminatedUnion('type', [
  z.object({
    type: z.literal('github'),
    repo: z.string(), // "owner/repo"
    ref: z.string(), // branch or tag, e.g. "main"
    path: z.string().default('/'), // subdirectory within the repo
    enabledModules: z.array(z.string()),
    enabled: z.boolean(),
    auth: githubAuthSchema.default({type: 'none'}),
  }),
  z.object({
    type: z.literal('https'),
    url: z.string(),
    enabledModules: z.array(z.string()),
    enabled: z.boolean(),
    auth: httpsAuthSchema.default({type: 'none'}),
  }),
]);

// Array of extension servers.
// This is the schema used for the Settings registration.
export const extensionServersSchema = z.array(extensionServerSchema);

// The minimal set of fields needed to fetch from an extension server.
// ExtensionServer is structurally compatible with this (has extra fields like
// enabledModules/enabled which are ignored).
export type UserInput =
  | {
      type: 'github';
      repo: string;
      ref: string;
      path: string;
      auth: {type: 'none'} | {type: 'github_pat'; pat: string};
    }
  | {type: 'https'; url: string; auth: {type: 'none'}};

// Manifest format from {base_url}/manifest
// Provides server metadata, features, and available modules.
//
// For each enabled module, the client fetches:
//   - {base_url}/modules/{name}/macros             → using MacrosSchema
//   - {base_url}/modules/{name}/sql_modules        → using SqlModulesSchema
//   - {base_url}/modules/{name}/proto_descriptors  → using ProtoDescriptorsSchema
export const manifestFeatureSchema = z.object({
  name: z.string(),
});

export const manifestModuleSchema = z.object({
  name: z.string(),
});

export const manifestSchema = z.object({
  name: z.string(),
  namespace: z.string(),
  features: z.array(manifestFeatureSchema),
  modules: z.array(manifestModuleSchema),
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
  sql_modules: z.array(sqlModuleSchema),
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
  proto_descriptors: z.array(protoDescriptorSchema),
});

// =============================================================================
// TypeScript Types (derived from Zod schemas)
// =============================================================================

export type ExtensionServer = z.infer<typeof extensionServerSchema>;
export type Manifest = z.infer<typeof manifestSchema>;
export type ManifestFeature = z.infer<typeof manifestFeatureSchema>;
export type ManifestModule = z.infer<typeof manifestModuleSchema>;
export type SqlModule = z.infer<typeof sqlModuleSchema>;
export type ProtoDescriptor = z.infer<typeof protoDescriptorSchema>;
