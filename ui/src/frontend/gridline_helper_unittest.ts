// Copyright (C) 2018 The Android Open Source Project
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

import {TimeSpan} from '../common/time';

import {getStepSize, Tick, TickGenerator, TickType} from './gridline_helper';
import {TimeScale} from './time_scale';

const pattern1 = '|....:....';
const pattern2 = '|.:.';
const pattern5 = '|....';
const timeScale = new TimeScale(new TimeSpan(0, 1), [1, 2]);

test('gridline helper to have sensible step sizes', () => {
  expect(getStepSize(10, 14)).toEqual([1, pattern1]);
  expect(getStepSize(30, 14)).toEqual([5, pattern5]);
  expect(getStepSize(60, 14)).toEqual([5, pattern5]);
  expect(getStepSize(100, 14)).toEqual([10, pattern1]);

  expect(getStepSize(10, 21)).toEqual([0.5, pattern5]);
  expect(getStepSize(30, 21)).toEqual([2, pattern2]);
  expect(getStepSize(60, 21)).toEqual([5, pattern5]);
  expect(getStepSize(100, 21)).toEqual([5, pattern5]);

  expect(getStepSize(10, 3)).toEqual([5, pattern5]);
  expect(getStepSize(30, 3)).toEqual([10, pattern1]);
  expect(getStepSize(60, 3)).toEqual([20, pattern2]);
  expect(getStepSize(100, 3)).toEqual([50, pattern5]);

  expect(getStepSize(800, 4)).toEqual([200, pattern2]);
});

test('gridline helper to scale to very small and very large values', () => {
  expect(getStepSize(.01, 14)).toEqual([.001, pattern1]);
  expect(getStepSize(10000, 14)).toEqual([1000, pattern1]);
});

test('gridline helper to always return a reasonable number of steps', () => {
  for (let i = 1; i <= 1000; i++) {
    const [stepSize, _] = getStepSize(i, 14);
    expect(Math.round(i / stepSize)).toBeGreaterThanOrEqual(6);
    expect(Math.round(i / stepSize)).toBeLessThanOrEqual(14);
  }
});

describe('TickGenerator with range 0.0-1.0 and room for 2 labels', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.0, 1.0);
    const timeScale = new TimeScale(timeSpan, [0, 200]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should produce major ticks at 0.5s and minor ticks at 0.1s starting at 0',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 0.0},
         {type: TickType.MINOR, time: 0.1},
         {type: TickType.MINOR, time: 0.2},
         {type: TickType.MINOR, time: 0.3},
         {type: TickType.MINOR, time: 0.4},
         {type: TickType.MAJOR, time: 0.5},
         {type: TickType.MINOR, time: 0.6},
         {type: TickType.MINOR, time: 0.7},
         {type: TickType.MINOR, time: 0.8},
         {type: TickType.MINOR, time: 0.9},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 1 decimal place for labels', () => {
    expect(tickGen!.digits).toEqual(1);
  });
});

describe('TickGenerator with range 0.3-1.3 and room for 2 labels', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.3, 1.3);
    const timeScale = new TimeScale(timeSpan, [0, 200]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should produce major ticks at 0.5s and minor ticks at 0.1s starting at 0',
     () => {
       const expected = [
         {type: TickType.MINOR, time: 0.3},
         {type: TickType.MINOR, time: 0.4},
         {type: TickType.MAJOR, time: 0.5},
         {type: TickType.MINOR, time: 0.6},
         {type: TickType.MINOR, time: 0.7},
         {type: TickType.MINOR, time: 0.8},
         {type: TickType.MINOR, time: 0.9},
         {type: TickType.MAJOR, time: 1.0},
         {type: TickType.MINOR, time: 1.1},
         {type: TickType.MINOR, time: 1.2},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 1 decimal place for labels', () => {
    expect(tickGen!.digits).toEqual(1);
  });
});

describe('TickGenerator with range 0.0-0.2 and room for 1 label', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.0, 0.2);
    const timeScale = new TimeScale(timeSpan, [0, 100]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should produce major ticks at 0.2s and minor ticks at 0.1s starting at 0',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 0.0},
         {type: TickType.MINOR, time: 0.05},
         {type: TickType.MEDIUM, time: 0.1},
         {type: TickType.MINOR, time: 0.15},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 1 decimal place for labels', () => {
    expect(tickGen!.digits).toEqual(1);
  });
});

describe('TickGenerator with range 0.0-0.1 and room for 1 label', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.0, 0.1);
    const timeScale = new TimeScale(timeSpan, [0, 100]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should produce major ticks at 0.1s & minor ticks at 0.02s starting at 0',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 0.0},
         {type: TickType.MINOR, time: 0.01},
         {type: TickType.MINOR, time: 0.02},
         {type: TickType.MINOR, time: 0.03},
         {type: TickType.MINOR, time: 0.04},
         {type: TickType.MEDIUM, time: 0.05},
         {type: TickType.MINOR, time: 0.06},
         {type: TickType.MINOR, time: 0.07},
         {type: TickType.MINOR, time: 0.08},
         {type: TickType.MINOR, time: 0.09},
       ];
       const actual = Array.from(tickGen!);
       expect(tickGen!.digits).toEqual(1);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 1 decimal place for labels', () => {
    expect(tickGen!.digits).toEqual(1);
  });
});

