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

export class BigintMath {
  static INT64_MAX: bigint = (2n ** 63n) - 1n;
  static INT64_MIN: bigint = -(2n ** 63n);

  // Returns the smallest integral power of 2 that is not smaller than n.
  // If n is less than or equal to 0, returns 1.
  static bitCeil(n: bigint): bigint {
    let result = 1n;
    while (result < n) {
      result <<= 1n;
    }
    return result;
  };

  // Returns the largest integral power of 2 which is not greater than n.
  // If n is less than or equal to 0, returns 1.
  static bitFloor(n: bigint): bigint {
    let result = 1n;
    while ((result << 1n) <= n) {
      result <<= 1n;
    }
    return result;
  };

  // Returns the largest integral value x where 2^x is not greater than n.
  static log2(n: bigint): number {
    let result = 1n;
    let log2 = 0;
    while ((result << 1n) <= n) {
      result <<= 1n;
      ++log2;
    }
    return log2;
  }

  // Returns the integral multiple of step which is closest to n.
  // If step is less than or equal to 0, returns n.
  static quant(n: bigint, step: bigint): bigint {
    step = BigintMath.max(1n, step);
    const halfStep = step / 2n;
    return step * ((n + halfStep) / step);
  }

  // Returns the largest integral multiple of step which is not larger than n.
  // If step is less than or equal to 0, returns n.
  static quantFloor(n: bigint, step: bigint): bigint {
    step = BigintMath.max(1n, step);
    return step * (n / step);
  }

  // Returns the smallest integral multiple of step which is not smaller than n.
  // If step is less than or equal to 0, returns n.
  static quantCeil(n: bigint, step: bigint): bigint {
    step = BigintMath.max(1n, step);
    const remainder = n % step;
    if (remainder === 0n) {
      return n;
    }
    const quotient = n / step;
    return (quotient + 1n) * step;
  }

  // Returns the greater of a and b.
  static max(a: bigint, b: bigint): bigint {
    return a > b ? a : b;
  }

  // Returns the smaller of a and b.
  static min(a: bigint, b: bigint): bigint {
    return a < b ? a : b;
  }

  // Returns the number of 1 bits in n.
  static popcount(n: bigint): number {
    if (n < 0n) {
      throw Error(`Can\'t get popcount of negative number ${n}`);
    }
    let count = 0;
    while (n) {
      if (n & 1n) {
        ++count;
      }
      n >>= 1n;
    }
    return count;
  }

  // Return the ratio between two bigints as a number.
  static ratio(dividend: bigint, divisor: bigint): number {
    return Number(dividend) / Number(divisor);
  }

  // Calculates the absolute value of a n.
  static abs(n: bigint) {
    return n < 0n ? -1n * n : n;
  }
}
