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

import {AsyncLimiter} from '../../base/async_limiter';
import {assertUnreachable} from '../../base/assert';
import {defer} from '../../base/deferred';
import {DisposableStack} from '../../base/disposable_stack';
import {elementIsEditable} from '../../base/dom_utils';
import {download} from '../../base/download_utils';
import {getErrorMessage} from '../../base/errors';
import type {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Time, type time} from '../../base/time';
import {KeyMapping} from '../../base/wasd_key_mapping';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {type AreaSelection, areaSelectionsEqual} from '../../public/selection';
import {TimestampFormat} from '../../public/timeline';
import type {Trace} from '../../public/trace';
import type {TrackRenderer} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {
  LIVE_COUNTER_TRACK_KIND,
  LIVE_SLICE_TRACK_KIND,
  LiveCounterAggregator,
  LiveSliceAggregator,
} from './aggregators';
import {fetchDownload, fetchStream, fetchTargets} from './api';
import type {LiveHost, LiveSessionModel} from './controller';
import {LiveSliceDetailsPanel, type SliceTrackInfo} from './details_panel';
import {BusyTime, LiveMinimapProvider} from './minimap';
import {type LiveTraceArgs, PLUGIN_ID} from './opener';
import {followWindow, isSameWidth, PlayheadClock} from './playhead';
import {Backoff, sleep} from './retry';
import type {LiveSettings} from './settings';
import {computeStatus, type LiveStatus} from './status';
import {
  BrokenStoreError,
  COUNTER_TABLE,
  LiveStore,
  overlappingSlices,
  type SeedTrack,
  type Span,
  type StoreChanges,
  type TrackState,
} from './store';
import type {WallClock} from './wall_clock';
import type {StreamResponse, Target, Track} from './wire';

// History requested in full responses, e.g. when the seed holds none.
const WINDOW_SEC = 120;

const INITIAL_FOLLOW_WIDTH_NS = 10e9;

// Width changes this soon after zoom input are zooms. The timeline's W/S
// animation ends at most 700 ms after the last keydown; Ctrl+wheel zooms at
// once.
const ZOOM_INPUT_MS = 1_000;

// The trace end grows in steps, as each extension redraws the whole UI.
const TRACE_END_STEP_NS = 1_000_000_000n;

// The minimap follows the growing trace this often, as each refresh rebuilds
// all its rows. Refreshes skip when nothing changed.
const MINIMAP_REFRESH_MS = 5_000;

const PRUNE_INTERVAL_MS = 5_000;

// Tracks rebuild for a window this many times narrower than their rows'
// range, e.g. after zooming in, see updateLoadedRange().
const MAX_RANGE_WINDOWS = 6;

// Each move of the loaded range rebuilds all tracks: at most this often.
const RANGE_INTERVAL_MS = 250;

// Changes between publishes are merged beyond this many.
const MAX_PENDING_SPANS = 64;

// Tracks show changes at most this often: each change rebuilds a track's
// tables. Followed rows land the buffer delay (default 5 s) right of the
// window, so they still show before they scroll into view. With a 0 s delay,
// they show up to 0.5 s late.
const PUBLISH_INTERVAL_MS = 500;

// Time-based statuses, e.g. buffering without new data, are checked this
// often.
const TICK_MS = 1_000;

const NS_PER_SEC = 1_000_000_000n;
const NS_PER_MIN = 60n * NS_PER_SEC;

// Columns of the slice tracks' datasets; implements SLICE_SCHEMA of the
// aggregators too.
const SLICE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  depth: NUM,
  name: STR,
  category: STR,
  track_id: NUM,
};

/**
 * The live view of one trace: copies the seed's history into the store, then
 * polls the server for changes, shows the tracks and keeps the timeline at
 * the playhead.
 *
 *   seed ────importSeed───┐
 *   fetchStream ──ingest──┴─► store ──► tracks: rows near the visible window,
 *        │                              reloaded when they change
 *        └──► PlayheadClock ──► per frame: visible window, trace end
 *
 * The HUD shows it through a LiveSessionModel, see LiveHost.
 */