describe('TickGenerator with a very small timespan', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.0, 1e-9);
    const timeScale = new TimeScale(timeSpan, [0, 100]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should generate minor ticks at 2e-10s and one major tick at the start',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 0.0},
         {type: TickType.MINOR, time: 1e-10},
         {type: TickType.MINOR, time: 2e-10},
         {type: TickType.MINOR, time: 3e-10},
         {type: TickType.MINOR, time: 4e-10},
         {type: TickType.MEDIUM, time: 5e-10},
         {type: TickType.MINOR, time: 6e-10},
         {type: TickType.MINOR, time: 7e-10},
         {type: TickType.MINOR, time: 8e-10},
         {type: TickType.MINOR, time: 9e-10},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 9 decimal places for labels', () => {
    expect(tickGen!.digits).toEqual(9);
  });
});

describe('TickGenerator with a very large timespan', () => {
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(0.0, 1e9);
    const timeScale = new TimeScale(timeSpan, [0, 100]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should generate minor ticks at 2e8 and one major tick at the start',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 0.0},
         {type: TickType.MINOR, time: 1e8},
         {type: TickType.MINOR, time: 2e8},
         {type: TickType.MINOR, time: 3e8},
         {type: TickType.MINOR, time: 4e8},
         {type: TickType.MEDIUM, time: 5e8},
         {type: TickType.MINOR, time: 6e8},
         {type: TickType.MINOR, time: 7e8},
         {type: TickType.MINOR, time: 8e8},
         {type: TickType.MINOR, time: 9e8},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 0 decimal places for labels', () => {
    expect(tickGen!.digits).toEqual(0);
  });
});

describe('TickGenerator where the timespan has a dynamic range of 1e12', () => {
  // This is the equivalent of zooming in to the nanosecond level, 1000 seconds
  // into a trace Note: this is about the limit of what this generator can
  // handle.
  let tickGen: TickGenerator|undefined = undefined;
  beforeAll(() => {
    const timeSpan = new TimeSpan(1000, 1000.000000001);
    const timeScale = new TimeScale(timeSpan, [0, 100]);
    tickGen = new TickGenerator(timeScale, {minLabelPx: 100});
  });
  it('should generate minor ticks at 1e-10s and one major tick at the start',
     () => {
       const expected = [
         {type: TickType.MAJOR, time: 1000.0000000000},
         {type: TickType.MINOR, time: 1000.0000000001},
         {type: TickType.MINOR, time: 1000.0000000002},
         {type: TickType.MINOR, time: 1000.0000000003},
         {type: TickType.MINOR, time: 1000.0000000004},
         {type: TickType.MEDIUM, time: 1000.0000000005},
         {type: TickType.MINOR, time: 1000.0000000006},
         {type: TickType.MINOR, time: 1000.0000000007},
         {type: TickType.MINOR, time: 1000.0000000008},
         {type: TickType.MINOR, time: 1000.0000000009},
       ];
       const actual = Array.from(tickGen!);
       expectTicksEqual(actual, expected);
     });
  it('should tell us to use 9 decimal places for labels', () => {
    expect(tickGen!.digits).toEqual(9);
  });
});

describe(
    'TickGenerator where the timespan has a ridiculously huge dynamic range',
    () => {
      // We don't expect this to work, just wanna make sure it doesn't crash or
      // get stuck
      it('should not crash or get stuck in an infinite loop', () => {
        const timeSpan = new TimeSpan(1000, 1000.000000000001);
        const timeScale = new TimeScale(timeSpan, [0, 100]);
        new TickGenerator(timeScale);
      });
    });

describe(
    'TickGenerator where the timespan has a ridiculously huge dynamic range',
    () => {
      // We don't expect this to work, just wanna make sure it doesn't crash or
      // get stuck
      it('should not crash or get stuck in an infinite loop', () => {
        const timeSpan = new TimeSpan(1000, 1000.000000000001);
        const timeScale = new TimeScale(timeSpan, [0, 100]);
        new TickGenerator(timeScale);
      });
    });

test('TickGenerator constructed with a 0 width throws an error', () => {
  expect(() => {
    const timeScale = new TimeScale(new TimeSpan(0.0, 1.0), [0, 0]);
    new TickGenerator(timeScale);
  }).toThrow(Error);
});

test(
    'TickGenerator constructed with desiredPxPerStep of 0 throws an error',
    () => {
      expect(() => {
        new TickGenerator(timeScale, {minLabelPx: 0});
      }).toThrow(Error);
    });

test('TickGenerator constructed with a 0 duration throws an error', () => {
  expect(() => {
    const timeScale = new TimeScale(new TimeSpan(0.0, 0.0), [0, 1]);
    new TickGenerator(timeScale);
  }).toThrow(Error);
});

function expectTicksEqual(actual: Tick[], expected: any[]) {
  // TODO(stevegolton) We could write a custom matcher for this; this approach
  // produces cryptic error messages.
  expect(actual.length).toEqual(expected.length);
  for (let i = 0; i < actual.length; ++i) {
    const ex = expected[i];
    const ac = actual[i];
    expect(ac.type).toEqual(ex.type);
    expect(ac.time).toBeCloseTo(ex.time, 9);
  }
}
