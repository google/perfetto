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
import {range} from '../../base/array_utils';
import type {Trace} from '../../public/trace';
import {BLOB, LONG, NUM, STR_NULL} from '../../trace_processor/query_result';

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
  config: Uint8Array; // Annex-B SPS/PPS, prepended to every key frame
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
  private framesLoading?: Promise<void>;
  private configId?: number;
  private codecString?: string;
  private setup?: Setup;
  private setupLoading?: Promise<Setup | undefined>;

  // Render target, mounted/unmounted by the details panel.
  private canvas?: HTMLCanvasElement;

  // Bumped on every stop/seek so in-flight async ops can detect cancellation.
  private token = 0;
  // Present iff currently playing.
  private playSession?: PlaySession;

  constructor(trace: Trace, trackUri: string, displayId: number) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.displayId = displayId;
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

  // Idempotent and safe to call concurrently (e.g. from the details panel,
  // the track tooltip and the minimap).
  ensureFramesLoaded(): Promise<void> {
    this.framesLoading ??= this.loadFrames().catch((e) => {
      // Allow retrying, e.g. after a transient query failure.
      this.framesLoading = undefined;
      throw e;
    });
    return this.framesLoading;
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
    const frames: FrameInfo[] = [];
    for (; it.valid(); it.next()) {
      if (it.codecString !== null) this.codecString = it.codecString;
      if (it.isConfig) {
        this.configId = it.id;
        continue;
      }
      frames.push({
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
    const errors: string[] = [];
    const errIt = errRes.iter({name: STR_NULL});
    for (; errIt.valid(); errIt.next()) {
      if (errIt.name !== null) errors.push(errIt.name);
    }

    this.frames = frames;
    this.errors = errors;
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
    this.trace.selection.selectTrackEvent(this.trackUri, this.frames[idx]!.id);
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

  // Decodes the frame at `idx`; the caller must close it. Touches no playback
  // state, so seek and the tooltip path can call it concurrently.
  private async decodeFrame(idx: number): Promise<VideoFrame | undefined> {
    let result: VideoFrame | undefined;
    await this.decodeFrames([idx], (_, frame) => {
      result = frame;
    });
    return result;
  }

  // Seek to a frame by index and paint it onto the live preview canvas.
  private async seekToIndex(idx: number): Promise<void> {
    if (idx < 0 || idx >= this.frames.length) return;
    // Capture the post-stop() token so we can drop a stale decode if a
    // newer seek/play started while ours was in flight.
    this.stop();
    const token = this.token;
    this.currentIdx = idx;
    const frame = await this.decodeFrame(idx);
    if (frame === undefined) return;
    try {
      if (token !== this.token) return;
      this.drawFrame(frame);
      m.redraw();
    } finally {
      frame.close();
    }
  }

  // Decode one frame (by event id) to a PNG data URL for the hover
  // tooltip. Read-only: decodes onto a throwaway canvas, never touching
  // the live preview or playback state.
  async decodeFrameImage(eventId: number): Promise<string | undefined> {
    await this.ensureFramesLoaded();
    const idx = this.frames.findIndex((f) => f.id === eventId);
    const frame = await this.decodeFrame(idx);
    if (frame === undefined) return undefined;
    try {
      return frameToCanvas(frame)?.toDataURL('image/png');
    } finally {
      frame.close();
    }
  }

  // Decodes `frames[i]` for each of `indices` (sorted ascending), passing each
  // to `onFrame` in order; `onFrame` must close it. Only the GOPs containing
  // the requested frames are decoded, so sparse requests on long streams are
  // cheap. Undecodable frames (no preceding key frame, unsupported codec) are
  // skipped.
  async decodeFrames(
    indices: ReadonlyArray<number>,
    onFrame: (idx: number, frame: VideoFrame) => void,
    signal?: AbortSignal,
  ): Promise<void> {
    await this.ensureFramesLoaded();
    const setup = await this.loadSetup();
    if (setup === undefined || signal?.aborted) return;

    const wanted = new Set(indices);
    // Frames fed to the decoder, in order. The decoder outputs one frame per
    // input, in the same order.
    const fed: number[] = [];
    const decoder = new VideoDecoder({
      output: (f) => {
        const idx = fed.shift();
        if (idx !== undefined && wanted.has(idx) && !signal?.aborted) {
          onFrame(idx, f);
        } else {
          f.close();
        }
      },
      error: (e) => console.warn('VideoFrames:', e),
    });
    decoder.configure(decoderConfig(setup));
    try {
      const toDecode = gopRanges(this.frames, indices).flatMap(([start, end]) =>
        range(end - start + 1).map((i) => start + i),
      );
      // Fetch a batch at a time so peak JS memory is O(batch), not O(stream).
      for (let base = 0; base < toDecode.length; base += FETCH_BATCH) {
        const batch = toDecode.slice(base, base + FETCH_BATCH);
        const datas = await this.fetchAuData(
          batch.map((i) => this.frames[i]!.id),
        );
        for (let i = 0; i < batch.length; i++) {
          const cancelled = () =>
            signal?.aborted === true || decoder.state === 'closed';
          if (!(await waitForDecoderCapacity(decoder, cancelled))) return;
          const idx = batch[i]!;
          fed.push(idx);
          decoder.decode(chunkFor(setup, this.frames[idx]!, datas[i]!));
        }
      }
      await decoder.flush().catch(() => undefined);
    } finally {
      if (decoder.state !== 'closed') decoder.close();
    }
  }

  // openPlaySession() sets up the decoder, startPlaybackLoop() paints from
  // the queue at wall-clock cadence, feedDecoder() decodes AUs with
  // backpressure. All gated by `token`; teardown is in stop().
  private async runPlayLoop(token: number, fromIdx: number): Promise<void> {
    // Decode from the nearest key frame (needed to seed the decoder) but
    // paint from fromIdx, so play() starts on the selected frame. Frames in
    // [k, fromIdx) are decoded only to prime the decoder, not painted.
    const k = nearestKey(this.frames, fromIdx);
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
      while (session.queue.length > 0 && session.queue[0]!.idx < fromIdx) {
        session.queue.shift()!.frame.close();
      }
      let due: QueuedFrame | undefined;
      if (!started) {
        // Paint the first frame immediately and anchor the clock to it.
        if (session.queue.length > 0) {
          due = session.queue.shift();
          started = true;
          lastWallMs = performance.now();
          playbackTs = this.frames[due!.idx]!.ts;
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
          this.frames[session.queue[0]!.idx]!.ts <= playbackTs
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
            this.frames[due.idx]!.id,
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
        const cancelled = () => token !== this.token;
        if (!(await waitForDecoderCapacity(session.decoder, cancelled))) {
          return;
        }
        session.decoder.decode(chunkFor(setup, batch[i]!, datas[i]!));
      }
    }
    // flush() rejects if the decoder was already closed (end-of-stream or
    // stop()); the session is being torn down either way.
    await session.decoder.flush().catch(() => undefined);
  }

  // Loads the decoder setup once. Resolves to undefined if the stream can't be
  // decoded here (no WebCodecs, no codec config or an unsupported codec).
  private loadSetup(): Promise<Setup | undefined> {
    this.setupLoading ??= this.createSetup();
    return this.setupLoading;
  }

  private async createSetup(): Promise<Setup | undefined> {
    if (typeof VideoDecoder === 'undefined') return undefined;
    if (this.configId === undefined || this.codecString === undefined) {
      return undefined;
    }
    const [config] = await this.fetchAuData([this.configId]);
    if (config === undefined || config.length === 0) return undefined;
    const setup = {codecString: this.codecString, config};
    const {supported} = await VideoDecoder.isConfigSupported(
      decoderConfig(setup),
    );
    if (supported !== true) return undefined;
    this.setup = setup;
    return setup;
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

// Key frames carry the codec_config (SPS/PPS) prepended, since those NALs
// were emitted out-of-band. Every key frame needs it (not just the first) for
// the decoder to accept it when decoding across GOPs.
function chunkFor(
  setup: Setup,
  frame: FrameInfo,
  bytes: Uint8Array,
): EncodedVideoChunk {
  return new EncodedVideoChunk({
    type: frame.isKey ? 'key' : 'delta',
    timestamp: frame.ptsUs,
    data: frame.isKey ? concat(setup.config, bytes) : bytes,
  });
}

// Copies a decoded frame onto a new canvas, downscaled to fit within
// `maxSizePx` x `maxSizePx`. Returns undefined for frames without dimensions.
export function frameToCanvas(
  frame: VideoFrame,
  maxSizePx = Infinity,
): HTMLCanvasElement | undefined {
  const w = frame.displayWidth || frame.codedWidth;
  const h = frame.displayHeight || frame.codedHeight;
  if (w <= 0 || h <= 0) return undefined;
  const scale = Math.min(1, maxSizePx / w, maxSizePx / h);
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(w * scale));
  canvas.height = Math.max(1, Math.round(h * scale));
  canvas.getContext('2d')?.drawImage(frame, 0, 0, canvas.width, canvas.height);
  return canvas;
}

// Index of the nearest key frame at or before `idx`, or -1 if there is none
// (e.g. a ring buffer whose GOP start was overwritten), in which case the frame
// can't be decoded: decoders need a key frame to seed their reference list.
export function nearestKey(
  frames: ReadonlyArray<Pick<FrameInfo, 'isKey'>>,
  idx: number,
): number {
  let k = idx;
  while (k >= 0 && !frames[k]?.isKey) k--;
  return k;
}

// Merges the ranges [nearestKey(idx), idx] for each of `indices` (sorted) into
// the minimal list of inclusive frame ranges that need decoding.
export function gopRanges(
  frames: ReadonlyArray<Pick<FrameInfo, 'isKey'>>,
  indices: ReadonlyArray<number>,
): Array<[number, number]> {
  const ranges: Array<[number, number]> = [];
  for (const idx of indices) {
    if (idx < 0 || idx >= frames.length) continue;
    const k = nearestKey(frames, idx);
    if (k < 0) continue;
    const last = ranges[ranges.length - 1];
    if (last !== undefined && k <= last[1] + 1) {
      last[1] = Math.max(last[1], idx);
    } else {
      ranges.push([k, idx]);
    }
  }
  return ranges;
}

// Returns true once the decoder can accept another chunk, false if the caller
// was cancelled while waiting.
async function waitForDecoderCapacity(
  decoder: VideoDecoder,
  cancelled: () => boolean,
): Promise<boolean> {
  while (decoder.decodeQueueSize > DECODER_BACKPRESSURE_LIMIT) {
    if (cancelled()) return false;
    await new Promise((r) => setTimeout(r));
  }
  return !cancelled();
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
