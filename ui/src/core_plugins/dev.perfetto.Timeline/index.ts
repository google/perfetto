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
import {findRef, toHTMLElement} from '../../base/dom_utils';
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
import {HotkeyContext} from '../../widgets/hotkey_context';
import {TrackSearchBarApi, TrackSearchBar} from './track_search_bar';
import {
  searchTracks,
  TrackSearchMatch,
  TrackSearchModel,
} from '../../core/track_search_manager';
import {TrackNode} from '../../public/workspace';
import {maybeUndefined} from '../../base/utils';
import {GateDetector} from '../../base/mithril_utils';
import {assertIsInstance} from '../../base/assert';
import {Flag} from '../../public/feature_flag';
import {PerfettoPlugin} from '../../public/plugin';
import {Setting} from '../../public/settings';

const MIN_TRACK_SHELL_WIDTH = 100;
const MAX_TRACK_SHELL_WIDTH = 1000;
const HOTKEY_CONTEXT_REF = 'context';

export default class TimelinePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Timeline';
  static readonly description = 'The main timeline view';
  private static minimapFlag: Flag;
  private static virtualTrackScrolling: Setting<boolean>;
  private static useAlternativeSearchHotkey: Setting<boolean>;

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

    TimelinePlugin.virtualTrackScrolling = app.settings.register({
      id: 'virtualTrackScrolling',
      name: 'Virtual track scrolling',
      description: `Use virtual scrolling in the timeline view to improve performance on large traces.
        WARNING: Disabling this feature can severely degrade performance on large traces.`,
      defaultValue: true,
      schema: z.boolean(),
    });

    TimelinePlugin.useAlternativeSearchHotkey = app.settings.register({
      id: 'alternativeSearchHotkey',
      name: 'Use Shift+F for track search',
      description:
        'Use Shift+F instead of Mod+F for track search, to avoid overriding browser find.',
      defaultValue: false,
      schema: z.boolean(),
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
          virtualScrollingEnabled: TimelinePlugin.virtualTrackScrolling.get(),
          useAlternativeSearchHotkey:
            TimelinePlugin.useAlternativeSearchHotkey.get(),
        });
      },
    });
  }
}

interface TimelinePageAttrs {
  readonly trace: TraceImpl;
  readonly showMinimap: boolean;
  readonly virtualScrollingEnabled: boolean;
  readonly useAlternativeSearchHotkey: boolean;
}

class TimelinePage implements m.ClassComponent<TimelinePageAttrs> {
  private readonly trash = new DisposableStack();
  private timelineBounds?: Rect2D;
  private pinnedTracksHeight: number | 'auto' = 'auto';
  private trackSearchModel: TrackSearchModel = {
    searchTerm: '',
    useRegex: false,
    searchWithinCollapsedGroups: true,
  };
  private trackSearchBarVisible = false;
  private trackSearchBarApi?: TrackSearchBarApi;
  private trackSearchMatches: readonly TrackSearchMatch[] = [];
  private currentSearchMatchIndex = 0;

