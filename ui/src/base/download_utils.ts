// Copyright (C) 2025 The Android Open Source Project
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

// A representation of some content that can be downloaded.
type Content = string | Uint8Array | File | Blob;

export type FilePickerOptions = {
  types?: FilePickerAcceptType[];
};

/**
 * Downloads some content to a file.
 *
 * @param args The arguments for the download.
 * @param args.content The content to download.
 * @param args.fileName The name of the file to download.
 * @param args.mimeType The MIME type of the content.
 * @param args.filePicker If provided, the file picker will be used to save the file.
 */
export async function download({
  content,
  fileName,
  mimeType,
  filePicker,
}: {
  content: Content;
  fileName: string;
  mimeType?: string;
  filePicker?: FilePickerOptions;
}) {
  let blob: Blob;
  if (content instanceof File || content instanceof Blob) {
    blob = content;
  } else {
    const inferredMimeType =
      typeof content === 'string' ? 'text/plain' : 'application/octet-stream';
    blob = new Blob([content], {
      type: mimeType ?? inferredMimeType,
    });
  }

  if (filePicker && exists(window.showSaveFilePicker)) {
    try {
      const handle = await window.showSaveFilePicker({
        suggestedName: fileName,
        types: filePicker.types,
      });
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
    } catch (e) {
      console.error(e);
      // The user pressed cancel, do nothing.
    }
  } else {
    // No file picker available or requested, fallback to the old method.
    using url = createUrl(blob);
    downloadUrl({url: url.value, fileName});
  }
}

/**
 * Initiate download of a resource identified by a URL.
 *
 * @param args The arguments for the download.
 * @param args.fileName The name of the file to download.
 * @param args.url The URL of the resource to download.
 */
export function downloadUrl({fileName, url}: {fileName: string; url: string}) {
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  a.target = '_blank';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
}

function createUrl(blob: Blob): Disposable & {readonly value: string} {
  const url = URL.createObjectURL(blob);
  return {
    [Symbol.dispose]: () => URL.revokeObjectURL(url),
    value: url,
  };
}
