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

import m from 'mithril';
import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {Time, TimeSpan} from '../../base/time';
import {redrawModal, showModal} from '../../widgets/modal';
import {assertExists} from '../../base/logging';

const PLUGIN_ID = 'dev.perfetto.TimelineSync';
const DEFAULT_BROADCAST_CHANNEL = `${PLUGIN_ID}#broadcastChannel`;
const VIEWPORT_UPDATE_THROTTLE_TIME_FOR_SENDING_AFTER_RECEIVING_MS = 1_000;
const BIGINT_PRECISION_MULTIPLIER = 1_000_000_000n;
const ADVERTISE_PERIOD_MS = 10_000;
const DEFAULT_SESSION_ID = 1;
type ClientId = number;
type SessionId = number;

/**
 * Synchronizes the timeline of 2 or more perfetto traces.
 *
 * To trigger the sync, the command needs to be executed on one tab. It will
 * prompt a list of other tabs to keep in sync. Each tab advertise itself
 * on a BroadcastChannel upon trace load.
 *
 * This is able to sync between traces recorded at different times, even if
 * their durations don't match. The initial viewport bound for each trace is
 * selected when the enable command is called.
 */
class TimelineSync implements PerfettoPlugin {
  private _chan?: BroadcastChannel;
  private _ctx?: Trace;
  private _traceLoadTime = 0;
  // Attached to broadcast messages to allow other windows to remap viewports.
  private readonly _clientId: ClientId = Math.floor(Math.random() * 1_000_000);
  // Used to throttle sending updates after one has been received.
  private _lastReceivedUpdateMillis: number = 0;
  private _lastViewportBounds?: ViewportBounds;
  private _advertisedClients = new Map<ClientId, ClientInfo>();
  private _sessionId: SessionId = 0;
  // Used when the url passes ?dev.perfetto.TimelineSync:enable to auto-enable
  // timeline sync on trace load.
  private _sessionidFromUrl: SessionId = 0;

  // Contains the Viewport bounds of this window when it received the first sync
  // message from another one. This is used to re-scale timestamps, so that we
  // can sync 2 (or more!) traces with different length.
  // The initial viewport will be the one visible when the command is enabled.
  private _initialBoundsForSibling = new Map<
    ClientId,
    ViewportBoundsSnapshot
  >();

  onActivate(ctx: App): void {
    ctx.commands.registerCommand({
      id: `dev.perfetto.SplitScreen#enableTimelineSync`,
      name: 'Enable timeline sync with other Perfetto UI tabs',
      callback: () => this.showTimelineSyncDialog(),
    });
    ctx.commands.registerCommand({
      id: `dev.perfetto.SplitScreen#disableTimelineSync`,
      name: 'Disable timeline sync',
      callback: () => this.disableTimelineSync(this._sessionId),
    });
    ctx.commands.registerCommand({
      id: `dev.perfetto.SplitScreen#toggleTimelineSync`,
      name: 'Toggle timeline sync with other PerfettoUI tabs',
      callback: () => this.toggleTimelineSync(),
      defaultHotkey: 'Mod+Alt+S',
    });

    // Start advertising this tab. This allows the command run in other
    // instances to discover us.
    this._chan = new BroadcastChannel(DEFAULT_BROADCAST_CHANNEL);
    this._chan.onmessage = this.onmessage.bind(this);
    document.addEventListener('visibilitychange', () => this.advertise());
    window.addEventListener('focus', () => this.advertise());
    setInterval(() => this.advertise(), ADVERTISE_PERIOD_MS);

    // Allow auto-enabling of timeline sync from the URI. The user can
    // optionally specify a session id, otherwise we just use a default one.
    const m = /dev.perfetto.TimelineSync:enable(=\d+)?/.exec(location.hash);
    if (m !== null) {
      this._sessionidFromUrl = m[1]
        ? parseInt(m[1].substring(1))
        : DEFAULT_SESSION_ID;
    }
  }

