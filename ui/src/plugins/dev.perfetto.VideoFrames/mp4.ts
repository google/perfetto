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

// Remuxes trace H.264/H.265 access units into an .mp4 without re-encoding.
// mediabunny reads the parameter sets from the first (Annex-B) access unit to
// build the avcC/hvcC record and length-prefixes the rest; we supply the dims.

import {
  ALL_FORMATS,
  BufferSource,
  BufferTarget,
  Conversion,
  EncodedPacket,
  EncodedVideoPacketSource,
  Input,
  Mp4OutputFormat,
  Output,
} from 'mediabunny';

export interface EncodedFrame {
  readonly data: Uint8Array; // one Annex-B access unit
  readonly isKey: boolean;
  readonly pts: number; // microseconds
}

type Mp4Codec = 'avc' | 'hevc';

// avc1/avc3 -> H.264, hev1/hvc1 -> H.265; undefined if not remuxable.
function mp4Codec(codecString: string | undefined): Mp4Codec | undefined {
  if (codecString?.startsWith('avc')) return 'avc';
  if (codecString?.startsWith('hev') || codecString?.startsWith('hvc')) {
    return 'hevc';
  }
  return undefined;
}

export async function muxToMp4(
  codecString: string,
  configAnnexB: Uint8Array,
  frames: ReadonlyArray<EncodedFrame>,
  width: number,
  height: number,
  fallbackFps: number,
): Promise<Uint8Array<ArrayBuffer>> {
  const codec = mp4Codec(codecString);
  if (codec === undefined) {
    throw new Error(`cannot mux codec '${codecString}' into mp4`);
  }
  if (frames.length === 0) throw new Error('no frames to mux');
  // mediabunny timestamps are in seconds; frame pts are microseconds.
  const base = frames[0].pts;
  const tailDur = 1 / (fallbackFps || 30); // last frame has no successor

  const output = new Output({
    format: new Mp4OutputFormat({fastStart: 'in-memory'}),
    target: new BufferTarget(),
  });
  const source = new EncodedVideoPacketSource(codec);
  output.addVideoTrack(source);
  await output.start();

  for (let i = 0; i < frames.length; i++) {
    const f = frames[i];
    const next = frames[i + 1];
    const duration =
      next !== undefined && next.pts > f.pts
        ? (next.pts - f.pts) / 1_000_000
        : tailDur;
    const timestamp = (f.pts - base) / 1_000_000;
    // Prepend the out-of-band parameter sets to the first access unit; passing
    // no `description` tells mediabunny the packets are Annex-B.
    const data = i === 0 ? concat(configAnnexB, f.data) : f.data;
    await source.add(
      new EncodedPacket(data, f.isKey ? 'key' : 'delta', timestamp, duration),
      i === 0
        ? {
            decoderConfig: {
              codec: codecString,
              codedWidth: width,
              codedHeight: height,
            },
          }
        : undefined,
    );
  }
  await output.finalize();
  return new Uint8Array(output.target.buffer!);
}

// Re-encode `mp4` to keep only [startSec, endSec), relative to its own start.
// A region clip is muxed from the key frame enclosing the requested start, so
// it carries leading frames that decode but should not be shown; a mid-GOP cut
// like this can't be expressed by a copy, so mediabunny re-encodes from
// startSec. Used only when the requested start falls after the key frame -- a
// cut already on a key frame stays a lossless remux.
export async function trimMp4(
  mp4: Uint8Array,
  startSec: number,
  endSec?: number,
): Promise<Uint8Array<ArrayBuffer>> {
  const input = new Input({
    formats: ALL_FORMATS,
    source: new BufferSource(mp4),
  });
  const output = new Output({
    format: new Mp4OutputFormat({fastStart: 'in-memory'}),
    target: new BufferTarget(),
  });
  const trim =
    endSec !== undefined ? {start: startSec, end: endSec} : {start: startSec};
  const conversion = await Conversion.init({input, output, trim});
  await conversion.execute();
  return new Uint8Array(output.target.buffer!);
}

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}
