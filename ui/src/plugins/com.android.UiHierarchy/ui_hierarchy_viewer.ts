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
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {Icon} from '../../widgets/icon';
import {RadioGroup} from '../../widgets/radio_group';
import {Section} from '../../widgets/section';
import {SplitPanel} from '../../widgets/split_panel';
import {TextInput} from '../../widgets/text_input';
import type {Rect} from './canvas_pan_zoom';
import {renderUiHierarchyControls} from './ui_hierarchy_controls';
import {kindToString, type UiHierarchyNode} from './ui_hierarchy_data';
import {
  changedWithAncestors,
  type NodeChange,
  type TreeDiff,
} from './ui_hierarchy_diff';
import {displayName, type PropTarget} from './ui_hierarchy_props';
import {renderPropSections, renderPropsHeader} from './ui_hierarchy_props_view';
import {
  colorForNode,
  intentForKind,
  UiHierarchyRectsView,
} from './ui_hierarchy_rects';
import {ownerLabel} from './ui_hierarchy_screen';
import {layerLabel} from './ui_hierarchy_sf';
import type {
  HierarchyLevel,
  TreeSource,
  UiHierarchySession,
  UiHierarchyViewOptions,
} from './ui_hierarchy_session';
import {
  SCREEN_KIND_PROCESS,
  WM_KIND_DISPLAY,
  WM_KIND_WINDOW,
  displayOf,
} from './ui_hierarchy_wm';

export interface UiHierarchyViewerAttrs {
  readonly session: UiHierarchySession;
}

interface HierarchyTreeItem {
  readonly node: UiHierarchyNode;
  readonly children: HierarchyTreeItem[];
}

type KindFilter = {label: string; kind?: number};

const WINDOW_KIND_FILTERS: ReadonlyArray<KindFilter> = [
  {label: 'All'},
  {label: 'View', kind: 1},
  {label: 'ComposeView', kind: 2},
  {label: 'ComposeNode', kind: 3},
  {label: 'Composable', kind: 4},
];

const TREE_INDENT_PX = 14;

// Default width of the hierarchy and properties panes.
const SIDE_PANE_PX = 380;

type PaneId = 'layout' | 'hierarchy' | 'properties';

interface Pane {
  readonly id: PaneId;
  readonly title: string;
  readonly render: () => m.Children;
}

// Lays out consecutive open panes with draggable splitters. The layout pane
// flexes; the side panes start at SIDE_PANE_PX.
function renderRun(run: Pane[]): m.Children {
  if (run.length === 1) return run[0].render();
  const [first, ...rest] = run;
  const layoutFirst = first.id === 'layout';
  return m(SplitPanel, {
    direction: 'horizontal',
    controlledPanel: layoutFirst ? 'second' : 'first',
    initialSplit: {
      pixels: layoutFirst ? SIDE_PANE_PX * rest.length : SIDE_PANE_PX,
    },
    minSize: 220,
    firstPanel: first.render(),
    secondPanel: renderRun(rest),
  });
}

// Three panes (layout, hierarchy, properties) for the current level of the
// session: WM windows on the Screen level, or the UI hierarchy of one window.
export class UiHierarchyViewer implements m.ClassComponent<UiHierarchyViewerAttrs> {
  // Last selection revealed in the tree (per level), so we only
  // auto-expand/scroll when the selection changes (not on every redraw, which
  // would fight the user).
  private revealedKey?: string;

  // Panes collapsed to a thin strip, shared by both levels.
  private readonly collapsedPanes = new Set<PaneId>();

