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

import m from 'mithril';
import {Button, ButtonGroup, ButtonVariant} from '../../widgets/button';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {
  BigtraceQueryClient,
  type ExperimentMetadataItem,
  QueryCancelledError,
} from '../query/bigtrace_query_client';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import type {ExperimentFilterState} from './query_tabs_state';

// What the backend says about an arm the caller can't query. Shown wherever
// that arm's name is, so the limitation travels with the name.
export const DENIED_PREFIX = '(Not available in Telemetry datasets)';

// Long enough that typing a few characters doesn't fire a request per
// keystroke, short enough to feel immediate.
const SEARCH_DEBOUNCE_MS = 500;

export interface ExperimentFilterControlAttrs {
  readonly bindings: SettingsBindings;
}

// Ids and names of one arm, as displayed everywhere: the id first (short,
// stable, always legible) and the name after it, free to be clamped.
function armName(name: string | undefined, denied: boolean | undefined) {
  const shown = name ?? '';
  if (denied !== true) return shown;
  return shown === '' ? DENIED_PREFIX : `${DENIED_PREFIX} ${shown}`;
}

// One line of both arms in full, for tooltips where clamping would hide the
// part that distinguishes two similarly-named experiments.
export function describeExperimentFilter(f: ExperimentFilterState): string {
  const experiment = armName(f.experimentName, f.experimentDenied);
  const control = armName(f.controlName, f.controlDenied);
  return (
    `Experiment #${f.experimentId}${experiment === '' ? '' : ` ${experiment}`}\n` +
    `Control #${f.controlId}${control === '' ? '' : ` ${control}`}`
  );
}

// The active arm, for the chip and the inline panel: "#123 name", or just
// the id until the catalog answers.
export function summarizeExperimentFilter(f: ExperimentFilterState): string {
  const id = f.isTreatment ? f.experimentId : f.controlId;
  const name = f.isTreatment
    ? armName(f.experimentName, f.experimentDenied)
    : armName(f.controlName, f.controlDenied);
  return name === '' ? `#${id}` : `#${id} ${name}`;
}

// Ids resolved to names, once per id per session. A filter can arrive with
// ids alone — from history, a preset, or a reloaded tab — and every surface
// that displays one asks for the names it needs.
type NameLookupState = 'pending' | 'done';
const nameLookups = new Map<number, NameLookupState>();

export function ensureExperimentNames(bindings: SettingsBindings): void {
  const filter = bindings.getExperimentFilter();
  if (filter === undefined) return;
  // Names present, or already asked: an unknown id answers with no
  // experiment, which is not an error and must not be retried per render.
  if (filter.experimentName !== undefined) return;
  if (nameLookups.get(filter.experimentId) !== undefined) return;
  const endpoint = getBigtraceEndpoint();
  if (endpoint === '') return;

  const experimentId = filter.experimentId;
  nameLookups.set(experimentId, 'pending');
  const client = new BigtraceQueryClient(endpoint);
  client
    .getExperimentMetadata(experimentId)
    .then((item) => {
      nameLookups.set(experimentId, 'done');
      if (item === undefined) return;
      // The filter may have been cleared, cancelled or re-picked while this
      // was in flight; only the one we asked about may be relabelled.
      const current = bindings.getExperimentFilter();
      if (current === undefined || current.experimentId !== experimentId) {
        return;
      }
      bindings.setExperimentFilter({...current, ...namesOf(item)});
      // The bindings only mark the tab dirty; nothing else repaints a view
      // that has been waiting on this.
      m.redraw();
    })
    .catch(() => {
      // A missing name is cosmetic — the filter runs on ids.
      nameLookups.set(experimentId, 'done');
    });
}

function namesOf(item: ExperimentMetadataItem) {
  return {
    experimentName: item.experimentName,
    controlName: item.controlName,
    experimentDenied: item.isExperimentDenied,
    controlDenied: item.isControlDenied,
  };
}

