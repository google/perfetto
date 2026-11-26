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

import {reportError} from '../base/logging';
import {openTraceFiles} from '../core/open_trace_files';

export interface LaunchParams {
  readonly targetURL?: string;
  readonly files: readonly FileSystemHandle[];
}

export type LaunchConsumer = (params: LaunchParams) => void | Promise<void>;

export interface LaunchQueue {
  setConsumer(consumer: LaunchConsumer): void;
}

declare global {
  interface Window {
    readonly launchQueue?: LaunchQueue;
  }
}

function isFileHandle(
  handle: FileSystemHandle,
): handle is FileSystemFileHandle {
  return handle.kind === 'file';
}

export function installFileLaunchHandler(): void {
  if (window.launchQueue === undefined) {
    return;
  }

  window.launchQueue.setConsumer(async (launchParams: LaunchParams) => {
    const fileHandles = launchParams.files.filter(isFileHandle);
    if (fileHandles.length === 0) {
      return;
    }

    try {
      const files = await Promise.all(
        fileHandles.map((handle) => handle.getFile()),
      );
      openTraceFiles(files);
    } catch (err) {
      reportError(err);
    }
  });
}
