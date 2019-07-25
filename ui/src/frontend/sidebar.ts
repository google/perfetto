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

import * as m from 'mithril';

import {Actions} from '../common/actions';

import {globals} from './globals';
import {
  isLegacyTrace,
  openFileWithLegacyTraceViewer,
} from './legacy_trace_viewer';

const ALL_PROCESSES_QUERY = 'select name, pid from process order by name;';

const CPU_TIME_FOR_PROCESSES = `
select
  process.name,
  tot_proc/1e9 as cpu_sec
from
  (select
    upid,
    sum(tot_thd) as tot_proc
  from
    (select
      utid,
      sum(dur) as tot_thd
    from sched group by utid)
  join thread using(utid) group by upid)
join process using(upid)
order by cpu_sec desc limit 100;`;

const CYCLES_PER_P_STATE_PER_CPU = `
select
  cpu,
  freq,
  dur,
  sum(dur * freq)/1e6 as mcycles
from (
  select
    ref as cpu,
    value as freq,
    lead(ts) over (partition by ref order by ts) - ts as dur
  from counters
  where name = 'cpufreq'
) group by cpu, freq
order by mcycles desc limit 32;`;

const CPU_TIME_BY_CLUSTER_BY_PROCESS = `
select process.name as process, thread, core, cpu_sec from (
  select thread.name as thread, upid,
    case when cpug = 0 then 'little' else 'big' end as core,
    cpu_sec from (select cpu/4 as cpug, utid, sum(dur)/1e9 as cpu_sec
    from sched group by utid, cpug order by cpu_sec desc
  ) inner join thread using(utid)
) inner join process using(upid) limit 30;`;


const SQL_STATS = `
with first as (select started as ts from sqlstats limit 1)
select query,
    round((max(ended - started, 0))/1e6) as runtime_ms,
    round((max(started - queued, 0))/1e6) as latency_ms,
    round((started - first.ts)/1e6) as t_start_ms
from sqlstats, first
order by started desc`;

const TRACE_STATS = 'select * from stats order by severity, source, name, idx';

function createCannedQuery(query: string): (_: Event) => void {
  return (e: Event) => {
    e.preventDefault();
    globals.dispatch(Actions.executeQuery({
      engineId: '0',
      queryId: 'command',
      query,
    }));
  };
}

const EXAMPLE_ANDROID_TRACE_URL =
    'https://storage.googleapis.com/perfetto-misc/example_android_trace_30s_1';

const EXAMPLE_CHROME_TRACE_URL =
    'https://storage.googleapis.com/perfetto-misc/example_chrome_trace_4s_1.json';

const SECTIONS = [
  {
    title: 'Navigation',
    summary: 'Open or record a new trace',
    expanded: true,
    items: [
      {t: 'Open trace file', a: popupFileSelectionDialog, i: 'folder_open'},
      {t: 'Record new trace', a: navigateRecord, i: 'fiber_smart_record'},
      {t: 'Show timeline', a: navigateViewer, i: 'line_style'},
      {
        t: 'Share current trace',
        a: dispatchCreatePermalink,
        i: 'share',
        disableInLocalOnlyMode: true,
      },
      {
        t: 'Download current trace',
        a: downloadTrace,
        i: 'file_download',
        disableInLocalOnlyMode: true,
      },
    ],
  },
  {
    title: 'Legacy UI',
    expanded: true,
    summary: 'Open trace with legacy UI',
    items: [
      {
        t: 'Open with legacy UI',
        a: popupFileSelectionDialogOldUI,
        i: 'folder_open'
      },
      {
        t: 'Truncate and open',
        a: popupFileSelectionDialogOldUITruncate,
        i: 'flip'
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
        i: 'description'
      },
      {
        t: 'Open Chrome example',
        a: openTraceUrl(EXAMPLE_CHROME_TRACE_URL),
        i: 'description'
      },
    ],
  },
  {
    title: 'Metrics and auditors',
    summary: 'Compute summary statistics',
    items: [
      {
        t: 'All Processes',
        a: createCannedQuery(ALL_PROCESSES_QUERY),
        i: 'search',
      },
      {
        t: 'CPU Time by process',
        a: createCannedQuery(CPU_TIME_FOR_PROCESSES),
        i: 'search',
      },
      {
        t: 'Cycles by p-state by CPU',
        a: createCannedQuery(CYCLES_PER_P_STATE_PER_CPU),
        i: 'search',
      },
      {
        t: 'CPU Time by cluster by process',
        a: createCannedQuery(CPU_TIME_BY_CLUSTER_BY_PROCESS),
        i: 'search',
      },
      {
        t: 'Trace stats',
        a: createCannedQuery(TRACE_STATS),
        i: 'bug_report',
      },
      {
        t: 'Debug SQL performance',
        a: createCannedQuery(SQL_STATS),
        i: 'bug_report',
      },
    ],
  },
  {
    title: 'Support',
    summary: 'Documentation & Bugs',
    items: [
      {
        t: 'Documentation',
        a: 'https://perfetto.dev',
        i: 'help',
      },
      {
        t: 'Report a bug',
        a: 'https://goto.google.com/perfetto-ui-bug',
        i: 'bug_report',
      },
    ],
  },

];

