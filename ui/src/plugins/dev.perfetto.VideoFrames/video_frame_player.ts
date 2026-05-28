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

// Max in-flight decoder inputs before the feed loop awaits. Bounds peak GPU
// memory: at 720p each held VideoFrame is ~1.8 MB.
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

  // Stream metadata, filled by ensureFramesLoaded.
  frames: FrameInfo[] = [];
  currentIdx = 0;
  // Playback speed multiplier. 1 = real time (matches the device's own
  // cadence from the captured ts deltas); 0.5 = half speed; etc. Read live
  // by the playback loop, so changing it takes effect on the next frame
  // without restarting playback.
  playbackRate = 1;
  // Names of stats entries this stream failed with (from the global
  // `stats` table; one entry per VideoFrameError.Reason, keyed by
  // display_id, e.g. 'android_video_size_cap_hit'). Empty array = healthy
  // stream. UI surfaces the names verbatim in the details panel.
  errors: string[] = [];
  private framesLoaded = false;
  // Codec config row id + codec string, learned at frame-loading time. The
  // codec_config bytes themselves are fetched lazily by loadSetup.
  private configId?: number;
  private codecString?: string;
  private setup?: Setup;

  // Render target, mounted/unmounted by the details panel.
  private canvas?: HTMLCanvasElement;

  // Bumped on every stop/seek so any in-flight async op can notice it was
  // cancelled before it next touches state.
  private token = 0;
  // Present iff currently playing. Cleared and torn down by stop().
  private playSession?: PlaySession;

  constructor(trace: Trace, trackUri: string, displayId: number) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.displayId = displayId;
  }

  get playing(): boolean {
    return this.playSession !== undefined;
  }

  get currentFrame(): FrameInfo | undefined {
    return this.frames[this.currentIdx];
  }

  // Public API

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

  async ensureFramesLoaded(): Promise<void> {
    if (this.framesLoaded) return;
    const res = await this.trace.engine.query(`
      SELECT id, ts, frame_number AS frameNumber,
             COALESCE(is_key_frame, 0) AS isKey,
             COALESCE(pts_us, 0) AS ptsUs,
             COALESCE(is_config, 0) AS isConfig,
             codec_string AS codecString
      FROM android_video_frames
      WHERE display_id = ${this.displayId}
      ORDER BY ts
    `);
    const it = res.iter({
      id: NUM,
      ts: LONG,
      frameNumber: NUM,
      isKey: NUM,
      ptsUs: LONG,
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
        ptsUs: Number(it.ptsUs),
      });
    }

    // Pick up any per-stream failure entries (one stats row per
    // VideoFrameError.Reason that fired). All start with the
    // 'android_video_' prefix and are keyed by display_id.
    const errRes = await this.trace.engine.query(`
      SELECT name FROM stats
      WHERE name LIKE 'android_video_%'
        AND idx = ${this.displayId}
        AND value > 0
      ORDER BY name
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

  // Move the timeline instant alongside the seek so the cursor follows the
  // active frame. load() in the details panel guards against re-decoding
  // when the resulting selection is already the frame we just seeked to.
  private selectAndSeek(idx: number): void {
    void this.seekToIndex(idx);
    this.trace.selection.selectTrackEvent(this.trackUri, this.frames[idx].id);
  }

  togglePlay(): void {
    if (this.playing) this.stop();
    else this.play();
    m.redraw();
  }

  // Set the playback speed multiplier. Takes effect immediately, mid-play
  // (the playback loop reads playbackRate every frame), without restarting
  // the decoder.
  setPlaybackRate(rate: number): void {
    this.playbackRate = rate;
    m.redraw();
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

  // Internals

  // Decode nearestKey(idx)..idx with a one-shot decoder and return the
  // decoded frames in presentation order plus the key-frame index k (so
  // the caller can pick outs[idx - k] as the target). The caller owns
  // closing every returned VideoFrame. Pure: touches no playback state
  // (canvas/token/session), so it is safe to call from the seek path and
  // from the read-only tooltip path concurrently.
  private async decodeUpTo(
    idx: number,
  ): Promise<{outs: VideoFrame[]; k: number} | undefined> {
    if (typeof VideoDecoder === 'undefined') return undefined;
    const setup = await this.loadSetup();
    if (setup === undefined) return undefined;
    const k = this.nearestKey(idx);
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

  // Seek to a single frame by array index, painting the target output onto
  // the live preview canvas. Called from the public
  // seek/prev/next/attachCanvas paths.
  private async seekToIndex(idx: number): Promise<void> {
    if (idx < 0 || idx >= this.frames.length) return;
    this.stop();
    this.currentIdx = idx;
    const decoded = await this.decodeUpTo(idx);
    if (decoded === undefined) return;
    const {outs, k} = decoded;
    const target = outs[idx - k] ?? outs[outs.length - 1];
    if (target !== undefined) this.drawFrame(target);
    for (const f of outs) f.close();
    m.redraw();
  }

  // Decode a single frame (by its timeline event id) to a PNG data URL,
  // for the track's hover tooltip. Fully read-only: it decodes onto a
  // detached, throwaway canvas and never touches the live preview canvas,
  // the play session, or the seek token -- so hovering the track while a
  // frame is selected or while playback is running has no effect on
  // either. Returns undefined if the id isn't a displayable frame or
  // WebCodecs is unavailable.
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

  // Composed of three steps gated by `token`:
  //   1. openPlaySession() resolves codec setup and registers a fresh
  //      decoder + queue on this.playSession.
  //   2. startPlaybackLoop() schedules the rAF playback loop that drains the queue at
  //      wall-clock cadence and paints the frame whose ts is now due.
  //   3. feedDecoder() pulls AUs out of trace_processor a batch at a time
  //      and decode()s them, backpressured against decoder.decodeQueueSize.
  // Teardown (decoder.close + queue close) lives in stop().
  private async runPlayLoop(token: number, fromIdx: number): Promise<void> {
    // Decode from the nearest key frame (the decoder needs it to seed the
    // reference list), but begin *painting* at fromIdx so play() starts on
    // the frame the user selected rather than jumping back to the key
    // frame. Frames in [k, fromIdx) are decoded only to prime the decoder
    // and are dropped without painting.
    const k = this.nearestKey(fromIdx);
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
    // Monotonic decode counter. The decoder emits outputs in presentation
    // order starting at frame k, so the n-th output is frame k+n. We must
    // NOT derive the index from queue.length: the playback loop shifts frames
    // off the queue concurrently, so queue.length undercounts once draining
    // begins, which would mislabel every later frame's index (wrong ts for
    // pacing, wrong timeline selection).
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
    // Virtual playback clock in trace-ns. It is only started once the
    // first paintable frame (idx >= fromIdx) is on screen, so the
    // asynchronous decode latency at startup doesn't burn playback time
    // and skip frames. Each subsequent frame advances it by the real
    // wall-clock delta scaled by playbackRate, so changing the rate
    // mid-play just changes the slope -- no jump, no re-anchor.
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
        // Paint the first paintable frame immediately and anchor the
        // clock to it.
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
          // Move the timeline's selected instant alongside playback so the
          // user can tell where they are. The details panel reads
          // player.playing in its load() and skips re-seeking when we
          // selected via this path, so the canvas keeps the frame we just
          // painted -- no flicker.
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
    // Pull bytes a batch at a time so peak JS memory stays O(batch) rather
    // than O(stream) -- a long capture would otherwise hold every encoded
    // AU in memory for the whole play duration.
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
    // flush() rejects if the decoder was closed mid-flight (the playback loop hit
    // end-of-stream, or stop() called from elsewhere). Either way the
    // session is already being torn down -- swallow.
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
    while (k > 0 && !this.frames[k].isKey) k--;
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
      FROM android_video_frames
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

// Build an EncodedVideoChunk from a FrameInfo + its raw bytes. The first
// decoded chunk per stream must carry the Annex-B SPS/PPS prepended (those
// NALs were emitted out-of-band on the codec_config packet).
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
