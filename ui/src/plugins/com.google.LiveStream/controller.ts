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

// The app-level side of the plugin: the HUD, the settings, and opening live
// traces. The HUD, a side panel tab or a row above the app, lives as long as
// the page, so it shows from page open and stays while a target switch
// replaces the trace:
//
//   page open ──► LiveController ──openLiveTrace──► trace ──► LiveSession
//                   │  HUD in the panel or top row ◄──attach(model)─┘
//                   └─ opens the first trace, switches and reconnects, and
//                      shows their state until the new session attaches

import m from 'mithril';
import {getErrorMessage} from '../../base/errors';
import type {App} from '../../public/app';
import {
  LiveHudState,
  type LiveHudLayout,
  renderLiveStreamHud,
  renderOtherTraceNote,
} from './hud';
import {openLiveTrace, type OpenObserver, PLUGIN_ID} from './opener';
import {type LiveSettings, registerLiveSettings} from './settings';
import type {LiveStatus} from './status';
import type {Target} from './wire';

/** A live session as the HUD sees it: its state and actions. */
export interface LiveSessionModel {
  state(): LiveSessionState;
  toggleFollow(): void;
  togglePause(): void;
  download(): void;
  refreshTargets(): void;
  // The user dismissed the popout of `error`. Ends one-shot errors, e.g. of
  // a failed history load, which nothing else ends.
  dismissError(error: string): void;
}

/** What a live session shows in the HUD. */
export interface LiveSessionState {
  // Before debouncing, see LiveHudState.syncStatus().
  readonly status: LiveStatus;
  readonly error?: string;
  readonly target: Target;
  readonly paused: boolean;
}

/** What live sessions use of the app-level side, see LiveController. */
export interface LiveHost {
  readonly settings: LiveSettings;
  // Whether to follow the playhead; kept across traces.
  following: boolean;
  // Shows `model` in the HUD until disposed.
  attach(model: LiveSessionModel): Disposable;
  setTargets(targets: readonly Target[]): void;
  // Updates the displayed status and texts, e.g. after time-based changes.
  refreshStatus(): void;
  // Reopens `target` with a new seed, e.g. after a server restart, unless
  // another open is pending.
  reopen(target: string, epochRetry: boolean): void;
}

/** Where the HUD shows: a side panel tab, or a row above the app. */
export type LiveHudPlacement = 'panel' | 'top';

/** The placement of route argument `hud`: "top", else the panel. */
export function hudPlacement(arg: unknown): LiveHudPlacement {
  return arg === 'top' ? 'top' : 'panel';
}

const FOLLOW_COMMAND = 'com.google.LiveStream#ToggleFollowLive';
const HUD_TAB = `${PLUGIN_ID}#hud`;

// An open of a live trace, at page open or in place of the current one.
interface Opening {
  readonly spec: string;
  readonly abort: AbortController;
  // Once the server resolved the spec.
  target?: Target;
  // The latest failure, while retrying or once failed.
  error?: string;
  // Whether the open gave up.
  failed: boolean;
  // Whether its trace loads, which replaces the current session.
  loading: boolean;
}

type ShownState = Omit<LiveSessionState, 'target'> & {readonly target?: Target};

/**
 * Shows the HUD at `placement` for the page's lifetime, and opens live
 * traces. Sessions attach to it. While an open is pending or failed, the HUD
 * shows its state: buffering, or an error while the server or its target
 * fails, or once the open failed. The error of a failed switch shows until
 * dismissed or the next open. While another trace is open, the tab says so
 * and the top row hides.
 */
export class LiveController implements LiveHost {
  readonly settings: LiveSettings;
  following = true;
  private readonly hudState: LiveHudState;
  private targets: readonly Target[] = [];
  private model?: LiveSessionModel;
  // The target of the latest session, shown until the next one attaches.
  private lastTarget?: Target;
  // The text of the latest error, for a popout that outlasts it by the
  // status delay.
  private lastError = '';
  // The error at the latest refreshStatus(), to redraw on changes.
  private shownError = '';
  private opening?: Opening;
  // Whether a trace that isn't live is open, see showOtherTrace().
  private otherTrace = false;
  private followCommand?: Disposable;