  view({attrs}: m.Vnode<UiHierarchyViewerAttrs>): m.Children {
    const s = attrs.session;
    const L = s.current;
    const src = s.level === 'screen' ? s.screenTree : L;
    const diff = s.treeDiff;
    const revealKey = `${s.level}:${s.treeMode}:${L.selectedNodeId}`;
    const reveal = revealKey !== this.revealedKey;
    if (reveal) expandAncestors(L, src, L.selectedNodeId);

    const panes: Pane[] = [
      {
        id: 'layout',
        title: s.level === 'screen' ? 'Screen' : 'Layout',
        render: () => this.renderLayoutPane(s, L, src),
      },
      {
        id: 'hierarchy',
        title: 'Hierarchy',
        render: () =>
          this.renderHierarchyPane(s, L, src, diff, reveal ? revealKey : ''),
      },
      {
        id: 'properties',
        title: 'Properties',
        render: () =>
          s.level === 'screen'
            ? renderWmPropertiesPane(
                s,
                this.paneTitle('Properties', 'properties'),
              )
            : this.renderPropertiesPane(s),
      },
    ];
    return m('.pf-uih-viewer', this.renderPanes(panes));
  }

  // ---------------------------------------------------------------------------
  // Pane layout

  // Collapsed panes become strips in place; each run of open panes shares
  // SplitPanels. The run holding the layout (or the only run) takes the
  // remaining width.
  private renderPanes(panes: Pane[]): m.Children {
    const runs: Array<Pane[] | Pane> = [];
    for (const p of panes) {
      if (this.collapsedPanes.has(p.id)) {
        runs.push(p);
      } else {
        const last = runs[runs.length - 1];
        if (Array.isArray(last)) last.push(p);
        else runs.push([p]);
      }
    }
    const openRuns = runs.filter((r) => Array.isArray(r)).length;
    return runs.map((r) => {
      if (!Array.isArray(r)) return this.renderStrip(r);
      const grow = openRuns === 1 || r.some((p) => p.id === 'layout');
      return m(
        '.pf-uih-pane-run',
        {style: grow ? {flex: '1 1 0'} : {flex: `0 0 ${SIDE_PANE_PX}px`}},
        renderRun(r),
      );
    });
  }

  private renderStrip(p: Pane): m.Children {
    return m(
      '.pf-uih-pane-strip',
      {
        title: `Expand ${p.title}`,
        onclick: () => this.collapsedPanes.delete(p.id),
      },
      [
        m(Icon, {icon: 'open_in_full'}),
        m('.pf-uih-pane-strip__label', p.title),
      ],
    );
  }

  private paneTitle(title: string, id: PaneId): m.Children {
    return m('.pf-uih-pane-title', [
      m('span', title),
      m(Button, {
        icon: 'close_fullscreen',
        compact: true,
        title: `Collapse ${title}`,
        onclick: () => this.collapsedPanes.add(id),
      }),
    ]);
  }

  // ---------------------------------------------------------------------------
  // Layout pane

  private renderLayoutPane(
    s: UiHierarchySession,
    L: HierarchyLevel,
    src: TreeSource,
  ): m.Children {
    const screen = s.level === 'screen';
    const o = L.options;
    const canShowInput = screen && s.hasInputData;
    const input = canShowInput && o.showInput ? s.screenInputCanvas : undefined;
    // The Screen level draws windows (or layers) only, or their input
    // windows; containers are in the tree.
    const nodes = (
      input?.nodes ?? (screen ? s.screenCanvasNodes : filteredNodes(L))
    ).filter((n) => !L.hidden.has(n.nodeId) && (!o.onlyVisible || n.isVisible));
    return m(
      '.pf-uih-pane',
      m(
        Section,
        {title: this.paneTitle(screen ? 'Screen' : 'Layout', 'layout')},
        [
          renderUiHierarchyControls(o, {
            screenLevel: screen,
            canShowScreenContext: !screen && s.hasScreenLevel,
            canShowInput,
          }),
          m(
            '.pf-uih-canvas-container',
            m(UiHierarchyRectsView, {
              nodes,
              byNodeId: src.byNodeId,
              selectedNodeId: L.selectedNodeId,
              onSelect: (id) => this.select(s, id),
              onActivate: screen
                ? (id) => {
                    if (s.uiWindowFor(id) === undefined) return false;
                    void s.openWindowFor(id);
                    return true;
                  }
                : undefined,
              context: screen
                ? displayOutline(s)
                : o.showScreenContext
                  ? screenContext(s)
                  : undefined,
              clip: screen ? displayRect(s) : undefined,
              fillRegions: input?.touchable,
              options: o,
            }),
          ),
        ],
      ),
    );
  }

