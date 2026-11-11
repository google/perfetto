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

// Records who owns an object, such as a tab, a sidebar item or a command.
//
// The owner is stored on the object itself under a private Symbol key, so the
// object is passed on unchanged and class instances keep their methods. The
// key is set with a plain assignment, which makes it an own enumerable
// property: object spread ({...obj}) copies it, so the owner survives copies
// of the object.

const OWNER = Symbol('perfetto.owner');

interface Owned {
  [OWNER]?: string;
}

/**
 * Marks `obj` as owned by `owner`.
 *
 * The first owner wins: an object that already has an owner is left alone.
 * Objects that can't be extended (frozen, sealed or non-extensible) are
 * skipped, so callers never throw because of this.
 */
export function setOwner(obj: object, owner: string): void {
  if (!Object.isExtensible(obj)) return;
  const owned = obj as Owned;
  if (owned[OWNER] !== undefined) return;
  owned[OWNER] = owner;
}

/** Returns the owner of `obj`, or undefined if it was never marked. */
export function getOwner(obj: object): string | undefined {
  return (obj as Owned)[OWNER];
}

/**
 * Copies the owner of `from` onto `to`, for places where an object is rebuilt
 * into a new one. Returns `to`.
 */
export function carryOwner<T extends object>(from: object, to: T): T {
  const owner = getOwner(from);
  if (owner !== undefined) setOwner(to, owner);
  return to;
}
