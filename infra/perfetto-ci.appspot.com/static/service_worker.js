/**
 * Copyright (c) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License. You may
 * obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

'use strict';

const CACHE_NAME = 'travis-cache';
const JOBS_URL = 'https://api.travis-ci.org/jobs/';

async function FetchAndCacheIfJob(event) {
  if (!event.request.url.startsWith(JOBS_URL)) {
    return fetch(event.request);
  }

  // Try and retrieve from the cache.
  const cachedResponse = await caches.match(event.request);
  if (cachedResponse) {
    return cachedResponse;
  }

  // If network request failed just return the response.
  const response = await fetch(event.request);
  if (!response || response.status !== 200) {
    return response;
  }

  // Extract the JSON from the response.
  const json = await response.clone().json();
  if (json.state !== 'cancelled' && json.state !== 'finished') {
    return response;
  }

  var responseToCache = response.clone();
  caches.open(CACHE_NAME)
    .then(cache => {
      cache.put(event.request, responseToCache);
    });

  return response;
}

self.addEventListener('fetch', event => {
  event.respondWith(FetchAndCacheIfJob(event));
});