export class LiveSession implements Disposable {
  private readonly trash = new DisposableStack();
  // Aborted on dispose.
  private readonly lifetime = new AbortController();
  private readonly target: Target;
  // Whether the previous seed's clock didn't match the stream.
  private readonly epochRetry: boolean;
  private readonly store: LiveStore;
  private readonly busy = new BusyTime();
  private readonly minimap: LiveMinimapProvider;
  private readonly settings: LiveSettings;
  private readonly clock = new PlayheadClock();
  private readonly targetsLoader = new AsyncLimiter();
  private readonly nodes = new Map<string, TrackNode>();
  // Table ids of the tracks by URI, for the area selection tabs.
  private readonly sqlIds = new Map<string, number>();

  // Store generations of the tracks' rows, as published by publishGens().
  private readonly publishedGens = new Map<string, number>();
  // Bumped when a published change reaches a track's loaded range, which
  // reloads its rows.
  private readonly loadedGens = new Map<string, number>();
  // Spans of the rows changed since the last publish, by track.
  private readonly pendingSpans = new Map<string, Span[]>();
  // When publish() last published a change.
  private lastPublishMs = -Infinity;

  // The rows that the tracks load: see updateLoadedRange().
  private loadedRange?: Span;
  private lastRangeMs = -Infinity;
  private rangeTimer?: ReturnType<typeof setTimeout>;

  // Whether refreshMinimap() runs.
  private minimapRefreshing = false;

  // Position in the server stream.
  private streamId: string;
  private seq: bigint;
  private playhead = Time.ZERO;
  private lastDataMs?: number;
  private lastPruneMs = 0;
  // The server's wall clock, from the latest data.
  private wallClock?: WallClock;

  // Errors, see error(). A request error lasts until a request succeeds; a
  // stop error until the trace is reopened; a notice until dismissed.
  private requestError?: string;
  private serverError = '';
  private stopError?: string;
  private notice?: string;
  // Whether the latest response had no data yet: buffering, unless the server
  // reports an error.
  private waiting = false;

  private paused = false;
  private resumed = defer<void>();
  // Aborts the current request or wait on pause and dispose.
  private request?: AbortController;

  private followWidthNs = INITIAL_FOLLOW_WIDTH_NS;
  // The window and playhead of the latest follow, to notice when the user pans
  // or zooms, and when the playhead stands still.
  private followedWindow?: HighPrecisionTimeSpan;
  private followedDisplay?: time;
  // The latest area selection, to notice new ones.
  private area?: AreaSelection;
  // Whether a pointer button is down, e.g. while dragging an area.
  private pointerDown = false;
  // When the user last zoomed with W/S or Ctrl+wheel.
  private zoomInputMs = -Infinity;

  constructor(
    private readonly trace: Trace,
    args: LiveTraceArgs,
    private readonly host: LiveHost,
  ) {
    this.target = args.target;
    this.epochRetry = args.epochRetry;
    this.streamId = args.streamId;
    this.seq = BigInt(args.seedSeq);
    this.store = new LiveStore(trace.engine, BigInt(WINDOW_SEC) * NS_PER_SEC);
    this.minimap = new LiveMinimapProvider(trace.engine, this.busy);
    this.settings = host.settings;
  }

  // Creates the tables and shows the session in the HUD, and copies the
  // seed's history, so that the trace opens with it. Streaming starts once
  // the trace is ready.
  async init(): Promise<void> {
    await this.store.createTables();
    this.registerAreaSelectionTabs();
    this.trace.minimap.registerContentProvider(this.minimap);

    // The core loads the minimap after onTraceLoad, but not for an empty
    // trace span, which would leave the minimap without our rows for good.
    const {start, end} = this.trace.traceInfo;
    if (end <= start) {
      growTraceEnd(this.trace, Time.fromRaw(start + TRACE_END_STEP_NS));
    }

    this.useTraceTimeZone();
    this.trash.use(this.host.attach(this.model()));

    const raf = this.trace.raf;
    this.trash.use(raf.addCanvasRedrawCallback(() => this.onFrame()));
    const timer = setInterval(() => {
      // Publishes changes held back by apply(), e.g. right before a pause.
      this.publish();
      this.refreshStatus();
    }, TICK_MS);
    const minimapTimer = setInterval(
      () => void this.refreshMinimap(),
      MINIMAP_REFRESH_MS,
    );
    this.trash.defer(() => {
      clearInterval(timer);
      clearInterval(minimapTimer);
      clearTimeout(this.rangeTimer);
    });
    this.watchPointer();
    this.watchZoomInput();

    const seedTracks = await this.importSeed();

    this.trash.use(
      this.trace.onTraceReady.addListener(() => {
        this.removeSeedTracks();
        this.showTracks(seedTracks);
        if (seedTracks.length > 0) this.showHistoryEnd();
        // Not awaited: trace loading waits for the listeners.
        void this.stream();
        this.refreshTargets();
      }),
    );
  }

