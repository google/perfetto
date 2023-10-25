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

import {assertExists, assertTrue} from '../base/logging';
import {isString} from '../base/object_utils';
import {Actions} from '../common/actions';
import {getCurrentChannel} from '../common/channels';
import {TRACE_SUFFIX} from '../common/constants';
import {ConversionJobStatus} from '../common/conversion_jobs';
import {Engine} from '../common/engine';
import {featureFlags} from '../common/feature_flags';
import {
  disableMetatracingAndGetTrace,
  enableMetatracing,
  isMetatracingEnabled,
} from '../common/metatracing';
import {EngineMode} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {SCM_REVISION, VERSION} from '../gen/perfetto_version';

import {Animation} from './animation';
import {downloadData, downloadUrl} from './download_utils';
import {globals} from './globals';
import {toggleHelp} from './help_modal';
import {
  isLegacyTrace,
  openFileWithLegacyTraceViewer,
} from './legacy_trace_viewer';
import {showModal} from './modal';
import {Router} from './router';
import {createTraceLink, isDownloadable, shareTrace} from './trace_attrs';
import {
  convertToJson,
  convertTraceToJsonAndDownload,
  convertTraceToSystraceAndDownload,
} from './trace_converter';

const GITILES_URL =
    'https://android.googlesource.com/platform/external/perfetto';

let lastTabTitle = '';

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


function shouldShowHiringBanner(): boolean {
  return globals.isInternalUser && HIRING_BANNER_FLAG.get();
}

export const EXAMPLE_ANDROID_TRACE_URL =
    'https://storage.googleapis.com/perfetto-misc/example_android_trace_15s';

export const EXAMPLE_CHROME_TRACE_URL =
    'https://storage.googleapis.com/perfetto-misc/chrome_example_wikipedia.perfetto_trace.gz';

interface SectionItem {
  t: string;
  a: string|((e: Event) => void);
  i: string;
  isPending?: () => boolean;
  isVisible?: () => boolean;
  internalUserOnly?: boolean;
  checkDownloadDisabled?: boolean;
  checkMetatracingEnabled?: boolean;
  checkMetatracingDisabled?: boolean;
}

interface Section {
  title: string;
  summary: string;
  items: SectionItem[];
  expanded?: boolean;
  hideIfNoTraceLoaded?: boolean;
  appendOpenedTraceTitle?: boolean;
}

