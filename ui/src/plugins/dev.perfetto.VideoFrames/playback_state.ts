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
import {BLOB, LONG, NUM} from '../../trace_processor/query_result';

export interface FrameInfo {
  id: number;
  ts: bigint;
  frameNumber: number;
  // v2 (hardware video) fields. codec is undefined for v1 still images.
  codec?: number;
  isKey?: boolean;
  ptsUs?: number;
}

export const FPS_OPTIONS = [1, 5, 10, 15, 30, 60, 120, 240];

// Shared fps across all players in the session.
let sessionFps = 30;

export function getSessionFps(): number {
  return sessionFps;
}
export function setSessionFps(fps: number): void {
  sessionFps = fps;
}

// Player for a single video frame stream (identified by trackId).
// One instance per stream, created once in onTraceLoad and reused across
// panel recreations.
export class VideoFramePlayer {
  readonly trace: Trace;
  readonly trackUri: string;
  private readonly trackId: number;

  frames: FrameInfo[] = [];
  currentIdx = 0;
  imageUrl?: string;
  playing = false;

  private playTimer?: ReturnType<typeof setInterval>;
  private framesLoaded = false;
  private playbackStartIdx = 0;

  // v2 hardware-video decode state (lazy).
  private isVideo = false;
  private streamCodec = 0; // VideoFrame.Codec: 1 = H264, 2 = HEVC
  private codecConfigId?: number; // row id of the codec_config-only frame
  private codecString?: string; // e.g. avc1.42c00b / hev1.1.6.L93.B0, from the SPS
  private codecConfig?: Uint8Array; // Annex-B SPS/PPS, prepended to key frames

  constructor(trace: Trace, trackUri: string, trackId: number) {
    this.trace = trace;
    this.trackUri = trackUri;
    this.trackId = trackId;
  }

  get fps(): number {
    return sessionFps;
  }

  get currentFrame(): FrameInfo | undefined {
    return this.frames[this.currentIdx];
  }

  async ensureFramesLoaded(): Promise<void> {
    if (this.framesLoaded) return;
    const res = await this.trace.engine.query(`
      SELECT id, ts, frame_number AS frameNumber,
             COALESCE(codec, 0) AS codec,
             COALESCE(is_key_frame, 0) AS isKey,
             COALESCE(pts_us, 0) AS ptsUs,
             COALESCE(is_config, 0) AS isConfig
      FROM android_video_frames
      WHERE COALESCE(track_id, 0) = ${this.trackId}
      ORDER BY ts
    `);
    const it = res.iter({
      id: NUM,
      ts: LONG,
      frameNumber: NUM,
      codec: NUM,
      isKey: NUM,
      ptsUs: LONG,
      isConfig: NUM,
    });
    this.frames = [];
    for (; it.valid(); it.next()) {
      if (it.isConfig) {
        // Decoder setup, not a displayable frame: remember it for v2 decode.
        this.codecConfigId = it.id;
        this.isVideo = true;
        if (it.codec) this.streamCodec = it.codec;
        continue;
      }
      const codec = it.codec || undefined;
      if (codec !== undefined) {
        this.isVideo = true;
        this.streamCodec = codec;
      }
      this.frames.push({
        id: it.id,
        ts: it.ts,
        frameNumber: it.frameNumber,
        codec,
        isKey: it.isKey !== 0,
        ptsUs: Number(it.ptsUs),
      });
    }
    this.framesLoaded = true;
  }

  async goToId(eventId: number): Promise<void> {
    const idx = this.frames.findIndex((f) => f.id === eventId);
    if (idx >= 0) {
      await this.loadImage(idx);
    }
  }

  async loadImage(idx: number): Promise<void> {
    if (idx < 0 || idx >= this.frames.length) return;
    this.currentIdx = idx;

    if (this.imageUrl) {
      URL.revokeObjectURL(this.imageUrl);
      this.imageUrl = undefined;
    }

    if (this.isVideo && this.frames[idx].codec !== undefined) {
      await this.decodeVideoFrame(idx);
      m.redraw();
      return;
    }

    const id = this.frames[idx].id;
    const res = await this.trace.engine.query(
      `SELECT video_frame_image(${id}) AS img`,
    );
    const row = res.firstRow({img: BLOB});
    if (row.img.length > 0) {
      const blob = new Blob([row.img]);
      this.imageUrl = URL.createObjectURL(blob);
    }
    m.redraw();
  }

