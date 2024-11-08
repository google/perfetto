// Copyright (C) 2018 The Android Open Source Project
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
import {getCurrentChannel} from '../core/channels';
import {TRACE_SUFFIX} from '../common/constants';
import {
  disableMetatracingAndGetTrace,
  enableMetatracing,
  isMetatracingEnabled,
} from '../core/metatracing';
import {Engine, EngineMode} from '../trace_processor/engine';
import {featureFlags} from '../core/feature_flags';
import {raf} from '../core/raf_scheduler';
import {SCM_REVISION, VERSION} from '../gen/perfetto_version';
import {showModal} from '../widgets/modal';
import {Animation} from './animation';
import {downloadData, downloadUrl} from './download_utils';
import {globals} from './globals';
import {toggleHelp} from './help_modal';
import {shareTrace} from './trace_share_utils';
import {
  convertTraceToJsonAndDownload,
  convertTraceToSystraceAndDownload,
} from './trace_converter';
import {openInOldUIWithSizeCheck} from './legacy_trace_viewer';
import {SIDEBAR_SECTIONS, SidebarSections} from '../public/sidebar';
import {AppImpl} from '../core/app_impl';
import {Trace} from '../public/trace';
import {OptionalTraceImplAttrs, TraceImpl} from '../core/trace_impl';
import {Command} from '../public/command';
import {SidebarMenuItemInternal} from '../core/sidebar_manager';
import {exists, getOrCreate} from '../base/utils';
import {copyToClipboard} from '../base/clipboard';
import {classNames} from '../base/classnames';
import {formatHotkey} from '../base/hotkeys';
import {assetSrc} from '../base/assets';

const GITILES_URL =
  'https://android.googlesource.com/platform/external/perfetto';

function getBugReportUrl(): string {
  if (globals.isInternalUser) {
    return 'https://goto.google.com/perfetto-ui-bug';
  } else {
    return 'https://github.com/google/perfetto/issues/new';
  }
}

const HIRING_BANNER_FLAG = featureFlags.register({
  id: 'showHiringBanner',
  name: 'Show hiring banner',
  description: 'Show the "We\'re hiring" banner link in the side bar.',
  defaultValue: false,
});

function shouldShowHiringBanner(): boolean {
  return globals.isInternalUser && HIRING_BANNER_FLAG.get();
}

async function openCurrentTraceWithOldUI(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent(
    'Trace Actions',
    'Open current trace in legacy UI',
  );
  const file = await trace.getTraceFile();
  await openInOldUIWithSizeCheck(file);
}

async function convertTraceToSystrace(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Convert to .systrace');
  const file = await trace.getTraceFile();
  await convertTraceToSystraceAndDownload(file);
}

async function convertTraceToJson(trace: Trace): Promise<void> {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Convert to .json');
  const file = await trace.getTraceFile();
  await convertTraceToJsonAndDownload(file);
}

function downloadTrace(trace: TraceImpl) {
  if (!trace.traceInfo.downloadable) return;
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Download trace');

  let url = '';
  let fileName = `trace${TRACE_SUFFIX}`;
  const src = trace.traceInfo.source;
  if (src.type === 'URL') {
    url = src.url;
    fileName = url.split('/').slice(-1)[0];
  } else if (src.type === 'ARRAY_BUFFER') {
    const blob = new Blob([src.buffer], {type: 'application/octet-stream'});
    const inputFileName = window.prompt(
      'Please enter a name for your file or leave blank',
    );
    if (inputFileName) {
      fileName = `${inputFileName}.perfetto_trace.gz`;
    } else if (src.fileName) {
      fileName = src.fileName;
    }
    url = URL.createObjectURL(blob);
  } else if (src.type === 'FILE') {
    const file = src.file;
    url = URL.createObjectURL(file);
    fileName = file.name;
  } else {
    throw new Error(`Download from ${JSON.stringify(src)} is not supported`);
  }
  downloadUrl(fileName, url);
}

