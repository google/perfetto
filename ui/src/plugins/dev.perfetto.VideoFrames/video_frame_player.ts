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

import m from 'mithril';
import type {Trace} from '../../public/trace';
import {BLOB, LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import {download, downloadUrl} from '../../base/download_utils';
import {muxToMp4, trimMp4} from './mp4';

// Max in-flight decoder inputs before the feed loop awaits, to bound the
// decoder's queue and held-frame memory.
const DECODER_BACKPRESSURE_LIMIT = 32;

// How many AUs the feed loop fetches in one SQL round trip. Keeps peak JS
// memory bounded to O(batch) encoded bytes rather than O(stream).
const FETCH_BATCH = 32;

// One displayable access unit. Encoded bytes are not held here; they're
// fetched on demand via __INTRINSIC_VIDEO_FRAME_AU_DATA(id).
export interface FrameInfo {
  id: number;
  ts: bigint; // perfetto boot-time ns of the on-screen change
  frameNumber: number;
  isKey: boolean;
  ptsUs: number;
}

// Decoder setup, cached once per stream.
interface Setup {
  codecString: string; // RFC 6381, e.g. "avc1.42c029"
  config: Uint8Array; // Annex-B SPS/PPS, prepended to the first decoded chunk
}

// A decoded frame queued for paint, tagged with its index into `frames`.
interface QueuedFrame {
  idx: number;
  frame: VideoFrame;
}

// The live play session: decoder + queue of decoded frames awaiting paint.
// Owned by play() and torn down by stop().
interface PlaySession {
  decoder: VideoDecoder;
  queue: QueuedFrame[];
}

// Per-stream player. The details panel mounts a <canvas> and registers it
// via attachCanvas(); decoded VideoFrames are drawn onto it with
// ctx.drawImage(). play() pipes AUs through one long-lived VideoDecoder
// backpressured against decodeQueueSize; a requestAnimationFrame loop paints
// whichever decoded frame is due against wall-clock, using the captured `ts`
// deltas so playback matches the device's own cadence.
export class VideoFramePlayer {
  // Stream identity.
  readonly trace: Trace;
  readonly trackUri: string;
  private readonly displayId: number;
  private readonly displayName: string;

  // Stream metadata, filled by ensureFramesLoaded.
  frames: FrameInfo[] = [];
  currentIdx = 0;
  // Playback speed multiplier (1 = real time). Read live by the playback
  // loop, so changes take effect on the next frame.
  playbackRate = 1;
  // android_video_* stats entries that fired for this stream; surfaced in
  // the details panel. Empty = healthy.
  errors: string[] = [];
  private framesLoaded = false;
  private framesLoadedPromise?: Promise<void>;
  private configId?: number;
  private codecString?: string;
  private setup?: Setup;

  // Render target, mounted/unmounted by the details panel.
  private canvas?: HTMLCanvasElement;

  // Bumped on every stop/seek so in-flight async ops can detect cancellation.
  private token = 0;
  // Present iff currently playing.
  private playSession?: PlaySession;

  constructor(
    trace: Trace,
    trackUri: string,
    displayId: number,
    displayName: string,
  ) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.displayId = displayId;
    this.displayName = displayName;
  }

  // Download the currently shown frame as a PNG image.
  async downloadFrameImage(): Promise<void> {
    const frame = this.currentFrame;
    if (frame === undefined) return;
    const url = await this.decodeFrameImage(frame.id);
    if (url === undefined) return;
    downloadUrl({
      fileName: `${this.displayName}-frame${frame.frameNumber}.png`,
      url,
    });
  }

  // Download the whole stream (from the first key frame) as an .mp4.
  async downloadVideo(): Promise<void> {
    await this.ensureFramesLoaded();
    await this.downloadFrames(this.frames, '');
  }

  // Download the frames covering [start, end] as an .mp4, trimmed to exactly
  // that window. The clip is extended back to the enclosing key frame so it can
  // be decoded; when the requested start falls after that key frame, the extra
  // lead-in frames are dropped by re-encoding from `start`.
  async downloadRegion(start: bigint, end: bigint): Promise<void> {
    await this.ensureFramesLoaded();
    // The frame on screen at `start` is the last one that began at or before it
    // (a capture stays up until the next frame), not the first one after.
    let lo = 0;
    while (lo + 1 < this.frames.length && this.frames[lo + 1].ts <= start) lo++;
    // A clip only decodes from a key frame, so back up to the enclosing one.
    let seed = lo;
    while (seed > 0 && !this.frames[seed].isKey) seed--;
    // Include every frame on screen through `end`.
    let hi = lo + 1;
    while (hi < this.frames.length && this.frames[hi].ts <= end) hi++;
    // Lead-in frames [seed, lo) decode but fall before the window. Drop them by
    // trimming to where frame `lo` begins -- measured in the clip's own clock,
    // which the mux builds from pts relative to the seed key frame (clip time
    // 0). With no lead-in -- start already on a key frame, or the enclosing key
    // frame was evicted so none precedes it -- the clip is a lossless remux.
    const trimStart =
      this.frames[seed].isKey && seed < lo
        ? (this.frames[lo].ptsUs - this.frames[seed].ptsUs) / 1e6
        : undefined;
    await this.downloadFrames(
      this.frames.slice(seed, hi),
      '-region',
      trimStart,
    );
  }

  // Mux the given frames (config + their access units) into an .mp4 and
  // download it. trimStartSec, when set, drops the lead-in before that time by
  // re-encoding (see trimMp4); it must fall on or after the first key frame.
  private async downloadFrames(
    sel: FrameInfo[],
    suffix: string,
    trimStartSec?: number,
  ): Promise<void> {
    // A clip must start on a key frame to decode; drop any leading delta frames.
    const firstKey = sel.findIndex((f) => f.isKey);
    if (firstKey < 0) return;
    sel = sel.slice(firstKey);
    const ids: number[] = [];
    if (this.configId !== undefined) ids.push(this.configId);
    ids.push(...sel.map((f) => f.id));
    const chunks = await this.fetchAuData(ids);
    let i = 0;
    const config =
      this.configId !== undefined ? chunks[i++] : new Uint8Array(0);
    const frames = sel.map((f, k) => ({
      data: chunks[i + k],
      isKey: f.isKey,
      pts: f.ptsUs,
    }));
    const {width, height} = await this.codedSize();
    let mp4 = await muxToMp4(
      this.codecString!,
      config,
      frames,
      width,
      height,
      30,
    );
    if (trimStartSec !== undefined && trimStartSec > 0) {
      mp4 = await trimMp4(mp4, trimStartSec);
    }
    await download({
      content: mp4,
      fileName: `${this.displayName}${suffix}.mp4`,
    });
  }

  // The stream's coded dimensions, read from a decoded key frame (mediabunny
  // needs them for the container). Throws if a key frame can't be decoded.
  private async codedSize(): Promise<{width: number; height: number}> {
    const keyIdx = this.frames.findIndex((f) => f.isKey);
    const decoded = keyIdx < 0 ? undefined : await this.decodeUpTo(keyIdx);
    if (decoded === undefined || decoded.outs.length === 0) {
      throw new Error('cannot decode a key frame to size the video');
    }
    const {codedWidth: width, codedHeight: height} = decoded.outs[0];
    for (const f of decoded.outs) f.close();
    return {width, height};
  }

  // Whether this browser/context can decode frames. WebCodecs is absent in
  // older browsers and in non-secure contexts; when false the preview/play
  // controls can't do anything, so the panel shows a hint instead.
  get webCodecsAvailable(): boolean {
    return typeof VideoDecoder !== 'undefined';
  }

  get playing(): boolean {
    return this.playSession !== undefined;
  }

  get currentFrame(): FrameInfo | undefined {
    return this.frames[this.currentIdx];
  }

  // The details panel calls this on canvas mount. Re-paints the current
  // frame so navigating away and back doesn't leave a blank box.
  attachCanvas(c: HTMLCanvasElement): void {
    this.canvas = c;
    if (this.framesLoaded && !this.playing) {
      void this.seekToIndex(this.currentIdx);
    }
  }

  detachCanvas(): void {
    this.canvas = undefined;
    this.stop();
  }

  // Cache the load promise so concurrent callers (e.g. the details panel and a
  // download firing together) share one load rather than doubling `frames`.
  async ensureFramesLoaded(): Promise<void> {
    return (this.framesLoadedPromise ??= this.loadFrames());
  }

  private async loadFrames(): Promise<void> {
    const res = await this.trace.engine.query(`
      SELECT id, ts, frame_number AS frameNumber,
             COALESCE(is_key_frame, 0) AS isKey,
             COALESCE(pts_us, 0) AS ptsUs,
             COALESCE(is_config, 0) AS isConfig,
             codec_string AS codecString
      FROM __intrinsic_video_frames
      WHERE display_id = ${this.displayId}
      ORDER BY ts
    `);
    const it = res.iter({
      id: NUM,
      ts: LONG,
      frameNumber: NUM,
      isKey: NUM,
      ptsUs: NUM,
      isConfig: NUM,
      codecString: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      if (it.codecString !== null) this.codecString = it.codecString;
      if (it.isConfig) {
        this.configId = it.id;
        continue;
      }
      this.frames.push({
        id: it.id,
        ts: it.ts,
        frameNumber: it.frameNumber,
        isKey: it.isKey !== 0,
        ptsUs: it.ptsUs,
      });
    }

    // Per-stream failures: import-log rows for this display_id; name = reason.
    const errRes = await this.trace.engine.query(`
      SELECT DISTINCT l.name AS name
      FROM _trace_import_logs l
      JOIN args a USING (arg_set_id)
      WHERE l.name LIKE 'android_video_%'
        AND a.key = 'display_id'
        AND a.int_value = ${this.displayId}
      ORDER BY l.name
    `);
    const errIt = errRes.iter({name: STR_NULL});
    for (; errIt.valid(); errIt.next()) {
      if (errIt.name !== null) this.errors.push(errIt.name);
    }

    this.framesLoaded = true;
  }

  async seek(eventId: number): Promise<void> {
    const idx = this.frames.findIndex((f) => f.id === eventId);
    if (idx >= 0) await this.seekToIndex(idx);
  }

  prev(): void {
    if (this.currentIdx > 0) this.selectAndSeek(this.currentIdx - 1);
  }

  next(): void {
    if (this.currentIdx < this.frames.length - 1) {
      this.selectAndSeek(this.currentIdx + 1);
    }
  }

  // Seek and move the timeline cursor together. The details panel's load()
  // skips re-decoding when the selection is already the seeked frame.
  private selectAndSeek(idx: number): void {
    void this.seekToIndex(idx);
    this.trace.selection.selectTrackEvent(this.trackUri, this.frames[idx].id);
  }

  togglePlay(): void {
    if (this.playing) this.stop();
    else this.play();
  }

  setPlaybackRate(rate: number): void {
    this.playbackRate = rate;
  }

  play(): void {
    this.stop();
    void this.runPlayLoop(++this.token, this.currentIdx);
  }

  stop(): void {
    this.token++;
    const s = this.playSession;
    if (s === undefined) return;
    this.playSession = undefined;
    if (s.decoder.state !== 'closed') s.decoder.close();
    for (const f of s.queue) f.frame.close();
  }

  // Decode nearestKey(idx)..idx with a one-shot decoder, returning the
  // frames and the key-frame index k. The caller owns closing them. Pure:
  // touches no playback state, so seek and the tooltip path can call it
  // concurrently. Cost is O(distance to the preceding key frame).
  private async decodeUpTo(
    idx: number,
  ): Promise<{outs: VideoFrame[]; k: number} | undefined> {
    if (typeof VideoDecoder === 'undefined') return undefined;
    const setup = await this.loadSetup();
    if (setup === undefined) return undefined;
    const k = this.nearestKey(idx);
    // No key frame precedes idx (ring buffer dropped the GOP start); the
    // decoder can't be seeded, so this frame is undecodable.
    if (k < 0) return undefined;
    const range = this.frames.slice(k, idx + 1);
    const datas = await this.fetchAuData(range.map((f) => f.id));
    const outs: VideoFrame[] = [];
    const decoder = new VideoDecoder({
      output: (f) => outs.push(f),
      error: (e) => console.warn('VideoFrames:', e),
    });
    decoder.configure(decoderConfig(setup));
    for (let i = 0; i < range.length; i++) {
      decoder.decode(chunkFor(setup, range[i], datas[i], i === 0));
    }
    await decoder.flush().catch(() => undefined);
    if (decoder.state !== 'closed') decoder.close();
    return {outs, k};
  }

  // Seek to a frame by index and paint it onto the live preview canvas.
  private async seekToIndex(idx: number): Promise<void> {
    if (idx < 0 || idx >= this.frames.length) return;
    // Capture the post-stop() token so we can drop a stale decode if a
    // newer seek/play started while ours was in flight.
    this.stop();
    const token = this.token;
    this.currentIdx = idx;
    const decoded = await this.decodeUpTo(idx);
    if (decoded === undefined) return;
    const {outs, k} = decoded;
    if (token !== this.token) {
      for (const f of outs) f.close();
      return;
    }
    const target = outs[idx - k] ?? outs[outs.length - 1];
    if (target !== undefined) this.drawFrame(target);
    for (const f of outs) f.close();
    m.redraw();
  }

  // Decode one frame (by event id) to a PNG data URL for the hover
  // tooltip. Read-only: decodes onto a throwaway canvas, never touching
  // the live preview or playback state.
  async decodeFrameImage(eventId: number): Promise<string | undefined> {
    await this.ensureFramesLoaded();
    const idx = this.frames.findIndex((f) => f.id === eventId);
    if (idx < 0) return undefined;
    const decoded = await this.decodeUpTo(idx);
    if (decoded === undefined) return undefined;
    const {outs, k} = decoded;
    try {
      const frame = outs[idx - k] ?? outs[outs.length - 1];
      if (frame === undefined) return undefined;
      const w = frame.displayWidth || frame.codedWidth;
      const h = frame.displayHeight || frame.codedHeight;
      if (w <= 0 || h <= 0) return undefined;
      const canvas = document.createElement('canvas');
      canvas.width = w;
      canvas.height = h;
      canvas.getContext('2d')?.drawImage(frame, 0, 0);
      return canvas.toDataURL('image/png');
    } finally {
      for (const f of outs) f.close();
    }
  }

  // openPlaySession() sets up the decoder, startPlaybackLoop() paints from
  // the queue at wall-clock cadence, feedDecoder() decodes AUs with
  // backpressure. All gated by `token`; teardown is in stop().
  private async runPlayLoop(token: number, fromIdx: number): Promise<void> {
    // Decode from the nearest key frame (needed to seed the decoder) but
    // paint from fromIdx, so play() starts on the selected frame. Frames in
    // [k, fromIdx) are decoded only to prime the decoder, not painted.
    const k = this.nearestKey(fromIdx);
    if (k < 0) return; // no key frame to seed from (ring buffer start)
    const session = await this.openPlaySession(token, k);
    if (session === undefined) return;
    this.startPlaybackLoop(token, session, fromIdx);
    await this.feedDecoder(token, session, k);
  }

  private async openPlaySession(
    token: number,
    k: number,
  ): Promise<PlaySession | undefined> {
    if (typeof VideoDecoder === 'undefined' || this.canvas === undefined) {
      return undefined;
    }
    const setup = await this.loadSetup();
    if (setup === undefined || token !== this.token) return undefined;
    const queue: QueuedFrame[] = [];
    // n-th output is frame k+n. Counted independently of queue.length,
    // which the playback loop drains concurrently.
    let decoded = 0;
    const decoder = new VideoDecoder({
      output: (f) => {
        if (token !== this.token) {
          f.close();
          return;
        }
        queue.push({idx: k + decoded++, frame: f});
      },
      error: (e) => console.warn('VideoFrames:', e),
    });
    decoder.configure(decoderConfig(setup));
    const session: PlaySession = {decoder, queue};
    this.playSession = session;
    return session;
  }

  private startPlaybackLoop(
    token: number,
    session: PlaySession,
    fromIdx: number,
  ): void {
    // Virtual playback clock in trace-ns, anchored to the first painted
    // frame (so startup decode latency doesn't skip frames) and advanced
    // each frame by the wall-clock delta scaled by playbackRate.
    let lastWallMs = 0;
    let playbackTs = 0n;
    let started = false;
    const step = () => {
      if (token !== this.token) return;
      // Drop seed frames decoded before fromIdx without painting (they
      // exist only to prime the decoder's reference list).
      while (session.queue.length > 0 && session.queue[0].idx < fromIdx) {
        session.queue.shift()!.frame.close();
      }
      let due: QueuedFrame | undefined;
      if (!started) {
        // Paint the first frame immediately and anchor the clock to it.
        if (session.queue.length > 0) {
          due = session.queue.shift();
          started = true;
          lastWallMs = performance.now();
          playbackTs = this.frames[due!.idx].ts;
        }
      } else {
        const nowMs = performance.now();
        playbackTs += BigInt(
          Math.floor((nowMs - lastWallMs) * 1e6 * this.playbackRate),
        );
        lastWallMs = nowMs;
        // Drain all frames whose ts is now due; paint only the latest.
        while (
          session.queue.length > 0 &&
          this.frames[session.queue[0].idx].ts <= playbackTs
        ) {
          if (due !== undefined) due.frame.close();
          due = session.queue.shift();
        }
      }
      if (due !== undefined) {
        this.drawFrame(due.frame);
        due.frame.close();
        if (due.idx !== this.currentIdx) {
          this.currentIdx = due.idx;
          // Move the timeline cursor with playback. The details panel skips
          // re-seeking while playing, so the canvas doesn't flicker.
          this.trace.selection.selectTrackEvent(
            this.trackUri,
            this.frames[due.idx].id,
          );
          m.redraw();
        }
        if (due.idx >= this.frames.length - 1) {
          this.stop();
          m.redraw();
          return;
        }
      }
      requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  }

  private async feedDecoder(
    token: number,
    session: PlaySession,
    k: number,
  ): Promise<void> {
    const setup = this.setup;
    if (setup === undefined) return;
    // Fetch a batch at a time so peak JS memory is O(batch), not O(stream).
    for (let base = k; base < this.frames.length; base += FETCH_BATCH) {
      if (token !== this.token) break;
      const end = Math.min(base + FETCH_BATCH, this.frames.length);
      const batch = this.frames.slice(base, end);
      const datas = await this.fetchAuData(batch.map((f) => f.id));
      for (let i = 0; i < batch.length; i++) {
        if (!(await this.waitForDecoderCapacity(token, session.decoder))) {
          return;
        }
        session.decoder.decode(
          chunkFor(setup, batch[i], datas[i], base + i === k),
        );
      }
    }
    // flush() rejects if the decoder was already closed (end-of-stream or
    // stop()); the session is being torn down either way.
    await session.decoder.flush().catch(() => undefined);
  }

  // Returns true once the decoder can accept another chunk, false if `token`
  // was invalidated while we waited (caller should bail).
  private async waitForDecoderCapacity(
    token: number,
    decoder: VideoDecoder,
  ): Promise<boolean> {
    while (decoder.decodeQueueSize > DECODER_BACKPRESSURE_LIMIT) {
      if (token !== this.token) return false;
      await new Promise<void>((r) => requestAnimationFrame(() => r()));
    }
    return token === this.token;
  }

  // Index of the nearest key frame at or before idx. Decoders need a key
  // frame to seed the reference list before any non-key frame.
  private nearestKey(idx: number): number {
    let k = idx;
    while (k >= 0 && !this.frames[k].isKey) k--;
    // -1 when no key frame precedes idx, e.g. a ring buffer whose start (and
    // the GOP's key frame) was overwritten. Such frames can't be decoded.
    return k;
  }

  private async loadSetup(): Promise<Setup | undefined> {
    if (this.setup !== undefined) return this.setup;
    if (this.configId === undefined || this.codecString === undefined) return;
    const [config] = await this.fetchAuData([this.configId]);
    if (config.length === 0) return;
    this.setup = {codecString: this.codecString, config};
    return this.setup;
  }

  private async fetchAuData(ids: ReadonlyArray<number>): Promise<Uint8Array[]> {
    if (ids.length === 0) return [];
    const res = await this.trace.engine.query(`
      SELECT id, __intrinsic_video_frame_au_data(id) AS data
      FROM __intrinsic_video_frames
      WHERE id IN (${ids.join(',')})
    `);
    const it = res.iter({id: NUM, data: BLOB});
    const byId = new Map<number, Uint8Array>();
    for (; it.valid(); it.next()) byId.set(it.id, it.data);
    return ids.map((id) => byId.get(id) ?? new Uint8Array(0));
  }

  private drawFrame(frame: VideoFrame): void {
    const c = this.canvas;
    if (c === undefined) return;
    const w = frame.displayWidth || frame.codedWidth;
    const h = frame.displayHeight || frame.codedHeight;
    if (w <= 0 || h <= 0) return;
    if (c.width !== w) c.width = w;
    if (c.height !== h) c.height = h;
    c.getContext('2d')?.drawImage(frame, 0, 0);
  }
}

// The first chunk per stream carries the codec_config (SPS/PPS) prepended,
// since those NALs were emitted out-of-band.
function chunkFor(
  setup: Setup,
  frame: FrameInfo,
  bytes: Uint8Array,
  isFirst: boolean,
): EncodedVideoChunk {
  return new EncodedVideoChunk({
    type: frame.isKey ? 'key' : 'delta',
    timestamp: frame.ptsUs,
    data: isFirst ? concat(setup.config, bytes) : bytes,
  });
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

function decoderConfig(setup: Setup): VideoDecoderConfig {
  return {
    codec: setup.codecString,
    optimizeForLatency: true,
    hardwareAcceleration: 'no-preference',
  };
}
