// Copyright (C) 2023 The Android Open Source Project
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

import {exists} from './utils';

declare global {
  interface Window {
    showSaveFilePicker: (
      options?: SaveFilePickerOptions,
    ) => Promise<FileSystemFileHandle>;
  }

  interface SaveFilePickerOptions {
    suggestedName?: string;
    types?: FilePickerAcceptType[];
    excludeAcceptAllOption?: boolean;
  }

  interface FilePickerAcceptType {
    description?: string;
    accept: Record<string, string[]>;
  }
}

// Initiate download of a resource identified by |url| into |filename|.
export function downloadUrl(fileName: string, url: string) {
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  a.target = '_blank';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// Initiate download of |data| a file with a given name.
export function downloadData(fileName: string, ...data: Uint8Array[]) {
  const blob = new Blob(data, {type: 'application/octet-stream'});
  const url = URL.createObjectURL(blob);
  downloadUrl(fileName, url);
}

/**
 * Downloads a url or a File/Blob using the file picker.
 * The file picker is omitted if the source is a URL to avoid re-fetching
 * URL-based traces.
 * @param fileName the suggested file name.
 * @param data the url or File or blob
 */
export async function downloadFileOrUrlWithFilePicker(
  fileName: string,
  data: string | File | Blob,
) {
  if (typeof data === 'string') {
    return downloadUrl(fileName, data);
  }
  const hasFilePicker = exists(window.showSaveFilePicker);
  if (!hasFilePicker) {
    const url = URL.createObjectURL(data);
    return downloadUrl(fileName, url);
  }

  let handle: FileSystemFileHandle;
  try {
    handle = await window.showSaveFilePicker({
      suggestedName: fileName,
      types: [
        {
          description: 'Perfetto trace',
          accept: {'*/*': ['.pftrace']},
        },
      ],
    });
  } catch (e) {
    console.error(e);
    return; // The user pressed cancel.
  }

  const writable = await handle.createWritable();
  await writable.write(data);
  await writable.close();
}