  private select(s: UiHierarchySession, id: string): void {
    if (s.level === 'screen') s.selectScreenNode(id);
    else void s.selectNode(id);
  }

  // ---------------------------------------------------------------------------
  // Hierarchy pane

  private renderHierarchyPane(
    s: UiHierarchySession,
    L: HierarchyLevel,
    src: TreeSource,
    diff: TreeDiff,
    revealKey: string,
  ): m.Children {
    const o = L.options;
    const screen = s.level === 'screen';
    const tree = buildTree(L, src, screen, diff);
    const removed = new Set(diff.removed.map((n) => n.nodeId));
    const changeOf = (id: string): NodeChange | undefined =>
      diff.changes.get(id) ?? (removed.has(id) ? 'removed' : undefined);
    return m(
      '.pf-uih-pane',
      m(Section, {title: this.paneTitle('Hierarchy', 'hierarchy')}, [
        m('.pf-uih-hier-toolbar', [
          m(TextInput, {
            className: 'pf-uih-hier-toolbar__search',
            placeholder: screen
              ? 'Search processes, windows and containers...'
              : 'Search name, id, text, tag, description...',
            leftIcon: 'search',
            value: o.searchQuery,
            onInput: (value: string) => {
              o.searchQuery = value;
            },
          }),
          m(Button, {
            icon: 'unfold_more',
            compact: true,
            title: 'Expand all',
            onclick: () => L.collapsed.clear(),
          }),
          m(Button, {
            icon: 'unfold_less',
            compact: true,
            title: 'Collapse all',
            onclick: () => collapseAll(L, tree),
          }),
          m(Button, {
            icon: 'visibility',
            compact: true,
            title: 'Show all in layout',
            disabled: L.hidden.size === 0,
            onclick: () => L.hidden.clear(),
          }),
          m(Button, {
            icon: 'visibility_off',
            compact: true,
            title: 'Hide all in layout',
            onclick: () => src.nodes.forEach((n) => L.hidden.add(n.nodeId)),
          }),
          m(Button, {
            icon: 'difference',
            compact: true,
            active: o.showDiff,
            title: o.showDiff
              ? 'Hide changes'
              : 'Show changes since the previous snapshot',
            onclick: () => void s.setShowDiff(!o.showDiff),
          }),
          renderDiffBase(s),
          o.showDiff &&
            m(Button, {
              icon: 'filter_list',
              compact: true,
              active: o.changedOnly,
              title: o.changedOnly ? 'Show all nodes' : 'Show only changes',
              onclick: () => {
                o.changedOnly = !o.changedOnly;
              },
            }),
        ]),
        screen ? renderTreeModes(s) : renderKindChips(o),
        m(
          '.pf-uih-tree',
          {role: 'tree'},
          tree.length === 0
            ? m(EmptyState, {
                icon: 'search_off',
                title: 'No matching nodes in this snapshot',
              })
            : // Rows share the width of the widest row, so hover and
              // selection backgrounds span the full row even when the tree
              // scrolls horizontally.
              m(
                '.pf-uih-tree__rows',
                tree.map((item) =>
                  this.renderTreeRow(s, L, src, changeOf, item, 0, revealKey),
                ),
              ),
        ),
      ]),
    );
  }

