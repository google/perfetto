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

import {navigate, openTraceFromFile, openTraceFromUrl} from '../common/actions';

import {globals} from './globals';
import {quietHandler} from './mithril_helpers';

const EXAMPLE_TRACE_URL =
    'https://storage.googleapis.com/perfetto-misc/example_trace_30s';

const SECTIONS = [
  {
    title: 'Traces',
    summary: 'Open or record a trace',
    expanded: true,
    items: [
      {t: 'Open trace file', a: popupFileSelectionDialog, i: 'folder_open'},
      {t: 'Open example trace', a: handleOpenTraceURL, i: 'description'},
      {t: 'Record new trace', a: navigateHome, i: 'fiber_smart_record'},
      {t: 'Share current trace', a: navigateHome, i: 'share'},
    ],
  },
  {
    title: 'Workspaces',
    summary: 'Custom and pre-arranged views',
    items: [
      {t: 'Big Picture', a: navigateHome, i: 'art_track'},
      {t: 'Apps and process', a: navigateHome, i: 'apps'},
      {t: 'Storage and I/O', a: navigateHome, i: 'storage'},
      {t: 'Add custom...', a: navigateHome, i: 'library_add'},
    ],
  },
  {
    title: 'Tracks and views',
    summary: 'Add new tracks to the workspace',
    items: [
      {t: 'User interactions', a: navigateHome, i: 'touch_app'},
      {t: 'Device info', a: navigateHome, i: 'perm_device_information'},
      {t: 'Scheduler trace', a: navigateHome, i: 'blur_linear'},
      {t: 'Process list', a: navigateHome, i: 'equalizer'},
      {t: 'Battery and power', a: navigateHome, i: 'battery_alert'},
    ],
  },
  {
    title: 'Metrics and auditors',
    summary: 'Add new tracks to the workspace',
    items: [
      {t: 'CPU Usage breakdown', a: navigateHome, i: 'table_chart'},
      {t: 'Memory breakdown', a: navigateHome, i: 'memory'},
    ],
  },
];

function popupFileSelectionDialog(e: Event) {
  e.preventDefault();
  (document.querySelector('input[type=file]')! as HTMLInputElement).click();
}

function handleOpenTraceURL(e: Event) {
  e.preventDefault();
  globals.dispatch(openTraceFromUrl(EXAMPLE_TRACE_URL));
}

function onInputElementFileSelectionChanged(e: Event) {
  if (!(e.target instanceof HTMLInputElement)) {
    throw new Error('Not an input element');
  }
  if (!e.target.files) return;
  globals.dispatch(openTraceFromFile(e.target.files[0]));
}

function stopClickPropagation(e: Event) {
  e.stopImmediatePropagation();
}

function navigateHome(_: Event) {
  globals.dispatch(navigate('/'));
}


export const Sidebar: m.Component<{}, {}> = {
  view() {
    const vdomSections = [];
    for (const section of SECTIONS) {
      const vdomItems = [];
      for (const item of section.items) {
        vdomItems.push(
            m('li',
              m(`a[href=#]`,
                {onclick: quietHandler(item.a)},
                m('i.material-icons', item.i),
                item.t)));
      }
      vdomSections.push(
          m(`section${section.expanded ? '.expanded' : ''}`,
            {onclick: () => section.expanded = !section.expanded},
            m('h1', section.title),
            m('h2', section.summary),
            m('.section-content',
              // Prevent that the clicks on the bottom part of the expanded
              // sections propagate up to and trigger their compression.
              m('ul', {onclick: stopClickPropagation}, vdomItems))));
    }
    return m(
        'nav.sidebar',
        m('header', 'Perfetto'),
        m('input[type=file]',
          {onchange: quietHandler(onInputElementFileSelectionChanged)}),
        ...vdomSections);
  },
};