const vidSection = {
  title: 'Video',
  summary: 'Open a screen recording',
  expanded: true,
  items: [
    {t: 'Open video file', a: popupVideoSelectionDialog, i: 'folder_open'},
    {t: 'View hotkeys', a: showHotkeys, i: 'description'},
  ],
};

function showHotkeys() {
  let hks = '';
  hks += '\'v\': Shows/hides video components \n';
  hks += '\'p\': Enables/disables pausing and flagging synchronization \n';
  alert(hks);
  return false;
}

function getFileElement(): HTMLInputElement {
  return document.querySelector('input[type=file]')! as HTMLInputElement;
}

function popupFileSelectionDialog(e: Event) {
  e.preventDefault();
  delete getFileElement().dataset['useCatapultLegacyUi'];
  delete getFileElement().dataset['video'];
  delete getFileElement().dataset['truncate'];
  getFileElement().click();
}

function popupFileSelectionDialogOldUI(e: Event) {
  e.preventDefault();
  delete getFileElement().dataset['video'];
  delete getFileElement().dataset['truncate'];
  getFileElement().dataset['useCatapultLegacyUi'] = '1';
  getFileElement().click();
}

function popupFileSelectionDialogOldUITruncate(e: Event) {
  e.preventDefault();
  delete getFileElement().dataset['video'];
  getFileElement().dataset['useCatapultLegacyUi'] = '1';
  getFileElement().dataset['truncate'] = '1';
  getFileElement().click();
}

function popupVideoSelectionDialog(e: Event) {
  e.preventDefault();
  delete getFileElement().dataset['useCatapultLegacyUi'];
  getFileElement().dataset['video'] = '1';
  getFileElement().click();
}

