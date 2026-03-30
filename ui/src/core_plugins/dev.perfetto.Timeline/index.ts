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
import z from 'zod';
import {DisposableStack} from '../../base/disposable_stack';
import {toHTMLElement} from '../../base/dom_utils';
import {Rect2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {AppImpl} from '../../core/app_impl';
import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {trackMatchesFilter} from '../../core/track_manager';
import {ResizeHandle} from '../../widgets/resize_handle';
import {
  setTrackShellWidth,
  TRACK_SHELL_WIDTH,
} from '../../frontend/css_constants';
import {Minimap} from './minimap';
import {TabPanel} from './tab_panel';
import {TimelineHeader} from './timeline_header';
import {TrackTreeView} from './track_tree_view';
import {
  DEFAULT_TRACK_MIN_HEIGHT_PX,
  MINIMUM_TRACK_MIN_HEIGHT_PX,
  TRACK_MIN_HEIGHT_SETTING,
} from './track_view';
import {KeyboardNavigationHandler} from './wasd_navigation_handler';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';

const MIN_TRACK_SHELL_WIDTH = 100;
const MAX_TRACK_SHELL_WIDTH = 1000;

export default class TimelinePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Timeline';
  static readonly description = 'The main timeline view';
  private static minimapFlag: Flag;

  static onActivate(app: AppImpl): void {
    // This setting is referenced in the track view by name
    app.settings.register({
      id: TRACK_MIN_HEIGHT_SETTING,
      name: 'Track Height',
      description:
        'Minimum height of tracks in the trace viewer page, in pixels.',
      schema: z.number().int().min(MINIMUM_TRACK_MIN_HEIGHT_PX),
      defaultValue: DEFAULT_TRACK_MIN_HEIGHT_PX,
    });

    TimelinePlugin.minimapFlag = app.featureFlags.register({
      id: 'overviewVisible',
      name: 'Overview Panel',
      description: 'Show the panel providing an overview of the trace',
      defaultValue: true,
    });
  }

  async onTraceLoad(trace: TraceImpl): Promise<void> {
    trace.pages.registerPage({
      route: '/viewer',
      render: () => {
        return m(TimelinePage, {
          trace,
          showMinimap: TimelinePlugin.minimapFlag.get(),
        });
      },
    });
  }
}

interface TimelinePageAttrs {
  readonly trace: TraceImpl;
  readonly showMinimap: boolean;
}

class TimelinePage implements m.ClassComponent<TimelinePageAttrs> {
  private readonly trash = new DisposableStack();
  private timelineBounds?: Rect2D;
  private pinnedTracksHeight: number | 'auto' = 'auto';

  view({attrs}: m.CVnode<TimelinePageAttrs>) {
    const {trace} = attrs;
    return m(
      '.pf-timeline-page',
      m(
        TabPanel,
        {trace},
        attrs.showMinimap && this.renderMinimap(trace),
        this.renderTimeline(trace),
      ),
    );
  }

  private renderTimeline(trace: TraceImpl): m.Children {
    return m(
      '.pf-timeline-page__timeline',
      this.renderHeader(trace),
      this.renderTracks(trace),
      this.renderTrackShellResizeHandle(),
    );
  }

  private renderTrackShellResizeHandle(): m.Children {
    return m(ResizeHandle, {
      direction: 'horizontal',
      style: {
        position: 'absolute',
        left: `${TRACK_SHELL_WIDTH}px`,
        top: '0',
        bottom: '0',
      },
      onResizeAbsolute: (positionPx: number) => {
        const clamped = Math.max(
          MIN_TRACK_SHELL_WIDTH,
          Math.min(MAX_TRACK_SHELL_WIDTH, positionPx),
        );
        setTrackShellWidth(clamped);
        raf.scheduleFullRedraw();
      },
    });
  }

  private renderMinimap(trace: TraceImpl): m.Children {
    return m(Minimap, {
      trace,
      className: 'pf-timeline-page__overview',
    });
  }

  private renderHeader(trace: TraceImpl): m.Children {
    return m(TimelineHeader, {
      trace,
      className: 'pf-timeline-page__header',
      // There are three independent canvases on this page which we could
      // use keep track of the timeline width, but we use the header one
      // because it's always rendered.
      onTimelineBoundsChange: (rect) => (this.timelineBounds = rect),
    });
  }

  private renderTracks(trace: TraceImpl): m.Children {
    // Hide tracks while the trace is loading to prevent thrashing.
    if (AppImpl.instance.isLoadingTrace) return null;

    return [this.renderPinnedTracks(trace), this.renderMainTracks(trace)];
  }

  private renderPinnedTracks(trace: TraceImpl): m.Children {
    if (trace.currentWorkspace.pinnedTracks.length === 0) return null;

    return [
      m(
        '.pf-timeline-page__pinned-track-tree',
        {
          style:
            this.pinnedTracksHeight === 'auto'
              ? {maxHeight: '40%'}
              : {height: `${this.pinnedTracksHeight}px`},
        },
        m(TrackTreeView, {
          trace,
          rootNode: trace.currentWorkspace.pinnedTracksNode,
          canReorderNodes: true,
          scrollToNewTracks: true,
        }),
      ),
      m(ResizeHandle, {
        onResize: (deltaPx: number) => {
          if (this.pinnedTracksHeight === 'auto') {
            this.pinnedTracksHeight = toHTMLElement(
              document.querySelector('.pf-timeline-page__pinned-track-tree')!,
            ).getBoundingClientRect().height;
          }
          this.pinnedTracksHeight = this.pinnedTracksHeight + deltaPx;
          m.redraw();
        },
        ondblclick: () => {
          this.pinnedTracksHeight = 'auto';
        },
      }),
    ];
  }

  private renderMainTracks(trace: TraceImpl): m.Children {
    return m(TrackTreeView, {
      trace,
      className: 'pf-timeline-page__scrolling-track-tree',
      rootNode: trace.currentWorkspace.tracks,
      canReorderNodes: trace.currentWorkspace.userEditable,
      canRemoveNodes: trace.currentWorkspace.userEditable,
      trackFilter: (track) => trackMatchesFilter(trace, track),
    });
  }

  oncreate(vnode: m.VnodeDOM<TimelinePageAttrs>) {
    const {attrs, dom} = vnode;

    // Handles WASD keybindings to pan & zoom
    const panZoomHandler = new KeyboardNavigationHandler({
      element: toHTMLElement(dom),
      onPanned: (pannedPx: number) => {
        if (!this.timelineBounds) return;
        const timeline = attrs.trace.timeline;
        const timescale = new TimeScale(
          timeline.visibleWindow,
          this.timelineBounds,
        );
        const tDelta = timescale.pxToDuration(pannedPx);
        timeline.pan(tDelta);
        raf.scheduleCanvasRedraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        if (!this.timelineBounds) return;
        const timeline = attrs.trace.timeline;
        const zoomPx = zoomedPositionPx - this.timelineBounds.left;
        const centerPoint = zoomPx / this.timelineBounds.width;
        timeline.zoom(1 - zoomRatio, centerPoint);
        raf.scheduleCanvasRedraw();
      },
    });
    this.trash.use(panZoomHandler);
  }

  onremove() {
    this.trash.dispose();
  }
}
