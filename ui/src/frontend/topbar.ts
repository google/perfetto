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

import {searchSegment} from '../base/binary_search';
import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';
import {EngineConfig} from '../common/state';
import {fromNs, TimeSpan, toNs} from '../common/time';

import {globals} from './globals';

const QUERY_ID = 'quicksearch';

const SEARCH = Symbol('search');
const COMMAND = Symbol('command');
type Mode = typeof SEARCH|typeof COMMAND;

const PLACEHOLDER = {
  [SEARCH]: 'Search',
  [COMMAND]: 'e.g. select * from sched left join thread using(utid) limit 10'
};

let selResult = 0;
let numResults = 0;
let mode: Mode = SEARCH;
let displayStepThrough = false;
let prevOmniBox: string|undefined = undefined;

function clearOmniboxResults(e: Event) {
  globals.queryResults.delete(QUERY_ID);
  globals.dispatch(Actions.deleteQuery({queryId: QUERY_ID}));
  const txt = (e.target as HTMLInputElement);
  if (txt.value.length <= 0) {
    mode = SEARCH;
    globals.rafScheduler.scheduleFullRedraw();
  }
}

function onKeyDown(e: Event) {
  e.stopPropagation();
  const key = (e as KeyboardEvent).key;
  const txt = (e.target as HTMLInputElement);

  // Avoid that the global 'a', 'd', 'w', 's' handler sees these keystrokes.
  // TODO: this seems a bug in the pan_and_zoom_handler.ts.
  if (key === 'ArrowUp' || key === 'ArrowDown') {
    e.preventDefault();
    return;
  }

  if (mode === SEARCH && txt.value === '' && key === ':') {
    e.preventDefault();
    mode = COMMAND;
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }

  if (mode === COMMAND && txt.value === '' && key === 'Backspace') {
    mode = SEARCH;
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }
}

function onKeyUp(e: Event) {
  e.stopPropagation();
  const event = (e as KeyboardEvent);
  const state = globals.frontendLocalState;
  const key = event.key;
  const txt = e.target as HTMLInputElement;
  if (key === 'ArrowUp' || key === 'ArrowDown') {
    selResult += (key === 'ArrowUp') ? -1 : 1;
    selResult = Math.max(selResult, 0);
    selResult = Math.min(selResult, numResults - 1);
    e.preventDefault();
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }

  if (key === 'Escape') {
    globals.queryResults.delete(QUERY_ID);
    globals.dispatch(Actions.deleteQuery({queryId: 'command'}));
    mode = SEARCH;
    txt.value = '';
    txt.blur();
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }
  if (mode === COMMAND && key === 'Enter') {
    globals.dispatch(Actions.executeQuery(
        {engineId: '0', queryId: 'command', query: txt.value}));
  }
  if (mode === SEARCH && key === 'Enter') {
    const index = state.searchIndex;
    const startNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
    const endNs = toNs(globals.frontendLocalState.visibleWindowTime.end);
    const currentTs = globals.currentSearchResults.tsStarts[index];
    // If this is a new search or the currentTs is not in the viewport,
    // select the first/last item in the viewport.
    if (index === -1 || currentTs < startNs || currentTs > endNs) {
      if (event.shiftKey) {
        const [smaller,] =
        searchSegment(globals.currentSearchResults.tsStarts, endNs);
        globals.frontendLocalState.setSearchIndex(smaller);
      } else {
        const [, larger] =
            searchSegment(globals.currentSearchResults.tsStarts, startNs);
        globals.frontendLocalState.setSearchIndex(larger);
      }
    } else {
      // If the currentTs is in the viewport, increment the index and move the
      // viewport if necessary.
      if (event.shiftKey) {
        globals.frontendLocalState.setSearchIndex(Math.max(index - 1, 0));
      } else {
        globals.frontendLocalState.setSearchIndex(Math.min(
            index + 1, globals.currentSearchResults.sliceIds.length - 1));
      }
      moveViewportToCurrent();
    }
    displaySearchResults();
  }
}

function moveViewportToCurrent() {
  // Move viewport if our selection moves outside.
  const startNs = toNs(globals.frontendLocalState.visibleWindowTime.start);
  const endNs = toNs(globals.frontendLocalState.visibleWindowTime.end);
  const currentTs = globals.currentSearchResults
                        .tsStarts[globals.frontendLocalState.searchIndex];
  const currentViewNs = endNs - startNs;
  if (currentTs < startNs || currentTs > endNs) {
    // TODO(taylori): This is an ugly jump, we should do a smooth pan instead.
    globals.frontendLocalState.updateVisibleTime(new TimeSpan(
        fromNs(currentTs - currentViewNs / 2),
        fromNs(currentTs + currentViewNs / 2)));
  }
}