function openTraceUrl(url: string): (e: Event) => void {
  return e => {
    e.preventDefault();
    globals.frontendLocalState.localOnlyMode = false;
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

  globals.frontendLocalState.localOnlyMode = false;

  if (e.target.dataset['useCatapultLegacyUi'] === '1') {
    // Switch back to the old catapult UI.
    if (isLegacyTrace(file.name)) {
      openFileWithLegacyTraceViewer(file);
    } else {
      if (e.target.dataset['truncate'] === '1') {
        globals.dispatch(Actions.convertTraceToJson({file, truncate: true}));
        return;
      } else if (file.size > 1024 * 1024 * 50) {
        const size = Math.round(file.size / (1024 * 1024));
        const result = confirm(
            `This trace is ${size}mb, opening it in ` +
            `the legacy UI may fail.\nPress 'OK' to attempt to open this ` +
            `trace or press 'Cancel' and use the 'Truncate' button ` +
            `to load just the first 50mb.\nMore options can be found at ` +
            `go/opening-large-traces.`);
        if (!result) return;
      }
      globals.dispatch(Actions.convertTraceToJson({file, truncate: false}));
    }
    return;
  }

  if (e.target.dataset['video'] === '1') {
    globals.dispatch(Actions.openVideoFromFile({file}));
  } else {
    globals.dispatch(Actions.openTraceFromFile({file}));
  }
}

function navigateRecord(e: Event) {
  e.preventDefault();
  globals.dispatch(Actions.navigate({route: '/record'}));
}

function navigateViewer(e: Event) {
  e.preventDefault();
  globals.dispatch(Actions.navigate({route: '/viewer'}));
}

function localOnlyMode(): boolean {
  if (globals.frontendLocalState.localOnlyMode) return true;
  const engine = Object.values(globals.state.engines)[0];
  if (!engine) return true;
  const src = engine.source;
  return (src instanceof ArrayBuffer);
}

function dispatchCreatePermalink(e: Event) {
  e.preventDefault();
  if (localOnlyMode()) return;

  const result = confirm(
      `Upload the trace and generate a permalink. ` +
      `The trace will be accessible by anybody with the permalink.`);
  if (result) globals.dispatch(Actions.createPermalink({}));
}

function downloadTrace(e: Event) {
  e.preventDefault();
  if (localOnlyMode()) return;

  const engine = Object.values(globals.state.engines)[0];
  if (!engine) return;
  const src = engine.source;
  if (typeof src === 'string') {
    window.open(src);
  } else if (src instanceof ArrayBuffer) {
    console.error('Can not download external trace.');
    return;
  } else {
    const url = URL.createObjectURL(src);
    const a = document.createElement('a');
    a.href = url;
    a.download = src.name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }
}


export class Sidebar implements m.ClassComponent {
  private displaySidebar = 'show-sidebar';

  private showing(): boolean {
    return this.displaySidebar === 'show-sidebar';
  }

  view() {
    const vdomSections = [];
    for (const section of SECTIONS) {
      const vdomItems = [];
      for (const item of section.items) {
        let attrs = {
          onclick: typeof item.a === 'function' ? item.a : null,
          href: typeof item.a === 'string' ? item.a : '#',
          disabled: false,
        };
        if (globals.frontendLocalState.localOnlyMode &&
            item.hasOwnProperty('disableInLocalOnlyMode')) {
          attrs = {
            onclick: () => alert('Can not download or share external trace.'),
            href: '#',
            disabled: true
          };
        }
        vdomItems.push(
            m('li', m('a', attrs, m('i.material-icons', item.i), item.t)));
      }
      vdomSections.push(
          m(`section${section.expanded ? '.expanded' : ''}`,
            m('.section-header',
              {
                onclick: () => {
                  section.expanded = !section.expanded;
                  globals.rafScheduler.scheduleFullRedraw();
                }
              },
              m('h1', section.title),
              m('h2', section.summary), ),
            m('.section-content', m('ul', vdomItems))));
    }
    if (globals.state.videoEnabled) {
      const videoVdomItems = [];
      for (const item of vidSection.items) {
        videoVdomItems.push(
          m('li',
            m(`a`,
              {
                onclick: typeof item.a === 'function' ? item.a : null,
                href: typeof item.a === 'string' ? item.a : '#',
              },
              m('i.material-icons', item.i),
              item.t)));
      }
      vdomSections.push(
        m(`section${vidSection.expanded ? '.expanded' : ''}`,
          m('.section-header',
            {
              onclick: () => {
                vidSection.expanded = !vidSection.expanded;
                globals.rafScheduler.scheduleFullRedraw();
              }
            },
            m('h1', vidSection.title),
            m('h2', vidSection.summary), ),
          m('.section-content', m('ul', videoVdomItems))));
    }
    return m(
        `nav.sidebar.${this.displaySidebar}`,
        m('header',
          'Perfetto',
          m('.sidebar-button',
            {
              style: {
                left: (this.showing()) ? '46px' : '96px',
              }
            },
            m('button',
              m('i.material-icons',
                {
                  title: (this.showing()) ? 'Hide menu' : 'Show menu',
                  onclick: () => {
                    this.displaySidebar =
                        (this.showing()) ? 'hide-sidebar' : 'show-sidebar';
                    globals.rafScheduler.scheduleFullRedraw();
                  },
                },
                'menu')), )),
        m('input[type=file]', {onchange: onInputElementFileSelectionChanged}),
        ...vdomSections);
  }
}
