// Copyright (C) 2024 The Android Open Source Project
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

/**
 * This module provides the TrackNodeTree mithril component, which is
 * responsible for rendering out a tree of tracks and drawing their content
 * onto the canvas.
 * - Rendering track panels and handling nested and sticky headers.
 * - Managing the virtual canvas & drawing the grid-lines, tracks and overlays
 *   onto the canvas.
 * - Handling track interaction events such as dragging, panning and scrolling.
 */

import m from 'mithril';
import {canvasClip, canvasSave} from '../../base/canvas_utils';
import {classNames} from '../../base/classnames';
import {Bounds2D, Rect2D, Size2D, VerticalBounds} from '../../base/geom';
import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Icons} from '../../base/semantic_icons';
import {TimeScale} from '../../base/time_scale';
import {RequiredField} from '../../base/utils';
import {AppImpl} from '../../core/app_impl';
import {PerfStats, runningStatStr} from '../../core/perf_stats';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {TrackWithFSM} from '../../core/track_manager';
import {TrackRenderer, Track} from '../../public/track';
import {TrackNode, Workspace} from '../../public/workspace';
import {Button} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu} from '../../widgets/menu';
import {TrackShell} from '../../widgets/track_shell';
import {Tree, TreeNode} from '../../widgets/tree';
import {COLOR_ACCENT} from '../css_constants';
import {calculateResolution} from './resolution';
import {Trace} from '../../public/trace';
import {Anchor, linkify} from '../../widgets/anchor';
import {showModal} from '../../widgets/modal';
import {Popup} from '../../widgets/popup';
import {CanvasColors} from '../../public/canvas_colors';
import {CodeSnippet} from '../../widgets/code_snippet';

export const TRACK_MIN_HEIGHT_SETTING = 'dev.perfetto.TrackMinHeightPx';
export const DEFAULT_TRACK_MIN_HEIGHT_PX = 18;
export const MINIMUM_TRACK_MIN_HEIGHT_PX = DEFAULT_TRACK_MIN_HEIGHT_PX;

function getTrackHeight(node: TrackNode, track?: TrackRenderer) {
  // Headless tracks have an effective height of 0.
  if (node.headless) return 0;

  const TRACK_HEIGHT_MIN_PX =
    (AppImpl.instance.settings
      .get(TRACK_MIN_HEIGHT_SETTING)
      ?.get() as number) ?? DEFAULT_TRACK_MIN_HEIGHT_PX;

  // Expanded summary tracks don't show any data, so make them a little more
  // compact to save space.
  if (node.isSummary && node.expanded) return TRACK_HEIGHT_MIN_PX;

  const trackHeight = track?.getHeight?.();
  if (trackHeight === undefined) return TRACK_HEIGHT_MIN_PX;

  // Limit the minimum height of a track, and also round up to the nearest
  // integer, as sub-integer DOM alignment can cause issues e.g. with sticky
  // positioning.
  return Math.ceil(Math.max(trackHeight, TRACK_HEIGHT_MIN_PX));
}

export interface TrackViewAttrs {
  // Render a lighter version of this track view (for when tracks are offscreen).
  readonly lite: boolean;
  readonly scrollToOnCreate?: boolean;
  readonly reorderable?: boolean;
  readonly removable?: boolean;
  readonly depth: number;
  readonly stickyTop: number;
  readonly collapsible: boolean;
  onTrackMouseOver(): void;
  onTrackMouseOut(): void;
}

/**
 * The `TrackView` class is responsible for managing and rendering individual
 * tracks in the `TrackTreeView` Mithril component. It handles operations such
 * as:
 *
 * - Rendering track content in the DOM and virtual canvas.
 * - Managing user interactions like dragging, panning, scrolling, and area
 *   selection.
 * - Tracking and displaying rendering performance metrics.
 */
export class TrackView {
  readonly node: TrackNode;
  readonly renderer?: TrackWithFSM;
  readonly height: number;
  readonly verticalBounds: VerticalBounds;

  private readonly trace: TraceImpl;
  private readonly descriptor?: Track;

  constructor(trace: TraceImpl, node: TrackNode, top: number) {
    this.trace = trace;
    this.node = node;

    if (node.uri) {
      this.descriptor = trace.tracks.getTrack(node.uri);
      this.renderer = this.trace.tracks.getTrackFSM(node.uri);
    }

    const heightPx = getTrackHeight(node, this.renderer?.track);
    this.height = heightPx;
    this.verticalBounds = {top, bottom: top + heightPx};
  }