  [Symbol.dispose](): void {
    this.lifetime.abort();
    this.request?.abort();
    this.resumed.resolve();
    this.trash.dispose();
  }

  // Copies the seed's history into the store and returns its tracks. On
  // failure, streaming starts with a full response instead, which holds the
  // latest WINDOW_SEC.
  private async importSeed(): Promise<SeedTrack[]> {
    // Seeds without a stream position hold no history.
    if (this.seq === 0n) return [];

    try {
      // Rows that would expire at once stay out.
      const minEnd = this.trace.traceInfo.end - this.retentionNs();
      const tracks = await this.store.importSeed(minEnd);
      await this.busy.load(this.trace.engine);
      return tracks;
    } catch (e) {
      this.streamId = '';
      this.seq = 0n;
      this.notice = `Cannot load the history: ${getErrorMessage(e)}`;
      this.refreshStatus();
      return [];
    }
  }

  // Shows the end of the history, where following starts, until the first
  // response: the tracks then load only the rows near it.
  private showHistoryEnd() {
    const delayNs = BigInt(this.settings.bufferDelay.get()) * NS_PER_SEC;
    const {start, end} = this.trace.traceInfo;
    this.trace.timeline.setVisibleWindow(
      followWindow(Time.sub(end, delayNs), this.followWidthNs, start),
    );
  }

  // Polls the server until disposed, or until the stream restarts.
  private async stream(): Promise<void> {
    const backoff = new Backoff();
    while (!this.lifetime.signal.aborted) {
      if (this.paused) {
        await this.resumed;
        continue;
      }

      const request = new AbortController();
      this.request = request;
      const startMs = performance.now();
      try {
        const resp = await fetchStream(
          {
            target: this.target.id,
            streamId: this.streamId,
            sinceSeq: this.seq,
            windowSec: WINDOW_SEC,
          },
          request.signal,
        );
        if (this.isOtherStream(resp)) {
          this.onOtherStream();
          return;
        }
        const previousSeq = this.seq;
        await this.apply(resp, performance.now());
        this.requestError = undefined;
        this.refreshStatus();
        backoff.reset();

        // The server holds more changes than fit in one response.
        if (resp.more && this.seq > previousSeq) continue;

        // Requests start once per interval, unless long-polls take longer.
        const intervalMs = Number(this.settings.updateInterval.get()) * 1000;
        await sleep(intervalMs - (performance.now() - startMs), request.signal);
      } catch (e) {
        // Aborted by pause or dispose.
        if (request.signal.aborted) continue;
        this.requestError = getErrorMessage(e);
        this.refreshStatus();
        // The tables are in an unknown state: only reconnecting recovers.
        if (e instanceof BrokenStoreError) return;
        await sleep(backoff.next(), request.signal);
      } finally {
        this.request = undefined;
      }
    }
  }

  // Whether `resp` comes from another stream than this trace's seed, e.g.
  // after a server restart. Its timestamps don't fit this trace.
  private isOtherStream(resp: StreamResponse): boolean {
    if (resp.seq === 0n) return false;
    if (this.streamId !== '') return resp.stream_id !== this.streamId;
    return resp.start_epoch_ns !== -this.trace.traceInfo.unixOffset;
  }