const SECTIONS: Section[] = [

  {
    title: 'Navigation',
    summary: 'Open or record a new trace',
    expanded: true,
    items: [
      {t: 'Open trace file', a: popupFileSelectionDialog, i: 'folder_open'},
      {
        t: 'Open with legacy UI',
        a: popupFileSelectionDialogOldUI,
        i: 'filter_none',
      },
      {t: 'Record new trace', a: navigateRecord, i: 'fiber_smart_record'},
      {
        t: 'Widgets',
        a: navigateWidgets,
        i: 'widgets',
        isVisible: () => WIDGETS_PAGE_IN_NAV_FLAG.get(),
      },
    ],
  },

  {
    title: 'Current Trace',
    summary: 'Actions on the current trace',
    expanded: true,
    hideIfNoTraceLoaded: true,
    appendOpenedTraceTitle: true,
    items: [
      {t: 'Show timeline', a: navigateViewer, i: 'line_style'},
      {
        t: 'Share',
        a: handleShareTrace,
        i: 'share',
        internalUserOnly: true,
        isPending: () => globals.getConversionJobStatus('create_permalink') ===
            ConversionJobStatus.InProgress,
      },
      {
        t: 'Download',
        a: downloadTrace,
        i: 'file_download',
        checkDownloadDisabled: true,
      },
      {t: 'Query (SQL)', a: navigateQuery, i: 'database'},
      {
        t: 'Insights',
        a: navigateInsights,
        i: 'insights',
        isVisible: () => INSIGHTS_PAGE_IN_NAV_FLAG.get(),
      },
      {
        t: 'Viz',
        a: navigateViz,
        i: 'area_chart',
        isVisible: () => VIZ_PAGE_IN_NAV_FLAG.get(),
      },
      {t: 'Metrics', a: navigateMetrics, i: 'speed'},
      {t: 'Info and stats', a: navigateInfo, i: 'info'},
    ],
  },

  {
    title: 'Convert trace',
    summary: 'Convert to other formats',
    expanded: true,
    hideIfNoTraceLoaded: true,
    items: [
      {
        t: 'Switch to legacy UI',
        a: openCurrentTraceWithOldUI,
        i: 'filter_none',
        isPending: () => globals.getConversionJobStatus('open_in_legacy') ===
            ConversionJobStatus.InProgress,
      },
      {
        t: 'Convert to .json',
        a: convertTraceToJson,
        i: 'file_download',
        isPending: () => globals.getConversionJobStatus('convert_json') ===
            ConversionJobStatus.InProgress,
        checkDownloadDisabled: true,
      },

      {
        t: 'Convert to .systrace',
        a: convertTraceToSystrace,
        i: 'file_download',
        isVisible: () => globals.hasFtrace,
        isPending: () => globals.getConversionJobStatus('convert_systrace') ===
            ConversionJobStatus.InProgress,
        checkDownloadDisabled: true,
      },

    ],
  },

  {
    title: 'Example Traces',
    expanded: true,
    summary: 'Open an example trace',
    items: [
      {
        t: 'Open Android example',
        a: openTraceUrl(EXAMPLE_ANDROID_TRACE_URL),
        i: 'description',
      },
      {
        t: 'Open Chrome example',
        a: openTraceUrl(EXAMPLE_CHROME_TRACE_URL),
        i: 'description',
      },
    ],
  },

  {
    title: 'Support',
    expanded: true,
    summary: 'Documentation & Bugs',
    items: [
      {t: 'Keyboard shortcuts', a: openHelp, i: 'help'},
      {t: 'Documentation', a: 'https://perfetto.dev/docs', i: 'find_in_page'},
      {t: 'Flags', a: navigateFlags, i: 'emoji_flags'},
      {
        t: 'Report a bug',
        a: () => window.open(getBugReportUrl()),
        i: 'bug_report',
      },
      {
        t: 'Record metatrace',
        a: recordMetatrace,
        i: 'fiber_smart_record',
        checkMetatracingDisabled: true,
      },
      {
        t: 'Finalise metatrace',
        a: finaliseMetatrace,
        i: 'file_download',
        checkMetatracingEnabled: true,
      },
    ],
  },
];

function openHelp(e: Event) {
  e.preventDefault();
  toggleHelp();
}

function getFileElement(): HTMLInputElement {
  return assertExists(
      document.querySelector<HTMLInputElement>('input[type=file]'));
}

function popupFileSelectionDialog(e: Event) {
  e.preventDefault();
  delete getFileElement().dataset['useCatapultLegacyUi'];
  getFileElement().click();
}

function popupFileSelectionDialogOldUI(e: Event) {
  e.preventDefault();
  getFileElement().dataset['useCatapultLegacyUi'] = '1';
  getFileElement().click();
}

function downloadTraceFromUrl(url: string): Promise<File> {
  return m.request({
    method: 'GET',
    url,
    // TODO(hjd): Once mithril is updated we can use responseType here rather
    // than using config and remove the extract below.
    config: (xhr) => {
      xhr.responseType = 'blob';
      xhr.onprogress = (progress) => {
        const percent = (100 * progress.loaded / progress.total).toFixed(1);
        globals.dispatch(Actions.updateStatus({
          msg: `Downloading trace ${percent}%`,
          timestamp: Date.now() / 1000,
        }));
      };
    },
    extract: (xhr) => {
      return xhr.response;
    },
  });
}

export async function getCurrentTrace(): Promise<Blob> {
  // Caller must check engine exists.
  const engine = assertExists(globals.getCurrentEngine());
  const src = engine.source;
  if (src.type === 'ARRAY_BUFFER') {
    return new Blob([src.buffer]);
  } else if (src.type === 'FILE') {
    return src.file;
  } else if (src.type === 'URL') {
    return downloadTraceFromUrl(src.url);
  } else {
    throw new Error(`Loading to catapult from source with type ${src.type}`);
  }
}

function openCurrentTraceWithOldUI(e: Event) {
  e.preventDefault();
  assertTrue(isTraceLoaded());
  globals.logging.logEvent('Trace Actions', 'Open current trace in legacy UI');
  if (!isTraceLoaded) return;
  getCurrentTrace()
      .then((file) => {
        openInOldUIWithSizeCheck(file);
      })
      .catch((error) => {
        throw new Error(`Failed to get current trace ${error}`);
      });
}