  private renderTreeRow(
    s: UiHierarchySession,
    L: HierarchyLevel,
    src: TreeSource,
    changeOf: (id: string) => NodeChange | undefined,
    item: HierarchyTreeItem,
    depth: number,
    revealKey: string,
  ): m.Children {
    const n = item.node;
    const change = changeOf(n.nodeId);
    // Removed nodes are only shown, they are not in the snapshot.
    const removed = change === 'removed';
    const selected = L.selectedNodeId === n.nodeId;
    const hasChildren = item.children.length > 0;
    const collapsed = hasChildren && L.collapsed.has(n.nodeId);
    const toggle = (e: MouseEvent) => {
      e.stopPropagation();
      toggleCollapsed(L, item, e.altKey || e.shiftKey);
    };
    // Screen level: windows with UI hierarchy data can be opened.
    const openable =
      !removed && s.level === 'screen' && s.uiWindowFor(n.nodeId) !== undefined;
    const open = (e: MouseEvent) => {
      e.stopPropagation();
      void s.openWindowFor(n.nodeId);
    };
    const scrollIfRevealed = (v: m.VnodeDOM) => {
      if (selected && revealKey !== '') {
        (v.dom as HTMLElement).scrollIntoView({
          block: 'nearest',
          inline: 'start',
        });
        this.revealedKey = revealKey;
      }
    };
    const hidden = L.hidden.has(n.nodeId);
    const rowClass = [
      selected ? 'pf-uih-tree__row--selected' : '',
      hidden ? 'pf-uih-tree__row--hidden' : '',
      removed ? 'pf-uih-tree__row--removed' : '',
    ].join(' ');
    const isProcess = n.kind === SCREEN_KIND_PROCESS;
    const windowCount = item.children.length;
    // Short names; the full name is in the tooltip.
    const label =
      (isProcess ? n.name : displayName(n.name)) || `Node ${n.nodeId}`;

    return [
      m(
        '.pf-uih-tree__row',
        {
          'role': 'treeitem',
          'aria-selected': selected,
          'aria-expanded': hasChildren ? !collapsed : undefined,
          'class': rowClass,
          'style': {paddingLeft: '2px'},
          'title': removed
            ? `${n.name}\nRemoved since the previous snapshot`
            : isProcess
              ? `${n.name}\n${pluralize(windowCount, 'window')}`
              : nodeSummary(n),
          'onclick': removed ? undefined : () => this.select(s, n.nodeId),
          'ondblclick': openable ? open : hasChildren ? toggle : undefined,
          'oncreate': scrollIfRevealed,
          'onupdate': scrollIfRevealed,
        },
        [
          m(
            '.pf-uih-tree__actions',
            !removed && [
              m(Button, {
                icon: hidden ? 'visibility_off' : 'visibility',
                compact: true,
                active: hidden,
                title: hidden ? 'Show subtree' : 'Hide subtree',
                onclick: (e: MouseEvent) => {
                  e.stopPropagation();
                  setSubtreeHidden(L, src, n.nodeId, !hidden);
                },
              }),
              m(Button, {
                icon: 'filter_center_focus',
                compact: true,
                title: 'Show only this subtree',
                onclick: (e: MouseEvent) => {
                  e.stopPropagation();
                  showOnly(L, src, n.nodeId);
                },
              }),
            ],
          ),
          m('span.pf-uih-tree__indent', {
            style: {width: `${depth * TREE_INDENT_PX}px`},
          }),
          hasChildren
            ? m(
                'span.pf-uih-tree__caret',
                {
                  title: collapsed
                    ? 'Expand (Alt+click: expand subtree)'
                    : 'Collapse (Alt+click: collapse subtree)',
                  onclick: toggle,
                },
                m(Icon, {icon: collapsed ? 'chevron_right' : 'expand_more'}),
              )
            : m('span.pf-uih-tree__caret.pf-uih-tree__caret--leaf'),
          m('span.pf-uih-tree__kind', {
            title: kindToString(n.kind),
            style: {background: colorForNode(n)},
          }),
          m('span.pf-uih-tree__name', label),
          change !== undefined &&
            m(`span.pf-uih-tree__diff.pf-uih-tree__diff--${change}`, change),
          openable &&
            m(Button, {
              icon: 'account_tree',
              compact: true,
              className: 'pf-uih-tree__open',
              title: 'Open UI hierarchy (double-click)',
              onclick: open,
            }),
          isProcess
            ? m(
                'span.pf-uih-tree__count',
                {title: pluralize(windowCount, 'window')},
                windowCount,
              )
            : collapsed &&
              m(
                'span.pf-uih-tree__count',
                {
                  title:
                    `${item.children.length} children, ` +
                    `${countDescendants(item)} descendants`,
                },
                item.children.length,
              ),
          n.isTextRedacted
            ? m(Icon, {
                icon: 'lock',
                className: 'pf-uih-tree__meta',
                title: 'Text redacted (sensitive field)',
              })
            : n.text && m('span.pf-uih-tree__text', `"${n.text}"`),
          n.testTag &&
            n.testTag !== n.name &&
            m('span.pf-uih-tree__tag', n.testTag),
          !n.isVisible &&
            m(Icon, {
              icon: 'visibility_off',
              className: 'pf-uih-tree__meta',
              title: 'Not visible',
            }),
        ],
      ),
      !collapsed &&
        item.children.map((c) =>
          this.renderTreeRow(s, L, src, changeOf, c, depth + 1, revealKey),
        ),
    ];
  }