  // --- v2: decode an H.264/HEVC frame with WebCodecs ---

  private async fetchBytes(id: number): Promise<Uint8Array> {
    const res = await this.trace.engine.query(
      `SELECT video_frame_image(${id}) AS img`,
    );
    return res.firstRow({img: BLOB}).img;
  }

  private async ensureCodecConfig(): Promise<boolean> {
    if (this.codecConfig) return true;
    if (this.codecConfigId === undefined) return false;
    this.codecConfig = await this.fetchBytes(this.codecConfigId);
    this.codecString =
      this.streamCodec === 2 /* CODEC_HEVC */
        ? hevcCodecStringFromAnnexB(this.codecConfig)
        : avcCodecStringFromAnnexB(this.codecConfig);
    return this.codecConfig.length > 0 && this.codecString !== undefined;
  }

  // Decode from the nearest preceding key frame up to `idx` and paint the
  // target frame to a canvas, reusing the existing <img> rendering path.
  private async decodeVideoFrame(idx: number): Promise<void> {
    const VD = (self as unknown as {VideoDecoder?: unknown}).VideoDecoder as
      | (new (init: object) => VideoDecoderLike)
      | undefined;
    if (VD === undefined) {
      console.warn('VideoFrames: WebCodecs VideoDecoder unavailable');
      return;
    }
    if (!(await this.ensureCodecConfig())) return;

    // nearest key frame at or before idx
    let k = idx;
    while (k > 0 && !this.frames[k].isKey) k--;

    const ids = this.frames.slice(k, idx + 1).map((f) => f.id);
    const datas = await Promise.all(ids.map((id) => this.fetchBytes(id)));

    const outputs: VideoFrameLike[] = [];
    let decodeErr: unknown;
    const decoder = new VD({
      output: (frame: VideoFrameLike) => outputs.push(frame),
      error: (e: unknown) => {
        decodeErr = e;
      },
    });
    decoder.configure({
      codec: this.codecString!,
      optimizeForLatency: true,
      hardwareAcceleration: 'prefer-software',
    });

    for (let i = 0; i < datas.length; i++) {
      const f = this.frames[k + i];
      // The Annex-B AUs carry no in-band SPS/PPS; prepend the codec config to
      // the (key) chunk that seeds the decode.
      const data =
        i === 0 ? concatBytes(this.codecConfig!, datas[i]) : datas[i];
      decoder.decode(
        new (
          self as unknown as {EncodedVideoChunk: new (i: object) => unknown}
        ).EncodedVideoChunk({
          type: f.isKey ? 'key' : 'delta',
          timestamp: f.ptsUs ?? 0,
          data,
        }),
      );
    }
    try {
      await decoder.flush();
    } catch (e) {
      decodeErr = e;
    }
    decoder.close();
    if (decodeErr !== undefined) {
      console.warn('VideoFrames: decode error', decodeErr);
    }

    // baseline H.264 has no reordering: decode order == display order.
    const target: VideoFrameLike | undefined =
      outputs[idx - k] ?? outputs[outputs.length - 1];
    if (target !== undefined) {
      const w = target.displayWidth || target.codedWidth;
      const h = target.displayHeight || target.codedHeight;
      const canvas = document.createElement('canvas');
      canvas.width = w;
      canvas.height = h;
      const ctx = canvas.getContext('2d');
      if (ctx) {
        ctx.drawImage(target as unknown as CanvasImageSource, 0, 0);
        this.imageUrl = canvas.toDataURL('image/png');
      }
    }
    for (const f of outputs) f.close();
  }

  togglePlay(): void {
    if (this.playing) {
      this.stop();
    } else {
      this.play();
    }
    m.redraw();
  }

  play(): void {
    this.stop();
    this.playing = true;
    this.playbackStartIdx = this.currentIdx;
    this.playTimer = setInterval(() => {
      if (this.currentIdx < this.frames.length - 1) {
        const nextIdx = this.currentIdx + 1;
        this.loadImage(nextIdx).then(() => {
          if (this.playing) {
            this.trace.selection.selectTrackEvent(
              this.trackUri,
              this.frames[nextIdx].id,
            );
          }
        });
      } else {
        this.stop();
        this.loadImage(this.playbackStartIdx).then(() => {
          this.trace.selection.selectTrackEvent(
            this.trackUri,
            this.frames[this.playbackStartIdx].id,
          );
        });
      }
    }, 1000 / sessionFps);
  }