  // Stops streaming and reopens the trace with the seed of the new stream,
  // unless a switch is pending. If the seed's clock doesn't match the first
  // data twice in a row, e.g. as the seed has no REALTIME clock snapshot,
  // reopening again won't help.
  private onOtherStream() {
    const firstData = this.streamId === '';
    if (firstData && this.epochRetry) {
      this.stopError = "The seed's clock doesn't match the stream";
    } else {
      this.stopError = 'The live stream restarted';
      this.host.reopen(this.target.id, firstData);
    }
    this.refreshStatus();
  }

  private async apply(resp: StreamResponse, receivedMs: number) {
    this.serverError = resp.last_error;

    // No data yet.
    this.waiting = resp.seq === 0n;
    if (this.waiting) return;

    await this.collect(await this.store.ingest(resp));
    this.streamId = resp.stream_id;
    this.seq = resp.seq;
    // The time zone may change, e.g. for daylight saving time.
    this.wallClock = {
      startEpochNs: resp.start_epoch_ns,
      tzOffsetMin: resp.tz_offset_min,
      tzAbbr: resp.tz_abbr,
    };
    this.showTracks(resp.tracks);

    if (resp.playhead_ns > this.playhead) {
      this.playhead = Time.fromRaw(resp.playhead_ns);
      this.lastDataMs = receivedMs;
      this.clock.update(resp.playhead_ns, receivedMs);
      this.extendTraceEnd(this.playhead);
    }

    const nowMs = performance.now();
    if (nowMs - this.lastPruneMs >= PRUNE_INTERVAL_MS) {
      this.lastPruneMs = nowMs;
      const retentionNs = this.retentionNs();
      await this.collect(await this.store.prune(this.playhead, retentionNs));
      this.busy.prune(this.playhead - retentionNs);
    }

    // New data moves a playhead that stood still, see onFrame().
    this.trace.raf.scheduleCanvasRedraw();
    this.publish();
  }

  // Publishes the changes, at most every PUBLISH_INTERVAL_MS. Tracks redraw
  // from the canvas. The DOM shows the details of a selected slice, which
  // may have changed.
  private publish() {
    const nowMs = performance.now();
    if (nowMs - this.lastPublishMs < PUBLISH_INTERVAL_MS) return;
    if (!this.publishGens()) return;
    this.lastPublishMs = nowMs;
    this.trace.raf.scheduleCanvasRedraw();
    if (this.trace.selection.selection.kind === 'track_event') {
      this.trace.raf.scheduleFullRedraw();
    }
  }

  // Records the rows that a write changed, for publishGens() and the
  // minimap.
  private async collect(changes: StoreChanges) {
    for (const [id, span] of changes.spans) {
      const spans = this.pendingSpans.get(id) ?? [];
      spans.push(span);
      this.pendingSpans.set(
        id,
        spans.length > MAX_PENDING_SPANS ? [coverSpans(spans)] : spans,
      );
    }

    // The seed's rows were replaced: all slices may be new.
    if (changes.completed === undefined) {
      await this.busy.load(this.trace.engine);
      return;
    }
    for (const s of changes.completed) this.busy.add(s.trackId, s.ts, s.dur);
  }

  // Lets tracks show their latest rows. Details panels reload with the
  // published generation of their track, and tracks with changes in their
  // loaded range reload it. Returns whether any track changed.
  private publishGens(): boolean {
    const range = this.loadedRange;
    let changed = false;
    for (const id of this.nodes.keys()) {
      const gen = this.store.track(id)?.gen ?? 0;
      if (this.publishedGens.get(id) === gen) continue;
      this.publishedGens.set(id, gen);
      changed = true;
      const spans = this.pendingSpans.get(id) ?? [];
      if (range === undefined || spans.some((s) => overlaps(s, range))) {
        this.loadedGens.set(id, (this.loadedGens.get(id) ?? 0) + 1);
      }
    }
    this.pendingSpans.clear();
    return changed;
  }