  // ---------------------------------------------------------------------------
  // Properties pane

  private renderPropertiesPane(s: UiHierarchySession): m.Children {
    const node = s.win.selectedNode;
    const wmNode = s.wmNodeForSelectedWindow;
    if (node === undefined) {
      return m(
        '.pf-uih-pane',
        m(
          Section,
          {title: this.paneTitle('Properties', 'properties')},
          m(EmptyState, {
            icon: 'ads_click',
            title: 'Select a node in the layout or hierarchy',
            fillHeight: true,
          }),
        ),
      );
    }

    return m(
      '.pf-uih-pane',
      m(Section, {title: this.paneTitle('Properties', 'properties')}, [
        renderPropsHeader(
          node.name || `Node ${node.nodeId}`,
          node.kindName,
          [`id ${node.nodeId}`, `window ${node.windowId}`],
          intentForKind(node.kind),
        ),
        node.isTextRedacted &&
          m(
            Callout,
            {icon: 'lock', intent: Intent.Danger},
            'Text redacted: this node is a password or sensitive input.',
          ),
        renderPropSections(
          [
            ...s.winSections(node),
            // The root node stands for the window.
            ...(node.parentNodeId === undefined && wmNode !== undefined
              ? s.windowSections(wmNode)
              : []),
          ],
          (t) => void s.followLink(t),
        ),
        this.renderHistory(s),
      ]),
    );
  }

  private renderHistory(s: UiHierarchySession): m.Children {
    return m('.pf-uih-history', [
      m(
        '.pf-uih-section-title',
        `Change history (${s.nodeVersions.length} versions)`,
      ),
      s.nodeVersionChanges.some((v) => v.idReused) &&
        m(
          '.pf-uih-muted.pf-uih-history__note',
          {
            title:
              'Node ids should identify a node across snapshots, but ' +
              'producers that predate stable ids numbered nodes per ' +
              'capture. Versions marked "different node" have another ' +
              'name or kind: their changes compare unrelated nodes.',
          },
          'This id was reused by different nodes (trace without stable ' +
            'node ids).',
        ),
      s.nodeVersionChanges.length === 0
        ? m('.pf-uih-muted', 'No versions recorded for this node.')
        : s.nodeVersionChanges.map((v) =>
            m('.pf-uih-history__item', [
              m('.pf-uih-history__head', [
                m('span.pf-uih-history__version', `#${v.versionIndex + 1}`),
                m(Timestamp, {trace: s.trace, ts: Time.fromRaw(v.ts)}),
                m(Button, {
                  label: 'Go to',
                  icon: 'my_location',
                  compact: true,
                  title: 'Show the snapshot containing this version',
                  onclick: () => void s.setNearestTs(v.ts),
                }),
              ]),
              m(
                'ul.pf-uih-history__changes',
                v.changedProps.map((c) => m('li', c)),
              ),
            ]),
          ),
    ]);
  }
}

