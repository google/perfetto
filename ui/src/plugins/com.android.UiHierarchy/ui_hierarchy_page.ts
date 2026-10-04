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

import './ui_hierarchy.scss';
import m from 'mithril';
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import {Button, ButtonBar} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {RadioGroup} from '../../widgets/radio_group';
import {Select} from '../../widgets/select';
import {queryWindowFrameIdAt} from './ui_hierarchy_data';
import {displayName} from './ui_hierarchy_props';
import {ownerLabel} from './ui_hierarchy_screen';
import {transitionLabel} from './ui_hierarchy_transitions';
import {UiHierarchyViewer} from './ui_hierarchy_viewer';
import type {UiHierarchySession} from './ui_hierarchy_session';

export interface UiHierarchyPageAttrs {
  readonly session: UiHierarchySession;
  readonly subpage: string | undefined;
}

const PLAYBACK_RATES = [0.25, 0.5, 1, 1.5, 2, 4];

export class UiHierarchyPage implements m.ClassComponent<UiHierarchyPageAttrs> {
  private session?: UiHierarchySession;

  // Left/Right step through the snapshots (the slider's own keys included,
  // so it steps the same way focused or not). Esc on the Window level goes
  // back up to the Screen level.
  private readonly onKeyDown = (e: KeyboardEvent) => {
    const s = this.session;
    // The canvas stops the propagation of the arrows it pans with.
    if (s === undefined) return;
    if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
    const t = e.target as HTMLElement | null;
    const slider = t?.closest('.pf-uih-scrub') !== null;
    if (!slider && t?.closest('input, textarea, select, [contenteditable]')) {
      return;
    }
    // Let an open popup (e.g. the window menu) take its keys first.
    if (document.querySelector('.pf-popup-menu') !== null) return;
    switch (e.key) {
      case 'ArrowLeft':
        void s.prevSnapshot();
        break;
      case 'ArrowRight':
        void s.nextSnapshot();
        break;
      case 'Escape':
        if (s.level !== 'window' || !s.hasScreenLevel) return;
        void s.goToScreen();
        break;
      default:
        return;
    }
    e.preventDefault();
  };

  oninit(vnode: m.Vnode<UiHierarchyPageAttrs>): void {
    this.session = vnode.attrs.session;
    this.syncSubpage(vnode);
  }

  oncreate(): void {
    document.addEventListener('keydown', this.onKeyDown);
  }

  onremove(): void {
    document.removeEventListener('keydown', this.onKeyDown);
  }

  onbeforeupdate(vnode: m.Vnode<UiHierarchyPageAttrs>): void {
    this.session = vnode.attrs.session;
    this.syncSubpage(vnode);
  }

  private syncSubpage(vnode: m.Vnode<UiHierarchyPageAttrs>): void {
    const subpage = vnode.attrs.subpage;
    if (!subpage) return;
    const s = vnode.attrs.session;
    const targetKey = decodeURIComponent(subpage);
    const targetWin = s.windows.find(
      (w) => w.key === targetKey || `${w.upid}` === targetKey,
    );
    if (targetWin && targetWin.key !== s.selectedWindowKey) {
      void s.setWindow(targetWin.key);
    }
  }

