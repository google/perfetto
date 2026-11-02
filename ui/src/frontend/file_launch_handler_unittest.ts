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

import {vi} from 'vitest';
import {reportError} from '../base/logging';
import {openTraceFiles} from '../core/open_trace_files';
import {
  installFileLaunchHandler,
  type LaunchConsumer,
  type LaunchQueue,
} from './file_launch_handler';

vi.mock('../core/open_trace_files', () => ({
  openTraceFiles: vi.fn(),
}));

vi.mock('../base/logging', () => ({
  reportError: vi.fn(),
}));

function setLaunchQueue(queue: LaunchQueue | undefined): void {
  Object.defineProperty(window, 'launchQueue', {
    value: queue,
    configurable: true,
    writable: true,
  });
}

function makeFileHandle(file: File): FileSystemFileHandle {
  return {
    kind: 'file',
    name: file.name,
    getFile: () => Promise.resolve(file),
    isSameEntry: () => Promise.resolve(false),
    createWritable: () => Promise.reject(new Error('not implemented')),
  };
}

describe('fileLaunchHandler', () => {
  afterEach(() => {
    setLaunchQueue(undefined);
    vi.clearAllMocks();
  });

  test('is a no-op when window.launchQueue is unavailable', () => {
    setLaunchQueue(undefined);
    expect(() => installFileLaunchHandler()).not.toThrow();
    expect(openTraceFiles).not.toHaveBeenCalled();
  });

  test('ignores launches with no files', async () => {
    let consumer: LaunchConsumer | undefined;
    setLaunchQueue({
      setConsumer: (cb) => {
        consumer = cb;
      },
    });

    installFileLaunchHandler();
    expect(consumer).toBeDefined();
    await consumer?.({files: []});

    expect(openTraceFiles).not.toHaveBeenCalled();
  });

  test('opens single and multiple launched trace files', async () => {
    let consumer: LaunchConsumer | undefined;
    setLaunchQueue({
      setConsumer: (cb) => {
        consumer = cb;
      },
    });

    const f1 = new File(['trace1'], 'a.pftrace');
    const f2 = new File(['trace2'], 'b.perfetto-trace');
    const dirHandle: FileSystemHandle = {
      kind: 'directory',
      name: 'ignored-dir',
      isSameEntry: () => Promise.resolve(false),
    };

    installFileLaunchHandler();
    await consumer?.({
      files: [makeFileHandle(f1), dirHandle, makeFileHandle(f2)],
    });

    expect(openTraceFiles).toHaveBeenCalledTimes(1);
    expect(openTraceFiles).toHaveBeenCalledWith([f1, f2]);
  });

  test('reports errors when reading a launched file fails', async () => {
    let consumer: LaunchConsumer | undefined;
    setLaunchQueue({
      setConsumer: (cb) => {
        consumer = cb;
      },
    });

    const readError = new Error('permission denied');
    const brokenHandle: FileSystemFileHandle = {
      kind: 'file',
      name: 'broken.pftrace',
      getFile: () => Promise.reject(readError),
      isSameEntry: () => Promise.resolve(false),
      createWritable: () => Promise.reject(new Error('not implemented')),
    };

    installFileLaunchHandler();
    await consumer?.({files: [brokenHandle]});

    expect(openTraceFiles).not.toHaveBeenCalled();
    expect(reportError).toHaveBeenCalledWith(readError);
  });
});
