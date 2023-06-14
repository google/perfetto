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

import {BigintMath as BIM} from './bigint_math';

describe('BigIntMath', () => {
  describe('bitCeil', () => {
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

  describe('bitFloor', () => {
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

  describe('log2', () => {
    it('calcs exact powers of 2', () => {
      expect(BIM.log2(1n)).toBe(0);
      expect(BIM.log2(2n)).toBe(1);
      expect(BIM.log2(4n)).toBe(2);
      expect(BIM.log2(4294967296n)).toBe(32);
      expect(BIM.log2(2305843009213693952n)).toBe(61);
    });

    it('rounds non powers of 2 down to nearest power of 2', () => {
      expect(BIM.log2(3n)).toBe(1);
      expect(BIM.log2(11n)).toBe(3);
      expect(BIM.log2(33n)).toBe(5);
      expect(BIM.log2(63n)).toBe(5);
      expect(BIM.log2(1234567890123456789n)).toBe(60);
    });

    it('returns 0 for 0n negative numbers', () => {
      expect(BIM.log2(0n)).toBe(0);
      expect(BIM.log2(-123n)).toBe(0);
    });
  });

  describe('quant', () => {
    it('should round an int to the nearest multiple of a stepsize', () => {
      expect(BIM.quant(0n, 2n)).toEqual(0n);
      expect(BIM.quant(1n, 2n)).toEqual(2n);
      expect(BIM.quant(2n, 2n)).toEqual(2n);
      expect(BIM.quant(3n, 2n)).toEqual(4n);
      expect(BIM.quant(4n, 2n)).toEqual(4n);

      expect(BIM.quant(0n, 3n)).toEqual(0n);
      expect(BIM.quant(1n, 3n)).toEqual(0n);
      expect(BIM.quant(2n, 3n)).toEqual(3n);
      expect(BIM.quant(3n, 3n)).toEqual(3n);
      expect(BIM.quant(4n, 3n)).toEqual(3n);
      expect(BIM.quant(5n, 3n)).toEqual(6n);
      expect(BIM.quant(6n, 3n)).toEqual(6n);
    });

    it('should return value if stepsize is smaller than 1', () => {
      expect(BIM.quant(123n, 0n)).toEqual(123n);
      expect(BIM.quant(123n, -10n)).toEqual(123n);
    });
  });

  describe('quantFloor', () => {
    it('should quantize a number to the nearest multiple of a stepsize', () => {
      expect(BIM.quantFloor(10n, 2n)).toEqual(10n);
      expect(BIM.quantFloor(11n, 2n)).toEqual(10n);
      expect(BIM.quantFloor(12n, 2n)).toEqual(12n);
      expect(BIM.quantFloor(13n, 2n)).toEqual(12n);

      expect(BIM.quantFloor(9n, 4n)).toEqual(8n);
      expect(BIM.quantFloor(10n, 4n)).toEqual(8n);
      expect(BIM.quantFloor(11n, 4n)).toEqual(8n);
      expect(BIM.quantFloor(12n, 4n)).toEqual(12n);
      expect(BIM.quantFloor(13n, 4n)).toEqual(12n);
    });

    it('should return value if stepsize is smaller than 1', () => {
      expect(BIM.quantFloor(123n, 0n)).toEqual(123n);
      expect(BIM.quantFloor(123n, -10n)).toEqual(123n);
    });
  });

  describe('quantCeil', () => {
    it('should round an int up to the nearest multiple of a stepsize', () => {
      expect(BIM.quantCeil(10n, 2n)).toEqual(10n);
      expect(BIM.quantCeil(11n, 2n)).toEqual(12n);
      expect(BIM.quantCeil(12n, 2n)).toEqual(12n);
      expect(BIM.quantCeil(13n, 2n)).toEqual(14n);

      expect(BIM.quantCeil(9n, 4n)).toEqual(12n);
      expect(BIM.quantCeil(10n, 4n)).toEqual(12n);
      expect(BIM.quantCeil(11n, 4n)).toEqual(12n);
      expect(BIM.quantCeil(12n, 4n)).toEqual(12n);
      expect(BIM.quantCeil(13n, 4n)).toEqual(16n);
    });

    it('should return value if stepsize is smaller than 1', () => {
      expect(BIM.quantCeil(123n, 0n)).toEqual(123n);
      expect(BIM.quantCeil(123n, -10n)).toEqual(123n);
    });
  });

  describe('quantRound', () => {
    it('should quantize a number to the nearest multiple of a stepsize', () => {
      expect(BIM.quant(0n, 2n)).toEqual(0n);
      expect(BIM.quant(1n, 2n)).toEqual(2n);
      expect(BIM.quant(2n, 2n)).toEqual(2n);
      expect(BIM.quant(3n, 2n)).toEqual(4n);
      expect(BIM.quant(4n, 2n)).toEqual(4n);

      expect(BIM.quant(0n, 3n)).toEqual(0n);
      expect(BIM.quant(1n, 3n)).toEqual(0n);
      expect(BIM.quant(2n, 3n)).toEqual(3n);
      expect(BIM.quant(3n, 3n)).toEqual(3n);
      expect(BIM.quant(4n, 3n)).toEqual(3n);
      expect(BIM.quant(5n, 3n)).toEqual(6n);
      expect(BIM.quant(6n, 3n)).toEqual(6n);
    });

    it('should return value if stepsize is smaller than 1', () => {
      expect(BIM.quant(123n, 0n)).toEqual(123n);
      expect(BIM.quant(123n, -10n)).toEqual(123n);
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

  describe('ratio', () => {
    it('should return ratio as number', () => {
      expect(BIM.ratio(0n, 1n)).toBeCloseTo(0);
      expect(BIM.ratio(1n, 1n)).toBeCloseTo(1);
      expect(BIM.ratio(1n, 2n)).toBeCloseTo(0.5);
      expect(BIM.ratio(1n, 100n)).toBeCloseTo(0.01);
      expect(
          BIM.ratio(
              987654321098765432109876543210n, 123456789012345678901234567890n))
          .toBeCloseTo(8);
      expect(
          BIM.ratio(
              123456789012345678901234567890n, 987654321098765432109876543210n))
          .toBeCloseTo(0.125, 3);
    });
  });

  describe('abs', () => {
    test('should return the absolute value of a positive BigInt', () => {
      const result = BIM.abs(12345678901234567890n);
      expect(result).toEqual(12345678901234567890n);
    });

    test('should return the absolute value of a negative BigInt', () => {
      const result = BIM.abs(-12345678901234567890n);
      expect(result).toEqual(12345678901234567890n);
    });

    test('should return the absolute value of zero', () => {
      const result = BIM.abs(0n);
      expect(result).toEqual(0n);
    });
  });
});