  // Tracks load the rows near the visible window only, so that their cost
  // follows the window, not the history:
  //
  //   loaded range  |<- width ->|<- visible window ->|<- width ->|
  //
  // The range moves when the window leaves it, or is much narrower, at most
  // every RANGE_INTERVAL_MS.
  private updateLoadedRange() {
    const visible = this.trace.timeline.visibleWindow.toTimeSpan();
    const range = this.loadedRange;
    if (
      range !== undefined &&
      range.start <= visible.start &&
      range.end >= visible.end &&
      range.end - range.start <= BigInt(MAX_RANGE_WINDOWS) * visible.duration
    ) {
      return;
    }

    // A pending move ends with a redraw, which runs this again.
    if (this.rangeTimer !== undefined) return;
    const waitMs = this.lastRangeMs + RANGE_INTERVAL_MS - performance.now();
    if (waitMs > 0) {
      this.rangeTimer = setTimeout(() => {
        this.rangeTimer = undefined;
        this.trace.raf.scheduleCanvasRedraw();
      }, waitMs);
      return;
    }

    this.loadedRange = rangeAround(visible.start, visible.end);
    this.lastRangeMs = performance.now();
    this.trace.raf.scheduleCanvasRedraw();
  }

  // The loaded range, initially around the visible window.
  private range(): Span {
    if (this.loadedRange === undefined) {
      const visible = this.trace.timeline.visibleWindow.toTimeSpan();
      this.loadedRange = rangeAround(visible.start, visible.end);
    }
    return this.loadedRange;
  }

  // Adds new tracks in server order, renames and reorders the others.
  private showTracks(
    tracks: ReadonlyArray<Pick<Track, 'id' | 'name' | 'unit'>>,
  ) {
    const workspace = this.trace.defaultWorkspace;
    let changed = false;
    let previous: TrackNode | undefined;
    for (const track of tracks) {
      // Tracks of unknown kinds have no state.
      const state = this.store.track(track.id);
      if (state === undefined) continue;

      let node = this.nodes.get(track.id);
      if (node === undefined) {
        node = this.addTrack(track, state);
        changed = true;
      } else if (node.name !== track.name) {
        node.name = track.name;
        changed = true;
      }

      // Each track follows its predecessor.
      const children = workspace.children;
      const index = previous === undefined ? 0 : children.indexOf(previous) + 1;
      if (children[index] !== node) {
        if (previous === undefined) {
          workspace.addChildFirst(node);
        } else {
          workspace.addChildAfter(node, previous);
        }
        changed = true;
      }
      previous = node;
    }
    if (changed) this.trace.raf.scheduleFullRedraw();
  }

  private addTrack(
    track: Pick<Track, 'id' | 'name' | 'unit'>,
    state: TrackState,
  ): TrackNode {
    const uri = `${PLUGIN_ID}#${encodeURIComponent(track.id)}`;
    const renderer = this.createRenderer(uri, track, state);
    const kind =
      state.kind === 'KIND_SLICE'
        ? LIVE_SLICE_TRACK_KIND
        : LIVE_COUNTER_TRACK_KIND;
    this.trace.tracks.registerTrack({uri, renderer, tags: {kinds: [kind]}});
    this.sqlIds.set(uri, state.sqlId);

    const node = new TrackNode({uri, name: track.name});
    this.nodes.set(track.id, node);
    // The first load sees all current rows.
    this.publishedGens.set(track.id, state.gen);
    return node;
  }

  private createRenderer(
    uri: string,
    track: Pick<Track, 'id' | 'unit'>,
    state: TrackState,
  ): TrackRenderer {
    const trace = this.trace;
    const id = state.sqlId;

    // Ends the track's query. The query, and so the track's data, changes
    // with the loaded range and the track's loaded generation.
    const gen = () => `/* gen ${this.loadedGens.get(track.id) ?? 0} */`;

    switch (state.kind) {
      case 'KIND_SLICE':
        // Colored by name, the default.
        return SliceTrack.create({
          trace,
          uri,
          dataset: () => {
            const {start, end} = this.range();
            const columns = Object.keys(SLICE_SCHEMA).join(', ');
            const rows = overlappingSlices(
              columns,
              `track_id = ${id}`,
              start,
              end,
            );
            return new SourceDataset({
              src: `${rows} ${gen()}`,
              schema: SLICE_SCHEMA,
            });
          },
          detailsPanel: (row) =>
            new LiveSliceDetailsPanel(
              trace.engine,
              row.id,
              this.trackInfo(track.id),
            ),
        });
      case 'KIND_COUNTER':
        return CounterTrack.create({
          trace,
          uri,
          // The point before the range holds the value at its start. The
          // counter mipmap needs rows sorted by ts.
          sqlSource: () => {
            const {start, end} = this.range();
            return `
              SELECT ts, value FROM (
                SELECT ts, value FROM ${COUNTER_TABLE}
                WHERE track_id = ${id} AND ts < ${start}
                ORDER BY ts DESC
                LIMIT 1
              )
              UNION ALL
              SELECT ts, value FROM ${COUNTER_TABLE}
              WHERE track_id = ${id} AND ts BETWEEN ${start} AND ${end}
              ORDER BY ts
              ${gen()}
            `;
          },
          unit: track.unit,
          // Retained history can dwarf the visible values.
          yRange: 'viewport',
          yDisplay: 'minmax',
          chartHeightSize: 2,
        });
      default:
        assertUnreachable(state.kind);
    }
  }

