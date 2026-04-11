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

// Toolbar and tab-strip for the GPU Compute tab.
//
// Renders the kernel-selection dropdown, current/baseline metric cards,
// the Details/Summary/Analysis tab strip, and a View popup with
// unit-humanization and terminology toggles.
//
// Module-level state:
// - {@link enableHumanizeMetrics}: whether raw values are auto-converted
// - {@link activeInfoTab}: which sub-tab (details | summary | analysis)

import m from 'mithril';
import type {KernelLaunchOption, ToolbarInfo} from './details';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Button, ButtonVariant} from '../../widgets/button';
import {Card} from '../../widgets/card';
import {Icons} from '../../base/semantic_icons';
import {Switch} from '../../widgets/switch';
import {
  getTerminology,
  getTerminologyId,
  getTerminologyOptions,
  setTerminologyId,
} from './terminology';
import {isAnalysisAvailable} from './analysis';

// Maximum label length before truncation.
const MAX_LABEL_LENGTH = 50;

// Truncates a string to `n` characters, appending '...' when shortened.
function trunc(s: string, n = MAX_LABEL_LENGTH): string {
  return s.length > n ? s.slice(0, n - 3) + '...' : s;
}

interface TabOption {
  readonly key: string;
  readonly title: string;
}

interface TabStripAttrs {
  readonly className?: string;
  readonly tabs: ReadonlyArray<TabOption>;
  readonly currentTabKey: string;
  onTabChange(key: string): void;
}

class TabStrip implements m.ClassComponent<TabStripAttrs> {
  view({attrs}: m.CVnode<TabStripAttrs>) {
    const {tabs, currentTabKey, onTabChange, className} = attrs;
    return m(
      '.pf-gpu-compute__toolbar-tab-strip',
      {className},
      m(
        '.pf-gpu-compute__toolbar-tab-strip__tabs',
        tabs.map((tab) => {
          const {key, title} = tab;
          return m(
            '.pf-gpu-compute__toolbar-tab-strip__tab',
            {
              active: currentTabKey === key,
              key,
              onclick: () => {
                onTabChange(key);
              },
            },
            m('span.pf-gpu-compute__toolbar-tab-strip__tab-title', title),
          );
        }),
      ),
    );
  }
}

