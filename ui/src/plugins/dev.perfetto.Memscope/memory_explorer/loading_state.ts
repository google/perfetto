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

import type {App} from '../../../public/app';
import {MEMORY_EXPLORER_ROUTE} from './route';

export interface PendingMemoryCapture {
  readonly title: string;
  readonly fromLive: boolean;
  readonly processName?: string;
  readonly pid?: number;
  readonly deviceName?: string;
  readonly stage: 'stopping' | 'opening' | 'error';
  readonly detail: string;
  readonly capturedBytes?: number;
  readonly error?: string;
}

let pendingCapture: PendingMemoryCapture | undefined;
let lastCapturedFocusProcess: string | undefined;

export function beginPendingMemoryCapture(capture: PendingMemoryCapture): void {
  pendingCapture = capture;
  if (capture.processName) {
    lastCapturedFocusProcess = capture.processName;
  }
}

export function updatePendingMemoryCapture(
  patch: Partial<PendingMemoryCapture>,
): void {
  if (pendingCapture === undefined) return;
  pendingCapture = {...pendingCapture, ...patch};
}

export function failPendingMemoryCapture(error: string): void {
  pendingCapture = {
    title: pendingCapture?.title ?? 'Memory trace',
    fromLive: pendingCapture?.fromLive ?? false,
    processName: pendingCapture?.processName,
    pid: pendingCapture?.pid,
    deviceName: pendingCapture?.deviceName,
    stage: 'error',
    detail: 'Failed to load capture',
    error,
  };
}

export function clearPendingMemoryCapture(): void {
  pendingCapture = undefined;
}

export function getPendingMemoryCapture(): PendingMemoryCapture | undefined {
  return pendingCapture;
}

export function consumeCapturedFocusProcess(): string | undefined {
  const proc = lastCapturedFocusProcess;
  lastCapturedFocusProcess = undefined;
  return proc;
}

export function isMemoryExplorerLoading(app: App): boolean {
  return (
    app.isLoadingTrace ||
    (pendingCapture !== undefined && pendingCapture.stage !== 'error')
  );
}

export function hasPendingMemoryLoadOrError(app: App): boolean {
  return app.isLoadingTrace || pendingCapture !== undefined;
}

// Route to the Memory Explorer loading page with an empty local_cache_key so
// Router.onHashChange does not re-attach a previous trace's cache key.
export function memoryLoadingHref(processName?: string): string {
  const proc =
    processName !== undefined && processName.trim() !== ''
      ? `&process=${encodeURIComponent(processName.trim())}`
      : '';
  return `#!${MEMORY_EXPLORER_ROUTE}?local_cache_key=${proc}`;
}
