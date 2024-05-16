// Copyright (C) 2024 The Android Open Source Project
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

import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {duration, Span, Time, time, TimeSpan} from '../../base/time';

const PLUGIN_ID = 'dev.perfetto.TimelineSync';
const DEFAULT_BROADCAST_CHANNEL = `${PLUGIN_ID}#broadcastChannel`;
const VIEWPORT_UPDATE_THROTTLE_TIME_FOR_SENDING_AFTER_RECEIVING_MS = 1_000;
const BIGINT_PRECISION_MULTIPLIER = 1_000_000_000n;
type ClientId = number;

/**
 * Synchronizes the timeline of 2 or more perfetto traces.
 *
 * To trigger the sync, the command needs to be enabled.
 *
 * This is able to sync between traces recorded at different times, even if
 * their durations don't match. The initial viewport bound for each trace is
 * selected when the enable command is called.
 */
class TimelineSync implements Plugin {
  private _chan?: BroadcastChannel;
  private _ctx?: PluginContextTrace;
  // Attached to broadcast messages to allow other windows to remap viewports.
  private _clientId: ClientId = 0;
  // Used to throttle sending updates after one has been received.
  private _lastReceivedUpdateMillis: number = 0;
  private _lastViewportBounds?: ViewportBounds;

  // Contains the Viewport bounds of this window when it received the first sync
  // message from another one. This is used to re-scale timestamps, so that we
  // can sync 2 (or more!) traces with different length.
  // The initial viewport will be the one visible when the command is enabled.
  private _initialBoundsForSibling = new Map<
    ClientId,
    ViewportBoundsSnapshot
  >();

  private _active: boolean = false;

  onActivate(ctx: PluginContext): void {
    ctx.registerCommand({
      id: `dev.perfetto.SplitScreen#enableTimelineSync`,
      name: 'Enable timeline sync with open windows',
      callback: this.enableTimelineSync.bind(this),
    });
    ctx.registerCommand({
      id: `dev.perfetto.SplitScreen#disableTimelineSync`,
      name: 'Disable timeline sync',
      callback: this.disableTimelineSync.bind(this),
    });
  }

  onDeactivate(_: PluginContext) {
    this.disableTimelineSync();
  }

  async onTraceLoad(ctx: PluginContextTrace) {
    this._ctx = ctx;
  }

  private enableTimelineSync() {
    this._active = true;
    this._initialBoundsForSibling.clear();
    this._clientId = this.generateClientId();
    this._chan = new BroadcastChannel(DEFAULT_BROADCAST_CHANNEL);
    this._chan.onmessage = this.onmessage.bind(this);
    this.scheduleViewportUpdateMessage();
  }

  private disableTimelineSync() {
    this._active = false;
    this._initialBoundsForSibling.clear();
    this._chan?.close();
  }

  private shouldThrottleViewportUpdates() {
    return (
      Date.now() - this._lastReceivedUpdateMillis <=
      VIEWPORT_UPDATE_THROTTLE_TIME_FOR_SENDING_AFTER_RECEIVING_MS
    );
  }

  private scheduleViewportUpdateMessage() {
    if (!this._active) return;
    const currentViewport = this.getCurrentViewportBounds();
    if (
      (!this._lastViewportBounds ||
        !this._lastViewportBounds.equals(currentViewport)) &&
      !this.shouldThrottleViewportUpdates()
    ) {
      this.sendViewportBounds(currentViewport);
      this._lastViewportBounds = currentViewport;
    }
    requestAnimationFrame(this.scheduleViewportUpdateMessage.bind(this));
  }

  private sendViewportBounds(viewportBounds: ViewportBounds) {
    this._chan?.postMessage({
      perfettoSync: {
        cmd: 'MSG_SET_VIEWPORT',
        viewportBounds,
      },
      clientId: this._clientId,
    } as SyncMessage);
  }