// -----------------------------------------------------------------------------
// Screen level

function renderWmPropertiesPane(
  s: UiHierarchySession,
  title: m.Children,
): m.Children {
  const onLink = (t: PropTarget) => void s.followLink(t);
  const process = s.selectedProcess;
  const layer = s.selectedLayer;
  const display = s.screenDisplay;
  // Nothing selected: the display.
  const node = s.selectedScreenNode ?? display;
  if (node === undefined) {
    return m(
      '.pf-uih-pane',
      m(
        Section,
        {title},
        m(EmptyState, {
          icon: 'ads_click',
          title: 'Select a window on the screen or in the hierarchy',
          fillHeight: true,
        }),
      ),
    );
  }
  let header: m.Children;
  if (process !== undefined) {
    const owner = process.owner;
    const filtered = s.filteredProcess?.owner.key === owner.key;
    header = [
      renderPropsHeader(ownerLabel(owner), 'Process', [
        pluralize(process.windows.length, 'window'),
      ]),
      m(Button, {
        label: filtered ? 'Show all processes' : 'Show only this process',
        className: 'pf-uih-props-open',
        onclick: () =>
          void s.setProcessFilter(filtered ? undefined : owner.key),
      }),
    ];
  } else if (layer !== undefined) {
    header = renderPropsHeader(layerLabel(layer), 'Layer', [`id ${layer.id}`]);
  } else if (node.kind === WM_KIND_DISPLAY) {
    header = renderPropsHeader(node.name, 'Display', []);
  } else {
    const uiWin = s.uiWindowFor(node.nodeId);
    header = [
      renderPropsHeader(
        node.name,
        node.wm?.containerType ?? node.kindName,
        display !== undefined ? [display.name] : [],
      ),
      uiWin !== undefined &&
        m(Button, {
          label: 'Open UI hierarchy',
          icon: 'account_tree',
          intent: Intent.Primary,
          className: 'pf-uih-props-open',
          onclick: () => void s.openWindowFor(node.nodeId),
        }),
    ];
  }
  return m(
    '.pf-uih-pane',
    m(Section, {title}, [
      header,
      renderPropSections(s.screenSections(node), onLink),
    ]),
  );
}

// The Screen level tree modes.
function renderTreeModes(s: UiHierarchySession): m.Children {
  return m(
    '.pf-uih-chip-row',
    m(
      RadioGroup,
      {
        selectedValue: s.treeMode,
        onValueChange: (v: string) => {
          if (v === 'processes' || v === 'wm' || v === 'sf') s.setTreeMode(v);
        },
      },
      [
        m(RadioGroup.Button, {value: 'processes'}, 'Processes'),
        m(RadioGroup.Button, {value: 'wm'}, 'WindowManager'),
        s.hasSfData && m(RadioGroup.Button, {value: 'sf'}, 'SurfaceFlinger'),
      ],
    ),
  );
}

// Window level node kind filter.
function renderKindChips(o: UiHierarchyViewOptions): m.Children {
  return m(
    '.pf-uih-chip-row',
    WINDOW_KIND_FILTERS.map((f) =>
      m(Chip, {
        label: f.label,
        compact: true,
        intent: o.kindFilter === f.kind ? Intent.Primary : Intent.None,
        onclick: () => {
          o.kindFilter = o.kindFilter === f.kind ? undefined : f.kind;
        },
      }),
    ),
  );
}

function displayOutline(s: UiHierarchySession): UiHierarchyNode[] {
  const d = s.screenDisplay;
  return d !== undefined ? [d] : [];
}

function displayRect(s: UiHierarchySession): Rect | undefined {
  const d = s.screenDisplay;
  if (d === undefined || d.boundsRight <= d.boundsLeft) return undefined;
  return {
    left: d.boundsLeft,
    top: d.boundsTop,
    right: d.boundsRight,
    bottom: d.boundsBottom,
  };
}

function pluralize(n: number, noun: string): string {
  return `${n} ${noun}${n === 1 ? '' : 's'}`;
}

