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
  c,
  cmpFromExpr,
  cmpFromSort,
  ConcreteEventSet,
  Direction,
  EmptyEventSet,
  EmptyKeySet,
  eq,
  Event,
  EventSet,
  isConcreteEventSet,
  isEmptyEventSet,
  KeySet,
  Num,
  Str,
  UntypedEvent,
  v,
} from './event_set';

describe('EventSet', () => {
  test('Event', () => {
    {
      const keyset: EmptyKeySet = {};
      const event: Event<typeof keyset> = {
        id: 'foo',
      };
      void event;
    } {
      const keyset = {
        'bar': Num,
      };
      const event: Event<typeof keyset> = {
        id: 'foo',
        bar: 42,
      };
      void event;
    }
  });

  describe('EmptyEventSet', () => {
    test('isEmpty', async () => {
      const events = EmptyEventSet.get();
      expect(await events.isEmpty()).toEqual(true);
      expect(await events.count()).toEqual(0);
    });

    test('isEmptyEventSet', () => {
      const events: EventSet<KeySet> = EmptyEventSet.get();
      expect(isEmptyEventSet(events)).toEqual(true);
    });

    test('materialise', async () => {
      const events: EventSet<KeySet> = EmptyEventSet.get();
      const materialised = await events.materialise({});

      expect(await materialised.isEmpty()).toEqual(true);
      expect(await materialised.count()).toEqual(0);
      expect(materialised.events).toEqual([]);
      expect(isConcreteEventSet(materialised)).toEqual(true);
    });

    test('union', async () => {
      const a: EventSet<KeySet> = EmptyEventSet.get();
      const b: EventSet<KeySet> = EmptyEventSet.get();

      const aUnionB = a.union(b);

      expect(await aUnionB.isEmpty()).toEqual(true);
      expect(await aUnionB.count()).toEqual(0);
    });

    test('intersect', async () => {
      const a: EventSet<KeySet> = EmptyEventSet.get();
      const b: EventSet<KeySet> = EmptyEventSet.get();

      const aIntersectB = a.intersect(b);

      expect(await aIntersectB.isEmpty()).toEqual(true);
      expect(await aIntersectB.count()).toEqual(0);
    });

    test('filter', async () => {
      const events: EventSet<KeySet> = EmptyEventSet.get();
      const filtered = await events.filter(c(true));

      expect(filtered).toBe(events);
      expect(await filtered.isEmpty()).toEqual(true);
      expect(await filtered.count()).toEqual(0);
    });

    test('sort', async () => {
      const events: EventSet<KeySet> = EmptyEventSet.get();
      const sorted = await events.sort({
        direction: Direction.ASC,
        expression: c(0),
      });

      expect(sorted).toBe(events);
      expect(await sorted.isEmpty()).toEqual(true);
      expect(await sorted.count()).toEqual(0);
    });
  });

  describe('ConcreteEventSet', () => {
    test('isEmpty', async () => {
      const event: Event<EmptyKeySet> = {
        id: 'foo',
      };
      const empty = new ConcreteEventSet<EmptyKeySet>({}, []);
      const events = new ConcreteEventSet<EmptyKeySet>({}, [event]);
      expect(await empty.isEmpty()).toEqual(true);
      expect(await empty.count()).toEqual(0);
      expect(await events.isEmpty()).toEqual(false);
      expect(await events.count()).toEqual(1);
    });

    test('isConcreteEventSet', () => {
      expect(isConcreteEventSet(new ConcreteEventSet<EmptyKeySet>({}, [])))
          .toEqual(true);
      expect(isConcreteEventSet(EmptyEventSet.get())).toEqual(false);
    });

    test('materialise', async () => {
      const keys = {
        num: Num,
        char: Str,
      };

      const a: Event<typeof keys> = {
        id: 'a',
        num: 97,
        char: 'a',
      };
      const b: Event<typeof keys> = {
        id: 'b',
        num: 98,
        char: 'b',
      };
      const d: Event<typeof keys> = {
        id: 'd',
        num: 100,
        char: 'd',
      };

      const events = new ConcreteEventSet(keys, [a, b, d]);

      expect((await events.materialise(keys)).events).toEqual([a, b, d]);
      expect((await events.materialise(keys, 1)).events).toEqual([b, d]);
      expect((await events.materialise(keys, 1, 1)).events).toEqual([b]);
      expect((await events.materialise(keys, 99)).events).toEqual([]);
      expect((await events.materialise(keys, 99, 0)).events).toEqual([]);
      expect((await events.materialise({num: Num})).events).toEqual([
        {id: 'a', num: 97},
        {id: 'b', num: 98},
        {id: 'd', num: 100},
      ]);
      expect((await events.materialise({char: Str}, 1, 1)).events).toEqual([
        {id: 'b', char: 'b'},
      ]);
    });

    test('union', async () => {
      const a: Event<EmptyKeySet> = {
        id: 'a',
      };
      const b: Event<EmptyKeySet> = {
        id: 'b',
      };
      const d: Event<EmptyKeySet> = {
        id: 'd',
      };

      const empty = EmptyEventSet.get();
      const justA = new ConcreteEventSet({}, [a]);
      const justB = new ConcreteEventSet({}, [b]);
      const justD = new ConcreteEventSet({}, [d]);

      const aAndB = justA.union(justB);
      const aAndA = justA.union(justA);
      const aAndD = justA.union(justD);
      const aAndBAndEmpty = aAndB.union(empty);
      const aAndDAndAAndB = aAndD.union(aAndB);

      expect((await aAndB.materialise({})).events).toEqual([a, b]);
      expect((await aAndA.materialise({})).events).toEqual([a]);
      expect((await aAndD.materialise({})).events).toEqual([a, d]);
      expect((await aAndBAndEmpty.materialise({})).events).toEqual([a, b]);
      expect((await aAndDAndAAndB.materialise({})).events).toEqual([a, d, b]);

      expect(await aAndB.isEmpty()).toEqual(false);
      expect(await aAndA.isEmpty()).toEqual(false);
      expect(await aAndD.isEmpty()).toEqual(false);
      expect(await aAndBAndEmpty.isEmpty()).toEqual(false);
      expect(await aAndDAndAAndB.isEmpty()).toEqual(false);

      expect(await aAndB.count()).toEqual(2);
      expect(await aAndA.count()).toEqual(1);
      expect(await aAndD.count()).toEqual(2);
      expect(await aAndBAndEmpty.count()).toEqual(2);
      expect(await aAndDAndAAndB.count()).toEqual(3);
    });

    test('intersection', async () => {
      const a: Event<EmptyKeySet> = {
        id: 'a',
      };
      const b: Event<EmptyKeySet> = {
        id: 'b',
      };
      const d: Event<EmptyKeySet> = {
        id: 'd',
      };

      const empty = EmptyEventSet.get();
      const justA = new ConcreteEventSet({}, [a]);
      const justB = new ConcreteEventSet({}, [b]);
      const justD = new ConcreteEventSet({}, [d]);

      const aAndB = justA.intersect(justB);
      const aAndA = justA.intersect(justA);
      const aAndD = justA.intersect(justD);
      const aBAndEmpty = justA.union(justB).intersect(empty);
      const aDAndAB = justA.union(justB).intersect(justA.union(justD));

      expect((await aAndB.materialise({})).events).toEqual([]);
      expect((await aAndA.materialise({})).events).toEqual([a]);
      expect((await aAndD.materialise({})).events).toEqual([]);
      expect((await aBAndEmpty.materialise({})).events).toEqual([]);
      expect((await aDAndAB.materialise({})).events).toEqual([a]);

      expect(await aAndB.isEmpty()).toEqual(true);
      expect(await aAndA.isEmpty()).toEqual(false);
      expect(await aAndD.isEmpty()).toEqual(true);
      expect(await aBAndEmpty.isEmpty()).toEqual(true);
      expect(await aDAndAB.isEmpty()).toEqual(false);

      expect(await aAndB.count()).toEqual(0);
      expect(await aAndA.count()).toEqual(1);
      expect(await aAndD.count()).toEqual(0);
      expect(await aBAndEmpty.count()).toEqual(0);
      expect(await aDAndAB.count()).toEqual(1);
    });

    test('filter', async () => {
      const keys = {
        num: Num,
        char: Str,
      };

      const a: Event<typeof keys> = {
        id: 'a',
        num: 97,
        char: 'a',
      };
      const b: Event<typeof keys> = {
        id: 'b',
        num: 98,
        char: 'b',
      };
      const d: Event<typeof keys> = {
        id: 'd',
        num: 100,
        char: 'd',
      };

      const events = new ConcreteEventSet(keys, [a, b, d]);


      const justA = events.filter(eq(v('id'), c('a')));
      const justD = events.filter(eq(v('num'), c(100)));

      expect((await justA.materialise(keys)).events).toEqual([a]);
      expect((await justD.materialise(keys)).events).toEqual([d]);
    });

    test('sort', async () => {
      const keys = {
        num: Num,
        char: Str,
      };

      const a: Event<typeof keys> = {
        id: 'a',
        num: 97,
        char: 'a',
      };
      const b: Event<typeof keys> = {
        id: 'b',
        num: 98,
        char: 'b',
      };
      const d: Event<typeof keys> = {
        id: 'd',
        num: 100,
        char: 'd',
      };

      const events = new ConcreteEventSet(keys, [a, b, d]);


      const byNum = events.sort({
        expression: v('num'),
        direction: Direction.ASC,
      });
      const byStr = events.sort({
        expression: v('char'),
        direction: Direction.ASC,
      });

      expect((await byNum.materialise(keys)).events).toEqual([a, b, d]);
      expect((await byStr.materialise(keys)).events).toEqual([a, b, d]);
    });

    test('sort desc', async () => {
      const keys = {
        num: Num,
        char: Str,
      };

      const a: Event<typeof keys> = {
        id: 'a',
        num: 97,
        char: 'a',
      };
      const b: Event<typeof keys> = {
        id: 'b',
        num: 98,
        char: 'b',
      };
      const d: Event<typeof keys> = {
        id: 'd',
        num: 100,
        char: 'd',
      };

      const events = new ConcreteEventSet(keys, [a, b, d]);


      const byNum = events.sort({
        expression: v('num'),
        direction: Direction.DESC,
      });
      const byStr = events.sort({
        expression: v('char'),
        direction: Direction.DESC,
      });

      expect((await byNum.materialise(keys)).events).toEqual([d, b, a]);
      expect((await byStr.materialise(keys)).events).toEqual([d, b, a]);
    });
  });
});