function highPrecisionTimersAvailable(): boolean {
  // High precision timers are available either when the page is cross-origin
  // isolated or when the trace processor is a standalone binary.
  return (
    window.crossOriginIsolated ||
    AppImpl.instance.trace?.engine.mode === 'HTTP_RPC'
  );
}

function recordMetatrace(engine: Engine) {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Record metatrace');

  if (!highPrecisionTimersAvailable()) {
    const PROMPT = `High-precision timers are not available to WASM trace processor yet.

Modern browsers restrict high-precision timers to cross-origin-isolated pages.
As Perfetto UI needs to open traces via postMessage, it can't be cross-origin
isolated until browsers ship support for
'Cross-origin-opener-policy: restrict-properties'.

Do you still want to record a metatrace?
Note that events under timer precision (1ms) will dropped.
Alternatively, connect to a trace_processor_shell --httpd instance.
`;
    showModal({
      title: `Trace processor doesn't have high-precision timers`,
      content: m('.modal-pre', PROMPT),
      buttons: [
        {
          text: 'YES, record metatrace',
          primary: true,
          action: () => {
            enableMetatracing();
            engine.enableMetatrace();
          },
        },
        {
          text: 'NO, cancel',
        },
      ],
    });
  } else {
    engine.enableMetatrace();
  }
}

async function toggleMetatrace(e: Engine) {
  return isMetatracingEnabled() ? finaliseMetatrace(e) : recordMetatrace(e);
}

async function finaliseMetatrace(engine: Engine) {
  AppImpl.instance.analytics.logEvent('Trace Actions', 'Finalise metatrace');

  const jsEvents = disableMetatracingAndGetTrace();

  const result = await engine.stopAndGetMetatrace();
  if (result.error.length !== 0) {
    throw new Error(`Failed to read metatrace: ${result.error}`);
  }

  downloadData('metatrace', result.metatrace, jsEvents);
}

class EngineRPCWidget implements m.ClassComponent<OptionalTraceImplAttrs> {
  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    let cssClass = '';
    let title = 'Number of pending SQL queries';
    let label: string;
    let failed = false;
    let mode: EngineMode | undefined;

    const engine = attrs.trace?.engine;
    if (engine !== undefined) {
      mode = engine.mode;
      if (engine.failed !== undefined) {
        cssClass += '.red';
        title = 'Query engine crashed\n' + engine.failed;
        failed = true;
      }
    }

    // If we don't have an engine yet, guess what will be the mode that will
    // be used next time we'll create one. Even if we guess it wrong (somehow
    // trace_controller.ts takes a different decision later, e.g. because the
    // RPC server is shut down after we load the UI and cached httpRpcState)
    // this will eventually become  consistent once the engine is created.
    if (mode === undefined) {
      if (
        AppImpl.instance.httpRpc.httpRpcAvailable &&
        AppImpl.instance.httpRpc.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE'
      ) {
        mode = 'HTTP_RPC';
      } else {
        mode = 'WASM';
      }
    }

    if (mode === 'HTTP_RPC') {
      cssClass += '.green';
      label = 'RPC';
      title += '\n(Query engine: native accelerator over HTTP+RPC)';
    } else {
      label = 'WSM';
      title += '\n(Query engine: built-in WASM)';
    }

    const numReqs = attrs.trace?.engine.numRequestsPending ?? 0;
    return m(
      `.dbg-info-square${cssClass}`,
      {title},
      m('div', label),
      m('div', `${failed ? 'FAIL' : numReqs}`),
    );
  }
}

