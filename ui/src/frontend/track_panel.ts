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

import m from 'mithril';
import {canvasClip, canvasSave} from '../base/canvas_utils';
import {classNames} from '../base/classnames';
import {Bounds2D, Size2D, VerticalBounds} from '../base/geom';
import {Icons} from '../base/semantic_icons';
import {TimeScale} from '../base/time_scale';
import {RequiredField} from '../base/utils';
import {calculateResolution} from '../common/resolution';
import {featureFlags} from '../core/feature_flags';
import {TrackRenderer} from '../core/track_manager';
import {TrackDescriptor, TrackRenderContext} from '../public/track';
import {TrackNode} from '../public/workspace';
import {Button} from '../widgets/button';
import {Popup, PopupPosition} from '../widgets/popup';
import {Tree, TreeNode} from '../widgets/tree';
import {SELECTION_FILL_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {Panel} from './panel_container';
import {TrackWidget} from '../widgets/track_widget';
import {raf} from '../core/raf_scheduler';
import {Intent} from '../widgets/common';
import {TraceImpl} from '../core/trace_impl';

const SHOW_TRACK_DETAILS_BUTTON = featureFlags.register({
  id: 'showTrackDetailsButton',
  name: 'Show track details button',
  description: 'Show track details button in track shells.',
  defaultValue: false,
});

// Default height of a track element that has no track, or is collapsed.
// Note: This is designed to roughly match the height of a cpu slice track.
export const DEFAULT_TRACK_HEIGHT_PX = 30;

interface TrackPanelAttrs {
  readonly trace: TraceImpl;
  readonly node: TrackNode;
  readonly indentationLevel: number;
  readonly trackRenderer?: TrackRenderer;
  readonly revealOnCreate?: boolean;
  readonly topOffsetPx: number;
  readonly reorderable?: boolean;
}

export class TrackPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  readonly trackNode?: TrackNode;

  private readonly attrs: TrackPanelAttrs;

  constructor(attrs: TrackPanelAttrs) {
    this.attrs = attrs;
    this.trackNode = attrs.node;
  }

  get heightPx(): number {
    const {trackRenderer, node} = this.attrs;

    // If the node is a summary track and is expanded, shrink it to save
    // vertical real estate).
    if (node.isSummary && node.expanded) return DEFAULT_TRACK_HEIGHT_PX;

    // Otherwise return the height of the track, if we have one.
    return trackRenderer?.track.getHeight() ?? DEFAULT_TRACK_HEIGHT_PX;
  }

  render(): m.Children {
    const {
      node,
      indentationLevel,
      trackRenderer,
      revealOnCreate,
      topOffsetPx,
      reorderable = false,
    } = this.attrs;

    const error = trackRenderer?.getError();

    const buttons = [
      SHOW_TRACK_DETAILS_BUTTON.get() &&
        renderTrackDetailsButton(node, trackRenderer?.desc),
      trackRenderer?.track.getTrackShellButtons?.(),
      node.removable && renderCloseButton(node),
      // Can't pin groups.. yet!
      !node.hasChildren && renderPinButton(node),
      this.renderAreaSelectionCheckbox(node),
      error && renderCrashButton(error, trackRenderer?.desc.pluginId),
    ];

    let scrollIntoView = false;
    const tracks = this.attrs.trace.tracks;
    if (tracks.scrollToTrackNodeId === node.id) {
      tracks.scrollToTrackNodeId = undefined;
      scrollIntoView = true;
    }

    return m(TrackWidget, {
      id: node.id,
      title: node.title,
      path: node.fullPath.join('/'),
      heightPx: this.heightPx,
      error: Boolean(trackRenderer?.getError()),
      chips: trackRenderer?.desc.chips,
      indentationLevel,
      topOffsetPx,
      buttons,
      revealOnCreate: revealOnCreate || scrollIntoView,
      collapsible: node.hasChildren,
      collapsed: node.collapsed,
      highlight: this.isHighlighted(node),
      isSummary: node.isSummary,
      reorderable,
      onToggleCollapsed: () => {
        node.hasChildren && node.toggleCollapsed();
      },
      onTrackContentMouseMove: (pos, bounds) => {
        const timescale = this.getTimescaleForBounds(bounds);
        trackRenderer?.track.onMouseMove?.({
          ...pos,
          timescale,
        });
        raf.scheduleRedraw();
      },
      onTrackContentMouseOut: () => {
        trackRenderer?.track.onMouseOut?.();
        raf.scheduleRedraw();
      },
      onTrackContentClick: (pos, bounds) => {
        const timescale = this.getTimescaleForBounds(bounds);
        raf.scheduleRedraw();
        return (
          trackRenderer?.track.onMouseClick?.({
            ...pos,
            timescale,
          }) ?? false
        );
      },
      onupdate: () => {
        trackRenderer?.track.onFullRedraw?.();
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
    });
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    const {trackRenderer: tr, node} = this.attrs;

    // Don't render if expanded and isSummary
    if (node.isSummary && node.expanded) {
      return;
    }

    const trackSize = {
      width: size.width - TRACK_SHELL_WIDTH,
      height: size.height,
    };

    using _ = canvasSave(ctx);
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);

    const visibleWindow = this.attrs.trace.timeline.visibleWindow;
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: trackSize.width,
    });

    if (tr) {
      if (!tr.getError()) {
        const trackRenderCtx: TrackRenderContext = {
          trackUri: tr.desc.uri,
          visibleWindow,
          size: trackSize,
          resolution: calculateResolution(visibleWindow, trackSize.width),
          ctx,
          timescale,
        };
        tr.render(trackRenderCtx);
      }
    }

    this.highlightIfTrackInAreaSelection(ctx, timescale, node, trackSize);
  }

  getSliceVerticalBounds(depth: number): VerticalBounds | undefined {
    if (this.attrs.trackRenderer === undefined) {
      return undefined;
    }
    return this.attrs.trackRenderer.track.getSliceVerticalBounds?.(depth);
  }

  private getTimescaleForBounds(bounds: Bounds2D) {
    const timeWindow = this.attrs.trace.timeline.visibleWindow;
    return new TimeScale(timeWindow, {
      left: 0,
      right: bounds.right - bounds.left,
    });
  }

  private isHighlighted(node: TrackNode) {
    // The track should be highlighted if the current search result matches this
    // track or one of its children.
    const searchIndex = this.attrs.trace.search.resultIndex;
    const searchResults = this.attrs.trace.search.searchResults;

    if (searchIndex !== -1 && searchResults !== undefined) {
      const uri = searchResults.trackUris[searchIndex];
      // Highlight if this or any children match the search results
      if (uri === node.uri || node.flatTracks.find((t) => t.uri === uri)) {
        return true;
      }
    }

    const curSelection = this.attrs.trace.selection;
    if (
      curSelection.selection.kind === 'track' &&
      curSelection.selection.trackUri === node.uri
    ) {
      return true;
    }

    return false;
  }

  private highlightIfTrackInAreaSelection(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    node: TrackNode,
    size: Size2D,
  ) {
    const selection = this.attrs.trace.selection.selection;
    if (selection.kind !== 'area') {
      return;
    }

    const tracksWithUris = node.flatTracks.filter(
      (t) => t.uri !== undefined,
    ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;

    let selected = false;
    if (node.isSummary) {
      selected = tracksWithUris.some((track) =>
        selection.trackUris.includes(track.uri),
      );
    } else {
      if (node.uri) {
        selected = selection.trackUris.includes(node.uri);
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

  private renderAreaSelectionCheckbox(node: TrackNode): m.Children {
    const selectionManager = this.attrs.trace.selection;
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
        if (childTracksInSelection.every((b) => b)) {
          return m(Button, {
            onclick: (e: MouseEvent) => {
              const uris = tracksWithUris.map((t) => t.uri);
              selectionManager.toggleGroupAreaSelection(uris);
              e.stopPropagation();
            },
            compact: true,
            icon: Icons.Checkbox,
            title: 'Remove child tracks from selection',
          });
        } else if (childTracksInSelection.some((b) => b)) {
          return m(Button, {
            onclick: (e: MouseEvent) => {
              const uris = tracksWithUris.map((t) => t.uri);
              selectionManager.toggleGroupAreaSelection(uris);
              e.stopPropagation();
            },
            compact: true,
            icon: Icons.IndeterminateCheckbox,
            title: 'Add remaining child tracks to selection',
          });
        } else {
          return m(Button, {
            onclick: (e: MouseEvent) => {
              const uris = tracksWithUris.map((t) => t.uri);
              selectionManager.toggleGroupAreaSelection(uris);
              e.stopPropagation();
            },
            compact: true,
            icon: Icons.BlankCheckbox,
            title: 'Add child tracks to selection',
          });
        }
      } else {
        const nodeUri = node.uri;
        if (nodeUri) {
          return (
            selection.kind === 'area' &&
            m(Button, {
              onclick: (e: MouseEvent) => {
                selectionManager.toggleTrackAreaSelection(nodeUri);
                e.stopPropagation();
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
}

function renderCrashButton(error: Error, pluginId?: string) {
  return m(
    Popup,
    {
      trigger: m(Button, {
        icon: Icons.Crashed,
        compact: true,
      }),
    },
    m(
      '.pf-track-crash-popup',
      m('span', 'This track has crashed.'),
      pluginId && m('span', `Owning plugin: ${pluginId}`),
      m(Button, {
        label: 'View & Report Crash',
        intent: Intent.Primary,
        className: Popup.DISMISS_POPUP_GROUP_CLASS,
        onclick: () => {
          throw error;
        },
      }),
      // TODO(stevegolton): In the future we should provide a quick way to
      // disable the plugin, or provide a link to the plugin page, but this
      // relies on the plugin page being fully functional.
    ),
  );
}

function renderCloseButton(node: TrackNode) {
  return m(Button, {
    onclick: (e) => {
      node.remove();
      e.stopPropagation();
    },
    icon: Icons.Close,
    title: 'Close track',
    compact: true,
  });
}

function renderPinButton(node: TrackNode): m.Children {
  const isPinned = node.isPinned;
  return m(Button, {
    className: classNames(!isPinned && 'pf-visible-on-hover'),
    onclick: (e) => {
      isPinned ? node.unpin() : node.pin();
      e.stopPropagation();
    },
    icon: Icons.Pin,
    iconFilled: isPinned,
    title: isPinned ? 'Unpin' : 'Pin to top',
    compact: true,
  });
}

function renderTrackDetailsButton(
  node: TrackNode,
  td?: TrackDescriptor,
): m.Children {
  let parent = node.parent;
  let fullPath: m.ChildArray = [node.title];
  while (parent && parent instanceof TrackNode) {
    fullPath = [parent.title, ' \u2023 ', ...fullPath];
    parent = parent.parent;
  }
  return m(
    Popup,
    {
      trigger: m(Button, {
        className: 'pf-visible-on-hover',
        icon: 'info',
        title: 'Show track details',
        compact: true,
      }),
      position: PopupPosition.Bottom,
    },
    m(
      '.pf-track-details-dropdown',
      m(
        Tree,
        m(TreeNode, {left: 'Track Node ID', right: node.id}),
        m(TreeNode, {left: 'Collapsed', right: `${node.collapsed}`}),
        m(TreeNode, {left: 'URI', right: node.uri}),
        m(TreeNode, {left: 'Is Summary Track', right: `${node.isSummary}`}),
        m(TreeNode, {
          left: 'SortOrder',
          right: node.sortOrder ?? '0 (undefined)',
        }),
        m(TreeNode, {left: 'Path', right: fullPath}),
        m(TreeNode, {left: 'Title', right: node.title}),
        m(TreeNode, {
          left: 'Workspace',
          right: node.workspace?.title ?? '[no workspace]',
        }),
        td && m(TreeNode, {left: 'Plugin ID', right: td.pluginId}),
        td &&
          m(
            TreeNode,
            {left: 'Tags'},
            td.tags &&
              Object.entries(td.tags).map(([key, value]) => {
                return m(TreeNode, {left: key, right: value?.toString()});
              }),
          ),
      ),
    ),
  );
}