  view(vnode: m.Vnode<UiHierarchyPageAttrs>): m.Children {
    const s = vnode.attrs.session;

    if (!s.hasData) {
      return m(
        '.pf-uih-page',
        m(
          '.pf-uih-empty',
          'No UI hierarchy data found in trace. Record with the ' +
            'android.ui.hierarchy data source.',
        ),
      );
    }

    const screen = s.level === 'screen';
    const snap = screen ? undefined : s.currentSnapshot;
    const ts = s.currentTs;
    const index = s.timelineIndex;
    const length = s.timelineLength;

    return m('.pf-uih-page', [
      m('.pf-uih-bar', [
        renderLevelSwitch(s),
        s.hasScreenLevel ? renderBreadcrumb(s) : renderWindowSelect(s),
        m(Button, {
          icon: 'timeline',
          label: 'Timeline',
          title: 'Show this snapshot in the timeline',
          onclick: async () => {
            const cur = s.currentSnapshot;
            const win = s.selectedWindow;
            // The Window level selects the snapshot on its window's track;
            // otherwise flag the time.
            const frameId =
              !screen && cur !== undefined && win !== undefined
                ? await queryWindowFrameIdAt(
                    s.trace.engine,
                    win.upid,
                    win.windowId,
                    cur.ts,
                  )
                : undefined;
            if (frameId === undefined || win === undefined) {
              s.showInTimeline();
              return;
            }
            s.trace.navigate('#!/viewer');
            s.trace.selection.selectTrackEvent(
              `/ui_hierarchy_track/${win.key}`,
              frameId,
              {scrollToSelection: true},
            );
          },
        }),
        m(
          ButtonBar,
          m(Button, {
            icon: 'first_page',
            compact: true,
            disabled: index <= 0 || s.playing,
            title: 'First snapshot',
            onclick: () => void s.setIndex(0),
          }),
          m(Button, {
            icon: 'skip_previous',
            compact: true,
            disabled: index <= 0 || s.playing,
            title: 'Previous snapshot',
            onclick: () => void s.prevSnapshot(),
          }),
          m(Button, {
            icon: s.playing ? 'pause' : 'play_arrow',
            intent: s.playing ? Intent.Warning : Intent.Primary,
            compact: true,
            title: s.playing ? 'Pause' : 'Play',
            onclick: () => s.togglePlay(),
          }),
          m(Button, {
            icon: 'skip_next',
            compact: true,
            disabled: index >= length - 1 || s.playing,
            title: 'Next snapshot',
            onclick: () => void s.nextSnapshot(),
          }),
          m(Button, {
            icon: 'last_page',
            compact: true,
            disabled: index >= length - 1 || s.playing,
            title: 'Last snapshot',
            onclick: () => void s.setIndex(length - 1),
          }),
          m(
            Select,
            {
              title: 'Playback speed',
              onchange: (e: Event) => {
                s.setPlaybackRate(
                  Number((e.target as HTMLSelectElement).value),
                );
              },
            },
            PLAYBACK_RATES.map((rate) =>
              m(
                'option',
                {value: rate, selected: rate === s.playbackRate},
                `${rate}x`,
              ),
            ),
          ),
        ),
        m('.pf-uih-scrub', [
          m('input[type=range]', {
            min: 0,
            max: Math.max(0, length - 1),
            value: index,
            title: 'Scrub snapshots across time',
            oninput: (e: Event) =>
              void s.setIndex(Number((e.target as HTMLInputElement).value)),
          }),
          // Transitions under the slider; click one to jump to its start.
          length > 1 &&
            s.timelineTransitions.map((span) =>
              m('.pf-uih-scrub__transition', {
                key: span.transition.id,
                class:
                  index >= span.first && index <= span.last
                    ? 'pf-uih-scrub__transition--current'
                    : '',
                style: {
                  left: `${(span.first / (length - 1)) * 100}%`,
                  width: `${((span.last - span.first) / (length - 1)) * 100}%`,
                  bottom: `${-5 - 5 * span.lane}px`,
                },
                title:
                  `Transition ${transitionLabel(span.transition)}\n` +
                  'Click to compare before and after it',
                onclick: () => void s.compareTransition(span),
              }),
            ),
        ]),
        m(
          'span.pf-uih-bar__pos',
          `${length === 0 ? 0 : index + 1} / ${length}`,
        ),
        snap !== undefined &&
          m('span.pf-uih-bar__badge', [
            snap.isKeyframe
              ? m('span.pf-uih-keyframe-badge', 'KEYFRAME')
              : m(
                  'span.pf-uih-delta-badge',
                  `Update (+${snap.changedNodeCount} -${snap.removedNodeCount})`,
                ),
          ]),
        ts !== undefined &&
          m('span.pf-uih-bar__ts', [
            m(Timestamp, {trace: s.trace, ts: Time.fromRaw(ts)}),
          ]),
      ]),
      m(
        '.pf-uih-main',
        length === 0
          ? m(
              '.pf-uih-empty',
              screen
                ? 'No WindowManager snapshots in this trace.'
                : 'No snapshots recorded for this window.',
            )
          : m(UiHierarchyViewer, {session: s}),
      ),
    ]);
  }
}

// `Display ▾ › Process ▾ › Window ▾`. The display crumb goes back to the
// Screen level; the process crumb filters the Screen level; the window crumb
// opens or selects a window.
function renderBreadcrumb(s: UiHierarchySession): m.Children {
  const sep = () =>
    m(Icon, {icon: 'chevron_right', className: 'pf-uih-crumbs__sep'});
  return m('.pf-uih-crumbs', [
    renderDisplayCrumb(s),
    sep(),
    renderProcessMenu(s),
    sep(),
    renderWindowMenu(s),
  ]);
}

function renderDisplayCrumb(s: UiHierarchySession): m.Children {
  const display = s.currentDisplay;
  const name = display?.name ?? 'Screen';
  const displays = s.displays;
  if (displays.length > 1) {
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: 'smartphone',
          label: name,
          rightIcon: 'arrow_drop_down',
          compact: true,
          title: 'Show another display',
        }),
      },
      displays.map((d) =>
        m(MenuItem, {
          label: d.name,
          icon: 'smartphone',
          active: d === display,
          onclick: () => void s.selectDisplay(d.wm?.displayId),
        }),
      ),
    );
  }
  return m('span.pf-uih-crumbs__current', [
    m(Icon, {icon: 'smartphone'}),
    name,
  ]);
}

