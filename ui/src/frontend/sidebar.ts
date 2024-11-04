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
import {isString} from '../base/object_utils';
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
import {createTraceLink, shareTrace} from './trace_share_utils';
import {
  convertTraceToJsonAndDownload,
  convertTraceToSystraceAndDownload,
} from './trace_converter';
import {openInOldUIWithSizeCheck} from './legacy_trace_viewer';
import {formatHotkey} from '../base/hotkeys';
import {SidebarMenuItem} from '../public/sidebar';
import {AppImpl} from '../core/app_impl';
import {Trace} from '../public/trace';
import {removeFalsyValues} from '../base/array_utils';
import {OptionalTraceImplAttrs, TraceImpl} from '../core/trace_impl';

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

const WIDGETS_PAGE_IN_NAV_FLAG = featureFlags.register({
  id: 'showWidgetsPageInNav',
  name: 'Show widgets page',
  description: 'Show a link to the widgets page in the side bar.',
  defaultValue: false,
});

const PLUGINS_PAGE_IN_NAV_FLAG = featureFlags.register({
  id: 'showPluginsPageInNav',
  name: 'Show plugins page',
  description: 'Show a link to the plugins page in the side bar.',
  defaultValue: false,
});

const INSIGHTS_PAGE_IN_NAV_FLAG = featureFlags.register({
  id: 'showInsightsPageInNav',
  name: 'Show insights page',
  description: 'Show a link to the insights page in the side bar.',
  defaultValue: false,
});

const VIZ_PAGE_IN_NAV_FLAG = featureFlags.register({
  id: 'showVizPageInNav',
  name: 'Show viz page',
  description: 'Show a link to the viz page in the side bar.',
  defaultValue: true,
});

const EXPLORE_PAGE_IN_NAV_FLAG = featureFlags.register({
  id: 'showExplorePageInNav',
  name: 'Show explore page',
  description: 'Show a link to the explore page in the side bar.',
  defaultValue: false,
});

function shouldShowHiringBanner(): boolean {
  return globals.isInternalUser && HIRING_BANNER_FLAG.get();
}

interface SectionItem {
  t: string;
  a: string | (() => void | Promise<void>);
  i: string;
  tooltip?: string;
  isVisible?: () => boolean;
  internalUserOnly?: boolean;
  disabled?: string; // If !undefined provides the reason why it's disabled.
}

interface Section {
  title: string;
  summary: string;
  items: SectionItem[];
  expanded?: boolean;
  appendOpenedTraceTitle?: boolean;
}

function insertSidebarMenuitems(
  groupSelector: SidebarMenuItem['group'],
): ReadonlyArray<SectionItem> {
  return AppImpl.instance.sidebar.menuItems
    .valuesAsArray()
    .filter(({group}) => group === groupSelector)
    .sort((a, b) => {
      const prioA = a.priority ?? 0;
      const prioB = b.priority ?? 0;
      return prioA - prioB;
    })
    .map((item) => {
      const cmd = AppImpl.instance.commands.getCommand(item.commandId);
      const title = cmd.defaultHotkey
        ? `${cmd.name} [${formatHotkey(cmd.defaultHotkey)}]`
        : cmd.name;
      return {
        t: cmd.name,
        a: cmd.callback,
        i: item.icon,
        title,
      };
    });
}