  private trackInfo(trackId: string): SliceTrackInfo {
    return {
      gen: () => this.publishedGens.get(trackId) ?? 0,
      clock: () => this.wallClock,
      playhead: () => this.playhead,
    };
  }

  // Area selection tabs over the live tracks, e.g. slices by name.
  private registerAreaSelectionTabs() {
    const trace = this.trace;
    const sqlId = (uri: string) => this.sqlIds.get(uri);
    const slices = new LiveSliceAggregator(trace, sqlId, () => this.playhead);
    const counters = new LiveCounterAggregator(sqlId);
    trace.selection.registerAreaSelectionTab(
      createAggregationTab(trace, slices),
    );
    trace.selection.registerAreaSelectionTab(
      createAggregationTab(trace, counters),
    );
  }

  // Follows the playhead with the visible window, once per frame.
  private onFrame() {
    this.checkAreaSelection();
    this.updateLoadedRange();
    if (this.host.following) this.checkWindowChange();
    if (!this.isFollowing()) return;
    const display = this.displayedPlayhead();
    if (display === undefined) return;

    // The timeline holds still under a pressed pointer, so that an area drag
    // ends where it was aimed. The new area then stops following.
    if (this.pointerDown) return;

    // A playhead that stands still, e.g. waiting for data, needs no next
    // frame: new data schedules one.
    const followed = this.followedWindow;
    if (followed !== undefined && display === this.followedDisplay) return;

    this.extendTraceEnd(display);
    const timeline = this.trace.timeline;
    const traceStart = this.trace.traceInfo.start;
    timeline.setVisibleWindow(
      followWindow(display, this.followWidthNs, traceStart),
    );
    this.followedWindow = timeline.visibleWindow;
    this.followedDisplay = display;
    this.trace.raf.scheduleCanvasRedraw();
  }

  // Other changes of the followed window come from the user. A zoom keeps
  // following at the new width: another width right after zoom input (W/S,
  // Ctrl+wheel). Anything else stops following, e.g. a pan, or F on a slice.
  // Checked while paused too: Resume keeps a zoom.
  private checkWindowChange() {
    const followed = this.followedWindow;
    const current = this.trace.timeline.visibleWindow;
    if (followed === undefined || current.equals(followed)) return;
    const zoomed =
      !isSameWidth(current.duration, followed.duration) &&
      performance.now() - this.zoomInputMs <= ZOOM_INPUT_MS;
    if (!zoomed) {
      this.setFollowing(false);
      return;
    }
    this.followWidthNs = current.duration;
    this.followedWindow = current;
    // Re-anchors at the playhead on the next follow, even if it stands still.
    this.followedDisplay = undefined;
  }

  // Advances the playhead and returns the displayed one: the playhead minus
  // the buffer delay. Undefined before the first data.
  private displayedPlayhead(): time | undefined {
    const playhead = this.clock.tick(frameTimeMs());
    if (playhead === undefined) return undefined;
    const delayNs = BigInt(this.settings.bufferDelay.get()) * NS_PER_SEC;
    return Time.fromRaw(playhead - delayNs);
  }