  private onmessage(msg: MessageEvent) {
    if (!('perfettoSync' in msg.data)) return;
    const msgData = msg.data as SyncMessage;
    const sync = msgData.perfettoSync;
    switch (sync.cmd) {
      case 'MSG_SET_VIEWPORT':
        this.onViewportSyncReceived(sync.viewportBounds, msgData.clientId);
    }
  }

  private onViewportSyncReceived(
    requestViewBounds: ViewportBounds,
    source: ClientId,
  ) {
    this.cacheSiblingInitialBoundIfNeeded(requestViewBounds, source);
    const remappedViewport = this.remapViewportBounds(
      requestViewBounds,
      source,
    );
    if (!this.getCurrentViewportBounds().equals(remappedViewport)) {
      this._lastReceivedUpdateMillis = Date.now();
      this._lastViewportBounds = remappedViewport;
      this._ctx?.timeline.setViewportTime(
        remappedViewport.start,
        remappedViewport.end,
      );
    }
  }

  private cacheSiblingInitialBoundIfNeeded(
    requestViewBounds: ViewportBounds,
    source: ClientId,
  ) {
    if (!this._initialBoundsForSibling.has(source)) {
      this._initialBoundsForSibling.set(source, {
        thisWindow: this.getCurrentViewportBounds(),
        otherWindow: requestViewBounds,
      });
    }
  }

  private remapViewportBounds(
    otherWindowBounds: ViewportBounds,
    source: ClientId,
  ): ViewportBounds {
    const initialSnapshot = this._initialBoundsForSibling.get(source)!;
    const otherInitial = initialSnapshot.otherWindow;
    const thisInitial = initialSnapshot.thisWindow;

    const [l, r] = this.percentageChange(
      otherInitial.start,
      otherInitial.end,
      otherWindowBounds.start,
      otherWindowBounds.end,
    );
    const thisWindowInitialLength = thisInitial.end - thisInitial.start;

    return new TimeSpan(
      Time.fromRaw(
        thisInitial.start +
          (thisWindowInitialLength * l) / BIGINT_PRECISION_MULTIPLIER,
      ),
      Time.fromRaw(
        thisInitial.start +
          (thisWindowInitialLength * r) / BIGINT_PRECISION_MULTIPLIER,
      ),
    );
  }

  /*
   * Returns the percentage (*1e9) of the starting point inside
   * [initialL, initialR] of [currentL, currentR].
   *
   * A few examples:
   * - If current == initial, the output is expected to be [0,1e9]
   * - If current  is inside the initial -> [>0, < 1e9]
   * - If current is completely outside initial to the right -> [>1e9, >>1e9].
   * - If current is completely outside initial to the left -> [<<0, <0]
   */
  private percentageChange(
    initialL: bigint,
    initialR: bigint,
    currentL: bigint,
    currentR: bigint,
  ): [bigint, bigint] {
    const initialW = initialR - initialL;
    const dtL = currentL - initialL;
    const dtR = currentR - initialL;
    return [this.divide(dtL, initialW), this.divide(dtR, initialW)];
  }

  private divide(a: bigint, b: bigint): bigint {
    // Let's not lose precision
    return (a * BIGINT_PRECISION_MULTIPLIER) / b;
  }

  private getCurrentViewportBounds(): ViewportBounds {
    return this._ctx!.timeline.viewport;
  }

  private generateClientId(): ClientId {
    return Math.floor(Math.random() * 1_000_000);
  }
}

type ViewportBounds = Span<time, duration>;

interface ViewportBoundsSnapshot {
  thisWindow: ViewportBounds;
  otherWindow: ViewportBounds;
}

interface MsgSetViewport {
  cmd: 'MSG_SET_VIEWPORT';
  viewportBounds: ViewportBounds;
}

// In case of new messages, they should be "or-ed" here.
type SyncMessages = MsgSetViewport;

interface SyncMessage {
  perfettoSync: SyncMessages;
  clientId: ClientId;
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: TimelineSync,
};