  renderDOM(attrs: TrackViewAttrs, children: m.Children) {
    const {
      scrollToOnCreate,
      reorderable = false,
      collapsible,
      removable,
    } = attrs;
    const {node, renderer, height} = this;

    const description = renderer?.desc.description;

    const buttons = attrs.lite
      ? []
      : [
          renderer?.track.getTrackShellButtons?.(),
          description !== undefined &&
            this.renderHelpButton(
              typeof description === 'function'
                ? description()
                : linkify(description),
            ),
          (removable || node.removable) && this.renderCloseButton(),
          // We don't want summary tracks to be pinned as they rarely have
          // useful information.
          !node.isSummary && this.renderPinButton(),
          this.renderTrackMenuButton(),
          this.renderAreaSelectionCheckbox(),
        ];

    let scrollIntoView = false;
    const tracks = this.trace.tracks;
    if (tracks.scrollToTrackNodeId === node.id) {
      tracks.scrollToTrackNodeId = undefined;
      scrollIntoView = true;
    }

    function showTrackMoveErrorModal(msg: string) {
      showModal({
        title: 'Error',
        content: msg,
        buttons: [{text: 'OK'}],
      });
    }

    return m(
      TrackShell,
      {
        id: node.id,
        title: node.name,
        subtitle: renderer?.desc.subtitle,
        ref: node.fullPath.join('/'),
        heightPx: height,
        error: renderer?.getError(),
        chips: renderer?.desc.chips,
        buttons,
        scrollToOnCreate: scrollToOnCreate || scrollIntoView,
        collapsible: collapsible && node.hasChildren,
        collapsed: collapsible && node.collapsed,
        highlight: this.isHighlighted(),
        summary: node.isSummary,
        reorderable,
        depth: attrs.depth,
        stickyTop: attrs.stickyTop,
        pluginId: renderer?.desc.pluginId,
        lite: attrs.lite,
        onCollapsedChanged: () => {
          node.hasChildren && node.toggleCollapsed();
        },
        onTrackContentMouseMove: (pos, bounds) => {
          const timescale = this.getTimescaleForBounds(bounds);
          renderer?.track.onMouseMove?.({
            ...pos,
            timescale,
          });
          raf.scheduleCanvasRedraw();
          attrs.onTrackMouseOver();
        },
        onTrackContentMouseOut: () => {
          renderer?.track.onMouseOut?.();
          raf.scheduleCanvasRedraw();
          attrs.onTrackMouseOut();
        },
        onTrackContentClick: (pos, bounds) => {
          const timescale = this.getTimescaleForBounds(bounds);
          raf.scheduleCanvasRedraw();
          return (
            renderer?.track.onMouseClick?.({
              ...pos,
              timescale,
            }) ?? false
          );
        },
        onupdate: () => {
          renderer?.track.onFullRedraw?.();
        },
        onMoveBefore: (nodeId: string) => {
          // We are the reference node (the one to be moved relative to), nodeId
          // references the target node (the one to be moved)
          const nodeToMove = node.workspace?.getTrackById(nodeId);
          const targetNode = this.node.parent;
          if (nodeToMove && targetNode) {
            // Insert the target node before this one
            const result = targetNode.addChildBefore(nodeToMove, node);
            if (!result.ok) {
              showTrackMoveErrorModal(result.error);
            }
          }
        },
        onMoveInside: (nodeId: string) => {
          // This one moves the node inside this node & expand it if it's not
          // expanded already.
          const nodeToMove = node.workspace?.getTrackById(nodeId);
          if (nodeToMove) {
            const result = this.node.addChildLast(nodeToMove);
            if (result.ok) {
              this.node.expand();
            } else {
              showTrackMoveErrorModal(result.error);
            }
          }
        },
        onMoveAfter: (nodeId: string) => {
          // We are the reference node (the one to be moved relative to), nodeId
          // references the target node (the one to be moved)
          const nodeToMove = node.workspace?.getTrackById(nodeId);
          const targetNode = this.node.parent;
          if (nodeToMove && targetNode) {
            // Insert the target node after this one
            const result = targetNode.addChildAfter(nodeToMove, node);
            if (!result.ok) {
              showTrackMoveErrorModal(result.error);
            }
          }
        },
      },
      children,
    );
  }

