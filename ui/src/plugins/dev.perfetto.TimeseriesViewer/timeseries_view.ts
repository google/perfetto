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
import {classNames} from '../../base/classnames';
import type {YMode} from '../../components/tracks/counter_track';
import {
  Button,
  ButtonBar,
  ButtonGroup,
  ButtonVariant,
} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {MultiSelect, type MultiSelectDiff} from '../../widgets/multiselect';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {Tooltip} from '../../widgets/tooltip';
import {
  COLOR_CHOICES,
  type CounterSeries,
  type CounterTrackInfo,
  type LineStyle,
  type NameBy,
  counterDescriptor,
  counterLabel,
  swatchClassFor,
} from './counter_index';
import type {ViewerModel} from './model';
import {TimeseriesChart} from './timeseries_chart';

const LINE_STYLES: ReadonlyArray<LineStyle> = ['solid', 'dashed', 'dotted'];
// Cap how many legend chips we render (the strip scrolls); with thousands
// selected we don't want thousands of DOM nodes.
const LEGEND_MAX = 200;
// Default legend strip max-height (px) before the user drags to resize it.
const DEFAULT_LEGEND_H = 66;

export interface TimeseriesViewAttrs {
  readonly model: ViewerModel;
}

// The viewer widget: toolbar + legend + Canvas2D chart for a ViewerModel. It
// holds no lifecycle of its own — the caller owns the model — so the contextual
// area-selection tab and the command-opened drawer tab can share one instance.
export class TimeseriesView implements m.ClassComponent<TimeseriesViewAttrs> {
  // Pending picker selection: edited by the picker's checkboxes/bulk actions
  // and only committed to the model on "Apply". Seeded from the current
  // selection when the picker opens, cleared when it closes.
  private pending?: Set<number>;
  // Active legend-resize drag (dragging the chart's top edge down/up).
  private legendDrag?: {startY: number; startH: number};

  // Seeds the pending set from the model's current selection if not already
  // editing (i.e. the picker just opened).
  private ensurePending(model: ViewerModel): Set<number> {
    if (this.pending === undefined) {
      this.pending = new Set(model.selected.map((s) => s.info.id));
    }
    return this.pending;
  }

  // Popup open/close: discard uncommitted edits when it closes so the next open
  // re-seeds from the (possibly Apply-updated) selection.
  private onPickerToggle(shown: boolean) {
    if (!shown) this.pending = undefined;
    m.redraw();
  }

  // Commits the pending set: adds newly-checked counters and removes unchecked
  // ones so the plotted set matches exactly what's ticked.
  private applyPending(model: ViewerModel) {
    const pending = this.pending;
    if (pending === undefined) return;
    const byId = new Map(model.all.map((i) => [i.id, i]));
    const current = new Set(model.selected.map((s) => s.info.id));
    const toAdd = [...pending]
      .filter((id) => !current.has(id))
      .map((id) => byId.get(id))
      .filter((i): i is CounterTrackInfo => i !== undefined);
    const toRemove = [...current].filter((id) => !pending.has(id));
    if (toAdd.length > 0) model.selectMany(toAdd);
    if (toRemove.length > 0) model.deselectMany(toRemove);
  }