const ServiceWorkerWidget: m.Component = {
  view() {
    let cssClass = '';
    let title = 'Service Worker: ';
    let label = 'N/A';
    const ctl = AppImpl.instance.serviceWorkerController;
    if (!('serviceWorker' in navigator)) {
      label = 'N/A';
      title += 'not supported by the browser (requires HTTPS)';
    } else if (ctl.bypassed) {
      label = 'OFF';
      cssClass = '.red';
      title += 'Bypassed, using live network. Double-click to re-enable';
    } else if (ctl.installing) {
      label = 'UPD';
      cssClass = '.amber';
      title += 'Installing / updating ...';
    } else if (!navigator.serviceWorker.controller) {
      label = 'N/A';
      title += 'Not available, using network';
    } else {
      label = 'ON';
      cssClass = '.green';
      title += 'Serving from cache. Ready for offline use';
    }

    const toggle = async () => {
      if (ctl.bypassed) {
        ctl.setBypass(false);
        return;
      }
      showModal({
        title: 'Disable service worker?',
        content: m(
          'div',
          m(
            'p',
            `If you continue the service worker will be disabled until
                      manually re-enabled.`,
          ),
          m(
            'p',
            `All future requests will be served from the network and the
                    UI won't be available offline.`,
          ),
          m(
            'p',
            `You should do this only if you are debugging the UI
                    or if you are experiencing caching-related problems.`,
          ),
          m(
            'p',
            `Disabling will cause a refresh of the UI, the current state
                    will be lost.`,
          ),
        ),
        buttons: [
          {
            text: 'Disable and reload',
            primary: true,
            action: () => ctl.setBypass(true).then(() => location.reload()),
          },
          {text: 'Cancel'},
        ],
      });
    };

    return m(
      `.dbg-info-square${cssClass}`,
      {title, ondblclick: toggle},
      m('div', 'SW'),
      m('div', label),
    );
  },
};

class SidebarFooter implements m.ClassComponent<OptionalTraceImplAttrs> {
  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    return m(
      '.sidebar-footer',
      m(EngineRPCWidget, attrs),
      m(ServiceWorkerWidget),
      m(
        '.version',
        m(
          'a',
          {
            href: `${GITILES_URL}/+/${SCM_REVISION}/ui`,
            title: `Channel: ${getCurrentChannel()}`,
            target: '_blank',
          },
          VERSION,
        ),
      ),
    );
  }
}

class HiringBanner implements m.ClassComponent {
  view() {
    return m(
      '.hiring-banner',
      m(
        'a',
        {
          href: 'http://go/perfetto-open-roles',
          target: '_blank',
        },
        "We're hiring!",
      ),
    );
  }
}

export class Sidebar implements m.ClassComponent<OptionalTraceImplAttrs> {
  private _redrawWhileAnimating = new Animation(() => raf.scheduleFullRedraw());
  private _asyncJobPending = new Set<string>();
  private _sectionExpanded = new Map<string, boolean>();

