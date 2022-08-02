// Copyright (C) 2021 The Android Open Source Project
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

import * as protobuf from 'protobufjs/minimal';
import {assertTrue} from '../base/logging';

import {ProtoRingBuffer} from './proto_ring_buffer';

let seed = 1;

// For reproducibility.
function Rnd(max: number) {
  seed = seed * 16807 % 2147483647;
  return seed % max;
}

function MakeProtoMessage(fieldId: number, len: number) {
  const writer = protobuf.Writer.create();
  const tag = (fieldId << 3) | 2;
  assertTrue(tag < 0x80 && (tag & 7) === 2);
  writer.uint32(tag);
  const data = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    data[i] = 48 + ((fieldId + i) % 73);
  }
  writer.bytes(data);
  const res = writer.finish();
  // For whatever reason the object returned by protobufjs' Writer cannot be
  // directly .toEqual()-ed with Uint8Arrays.
  const buf = new Uint8Array(res.length);
  buf.set(res);
  return buf;
}

test('ProtoRingBufferTest.Fastpath', () => {
  const buf = new ProtoRingBuffer();

  for (let rep = 0; rep < 3; rep++) {
    let inputBuf = MakeProtoMessage(1, 32);
    buf.append(inputBuf);
    let msg = buf.readMessage();
    expect(msg).toBeDefined();
    expect(msg).toBeInstanceOf(Uint8Array);
    expect(msg!.length).toBe(32);

    // subarray(2) is to strip the proto preamble. The returned buffer starts at
    // the start of the payload.
    expect(msg).toEqual(inputBuf.subarray(2));

    // When we hit the fastpath, the returned message should be a subarray of
    // the same ArrayBuffer passed to append.
    expect(msg!.buffer).toBe(inputBuf.buffer);

    inputBuf = MakeProtoMessage(2, 32);
    buf.append(inputBuf.subarray(0, 13));
    expect(buf.readMessage()).toBeUndefined();
    buf.append(inputBuf.subarray(13));
    msg = buf.readMessage();
    expect(msg).toBeDefined();
    expect(msg).toBeInstanceOf(Uint8Array);
    expect(msg).toEqual(inputBuf.subarray(2));
    expect(msg!.buffer !== inputBuf.buffer).toBeTruthy();
  }
});

test('ProtoRingBufferTest.CoalescingStream', () => {
  const buf = new ProtoRingBuffer();

  const mergedBuf = new Uint8Array(612);
  const expected = new Array<Uint8Array>();
  for (let i = 1, pos = 0; i <= 6; i++) {
    const msg = MakeProtoMessage(i, 100);
    expected.push(msg);
    mergedBuf.set(msg, pos);
    pos += msg.length;
  }

  const fragLens = [120, 20, 471, 1];
  let fragSum = 0;
  fragLens.map((fragLen) => {
    buf.append(mergedBuf.subarray(fragSum, fragSum + fragLen));
    fragSum += fragLen;
    for (;;) {
      const msg = buf.readMessage();
      if (msg === undefined) break;
      const exp = expected.shift();
      expect(exp).toBeDefined();
      expect(msg).toEqual(exp!.subarray(-1 * msg.length));
    }
  });
  expect(expected.length).toEqual(0);
});


test('ProtoRingBufferTest.RandomSizes', () => {
  const buf = new ProtoRingBuffer();
  const kNumMsg = 100;
  const mergedBuf = new Uint8Array(1024 * 1024 * 32);
  const expectedLengths = [];
  let mergedLen = 0;
  for (let i = 0; i < kNumMsg; i++) {
    const fieldId = 1 + Rnd(15);  // We support only one byte tag.
    const rndVal = Rnd(1024);
    let len = 1 + rndVal;
    if ((rndVal % 100) < 5) {
      len *= 1000;
    }
    const msg = MakeProtoMessage(fieldId, len);
    assertTrue(mergedBuf.length >= mergedLen + msg.length);
    expectedLengths.push(len);
    mergedBuf.set(msg, mergedLen);
    mergedLen += msg.length;
  }

  for (let fragSum = 0; fragSum < mergedLen; /**/) {
    let fragLen = 1 + Rnd(1024 * 32);
    fragLen = Math.min(fragLen, mergedLen - fragSum);
    buf.append(mergedBuf.subarray(fragSum, fragSum + fragLen));
    fragSum += fragLen;
    for (;;) {
      const msg = buf.readMessage();
      if (msg === undefined) break;
      const expLen = expectedLengths.shift();
      expect(expLen).toBeDefined();
      expect(msg.length).toEqual(expLen);
    }
  }
  expect(expectedLengths.length).toEqual(0);
});
