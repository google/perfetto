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
import {z} from 'zod';
import {DisposableStack} from '../../base/disposable_stack';
import {toHTMLElement} from '../../base/dom_utils';
import {Rect2D} from '../../base/geom';
import {TimeScale} from '../../base/time_scale';
import {AppImpl} from '../../core/app_impl';
import {featureFlags} from '../../core/feature_flags';
import {settingsManager} from '../../core/settings_manager';
import {raf} from '../../core/raf_scheduler';
import {Minimap} from './minimap';
import {TabPanel} from './tab_panel';
import {TimelineHeader} from './timeline_header';
import {TrackTreeView} from './track_tree_view';
import {KeyboardNavigationHandler} from './wasd_navigation_handler';
import {trackMatchesFilter} from '../../core/track_manager';
import {TraceImpl} from '../../core/trace_impl';
import {HotkeyContext} from '../../widgets/hotkey_context';
import {ResizeHandle} from '../../widgets/resize_handle';
import {setTrackShellWidth, TRACK_SHELL_WIDTH} from '../css_constants';
import {TrackSearchBarApi, TrackSearchBar} from './track_search_bar';
import {
  searchTracks,
  TrackSearchMatch,
  TrackSearchModel,
} from '../../core/track_search_manager';
import {TrackNode} from '../../public/workspace';

const OVERVIEW_PANEL_FLAG = featureFlags.register({
  id: 'overviewVisible',
  name: 'Overview Panel',
  description: 'Show the panel providing an overview of the trace',
  defaultValue: true,
});

const VIRTUAL_TRACK_SCROLLING = settingsManager.register({
  id: 'virtualTrackScrolling',
  name: 'Virtual track scrolling',
  description: `Use virtual scrolling in the timeline view to improve performance on large traces.
    WARNING: Disabling this feature can severely degrade performance on large traces.`,
  defaultValue: true,
  schema: z.boolean(),
});

const USE_ALTERNATIVE_SEARCH_HOTKEY = settingsManager.register({
  id: 'alternativeSearchHotkey',
  name: 'Use Shift+F for track search',
  description:
    'Use Shift+F instead of Mod+F for track search, to avoid overriding browser find.',
  defaultValue: false,
  schema: z.boolean(),
});

const MIN_TRACK_SHELL_WIDTH = 100;
const MAX_TRACK_SHELL_WIDTH = 1000;

export function renderTimelinePage() {
  // Only render if a trace is loaded
  const trace = AppImpl.instance.trace;
  if (trace) {
    return m(TimelinePage, {trace});
  } else {
    return undefined;
  }
}

interface TimelinePageAttrs {
  readonly trace: TraceImpl;
}

class TimelinePage implements m.ClassComponent<TimelinePageAttrs> {
  private readonly trash = new DisposableStack();
  private timelineBounds?: Rect2D;
  private pinnedTracksHeight: number | 'auto' = 'auto';
  private trackSearchModel: TrackSearchModel = {
    searchTerm: '',
    useRegex: false,
    searchWithinCollapsedGroups: false,
  };
  private trackSearchBarVisible = false;
  private trackSearchBarApi?: TrackSearchBarApi;
  private trackSearchMatches: readonly TrackSearchMatch[] = [];
  private currentSearchMatchIndex = 0;

  view({attrs}: m.CVnode<TimelinePageAttrs>) {
    const {trace} = attrs;
    const virtualScrollingEnabled = VIRTUAL_TRACK_SCROLLING.get();
    const useAlternativeHotkey = USE_ALTERNATIVE_SEARCH_HOTKEY.get();

    return m(
      HotkeyContext,
      {
        hotkeys: virtualScrollingEnabled
          ? [
              {
                hotkey: useAlternativeHotkey ? 'Shift+F' : '!Mod+F',
                callback: () => {
                  this.trackSearchBarVisible = true;
                  this.trackSearchBarApi?.focus();
                },
              },
            ]
          : [],
        focusable: false, // Global hotkey, works without element focus
        fillHeight: true,
      },
      m(
        TabPanel,
        {trace},
        this.renderMinimap(trace),
        this.trackSearchBarVisible && this.renderTrackSearchPanel(trace),
        this.renderTimeline(trace),
      ),
    );
  }

  private renderTrackSearchPanel(trace: TraceImpl): m.Children {
    if (!this.trackSearchBarVisible) return null;
    const matchCount = this.trackSearchMatches.length;
    return m(TrackSearchBar, {
      model: this.trackSearchModel,
      matchCount,
      currentMatchIndex: this.currentSearchMatchIndex,
      onModelChange: (newModel) => {
        this.trackSearchModel = newModel;
        // Recompute matches and scroll to first result
        this.trackSearchMatches = searchTracks(
          trace.currentWorkspace,
          newModel,
        );
        this.currentSearchMatchIndex = 0;
        const firstMatch = this.trackSearchMatches[0];
        if (firstMatch) {
          firstMatch.node.reveal();
          trace.tracks.scrollToTrackNodeId = firstMatch.node.id;
        }
      },
      onClose: () => {
        this.trackSearchBarVisible = false;
        this.trackSearchModel = {
          ...this.trackSearchModel,
          searchTerm: '',
        };
      },
      onStepForward: () => {
        if (matchCount > 0) {
          this.currentSearchMatchIndex =
            (this.currentSearchMatchIndex + 1) % matchCount;
          const match = this.trackSearchMatches[this.currentSearchMatchIndex];
          if (match) {
            match.node.reveal();
            trace.tracks.scrollToTrackNodeId = match.node.id;
          }
        }
      },
      onStepBackwards: () => {
        if (matchCount > 0) {
          this.currentSearchMatchIndex =
            (this.currentSearchMatchIndex - 1 + matchCount) % matchCount;
          const match = this.trackSearchMatches[this.currentSearchMatchIndex];
          if (match) {
            match.node.reveal();
            trace.tracks.scrollToTrackNodeId = match.node.id;
          }
        }
      },
      onReady: (api) => (this.trackSearchBarApi = api),
    });
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
    if (!OVERVIEW_PANEL_FLAG.get()) return null;
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

    // Get the current search match node
    const currentSearchMatch =
      this.trackSearchMatches[this.currentSearchMatchIndex]?.node;

    return [
      this.renderPinnedTracks(trace, currentSearchMatch),
      this.renderMainTracks(trace, currentSearchMatch),
    ];
  }

  private renderPinnedTracks(
    trace: TraceImpl,
    currentSearchMatch: TrackNode | undefined,
  ): m.Children {
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
          trackSearchMatches: this.trackSearchMatches,
          currentSearchMatch,
          virtualScrollingEnabled: VIRTUAL_TRACK_SCROLLING.get(),
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

  private renderMainTracks(
    trace: TraceImpl,
    currentSearchMatch: TrackNode | undefined,
  ): m.Children {
    return m(TrackTreeView, {
      trace,
      className: 'pf-timeline-page__scrolling-track-tree',
      rootNode: trace.currentWorkspace.tracks,
      canReorderNodes: trace.currentWorkspace.userEditable,
      canRemoveNodes: trace.currentWorkspace.userEditable,
      trackFilter: (track) => trackMatchesFilter(trace, track),
      trackSearchMatches: this.trackSearchMatches,
      currentSearchMatch,
      virtualScrollingEnabled: VIRTUAL_TRACK_SCROLLING.get(),
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