  drawCanvas(
    ctx: CanvasRenderingContext2D,
    rect: Rect2D,
    visibleWindow: HighPrecisionTimeSpan,
    perfStatsEnabled: boolean,
    trackPerfStats: WeakMap<TrackNode, PerfStats>,
    colors: CanvasColors,
  ) {
    // For each track we rendered in view(), render it to the canvas. We know the
    // vertical bounds, so we just need to combine it with the horizontal bounds
    // and we're golden.
    const {node, renderer, verticalBounds} = this;

    if (node.isSummary && node.expanded) return;
    if (renderer?.getError()) return;

    const trackRect = new Rect2D({
      ...rect,
      ...verticalBounds,
    });

    // Track renderers expect to start rendering at (0, 0), so we need to
    // translate the canvas and create a new timescale.
    using _ = canvasSave(ctx);
    canvasClip(ctx, trackRect);
    ctx.translate(trackRect.left, trackRect.top);

    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: trackRect.width,
    });

    const maybeNewResolution = calculateResolution(
      visibleWindow,
      trackRect.width,
    );
    if (!maybeNewResolution.ok) {
      return;
    }

    const start = performance.now();
    node.uri &&
      renderer?.render({
        trackUri: node.uri,
        visibleWindow,
        size: trackRect,
        resolution: maybeNewResolution.value,
        ctx,
        timescale,
        colors,
      });

    this.highlightIfTrackInAreaSelection(ctx, timescale, trackRect);

    const renderTime = performance.now() - start;

    if (!perfStatsEnabled) return;
    this.updateAndRenderTrackPerfStats(
      ctx,
      trackRect,
      renderTime,
      trackPerfStats,
    );
  }

  private renderCloseButton() {
    return m(Button, {
      // TODO(stevegolton): It probably makes sense to only show this button
      // when hovered for consistency with the other buttons, but hiding this
      // button currently breaks the tests as we wait for the buttons to become
      // available, enabled and visible before clicking on them.
      // className: 'pf-visible-on-hover',
      onclick: () => {
        this.node.remove();
      },
      icon: Icons.Close,
      title: 'Remove track',
      compact: true,
    });
  }

  private renderPinButton(): m.Children {
    const isPinned = this.node.isPinned;
    return m(Button, {
      className: classNames(!isPinned && 'pf-visible-on-hover'),
      onclick: () => {
        isPinned ? this.node.unpin() : this.node.pin();
      },
      icon: Icons.Pin,
      iconFilled: isPinned,
      title: isPinned ? 'Unpin' : 'Pin to top',
      compact: true,
    });
  }

  private renderHelpButton(helpText: m.Children | string): m.Children {
    return m(
      Popup,
      {
        trigger: m(Button, {
          className: classNames('pf-visible-on-hover'),
          icon: Icons.Help,
          compact: true,
        }),
      },
      helpText,
    );
  }

  private renderTrackMenuButton(): m.Children {
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          className: 'pf-visible-on-hover',
          icon: 'more_vert',
          compact: true,
          title: 'Track options',
        }),
      },
      // Putting these menu items inside a component means that view is only
      // called when the popup is actually open, which can improve DOM
      // render performance when we have thousands of tracks on screen.
      m(TrackPopupMenu, {
        trace: this.trace,
        node: this.node,
        descriptor: this.descriptor,
      }),
    );
  }

  private getTimescaleForBounds(bounds: Bounds2D) {
    const timeWindow = this.trace.timeline.visibleWindow;
    return new TimeScale(timeWindow, {
      left: 0,
      right: bounds.right - bounds.left,
    });
  }

  private isHighlighted() {
    const {trace, node} = this;
    // The track should be highlighted if the current search result matches this
    // track or one of its children.
    const searchIndex = trace.search.resultIndex;
    const searchResults = trace.search.searchResults;

    if (searchIndex !== -1 && searchResults !== undefined) {
      // using _ = autoTimer();
      const uri = searchResults.trackUris[searchIndex];
      // Highlight if this or any children match the search results
      if (uri === node.uri || node.getTrackByUri(uri)) {
        return true;
      }
    }

    const curSelection = trace.selection;
    if (
      curSelection.selection.kind === 'track' &&
      curSelection.selection.trackUri === node.uri
    ) {
      return true;
    }

    return false;
  }

  private renderAreaSelectionCheckbox(): m.Children {
    const {trace, node} = this;
    const selectionManager = trace.selection;
    const selection = selectionManager.selection;
    if (selection.kind === 'area') {
      if (node.isSummary) {
        const tracksWithUris = node.flatTracks.filter(
          (t) => t.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;

        // Check if any nodes within are selected
        const childTracksInSelection = tracksWithUris.map((t) =>
          selection.trackUris.includes(t.uri),
        );

        function renderButton(icon: string, title: string) {
          return m(Button, {
            onclick: () => {
              const uris = tracksWithUris.map((t) => t.uri);
              selectionManager.toggleGroupAreaSelection(uris);
            },
            compact: true,
            icon,
            title,
          });
        }

        if (childTracksInSelection.every((b) => b)) {
          return renderButton(
            Icons.Checkbox,
            'Remove child tracks from selection',
          );
        } else if (childTracksInSelection.some((b) => b)) {
          return renderButton(
            Icons.IndeterminateCheckbox,
            'Add remaining child tracks to selection',
          );
        } else {
          return renderButton(
            Icons.BlankCheckbox,
            'Add child tracks to selection',
          );
        }
      } else {
        const nodeUri = node.uri;
        if (nodeUri) {
          return (
            selection.kind === 'area' &&
            m(Button, {
              onclick: () => {
                selectionManager.toggleTrackAreaSelection(nodeUri);
              },
              compact: true,
              ...(selection.trackUris.includes(nodeUri)
                ? {icon: Icons.Checkbox, title: 'Remove track'}
                : {icon: Icons.BlankCheckbox, title: 'Add track to selection'}),
            })
          );
        }
      }
    }
    return undefined;
  }

  private highlightIfTrackInAreaSelection(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    const selection = this.trace.selection.selection;

    if (selection.kind !== 'area') {
      return;
    }

    let selected = false;
    if (this.node.isSummary) {
      // Summary tracks cannot themselves be area-selected. So, as a visual aid,
      // if this track is a summary track and some of its children are in the
      // area selecion, highlight this track as if it were in the area
      // selection too.
      selected = selection.trackUris.some((uri) =>
        this.node.getTrackByUri(uri),
      );
    } else {
      // For non-summary tracks, simply highlight this track if it's in the area
      // selection.
      if (this.node.uri !== undefined) {
        selected = selection.trackUris.includes(this.node.uri);
      }
    }

    if (selected) {
      const startPx = timescale.timeToPx(selection.start);
      const endPx = timescale.timeToPx(selection.end);

      // Clamp to viewport bounds [0, size.width]
      const clampedStartPx = Math.max(0, startPx);
      const clampedEndPx = Math.min(size.width, endPx);
      const clampedWidth = clampedEndPx - clampedStartPx;

      // Only draw if there's a visible portion
      if (clampedWidth > 0) {
        ctx.globalAlpha = 0.3;
        ctx.fillStyle = COLOR_ACCENT;
        ctx.fillRect(clampedStartPx, 0, clampedWidth, size.height);
        ctx.globalAlpha = 1.0;
      }
    }
  }

  private updateAndRenderTrackPerfStats(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    renderTime: number,
    trackPerfStats: WeakMap<TrackNode, PerfStats>,
  ) {
    let renderStats = trackPerfStats.get(this.node);
    if (renderStats === undefined) {
      renderStats = new PerfStats();
      trackPerfStats.set(this.node, renderStats);
    }
    renderStats.addValue(renderTime);

    // Draw a green box around the whole track
    ctx.strokeStyle = 'rgba(69, 187, 73, 0.5)';
    const lineWidth = 1;
    ctx.lineWidth = lineWidth;
    ctx.strokeRect(
      lineWidth / 2,
      lineWidth / 2,
      size.width - lineWidth,
      size.height - lineWidth,
    );

    const statW = 300;
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'start';
    ctx.textBaseline = 'alphabetic';
    ctx.direction = 'inherit';
    ctx.fillStyle = 'hsl(97, 100%, 96%)';
    ctx.fillRect(size.width - statW, size.height - 20, statW, 20);
    ctx.fillStyle = 'hsla(122, 77%, 22%)';
    const statStr = `Track ${this.node.id} | ` + runningStatStr(renderStats);
    ctx.fillText(statStr, size.width - statW, size.height - 10);
  }
}