// The display outline plus the other visible windows on the open window's
// display, drawn around it.
function screenContext(s: UiHierarchySession): UiHierarchyNode[] {
  const self = s.wmNodeForSelectedWindow;
  const display = s.currentDisplay;
  const windows = s.screen.nodes.filter(
    (n) =>
      n.kind === WM_KIND_WINDOW &&
      n.isVisible &&
      n.nodeId !== self?.nodeId &&
      displayOf(s.screen.byNodeId, n.nodeId) === display,
  );
  return display !== undefined ? [display, ...windows] : windows;
}

// -----------------------------------------------------------------------------
// Tree helpers

function filteredNodes(L: HierarchyLevel): UiHierarchyNode[] {
  const o = L.options;
  return L.nodes.filter(
    (n) =>
      (!o.onlyVisible || n.isVisible) &&
      (!o.onlyClickable || n.isClickable) &&
      (o.kindFilter === undefined || n.kind === o.kindFilter),
  );
}

// The Screen level tree ignores the canvas filters (e.g. Only visible) so
// hidden windows can still be found and inspected. With the diff shown,
// removed subtrees are added back, and with "changed only" the tree keeps
// just the changes and their ancestors.
function buildTree(
  L: HierarchyLevel,
  src: TreeSource,
  screen: boolean,
  diff: TreeDiff,
): HierarchyTreeItem[] {
  let pool: ReadonlyArray<UiHierarchyNode> = [
    ...(screen ? src.nodes : filteredNodes(L)),
    ...diff.removed,
  ];
  if (L.options.showDiff && L.options.changedOnly) {
    const keep = changedWithAncestors(diff, src.byNodeId);
    pool = pool.filter((n) => keep.has(n.nodeId));
  }

  const q = L.options.searchQuery.trim().toLowerCase();
  if (q !== '') {
    // Keep matches plus their ancestors so matches stay in context.
    const keep = new Set<string>();
    for (const n of src.nodes) {
      if (!matchesQuery(n, q)) continue;
      for (
        let cur: UiHierarchyNode | undefined = n;
        cur !== undefined && !keep.has(cur.nodeId);
        cur =
          cur.parentNodeId !== undefined
            ? src.byNodeId.get(cur.parentNodeId)
            : undefined
      ) {
        keep.add(cur.nodeId);
      }
    }
    pool = pool.filter((n) => keep.has(n.nodeId));
  }

  const inPool = new Set(pool.map((n) => n.nodeId));
  const childrenOf = new Map<string, UiHierarchyNode[]>();
  const roots: UiHierarchyNode[] = [];
  for (const n of pool) {
    if (n.parentNodeId !== undefined && inPool.has(n.parentNodeId)) {
      const arr = childrenOf.get(n.parentNodeId);
      if (arr) arr.push(n);
      else childrenOf.set(n.parentNodeId, [n]);
    } else {
      roots.push(n);
    }
  }
  // Guards against cycles in malformed traces.
  const visited = new Set<string>();
  const build = (n: UiHierarchyNode): HierarchyTreeItem => {
    if (visited.has(n.nodeId)) return {node: n, children: []};
    visited.add(n.nodeId);
    return {node: n, children: (childrenOf.get(n.nodeId) ?? []).map(build)};
  };
  return roots.map(build);
}

function toggleCollapsed(
  L: HierarchyLevel,
  item: HierarchyTreeItem,
  recursive: boolean,
): void {
  const collapse = !L.collapsed.has(item.node.nodeId);
  const apply = (it: HierarchyTreeItem) => {
    if (it.children.length === 0) return;
    if (collapse) L.collapsed.add(it.node.nodeId);
    else L.collapsed.delete(it.node.nodeId);
    if (recursive) it.children.forEach(apply);
  };
  apply(item);
}

function collapseAll(L: HierarchyLevel, items: HierarchyTreeItem[]): void {
  for (const it of items) {
    if (it.children.length === 0) continue;
    L.collapsed.add(it.node.nodeId);
    collapseAll(L, it.children);
  }
}

