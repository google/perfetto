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

// A model's role guides where it can be used:
// - agentic models can be used converstaionally and use tools.
// - flash models are more for smaller tasks such as summarizing information.
export const MODEL_ROLES = ['agentic', 'flash'] as const;
export type ModelRole = (typeof MODEL_ROLES)[number];

// One entry in a provider's catalog.
export const MODEL_SCHEMA = z.object({
  // Stable id, unique within the provider. Used in the selected-model pointer.
  id: z.string(),
  // Human-readable label for the picker.
  label: z.string().optional(),
  // The backend's own model name, e.g. 'gemini-2.5-flash'.
  modelName: z.string(),
  // The role(s) this model fills.
  roles: z.array(z.enum(MODEL_ROLES)),
});
export type Model = z.infer<typeof MODEL_SCHEMA>;

// A configured source of models. References a protocol by id and carries the
// credential bag that protocol's credentialFields describe.
export const PROVIDER_SCHEMA = z.object({
  // Stable id, unique across all providers. Used in the selected-model pointer.
  id: z.string(),
  // Which protocol to talk through, e.g. 'gemini'.
  protocol: z.string(),
  // Human-readable label for the picker.
  label: z.string().optional(),
  // Credential bag keyed by the protocol's CredentialField.key. API keys live
  // here; mark the field secret in the protocol so export can strip it.
  credentials: z
    .record(z.string(), z.string().meta({secret: true}))
    .default({}),
  // The models this provider surfaces.
  models: z.array(MODEL_SCHEMA).default([]),
});
export type Provider = z.infer<typeof PROVIDER_SCHEMA>;

// The persisted shape of the user's provider settings: a flat list. (Selected
// model is stored separately; see store.ts.)
export const PROVIDERS_SETTING_SCHEMA = z.array(PROVIDER_SCHEMA).default([]);
export type ProvidersSetting = z.infer<typeof PROVIDERS_SETTING_SCHEMA>;