// Renders the full toolbar (kernel card + tab strip + view popup).
export function renderToolbar(opts: {
  // Kernel launch entries for the Results dropdown.
  options: KernelLaunchOption[];
  // Currently selected kernel slice ID.
  sliceId?: number;
  // Called when the user picks a different kernel.
  onChange: (id: number | undefined, suppressAutoDetails?: boolean) => void;
  // Metric summary for the selected kernel.
  toolbarInfo?: ToolbarInfo;
  // Slice ID of the baseline kernel (if active).
  baselineId?: number;
  // Metric summary for the baseline kernel.
  baselineInfo?: ToolbarInfo;
  // Whether baseline comparison is enabled.
  baselineEnabled?: boolean;
  // Toggle baseline mode on/off.
  onToggleBaseline?: (enabled: boolean) => void;
  // Called when the humanize-metrics toggle changes.
  onHumanizeChanged?: () => void;
  // Called when the terminology dropdown changes.
  onTerminologyChanged?: () => void;
}): m.Children {
  const firstId = opts.options[0]?.id;
  const value = (opts.sliceId ?? firstId ?? '').toString();

  // Resolve baseline label from options list
  const baselineLabel = (() => {
    if (!opts.baselineEnabled || opts.baselineId == null) {
      return '—';
    }

    const i = opts.options.findIndex((o) => o.id === opts.baselineId);
    if (i < 0) return '—';

    return `${i} - ${trunc(opts.options[i].label)}`;
  })();

  // Available terminology providers for the View popup dropdown
  const terminologyOptions = getTerminologyOptions();

  return [
    m(
      Card,
      {
        className: 'pf-gpu-compute__toolbar-card',
      },
      [
        // Row 0: Headers
        m('div'),
        m('h1.pf-gpu-compute__toolbar-header', 'Result'),
        m('h1.pf-gpu-compute__toolbar-header', 'Size'),
        m('h1.pf-gpu-compute__toolbar-header', 'Time'),
        m('h1.pf-gpu-compute__toolbar-header', 'Cycles'),
        m('h1.pf-gpu-compute__toolbar-header', 'Arch'),
        m(
          'h1.pf-gpu-compute__toolbar-header',
          `${getTerminology().sm.title} Frequency`,
        ),
        m('h1.pf-gpu-compute__toolbar-header', 'Process'),

        // Row 1: Current kernel — includes the Results dropdown selector
        m('div.pf-gpu-compute__toolbar-row-label', [
          m(
            'span.pf-gpu-compute__toolbar-swatch.pf-gpu-compute__toolbar-swatch--current',
          ),
          m('span', 'Current'),
        ]),
        m(
          'select.pf-select',
          {
            value,
            className: 'pf-select',
            style: 'justify-self:start; width: max-content;',
            onchange: (e: Event) => {
              const value = (e.target as HTMLSelectElement).value;
              opts.onChange(value === '' ? undefined : Number(value));
            },
          },
          [
            ...opts.options.map((o) =>
              m(
                'option',
                {value: String(o.id)},
                `${opts.options.indexOf(o)} - ${trunc(o.label)}`,
              ),
            ),
          ],
        ),
        m(
          'span.pf-gpu-compute__toolbar-size',
          opts.toolbarInfo?.sizeText ?? '—',
        ),
        m('span', opts.toolbarInfo?.timeText ?? '—'),
        m('span', opts.toolbarInfo?.cyclesText ?? '—'),
        m('span', opts.toolbarInfo?.archText ?? '—'),
        m('span', opts.toolbarInfo?.smFrequencyText ?? '—'),
        m('span', opts.toolbarInfo?.processText ?? '—'),

        // Row 2: Baseline kernel row (only rendered when enabled)
        ...(opts.baselineEnabled
          ? [
              m('div.pf-gpu-compute__toolbar-row-label', [
                m(
                  'span.pf-gpu-compute__toolbar-swatch.pf-gpu-compute__toolbar-swatch--baseline',
                ),
                m('span', 'Baseline'),
              ]),
              m(
                'div',
                {style: 'justify-self:start; opacity:.85;'},
                baselineLabel,
              ),
              m(
                'span.pf-gpu-compute__toolbar-size',
                opts.baselineInfo?.sizeText ?? '—',
              ),
              m('span', opts.baselineInfo?.timeText ?? '—'),
              m('span', opts.baselineInfo?.cyclesText ?? '—'),
              m('span', opts.baselineInfo?.archText ?? '—'),
              m('span', opts.baselineInfo?.smFrequencyText ?? '—'),
              m('span', opts.baselineInfo?.processText ?? '—'),
            ]
          : []),
      ],
    ),

    // Secondary toolbar: tab strip + baseline toggle + View popup
    m('div.pf-gpu-compute__toolbar-secondary', [
      // Tabs
      m(TabStrip, {
        tabs: [
          {key: 'summary', title: 'Summary'},
          {key: 'details', title: 'Details'},
          ...(isAnalysisAvailable()
            ? [{key: 'analysis', title: 'Analysis'}]
            : []),
        ],
        currentTabKey: activeInfoTab,
        onTabChange: (key: string) => {
          setInfoTab(key as InfoTab);
        },
      }),

      m(Button, {
        style: 'width: 130px; justify-self: start; margin-bottom: 3px',
        icon: Icons.Change,
        label: opts.baselineEnabled ? 'Clear Baseline' : 'Add Baseline',
        variant: ButtonVariant.Outlined,
        onclick: () => {
          // Toggles baseline mode: when enabled, the caller will treat the current selection as baseline
          const enable = !opts.baselineEnabled;
          opts.onToggleBaseline?.(enable);

          // Auto focusing to 'Details' tab
          if (enable) {
            setInfoTab('details');
          }
        },
      }),
      m(
        Popup,
        {
          position: PopupPosition.BottomStart,
          offset: 4,
          matchWidth: false,
          fitContent: true,
          trigger: m(Button, {
            icon: 'visibility',
            label: 'View',
            style: 'justify-self: start; margin-bottom: 3px;',
            variant: ButtonVariant.Outlined,
          }),
        },
        [
          // Auto-Convert Metric Units
          m(
            'div.pf-gpu-compute__toolbar-popup-row',
            m('div', 'Auto-Convert Metric Units'),
            m(Switch, {
              checked: enableHumanizeMetrics,
              onchange: (e: Event) => {
                const checked = (e.target as HTMLInputElement).checked;
                setHumanizeMetrics(checked);
                opts.onHumanizeChanged?.();
              },
            }),
          ),
          // Terminology
          m('div.pf-gpu-compute__toolbar-popup-spacer'),
          m(
            'div.pf-gpu-compute__toolbar-popup-row',
            m('div', 'Terminology'),
            m(
              'select.pf-select',
              {
                value: getTerminologyId(),
                onchange: (e: Event) => {
                  const id = (e.target as HTMLSelectElement).value;
                  // refresh when terminology changes
                  setTerminologyId(id);
                  opts.onTerminologyChanged?.();
                  m.redraw();
                },
                style: 'max-width: 120px;',
              },
              terminologyOptions.map((opt) =>
                m('option', {value: opt.id}, opt.name),
              ),
            ),
          ),
        ],
      ),
    ]),

    // Toolbar divider
    m('hr.pf-gpu-compute__toolbar-divider'),
  ];
}

// Whether metric values are displayed in human-readable units.
export let enableHumanizeMetrics = true;
function setHumanizeMetrics(v: boolean) {
  enableHumanizeMetrics = v;
  m.redraw();
}

// Available sub-tabs in the GPU Compute tab.
export type InfoTab = 'details' | 'summary' | 'analysis';

let activeInfoTab: InfoTab = 'summary';
// Switches the active sub-tab and triggers a redraw.
export function setInfoTab(tab: InfoTab) {
  activeInfoTab = tab;
  m.redraw();
}

// Returns the currently active sub-tab key.
export function getActiveInfoTab(): InfoTab {
  return activeInfoTab;
}

// Resets toolbar state to defaults. Called on trace load so settings
// do not carry over between traces.
export function resetToolbarState(): void {
  enableHumanizeMetrics = true;
  activeInfoTab = 'summary';
}