  constructor(
    private readonly app: App,
    placement: LiveHudPlacement = 'panel',
  ) {
    this.settings = registerLiveSettings(app.settings);
    this.hudState = new LiveHudState(() => this.redraw());
    this.hudState.listen();
    // App-level, so that trace switches keep the HUD. Never disposed: it
    // lives as long as the page.
    if (placement === 'top') {
      mountTopRow(app, () => this.render('row'));
    } else {
      app.sidePanel.registerTab({
        uri: HUD_TAB,
        title: 'Live Stream',
        icon: 'sensors',
        render: () => this.render('panel'),
      });
      app.sidePanel.showTab(HUD_TAB);
    }
    this.registerFollowCommand();
    this.redraw();
  }

  /**
   * Opens the live trace of server spec `spec`, in place of the current one,
   * retrying while the server is unavailable. Replaces a pending open.
   */
  open(spec: string, epochRetry = false): void {
    this.opening?.abort.abort();
    const opening: Opening = {
      spec,
      abort: new AbortController(),
      failed: false,
      loading: false,
    };
    this.opening = opening;
    this.update();

    const {signal} = opening.abort;
    const historySec = Number(this.settings.retention.get()) * 60;
    // Each step that succeeds ends the error of a failed attempt.
    const observer: OpenObserver = {
      onTargets: (resp) => {
        opening.target = resp.selected;
        opening.error = undefined;
        this.targets = resp.targets;
        this.update();
      },
      onRetry: (message, kind) => {
        // Waiting for a target's first data is buffering, not an error.
        opening.error = kind === 'waiting' ? undefined : message;
        this.update();
      },
      beforeOpen: (target) => {
        opening.loading = true;
        opening.error = undefined;
        // Before the trace, and so the page title, changes: observers of
        // the page see the URL of the new title.
        setUrlTarget(target.id);
        this.update();
      },
    };
    openLiveTrace(this.app, spec, signal, observer, {
      historySec,
      epochRetry,
    }).catch((e) => {
      if (e === signal.reason || this.opening !== opening) return;
      opening.error = `Cannot open ${spec}: ${getErrorMessage(e)}`;
      opening.failed = true;
      this.update();
    });
  }

  attach(model: LiveSessionModel): Disposable {
    this.model = model;
    this.otherTrace = false;
    this.registerFollowCommand();
    // The open of this trace succeeded. A later open goes on.
    if (this.opening?.loading) this.opening = undefined;
    this.update();
    return {
      [Symbol.dispose]: () => {
        if (this.model !== model) return;
        this.lastTarget = model.state().target;
        this.model = undefined;
        this.update();
      },
    };
  }

  setTargets(targets: readonly Target[]): void {
    this.targets = targets;
    this.redraw();
  }

  refreshStatus(): void {
    const {changed} = this.hudState.syncStatus(() => this.state().status);

    // The error changes without the status too, e.g. as the server's reason
    // to fail does.
    const {error = ''} = this.state();
    if (!changed && error === this.shownError) return;
    this.shownError = error;
    this.redraw();
  }

  reopen(target: string, epochRetry: boolean): void {
    if (this.opening !== undefined && !this.opening.failed) return;
    this.open(target, epochRetry);
  }

  /**
   * Hides the HUD's controls and frees Space while a trace that isn't live is
   * open, e.g. one opened from a file. A live trace that opens shows them
   * again.
   */
  showOtherTrace(): void {
    this.otherTrace = true;
    this.followCommand?.[Symbol.dispose]();
    this.followCommand = undefined;
    this.hudState.closeMenus();
    this.redraw();
  }

  // The state of a pending or failed open, else the session's.
  private state(): ShownState {
    const session = this.model?.state();
    const opening = this.opening;
    if (opening === undefined) {
      return (
        session ?? {status: 'buffering', target: this.lastTarget, paused: false}
      );
    }

    // A failed switch leaves the session's trace, and so its target.
    const opened = opening.failed ? session?.target : undefined;
    return {
      status: opening.error === undefined ? 'buffering' : 'error',
      error: opening.error,
      target: opened ?? opening.target ?? session?.target ?? this.lastTarget,
      paused: session?.paused ?? false,
    };
  }

