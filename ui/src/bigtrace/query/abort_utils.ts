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

// Forward `parent` aborts to `child`; returns a detacher (call in `finally`
// to avoid leaking on long-lived parents). Already-aborted parent fires now.
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
