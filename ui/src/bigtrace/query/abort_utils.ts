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

// Forward abort events from `parent` to `child`, returning a function to
// detach the listener. Callers should detach in the request's `finally`
// to avoid leaking listeners on long-lived parent signals.
//
// If the parent is already aborted at attach time, the child is aborted
// immediately and a no-op detacher is returned.
export function forwardAbort(
  parent: AbortSignal,
  child: AbortController,
): () => void {
  if (parent.aborted) {
    child.abort(parent.reason);
    return () => {};
  }
  const handler = () => child.abort(parent.reason);
  parent.addEventListener('abort', handler, {once: true});
  return () => parent.removeEventListener('abort', handler);
}
