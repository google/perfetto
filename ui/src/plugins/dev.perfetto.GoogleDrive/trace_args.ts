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
import {isValidDriveId} from './google/drive_client';

export const GDRIVE_PLUGIN_ID = 'dev.perfetto.GoogleDrive';

// What this plugin attaches to a trace it opens from Drive, through the
// pluginArgs of openTraceFromBuffer(), and reads back in onTraceLoad(). It
// travels with that one trace, so it cannot end up on a different one.
//
// A trace handed to the UI by another site (via postMessage) can carry plugin
// args too, so what arrives is validated like any other untrusted input.
const TRACE_ARGS_SCHEMA = z.object({
  // The Drive file ID the trace was opened from.
  fileId: z.string().refine(isValidDriveId),
});

// The pluginArgs to pass to openTraceFromBuffer() for a trace opened from the
// Drive file `fileId`.
export function gdriveTraceArgs(fileId: string): {
  [pluginId: string]: {[key: string]: unknown};
} {
  return {[GDRIVE_PLUGIN_ID]: {fileId}};
}

// The Drive file ID a trace was opened from, given the args onTraceLoad()
// received, or undefined if it was not opened from Drive (or the args are not
// valid).
export function parseGDriveTraceArgs(args: unknown): string | undefined {
  const parsed = TRACE_ARGS_SCHEMA.safeParse(args);
  return parsed.success ? parsed.data.fileId : undefined;
}
