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

import {afterEach, describe, expect, test, vi} from 'vitest';
import {DisposableStack} from '../../base/disposable_stack';
import type {Trace} from '../../public/trace';
import {
  drawVideoFrame,
  gopRanges,
  VideoFramePlayer,
} from './video_frame_player';

// Key frames are marked with 'K', other frames with '.'.
function frames(pattern: string) {
  return [...pattern].map((c) => ({isKey: c === 'K'}));
}

describe('gopRanges', () => {
  test('decodes from the preceding key frame', () => {
    expect(gopRanges(frames('K...K...'), [2, 7])).toEqual([
      [0, 2],
      [4, 7],
    ]);
  });

  test('merges overlapping and adjacent ranges', () => {
    expect(gopRanges(frames('K...K...'), [1, 3, 4, 5])).toEqual([[0, 5]]);
  });

  test('skips frames without a preceding key frame', () => {
    expect(gopRanges(frames('..K..'), [0, 1, 3])).toEqual([[2, 3]]);
  });

  test('skips out of range indices', () => {
    expect(gopRanges(frames('K.'), [-1, 1, 2])).toEqual([[0, 1]]);
  });
});

describe('VideoFramePlayer cancellation and deduplication', () => {
  afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });

  function installFakeWebCodecs() {
    const decoders: Array<{
      closed: boolean;
      flushed: number;
      resolveFlush: () => void;
      emitFrame: () => void;
    }> = [];

    class FakeEncodedVideoChunk {
      constructor(readonly init: EncodedVideoChunkInit) {}
    }

    class FakeVideoDecoder {
      static isConfigSupported() {
        return Promise.resolve({supported: true});
      }
      state: CodecState = 'unconfigured';
      decodeQueueSize = 0;
      private readonly record: (typeof decoders)[number];

      constructor(private readonly init: VideoDecoderInit) {
        this.record = {
          closed: false,
          flushed: 0,
          resolveFlush: () => {},
          emitFrame: () => {
            this.init.output({
              displayWidth: 100,
              displayHeight: 200,
              codedWidth: 100,
              codedHeight: 200,
              close: () => {},
            } as unknown as VideoFrame);
          },
        };
        decoders.push(this.record);
      }

      configure() {
        this.state = 'configured';
      }

      decode() {}

      flush() {
        this.record.flushed++;
        if (this.state === 'closed') {
          return Promise.reject(new Error('closed'));
        }
        return new Promise<void>((resolve, reject) => {
          this.record.resolveFlush = () => {
            if (this.state === 'closed') reject(new Error('closed'));
            else resolve();
          };
        });
      }

      close() {
        this.state = 'closed';
        this.record.closed = true;
        this.record.resolveFlush();
      }
    }

    vi.stubGlobal('EncodedVideoChunk', FakeEncodedVideoChunk);
    vi.stubGlobal('VideoDecoder', FakeVideoDecoder);
    return decoders;
  }

  function makeLoadedPlayer() {
    const trash = new DisposableStack();
    const trace = {
      trash,
      engine: {
        query: () =>
          Promise.resolve({
            iter: () => {
              let done = false;
              return {
                id: 0,
                data: new Uint8Array([1, 2, 3]),
                valid: () => !done,
                next: () => {
                  done = true;
                },
              };
            },
          }),
      },
      selection: {selectTrackEvent: () => {}},
    } as unknown as Trace;

    const player = new VideoFramePlayer(trace, '/video_frames/0', 0);
    // Pre-seed loaded stream metadata via a controlled loadFrames mock.
    vi.spyOn(
      player as unknown as {ensureFramesLoaded: () => Promise<void>},
      'ensureFramesLoaded',
    ).mockImplementation(async () => {
      const internal = player as unknown as {
        framesLoaded: boolean;
        configId: number;
        codecString: string;
      };
      if (!internal.framesLoaded) {
        internal.configId = 0;
        internal.codecString = 'avc1.64001f';
        player.frames = [
          {id: 10, ts: 0n, frameNumber: 0, isKey: true, ptsUs: 0},
          {id: 11, ts: 16n, frameNumber: 1, isKey: true, ptsUs: 16},
        ];
        internal.framesLoaded = true;
      }
    });
    return {player, trash};
  }

  test('superseding seek immediately closes the previous VideoDecoder and deduplicates attachCanvas', async () => {
    const decoders = installFakeWebCodecs();
    const drawImage = vi.fn();
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
      drawImage,
    } as unknown as CanvasRenderingContext2D);

    const {player} = makeLoadedPlayer();
    await player.ensureFramesLoaded();

    // Start first seek (kept pending inside decoder.flush()).
    const seek1 = player.seek(10);
    await vi.waitFor(() => expect(decoders).toHaveLength(1));

    // Mounting the preview canvas while seek1 is already in flight must NOT
    // spawn a duplicate VideoDecoder.
    const canvas = document.createElement('canvas');
    player.attachCanvas(canvas);
    expect(decoders).toHaveLength(1);
    expect(decoders[0]!.closed).toBe(false);

    // Clicking another frame supersedes seek1 and immediately closes decoder 0.
    const seek2 = player.seek(11);
    await vi.waitFor(() => expect(decoders[0]!.closed).toBe(true));
    await vi.waitFor(() => expect(decoders).toHaveLength(2));

    decoders[1]!.emitFrame();
    decoders[1]!.resolveFlush();
    await Promise.all([seek1, seek2]);

    expect(drawImage).toHaveBeenCalledTimes(1);
    expect(decoders[1]!.closed).toBe(true);
  });

  test('decodeFrames closes decoder when CancellationSignal is cancelled', async () => {
    const decoders = installFakeWebCodecs();
    const {player} = makeLoadedPlayer();
    const signal = {isCancelled: false};

    const p = player.decodeFrames([0], () => {}, signal);
    await vi.waitFor(() => expect(decoders).toHaveLength(1));

    signal.isCancelled = true;
    decoders[0]!.emitFrame();
    await p;
    expect(decoders[0]!.closed).toBe(true);
  });

  test('decodeFrameImage cancels during hover debounce without opening a VideoDecoder', async () => {
    const decoders = installFakeWebCodecs();
    const {player} = makeLoadedPlayer();
    const signal = {isCancelled: false};

    const p = player.decodeFrameImage(10, signal);
    signal.isCancelled = true;
    expect(await p).toBeUndefined();
    expect(decoders).toHaveLength(0);
  });

  test('drawVideoFrame uses copyTo + putImageData and falls back to drawImage', async () => {
    const putImageData = vi.fn();
    const drawImage = vi.fn();
    const ctx = {
      createImageData: (w: number, h: number) => ({
        data: new Uint8ClampedArray(w * h * 4),
      }),
      putImageData,
      drawImage,
    } as unknown as CanvasRenderingContext2D;

    const frameWithCopyTo = {
      codedWidth: 2,
      codedHeight: 2,
      displayWidth: 2,
      displayHeight: 2,
      allocationSize: () => 16,
      copyTo: async (dst: Uint8Array) => {
        dst.set([
          10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 1, 2, 3, 255,
        ]);
        return [{offset: 0, stride: 8}];
      },
    } as unknown as VideoFrame;

    await drawVideoFrame(ctx, frameWithCopyTo, 4, 8, 1, 1);
    expect(putImageData).toHaveBeenCalledTimes(1);
    expect(drawImage).not.toHaveBeenCalled();

    const frameWithoutCopyTo = {
      codedWidth: 2,
      codedHeight: 2,
    } as unknown as VideoFrame;
    await drawVideoFrame(ctx, frameWithoutCopyTo, 0, 0, 2, 2);
    expect(drawImage).toHaveBeenCalledTimes(1);
  });
});
