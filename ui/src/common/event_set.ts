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

// A single value. These are often retived from trace_processor so
// need to map to the related sqlite type:
// null = NULL, string = TEXT, number = INTEGER/REAL, boolean = INTEGER
export type Primitive = null|string|boolean|number;

export const NullType = null;
export const NumType = 0 as const;
export const StrType = 'str' as const;
export const IdType = 'id' as const;
export const BoolType = true as const;

// Values may be of any of the above types:
type KeyType =
    typeof NumType|typeof StrType|typeof NullType|typeof IdType|typeof BoolType;

// KeySet is a specification for the key/value pairs on an Event.
// - Every event must have a string ID.
// - In addition Events may have 1 or more key/value pairs.
// The *specification* for the key/value pair has to be *precisely* one
// of the KeySet constants above. So:
// const thisTypeChecks: KeySet = { id: IdType, foo: StrType };
// const thisDoesNot: KeySet = { id: IdType, foo: "bar" };
// Since although are is a string it's not a KeySet.
export type KeySet = {
  readonly id: typeof IdType,
  readonly [key: string]: KeyType,
};

export interface EmptyKeySet extends KeySet {
  readonly id: typeof IdType;
}
;

// A particular key/value pair on an Event matches the relevant entry
// on the KeySet if the KeyType and the value type 'match':
// IdType => string
// StrType => string
// BoolType => boolean
// NullType => null
// NumType => number
type IsExactly<P, Q> = P extends Q ? (Q extends P ? any : never) : never;
type IsId<T> = T extends IsExactly<T, typeof IdType>? string : never;
type IsStr<T> = T extends IsExactly<T, typeof StrType>? string : never;
type IsNum<T> = T extends IsExactly<T, typeof NumType>? number : never;
type IsBool<T> = T extends IsExactly<T, typeof BoolType>? boolean : never;
type IsNull<T> = T extends IsExactly<T, typeof NullType>? null : never;
type MapType<T> = IsId<T>|IsStr<T>|IsNum<T>|IsBool<T>|IsNull<T>;
type ConformingValue<T> = T extends MapType<T>? MapType<T>: void;

// A single trace Event.
// Events have:
// - A globally unique identifier `id`.
// - Zero or more key/value pairs.
// Note: Events do *not* have to have all possible keys/value pairs for
// the given id. It is expected that users will only materialise the
// key/value pairs relevant to the specific use case at hand.
export type UntypedEvent = {
  readonly id: string,
  readonly [key: string]: Primitive,
};

export type Event<K extends KeySet> = {
  [Property in keyof K]: ConformingValue<K[Property]>;
};

type KeyUnion<P, Q> = P&Q;

// An EventSet is a:
// - ordered
// - immutable
// - subset
// of events in the trace.
export interface EventSet<P extends KeySet> {
  // All possible keys for Events in this EventSet.
  readonly keys: KeySet;

  // Methods for refining the set.
  // Note: these are all synchronous - we expect the cost (and hence
  // any asynchronous queries) to be deferred to analysis time.
  filter(...filters: Filter[]): EventSet<P>;
  sort(...sorts: Sort[]): EventSet<P>;
  union<Q extends KeySet>(other: EventSet<Q>): EventSet<KeyUnion<P, Q>>;
  intersect<Q extends KeySet>(other: EventSet<Q>): EventSet<KeyUnion<P, Q>>;

  // Methods for analysing the set.
  // Note: these are all asynchronous - it's expected that these will
  // often have to do queries.
  count(): Promise<number>;
  isEmpty(): Promise<boolean>;
  materialise<T extends P>(keys: T, offset?: number, limit?: number):
      Promise<ConcreteEventSet<T>>;
}

export type UntypedEventSet = EventSet<KeySet>;

// An expression that operates on an Event and produces a Primitive as
// output. Expressions have to work both in JavaScript and in SQL.
// In SQL users can use buildQueryFragment to convert the expression
// into a snippet of SQL. For JavaScript they call execute(). In both
// cases you need to know which keys the expression uses, for this call
// `freeVariables`.
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
  // freeVariables would return the set {'foo', 'bar'}.
  freeVariables(): Set<string>;
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

// An EventSet where the Event are accesible synchronously.
interface ConcreteEventSet<T extends KeySet> extends EventSet<T> {
  readonly events: Event<T>[];
}

export type UntypedConcreteEventSet = ConcreteEventSet<KeySet>;