function displaySearchResults() {
  const state = globals.frontendLocalState;
  const current = globals.currentSearchResults;
  if (current === undefined) return;
  if (globals.frontendLocalState.omnibox !== prevOmniBox) {
    globals.frontendLocalState.setSearchIndex(-1);
  }
  if (globals.frontendLocalState.omnibox === '' ||
      globals.frontendLocalState.omnibox.length < 4) {
    displayStepThrough = false;
    return;
  }
  displayStepThrough = true;

  const currentId = globals.currentSearchResults.sliceIds[state.searchIndex];
  if (currentId !== undefined) {
    globals.dispatch(Actions.selectSlice({
      utid: globals.currentSearchResults.utids[state.searchIndex],
      id: currentId
    }));
  }
  // TODO(taylori): Here we should zoom to an appropriate level.
  globals.rafScheduler.scheduleFullRedraw();
  prevOmniBox = globals.frontendLocalState.omnibox;
}


class Omnibox implements m.ClassComponent {
  oncreate(vnode: m.VnodeDOM) {
    const txt = vnode.dom.querySelector('input') as HTMLInputElement;
    txt.addEventListener('blur', clearOmniboxResults);
    txt.addEventListener('keydown', onKeyDown);
    txt.addEventListener('keyup', onKeyUp);
  }

  view() {
    displaySearchResults();
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    let enginesAreBusy = false;
    for (const engine of Object.values(globals.state.engines)) {
      enginesAreBusy = enginesAreBusy || !engine.ready;
    }

    if (msgTTL > 0 || enginesAreBusy) {
      setTimeout(
          () => globals.rafScheduler.scheduleFullRedraw(), msgTTL * 1000);
      return m(
          `.omnibox.message-mode`,
          m(`input[placeholder=${globals.state.status.msg}][readonly]`, {
            value: '',
          }));
    }

    // TODO(primiano): handle query results here.
    const results = [];
    const resp = globals.queryResults.get(QUERY_ID) as QueryResponse;
    if (resp !== undefined) {
      numResults = resp.rows ? resp.rows.length : 0;
      for (let i = 0; i < resp.rows.length; i++) {
        const clazz = (i === selResult) ? '.selected' : '';
        results.push(m(`div${clazz}`, resp.rows[i][resp.columns[0]]));
      }
    }
    const commandMode = mode === COMMAND;
    const state = globals.frontendLocalState;
    return m(
        `.omnibox${commandMode ? '.command-mode' : ''}`,
        m('input', {
          placeholder: PLACEHOLDER[mode],
          oninput: m.withAttr(
              'value',
              v => {
                globals.frontendLocalState.omnibox = v;
                if (v === '') {
                  displayStepThrough = false;
                  globals.rafScheduler.scheduleFullRedraw();
                }
              }),
          value: globals.frontendLocalState.omnibox,
        }),
        displayStepThrough ?
            m(
                '.stepthrough',
                m('.current',
                  `${
                      globals.currentSearchResults.totalResults === 0 ?
                          '0 / 0' :
                          `${state.searchIndex + 1} / ${
                              globals.currentSearchResults.totalResults}`}`),
                m('button',
                  {
                    disabled: state.searchIndex <= 0,
                    onclick: () => {
                      globals.frontendLocalState.setSearchIndex(
                          state.searchIndex - 1);
                      moveViewportToCurrent();
                      displaySearchResults();
                    }
                  },
                  m('i.material-icons.left', 'keyboard_arrow_left')),
                m('button',
                  {
                    disabled: state.searchIndex ===
                        globals.currentSearchResults.totalResults - 1,
                    onclick: () => {
                      globals.frontendLocalState.setSearchIndex(
                          state.searchIndex + 1);
                      moveViewportToCurrent();
                      displaySearchResults();
                    }
                  },
                  m('i.material-icons.right', 'keyboard_arrow_right')),
                ) :
            '',
        m('.omnibox-results', results));
  }
}

class Progress implements m.ClassComponent {
  private loading: () => void;
  private progressBar?: HTMLElement;

  constructor() {
    this.loading = () => this.loadingAnimation();
  }

  oncreate(vnodeDom: m.CVnodeDOM) {
    this.progressBar = vnodeDom.dom as HTMLElement;
    globals.rafScheduler.addRedrawCallback(this.loading);
  }

  onremove() {
    globals.rafScheduler.removeRedrawCallback(this.loading);
  }

  view() {
    return m('.progress');
  }

  loadingAnimation() {
    if (this.progressBar === undefined) return;
    const engine: EngineConfig = globals.state.engines['0'];
    if (globals.state.queries[QUERY_ID] !== undefined ||
        (engine !== undefined && !engine.ready) || globals.isLoading) {
      this.progressBar.classList.add('progress-anim');
    } else {
      this.progressBar.classList.remove('progress-anim');
    }
  }
}

export class Topbar implements m.ClassComponent {
  view() {
    return m('.topbar', m(Omnibox), m(Progress));
  }
}
