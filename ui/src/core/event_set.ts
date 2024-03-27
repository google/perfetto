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

import {arrayEquals, isArrayOf} from '../base/array_utils';
import {isString} from '../base/object_utils';
import {intersect} from '../base/set_utils';

// Contents:
// CORE_TYPES - The main types for using EventSet.
// EVENT_SET_IMPLS - Impl of {Concreate, Empty, Sql, Naive{...}}EventSet
// EXPR - Expression logic which can be lowered to either JS or SQL
// STUPID_TYPE_MAGIC
// HELPERS - Random helpers.

// CORE_TYPES =========================================================

// A single value. These are often retrieved from trace_processor so
// need to map to the related sqlite type:
// null = NULL, string = TEXT, number = INTEGER/REAL,
// boolean = INTEGER, bigint = INTEGER
export type Primitive = null | string | boolean | number | bigint;

export const Null = 'null' as const;
export const Num = 'num' as const;
export const BigInt = 'bigint' as const;
export const Str = 'str' as const;
export const Id = 'id' as const;
export const Bool = 'bool' as const;

// Values may be of any of the above types:
export type KeyType =
  | typeof Num
  | typeof Str
  | typeof Null
  | typeof Id
  | typeof Bool
  | typeof BigInt;

// KeySet is a specification for the key/value pairs on an Event.
// - Every event must have a string ID.
// - In addition Events may have 1 or more key/value pairs.
// The *specification* for the key/value pair has to be *precisely* one
// of the KeySet constants above. So:
// const thisTypeChecks: KeySet = { foo: Str };
// const thisDoesNot: KeySet = { foo: "bar" };
// Since although 'bar' is a string it's not a KeyType.
export type KeySet = {
  readonly [key: string]: KeyType;
};

// The empty keyset. Events from this KeySet will only have ids.
export interface EmptyKeySet extends KeySet {}

export type UntypedKeySet = KeySet;

// A single trace Event.
// Events have:
// - A globally unique identifier `id`.
// - Zero or more key/value pairs.
// Note: Events do *not* have to have all possible keys/value pairs for
// the given id. It is expected that users will only materialise the
// key/value pairs relevant to the specific use case at hand.
export type WritableUntypedEvent = {
  id: string;
  [key: string]: Primitive;
};

export type UntypedEvent = Readonly<WritableUntypedEvent>;

export type Event<K extends KeySet> = {
  readonly [Property in Exclude<keyof K, 'id'>]: ConformingValue<K[Property]>;
} & {
  readonly id: string;
};

// An EventSet is a:
// - ordered
// - immutable
// - subset
// of events in the trace.
export interface EventSet<P extends KeySet> {
  // All possible keys for Events in this EventSet.
  readonly keys: P;

  // Methods for refining the set.
  // Note: these are all synchronous - we expect the cost (and hence
  // any asynchronous queries) to be deferred to analysis time.
  filter(...filters: Filter[]): EventSet<P>;
  sort(...sorts: Sort[]): EventSet<P>;
  union<Q extends KeySet>(other: EventSet<Q>): Merged<P, Q>;
  intersect<Q extends KeySet>(other: EventSet<Q>): Merged<P, Q>;

  // Methods for analysing the set.
  // Note: these are all asynchronous - it's expected that these will
  // often have to do queries.
  count(): Promise<number>;
  isEmpty(): Promise<boolean>;
  materialise<T extends KeySet>(
    keys: T,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<T, P>>;
}

interface UnionEventSet<T extends KeySet> extends EventSet<T> {
  readonly parents: EventSet<T>[];
  readonly isUnion: true;
  create(...events: EventSet<KeySet>[]): UnionEventSet<T>;
}

interface IntersectionEventSet<T extends KeySet> extends EventSet<T> {
  readonly parents: EventSet<T>[];
  readonly isIntersection: true;
  create(...events: EventSet<KeySet>[]): IntersectionEventSet<T>;
}

interface FilterEventSet<T extends KeySet> extends EventSet<T> {
  readonly parent: EventSet<T>;
  readonly filters: Filter[];
  readonly isFilter: true;
}

interface SortEventSet<T extends KeySet> extends EventSet<T> {
  readonly parent: EventSet<T>;
  readonly sorts: Sort[];
  readonly isSort: true;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export type UntypedEventSet = EventSet<any>;

// An expression that operates on an Event and produces a Primitive as
// output. Expressions have to work both in JavaScript and in SQL.
// In SQL users can use buildQueryFragment to convert the expression
// into a snippet of SQL. For JavaScript they call execute(). In both
// cases you need to know which keys the expression uses, for this call
// `freeVariables`.
// TODO(hjd): These should also be paramatised by KeySet and final
// type.
export interface Expr {
  // Return a fragment of SQL that can be used to evaluate the
  // expression. `binding` maps key names to column names in the
  // resulting SQL. The caller must ensure that binding includes at
  // least all the keys from `freeVariables`.
  buildQueryFragment(binding: Map<string, string>): string;