function getSections(trace?: TraceImpl): Section[] {
  const downloadDisabled = trace?.traceInfo.downloadable
    ? undefined
    : 'Cannot download external trace';
  return removeFalsyValues([
    {
      title: 'Navigation',
      summary: 'Open or record a new trace',
      expanded: true,
      items: [
        ...insertSidebarMenuitems('navigation'),
        {
          t: 'Record new trace',
          a: '#!/record',
          i: 'fiber_smart_record',
        },
        {
          t: 'Widgets',
          a: '#!/widgets',
          i: 'widgets',
          isVisible: () => WIDGETS_PAGE_IN_NAV_FLAG.get(),
        },
        {
          t: 'Plugins',
          a: '#!/plugins',
          i: 'extension',
          isVisible: () => PLUGINS_PAGE_IN_NAV_FLAG.get(),
        },
      ],
    },

    trace && {
      title: 'Current Trace',
      summary: 'Actions on the current trace',
      expanded: true,
      appendOpenedTraceTitle: true,
      items: [
        {t: 'Show timeline', a: '#!/viewer', i: 'line_style'},
        {
          t: 'Share',
          a: async () => await shareTrace(trace),
          i: 'share',
          internalUserOnly: true,
        },
        {
          t: 'Download',
          a: () => downloadTrace(trace),
          i: 'file_download',
          disabled: downloadDisabled,
        },
        {
          t: 'Query (SQL)',
          a: '#!/query',
          i: 'database',
        },
        {
          t: 'Explore',
          a: '#!/explore',
          i: 'data_exploration',
          isVisible: () => EXPLORE_PAGE_IN_NAV_FLAG.get(),
        },
        {
          t: 'Insights',
          a: '#!/insights',
          i: 'insights',
          isVisible: () => INSIGHTS_PAGE_IN_NAV_FLAG.get(),
        },
        {
          t: 'Viz',
          a: '#!/viz',
          i: 'area_chart',
          isVisible: () => VIZ_PAGE_IN_NAV_FLAG.get(),
        },
        {t: 'Metrics', a: '#!/metrics', i: 'speed'},
        {t: 'Info and stats', a: '#!/info', i: 'info'},
      ],
    },

    trace && {
      title: 'Convert trace',
      summary: 'Convert to other formats',
      expanded: true,
      items: [
        {
          t: 'Switch to legacy UI',
          a: async () => await openCurrentTraceWithOldUI(trace),
          i: 'filter_none',
          disabled: downloadDisabled,
        },
        {
          t: 'Convert to .json',
          a: async () => await convertTraceToJson(trace),
          i: 'file_download',
          disabled: downloadDisabled,
        },

        {
          t: 'Convert to .systrace',
          a: async () => await convertTraceToSystrace(trace),
          i: 'file_download',
          isVisible: () => Boolean(trace?.traceInfo.hasFtrace),
          disabled: downloadDisabled,
        },
      ],
    },

    {
      title: 'Example Traces',
      expanded: true,
      summary: 'Open an example trace',
      items: [...insertSidebarMenuitems('example_traces')],
    },

    {
      title: 'Support',
      expanded: true,
      summary: 'Documentation & Bugs',
      items: removeFalsyValues([
        {t: 'Keyboard shortcuts', a: toggleHelp, i: 'help'},
        {t: 'Documentation', a: 'https://perfetto.dev/docs', i: 'find_in_page'},
        {t: 'Flags', a: '#!/flags', i: 'emoji_flags'},
        {
          t: 'Report a bug',
          a: getBugReportUrl(),
          i: 'bug_report',
        },
        trace &&
          (isMetatracingEnabled()
            ? {
                t: 'Finalise metatrace',
                a: () => finaliseMetatrace(trace.engine),
                i: 'file_download',
              }
            : {
                t: 'Record metatrace',
                a: () => recordMetatrace(trace.engine),
                i: 'fiber_smart_record',
              }),
      ]),
    },
  ]);
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
        globals.httpRpcState.connected &&
        AppImpl.instance.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE'
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
    const ctl = globals.serviceWorkerController;
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
      if (globals.serviceWorkerController.bypassed) {
        globals.serviceWorkerController.setBypass(false);
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
            action: () => {
              globals.serviceWorkerController
                .setBypass(true)
                .then(() => location.reload());
            },
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

  view({attrs}: m.CVnode<OptionalTraceImplAttrs>) {
    if (AppImpl.instance.sidebar.sidebarEnabled === 'DISABLED') {
      return null;
    }
    const vdomSections = [];
    const trace = attrs.trace;
    for (const section of getSections(trace)) {
      const vdomItems = [];
      for (const item of section.items) {
        if (item.isVisible !== undefined && !item.isVisible()) {
          continue;
        }
        let css = '';
        let attrs = {
          onclick: this.wrapClickHandler(item),
          href: isString(item.a) ? item.a : '#',
          target: isString(item.a) && !item.a.startsWith('#') ? '_blank' : null,
          disabled: false,
          id: item.t.toLowerCase().replace(/[^\w]/g, '_'),
        };

        if (this._asyncJobPending.has(item.t)) {
          css = '.pending';
        }
        if (item.internalUserOnly && !globals.isInternalUser) {
          continue;
        }
        if (item.disabled !== undefined) {
          attrs = {
            onclick: (e: Event) => {
              e.preventDefault();
              alert(item.disabled);
            },
            href: '#',
            target: null,
            disabled: true,
            id: '',
          };
        }
        vdomItems.push(
          m(
            'li',
            m(
              `a${css}`,
              {...attrs, title: item.tooltip},
              m('i.material-icons', item.i),
              item.t,
            ),
          ),
        );
      }
      if (section.appendOpenedTraceTitle && attrs.trace?.traceInfo.traceTitle) {
        const {traceTitle, traceUrl} = attrs.trace?.traceInfo;
        vdomItems.unshift(m('li', createTraceLink(traceTitle, traceUrl)));
      }
      vdomSections.push(
        m(
          `section${section.expanded ? '.expanded' : ''}`,
          m(
            '.section-header',
            {
              onclick: () => {
                section.expanded = !section.expanded;
                raf.scheduleFullRedraw();
              },
            },
            m('h1', {title: section.summary}, section.title),
            m('h2', section.summary),
          ),
          m('.section-content', m('ul', vdomItems)),
        ),
      );
    }
    return m(
      'nav.sidebar',
      {
        class:
          AppImpl.instance.sidebar.sidebarVisibility === 'VISIBLE'
            ? 'show-sidebar'
            : 'hide-sidebar',
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
        m(`img[src=${globals.root}assets/brand.png].brand`),
        m(
          'button.sidebar-button',
          {
            onclick: () => AppImpl.instance.sidebar.toggleSidebarVisbility(),
          },
          m(
            'i.material-icons',
            {
              title:
                AppImpl.instance.sidebar.sidebarVisibility === 'VISIBLE'
                  ? 'Hide menu'
                  : 'Show menu',
            },
            'menu',
          ),
        ),
      ),
      m(
        '.sidebar-scroll',
        m(
          '.sidebar-scroll-container',
          ...vdomSections,
          m(SidebarFooter, attrs),
        ),
      ),
    );
  }

  // creates the onClick handlers for the items which provided an
  // (async)function in the `a`. If `a` is a url, instead, just return null.
  // We repeate this in view() passes and not in the constructor because new
  // sidebar items can be added by plugins at any time.
  // What we want to achieve here is the following:
  // - We want to allow plugins that contribute to the sidebar to just specify
  //   either string URLs or (async) functions as actions for a sidebar menu.
  // - When they specify an async function, we want to render a spinner, next
  //   to the menu item, until the promise is resolved.
  // - [Minor] we want to call e.preventDefault() to override the behaviour of
  //   the <a href='#'> which gets rendered for accessibility reasons.
  private wrapClickHandler(item: SectionItem) {
    // item.a can be either a function or a URL. In the latter case, we
    // don't need to generate any onclick handler.
    const itemAction = item.a;
    if (typeof itemAction !== 'function') {
      return null;
    }
    const itemId = item.t;
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
