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

import m from 'mithril';

export type ActionState = 'idle' | 'working' | 'done';

export class ActionButtonHelper {
  private _state: ActionState = 'idle';
  private doneTimeoutId: ReturnType<typeof setTimeout> | undefined;
  private loadingTimeoutId: ReturnType<typeof setTimeout> | undefined;
  private readonly timeout: number;
  private readonly loadingDelay: number;

  constructor(timeout = 2000, loadingDelay = 100) {
    this.timeout = timeout;
    this.loadingDelay = loadingDelay;
  }

  get state(): ActionState {
    return this._state;
  }

  async execute(action: () => Promise<void>) {
    clearTimeout(this.doneTimeoutId);
    clearTimeout(this.loadingTimeoutId);
    this.doneTimeoutId = undefined;
    this.loadingTimeoutId = undefined;

    // Set to working after a delay
    this.loadingTimeoutId = setTimeout(() => {
      this._state = 'working';
      m.redraw();
    }, this.loadingDelay);

    try {
      await action();

      // Clear the loading timeout in case action completed quickly
      clearTimeout(this.loadingTimeoutId);
      this.loadingTimeoutId = undefined;

      this._state = 'done';
      m.redraw();

      this.doneTimeoutId = setTimeout(() => {
        this._state = 'idle';
        m.redraw();
      }, this.timeout);
    } catch (e) {
      // If the action fails, we should probably reset to idle or show error.
      // For now, let's just reset to idle immediately or maybe after a short delay?
      // The original code didn't handle errors explicitly, but async functions reject.
      // If it fails, we probably don't want to show "Copied".
      // Let's just log and reset.
      console.error(e);
      clearTimeout(this.loadingTimeoutId);
      this._state = 'idle';
      m.redraw();
      throw e; // Re-throw so caller can handle if needed
    }
  }
}
