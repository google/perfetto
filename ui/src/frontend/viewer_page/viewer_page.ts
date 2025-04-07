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
import {DisposableStack} from '../../base/disposable_stack';
import {toHTMLElement} from '../../base/dom_utils';
import {Rect2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {AppImpl} from '../../core/app_impl';
import {featureFlags} from '../../core/feature_flags';
import {PageWithTraceImplAttrs} from '../../core/page_manager';
import {raf} from '../../core/raf_scheduler';
import {OverviewTimeline} from './overview_timeline_panel';
import {TabPanel} from './tab_panel';
import {TimelineHeader} from './timeline_header';
import {TrackTreeView} from './track_tree_view';
import {KeyboardNavigationHandler} from './wasd_navigation_handler';
import {trackMatchesFilter} from '../../core/track_manager';

const OVERVIEW_PANEL_FLAG = featureFlags.register({
  id: 'overviewVisible',
  name: 'Overview Panel',
  description: 'Show the panel providing an overview of the trace',
  defaultValue: true,
});

export class ViewerPage implements m.ClassComponent<PageWithTraceImplAttrs> {
  private readonly trash = new DisposableStack();
  private timelineBounds?: Rect2D;

  view({attrs}: m.CVnode<PageWithTraceImplAttrs>) {
    const {trace} = attrs;

    return m(
      '.pf-viewer-page.page',
      m(
        TabPanel,
        {trace},
        OVERVIEW_PANEL_FLAG.get() &&
          m(OverviewTimeline, {
            trace,
            className: 'pf-viewer-page__overview',
          }),
        m(TimelineHeader, {
          trace,
          className: 'pf-viewer-page__header',
          // There are three independent canvases on this page which we could
          // use keep track of the timeline width, but we use the header one
          // because it's always rendered.
          onTimelineBoundsChange: (rect) => (this.timelineBounds = rect),
        }),
        // Hide tracks while the trace is loading to prevent thrashing.
        !AppImpl.instance.isLoadingTrace && [
          // Don't render pinned tracks if we have none.
          trace.workspace.pinnedTracks.length > 0 &&
            m(TrackTreeView, {
              trace,
              className: 'pf-viewer-page__pinned-track-tree',
              rootNode: trace.workspace.pinnedTracksNode,
              canReorderNodes: true,
              scrollToNewTracks: true,
            }),
          m(TrackTreeView, {
            trace,
            className: 'pf-viewer-page__scrolling-track-tree',
            rootNode: trace.workspace.tracks,
            canReorderNodes: trace.workspace.userEditable,
            canRemoveNodes: trace.workspace.userEditable,
            trackFilter: (track) => trackMatchesFilter(trace, track),
          }),
        ],
      ),
    );
  }

  oncreate(vnode: m.VnodeDOM<PageWithTraceImplAttrs>) {
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
        timeline.panVisibleWindow(tDelta);
        raf.scheduleCanvasRedraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomRatio: number) => {
        if (!this.timelineBounds) return;
        const timeline = attrs.trace.timeline;
        const zoomPx = zoomedPositionPx - this.timelineBounds.left;
        const centerPoint = zoomPx / this.timelineBounds.width;
        timeline.zoomVisibleWindow(1 - zoomRatio, centerPoint);
        raf.scheduleCanvasRedraw();
      },
    });
    this.trash.use(panZoomHandler);
    this.onupdate(vnode);
  }

  onupdate({attrs}: m.VnodeDOM<PageWithTraceImplAttrs>) {
    // TODO(stevegolton): It's assumed that the TrackStacks will call into
    // trace.tracks.getTrackRenderer() in their view() functions which will mark
    // track renderers as used. We call flushOldTracks() here as it's guaranteed
    // to be called after view() on all child elements, and is only called once
    // per render cycle. However, this approach involves a bit too much magic.
    // The TODO is to sort this out and make it so the track flushing is
    // consolidated into one place.
    attrs.trace.tracks.flushOldTracks();
  }

  onremove() {
    this.trash.dispose();
  }
}