function convertTraceToSystrace(e: Event) {
  e.preventDefault();
  assertTrue(isTraceLoaded());
  globals.logging.logEvent('Trace Actions', 'Convert to .systrace');
  if (!isTraceLoaded) return;
  getCurrentTrace()
      .then((file) => {
        convertTraceToSystraceAndDownload(file);
      })
      .catch((error) => {
        throw new Error(`Failed to get current trace ${error}`);
      });
}

function convertTraceToJson(e: Event) {
  e.preventDefault();
  assertTrue(isTraceLoaded());
  globals.logging.logEvent('Trace Actions', 'Convert to .json');
  if (!isTraceLoaded) return;
  getCurrentTrace()
      .then((file) => {
        convertTraceToJsonAndDownload(file);
      })
      .catch((error) => {
        throw new Error(`Failed to get current trace ${error}`);
      });
}

export function isTraceLoaded(): boolean {
  return globals.getCurrentEngine() !== undefined;
}

export function openTraceUrl(url: string): (e: Event) => void {
  return (e) => {
    globals.logging.logEvent('Trace Actions', 'Open example trace');
    e.preventDefault();
    globals.dispatch(Actions.openTraceFromUrl({url}));
  };
}

function onInputElementFileSelectionChanged(e: Event) {
  if (!(e.target instanceof HTMLInputElement)) {
    throw new Error('Not an input element');
  }
  if (!e.target.files) return;
  const file = e.target.files[0];
  // Reset the value so onchange will be fired with the same file.
  e.target.value = '';

  if (e.target.dataset['useCatapultLegacyUi'] === '1') {
    openWithLegacyUi(file);
    return;
  }

  globals.logging.logEvent('Trace Actions', 'Open trace from file');
  globals.dispatch(Actions.openTraceFromFile({file}));
}

async function openWithLegacyUi(file: File) {
  // Switch back to the old catapult UI.
  globals.logging.logEvent('Trace Actions', 'Open trace in Legacy UI');
  if (await isLegacyTrace(file)) {
    openFileWithLegacyTraceViewer(file);
    return;
  }
  openInOldUIWithSizeCheck(file);
}

function openInOldUIWithSizeCheck(trace: Blob) {
  // Perfetto traces smaller than 50mb can be safely opened in the legacy UI.
  if (trace.size < 1024 * 1024 * 50) {
    convertToJson(trace);
    return;
  }

  // Give the user the option to truncate larger perfetto traces.
  const size = Math.round(trace.size / (1024 * 1024));
  showModal({
    title: 'Legacy UI may fail to open this trace',
    content:
        m('div',
          m('p',
            `This trace is ${size}mb, opening it in the legacy UI ` +
                `may fail.`),
          m('p',
            'More options can be found at ',
            m('a',
              {
                href: 'https://goto.google.com/opening-large-traces',
                target: '_blank',
              },
              'go/opening-large-traces'),
            '.')),
    buttons: [
      {
        text: 'Open full trace (not recommended)',
        action: () => convertToJson(trace),
      },
      {
        text: 'Open beginning of trace',
        action: () => convertToJson(trace, /* truncate*/ 'start'),
      },
      {
        text: 'Open end of trace',
        primary: true,
        action: () => convertToJson(trace, /* truncate*/ 'end'),
      },
    ],
  });
  return;
}

function navigateRecord(e: Event) {
  e.preventDefault();
  Router.navigate('#!/record');
}

function navigateWidgets(e: Event) {
  e.preventDefault();
  Router.navigate('#!/widgets');
}

function navigateQuery(e: Event) {
  e.preventDefault();
  Router.navigate('#!/query');
}

function navigateInsights(e: Event) {
  e.preventDefault();
  Router.navigate('#!/insights');
}

function navigateViz(e: Event) {
  e.preventDefault();
  Router.navigate('#!/viz');
}

function navigateFlags(e: Event) {
  e.preventDefault();
  Router.navigate('#!/flags');
}

function navigateMetrics(e: Event) {
  e.preventDefault();
  Router.navigate('#!/metrics');
}

function navigateInfo(e: Event) {
  e.preventDefault();
  Router.navigate('#!/info');
}

function navigateViewer(e: Event) {
  e.preventDefault();
  Router.navigate('#!/viewer');
}

