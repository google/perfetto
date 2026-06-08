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
import {Time, type time} from '../../base/time';
import {BLOB, LONG_NULL, NUM} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';

// Plays a time range of one audio stream. The stored frames are raw AAC access
// units; we wrap them back into an ADTS stream (using the AudioSpecificConfig
// from codec_config) and hand the whole range to AudioContext.decodeAudioData,
// which the browser decodes to an AudioBuffer in one call. Simpler and more
// widely supported than driving WebCodecs AudioDecoder frame-by-frame.
export class AudioPlayer {
  private readonly trace: Trace;
  private readonly streamId: number;
  private ac?: AudioContext;
  private src?: AudioBufferSourceNode;
  // Decoded audio for the current range, retained across pause so play resumes.
  private buffer?: AudioBuffer;
  private cursorRaf?: number;
  // Playback speed (also shifts pitch — AudioBufferSourceNode.playbackRate).
  playbackRate = 1;
  // The trace-time range the decoded buffer covers.
  private rangeStartNs = 0n;
  private rangeEndNs = 0n;
  // Position accounting: audio seconds into the buffer at the last (re)start /
  // rate change / pause, plus the ac.currentTime at that point.
  private audioBaseSec = 0;
  private rateAcTime = 0;
  playing = false;

  constructor(trace: Trace, streamId: number) {
    this.trace = trace;
    this.streamId = streamId;
  }

  get available(): boolean {
    return typeof AudioContext !== 'undefined';
  }

  // True when stopped mid-stream with audio still buffered: the next play
  // resumes rather than restarts.
  get paused(): boolean {
    return !this.playing && this.buffer !== undefined && this.audioBaseSec > 0;
  }

  // Play/resume the whole stream (the play button on the track shell).
  async playAll(): Promise<void> {
    const r = await this.trace.engine.query(`
      SELECT MIN(ts) AS a, MAX(ts) AS b
      FROM __intrinsic_audio_frames
      WHERE stream_id = ${this.streamId} AND is_config IS NULL
    `);
    const {a, b} = r.firstRow({a: LONG_NULL, b: LONG_NULL});
    if (a === null || b === null) return;
    await this.playRange(a, b);
  }

  // Play a range, or resume it from where it was paused if it's the same range
  // already buffered. A different range decodes and plays from the start.
  async playRange(startNs: bigint, endNs: bigint): Promise<void> {
    if (
      !this.playing &&
      this.buffer !== undefined &&
      this.ac !== undefined &&
      this.rangeStartNs === startNs &&
      this.rangeEndNs === endNs &&
      this.audioBaseSec < this.buffer.duration
    ) {
      this.startSource(this.audioBaseSec);
      return;
    }
    await this.startFresh(startNs, endNs);
  }

  private async startFresh(startNs: bigint, endNs: bigint): Promise<void> {
    this.stop();

    // AudioSpecificConfig from the codec_config row: AOT(5) srIdx(4) ch(4).
    const cfg = await this.trace.engine.query(`
      SELECT id FROM __intrinsic_audio_frames
      WHERE stream_id = ${this.streamId} AND is_config = 1 LIMIT 1
    `);
    const cfgId = cfg.firstRow({id: NUM}).id;
    const ascRes = await this.trace.engine.query(
      `SELECT __intrinsic_audio_frame_au_data(${cfgId}) AS data`,
    );
    const asc = ascRes.firstRow({data: BLOB}).data;
    if (asc.length < 2) return;
    const profile = (asc[0] >> 3) - 1; // ADTS profile = AOT - 1
    const srIdx = ((asc[0] & 0x7) << 1) | (asc[1] >> 7);
    const channels = (asc[1] >> 3) & 0xf;

    // Raw AAC access units in the range, in order.
    const fr = await this.trace.engine.query(`
      SELECT __intrinsic_audio_frame_au_data(id) AS data
      FROM __intrinsic_audio_frames
      WHERE stream_id = ${this.streamId} AND is_config IS NULL
        AND ts >= ${startNs} AND ts <= ${endNs}
      ORDER BY ts
    `);
    const aus: Uint8Array[] = [];
    const it = fr.iter({data: BLOB});
    for (; it.valid(); it.next()) aus.push(it.data.slice());
    if (aus.length === 0) return;

    const adts = this.toAdts(aus, profile, srIdx, channels);
    const ac = new AudioContext();
    let buffer: AudioBuffer;
    try {
      buffer = await ac.decodeAudioData(adts.buffer.slice(0));
    } catch (e) {
      console.warn('Audio:', e);
      void ac.close();
      return;
    }
    this.ac = ac;
    this.buffer = buffer;
    this.rangeStartNs = startNs;
    this.rangeEndNs = endNs;
    this.startSource(0);
  }