  async onTraceLoad(ctx: Trace) {
    this._ctx = ctx;
    this._traceLoadTime = Date.now();
    this.advertise();
    if (this._sessionidFromUrl !== 0) {
      this.enableTimelineSync(this._sessionidFromUrl);
    }
  }

  async onTraceUnload(_: Trace) {
    this.disableTimelineSync(this._sessionId);
    this._ctx = undefined;
  }

  private advertise() {
    if (this._ctx === undefined) return; // Don't advertise if no trace loaded.
    this._chan?.postMessage({
      perfettoSync: {
        cmd: 'MSG_ADVERTISE',
        title: document.title,
        traceLoadTime: this._traceLoadTime,
      },
      clientId: this._clientId,
    } as SyncMessage);
  }

  private toggleTimelineSync() {
    if (this._sessionId === 0) {
      this.showTimelineSyncDialog();
    } else {
      this.disableTimelineSync(this._sessionId);
    }
  }

  private showTimelineSyncDialog() {
    let clientsSelect: HTMLSelectElement;

    // This nested function is invoked when the modal dialog buton is pressed.
    const doStartSession = () => {
      // Disable any prior session.
      this.disableTimelineSync(this._sessionId);
      const selectedClients = new Array<ClientId>();
      const sel = assertExists(clientsSelect).selectedOptions;
      for (let i = 0; i < sel.length; i++) {
        const clientId = parseInt(sel[i].value);
        if (!isNaN(clientId)) selectedClients.push(clientId);
      }
      selectedClients.push(this._clientId); // Always add ourselves.
      this._sessionId = Math.floor(Math.random() * 1_000_000);
      this._chan?.postMessage({
        perfettoSync: {
          cmd: 'MSG_SESSION_START',
          sessionId: this._sessionId,
          clients: selectedClients,
        },
        clientId: this._clientId,
      } as SyncMessage);
      this._initialBoundsForSibling.clear();
      this.scheduleViewportUpdateMessage();
    };

    // The function below is called on every mithril render pass. It's important
    // that this function re-computes the list of other clients on every pass.
    // The user will go to other tabs (which causes an advertise due to the
    // visibilitychange listener) and come back on here while the modal dialog
    // is still being displayed.
    const renderModalContents = (): m.Children => {
      const children: m.Children = [];
      this.purgeInactiveClients();
      const clients = Array.from(this._advertisedClients.entries());
      clients.sort((a, b) => b[1].traceLoadTime - a[1].traceLoadTime);
      for (const [clientId, info] of clients) {
        const opened = new Date(info.traceLoadTime).toLocaleTimeString();
        const attrs: {value: number; selected?: boolean} = {value: clientId};
        if (this._advertisedClients.size === 1) {
          attrs.selected = true;
        }
        children.push(m('option', attrs, `${info.title} (${opened})`));
      }
      return m(
        'div',
        {style: 'display: flex;  flex-direction: column;'},
        m(
          'div',
          'Select the perfetto UI tab(s) you want to keep in sync ' +
            '(Ctrl+Click to select many).',
        ),
        m(
          'div',
          "If you don't see the trace listed here, temporarily focus the " +
            'corresponding browser tab and then come back here.',
        ),
        m(
          'select[multiple=multiple][size=8]',
          {
            oncreate: (vnode: m.VnodeDOM) => {
              clientsSelect = vnode.dom as HTMLSelectElement;
            },
          },
          children,
        ),
      );
    };

    showModal({
      title: 'Synchronize timeline across several tabs',
      content: renderModalContents,
      buttons: [
        {
          primary: true,
          text: `Synchronize timelines`,
          action: doStartSession,
        },
      ],
    });
  }

  private enableTimelineSync(sessionId: SessionId) {
    if (sessionId === this._sessionId) return; // Already in this session id.
    this._sessionId = sessionId;
    this._initialBoundsForSibling.clear();
    this.scheduleViewportUpdateMessage();
  }