  private render(layout: LiveHudLayout): m.Children {
    // The HUD of another trace shows only while a live one opens. The panel's
    // tab stays, so it says why it's empty.
    if (this.otherTrace && this.opening === undefined) {
      return layout === 'panel' ? renderOtherTraceNote() : undefined;
    }
    const state = this.state();
    if (state.error !== undefined) this.lastError = state.error;
    const {status} = this.hudState.syncStatus(() => this.state().status);
    return renderLiveStreamHud({
      layout,
      status,
      error: state.error ?? this.lastError,
      targets: this.targets,
      target: state.target,
      following: this.following,
      paused: state.paused,
      settings: this.settings,
      hudState: this.hudState,
      onOpenTarget: (spec) => this.openTarget(spec),
      onTargetMenuOpen: () => this.model?.refreshTargets(),
      onToggleFollow: () => this.toggleFollow(),
      onTogglePause: () => this.model?.togglePause(),
      onReconnect: () => this.reconnect(),
      onDownload: () => this.model?.download(),
      onDismissError: (error) => this.dismissError(error),
    });
  }

  // Picking the open target cancels a pending switch.
  private openTarget(spec: string) {
    if (spec !== this.model?.state().target.id) {
      this.open(spec);
      return;
    }
    if (this.opening === undefined) return;
    this.opening.abort.abort();
    this.opening = undefined;
    this.update();
  }

  // Reopens the latest picked target: a pending or failed switch's, else the
  // open one.
  private reconnect() {
    const spec =
      this.opening?.spec ??
      this.model?.state().target.id ??
      this.lastTarget?.id;
    if (spec !== undefined) this.open(spec);
  }

  // A failed switch ends once dismissed. A pending open goes on, and a failed
  // first open stays the state: only its popout hides.
  private dismissError(error: string) {
    const opening = this.opening;
    if (opening?.error !== error) {
      this.model?.dismissError(error);
      return;
    }
    if (!opening.failed || this.model === undefined) return;
    this.opening = undefined;
    this.update();
  }

  private toggleFollow() {
    if (this.model !== undefined) {
      this.model.toggleFollow();
      return;
    }
    this.following = !this.following;
    this.redraw();
  }

  // Space toggles following, unless another trace is open.
  private registerFollowCommand() {
    if (this.followCommand !== undefined) return;
    this.followCommand = this.app.commands.registerCommand({
      id: FOLLOW_COMMAND,
      name: 'Live Stream: Toggle Follow Live Scroll',
      defaultHotkey: 'Space',
      callback: () => this.toggleFollow(),
    });
  }

  private update() {
    this.refreshStatus();
    this.redraw();
  }

  private redraw() {
    this.app.raf.scheduleFullRedraw();
  }
}

// Shows the HUD row of `render` above the app. The row has a root of its
// own, first in <body>, which the app's root (<body> itself) leaves in place.
// The body's top padding, kept at the row's height, moves the app below it.
function mountTopRow(app: App, render: () => m.Children) {
  const root = document.createElement('div');
  root.className = 'pf-live-stream-hud-root';
  document.body.prepend(root);
  new ResizeObserver(() => {
    const height = root.getBoundingClientRect().height;
    document.body.style.paddingTop = `${height}px`;
  }).observe(root);

  // The app's m.mount, so the row redraws with the app.
  m.mount(root, {
    view: () => {
      // E.g. once something replaced the body's children.
      if (!root.isConnected) document.body.prepend(root);
      // The app's theme provider wraps only the app.
      return m(
        '.pf-theme-provider',
        {className: `pf-theme-provider--${appTheme(app)}`},
        render(),
      );
    },
  });
}

function appTheme(app: App): 'dark' | 'light' {
  return app.settings.get('theme')?.get() === 'dark' ? 'dark' : 'light';
}

// Points the page's URL at `target`, so that reloading opens it.
function setUrlTarget(target: string) {
  const url = new URL(window.location.href);
  url.searchParams.set(`${PLUGIN_ID}:target`, target);
  window.history.replaceState(window.history.state, '', url);
}