  // Run the expression on an Event. The caller must ensure that event
  // has all the keys from `freeVariables` materialised.
  execute(event: UntypedEvent): Primitive;

  // Returns the set of keys used in this expression.
  // For example in an expression representing `(foo + 4) * bar`
  // freeVariables would return the set {foo: Num, bar: Num}.
  freeVariables(): KeySet;
}

// A filter is a (normally boolean) expression.
export type Filter = Expr;

// Sorting direction.
export enum Direction {
  ASC,
  DESC,
}

// A sort is an expression combined with a direction:
export interface Sort {
  direction: Direction;
  expression: Expr;
}

// EVENT_SET_IMPLS ====================================================

// OptimisingEventSet is what makes it a) tractable to write EventSet
// implementations and b) have those implementations be fast.
// The EventSet interface has two kinds of methods:
// 1. Synchronous refinement methods which produce an EventSet and
//    often take a second EventSet as an argument
// 2. Asynchronous 'analysis' methods
//
// Together this means in the minimal case subclasses only *have* to
// implement the single abstract method: materialise(). Everything else
// is handled for you.
export abstract class OptimisingEventSet<P extends KeySet>
  implements EventSet<P>
{
  abstract readonly keys: P;

  // OptimisingEventSet provides the synchronous refinement methods.
  // The basic pattern is to construct a 'NaiveFoo' EventSet which will
  // do the given operation (filter, sort, union, intersection) in
  // JavaScript then call optimise(). Optimse then tries to improve the
  // EventSet tree - and avoid having to use the fallback naive
  // implementaion.
  // Optimise does 'tree rewriting' of the EventSet tree. For example
  // considering a tree: 'union(A, 0)' where 0 is the empty set and
  // A is some arbitrary EventSet, optimise(union(A, 0)) returns A.
  // For more detail see optimise() below.

  filter(...filters: Filter[]): EventSet<P> {
    const result = new NaiveFilterEventSet(this, filters);
    const optimised = optimise(result);
    return optimised;
  }

  sort(...sorts: Sort[]): EventSet<P> {
    const result = new NaiveSortEventSet(this, sorts);
    const optimised = optimise(result);
    return optimised;
  }

  union<Q extends KeySet>(other: EventSet<Q>): Merged<P, Q> {
    const merged = mergeKeys(this.keys, other.keys);
    const result = new NaiveUnionEventSet<MergedKeys<P, Q>>(
      merged,
      this as UntypedEventSet,
      other as UntypedEventSet,
    );
    const optimised = optimise(result);
    return optimised;
  }

  intersect<Q extends KeySet>(other: EventSet<Q>): Merged<P, Q> {
    const merged = mergeKeys(this.keys, other.keys);
    const result = new NaiveIntersectionEventSet<MergedKeys<P, Q>>(
      merged,
      this as UntypedEventSet,
      other as UntypedEventSet,
    );
    const optimised = optimise(result);
    return optimised;
  }

  // Analysis methods should be implemented by the subclass.
  // Materialise is abstract and must be implemented by the subclass.
  abstract materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, P>>;

  // We provide a default implementation of count() on top of
  // materialise(). It's likely the subclass can provide a more
  // performant implementation.
  async count(): Promise<number> {
    const materialised = await this.materialise({});
    return materialised.events.length;
  }

  // We provide a default implementation of empty() on top of
  // materialise(). It's likely the subclass can provide a more
  // performant implementation.
  async isEmpty(): Promise<boolean> {
    const materialised = await this.materialise(
      {},
      0 /* offset */,
      1 /* limit */,
    );
    return materialised.events.length === 0;
  }
}

class NaiveFilterEventSet<P extends KeySet>
  extends OptimisingEventSet<P>
  implements FilterEventSet<P>
{
  readonly isFilter = true;
  readonly parent: EventSet<P>;
  readonly filters: Filter[];
  readonly keys: P;

  constructor(parent: EventSet<P>, filters: Filter[]) {
    super();
    this.parent = parent;
    this.keys = this.parent.keys;
    this.filters = filters;
  }

  async count(): Promise<number> {
    const keys = freeVariablesFromFilters(this.filters);
    const concreteParent = await this.parent.materialise(keys);
    const events = concreteParent.events;
    let total = 0;
    for (const e of events) {
      if (this.filters.every((f) => f.execute(e))) {
        total += 1;
      }
    }
    return total;
  }

  async isEmpty(): Promise<boolean> {
    const keys = freeVariablesFromFilters(this.filters);
    const concreateParent = await this.parent.materialise(keys);
    const events = concreateParent.events;
    for (const e of events) {
      if (this.filters.every((f) => f.execute(e))) {
        return false;
      }
    }
    return true;
  }

  async materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, P>> {
    const combined = freeVariablesFromFilters(this.filters, keys);
    const concreateParent = await this.parent.materialise(combined);
    let events = concreateParent.events;
    for (const filter of this.filters) {
      events = events.filter((e) => filter.execute(e));
    }
    return new ConcreteEventSet(combined, events).materialise(
      keys,
      offset,
      limit,
    );
  }
}

class NaiveSortEventSet<P extends KeySet>
  extends OptimisingEventSet<P>
  implements SortEventSet<P>
{
  readonly isSort = true;
  readonly parent: EventSet<P>;
  readonly sorts: Sort[];
  readonly keys: P;

  constructor(parent: EventSet<P>, sorts: Sort[]) {
    super();
    this.parent = parent;
    this.keys = this.parent.keys;
    this.sorts = sorts;
  }

  async count(): Promise<number> {
    return this.parent.count();
  }

  async isEmpty(): Promise<boolean> {
    return this.parent.isEmpty();
  }

  async materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, P>> {
    const combined = freeVariablesFromSorts(this.sorts, keys);
    const concreateParent = await this.parent.materialise(combined);
    let events = concreateParent.events;
    for (const sort of this.sorts) {
      events = events.sort(cmpFromSort(sort));
    }
    return new ConcreteEventSet(combined, events).materialise(
      keys,
      offset,
      limit,
    );
  }
}

export class NaiveUnionEventSet<T extends KeySet>
  extends OptimisingEventSet<T>
  implements UnionEventSet<T>
{
  readonly isUnion = true;
  readonly parents: EventSet<T>[];
  readonly keys: T;

  constructor(keys: T, ...parents: EventSet<T>[]) {
    super();
    this.keys = keys;
    this.parents = parents;
  }

  create(...parents: EventSet<T>[]): NaiveUnionEventSet<T> {
    return new NaiveUnionEventSet(this.keys, ...parents);
  }

  // TODO(hjd): We could implement a more efficient dedicated count().
  // TODO(hjd): We could implement a more efficient dedicated isEmpty().

  async materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, T>> {
    const promises = this.parents.map((p) => p.materialise(keys));
    const materialisedParents = (await Promise.all(
      promises,
    )) as ConcreteEventSet<Q>[];
    const seen = new Set<string>();
    let events = [];

    // TODO(hjd): There are various options for doing this in faster
    // way and we should do one of them.
    for (const parent of materialisedParents) {
      for (const e of parent.events) {
        if (!seen.has(e.id)) {
          events.push(e);
          seen.add(e.id);
        }
      }
    }

    events = applyLimitOffset(events, limit, offset);
    return ConcreteEventSet.from(keys, events) as unknown as Materialised<Q, T>;
  }
}

export class NaiveIntersectionEventSet<T extends KeySet>
  extends OptimisingEventSet<T>
  implements IntersectionEventSet<T>
{
  readonly isIntersection = true;
  readonly parents: EventSet<T>[];
  readonly keys: T;

  constructor(keys: T, ...parents: EventSet<T>[]) {
    super();
    this.keys = keys;
    this.parents = parents;
  }

  create(...parents: EventSet<T>[]): NaiveIntersectionEventSet<T> {
    return new NaiveIntersectionEventSet(this.keys, ...parents);
  }

  // TODO(hjd): We could implement a more efficient dedicated count().
  // TODO(hjd): We could implement a more efficient dedicated isEmpty().

  async materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, T>> {
    if (this.parents.length === 0) {
      return ConcreteEventSet.from(keys, []) as Materialised<Q, T>;
    }

    const parents = this.parents.slice();
    const firstParent = parents.pop()!;

    const promises = parents.map((p) => p.materialise({}));
    const firstPromise = firstParent.materialise(
      keys,
    ) as unknown as ConcreteEventSet<Q>;

    const materialised = await Promise.all(promises);
    const firstMaterialised = await firstPromise;

    let ids = new Set<string>();
    for (const e of firstMaterialised.events) {
      ids.add(e.id);
    }
    for (const m of materialised) {
      const newIds = new Set<string>();
      for (const e of m.events) {
        newIds.add(e.id);
      }
      ids = intersect(ids, newIds);
    }

    let events = firstMaterialised.events.filter((e) => ids.has(e.id));
    events = applyLimitOffset(events, limit, offset);
    return ConcreteEventSet.from(keys, events) as unknown as Materialised<Q, T>;
  }
}

// A completely empty EventSet.
export class EmptyEventSet<T extends KeySet>
  extends OptimisingEventSet<T>
  implements EventSet<T>
{
  readonly isEmptyEventSet = true;
  readonly keys: T;

  constructor(keys: T) {
    super();
    this.keys = keys;
  }

  static get(): EmptyEventSet<EmptyKeySet> {
    return new EmptyEventSet<EmptyKeySet>({});
  }

  count(): Promise<number> {
    return Promise.resolve(0);
  }

  isEmpty(): Promise<boolean> {
    return Promise.resolve(true);
  }

  async materialise<Q extends KeySet>(
    keys: Q,
    _offset?: number,
    _limit?: number,
  ): Promise<Materialised<Q, T>> {
    return Promise.resolve(
      new ConcreteEventSet<Q>(keys, []) as unknown as Materialised<Q, T>,
    );
  }
}

export class ConcreteEventSet<P extends KeySet>
  extends OptimisingEventSet<P>
  implements EventSet<P>
{
  readonly isConcreteEventSet = true;
  readonly events: Event<P>[];
  readonly keys: P;

  static from<Q extends KeySet>(
    keys: Q,
    events: Event<Q>[],
  ): ConcreteEventSet<Q> {
    return new ConcreteEventSet<Q>(keys, events);
  }

  constructor(keys: P, events: Event<P>[]) {
    super();
    // TODO(hjd): Add some paranoid mode where we crash here if
    // `events` and `keys` mismatch?
    this.events = events;
    this.keys = keys;
  }

  count(): Promise<number> {
    return Promise.resolve(this.events.length);
  }

  isEmpty(): Promise<boolean> {
    return Promise.resolve(this.events.length === 0);
  }

  materialise<Q extends KeySet>(
    keys: Q,
    offset?: number,
    limit?: number,
  ): Promise<Materialised<Q, P>> {
    const actualOffset = offset === undefined ? 0 : offset;
    const actualEnd =
      limit === undefined ? this.events.length : actualOffset + limit;

    const shouldFilter = !isEqualKeySet(keys, this.keys);
    const shouldSlice = actualOffset !== 0 || actualEnd !== this.events.length;

    if (!shouldFilter && !shouldSlice) {
      return Promise.resolve(this as unknown as Materialised<Q, P>);
    }

    let events = this.events as Event<Q>[];

    if (shouldFilter) {
      events = events.map((e) => {
        const result: WritableUntypedEvent = {
          id: e.id,
        };
        for (const [k, v] of Object.entries(keys)) {
          // While the static typing prevents folks from hitting
          // this in the common case people can still on purpose pass
          // keysets and lie about the types.
          result[k] = (e as UntypedEvent)[k] ?? getKeyDefault(k, v);
        }
        return result as Event<Q>;
      });
    }

    if (shouldSlice) {
      events = events.slice(actualOffset, actualEnd);
    }

    return Promise.resolve(
      new ConcreteEventSet<Q>(keys, events) as unknown as Materialised<Q, P>,
    );
  }
}

// Optimse:
// We have a couple major kinds of optimisation:
// 1. Pushing down filters.
// 2. Set optimisations (e.g union(empty, A) == A)
// 3. Merging EventSets of the same kind
//
// In more detail:
// 1. Pushing down filters. For example:
//    filter(union(A, B), pred) ==
//      union(filter(A, pred), filter(B, pred))
//    This is more useful than it seems since if we manage to push down
//    filters all the may to SQL they can be implemented very
//    efficiently in C++.
// 2. Classic set optimisations. e.g.
//      union(A, empty) == A
//      union(A, A) == A
//      intersect(A, empty) == empty
//      etc
// 3. Merging EventSets of the same type. For example:
//    union(concrete(a, b), concrete(b, c)) == concrete(a, b, c)
//    Similarly the combinations of two SQL EventSets can be converted
//    into a single SQL EventSet with a more complicated query -
//    avoiding doing the processing in TypeScript.
//
// A critical pre-condition of this function is that EventSets are
// immutable - this allows us to reuse parts of the input event set tree
// in the output.
export function optimise<T extends KeySet>(eventSet: EventSet<T>): EventSet<T> {
  // Empty EventSet can't be futher optimised.
  if (isEmptyEventSet(eventSet)) {
    return eventSet;
  }

  if (isConcreteEventSet(eventSet)) {
    // A concrete events with zero elements is the empty events.
    if (eventSet.events.length === 0) {
      return new EmptyEventSet(eventSet.keys);
    }
    // ...but otherwise can not be optimised further.
    return eventSet;
  }

  if (isUnionEventSet(eventSet)) {
    const keys = eventSet.keys;

    let newParents: EventSet<T>[] = eventSet.parents.slice();

    // Empty sets don't contribute to the union.
    newParents = newParents.filter((p) => !isEmptyEventSet(p));

    // union([]) is empty.
    if (newParents.length === 0) {
      return new EmptyEventSet(keys);
    }

    if (newParents.length === 1) {
      return newParents[0];
    }

    // The union of concrete EventSets is a concrete EventSets with all
    // the events in.
    if (
      isArrayOf<ConcreteEventSet<T>, EventSet<T>>(
        isConcreteEventSet,
        newParents,
      )
    ) {
      const seen = new Set<string>();
      const events = [];
      for (const p of newParents) {
        for (const e of p.events) {
          if (!seen.has(e.id)) {
            events.push(e);
            seen.add(e.id);
          }
        }
      }
      return ConcreteEventSet.from(eventSet.keys, events);
    }

    if (arrayEquals(newParents, eventSet.parents)) {
      return eventSet;
    } else {
      return eventSet.create(...newParents);
    }
  }

  if (isIntersectionEventSet(eventSet)) {
    // For any x: intersect([x, 0]) is 0
    for (const parent of eventSet.parents) {
      if (isEmptyEventSet(parent)) {
        return parent;
      }
    }
    return eventSet;
  }

  if (isFilterEventSet(eventSet)) {
    const parent = eventSet.parent;

    if (isEmptyEventSet(parent)) {
      return parent;
    }

    return eventSet;
  }

  if (isSortEventSet(eventSet)) {
    const parent = eventSet.parent;

    if (isEmptyEventSet(parent)) {
      return parent;
    }

    return eventSet;
  }

  // TODO(hjd): Re-add the optimisations from the prototype.
  // TODO(hjd): Union([a, a]) === a but maybe not worth optimising.

  return eventSet;
}

// EXPR ===============================================================

abstract class BinOp implements Expr {
  readonly left: Expr;
  readonly right: Expr;