  private disableTimelineSync(sessionId: SessionId, skipMsg = false) {
    if (sessionId !== this._sessionId || this._sessionId === 0) return;

    if (!skipMsg) {
      this._chan?.postMessage({
        perfettoSync: {
          cmd: 'MSG_SESSION_STOP',
          sessionId: this._sessionId,
        },
        clientId: this._clientId,
      } as SyncMessage);
    }
    this._sessionId = 0;
    this._initialBoundsForSibling.clear();
  }

  private shouldThrottleViewportUpdates() {
    return (
      Date.now() - this._lastReceivedUpdateMillis <=
      VIEWPORT_UPDATE_THROTTLE_TIME_FOR_SENDING_AFTER_RECEIVING_MS
    );
  }

  private scheduleViewportUpdateMessage() {
    if (!this.active) return;
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
        sessionId: this._sessionId,
        viewportBounds,
      },
      clientId: this._clientId,
    } as SyncMessage);
  }

  private onmessage(msg: MessageEvent) {
    if (this._ctx === undefined) return; // Trace unloaded
    if (!('perfettoSync' in msg.data)) return;
    const msgData = msg.data as SyncMessage;
    const sync = msgData.perfettoSync;
    switch (sync.cmd) {
      case 'MSG_ADVERTISE':
        if (msgData.clientId !== this._clientId) {
          this._advertisedClients.set(msgData.clientId, {
            title: sync.title,
            traceLoadTime: sync.traceLoadTime,
            lastHeartbeat: Date.now(),
          });
          this.purgeInactiveClients();
          redrawModal();
        }
        break;
      case 'MSG_SESSION_START':
        if (sync.clients.includes(this._clientId)) {
          this.enableTimelineSync(sync.sessionId);
        }
        break;
      case 'MSG_SESSION_STOP':
        this.disableTimelineSync(sync.sessionId, /* skipMsg= */ true);
        break;
      case 'MSG_SET_VIEWPORT':
        if (sync.sessionId === this._sessionId) {
          this.onViewportSyncReceived(sync.viewportBounds, msgData.clientId);
        }
        break;
    }
  }

  private onViewportSyncReceived(
    requestViewBounds: ViewportBounds,
    source: ClientId,
  ) {
    if (!this.active) return;
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
    return this._ctx!.timeline.visibleWindow.toTimeSpan();
  }

  private purgeInactiveClients() {
    const now = Date.now();
    const TIMEOUT_MS = 30_000;
    for (const [clientId, info] of this._advertisedClients.entries()) {
      if (now - info.lastHeartbeat < TIMEOUT_MS) continue;
      this._advertisedClients.delete(clientId);
    }
  }

  private get active() {
    return this._sessionId !== 0;
  }
}

type ViewportBounds = TimeSpan;

interface ViewportBoundsSnapshot {
  thisWindow: ViewportBounds;
  otherWindow: ViewportBounds;
}

interface MsgSetViewport {
  cmd: 'MSG_SET_VIEWPORT';
  sessionId: SessionId;
  viewportBounds: ViewportBounds;
}

interface MsgAdvertise {
  cmd: 'MSG_ADVERTISE';
  title: string;
  traceLoadTime: number;
}

interface MsgSessionStart {
  cmd: 'MSG_SESSION_START';
  sessionId: SessionId;
  clients: ClientId[];
}

interface MsgSessionStop {
  cmd: 'MSG_SESSION_STOP';
  sessionId: SessionId;
}

// In case of new messages, they should be "or-ed" here.
type SyncMessages =
  | MsgSetViewport
  | MsgAdvertise
  | MsgSessionStart
  | MsgSessionStop;

interface SyncMessage {
  perfettoSync: SyncMessages;
  clientId: ClientId;
}

interface ClientInfo {
  title: string;
  lastHeartbeat: number; // Datetime.now() of the last MSG_ADVERTISE.
  traceLoadTime: number; // Datetime.now() of the onTraceLoad().
}

export const plugin: PluginDescriptor = {
  pluginId: PLUGIN_ID,
  plugin: TimelineSync,
};