  stop(): void {
    this.playing = false;
    if (this.playTimer !== undefined) {
      clearInterval(this.playTimer);
      this.playTimer = undefined;
    }
  }

  prev(): void {
    if (this.currentIdx > 0) this.loadImage(this.currentIdx - 1);
  }

  next(): void {
    if (this.currentIdx < this.frames.length - 1) {
      this.loadImage(this.currentIdx + 1);
    }
  }

  setFps(fps: number): void {
    sessionFps = fps;
    if (this.playing) {
      this.play();
    }
  }
}

// Minimal structural types so this compiles without DOM WebCodecs lib types.
interface VideoFrameLike {
  displayWidth: number;
  displayHeight: number;
  codedWidth: number;
  codedHeight: number;
  close(): void;
}
interface VideoDecoderLike {
  configure(config: object): void;
  decode(chunk: unknown): void;
  flush(): Promise<void>;
  close(): void;
}

function concatBytes(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

// Build the WebCodecs codec string (avc1.PPCCLL) from an Annex-B buffer that
// contains an SPS NAL (type 7): the three bytes after the NAL header are
// profile_idc, constraint flags, level_idc.
function avcCodecStringFromAnnexB(buf: Uint8Array): string | undefined {
  for (let i = 0; i + 4 < buf.length; i++) {
    const sc3 = buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1;
    const sc4 =
      buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 0 && buf[i + 3] === 1;
    if (!sc3 && !sc4) continue;
    const p = sc4 ? i + 4 : i + 3;
    if ((buf[p] & 0x1f) === 7 && p + 3 < buf.length) {
      const hex = (n: number) => n.toString(16).padStart(2, '0');
      return `avc1.${hex(buf[p + 1])}${hex(buf[p + 2])}${hex(buf[p + 3])}`;
    }
  }
  return undefined;
}

// Build the WebCodecs codec string (e.g. hev1.1.6.L93.B0) from an Annex-B buffer
// containing an HEVC SPS NAL (type 33). The profile_tier_level begins after the
// 2-byte NAL header + 1 byte (sps_video_parameter_set_id u4,
// sps_max_sub_layers_minus1 u3, sps_temporal_id_nesting_flag u1).
function hevcCodecStringFromAnnexB(buf: Uint8Array): string | undefined {
  for (let i = 0; i + 4 < buf.length; i++) {
    const sc3 = buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1;
    const sc4 =
      buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 0 && buf[i + 3] === 1;
    if (!sc3 && !sc4) continue;
    const p = sc4 ? i + 4 : i + 3;
    if (((buf[p] >> 1) & 0x3f) !== 33) continue; // not an SPS NAL
    const q = p + 3; // skip 2-byte NAL header + 1 byte before profile_tier_level
    if (q + 12 > buf.length) return undefined;
    const profileSpace = (buf[q] >> 6) & 0x3;
    const tierFlag = (buf[q] >> 5) & 0x1;
    const profileIdc = buf[q] & 0x1f;
    // 32-bit general_profile_compatibility_flags, emitted in reverse bit order.
    let compat =
      ((buf[q + 1] << 24) |
        (buf[q + 2] << 16) |
        (buf[q + 3] << 8) |
        buf[q + 4]) >>>
      0;
    let rev = 0;
    for (let k = 0; k < 32; k++) {
      rev = ((rev << 1) | (compat & 1)) >>> 0;
      compat >>>= 1;
    }
    const levelIdc = buf[q + 11];
    // 6 general_constraint_indicator bytes; trailing zero bytes are omitted.
    const cons: string[] = [];
    let last = -1;
    for (let k = 0; k < 6; k++) {
      const b = buf[q + 5 + k];
      cons.push(b.toString(16).padStart(2, '0').toUpperCase());
      if (b !== 0) last = k;
    }
    const space = ['', 'A', 'B', 'C'][profileSpace];
    const tier = tierFlag ? 'H' : 'L';
    let s = `hev1.${space}${profileIdc}.${rev.toString(16)}.${tier}${levelIdc}`;
    if (last >= 0) s += '.' + cons.slice(0, last + 1).join('.');
    return s;
  }
  return undefined;
}
