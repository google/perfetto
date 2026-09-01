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

/**
 * Parses a Server-Sent Events (SSE) stream from a fetch response body. SSE
 * events are blank-line-separated, each consisting of one or more `data:
 * <json>` lines. The terminal event is the literal `data: [DONE]`.
 *
 * @param body - The response body stream from a fetch request.
 * @param signal - Optional abort signal to cancel the stream early.
 * @yields The parsed JSON payload from each SSE event.
 */
export async function* parseSse(
  body: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
): AsyncGenerator<unknown, void, void> {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  let buf = '';
  for (;;) {
    if (signal?.aborted) return;
    const {value, done} = await reader.read();
    if (done) break;
    buf += decoder.decode(value, {stream: true});
    buf = buf.replace(/\r\n/g, '\n');
    let sep: number;
    while ((sep = buf.indexOf('\n\n')) !== -1) {
      const event = buf.slice(0, sep);
      buf = buf.slice(sep + 2);
      const payload = event
        .split('\n')
        .filter((l) => l.startsWith('data:'))
        .map((l) => l.slice(5).trimStart())
        .join('');
      if (!payload || payload === '[DONE]') continue;
      try {
        yield JSON.parse(payload);
      } catch {
        // Ignore unparseable events (keepalives, partial buffers).
      }
    }
  }
}
