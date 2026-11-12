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

// The Google Drive credentials are not checked into the repository. They are
// fetched at runtime as a JSON blob from a public GCS bucket (which has CORS
// enabled) so that they may be updated in lockstep across all channels without
// requiring a new build.

const CONFIG_URL =
  'https://storage.googleapis.com/perfetto-misc/gdrive_config.json';
const CONFIG_LOAD_TIMEOUT_MS = 5000;

const GDRIVE_CONFIG_SCHEMA = z.object({
  apiKey: z.string(),
  clientId: z.string(),
  appId: z.string(),
});

export type GoogleDriveConfig = z.infer<typeof GDRIVE_CONFIG_SCHEMA>;

// Fetches and validates the config, throwing on any error (network failure,
// timeout, bad status, or schema mismatch).
export async function loadGDriveConfig(): Promise<GoogleDriveConfig> {
  try {
    const response = await fetch(CONFIG_URL, {
      signal: AbortSignal.timeout(CONFIG_LOAD_TIMEOUT_MS),
    });
    if (!response.ok) {
      throw new Error(
        `Failed to fetch Google Drive config: ${response.status}`,
      );
    }
    const result = GDRIVE_CONFIG_SCHEMA.safeParse(await response.json());
    if (!result.success) {
      throw new Error('Invalid Google Drive config', {cause: result.error});
    }
    return result.data;
  } catch (e) {
    throw new Error('Failed to load Google Drive config', {cause: e});
  }
}
