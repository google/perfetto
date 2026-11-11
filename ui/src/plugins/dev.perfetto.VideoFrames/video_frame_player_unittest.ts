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
      emitFrame: (onClose?: () => void) => void;
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
          emitFrame: (onClose?: () => void) => {
            this.init.output({
              displayWidth: 100,
              displayHeight: 200,
              codedWidth: 100,
              codedHeight: 200,
              close: () => onClose?.(),
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

  test('drawVideoFrame copies pixels via a staging canvas, one frame at a time', async () => {
    class FakeImageData {
      readonly data: Uint8ClampedArray;
      constructor(
        readonly width: number,
        readonly height: number,
      ) {
        this.data = new Uint8ClampedArray(width * height * 4);
      }
    }
    vi.stubGlobal('ImageData', FakeImageData);
    const staged: number[] = [];
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue({
      putImageData: (img: FakeImageData) => staged.push(img.data[0]!),
    } as unknown as CanvasRenderingContext2D);
    const drawImage = vi.fn();
    const ctx = {drawImage} as unknown as CanvasRenderingContext2D;

    // Each frame fills itself with `value`; the first copy is the slowest, so
    // without serialization the second would overwrite the shared buffer
    // before the first is staged.
    const makeFrame = (value: number, delayMs: number) =>
      ({
        codedWidth: 4,
        codedHeight: 2,
        visibleRect: {width: 2, height: 2},
        copyTo: async (dst: Uint8ClampedArray) => {
          await new Promise((r) => setTimeout(r, delayMs));
          dst.fill(value);
          return [{offset: 0, stride: 8}];
        },
      }) as unknown as VideoFrame;

    await Promise.all([
      drawVideoFrame(ctx, makeFrame(1, 20), 0, 0, 1, 1),
      drawVideoFrame(ctx, makeFrame(2, 0), 1, 0, 1, 1),
    ]);
    expect(staged).toEqual([1, 2]);
    // Both are drawn from the staging canvas, scaled into place.
    expect(drawImage).toHaveBeenCalledTimes(2);
    expect(drawImage.mock.calls[0]![0]).toBeInstanceOf(HTMLCanvasElement);
    expect(drawImage.mock.calls[1]!.slice(1)).toEqual([0, 0, 2, 2, 1, 0, 1, 1]);
  });

  test('drawVideoFrame falls back to drawImage without copyTo', async () => {
    const drawImage = vi.fn();
    const ctx = {drawImage} as unknown as CanvasRenderingContext2D;
    const frame = {codedWidth: 2, codedHeight: 2} as unknown as VideoFrame;
    await drawVideoFrame(ctx, frame, 0, 0, 2, 2);
    expect(drawImage).toHaveBeenCalledWith(frame, 0, 0, 2, 2);
  });

  test('decodeFrames closes frames when onFrame throws', async () => {
    const decoders = installFakeWebCodecs();
    const {player} = makeLoadedPlayer();
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    let closed = 0;
    const p = player.decodeFrames([0], () => {
      throw new Error('boom');
    });
    await vi.waitFor(() => expect(decoders).toHaveLength(1));
    // Emit a frame whose close() we can observe.
    const record = decoders[0]!;
    record.emitFrame(() => closed++);
    record.resolveFlush();
    await p;
    expect(closed).toBe(1);
  });
});