  view({attrs}: m.CVnode<TimeseriesViewAttrs>) {
    const model = attrs.model;
    if (model.loading) {
      return m(
        '.pf-tsv',
        m('.pf-tsv__loading', m(Spinner, {easing: true}), ' Loading counters…'),
      );
    }
    if (model.all.length === 0) {
      return m(
        '.pf-tsv',
        m(EmptyState, {
          icon: 'ssid_chart',
          title: 'No counters in this trace',
          detail: 'This viewer plots counter tracks; this trace has none.',
        }),
      );
    }
    // Nothing selected — offer the picker (the command opens the drawer empty)
    // and point at the timeline-selection path too.
    if (model.selected.length === 0) {
      return m(
        '.pf-tsv',
        m(
          EmptyState,
          {
            icon: 'ssid_chart',
            title: 'Add counters to plot',
            detail:
              'Search and add counters here, or select counter tracks ' +
              '(or a whole group) in the timeline.',
          },
          m(
            Popup,
            {
              position: PopupPosition.Bottom,
              onChange: (shown: boolean) => this.onPickerToggle(shown),
              className: 'pf-tsv__picker-popup',
              trigger: m(Button, {
                label: 'Add counters',
                icon: 'add',
                variant: ButtonVariant.Filled,
              }),
            },
            this.renderPicker(model),
          ),
        ),
      );
    }
    return m(
      '.pf-tsv',
      this.renderToolbar(model),
      this.renderLegend(model),
      this.renderLegendResize(model),
      m('.pf-tsv__body', m(TimeseriesChart, {model})),
    );
  }