interface TrackPopupMenuAttrs {
  readonly trace: Trace;
  readonly node: TrackNode;
  readonly descriptor?: Track;
}

// This component contains the track menu items which are displayed inside a
// popup menu on each track. They're in a component to avoid having to render
// them every single mithril cycle.
const TrackPopupMenu = {
  view({attrs}: m.Vnode<TrackPopupMenuAttrs>) {
    return [
      m(MenuItem, {
        label: 'Select track',
        icon: 'select',
        disabled: !attrs.node.uri,
        onclick: () => {
          attrs.trace.selection.selectTrack(attrs.node.uri!);
        },
        title: attrs.node.uri
          ? 'Select track'
          : 'Track has no URI and cannot be selected',
      }),
      m(
        MenuItem,
        {label: 'Track details', icon: 'info'},
        renderTrackDetailsMenu(attrs.node, attrs.descriptor),
      ),
      m(MenuDivider),
      m(
        MenuItem,
        {label: 'Copy to workspace', icon: 'content_copy'},
        attrs.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: () => copyToWorkspace(attrs.trace, attrs.node, ws),
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          icon: 'add',
          onclick: () => copyToWorkspace(attrs.trace, attrs.node),
        }),
      ),
      m(
        MenuItem,
        {label: 'Copy & switch to workspace', icon: 'content_copy'},
        attrs.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            disabled: !ws.userEditable,
            onclick: async () => {
              copyToWorkspace(attrs.trace, attrs.node, ws);
              attrs.trace.workspaces.switchWorkspace(ws);
            },
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace...',
          icon: 'add',
          onclick: async () => {
            const ws = copyToWorkspace(attrs.trace, attrs.node);
            attrs.trace.workspaces.switchWorkspace(ws);
          },
        }),
      ),
      m(MenuDivider),
      m(MenuItem, {
        label: 'Rename',
        icon: 'edit',
        disabled: !attrs.node.workspace?.userEditable,
        onclick: async () => {
          const newName = await attrs.trace.omnibox.prompt('New name');
          if (newName) {
            attrs.node.name = newName;
          }
        },
      }),
      m(MenuItem, {
        label: 'Remove',
        icon: 'delete',
        disabled: !attrs.node.workspace?.userEditable,
        onclick: () => {
          attrs.node.remove();
        },
      }),
    ];
  },
};