  constructor(left: Expr, right: Expr) {
    this.left = left;
    this.right = right;
  }

  buildQueryFragment(binding: Map<string, string>): string {
    const a = this.left.buildQueryFragment(binding);
    const b = this.right.buildQueryFragment(binding);
    const op = this.sqlOp();
    return `(${a} ${op} ${b})`;
  }

  execute(event: UntypedEvent): Primitive {
    const a = this.left.execute(event);
    const b = this.right.execute(event);
    return this.evaluate(a, b);
  }

  freeVariables(): KeySet {
    const a = this.left.freeVariables();
    const b = this.right.freeVariables();
    return mergeKeys(a, b);
  }

  abstract sqlOp(): string;
  abstract evaluate(lhs: Primitive, rhs: Primitive): Primitive;
}

class Le extends BinOp implements Expr {
  sqlOp(): string {
    return '<=';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs! <= rhs!;
  }
}

class Lt extends BinOp implements Expr {
  sqlOp(): string {
    return '<';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs! < rhs!;
  }
}

class Ge extends BinOp implements Expr {
  sqlOp(): string {
    return '>=';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs! >= rhs!;
  }
}

class Gt extends BinOp implements Expr {
  sqlOp(): string {
    return '>';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs! > rhs!;
  }
}

class Eq extends BinOp implements Expr {
  sqlOp(): string {
    return '=';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs === rhs;
  }
}

class And extends BinOp implements Expr {
  sqlOp(): string {
    return 'AND';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    return lhs && rhs;
  }
}

class Or extends BinOp implements Expr {
  sqlOp(): string {
    return 'OR';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    return lhs || rhs;
  }
}

class Ne extends BinOp implements Expr {
  sqlOp(): string {
    return '!=';
  }