function expandAncestors(
  L: HierarchyLevel,
  src: TreeSource,
  nodeId?: string,
): void {
  if (nodeId === undefined) return;
  let cur = src.byNodeId.get(nodeId);
  while (cur?.parentNodeId !== undefined) {
    L.collapsed.delete(cur.parentNodeId);
    cur = src.byNodeId.get(cur.parentNodeId);
  }
}

function setSubtreeHidden(
  L: HierarchyLevel,
  src: TreeSource,
  rootId: string,
  hide: boolean,
): void {
  for (const id of subtreeIds(src.nodes, rootId)) {
    if (hide) L.hidden.add(id);
    else L.hidden.delete(id);
  }
}

function showOnly(L: HierarchyLevel, src: TreeSource, rootId: string): void {
  const keep = subtreeIds(src.nodes, rootId);
  L.hidden.clear();
  for (const n of src.nodes) {
    if (!keep.has(n.nodeId)) L.hidden.add(n.nodeId);
  }
}

function matchesQuery(n: UiHierarchyNode, q: string): boolean {
  return (
    n.name.toLowerCase().includes(q) ||
    n.nodeId.includes(q) ||
    (n.text?.toLowerCase().includes(q) ?? false) ||
    (n.testTag?.toLowerCase().includes(q) ?? false) ||
    (n.contentDescription?.toLowerCase().includes(q) ?? false) ||
    (n.sourceLocation?.toLowerCase().includes(q) ?? false)
  );
}

function nodeSummary(n: UiHierarchyNode): string {
  const lines = [n.name || `Node ${n.nodeId}`, `Kind: ${kindToString(n.kind)}`];
  if (n.text && !n.isTextRedacted) lines.push(`Text: "${n.text}"`);
  if (n.contentDescription) {
    lines.push(`Description: "${n.contentDescription}"`);
  }
  if (n.role) lines.push(`Role: ${n.role}`);
  if (n.testTag) lines.push(`Test tag: ${n.testTag}`);
  lines.push(
    `Bounds: [${n.boundsLeft}, ${n.boundsTop}, ${n.boundsRight}, ` +
      `${n.boundsBottom}]`,
  );
  return lines.join('\n');
}

function countDescendants(item: HierarchyTreeItem): number {
  let count = item.children.length;
  for (const c of item.children) count += countDescendants(c);
  return count;
}

// Ids of `rootId` and all its descendants among `nodes`.
function subtreeIds(
  nodes: ReadonlyArray<UiHierarchyNode>,
  rootId: string,
): Set<string> {
  const childrenOf = new Map<string, string[]>();
  for (const n of nodes) {
    if (n.parentNodeId === undefined) continue;
    const arr = childrenOf.get(n.parentNodeId);
    if (arr) arr.push(n.nodeId);
    else childrenOf.set(n.parentNodeId, [n.nodeId]);
  }
  const out = new Set<string>();
  const stack = [rootId];
  for (let id = stack.pop(); id !== undefined; id = stack.pop()) {
    if (out.has(id)) continue; // Guards against cycles.
    out.add(id);
    stack.push(...(childrenOf.get(id) ?? []));
  }
  return out;
}

// What the changes are against: the previous snapshot, or the one picked
// by comparing across a transition, which can be dropped.
function renderDiffBase(s: UiHierarchySession): m.Children {
  const c = s.diffComparison;
  if (c === undefined) return null;
  return m(Chip, {
    label: `vs ${c.index + 1} · ${c.label}`,
    compact: true,
    intent: c.pinned ? Intent.Primary : Intent.None,
    className: 'pf-uih-diff-base',
    title: c.pinned
      ? `Changes since snapshot ${c.index + 1}, ${c.label}`
      : `Changes since the previous snapshot (${c.index + 1})`,
    removable: c.pinned,
    removeButtonTitle: 'Compare with the previous snapshot',
    onRemove: () => void s.unpinDiffBase(),
  });
}