  constructor() {
    registerMenuItems();
  }

  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    const sidebar = AppImpl.instance.sidebar;
    if (!sidebar.enabled) return null;
    return m(
      'nav.sidebar',
      {
        class: sidebar.visible ? 'show-sidebar' : 'hide-sidebar',
        // 150 here matches --sidebar-timing in the css.
        // TODO(hjd): Should link to the CSS variable.
        ontransitionstart: (e: TransitionEvent) => {
          if (e.target !== e.currentTarget) return;
          this._redrawWhileAnimating.start(150);
        },
        ontransitionend: (e: TransitionEvent) => {
          if (e.target !== e.currentTarget) return;
          this._redrawWhileAnimating.stop();
        },
      },
      shouldShowHiringBanner() ? m(HiringBanner) : null,
      m(
        `header.${getCurrentChannel()}`,
        m(`img[src=${assetSrc('assets/brand.png')}].brand`),
        m(
          'button.sidebar-button',
          {
            onclick: () => sidebar.toggleVisibility(),
          },
          m(
            'i.material-icons',
            {
              title: sidebar.visible ? 'Hide menu' : 'Show menu',
            },
            'menu',
          ),
        ),
      ),
      m(
        '.sidebar-scroll',
        m(
          '.sidebar-scroll-container',
          ...(Object.keys(SIDEBAR_SECTIONS) as SidebarSections[]).map((s) =>
            this.renderSection(s),
          ),
          m(SidebarFooter, attrs),
        ),
      ),
    );
  }

  private renderSection(sectionId: SidebarSections) {
    const section = SIDEBAR_SECTIONS[sectionId];
    const menuItems = AppImpl.instance.sidebar.menuItems
      .valuesAsArray()
      .filter((item) => item.section === sectionId)
      .sort((a, b) => (a.sortOrder ?? 0) - (b.sortOrder ?? 0))
      .map((item) => this.renderItem(item));

    // Don't render empty sections.
    if (menuItems.length === 0) return undefined;

    const expanded = getOrCreate(this._sectionExpanded, sectionId, () => true);
    return m(
      `section${expanded ? '.expanded' : ''}`,
      m(
        '.section-header',
        {
          onclick: () => {
            this._sectionExpanded.set(sectionId, !expanded);
            raf.scheduleFullRedraw();
          },
        },
        m('h1', {title: section.title}, section.title),
        m('h2', section.summary),
      ),
      m('.section-content', m('ul', menuItems)),
    );
  }

  private renderItem(item: SidebarMenuItemInternal): m.Child {
    let href = '#';
    let disabled = false;
    let target = null;
    let command: Command | undefined = undefined;
    let tooltip = valueOrCallback(item.tooltip);
    let onclick: (() => unknown | Promise<unknown>) | undefined = undefined;
    const commandId = 'commandId' in item ? item.commandId : undefined;
    const action = 'action' in item ? item.action : undefined;
    let text = valueOrCallback(item.text);
    const disabReason: boolean | string | undefined = valueOrCallback(
      item.disabled,
    );

    if (disabReason === true || typeof disabReason === 'string') {
      disabled = true;
      onclick = () => typeof disabReason === 'string' && alert(disabReason);
    } else if (action !== undefined) {
      onclick = action;
    } else if (commandId !== undefined) {
      const cmdMgr = AppImpl.instance.commands;
      command = cmdMgr.hasCommand(commandId ?? '')
        ? cmdMgr.getCommand(commandId)
        : undefined;
      if (command === undefined) {
        disabled = true;
      } else {
        text = text !== undefined ? text : command.name;
        if (command.defaultHotkey !== undefined) {
          tooltip =
            `${tooltip ?? command.name}` +
            ` [${formatHotkey(command.defaultHotkey)}]`;
        }
        onclick = () => cmdMgr.runCommand(commandId);
      }
    }

    // This is not an else if because in some rare cases the user might want
    // to have both an href and onclick, with different behaviors. The only case
    // today is the trace name / URL, where we want the URL in the href to
    // support right-click -> copy URL, but the onclick does copyToClipboard().
    if ('href' in item && item.href !== undefined) {
      href = item.href;
      target = href.startsWith('#') ? null : '_blank';
    }
    return m(
      'li',
      m(
        'a',
        {
          className: classNames(
            valueOrCallback(item.cssClass),
            this._asyncJobPending.has(item.id) && 'pending',
          ),
          onclick: onclick && this.wrapClickHandler(item.id, onclick),
          href,
          target,
          disabled,
          title: tooltip,
        },
        exists(item.icon) && m('i.material-icons', valueOrCallback(item.icon)),
        text,
      ),
    );
  }

  // Creates the onClick handlers for the items which provided a function in the
  // `action` member. The function can be either sync or async.
  // What we want to achieve here is the following:
  // - If the action is async (returns a Promise), we want to render a spinner,
  //   next to the menu item, until the promise is resolved.
  // - [Minor] we want to call e.preventDefault() to override the behaviour of
  //   the <a href='#'> which gets rendered for accessibility reasons.
  private wrapClickHandler(itemId: string, itemAction: Function) {
    return (e: Event) => {
      e.preventDefault(); // Make the <a href="#"> a no-op.
      const res = itemAction();
      if (!(res instanceof Promise)) return;
      if (this._asyncJobPending.has(itemId)) {
        return; // Don't queue up another action if not yet finished.
      }
      this._asyncJobPending.add(itemId);
      raf.scheduleFullRedraw();
      res.finally(() => {
        this._asyncJobPending.delete(itemId);
        raf.scheduleFullRedraw();
      });
    };
  }
}