  // A draggable splitter under the legend: drag down to enlarge the legend
  // strip (see/scroll more chips), up to shrink it back.
  private renderLegendResize(model: ViewerModel) {
    return m('.pf-tsv__legend-resize', {
      title: 'Drag to resize the legend',
      onpointerdown: (e: PointerEvent) => {
        (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
        this.legendDrag = {
          startY: e.clientY,
          startH: model.legendHeight ?? DEFAULT_LEGEND_H,
        };
        e.preventDefault();
      },
      onpointermove: (e: PointerEvent) => {
        if (this.legendDrag === undefined) return;
        const dy = e.clientY - this.legendDrag.startY;
        model.setLegendHeight(
          Math.max(28, Math.min(this.legendDrag.startH + dy, 480)),
        );
      },
      onpointerup: (e: PointerEvent) => {
        this.legendDrag = undefined;
        (e.currentTarget as HTMLElement).releasePointerCapture?.(e.pointerId);
      },
    });
  }

  private renderToolbar(model: ViewerModel) {
    // A connected segmented control (like the flamegraph's toggles) — the
    // options read as one tab-switch widget rather than separate buttons.
    const seg = (
      opts: ReadonlyArray<{
        label: string;
        active: boolean;
        onclick: () => void;
        title?: string;
      }>,
    ) =>
      m(
        ButtonGroup,
        opts.map((o) =>
          m(Button, {
            label: o.label,
            active: o.active,
            compact: true,
            onclick: o.onclick,
            title: o.title,
          }),
        ),
      );
    const divider = () => m('.pf-tsv__divider');
    return m(
      ButtonBar,
      {className: 'pf-tsv__toolbar'},
      // Add counters: a compact "+" that opens the searchable picker. Counters
      // added here union with any coming from the timeline selection.
      m(
        Popup,
        {
          position: PopupPosition.Bottom,
          onChange: (shown: boolean) => this.onPickerToggle(shown),
          className: 'pf-tsv__picker-popup',
          trigger: m(Button, {
            icon: 'add',
            compact: true,
            title: 'Add counters',
          }),
        },
        this.renderPicker(model),
      ),
      divider(),
      seg([
        {
          label: 'Stacked',
          active: model.layout === 'stacked',
          onclick: () => model.setLayout('stacked'),
        },
        {
          label: 'Overlay',
          active: model.layout === 'overlay',
          onclick: () => model.setLayout('overlay'),
        },
      ]),
      // Per-lane area fill (stacked layout only).
      model.layout === 'stacked' &&
        m(Button, {
          label: 'Area',
          icon: 'area_chart',
          active: model.fill,
          compact: true,
          title: 'Fill the area under each line',
          onclick: () => model.setFill(!model.fill),
        }),
      // Overlay-only: the stacked-area toggle. The % / Units axis, unit filter
      // and y-range mode live in the "Display options" menu to keep this row
      // uncluttered.
      model.layout === 'overlay' &&
        m(Button, {
          label: 'Stacked area',
          icon: 'area_chart',
          active: model.overlayStacked,
          compact: true,
          title:
            'Stack the counters as a cumulative area chart (shared value axis)',
          onclick: () => model.setOverlayStacked(!model.overlayStacked),
        }),
      divider(),
      seg([
        {
          label: 'Value',
          active: model.yMode === 'value',
          onclick: () => void model.setYMode('value').then(m.redraw),
        },
        {
          label: 'Delta',
          active: model.yMode === 'delta',
          onclick: () => void model.setYMode('delta').then(m.redraw),
        },
        {
          label: 'Rate',
          active: model.yMode === 'rate',
          onclick: () => void model.setYMode('rate').then(m.redraw),
        },
      ]),
      divider(),
      m(Button, {
        icon: 'zoom_out_map',
        compact: true,
        title: 'Reset zoom',
        onclick: () => model.resetView(),
      }),
      // Push the trailing controls (display-options menu + help) to the far
      // right of the toolbar.
      m('.pf-tsv__spacer'),
      // Overflow "hamburger" of secondary display options: y-scale and (in
      // stacked mode) lane height. Kept out of the main row to reduce clutter.
      m(
        Popup,
        {
          position: PopupPosition.Bottom,
          trigger: m(Button, {
            icon: 'menu',
            compact: true,
            title: 'Display options',
          }),
        },
        m('.pf-tsv__style-menu', [
          // Y-axis range mode.
          m('.pf-tsv__style-row', [
            m('span.pf-tsv__style-label', 'Range'),
            seg([
              {
                label: 'Global',
                active: model.yRangeMode === 'global',
                title: "Locked to each counter's whole-trace range",
                onclick: () => model.setYRangeMode('global'),
              },
              {
                label: 'Shared',
                active: model.yRangeMode === 'shared',
                title:
                  'One shared axis across all lanes, so heights are comparable',
                onclick: () => model.setYRangeMode('shared'),
              },
              {
                label: 'Fit',
                active: model.yRangeMode === 'fit',
                title: 'Fits the samples currently in view',
                onclick: () => model.setYRangeMode('fit'),
              },
            ]),
          ]),
          m('.pf-tsv__style-row', [
            m('span.pf-tsv__style-label', 'Scale'),
            seg([
              {
                label: 'Linear',
                active: model.yScale === 'linear',
                onclick: () => model.setYScale('linear'),
              },
              {
                label: 'Log',
                active: model.yScale === 'log',
                onclick: () => model.setYScale('log'),
              },
            ]),
          ]),
          model.layout === 'stacked' &&
            m('.pf-tsv__style-row', [
              m('span.pf-tsv__style-label', 'Height'),
              seg([
                {
                  label: 'S',
                  active: model.laneHeight === 64,
                  onclick: () => model.setLaneHeight(64),
                },
                {
                  label: 'M',
                  active: model.laneHeight === 96,
                  onclick: () => model.setLaneHeight(96),
                },
                {
                  label: 'L',
                  active: model.laneHeight === 140,
                  onclick: () => model.setLaneHeight(140),
                },
              ]),
            ]),
          // Overlay axis: normalised 0-100% per line, or one shared unit axis.
          model.layout === 'overlay' &&
            m('.pf-tsv__style-row', [
              m('span.pf-tsv__style-label', 'Axis'),
              seg([
                {
                  label: '%',
                  active: model.overlayNormalize && !model.overlayStacked,
                  title: 'Each line normalised to its own range (0-100%)',
                  onclick: () => {
                    model.setOverlayStacked(false);
                    model.setOverlayNormalize(true);
                  },
                },
                {
                  label: 'Units',
                  active: !model.overlayNormalize || model.overlayStacked,
                  title: "One shared value axis in the counters' unit",
                  onclick: () => model.setOverlayNormalize(false),
                },
              ]),
            ]),
          // Scope the value/stacked axis to one unit when the selection mixes
          // units (a shared axis across units is meaningless).
          model.layout === 'overlay' &&
            !model.overlayNormalize &&
            model.selectedUnits().length > 1 &&
            m('.pf-tsv__style-row', [
              m('span.pf-tsv__style-label', 'Unit'),
              seg([
                {
                  label: 'All',
                  active: model.overlayUnitFilter === undefined,
                  onclick: () => model.setOverlayUnitFilter(undefined),
                },
                ...model.selectedUnits().map((u) => ({
                  label: u,
                  active: model.overlayUnitFilter === u,
                  onclick: () => model.setOverlayUnitFilter(u),
                })),
              ]),
            ]),
        ]),
      ),
      m(
        Tooltip,
        {trigger: m(Button, {icon: 'help_outline', compact: true})},
        m('.pf-tsv__help', [
          m('div', 'WASD or pinch — pan & zoom'),
          m('div', 'drag — select a range to zoom'),
          m('div', 'shift-drag — pan'),
          m('div', 'wheel — scroll lanes'),
          m('div', 'double-click — reset zoom'),
          m('div', 'drag a lane / plot bottom edge — resize height'),
        ]),
      ),
    );
  }

  // Clickable legend: one chip per selected counter. Clicking the name toggles
  // the line's visibility; the gear opens a per-line style/colour menu.
  private renderLegend(model: ViewerModel) {
    const extra = model.selected.length - LEGEND_MAX;
    return m(
      '.pf-tsv__legend',
      {style: {maxHeight: `${model.legendHeight ?? DEFAULT_LEGEND_H}px`}},
      // Bulk visibility: hide everything, then click chips to bring lines back
      // one at a time (handy for isolating counters when many overlap).
      model.selected.length > 1 &&
        m('.pf-tsv__legend-actions', [
          m(Button, {
            label: 'Hide all',
            compact: true,
            title: 'Hide every line, then click a counter to show it',
            onclick: () => model.setAllHidden(true),
          }),
          m(Button, {
            label: 'Show all',
            compact: true,
            onclick: () => model.setAllHidden(false),
          }),
        ]),
      model.selected.slice(0, LEGEND_MAX).map((s, i) =>
        m(
          '.pf-tsv__chip',
          {
            key: s.info.id,
            className: classNames(
              s.hidden && 'pf-tsv__chip--off',
              s.emphasized && 'pf-tsv__chip--emphasized',
            ),
          },
          m(`span.pf-tsv__swatch.${swatchClassFor(s.colorOverride, i)}`),
          m(
            'span.pf-tsv__chip-name',
            {
              onclick: () => model.toggleHidden(s),
              title: 'Click to show/hide this counter',
            },
            s.label(model.nameBy),
          ),
          m(
            Popup,
            {
              position: PopupPosition.Bottom,
              trigger: m(Button, {
                icon: 'tune',
                title: 'Line style & colour',
                compact: true,
              }),
            },
            this.renderStyleMenu(model, s),
          ),
          // Remove this counter from the chart (deselect) directly from the
          // legend, without reopening the picker.
          m(Button, {
            icon: 'close',
            compact: true,
            title: 'Remove from chart',
            onclick: () => model.deselectMany([s.info.id]),
          }),
        ),
      ),
      extra > 0 &&
        m('span.pf-tsv__legend-more', `+${extra.toLocaleString()} more`),
    );
  }

  private renderStyleMenu(model: ViewerModel, s: CounterSeries) {
    const modes: ReadonlyArray<YMode> = ['value', 'delta', 'rate'];
    const cap = (x: string) => x.charAt(0).toUpperCase() + x.slice(1);
    return m('.pf-tsv__style-menu', [
      // Per-line rename: empty falls back to the naming-scheme default (shown as
      // the placeholder).
      m('.pf-tsv__style-row', [
        m('span.pf-tsv__style-label', 'Name'),
        m(TextInput, {
          value: s.nameOverride ?? '',
          placeholder: counterLabel(s.info, model.nameBy),
          onInput: (v: string) => model.setName(s, v),
        }),
      ]),
      m('.pf-tsv__style-row', [
        m('span.pf-tsv__style-label', 'Mode'),
        m(
          '.pf-tsv__seg',
          modes.map((md) =>
            m(Button, {
              label: cap(md),
              active: s.mode === md,
              compact: true,
              onclick: () => void model.setSeriesYMode(s, md).then(m.redraw),
            }),
          ),
        ),
      ]),
      m('.pf-tsv__style-row', [
        m('span.pf-tsv__style-label', 'Line'),
        m(
          '.pf-tsv__seg',
          LINE_STYLES.map((st) =>
            m(Button, {
              label: cap(st),
              active: s.lineStyle === st,
              compact: true,
              onclick: () => model.setLineStyle(s, st),
            }),
          ),
        ),
      ]),
      m('.pf-tsv__style-row', [
        m('span.pf-tsv__style-label', 'Colour'),
        m('.pf-tsv__colors', [
          ...COLOR_CHOICES.map((c, ci) =>
            m(`span.pf-tsv__color-dot.pf-tsv__color-dot--${ci}`, {
              className: s.colorOverride === c ? 'pf-tsv__color-dot--sel' : '',
              title: c,
              onclick: () => model.setColor(s, c),
            }),
          ),
          m(
            'span.pf-tsv__color-reset',
            {
              title: 'Default colour',
              onclick: () => model.setColor(s, undefined),
            },
            'reset',
          ),
        ]),
      ]),
    ]);
  }

  // The picker (ftrace-event-filter style): a MultiSelect with search, Select
  // All/Filtered and Clear, editing a *pending* set that only takes effect on
  // Apply. Options are labelled with the disambiguating owner+metric descriptor
  // (independent of the naming scheme) and searchable by metric or owner.
  private renderPicker(model: ViewerModel) {
    const pending = this.ensurePending(model);
    const cap = (x: string) => x.charAt(0).toUpperCase() + x.slice(1);
    const nameByOpts: ReadonlyArray<NameBy> = ['metric', 'process', 'thread'];
    const options = model.all.map((t) => ({
      id: String(t.id),
      name: counterDescriptor(t),
      checked: pending.has(t.id),
    }));
    const dirty =
      pending.size !== model.selected.length ||
      model.selected.some((s) => !pending.has(s.info.id));
    return m('.pf-tsv__picker', [
      // How lines are named on the chart (per-line rename still wins).
      m('.pf-tsv__picker-nameby', [
        m('span.pf-tsv__picker-label', 'Name by'),
        m(
          ButtonGroup,
          nameByOpts.map((nb) =>
            m(Button, {
              label: cap(nb),
              active: model.nameBy === nb,
              compact: true,
              onclick: () => model.setNameBy(nb),
            }),
          ),
        ),
      ]),
      m(MultiSelect, {
        options,
        repeatCheckedItemsAtTop: true,
        showNumSelected: true,
        onChange: (diffs: MultiSelectDiff[]) => {
          for (const {id, checked} of diffs) {
            if (checked) pending.add(Number(id));
            else pending.delete(Number(id));
          }
        },
      }),
      // Explicit commit step — nothing plots until Apply.
      m('.pf-tsv__picker-apply', [
        m(
          'span.pf-tsv__picker-count',
          `${pending.size.toLocaleString()} selected`,
        ),
        m(Button, {
          label: 'Apply',
          variant: ButtonVariant.Filled,
          disabled: !dirty,
          dismissPopup: true,
          onclick: () => this.applyPending(model),
        }),
        m(Button, {
          label: 'Cancel',
          dismissPopup: true,
          onclick: () => (this.pending = undefined),
        }),
      ]),
    ]);
  }
}
