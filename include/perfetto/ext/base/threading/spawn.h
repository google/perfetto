/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_EXT_BASE_THREADING_SPAWN_H_
#define INCLUDE_PERFETTO_EXT_BASE_THREADING_SPAWN_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/flat_set.h"
#include "perfetto/base/platform_handle.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/threading/channel.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/stream_combinators.h"
#include "perfetto/ext/base/threading/util.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/base/weak_ptr.h"

namespace perfetto {
namespace base {

class PolledFuture;

// A RAII object which tracks the polling of a Future.
//
// When this object is dropped, the backing Future will be cancelled as
// soon as possible. In practice, the cancellation happens on the TaskRunner
// thread so there can be some delay.
class SpawnHandle {
 public:
  SpawnHandle(TaskRunner* task_runner, std::function<Future<FVoid>()> fn);
  ~SpawnHandle();

 private:
  SpawnHandle(const SpawnHandle&) = delete;
  SpawnHandle& operator=(const SpawnHandle&) = delete;

  TaskRunner* task_runner_ = nullptr;
  std::shared_ptr<std::unique_ptr<PolledFuture>> polled_future_;
};

// Specialization of SpawnHandle used by Futures/Streams which return T.
//
// Values of T are returned through a Channel<T> which allows reading these
// values on a different thread to where the polling happens.
template <typename T>
class ResultSpawnHandle {
 public:
  ResultSpawnHandle(TaskRunner* task_runner,
                    std::shared_ptr<Channel<T>> channel,
                    std::function<Future<FVoid>()> fn)
      : handle_(task_runner, std::move(fn)), channel_(std::move(channel)) {}

  Channel<T>* channel() const { return channel_.get(); }

 private:
  SpawnHandle handle_;
  std::shared_ptr<Channel<T>> channel_;
};

// "Spawns" a Future<FVoid> on the given TaskRunner and returns an RAII
// SpawnHandle which can be used to cancel the spawn.
//
// Spawning a Future means to poll it to completion. In Perfetto, this is done
// by using a TaskRunner object to track FD readiness and polling the Future
// when progress can be made.
//
// The returned SpawnHandle should be stashed as it is responsible for the
// lifetime of the pollling. If the SpawnHandle is dropped, the Future is
// cancelled and dropped ASAP (this happens on the TaskRunner thread so there
// can be some delay).
PERFETTO_WARN_UNUSED_RESULT inline SpawnHandle SpawnFuture(
    TaskRunner* task_runner,
    std::function<Future<FVoid>()> fn) {
  return SpawnHandle(task_runner, std::move(fn));
}

// Variant of |SpawnFuture| for a Stream<T> allowing returning items of T.
//
// See ResultSpawnHandle for how elements from the stream can be consumed.
template <typename T>
PERFETTO_WARN_UNUSED_RESULT inline ResultSpawnHandle<T> SpawnResultStream(
    TaskRunner* task_runner,
    std::function<Stream<T>()> fn) {
  class AllVoidCollector : public Collector<FVoid, FVoid> {
   public:
    Optional<FVoid> OnNext(FVoid) override { return nullopt; }
    FVoid OnDone() override { return FVoid(); }
  };
  auto channel = std::make_shared<Channel<T>>(4);
  return ResultSpawnHandle<T>(
      task_runner, channel, [c = channel, fn = std::move(fn)]() {
        return fn()
            .MapFuture([c](T value) {
              return WriteChannelFuture(c.get(), std::move(value));
            })
            .Concat(OnDestroyStream<FVoid>([c]() { c->Close(); }))
            .Collect(std::unique_ptr<Collector<FVoid, FVoid>>(
                new AllVoidCollector()));
      });
}

// Variant of |SpawnFuture| for a Future<T> allowing returning items of T.
//
// See ResultSpawnHandle for how elements from the future can be consumed.
template <typename T>
PERFETTO_WARN_UNUSED_RESULT inline ResultSpawnHandle<T> SpawnResultFuture(
    TaskRunner* task_runner,
    std::function<Future<T>()> fn) {
  return SpawnResultStream<T>(task_runner, [fn = std::move(fn)]() {
    return StreamFromFuture(std::move(fn()));
  });
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_THREADING_SPAWN_H_