// TODO(primiano): The registrations below should be moved to dedicated
// plugins (most of this really belongs to core_plugins/commads/index.ts).
// For now i'm keeping everything here as splitting these require moving some
// functions like share_trace() out of core, splitting out permalink, etc.

let globalItemsRegistered = false;
const traceItemsRegistered = new WeakSet<TraceImpl>();

function registerMenuItems() {
  if (!globalItemsRegistered) {
    globalItemsRegistered = true;
    registerGlobalSidebarEntries();
  }
  const trace = AppImpl.instance.trace;
  if (trace !== undefined && !traceItemsRegistered.has(trace)) {
    traceItemsRegistered.add(trace);
    registerTraceMenuItems(trace);
  }
}

function registerGlobalSidebarEntries() {
  const app = AppImpl.instance;
  // TODO(primiano): The Open file / Open with legacy entries are registered by
  // the 'perfetto.CoreCommands' plugins. Make things consistent.
  app.sidebar.addMenuItem({
    section: 'navigation',
    text: 'Record new trace',
    href: '#!/record',
    icon: 'fiber_smart_record',
    sortOrder: 2,
  });
  app.sidebar.addMenuItem({
    section: 'support',
    text: 'Keyboard shortcuts',
    action: toggleHelp,
    icon: 'help',
  });
  app.sidebar.addMenuItem({
    section: 'support',
    text: 'Documentation',
    href: 'https://perfetto.dev/docs',
    icon: 'find_in_page',
  });
  app.sidebar.addMenuItem({
    section: 'support',
    sortOrder: 4,
    text: 'Report a bug',
    href: getBugReportUrl(),
    icon: 'bug_report',
  });
}

function registerTraceMenuItems(trace: TraceImpl) {
  const downloadDisabled = trace.traceInfo.downloadable
    ? false
    : 'Cannot download external trace';

  const traceTitle = trace?.traceInfo.traceTitle;
  traceTitle &&
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: traceTitle,
      href: trace.traceInfo.traceUrl,
      action: () => copyToClipboard(trace.traceInfo.traceUrl),
      tooltip: 'Click to copy the URL',
      cssClass: 'trace-file-name',
    });
  trace.sidebar.addMenuItem({
    section: 'current_trace',
    text: 'Show timeline',
    href: '#!/viewer',
    icon: 'line_style',
  });
  globals.isInternalUser &&
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Share',
      action: async () => await shareTrace(trace),
      icon: 'share',
    });
  trace.sidebar.addMenuItem({
    section: 'current_trace',
    text: 'Download',
    action: () => downloadTrace(trace),
    icon: 'file_download',
    disabled: downloadDisabled,
  });
  trace.sidebar.addMenuItem({
    section: 'convert_trace',
    text: 'Switch to legacy UI',
    action: async () => await openCurrentTraceWithOldUI(trace),
    icon: 'filter_none',
    disabled: downloadDisabled,
  });
  trace.sidebar.addMenuItem({
    section: 'convert_trace',
    text: 'Convert to .json',
    action: async () => await convertTraceToJson(trace),
    icon: 'file_download',
    disabled: downloadDisabled,
  });
  trace.traceInfo.hasFtrace &&
    trace.sidebar.addMenuItem({
      section: 'convert_trace',
      text: 'Convert to .systrace',
      action: async () => await convertTraceToSystrace(trace),
      icon: 'file_download',
      disabled: downloadDisabled,
    });
  trace.sidebar.addMenuItem({
    section: 'support',
    sortOrder: 5,
    text: () =>
      isMetatracingEnabled() ? 'Finalize metatrace' : 'Record metatrace',
    action: () => toggleMetatrace(trace.engine),
    icon: () => (isMetatracingEnabled() ? 'download' : 'fiber_smart_record'),
  });
}

// Used to deal with fields like the entry name, which can be either a direct
// string or a callback that returns the string.
function valueOrCallback<T>(value: T | (() => T)): T;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined;
function valueOrCallback<T>(value: T | (() => T) | undefined): T | undefined {
  if (value === undefined) return undefined;
  return value instanceof Function ? value() : value;
}
