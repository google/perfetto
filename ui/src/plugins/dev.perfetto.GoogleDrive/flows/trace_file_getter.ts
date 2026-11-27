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

import type {TraceStream} from '../../../public/stream';
import {describeAuthFailure} from '../dialogs';
import type {DriveClient} from '../google/drive_client';
import type {GoogleOAuthClient} from '../google/oauth_client';

const DRIVE_FILE_SCOPE = 'https://www.googleapis.com/auth/drive.file';

async function readStreamToBlob(stream: TraceStream): Promise<Blob> {
  const chunks: BlobPart[] = [];
  for (;;) {
    const chunk = await stream.readChunk();
    if (chunk.data.byteLength > 0) {
      chunks.push(new Uint8Array(chunk.data));
    }
    if (chunk.eof) break;
  }
  return new Blob(chunks);
}

// Returns an on-demand `getTraceFile` callback for `App.openTraceFromStream()`
// so streamed Google Drive traces remain downloadable (and usable by plugins
// calling `trace.getTraceFile()`) without buffering the trace in JS memory.
export function makeGDriveTraceFileGetter(
  oauth: GoogleOAuthClient,
  drive: DriveClient,
  fileId: string,
  resourceKey?: string,
): () => Promise<Blob> {
  return async () => {
    const auth = await oauth.authenticate([DRIVE_FILE_SCOPE]);
    if (!auth.ok) {
      throw new Error(describeAuthFailure(auth.error));
    }
    const res = await drive.openFile(auth.value, fileId, resourceKey);
    if (!res.ok) {
      throw new Error(
        `Failed to download trace from Google Drive (${res.error.type})`,
      );
    }
    return await readStreamToBlob(res.value);
  };
}