describe('cmpFromExpr', () => {
  test('simple', () => {
    const a: UntypedEvent = {
      id: 'a',
      x: 0,
    };
    const b: UntypedEvent = {
      id: 'b',
      x: 42,
    };
    const c: UntypedEvent = {
      id: 'c',
      x: 0,
    };

    const cmp = cmpFromExpr(v('x'));
    expect(cmp(a, b)).toEqual(-1);
    expect(cmp(a, a)).toEqual(0);
    expect(cmp(b, a)).toEqual(1);
    expect(cmp(a, c)).toEqual(0);
  });

  test('kinds', () => {
    const nullEvent: UntypedEvent = {
      id: 'nullEvent',
      x: null,
    };
    const sevenEvent: UntypedEvent = {
      id: 'sevenEvent',
      x: 7,
    };
    const oneEvent: UntypedEvent = {
      id: 'oneEvent',
      x: 1,
    };
    const zeroEvent: UntypedEvent = {
      id: 'zeroEvent',
      x: 0,
    };
    const trueEvent: UntypedEvent = {
      id: 'trueEvent',
      x: true,
    };
    const falseEvent: UntypedEvent = {
      id: 'falseEvent',
      x: false,
    };
    const aardvarkEvent: UntypedEvent = {
      id: 'aardvarkEvent',
      x: 'aardvark',
    };
    const zigguratEvent: UntypedEvent = {
      id: 'zigguratEvent',
      x: 'ziggurat',
    };
    const bigZeroEvent: UntypedEvent = {
      id: 'bigZeroEvent',
      x: 0n,
    };
    const bigOneEvent: UntypedEvent = {
      id: 'bigOneEvent',
      x: 1n,
    };
    const bigTwoEvent: UntypedEvent = {
      id: 'bigTwoEvent',
      x: 2n,
    };

    const cmp = cmpFromExpr(v('x'));

    // Everything is equal to itself:
    expect(cmp(nullEvent, nullEvent)).toEqual(0);
    expect(cmp(sevenEvent, sevenEvent)).toEqual(0);
    expect(cmp(oneEvent, oneEvent)).toEqual(0);
    expect(cmp(zeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(falseEvent, falseEvent)).toEqual(0);
    expect(cmp(trueEvent, trueEvent)).toEqual(0);
    expect(cmp(aardvarkEvent, aardvarkEvent)).toEqual(0);
    expect(cmp(zigguratEvent, zigguratEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, bigZeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, bigOneEvent)).toEqual(0);
    expect(cmp(bigTwoEvent, bigTwoEvent)).toEqual(0);

    // BigInt(x) == x
    expect(cmp(bigZeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, oneEvent)).toEqual(0);

    // one = true, zero = false:
    expect(cmp(oneEvent, trueEvent)).toEqual(0);
    expect(cmp(zeroEvent, falseEvent)).toEqual(0);
    expect(cmp(bigOneEvent, trueEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, falseEvent)).toEqual(0);

    // 0 < 1 < 7
    expect(cmp(zeroEvent, oneEvent)).toEqual(-1);
    expect(cmp(sevenEvent, oneEvent)).toEqual(1);

    // 0n < 1n < 2n
    expect(cmp(bigZeroEvent, bigOneEvent)).toEqual(-1);
    expect(cmp(bigTwoEvent, bigOneEvent)).toEqual(1);

    // 0 < 1n < 7
    expect(cmp(zeroEvent, bigOneEvent)).toEqual(-1);
    expect(cmp(sevenEvent, bigOneEvent)).toEqual(1);

    // aardvark < ziggurat
    expect(cmp(aardvarkEvent, zigguratEvent)).toEqual(-1);

    // null < {bools, numbers, BigInt} < strings
    expect(cmp(nullEvent, falseEvent)).toEqual(-1);
    expect(cmp(aardvarkEvent, sevenEvent)).toEqual(1);
    expect(cmp(nullEvent, bigZeroEvent)).toEqual(-1);
    expect(cmp(bigZeroEvent, sevenEvent)).toEqual(-1);
    expect(cmp(nullEvent, falseEvent)).toEqual(-1);
    expect(cmp(falseEvent, sevenEvent)).toEqual(-1);
  });
});

describe('cmpFromSort', () => {
  test('simple asc', () => {
    const a: UntypedEvent = {
      id: 'a',
      x: 0,
    };
    const b: UntypedEvent = {
      id: 'b',
      x: 42,
    };
    const c: UntypedEvent = {
      id: 'c',
      x: 0,
    };

    const cmp = cmpFromSort({
      expression: v('x'),
      direction: Direction.ASC,
    });
    expect(cmp(a, b)).toEqual(-1);
    expect(cmp(a, a)).toEqual(0);
    expect(cmp(b, a)).toEqual(1);
    expect(cmp(a, c)).toEqual(0);
  });

  test('kinds asc', () => {
    const nullEvent: UntypedEvent = {
      id: 'nullEvent',
      x: null,
    };
    const sevenEvent: UntypedEvent = {
      id: 'sevenEvent',
      x: 7,
    };
    const oneEvent: UntypedEvent = {
      id: 'oneEvent',
      x: 1,
    };
    const zeroEvent: UntypedEvent = {
      id: 'zeroEvent',
      x: 0,
    };
    const trueEvent: UntypedEvent = {
      id: 'trueEvent',
      x: true,
    };
    const falseEvent: UntypedEvent = {
      id: 'falseEvent',
      x: false,
    };
    const aardvarkEvent: UntypedEvent = {
      id: 'aardvarkEvent',
      x: 'aardvark',
    };
    const zigguratEvent: UntypedEvent = {
      id: 'zigguratEvent',
      x: 'ziggurat',
    };
    const bigZeroEvent: UntypedEvent = {
      id: 'bigZeroEvent',
      x: 0n,
    };
    const bigOneEvent: UntypedEvent = {
      id: 'bigOneEvent',
      x: 1n,
    };
    const bigTwoEvent: UntypedEvent = {
      id: 'bigTwoEvent',
      x: 2n,
    };

    const cmp = cmpFromSort({
      expression: v('x'),
      direction: Direction.ASC,
    });

    // Everything is equal to itself:
    expect(cmp(nullEvent, nullEvent)).toEqual(0);
    expect(cmp(sevenEvent, sevenEvent)).toEqual(0);
    expect(cmp(oneEvent, oneEvent)).toEqual(0);
    expect(cmp(zeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(falseEvent, falseEvent)).toEqual(0);
    expect(cmp(trueEvent, trueEvent)).toEqual(0);
    expect(cmp(aardvarkEvent, aardvarkEvent)).toEqual(0);
    expect(cmp(zigguratEvent, zigguratEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, bigZeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, bigOneEvent)).toEqual(0);
    expect(cmp(bigTwoEvent, bigTwoEvent)).toEqual(0);

    // BigInt(x) == x
    expect(cmp(bigZeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, oneEvent)).toEqual(0);

    // one = true, zero = false:
    expect(cmp(oneEvent, trueEvent)).toEqual(0);
    expect(cmp(zeroEvent, falseEvent)).toEqual(0);
    expect(cmp(bigOneEvent, trueEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, falseEvent)).toEqual(0);

    // 0 < 1 < 7
    expect(cmp(zeroEvent, oneEvent)).toEqual(-1);
    expect(cmp(sevenEvent, oneEvent)).toEqual(1);

    // 0n < 1n < 2n
    expect(cmp(bigZeroEvent, bigOneEvent)).toEqual(-1);
    expect(cmp(bigTwoEvent, bigOneEvent)).toEqual(1);

    // 0 < 1n < 7
    expect(cmp(zeroEvent, bigOneEvent)).toEqual(-1);
    expect(cmp(sevenEvent, bigOneEvent)).toEqual(1);

    // aardvark < ziggurat
    expect(cmp(aardvarkEvent, zigguratEvent)).toEqual(-1);

    // null < {bools, numbers, BigInt} < strings
    expect(cmp(nullEvent, falseEvent)).toEqual(-1);
    expect(cmp(aardvarkEvent, sevenEvent)).toEqual(1);
    expect(cmp(nullEvent, bigZeroEvent)).toEqual(-1);
    expect(cmp(bigZeroEvent, sevenEvent)).toEqual(-1);
    expect(cmp(nullEvent, falseEvent)).toEqual(-1);
    expect(cmp(falseEvent, sevenEvent)).toEqual(-1);
  });

  test('simple desc', () => {
    const a: UntypedEvent = {
      id: 'a',
      x: 0,
    };
    const b: UntypedEvent = {
      id: 'b',
      x: 42,
    };
    const c: UntypedEvent = {
      id: 'c',
      x: 0,
    };

    const cmp = cmpFromSort({
      expression: v('x'),
      direction: Direction.DESC,
    });
    expect(cmp(a, b)).toEqual(1);
    expect(cmp(a, a)).toEqual(0);
    expect(cmp(b, a)).toEqual(-1);
    expect(cmp(a, c)).toEqual(0);
  });

  test('kinds desc', () => {
    const nullEvent: UntypedEvent = {
      id: 'nullEvent',
      x: null,
    };
    const sevenEvent: UntypedEvent = {
      id: 'sevenEvent',
      x: 7,
    };
    const oneEvent: UntypedEvent = {
      id: 'oneEvent',
      x: 1,
    };
    const zeroEvent: UntypedEvent = {
      id: 'zeroEvent',
      x: 0,
    };
    const trueEvent: UntypedEvent = {
      id: 'trueEvent',
      x: true,
    };
    const falseEvent: UntypedEvent = {
      id: 'falseEvent',
      x: false,
    };
    const aardvarkEvent: UntypedEvent = {
      id: 'aardvarkEvent',
      x: 'aardvark',
    };
    const zigguratEvent: UntypedEvent = {
      id: 'zigguratEvent',
      x: 'ziggurat',
    };
    const bigZeroEvent: UntypedEvent = {
      id: 'bigZeroEvent',
      x: 0n,
    };
    const bigOneEvent: UntypedEvent = {
      id: 'bigOneEvent',
      x: 1n,
    };
    const bigTwoEvent: UntypedEvent = {
      id: 'bigTwoEvent',
      x: 2n,
    };

    const cmp = cmpFromSort({
      expression: v('x'),
      direction: Direction.DESC,
    });

    // Everything is equal to itself:
    expect(cmp(nullEvent, nullEvent)).toEqual(0);
    expect(cmp(sevenEvent, sevenEvent)).toEqual(0);
    expect(cmp(oneEvent, oneEvent)).toEqual(0);
    expect(cmp(zeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(falseEvent, falseEvent)).toEqual(0);
    expect(cmp(trueEvent, trueEvent)).toEqual(0);
    expect(cmp(aardvarkEvent, aardvarkEvent)).toEqual(0);
    expect(cmp(zigguratEvent, zigguratEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, bigZeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, bigOneEvent)).toEqual(0);
    expect(cmp(bigTwoEvent, bigTwoEvent)).toEqual(0);

    // BigInt(x) == x
    expect(cmp(bigZeroEvent, zeroEvent)).toEqual(0);
    expect(cmp(bigOneEvent, oneEvent)).toEqual(0);

    // one = true, zero = false:
    expect(cmp(oneEvent, trueEvent)).toEqual(0);
    expect(cmp(zeroEvent, falseEvent)).toEqual(0);
    expect(cmp(bigOneEvent, trueEvent)).toEqual(0);
    expect(cmp(bigZeroEvent, falseEvent)).toEqual(0);

    // 0 < 1 < 7
    expect(cmp(zeroEvent, oneEvent)).toEqual(1);
    expect(cmp(sevenEvent, oneEvent)).toEqual(-1);

    // 0n < 1n < 2n
    expect(cmp(bigZeroEvent, bigOneEvent)).toEqual(1);
    expect(cmp(bigTwoEvent, bigOneEvent)).toEqual(-1);

    // 0 < 1n < 7
    expect(cmp(zeroEvent, bigOneEvent)).toEqual(1);
    expect(cmp(sevenEvent, bigOneEvent)).toEqual(-1);

    // aardvark < ziggurat
    expect(cmp(aardvarkEvent, zigguratEvent)).toEqual(1);

    // null < {bools, numbers, BigInt} < strings
    expect(cmp(nullEvent, falseEvent)).toEqual(1);
    expect(cmp(aardvarkEvent, sevenEvent)).toEqual(-1);
    expect(cmp(nullEvent, bigZeroEvent)).toEqual(1);
    expect(cmp(bigZeroEvent, sevenEvent)).toEqual(1);
    expect(cmp(nullEvent, falseEvent)).toEqual(1);
    expect(cmp(falseEvent, sevenEvent)).toEqual(1);
  });
});
