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
import {Icons} from '../base/semantic_icons';
import {TrackRenderer} from '../core/track_manager';
import {TrackTags} from '../public/track';
import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {Size2D} from '../base/geom';
import {Panel} from './panel_container';
import {
  CrashButton,
  drawGridLines,
  renderChips,
  renderHoveredCursorVertical,
  renderHoveredNoteVertical,
  renderNoteVerticals,
  renderWakeupVertical,
  TrackContent,
} from './track_panel';
import {canvasClip} from '../base/canvas_utils';
import {Button} from '../widgets/button';
import {TrackRenderContext} from '../public/track';
import {calculateResolution} from '../common/resolution';
import {TimeScale} from '../base/time_scale';
import {exists} from '../base/utils';
import {classNames} from '../base/classnames';
import {GroupNode} from '../public/workspace';
import {raf} from '../core/raf_scheduler';
import {MiddleEllipsis} from '../widgets/middle_ellipsis';

interface Attrs {
  readonly groupNode: GroupNode;
  readonly title: string;
  readonly collapsed: boolean;
  readonly collapsable: boolean;
  readonly trackRenderer?: TrackRenderer;
  readonly tags?: TrackTags;
  readonly subtitle?: string;
  readonly chips?: ReadonlyArray<string>;
}

export class TrackGroupPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  readonly groupUri: string;

  constructor(private readonly attrs: Attrs) {
    this.groupUri = attrs.groupNode.uri;
  }

  render(): m.Children {
    const {title, subtitle, collapsed, trackRenderer} = this.attrs;

    // The shell should be highlighted if the current search result is inside
    // this track group.
    let highlightClass = '';
    const searchIndex = globals.searchManager.resultIndex;
    const searchResults = globals.searchManager.searchResults;
    if (searchIndex !== -1 && searchResults !== undefined) {
      const uri = searchResults.trackUris[searchIndex];
      if (this.attrs.groupNode.flatTracks.find((t) => t.uri === uri)) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.selectionManager.selection;

    // const trackGroup = globals.state.trackGroups[groupKey];
    let checkBox = Icons.BlankCheckbox;
    if (selection.kind === 'area') {
      if (
        this.attrs.groupNode.flatTracks.every((track) =>
          selection.trackUris.includes(track.uri),
        )
      ) {
        checkBox = Icons.Checkbox;
      } else if (
        this.attrs.groupNode.flatTracks.some((track) =>
          selection.trackUris.includes(track.uri),
        )
      ) {
        checkBox = Icons.IndeterminateCheckbox;
      }
    }

    const error = trackRenderer?.getError();
    const chips = this.attrs.chips && renderChips(this.attrs.chips);

    return m(
      `.track-group-panel[collapsed=${collapsed}]`,
      {
        id: 'track_' + this.groupUri,
        oncreate: (vnode) => this.onupdate(vnode),
        onupdate: (vnode) => this.onupdate(vnode),
      },
      m(
        `.shell`,
        {
          className: classNames(
            this.attrs.collapsable && 'pf-clickable',
            highlightClass,
          ),
          onclick: (e: MouseEvent) => {
            if (e.defaultPrevented) return;
            if (this.attrs.collapsable) {
              this.attrs.groupNode.toggleCollapsed();
              raf.scheduleFullRedraw();
            }
            e.stopPropagation();
          },
        },
        this.attrs.collapsable &&
          m(
            '.fold-button',
            m(
              'i.material-icons',
              collapsed ? Icons.ExpandDown : Icons.ExpandUp,
            ),
          ),
        m(
          '.title-wrapper',
          m(
            'h1.track-title',
            {
              ref: this.attrs.title,
            },
            m('.popup', title, chips),
            m(MiddleEllipsis, {text: title}, chips),
          ),
          collapsed && exists(subtitle) && m('h2.track-subtitle', subtitle),
        ),
        m(
          '.track-buttons',
          error && m(CrashButton, {error}),
          selection.kind === 'area' &&
            m(Button, {
              onclick: (e: MouseEvent) => {
                globals.selectionManager.toggleGroupAreaSelection(
                  // Dump URIs of all contained tracks & nodes, including this group
                  this.attrs.groupNode.flatNodes
                    .map((t) => t.uri)
                    .concat(this.attrs.groupNode.uri),
                );
                e.stopPropagation();
              },
              icon: checkBox,
              compact: true,
            }),
        ),
      ),
      trackRenderer
        ? m(
            TrackContent,
            {
              track: trackRenderer.track,
              hasError: Boolean(trackRenderer.getError()),
              height: this.attrs.trackRenderer?.track.getHeight(),
            },
            !collapsed && subtitle !== null ? m('span', subtitle) : null,
          )
        : null,
    );
  }

  private onupdate({dom}: m.VnodeDOM) {
    this.decidePopupRequired(dom);

    if (this.attrs.trackRenderer !== undefined) {
      this.attrs.trackRenderer.track.onFullRedraw?.();
    }
  }

  // Works out whether to display a title popup on hover, based on whether the
  // current title is truncated.
  private decidePopupRequired(dom: Element) {
    const popupElement = dom.querySelector('.popup') as HTMLElement;
    const titleElement = dom.querySelector(
      '.pf-middle-ellipsis',
    ) as HTMLElement;

    if (popupElement.clientWidth >= titleElement.clientWidth) {
      popupElement.classList.add('show-popup');
    } else {
      popupElement.classList.remove('show-popup');
    }
  }

  highlightIfTrackSelected(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    const selection = globals.selectionManager.selection;
    if (selection.kind !== 'area') return;
    const someSelected = this.attrs.groupNode.flatTracks.some((track) =>
      selection.trackUris.includes(track.uri),
    );
    const selectedAreaDuration = selection.end - selection.start;
    if (someSelected) {
      ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
      ctx.fillRect(
        timescale.timeToPx(selection.start),
        0,
        timescale.durationToPx(selectedAreaDuration),
        size.height,
      );
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    const {collapsed, trackRenderer: track} = this.attrs;

    if (!collapsed) return;

    const trackSize = {
      width: size.width - TRACK_SHELL_WIDTH,
      height: size.height,
    };

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);

    const visibleWindow = globals.timeline.visibleWindow;
    const timespan = visibleWindow.toTimeSpan();
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: trackSize.width,
    });

    drawGridLines(ctx, timespan, timescale, trackSize);

    if (track) {
      if (!track.getError()) {
        const trackRenderCtx: TrackRenderContext = {
          visibleWindow,
          size: trackSize,
          ctx,
          trackUri: track.desc.uri,
          resolution: calculateResolution(visibleWindow, trackSize.width),
          timescale,
        };
        track.render(trackRenderCtx);
      }
    }

    this.highlightIfTrackSelected(ctx, timescale, size);

    // Draw vertical line when hovering on the notes panel.
    renderHoveredNoteVertical(ctx, timescale, size);
    renderHoveredCursorVertical(ctx, timescale, size);
    renderWakeupVertical(ctx, timescale, size);
    renderNoteVerticals(ctx, timescale, size);

    ctx.restore();
  }
}
