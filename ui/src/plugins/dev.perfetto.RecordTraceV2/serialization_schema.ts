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
import {TARGET_PLATFORMS, TargetPlatformId} from './interfaces/target_platform';

// Overall view
// RECORD_PLUGIN_SCHEMA:
//   target: TARGET_SCHEMA
//   lastSession: RECORD_SESSION_SCHEMA
//      probes: PROBES_SCHEMA{}
//   savedSessions: Array<RECORD_SESSION_SCHEMA>
//      probes: PROBES_SCHEMA{}

// Holds the state of the PROBES_PAGE subpages (e.g., Memory).
// We don't define a strongly-typed schema for each probes as they are
// changed frequently. Each probe is modelled as:
// - An enable/disable boolean (the presence of the key)
// - A map of "settings". Each setting widget (Slider, Textarea, Toggle)
//   takes care of its own de/serialization.
export const PROBES_SCHEMA = z
  .record(
    z.string(), // key: the RecordProbe.id (it's globally unique).
    z.object({
      settings: z
        .record(
          z.string(), // key: the key in the RecordProbe.settings map.
          z.unknown(), // value: The result of ProbeSetting.serialize().
        )
        .default({}),
    }),
  )
  .default({});
export type ProbesSchema = z.infer<typeof PROBES_SCHEMA>;

// A probe-based session config built from the UI probe settings.
export const PROBES_SESSION_SCHEMA = z.object({
  // Old configs won't have 'kind', so default to 'probes'.
  kind: z.literal('probes').default('probes'),
  mode: z
    .enum(['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE'])
    .default('STOP_WHEN_FULL'),
  bufSizeKb: z.number().default(64 * 1024),
  durationMs: z.number().default(10_000),
  maxFileSizeMb: z.number().default(500),
  fileWritePeriodMs: z.number().default(2500),
  compression: z.boolean().default(false),
  probes: PROBES_SCHEMA,
});
export type ProbesSessionSchema = z.infer<typeof PROBES_SESSION_SCHEMA>;

// A custom session config from an imported TraceConfig textproto.
const CUSTOM_SESSION_SCHEMA = z.object({
  kind: z.literal('custom'),
  customTraceConfigBase64: z.string(),
  customConfigFileName: z.string().optional(),
});
export type CustomSessionSchema = z.infer<typeof CUSTOM_SESSION_SCHEMA>;

// The schema that holds the settings for a recording session.
// z.union tries custom first (requires kind:'custom'), then falls back to
// probes (where kind defaults to 'probes' for backward compat).
export const RECORD_SESSION_SCHEMA = z.union([
  CUSTOM_SESSION_SCHEMA,
  PROBES_SESSION_SCHEMA,
]);
export type RecordSessionSchema = z.infer<typeof RECORD_SESSION_SCHEMA>;

// The schema for the target selection page.
export const TARGET_SCHEMA = z
  .object({
    platformId: z
      .enum(TARGET_PLATFORMS.map((p) => p.id) as [TargetPlatformId])
      .optional(),
    transportId: z.string().optional(),
    targetId: z.string().optional(),
  })
  .default({});
export type TargetSchema = z.infer<typeof TARGET_SCHEMA>;

// A saved session: name + the full session config (probes or custom).
export const SAVED_SESSION_SCHEMA = z.object({
  name: z.string(),
  config: RECORD_SESSION_SCHEMA.default(() => PROBES_SESSION_SCHEMA.parse({})),
});
export type SavedSessionSchema = z.infer<typeof SAVED_SESSION_SCHEMA>;

// The schema for the root object that holds the whole state of the record
// plugin.
export const RECORD_PLUGIN_SCHEMA = z
  .object({
    target: TARGET_SCHEMA,
    autoOpenTrace: z.boolean().default(true),
    lastSession: RECORD_SESSION_SCHEMA.prefault({}),
    savedSessions: z.array(SAVED_SESSION_SCHEMA).default([]),
    selectedConfigId: z.string().optional(),
    configModified: z.boolean().default(false),
  })
  .prefault({});
export type RecordPluginSchema = z.infer<typeof RECORD_PLUGIN_SCHEMA>;