  // Stops following when the user selects another area, which following
  // would scroll away. Following again keeps the area.
  private checkAreaSelection() {
    const selection = this.trace.selection.selection;
    const area = selection.kind === 'area' ? selection : undefined;
    const previous = this.area;
    this.area = area;
    if (area === undefined || !this.host.following) return;
    if (previous === undefined || !areaSelectionsEqual(area, previous)) {
      this.setFollowing(false);
    }
  }

  // Tracks whether a pointer button is down anywhere on the page.
  private watchPointer() {
    const down = () => {
      this.pointerDown = true;
    };
    const up = () => {
      if (!this.pointerDown) return;
      this.pointerDown = false;
      this.trace.raf.scheduleCanvasRedraw();
    };
    const options = {capture: true, passive: true};
    window.addEventListener('pointerdown', down, options);
    window.addEventListener('pointerup', up, options);
    window.addEventListener('pointercancel', up, options);
    // A release outside the page may go unseen.
    window.addEventListener('blur', up);
    this.trash.defer(() => {
      window.removeEventListener('pointerdown', down, options);
      window.removeEventListener('pointerup', up, options);
      window.removeEventListener('pointercancel', up, options);
      window.removeEventListener('blur', up);
    });
  }

  // Notes zoom input as the timeline reads it: W/S without Ctrl/Meta outside
  // text fields, and Ctrl+wheel, which is also how Chrome reports a pinch.
  private watchZoomInput() {
    const keydown = (e: KeyboardEvent) => {
      const zoomKey =
        e.code === KeyMapping.KEY_ZOOM_IN || e.code === KeyMapping.KEY_ZOOM_OUT;
      if (!zoomKey || e.ctrlKey || e.metaKey) return;
      if (elementIsEditable(e.target)) return;
      this.zoomInputMs = performance.now();
    };
    const wheel = (e: WheelEvent) => {
      if (e.ctrlKey) this.zoomInputMs = performance.now();
    };
    const options = {capture: true, passive: true};
    window.addEventListener('keydown', keydown, options);
    window.addEventListener('wheel', wheel, options);
    this.trash.defer(() => {
      window.removeEventListener('keydown', keydown, options);
      window.removeEventListener('wheel', wheel, options);
    });
  }

  private extendTraceEnd(ts: time) {
    if (ts <= this.trace.traceInfo.end) return;
    growTraceEnd(this.trace, Time.fromRaw(ts + TRACE_END_STEP_NS));
  }

  // Updates the minimap for the trace bounds, which the core loads once. On
  // failure, the old rows stay until the next refresh.
  private async refreshMinimap() {
    if (this.minimapRefreshing) return;
    this.minimapRefreshing = true;
    try {
      const {start, end} = this.trace.traceInfo;
      if (await this.minimap.refresh(start, end)) {
        this.trace.raf.scheduleCanvasRedraw();
      }
    } catch (e) {
      if (this.lifetime.signal.aborted) return;
      this.showMessage(`Cannot update the minimap: ${getErrorMessage(e)}`);
    } finally {
      this.minimapRefreshing = false;
    }
  }

  private setFollowing(following: boolean) {
    this.host.following = following;
    this.followedWindow = undefined;
    if (following) {
      // Keep the user's zoom level.
      this.followWidthNs = this.trace.timeline.visibleWindow.duration;
    }
    this.trace.raf.scheduleFullRedraw();
  }

  // Stops or restarts updates, without changing whether to follow.
  private togglePause() {
    this.paused = !this.paused;
    if (this.paused) {
      this.resumed = defer<void>();
      this.request?.abort();
    } else {
      this.resumed.resolve();
    }
    this.refreshStatus();
    this.trace.raf.scheduleFullRedraw();
  }

  // Stops following, or follows and resumes updates. Stopping keeps a pause.
  private toggleFollow() {
    if (this.host.following) {
      this.setFollowing(false);
      return;
    }
    if (this.paused) this.togglePause();
    this.setFollowing(true);
  }

  private isFollowing(): boolean {
    return this.host.following && !this.paused;
  }