function copyToWorkspace(trace: Trace, node: TrackNode, ws?: Workspace) {
  // If no workspace provided, create a new one.
  if (!ws) {
    ws = trace.workspaces.createEmptyWorkspace('Untitled Workspace');
  }
  // Deep clone makes sure all group's content is also copied
  const newNode = node.clone(true);
  newNode.removable = true;
  ws.addChildLast(newNode);
  return ws;
}

function renderTrackDetailsMenu(node: TrackNode, descriptor?: Track) {
  const fullPath = node.fullPath.join(' \u2023 ');
  const query = descriptor?.renderer.getDataset?.()?.query();

  return m(
    '.pf-track__track-details-popup',
    m(
      Tree,
      m(TreeNode, {left: 'Track Node ID', right: node.id}),
      m(TreeNode, {left: 'Collapsed', right: `${node.collapsed}`}),
      m(TreeNode, {left: 'URI', right: node.uri}),
      m(TreeNode, {
        left: 'Is Summary Track',
        right: `${node.isSummary}`,
      }),
      m(TreeNode, {
        left: 'SortOrder',
        right: node.sortOrder ?? '0 (undefined)',
      }),
      m(TreeNode, {left: 'Path', right: fullPath}),
      m(TreeNode, {left: 'Name', right: node.name}),
      m(TreeNode, {
        left: 'Workspace',
        right: node.workspace?.title ?? '[no workspace]',
      }),
      descriptor &&
        m(TreeNode, {
          left: 'Plugin ID',
          right: descriptor.pluginId,
        }),
      query &&
        m(TreeNode, {
          left: 'Track Query',
          right: m(
            Anchor,
            {
              onclick: () => {
                showModal({
                  title: 'Query for track',
                  content: () => m(CodeSnippet, {text: query, language: 'SQL'}),
                });
              },
            },
            'Show query',
          ),
        }),
      descriptor &&
        m(
          TreeNode,
          {left: 'Tags'},
          descriptor.tags &&
            Object.entries(descriptor.tags).map(([key, value]) => {
              return m(TreeNode, {left: key, right: value?.toString()});
            }),
        ),
    ),
  );
}
