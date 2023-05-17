// Copyright (C) 2023 The Android Open Source Project
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

import {
  BigintMath as BIM,
} from './bigint_math';

describe('BigIntMath.bitCeil', () => {
  it('rounds powers of 2 to themselves', () => {
    expect(BIM.bitCeil(1n)).toBe(1n);
    expect(BIM.bitCeil(2n)).toBe(2n);
    expect(BIM.bitCeil(4n)).toBe(4n);
    expect(BIM.bitCeil(4294967296n)).toBe(4294967296n);
    expect(BIM.bitCeil(2305843009213693952n)).toBe(2305843009213693952n);
  });

  it('rounds non powers of 2 up to nearest power of 2', () => {
    expect(BIM.bitCeil(3n)).toBe(4n);
    expect(BIM.bitCeil(11n)).toBe(16n);
    expect(BIM.bitCeil(33n)).toBe(64n);
    expect(BIM.bitCeil(63n)).toBe(64n);
    expect(BIM.bitCeil(1234567890123456789n)).toBe(2305843009213693952n);
  });

  it('rounds 0 or negative values up to 1', () => {
    expect(BIM.bitCeil(0n)).toBe(1n);
    expect(BIM.bitCeil(-123n)).toBe(1n);
  });
});

describe('BigIntMath.bigFloor', () => {
  it('rounds powers of 2 to themselves', () => {
    expect(BIM.bitFloor(1n)).toBe(1n);
    expect(BIM.bitFloor(2n)).toBe(2n);
    expect(BIM.bitFloor(4n)).toBe(4n);
    expect(BIM.bitFloor(4294967296n)).toBe(4294967296n);
    expect(BIM.bitFloor(2305843009213693952n)).toBe(2305843009213693952n);
  });

  it('rounds non powers of 2 down to nearest power of 2', () => {
    expect(BIM.bitFloor(3n)).toBe(2n);
    expect(BIM.bitFloor(11n)).toBe(8n);
    expect(BIM.bitFloor(33n)).toBe(32n);
    expect(BIM.bitFloor(63n)).toBe(32n);
    expect(BIM.bitFloor(1234567890123456789n)).toBe(1152921504606846976n);
  });

  it('rounds 0 or negative values up to 1', () => {
    expect(BIM.bitFloor(0n)).toBe(1n);
    expect(BIM.bitFloor(-123n)).toBe(1n);
  });
});

describe('quantize', () => {
  it('should quantize a number to the nearest multiple of a stepsize', () => {
    expect(BIM.quantizeFloor(10n, 2n)).toEqual(10n);
    expect(BIM.quantizeFloor(11n, 2n)).toEqual(10n);
    expect(BIM.quantizeFloor(12n, 2n)).toEqual(12n);
    expect(BIM.quantizeFloor(13n, 2n)).toEqual(12n);

    expect(BIM.quantizeFloor(9n, 4n)).toEqual(8n);
    expect(BIM.quantizeFloor(10n, 4n)).toEqual(8n);
    expect(BIM.quantizeFloor(11n, 4n)).toEqual(8n);
    expect(BIM.quantizeFloor(12n, 4n)).toEqual(12n);
    expect(BIM.quantizeFloor(13n, 4n)).toEqual(12n);
  });

  it('should return value if stepsize is smaller than 1', () => {
    expect(BIM.quantizeFloor(123n, 0n)).toEqual(123n);
    expect(BIM.quantizeFloor(123n, -10n)).toEqual(123n);
  });
});

describe('quantizeRound', () => {
  it('should quantize a number to the nearest multiple of a stepsize', () => {
    expect(BIM.quantize(0n, 2n)).toEqual(0n);
    expect(BIM.quantize(1n, 2n)).toEqual(2n);
    expect(BIM.quantize(2n, 2n)).toEqual(2n);
    expect(BIM.quantize(3n, 2n)).toEqual(4n);
    expect(BIM.quantize(4n, 2n)).toEqual(4n);

    expect(BIM.quantize(0n, 3n)).toEqual(0n);
    expect(BIM.quantize(1n, 3n)).toEqual(0n);
    expect(BIM.quantize(2n, 3n)).toEqual(3n);
    expect(BIM.quantize(3n, 3n)).toEqual(3n);
    expect(BIM.quantize(4n, 3n)).toEqual(3n);
    expect(BIM.quantize(5n, 3n)).toEqual(6n);
    expect(BIM.quantize(6n, 3n)).toEqual(6n);
  });

  it('should return value if stepsize is smaller than 1', () => {
    expect(BIM.quantize(123n, 0n)).toEqual(123n);
    expect(BIM.quantize(123n, -10n)).toEqual(123n);
  });
});

describe('max', () => {
  it('should return the greater of two numbers', () => {
    expect(BIM.max(5n, 8n)).toEqual(8n);
    expect(BIM.max(3n, 7n)).toEqual(7n);
    expect(BIM.max(6n, 6n)).toEqual(6n);
    expect(BIM.max(-7n, -12n)).toEqual(-7n);
  });
});

describe('min', () => {
  it('should return the smaller of two numbers', () => {
    expect(BIM.min(5n, 8n)).toEqual(5n);
    expect(BIM.min(3n, 7n)).toEqual(3n);
    expect(BIM.min(6n, 6n)).toEqual(6n);
    expect(BIM.min(-7n, -12n)).toEqual(-12n);
  });
});

describe('popcount', () => {
  it('should return the number of set bits in an integer', () => {
    expect(BIM.popcount(0n)).toBe(0);
    expect(BIM.popcount(1n)).toBe(1);
    expect(BIM.popcount(2n)).toBe(1);
    expect(BIM.popcount(3n)).toBe(2);
    expect(BIM.popcount(4n)).toBe(1);
    expect(BIM.popcount(5n)).toBe(2);
    expect(BIM.popcount(3462151285050974216n)).toBe(10);
  });

  it('should throw when presented with a negative integer', () => {
    expect(() => BIM.popcount(-1n))
        .toThrowError('Can\'t get popcount of negative number -1');
  });
});