  // Loads the server's targets for the target menu.
  private refreshTargets() {
    void this.targetsLoader.schedule(async () => {
      try {
        // With this target, so that the server doesn't start streaming its
        // default one.
        const resp = await fetchTargets(this.target.id, this.lifetime.signal);
        this.host.setTargets(resp.targets);
      } catch (e) {
        if (this.lifetime.signal.aborted) return;
        this.showMessage(`Cannot load the targets: ${getErrorMessage(e)}`);
      }
    });
  }

  private async download() {
    this.showMessage('Downloading the trace…');
    try {
      const blob = await fetchDownload(this.target.id, this.lifetime.signal);
      await download({content: blob, fileName: 'live_stream_trace.pftrace'});
      this.showMessage('Trace downloaded');
    } catch (e) {
      if (this.lifetime.signal.aborted) return;
      this.showMessage(`Download failed: ${getErrorMessage(e)}`);
    }
  }

  private showMessage(message: string) {
    this.trace.omnibox.showStatusMessage(`Live stream: ${message}`, 5000);
    this.trace.raf.scheduleFullRedraw();
  }

  // Removes the tracks that other plugins added for the seed, e.g. for its
  // history, which the live tracks show. No live track exists yet.
  private removeSeedTracks() {
    this.trace.defaultWorkspace.clear();
    this.trace.raf.scheduleFullRedraw();
  }

  private retentionNs(): bigint {
    return BigInt(this.settings.retention.get()) * NS_PER_MIN;
  }

  // Shows wall-clock times in the server's time zone, unless the user picked
  // a timestamp format.
  private useTraceTimeZone() {
    // Registered by the core.
    const format = this.trace.settings.get<TimestampFormat>('timestampFormat');
    if (format?.isDefault) format.set(TimestampFormat.TraceTz);
  }

  // The notice first: dismissing it shows the next error, if any.
  private error(): string | undefined {
    return (
      this.notice ??
      this.requestError ??
      this.stopError ??
      (this.serverError || undefined)
    );
  }

  private status(): LiveStatus {
    return computeStatus({
      paused: this.paused,
      error: this.error(),
      waiting: this.waiting,
      lastDataMs: this.lastDataMs,
      nowMs: performance.now(),
    });
  }

  private refreshStatus() {
    this.host.refreshStatus();
  }

  // Ends the notice once its popout is dismissed.
  private dismissError(error: string) {
    if (error !== this.notice) return;
    this.notice = undefined;
    this.refreshStatus();
    this.trace.raf.scheduleFullRedraw();
  }

  // The session as the HUD sees it.
  private model(): LiveSessionModel {
    return {
      state: () => ({
        status: this.status(),
        error: this.error(),
        target: this.target,
        paused: this.paused,
      }),
      toggleFollow: () => this.toggleFollow(),
      togglePause: () => this.togglePause(),
      download: () => void this.download(),
      refreshTargets: () => this.refreshTargets(),
      dismissError: (error) => this.dismissError(error),
    };
  }
}

// The current frame's vsync time: the same for every callback of the frame,
// so that the playhead advances evenly whatever runs before. Without it,
// e.g. in tests, the current time.
function frameTimeMs(): number {
  const timeline =
    typeof document.timeline === 'object' ? document.timeline : undefined;
  const frameMs = timeline?.currentTime;
  return typeof frameMs === 'number' ? frameMs : performance.now();
}

// The window [start, end] and its width on each side.
function rangeAround(start: time, end: time): Span {
  const width = end - start;
  return {start: start - width, end: end + width};
}

function overlaps(a: Span, b: Span): boolean {
  return a.start <= b.end && a.end >= b.start;
}

// The smallest span that holds all `spans`, which must not be empty.
function coverSpans(spans: readonly Span[]): Span {
  let {start, end} = spans[0]!;
  for (const span of spans) {
    if (span.start < start) start = span.start;
    if (span.end > end) end = span.end;
  }
  return {start, end};
}

// Grows the trace bounds. The core has no API for this: it treats traces as
// immutable. TraceInfo.end is readonly in type only, and the core reads it on
// each use, so a write and a full redraw suffice.
function growTraceEnd(trace: Trace, end: time) {
  const info = trace.traceInfo as {end: time};
  if (end <= info.end) return;
  info.end = end;
  trace.raf.scheduleFullRedraw();
}