// The processes owning windows on the display, topmost first.
function renderProcessMenu(s: UiHierarchySession): m.Children {
  const groups = s.processGroups;
  const current =
    s.level === 'window'
      ? groups.find((g) => {
          const n = s.wmNodeForSelectedWindow;
          return n !== undefined && g.windows.includes(n);
        })
      : s.filteredProcess;
  const windows = (n: number) => `${n} window${n === 1 ? '' : 's'}`;
  const count = (n: number) => m('span.pf-uih-crumbs__count', windows(n));
  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        label:
          current !== undefined
            ? `${ownerLabel(current.owner)} · ${windows(current.windows.length)}`
            : `All processes · ${windows(s.displayWindows.length)}`,
        rightIcon: 'arrow_drop_down',
        compact: true,
        className: 'pf-uih-crumbs__process',
        title: 'Show only the windows of one process',
      }),
    },
    [
      m(MenuItem, {
        label: 'All processes',
        active: s.filteredProcess === undefined,
        onclick: () => void s.setProcessFilter(undefined),
      }),
      groups.map((g) =>
        m(MenuItem, {
          label: [ownerLabel(g.owner), count(g.windows.length)],
          active: g === s.filteredProcess,
          onclick: () => void s.setProcessFilter(g.owner.key),
        }),
      ),
    ],
  );
}

// The windows on the display (of the picked process), top of the stack
// first. Windows with UI hierarchy data are opened, others selected on the
// Screen level.
function renderWindowMenu(s: UiHierarchySession): m.Children {
  const screen = s.level === 'screen';
  const current = screen ? s.selectedWindowNode : s.wmNodeForSelectedWindow;
  const label = screen
    ? current !== undefined
      ? displayName(current.name)
      : 'Window'
    : (s.selectedWindow?.title ?? '');
  const windows = [...s.screenWindows].reverse();
  return m(
    PopupMenu,
    {
      trigger: m(Button, {
        label,
        rightIcon: 'arrow_drop_down',
        compact: true,
        className: 'pf-uih-crumbs__window',
        title: current?.name ?? 'Select a window',
      }),
    },
    windows.length === 0
      ? m(MenuItem, {label: 'No windows', disabled: true})
      : windows.map((w) => {
          const openable = s.uiWindowFor(w.nodeId) !== undefined;
          return m(MenuItem, {
            label: displayName(w.name),
            icon: openable ? 'account_tree' : 'crop_square',
            active: w.nodeId === current?.nodeId,
            title: [
              w.name,
              w.isVisible ? '' : 'Not visible',
              openable ? 'Open UI hierarchy' : 'No UI hierarchy data',
            ]
              .filter((t) => t !== '')
              .join('\n'),
            onclick: async () => {
              if (openable) {
                await s.openWindowFor(w.nodeId);
                return;
              }
              if (!screen) await s.goToScreen();
              s.selectScreenNode(w.nodeId);
            },
          });
        }),
  );
}

// Traces without WindowManager data have no Screen level; pick windows here.
function renderWindowSelect(s: UiHierarchySession): m.Children {
  return m(
    Select,
    {
      title: 'Window',
      onchange: (e: Event) =>
        void s.setWindow((e.target as HTMLSelectElement).value),
    },
    s.windows.map((w) =>
      m(
        'option',
        {value: w.key, selected: w.key === s.selectedWindowKey},
        `${w.processName ?? 'App'}: ${w.title}`,
      ),
    ),
  );
}

// The level switch: every window on the screen, or the View/Compose tree
// of the selected window when it has android.ui.hierarchy data. Esc and
// double-clicking a window do the same.
function renderLevelSwitch(s: UiHierarchySession): m.Children {
  if (!s.hasScreenLevel) return null;
  const why = s.viewsUnavailableReason;
  return m(
    '.pf-uih-level',
    m(
      RadioGroup,
      {
        selectedValue: s.level,
        onValueChange: (v: string) => {
          if (v === 'screen') void s.goToScreen();
          else if (v === 'window' && why === undefined) void s.openSelected();
        },
      },
      [
        m(
          RadioGroup.Button,
          {value: 'screen', icon: 'web_asset', title: 'All windows (Esc)'},
          'Windows',
        ),
        m(
          RadioGroup.Button,
          {
            value: 'window',
            icon: 'account_tree',
            title:
              why ?? 'The View/Compose tree of the window (double-click it)',
            className: why !== undefined ? 'pf-uih-level--unavailable' : '',
          },
          'Views',
        ),
      ],
    ),
  );
}
