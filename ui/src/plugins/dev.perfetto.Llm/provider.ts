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

// The Provider and Model layers: pure data. A Provider picks a Protocol (by
// id), adds connection details + an API key, and lists the Models it offers. A
// Model is one entry in that catalog, carrying its role(s). These shapes are
// what get persisted to settings (user-supplied providers) or pushed down by an
// extension server, so they're defined as zod schemas - one declaration buys us
// the TypeScript types and the runtime validation of stored/pushed data.

import {z} from 'zod';

// A model's role controls where it shows up. 'conversational' models back the
// assistant; 'flash' models back cheap/fast background tasks (summaries,
// autocomplete). A model can have both. Consumers ask the gateway for "the
// default model with role X" rather than naming a specific model.
export const MODEL_ROLES = ['conversational', 'flash'] as const;
export type ModelRole = (typeof MODEL_ROLES)[number];

// One entry in a provider's catalog.
export const MODEL_SCHEMA = z.object({
  // Stable id, unique within the provider. Used in the selected-model pointer.
  id: z.string(),
  // Human-readable label for the picker.
  label: z.string(),
  // The backend's own model name, e.g. 'gemini-2.5-flash'.
  modelName: z.string(),
  // The role(s) this model fills.
  roles: z.array(z.enum(MODEL_ROLES)).default(['conversational']),
  // Optional per-model system prompt prepended to the consumer's prompt.
  systemPrompt: z.string().optional(),
  temperature: z.number().optional(),
  maxOutputTokens: z.number().optional(),
  // The model's context-window size, used by the consumer for compaction
  // decisions. Optional - not every backend reports it.
  contextWindowTokens: z.number().optional(),
});
export type Model = z.infer<typeof MODEL_SCHEMA>;

// Where a provider came from. User-supplied providers are persisted to
// settings; extension-server providers are held in memory only and re-fetched
// on reload. Phase 1 only persists 'user' providers; 'extension-server' is
// modelled here so the gateway can carry both in one flat list.
export const PROVIDER_SOURCES = ['user', 'extension-server'] as const;
export type ProviderSource = (typeof PROVIDER_SOURCES)[number];

// A configured source of models. References a protocol by id and carries the
// credential bag that protocol's credentialFields describe.
export const PROVIDER_SCHEMA = z.object({
  // Stable id, unique across all providers. Used in the selected-model pointer.
  id: z.string(),
  // Which protocol to talk through, e.g. 'gemini'.
  protocol: z.string(),
  // Human-readable label for the picker.
  label: z.string(),
  // Credential bag keyed by the protocol's CredentialField.key. API keys live
  // here; mark the field secret in the protocol so export can strip it.
  credentials: z.record(z.string(), z.string().meta({secret: true})).default({}),
  // The models this provider surfaces.
  models: z.array(MODEL_SCHEMA).default([]),
});
export type Provider = z.infer<typeof PROVIDER_SCHEMA>;

// The persisted shape of the user's provider settings: a flat list. (Selected
// model is stored separately; see store.ts.)
export const PROVIDERS_SETTING_SCHEMA = z.array(PROVIDER_SCHEMA).default([]);
export type ProvidersSetting = z.infer<typeof PROVIDERS_SETTING_SCHEMA>;

// A pointer to the active Provider:Model pair.
export const SELECTED_MODEL_SCHEMA = z
  .object({
    providerId: z.string(),
    modelId: z.string(),
  })
  .nullable()
  .default(null);
export type SelectedModelRef = z.infer<typeof SELECTED_MODEL_SCHEMA>;

// A resolved (Provider, Model) pair - what consumers actually run against.
export interface ResolvedModel {
  readonly provider: Provider;
  readonly model: Model;
  readonly source: ProviderSource;
}