function filterFromItem(item: ExperimentMetadataItem): ExperimentFilterState {
  return {
    experimentId: item.experimentId,
    controlId: item.controlId,
    // The experiment arm is what someone reaching for an experiment wants.
    isTreatment: true,
    ...namesOf(item),
  };
}

// Picks the experiment/control pair a query runs over. Sits in the trace
// grid's header: what it selects is what the grid shows and what a run
// processes.
export class ExperimentFilterControl implements m.ClassComponent<ExperimentFilterControlAttrs> {
  private popupOpen = false;
  private query = '';
  private results: ReadonlyArray<ExperimentMetadataItem> = [];
  private searching = false;
  private error?: string;
  // Only the newest search may write results: a slow earlier one must not
  // overwrite what the user narrowed to since.
  private sequence = 0;
  private debounceTimer?: ReturnType<typeof setTimeout>;
  // The search currently in flight, so a superseded one stops costing the
  // backend the moment it is superseded.
  private inFlight?: AbortController;

  view({attrs}: m.Vnode<ExperimentFilterControlAttrs>): m.Children {
    const {bindings} = attrs;
    const filter = bindings.getExperimentFilter();
    if (filter !== undefined) {
      ensureExperimentNames(bindings);
      return this.renderActive(bindings, filter);
    }
    return this.renderPicker(bindings);
  }

  onremove(): void {
    clearTimeout(this.debounceTimer);
    this.abortSearch();
  }

  // Stop asking: the browser drops the response and closes the connection,
  // and a backend that watches for the disconnect stops the work too.
  private abortSearch(): void {
    this.inFlight?.abort();
    this.inFlight = undefined;
  }

  // Chosen: the arm toggle, the active arm spelled out, and the way back to
  // no experiment filtering.
  private renderActive(
    bindings: SettingsBindings,
    filter: ExperimentFilterState,
  ): m.Children {
    const setArm = (isTreatment: boolean) =>
      bindings.setExperimentFilter({...filter, isTreatment});
    return m('.pf-bt-experiment-filter', [
      // Welded rather than two loose buttons: the inner border is de-duped,
      // so the pair reads as one control with two states, which is what
      // picking an arm is. Both arms are outlined so the border belongs to
      // the pair; the chosen one is the one that looks pressed.
      m(
        ButtonGroup,
        m(Button, {
          label: 'Experiment',
          variant: ButtonVariant.Outlined,
          active: filter.isTreatment,
          onclick: () => setArm(true),
        }),
        m(Button, {
          label: 'Control',
          variant: ButtonVariant.Outlined,
          active: !filter.isTreatment,
          onclick: () => setArm(false),
        }),
      ),
      m(
        '.pf-bt-experiment-filter__arm',
        {title: describeExperimentFilter(filter)},
        summarizeExperimentFilter(filter),
      ),
      m(Button, {
        icon: 'close',
        title: 'Clear the experiment filter',
        onclick: () => bindings.setExperimentFilter(undefined),
      }),
    ]);
  }