function handleShareTrace(e: Event) {
  e.preventDefault();
  shareTrace();
}

function downloadTrace(e: Event) {
  e.preventDefault();
  if (!isDownloadable() || !isTraceLoaded()) return;
  globals.logging.logEvent('Trace Actions', 'Download trace');

  const engine = globals.getCurrentEngine();
  if (!engine) return;
  let url = '';
  let fileName = `trace${TRACE_SUFFIX}`;
  const src = engine.source;
  if (src.type === 'URL') {
    url = src.url;
    fileName = url.split('/').slice(-1)[0];
  } else if (src.type === 'ARRAY_BUFFER') {
    const blob = new Blob([src.buffer], {type: 'application/octet-stream'});
    const inputFileName =
        window.prompt('Please enter a name for your file or leave blank');
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

function getCurrentEngine(): Engine|undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) return undefined;
  return globals.engines.get(engineId);
}

function highPrecisionTimersAvailable(): boolean {
  // High precision timers are available either when the page is cross-origin
  // isolated or when the trace processor is a standalone binary.
  return window.crossOriginIsolated ||
      globals.getCurrentEngine()?.mode === 'HTTP_RPC';
}

function recordMetatrace(e: Event) {
  e.preventDefault();
  globals.logging.logEvent('Trace Actions', 'Record metatrace');

  const engine = getCurrentEngine();
  if (!engine) return;

  if (!highPrecisionTimersAvailable()) {
    const PROMPT =
        `High-precision timers are not available to WASM trace processor yet.

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

async function finaliseMetatrace(e: Event) {
  e.preventDefault();
  globals.logging.logEvent('Trace Actions', 'Finalise metatrace');

  const jsEvents = disableMetatracingAndGetTrace();

  const engine = getCurrentEngine();
  if (!engine) return;

  const result = await engine.stopAndGetMetatrace();
  if (result.error.length !== 0) {
    throw new Error(`Failed to read metatrace: ${result.error}`);
  }

  downloadData('metatrace', result.metatrace, jsEvents);
}


const EngineRPCWidget: m.Component = {
  view() {
    let cssClass = '';
    let title = 'Number of pending SQL queries';
    let label: string;
    let failed = false;
    let mode: EngineMode|undefined;

    const engine = globals.state.engine;
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
      if (globals.frontendLocalState.httpRpcState.connected &&
          globals.state.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE') {
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

    return m(
        `.dbg-info-square${cssClass}`,
        {title},
        m('div', label),
        m('div', `${failed ? 'FAIL' : globals.numQueuedQueries}`));
  },
};

const ServiceWorkerWidget: m.Component = {
  view() {
    let cssClass = '';
    let title = 'Service Worker: ';
    let label = 'N/A';
    const ctl = globals.serviceWorkerController;
    if ((!('serviceWorker' in navigator))) {
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
            m('p', `If you continue the service worker will be disabled until
                      manually re-enabled.`),
            m('p', `All future requests will be served from the network and the
                    UI won't be available offline.`),
            m('p', `You should do this only if you are debugging the UI
                    or if you are experiencing caching-related problems.`),
            m('p', `Disabling will cause a refresh of the UI, the current state
                    will be lost.`),
            ),
        buttons: [
          {
            text: 'Disable and reload',
            primary: true,
            action: () => {
              globals.serviceWorkerController.setBypass(true).then(
                  () => location.reload());
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
        m('div', label));
  },
};

const SidebarFooter: m.Component = {
  view() {
    return m(
        '.sidebar-footer',
        m(EngineRPCWidget),
        m(ServiceWorkerWidget),
        m(
            '.version',
            m('a',
              {
                href: `${GITILES_URL}/+/${SCM_REVISION}/ui`,
                title: `Channel: ${getCurrentChannel()}`,
                target: '_blank',
              },
              `${VERSION.substr(0, 11)}`),
            ),
    );
  },
};

class HiringBanner implements m.ClassComponent {
  view() {
    return m(
        '.hiring-banner',
        m('a',
          {
            href: 'http://go/perfetto-open-roles',
            target: '_blank',
          },
          'We\'re hiring!'));
  }
}

export class Sidebar implements m.ClassComponent {
  private _redrawWhileAnimating = new Animation(() => raf.scheduleFullRedraw());
  view() {
    if (globals.hideSidebar) return null;
    const vdomSections = [];
    for (const section of SECTIONS) {
      if (section.hideIfNoTraceLoaded && !isTraceLoaded()) continue;
      const vdomItems = [];
      for (const item of section.items) {
        if (item.isVisible !== undefined && !item.isVisible()) {
          continue;
        }
        let css = '';
        let attrs = {
          onclick: typeof item.a === 'function' ? item.a : null,
          href: isString(item.a) ? item.a : '#',
          target: isString(item.a) ? '_blank' : null,
          disabled: false,
          id: item.t.toLowerCase().replace(/[^\w]/g, '_'),
        };
        if (item.isPending && item.isPending()) {
          attrs.onclick = (e) => e.preventDefault();
          css = '.pending';
        }
        if (item.internalUserOnly && !globals.isInternalUser) {
          continue;
        }
        if (item.checkMetatracingEnabled || item.checkMetatracingDisabled) {
          if (item.checkMetatracingEnabled === true &&
              !isMetatracingEnabled()) {
            continue;
          }
          if (item.checkMetatracingDisabled === true &&
              isMetatracingEnabled()) {
            continue;
          }
          if (item.checkMetatracingDisabled &&
              !highPrecisionTimersAvailable()) {
            attrs.disabled = true;
          }
        }
        if (item.checkDownloadDisabled && !isDownloadable()) {
          attrs = {
            onclick: (e) => {
              e.preventDefault();
              alert('Can not download external trace.');
            },
            href: '#',
            target: null,
            disabled: true,
            id: '',
          };
        }
        vdomItems.push(m(
            'li', m(`a${css}`, attrs, m('i.material-icons', item.i), item.t)));
      }
      if (section.appendOpenedTraceTitle) {
        const engine = globals.state.engine;
        if (engine !== undefined) {
          let traceTitle = '';
          let traceUrl = '';
          switch (engine.source.type) {
            case 'FILE':
              // Split on both \ and / (because C:\Windows\paths\are\like\this).
              traceTitle = engine.source.file.name.split(/[/\\]/).pop()!;
              const fileSizeMB = Math.ceil(engine.source.file.size / 1e6);
              traceTitle += ` (${fileSizeMB} MB)`;
              break;
            case 'URL':
              traceUrl = engine.source.url;
              traceTitle = traceUrl.split('/').pop()!;
              break;
            case 'ARRAY_BUFFER':
              traceTitle = engine.source.title;
              traceUrl = engine.source.url || '';
              const arrayBufferSizeMB =
                  Math.ceil(engine.source.buffer.byteLength / 1e6);
              traceTitle += ` (${arrayBufferSizeMB} MB)`;
              break;
            case 'HTTP_RPC':
              traceTitle = 'External trace (RPC)';
              break;
            default:
              break;
          }
          if (traceTitle !== '') {
            const tabTitle = `${traceTitle} - Perfetto UI`;
            if (tabTitle !== lastTabTitle) {
              document.title = lastTabTitle = tabTitle;
            }
            vdomItems.unshift(m('li', createTraceLink(traceTitle, traceUrl)));
          }
        }
      }
      vdomSections.push(
          m(`section${section.expanded ? '.expanded' : ''}`,
            m('.section-header',
              {
                onclick: () => {
                  section.expanded = !section.expanded;
                  raf.scheduleFullRedraw();
                },
              },
              m('h1', {title: section.summary}, section.title),
              m('h2', section.summary)),
            m('.section-content', m('ul', vdomItems))));
    }
    return m(
        'nav.sidebar',
        {
          class: globals.state.sidebarVisible ? 'show-sidebar' : 'hide-sidebar',
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
            m('button.sidebar-button',
              {
                onclick: () => {
                  globals.commandManager.runCommand(
                      'dev.perfetto.CoreCommands#ToggleLeftSidebar');
                },
              },
              m('i.material-icons',
                {
                  title: globals.state.sidebarVisible ? 'Hide menu' :
                                                        'Show menu',
                },
                'menu')),
            ),
        m('input.trace_file[type=file]',
          {onchange: onInputElementFileSelectionChanged}),
        m('.sidebar-scroll',
          m(
              '.sidebar-scroll-container',
              ...vdomSections,
              m(SidebarFooter),
              )),
    );
  }
}
