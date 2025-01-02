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
import {PerfStats, runningStatStr} from '../../core/perf_stats';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {TrackRenderer} from '../../core/track_manager';
import {Track, TrackDescriptor} from '../../public/track';
import {TrackNode, Workspace} from '../../public/workspace';
import {Button} from '../../widgets/button';
import {MenuDivider, MenuItem, PopupMenu2} from '../../widgets/menu';
import {TrackShell} from '../../widgets/track_shell';
import {Tree, TreeNode} from '../../widgets/tree';
import {SELECTION_FILL_COLOR} from '../css_constants';
import {calculateResolution} from './resolution';

const TRACK_HEIGHT_MIN_PX = 18;
const TRACK_HEIGHT_DEFAULT_PX = 30;

function getTrackHeight(node: TrackNode, track?: Track) {
  // Headless tracks have an effective height of 0.
  if (node.headless) return 0;

  // Expanded summary tracks don't show any data, so make them a little more
  // compact to save space.
  if (node.isSummary && node.expanded) return TRACK_HEIGHT_DEFAULT_PX;

  const trackHeight = track?.getHeight();
  if (trackHeight === undefined) return TRACK_HEIGHT_DEFAULT_PX;

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
  readonly depth: number;
  readonly stickyTop: number;
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
  readonly renderer?: TrackRenderer;
  readonly height: number;
  readonly verticalBounds: VerticalBounds;

  private readonly trace: TraceImpl;
  private readonly descriptor?: TrackDescriptor;

  constructor(trace: TraceImpl, node: TrackNode, top: number) {
    this.trace = trace;
    this.node = node;

    if (node.uri) {
      this.descriptor = trace.tracks.getTrack(node.uri);
      this.renderer = this.trace.tracks.getTrackRenderer(node.uri);
    }

    const heightPx = getTrackHeight(node, this.renderer?.track);
    this.height = heightPx;
    this.verticalBounds = {top, bottom: top + heightPx};
  }

  renderDOM(attrs: TrackViewAttrs, children: m.Children) {
    const {scrollToOnCreate, reorderable = false} = attrs;
    const {node, renderer, height} = this;

    const buttons = attrs.lite
      ? []
      : [
          renderer?.track.getTrackShellButtons?.(),
          node.removable && this.renderCloseButton(),
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

    return m(
      TrackShell,
      {
        id: node.id,
        title: node.title,
        subtitle: renderer?.desc.subtitle,
        ref: node.fullPath.join('/'),
        heightPx: height,
        error: renderer?.getError(),
        chips: renderer?.desc.chips,
        buttons,
        scrollToOnCreate: scrollToOnCreate || scrollIntoView,
        collapsible: node.hasChildren,
        collapsed: node.collapsed,
        highlight: this.isHighlighted(),
        summary: node.isSummary,
        reorderable,
        depth: attrs.depth,
        stickyTop: attrs.stickyTop,
        pluginId: renderer?.desc.pluginId,
        lite: attrs.lite,
        onToggleCollapsed: () => {
          node.hasChildren && node.toggleCollapsed();
        },
        onTrackContentMouseMove: (pos, bounds) => {
          const timescale = this.getTimescaleForBounds(bounds);
          renderer?.track.onMouseMove?.({
            ...pos,
            timescale,
          });
          raf.scheduleCanvasRedraw();
        },
        onTrackContentMouseOut: () => {
          renderer?.track.onMouseOut?.();
          raf.scheduleCanvasRedraw();
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
          const targetNode = node.workspace?.getTrackById(nodeId);
          if (targetNode !== undefined) {
            // Insert the target node before this one
            targetNode.parent?.addChildBefore(targetNode, node);
          }
        },
        onMoveAfter: (nodeId: string) => {
          const targetNode = node.workspace?.getTrackById(nodeId);
          if (targetNode !== undefined) {
            // Insert the target node after this one
            targetNode.parent?.addChildAfter(targetNode, node);
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

    const start = performance.now();

    node.uri &&
      renderer?.render({
        trackUri: node.uri,
        visibleWindow,
        size: trackRect,
        resolution: calculateResolution(visibleWindow, trackRect.width),
        ctx,
        timescale,
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
      onclick: () => {
        this.node.remove();
      },
      icon: Icons.Close,
      title: 'Close track',
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

  private renderTrackMenuButton(): m.Children {
    return m(
      PopupMenu2,
      {
        trigger: m(Button, {
          className: 'pf-visible-on-hover',
          icon: 'more_vert',
          compact: true,
          title: 'Track options',
        }),
      },
      m(MenuItem, {
        label: 'Select track',
        disabled: !this.node.uri,
        onclick: () => {
          this.trace.selection.selectTrack(this.node.uri!);
        },
        title: this.node.uri
          ? 'Select track'
          : 'Track has no URI and cannot be selected',
      }),
      m(MenuItem, {label: 'Track details'}, this.renderTrackDetails()),
      m(MenuDivider),
      m(
        MenuItem,
        {label: 'Copy to workspace'},
        this.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            onclick: () => this.copyToWorkspace(ws),
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace',
          onclick: () => this.copyToWorkspace(),
        }),
      ),
      m(
        MenuItem,
        {label: 'Take to workspace'},
        this.trace.workspaces.all.map((ws) =>
          m(MenuItem, {
            label: ws.title,
            onclick: async () => {
              await this.copyToWorkspace(ws);
              this.trace.workspaces.switchWorkspace(ws);
            },
          }),
        ),
        m(MenuDivider),
        m(MenuItem, {
          label: 'New workspace',
          onclick: async () => {
            const ws = await this.copyToWorkspace();
            ws && this.trace.workspaces.switchWorkspace(ws);
          },
        }),
      ),
    );
  }

  private async copyToWorkspace(ws?: Workspace) {
    if (!ws) {
      const name = await this.trace.omnibox.prompt(
        'Enter a name for the new workspace...',
      );
      if (!name) return;
      ws = this.trace.workspaces.createEmptyWorkspace(name);
    }
    const newNode = this.node.clone();
    newNode.removable = true;
    ws.addChildLast(newNode);
    return ws;
  }

  private renderTrackDetails(): m.Children {
    let parent = this.node.parent;
    let fullPath: m.ChildArray = [this.node.title];
    while (parent && parent instanceof TrackNode) {
      fullPath = [parent.title, ' \u2023 ', ...fullPath];
      parent = parent.parent;
    }

    return m(
      '.pf-track__track-details-popup',
      m(
        Tree,
        m(TreeNode, {left: 'Track Node ID', right: this.node.id}),
        m(TreeNode, {left: 'Collapsed', right: `${this.node.collapsed}`}),
        m(TreeNode, {left: 'URI', right: this.node.uri}),
        m(TreeNode, {
          left: 'Is Summary Track',
          right: `${this.node.isSummary}`,
        }),
        m(TreeNode, {
          left: 'SortOrder',
          right: this.node.sortOrder ?? '0 (undefined)',
        }),
        m(TreeNode, {left: 'Path', right: fullPath}),
        m(TreeNode, {left: 'Title', right: this.node.title}),
        m(TreeNode, {
          left: 'Workspace',
          right: this.node.workspace?.title ?? '[no workspace]',
        }),
        this.descriptor &&
          m(TreeNode, {
            left: 'Plugin ID',
            right: this.descriptor.pluginId,
          }),
        this.descriptor &&
          m(
            TreeNode,
            {left: 'Tags'},
            this.descriptor.tags &&
              Object.entries(this.descriptor.tags).map(([key, value]) => {
                return m(TreeNode, {left: key, right: value?.toString()});
              }),
          ),
      ),
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
      // area selection, highlight this track as if it were in the area
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
      const selectedAreaDuration = selection.end - selection.start;
      ctx.fillStyle = SELECTION_FILL_COLOR;
      ctx.fillRect(
        timescale.timeToPx(selection.start),
        0,
        timescale.durationToPx(selectedAreaDuration),
        size.height,
      );
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