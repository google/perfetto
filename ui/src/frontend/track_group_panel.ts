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
import {PxSpan, TimeScale} from '../base/time_scale';
import {exists} from '../base/utils';
import {classNames} from '../base/classnames';
import {GroupNode} from '../public/workspace';
import {raf} from '../core/raf_scheduler';
import {Actions} from '../common/actions';

interface Attrs {
  readonly groupNode: GroupNode;
  readonly title: m.Children;
  readonly tooltip: string;
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
    const {title, subtitle, chips, collapsed, trackRenderer, tooltip} =
      this.attrs;

    // The shell should be highlighted if the current search result is inside
    // this track group.
    let highlightClass = '';
    const searchIndex = globals.state.searchIndex;
    if (searchIndex !== -1) {
      const uri = globals.currentSearchResults.trackUris[searchIndex];
      if (this.attrs.groupNode.flatTracks.find((t) => t.uri === uri)) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.state.selection;

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

    return m(
      `.track-group-panel[collapsed=${collapsed}]`,
      {
        id: 'track_' + this.groupUri,
        oncreate: () => this.onupdate(),
        onupdate: () => this.onupdate(),
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
            {title: tooltip},
            title,
            chips && renderChips(chips),
          ),
          collapsed && exists(subtitle) && m('h2.track-subtitle', subtitle),
        ),
        m(
          '.track-buttons',
          error && m(CrashButton, {error}),
          selection.kind === 'area' &&
            m(Button, {
              onclick: (e: MouseEvent) => {
                globals.dispatch(
                  Actions.toggleGroupAreaSelection({
                    // Dump URIs of all contained tracks & nodes, including this group
                    trackUris: this.attrs.groupNode.flatNodes
                      .map((t) => t.uri)
                      .concat(this.attrs.groupNode.uri),
                  }),
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

  private onupdate() {
    if (this.attrs.trackRenderer !== undefined) {
      this.attrs.trackRenderer.track.onFullRedraw?.();
    }
  }

  highlightIfTrackSelected(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    const selection = globals.state.selection;
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
    const timescale = new TimeScale(
      visibleWindow,
      new PxSpan(0, trackSize.width),
    );

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