  view({attrs}: m.CVnode<TimelinePageAttrs>) {
    const {trace, virtualScrollingEnabled, useAlternativeSearchHotkey} = attrs;
    const searchHotkey = useAlternativeSearchHotkey ? 'Shift+F' : '!Mod+F';

    return m(
      TabPanel,
      {trace},
      m(
        GateDetector,
        {
          onVisibilityChanged: (visible: boolean, dom: Element) => {
            if (visible) {
              // Focus the search input as soon as it becomes visible.
              const hotkeyContextEl = findRef(dom, HOTKEY_CONTEXT_REF);
              assertIsInstance(hotkeyContextEl, HTMLElement);
              hotkeyContextEl.focus();
            }
          },
        },
        m(
          HotkeyContext,
          {
            ref: HOTKEY_CONTEXT_REF,
            hotkeys: virtualScrollingEnabled
              ? [
                  {
                    hotkey: searchHotkey,
                    callback: () => {
                      this.trackSearchBarVisible = true;
                      this.trackSearchBarApi?.focus();
                    },
                  },
                ]
              : [],
            focusable: true,
            fillHeight: true,
            showFocusRing: true,
          },
          attrs.showMinimap && this.renderMinimap(trace),
          this.trackSearchBarVisible && this.renderTrackSearchPanel(trace),
          this.renderTimeline(trace, virtualScrollingEnabled),
        ),
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
          (track) => trackMatchesFilter(trace, track),
        );
        this.currentSearchMatchIndex = 0;
        const firstMatch = maybeUndefined(this.trackSearchMatches[0]);
        if (firstMatch) {
          firstMatch.node.reveal();
          trace.tracks.scrollToTrackNodeId = firstMatch.node.id;
        }
      },
      onClose: (target) => {
        this.trackSearchBarVisible = false;
        this.trackSearchModel = {
          ...this.trackSearchModel,
          searchTerm: '',
        };
        this.trackSearchMatches = [];
        if (target instanceof Element) {
          const hotkeyContextElem = target.closest(
            '.pf-hotkey-context[ref="' + HOTKEY_CONTEXT_REF + '"]',
          );
          assertIsInstance(hotkeyContextElem, HTMLElement);
          hotkeyContextElem.focus();
        }
      },
      onStepForward: () => {
        // Recalculate matches to pick up any state changes (e.g., expanded groups)
        this.trackSearchMatches = searchTracks(
          trace.currentWorkspace,
          this.trackSearchModel,
          (track) => trackMatchesFilter(trace, track),
        );
        const count = this.trackSearchMatches.length;
        if (count > 0) {
          this.currentSearchMatchIndex =
            (this.currentSearchMatchIndex + 1) % count;
          const match = maybeUndefined(
            this.trackSearchMatches[this.currentSearchMatchIndex],
          );
          if (match) {
            match.node.reveal();
            trace.tracks.scrollToTrackNodeId = match.node.id;
          }
        }
      },
      onStepBackwards: () => {
        // Recalculate matches to pick up any state changes (e.g., expanded groups)
        this.trackSearchMatches = searchTracks(
          trace.currentWorkspace,
          this.trackSearchModel,
          (track) => trackMatchesFilter(trace, track),
        );
        const count = this.trackSearchMatches.length;
        if (count > 0) {
          this.currentSearchMatchIndex =
            (this.currentSearchMatchIndex - 1 + count) % count;
          const match = maybeUndefined(
            this.trackSearchMatches[this.currentSearchMatchIndex],
          );
          if (match) {
            match.node.reveal();
            trace.tracks.scrollToTrackNodeId = match.node.id;
          }
        }
      },
      onReady: (api) => (this.trackSearchBarApi = api),
    });
  }

  private renderTimeline(
    trace: TraceImpl,
    virtualScrollingEnabled: boolean,
  ): m.Children {
    return m(
      '.pf-timeline-page__timeline',
      this.renderHeader(trace),
      this.renderTracks(trace, virtualScrollingEnabled),
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

  private renderTracks(
    trace: TraceImpl,
    virtualScrollingEnabled: boolean,
  ): m.Children {
    // Hide tracks while the trace is loading to prevent thrashing.
    if (AppImpl.instance.isLoadingTrace) return null;

    // Get the current search match node
    const currentSearchMatch =
      this.trackSearchMatches[this.currentSearchMatchIndex]?.node;

    return [
      this.renderPinnedTracks(
        trace,
        currentSearchMatch,
        virtualScrollingEnabled,
      ),
      this.renderMainTracks(trace, currentSearchMatch, virtualScrollingEnabled),
    ];
  }

  private renderPinnedTracks(
    trace: TraceImpl,
    currentSearchMatch: TrackNode | undefined,
    virtualScrollingEnabled: boolean,
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
          virtualScrollingEnabled,
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
    virtualScrollingEnabled: boolean,
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
      virtualScrollingEnabled,
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
