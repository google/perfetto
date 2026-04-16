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
import {Trace} from '../../public/trace';
import {BLOB, LONG, NUM} from '../../trace_processor/query_result';

export interface FrameInfo {
  id: number;
  ts: bigint;
  frameNumber: number;
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
      SELECT id, ts, frame_number AS frameNumber
      FROM android_video_frames
      WHERE COALESCE(track_id, 0) = ${this.trackId}
      ORDER BY ts
    `);
    const it = res.iter({id: NUM, ts: LONG, frameNumber: NUM});
    this.frames = [];
    for (; it.valid(); it.next()) {
      this.frames.push({id: it.id, ts: it.ts, frameNumber: it.frameNumber});
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