  // (Re)start a buffer source from the given offset (audio seconds into the
  // buffer) — used for both fresh playback (0) and resume.
  private startSource(offsetSec: number): void {
    if (this.ac === undefined || this.buffer === undefined) return;
    const src = this.ac.createBufferSource();
    src.buffer = this.buffer;
    src.playbackRate.value = this.playbackRate;
    src.connect(this.ac.destination);
    src.onended = () => {
      // Natural end only — pause()/stop() detach this handler first. Reset to
      // the start so the next play begins from the beginning.
      this.playing = false;
      this.audioBaseSec = 0;
      this.clearCursor();
      m.redraw();
    };
    src.start(0, offsetSec);
    this.src = src;
    this.audioBaseSec = offsetSec;
    this.rateAcTime = this.ac.currentTime;
    this.playing = true;
    this.startCursor();
    m.redraw();
  }

  // Pause: freeze the position and stop the source, keeping the decoded buffer
  // so a later play resumes from here.
  pause(): void {
    if (!this.playing || this.ac === undefined || this.src === undefined) {
      return;
    }
    this.audioBaseSec +=
      (this.ac.currentTime - this.rateAcTime) * this.playbackRate;
    if (this.buffer !== undefined && this.audioBaseSec > this.buffer.duration) {
      this.audioBaseSec = this.buffer.duration;
    }
    this.src.onended = null;
    try {
      this.src.stop();
    } catch {
      // Already stopped.
    }
    this.src = undefined;
    this.playing = false;
    this.clearCursor();
    m.redraw();
  }

  // Change playback speed, applied live if currently playing. Integrates the
  // audio consumed at the old rate before switching, so the cursor stays
  // aligned across rate changes.
  setRate(rate: number): void {
    if (this.playing && this.ac !== undefined && this.src !== undefined) {
      this.audioBaseSec +=
        (this.ac.currentTime - this.rateAcTime) * this.playbackRate;
      this.rateAcTime = this.ac.currentTime;
      this.src.playbackRate.value = rate;
    }
    this.playbackRate = rate;
    m.redraw();
  }

  // The current playback position as trace time. Defined while playing and
  // while paused mid-stream (so the playhead stays put on pause); undefined
  // when stopped or sitting at the start. Each track draws its own playhead
  // from this, so streams play independently.
  currentTs(): time | undefined {
    if (this.ac === undefined || this.buffer === undefined) return undefined;
    if (!this.playing && this.audioBaseSec <= 0) return undefined;
    let offsetSec = this.audioBaseSec;
    if (this.playing) {
      offsetSec += (this.ac.currentTime - this.rateAcTime) * this.playbackRate;
    }
    let ns = this.rangeStartNs + BigInt(Math.round(offsetSec * 1e9));
    if (ns > this.rangeEndNs) ns = this.rangeEndNs;
    return Time.fromRaw(ns);
  }

  // Repaint the waveform track each frame while playing so it can redraw its
  // own moving playhead.
  private startCursor(): void {
    const tick = () => {
      if (!this.playing || this.ac === undefined) {
        this.clearCursor();
        return;
      }
      this.trace.raf.scheduleCanvasRedraw();
      this.cursorRaf = requestAnimationFrame(tick);
    };
    this.cursorRaf = requestAnimationFrame(tick);
  }

  private clearCursor(): void {
    if (this.cursorRaf !== undefined) {
      cancelAnimationFrame(this.cursorRaf);
      this.cursorRaf = undefined;
    }
    // Repaint so the playhead reflects the new state.
    this.trace.raf.scheduleCanvasRedraw();
  }

  // Full teardown: stop, drop the decoded buffer, and reset to the start.
  stop(): void {
    if (this.src !== undefined) {
      this.src.onended = null;
      try {
        this.src.stop();
      } catch {
        // Already stopped.
      }
      this.src = undefined;
    }
    void this.ac?.close();
    this.ac = undefined;
    this.buffer = undefined;
    this.audioBaseSec = 0;
    this.playing = false;
    this.clearCursor();
    m.redraw();
  }

  // Wrap raw AAC access units into an ADTS elementary stream (7-byte headers).
  private toAdts(
    aus: Uint8Array[],
    profile: number,
    srIdx: number,
    channels: number,
  ): Uint8Array {
    let total = 0;
    for (const au of aus) total += au.length + 7;
    const out = new Uint8Array(total);
    let o = 0;
    for (const au of aus) {
      const len = au.length + 7;
      out[o++] = 0xff;
      out[o++] = 0xf1; // MPEG-4, no CRC
      out[o++] = (profile << 6) | (srIdx << 2) | ((channels >> 2) & 1);
      out[o++] = ((channels & 3) << 6) | ((len >> 11) & 3);
      out[o++] = (len >> 3) & 0xff;
      out[o++] = ((len & 7) << 5) | 0x1f;
      out[o++] = 0xfc;
      out.set(au, o);
      o += au.length;
    }
    return out;
  }
}