  private renderPicker(bindings: SettingsBindings): m.Children {
    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: 'science',
          label: 'Experiment Filter',
          className: 'pf-bt-experiment-filter__open',
          onclick: () => this.openPopup(),
        }),
        position: PopupPosition.BottomEnd,
        // Long names need the room; the default popup width would squeeze
        // every row into an unreadable column.
        fitContent: true,
        className: 'pf-bt-experiment-popup',
        isOpen: this.popupOpen,
        onChange: (shouldOpen: boolean) => {
          if (shouldOpen) {
            this.openPopup();
          } else {
            // Closing throws away the results, so stop paying for them; the
            // control outlives its popup, so nothing else would.
            this.popupOpen = false;
            this.abortSearch();
          }
        },
      },
      m('.pf-bt-experiment-picker', [
        m(TextInput, {
          autofocus: true,
          leftIcon: 'search',
          placeholder: 'Search experiments…',
          value: this.query,
          onInput: (value: string) => this.onQueryInput(value),
        }),
        m('.pf-bt-experiment-picker__body', this.renderResults(bindings)),
      ]),
    );
  }

  private renderResults(bindings: SettingsBindings): m.Children {
    if (this.error !== undefined) {
      return m('.pf-bt-experiment-picker__note', this.error);
    }
    if (this.searching) return m(Spinner);
    if (this.query.trim() === '') {
      return m('.pf-bt-experiment-picker__note', 'Type to search experiments.');
    }
    if (this.results.length === 0) {
      return m('.pf-bt-experiment-picker__note', 'No experiments match.');
    }
    return this.results.map((item) =>
      m(
        '.pf-bt-experiment-picker__row',
        {
          key: item.experimentId,
          onclick: () => {
            bindings.setExperimentFilter(filterFromItem(item));
            this.popupOpen = false;
          },
        },
        [
          renderArmLine(
            'Experiment',
            item.experimentId,
            item.experimentName,
            item.isExperimentDenied,
          ),
          renderArmLine(
            'Control',
            item.controlId,
            item.controlName,
            item.isControlDenied,
          ),
        ],
      ),
    );
  }

  private openPopup(): void {
    this.popupOpen = true;
    // A search always starts from nothing: stale text above fresh results
    // reads as a filter that isn't applied.
    this.query = '';
    this.results = [];
    this.error = undefined;
    this.searching = false;
    clearTimeout(this.debounceTimer);
    this.abortSearch();
  }

  private onQueryInput(value: string): void {
    this.query = value;
    clearTimeout(this.debounceTimer);
    // Matching everything is a round-trip nobody can use: an empty box is
    // the prompt to type, not a request.
    if (value.trim() === '') {
      this.sequence++;
      this.abortSearch();
      this.results = [];
      this.searching = false;
      this.error = undefined;
      return;
    }
    this.searching = true;
    this.error = undefined;
    this.debounceTimer = setTimeout(
      () => void this.search(value.trim()),
      SEARCH_DEBOUNCE_MS,
    );
  }

  private async search(query: string): Promise<void> {
    const endpoint = getBigtraceEndpoint();
    if (endpoint === '') {
      this.searching = false;
      this.error = 'Connect a backend to search experiments.';
      m.redraw();
      return;
    }
    const mySequence = ++this.sequence;
    // Whatever was being asked is no longer what the user wants to know.
    this.abortSearch();
    const controller = new AbortController();
    this.inFlight = controller;
    try {
      const items = await new BigtraceQueryClient(endpoint).listExperiments(
        query,
        controller.signal,
      );
      if (mySequence !== this.sequence) return;
      this.results = items;
    } catch (e) {
      if (mySequence !== this.sequence) return;
      // A search we cancelled ourselves is not a failure to report.
      if (e instanceof QueryCancelledError) return;
      this.results = [];
      this.error = e instanceof Error ? e.message : 'Search failed.';
    } finally {
      if (this.inFlight === controller) this.inFlight = undefined;
      if (mySequence === this.sequence) {
        this.searching = false;
        m.redraw();
      }
    }
  }
}

// "Experiment  #123  a long name…" — the id leads because it is short and
// identifying; the name takes the remaining width and clamps.
function renderArmLine(
  label: string,
  id: number,
  name: string,
  denied: boolean,
): m.Children {
  const shown = armName(name, denied);
  return m('.pf-bt-experiment-picker__arm', [
    m('span.pf-bt-experiment-picker__label', label),
    m('span.pf-bt-experiment-picker__id', `#${id}`),
    m(
      'span.pf-bt-experiment-picker__name',
      {
        className: denied ? 'pf-bt-experiment-picker__name--denied' : undefined,
        title: shown,
      },
      shown,
    ),
  ]);
}
