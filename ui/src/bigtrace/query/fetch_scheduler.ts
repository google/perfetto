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

import {forwardAbort} from './abort_utils';

// The grid asks for a new window on every scroll tick, so a drag would
// otherwise be one round trip per tick. Wait for the offset to settle and
// send one request for where the user actually stopped.
export const FETCH_DEBOUNCE_MS = 150;

// Decides when a grid data source actually talks to the backend: it coalesces
// a burst of window requests into one, and cancels the request a newer one
// replaces. Both data sources drive their fetches through it, so the two grids
// can't drift apart on behaviour this fiddly.
//
// A request is identified by a caller-supplied key covering everything that
// would change the reply (window, sort, filter, projection, and whatever else
// the caller varies). An identical request already queued or in flight is
// never repeated — which matters because a data source's "we still need the
// first page" condition stays true until a fetch succeeds, and every fetch
// triggers a redraw that re-evaluates it.
export class FetchScheduler {
  private pendingTimer?: number;
  private pendingKey?: string;
  private pendingRun?: (controller: AbortController) => Promise<void>;
  private inFlight?: AbortController;
  private inFlightKey?: string;
  private running = false;

  // `ownerSignal` kills everything at once when the owner goes away.
  // `onChange` is called whenever `isPending` may have changed.
  constructor(
    private readonly ownerSignal?: AbortSignal,
    private readonly onChange: () => void = () => {},
  ) {}

  // True while a request is in flight or still waiting out the debounce, so
  // the grid doesn't read as settled between the last tick and the request.
  get isPending(): boolean {
    return this.running || this.pendingTimer !== undefined;
  }

  // Whether the reply a caller is holding belongs to the request still being
  // waited for. A reply that raced its own abort must be dropped, or it puts
  // back the page the user scrolled away from.
  isCurrent(controller: AbortController): boolean {
    return this.inFlight === controller;
  }

  // Ask for `run` under `key`. `immediate` skips the wait — for the first
  // window, which has nothing on screen behind it.
  schedule(
    key: string,
    immediate: boolean,
    run: (controller: AbortController) => Promise<void>,
  ): void {
    if (this.ownerSignal?.aborted) return;
    // Already on its way, or already queued — asking again changes nothing.
    if (key === this.pendingKey) return;
    if (this.running && key === this.inFlightKey) return;
    this.clearTimer();
    if (immediate) {
      void this.start(key, run);
      return;
    }
    this.pendingKey = key;
    this.pendingRun = run;
    this.pendingTimer = window.setTimeout(() => {
      this.pendingTimer = undefined;
      this.pendingKey = undefined;
      const pending = this.pendingRun;
      this.pendingRun = undefined;
      if (pending !== undefined) void this.start(key, pending);
    }, FETCH_DEBOUNCE_MS);
    this.onChange();
  }

  // Run `run` now, dropping anything queued and superseding anything in
  // flight. For explicit refreshes, where waiting would be wrong.
  async runNow(
    run: (controller: AbortController) => Promise<void>,
  ): Promise<void> {
    this.clearTimer();
    await this.start(undefined, run);
  }

  // ----- Internals -----

  private clearTimer(): void {
    if (this.pendingTimer !== undefined) {
      window.clearTimeout(this.pendingTimer);
      this.pendingTimer = undefined;
    }
    this.pendingKey = undefined;
    this.pendingRun = undefined;
  }

  private async start(
    key: string | undefined,
    run: (controller: AbortController) => Promise<void>,
  ): Promise<void> {
    if (this.ownerSignal?.aborted) return;
    // Supersede whatever is still running: its page is one the user has
    // already moved off, and because the fetch is awaited it would otherwise
    // sit in front of the page they are waiting for.
    this.inFlight?.abort();
    const controller = new AbortController();
    this.inFlight = controller;
    this.inFlightKey = key;
    this.running = true;
    const detach =
      this.ownerSignal !== undefined
        ? forwardAbort(this.ownerSignal, controller)
        : () => {};
    this.onChange();
    try {
      await run(controller);
    } finally {
      detach();
      // A superseded request must not report the newer one as finished.
      if (this.inFlight === controller) {
        this.inFlight = undefined;
        this.inFlightKey = undefined;
        this.running = false;
      }
      this.onChange();
    }
  }
}