  evaluate(lhs: Primitive, rhs: Primitive): Primitive {
    return lhs !== rhs;
  }
}

class Var implements Expr {
  readonly name: string;

  constructor(name: string) {
    this.name = name;
  }

  buildQueryFragment(binding: Map<string, string>): string {
    // TODO(hjd): wrap in try catch?
    return binding.get(this.name)!;
  }

  execute(event: UntypedEvent): Primitive {
    return event[this.name]!;
  }

  freeVariables(): KeySet {
    return {
      [this.name]: Null,
    };
  }
}

class Constant implements Expr {
  readonly value: Primitive;

  constructor(value: Primitive) {
    this.value = value;
  }

  buildQueryFragment(_: Map<string, string>): string {
    const value = this.value;
    if (value === null) {
      return 'NULL';
    } else if (isString(value)) {
      return `'${value}'`;
    } else if (typeof value === 'boolean') {
      return value ? 'TRUE' : 'FALSE';
    } else {
      return `${value}`;
    }
  }

  execute(_: UntypedEvent): Primitive {
    return this.value;
  }

  freeVariables(): EmptyKeySet {
    return {};
  }
}

export function eq(left: Expr, right: Expr): Eq {
  return new Eq(left, right);
}
export function ne(left: Expr, right: Expr): Ne {
  return new Ne(left, right);
}

export function gt(left: Expr, right: Expr): Gt {
  return new Gt(left, right);
}

export function ge(left: Expr, right: Expr): Ge {
  return new Ge(left, right);
}

export function lt(left: Expr, right: Expr): Lt {
  return new Lt(left, right);
}

export function le(left: Expr, right: Expr): Le {
  return new Le(left, right);
}

export function and(left: Expr, right: Expr): And {
  return new And(left, right);
}

export function or(left: Expr, right: Expr): Or {
  return new Or(left, right);
}

export function c(value: Primitive): Constant {
  return new Constant(value);
}

export function v(name: string): Var {
  return new Var(name);
}

// Type guards:
export function isEmptyEventSet<T extends KeySet>(
  s: EventSet<T> | EmptyEventSet<T>,
): s is EmptyEventSet<T> {
  return !!(s as EmptyEventSet<T>).isEmptyEventSet;
}

export function isConcreteEventSet<T extends KeySet>(
  s: EventSet<T> | ConcreteEventSet<T>,
): s is ConcreteEventSet<T> {
  return !!(s as ConcreteEventSet<T>).isConcreteEventSet;
}

export function isUnionEventSet<T extends KeySet>(
  s: EventSet<T> | UnionEventSet<T>,
): s is UnionEventSet<T> {
  return (
    (s as UnionEventSet<T>).isUnion &&
    Array.isArray((s as UnionEventSet<T>).parents)
  );
}

export function isIntersectionEventSet<T extends KeySet>(
  s: EventSet<T> | IntersectionEventSet<T>,
): s is IntersectionEventSet<T> {
  return (
    (s as IntersectionEventSet<T>).isIntersection &&
    Array.isArray((s as IntersectionEventSet<T>).parents)
  );
}

export function isFilterEventSet<T extends KeySet>(
  s: EventSet<T> | FilterEventSet<T>,
): s is FilterEventSet<T> {
  return (
    (s as FilterEventSet<T>).isFilter &&
    Array.isArray((s as FilterEventSet<T>).filters)
  );
}

export function isSortEventSet<T extends KeySet>(
  s: EventSet<T> | SortEventSet<T>,
): s is SortEventSet<T> {
  return (
    (s as SortEventSet<T>).isSort && Array.isArray((s as SortEventSet<T>).sorts)
  );
}

// STUPID_TYPE_MAGIC ==================================================
type ErrorBrand<T extends string> = {
  [k in T]: void;
};

// A particular key/value pair on an Event matches the relevant entry
// on the KeySet if the KeyType and the value type 'match':
// Id => string
// Str => string
// Bool => boolean
// Null => null
// Num => number
type KeyToType = {
  num: number;
  str: string;
  bool: boolean;
  null: null;
  bigint: bigint;
  id: string;
};

type ConformingValue<T> = T extends keyof KeyToType ? KeyToType[T] : void;

type Materialised<
  Concrete extends KeySet,
  Parent extends KeySet,
> = Parent extends Concrete
  ? ConcreteEventSet<Concrete>
  : ErrorBrand<`Very bad!`>;

type MergedKeys<Left extends KeySet, Right extends KeySet> = Left & Right;

type Merged<Left extends KeySet, Right extends KeySet> = EventSet<
  MergedKeys<Left, Right>
>;

// HELPERS ============================================================
function applyLimitOffset<T>(arr: T[], limit?: number, offset?: number): T[] {
  const actualOffset = offset === undefined ? 0 : offset;
  const actualEnd = limit === undefined ? arr.length : actualOffset + limit;
  const shouldSlice = actualOffset !== 0 || actualEnd !== arr.length;
  return shouldSlice ? arr.slice(actualOffset, actualEnd) : arr;
}

function mergeKeys<P extends KeySet, Q extends KeySet>(
  left: P,
  right: Q,
): MergedKeys<P, Q> {
  return Object.assign({}, left, right);
}

function getKeyDefault(keyName: string, keyType: KeyType): Primitive {
  switch (keyType) {
    case Id:
      throw new Error(
        `Can't create default for key '${keyName}' with type '${keyType}'`,
      );
    case Num:
      return 0;
    case Null:
      return null;
    case Str:
      return '';
    case Bool:
      return false;
    case BigInt:
      return 0n;
    default:
      const _exhaustiveCheck: never = keyType;
      return _exhaustiveCheck;
  }
}

function isEqualKeySet(a: UntypedKeySet, b: UntypedKeySet): boolean {
  for (const k in a) {
    if (a[k] !== b[k]) {
      return false;
    }
  }
  for (const k in b) {
    if (b[k] !== a[k]) {
      return false;
    }
  }
  return true;
}

function freeVariablesFromFilters(
  filters: Filter[],
  initialKeySet?: KeySet,
): KeySet {
  let result = {};

  if (initialKeySet !== undefined) {
    result = mergeKeys(result, initialKeySet);
  }

  for (const filter of filters) {
    result = mergeKeys(result, filter.freeVariables());
  }

  return result;
}

function freeVariablesFromSorts(sorts: Sort[], initialKeySet?: KeySet): KeySet {
  let result = {};

  if (initialKeySet !== undefined) {
    result = mergeKeys(result, initialKeySet);
  }

  for (const sort of sorts) {
    result = mergeKeys(result, sort.expression.freeVariables());
  }

  return result;
}

function primativeToRank(p: Primitive) {
  if (p === null) {
    return 0;
  } else if (isString(p)) {
    return 2;
  } else {
    return 1;
  }
}

// TODO(hjd): test for bignums
// Convert an expression into a sort style comparison function.
// Exported for testing.
export function cmpFromExpr<T extends KeySet>(
  expr: Expr,
): (l: Event<T>, r: Event<T>) => number {
  return (l: Event<T>, r: Event<T>) => {
    const lhs = expr.execute(l);
    const rhs = expr.execute(r);
    const lhsRank = primativeToRank(lhs);
    const rhsRank = primativeToRank(rhs);
    if (lhsRank < rhsRank) {
      return -1;
    } else if (lhsRank > rhsRank) {
      return 1;
    } else {
      // Double equals on purpose so 0 == false and 1 == true are true
      if (lhs == rhs) {
        return 0;
      } else if (lhs! < rhs!) {
        return -1;
      } else {
        return 1;
      }
    }
  };
}

// Convert a 'sort' into a sort() style comparison function.
// Exported for testing.
export function cmpFromSort<T extends KeySet>(
  sort: Sort,
): (l: Event<T>, r: Event<T>) => number {
  const cmp = cmpFromExpr<T>(sort.expression);
  if (sort.direction === Direction.ASC) {
    return cmp;
  } else {
    // cmp(r, l) is better than -cmp(l, r) since JS distinguishes
    // between -0 and 0.
    return (l: Event<T>, r: Event<T>) => cmp(r, l);
  }
}
