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

#ifndef INCLUDE_PERFETTO_EXT_BASE_THREADING_UTIL_H_
#define INCLUDE_PERFETTO_EXT_BASE_THREADING_UTIL_H_

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/threading/channel.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/unix_task_runner.h"

namespace perfetto {
namespace base {

// Blocks the calling thread until |fd| is considered "readable". In Linux,
// this corresponds to |POLLOUT| or |POLLHUP| being returned if |fd| is polled.
inline void BlockUntilReadableFd(base::PlatformHandle fd) {
  base::UnixTaskRunner runner;
  runner.AddFileDescriptorWatch(fd, [&runner]() { runner.Quit(); });
  runner.Run();
}

// Creates a Stream<T> which returns all the data from |channel| and completes
// when |channel| is closed.
//
// Note: the caller retains ownership of the passed channel and must ensure that
// the channel outlives the lifetime of the returned stream.
template <typename T>
Stream<T> ReadChannelStream(Channel<T>* channel) {
  class ReadImpl : public StreamPollable<T> {
   public:
    explicit ReadImpl(Channel<T>* reader) : reader_(reader) {}

    StreamPollResult<T> PollNext(PollContext* ctx) override {
      auto result = reader_->ReadNonBlocking();
      if (!result.item.has_value()) {
        if (result.is_closed) {
          return DonePollResult();
        }
        ctx->RegisterInterested(reader_->read_fd());
        return PendingPollResult();
      }
      return std::move(*result.item);
    }

   private:
    Channel<T>* reader_ = nullptr;
  };
  return MakeStream<ReadImpl>(channel);
}

// Creates a Future<FVoid> which handles writing |item| into |channel|. The
// Future is completed when the item is succesfully written.
//
// Note: the caller retains ownership of the passed channel and must ensure that
// the channel outlives the lifetime of the returned future.
template <typename T>
Future<FVoid> WriteChannelFuture(Channel<T>* channel, T item) {
  class WriteImpl : public FuturePollable<FVoid> {
   public:
    WriteImpl(Channel<T>* writer, T to_write)
        : writer_(writer), to_write_(std::move(to_write)) {}

    FuturePollResult<FVoid> Poll(PollContext* ctx) override {
      auto res = writer_->WriteNonBlocking(std::move(to_write_));
      PERFETTO_CHECK(!res.is_closed);
      if (!res.success) {
        ctx->RegisterInterested(writer_->write_fd());
        return PendingPollResult();
      }
      return FVoid();
    }

   private:
    Channel<T>* writer_ = nullptr;
    T to_write_;
  };
  return MakeFuture<WriteImpl>(channel, std::move(item));
}

// Creates a Stream<T> which yields the result of executing |fn| on |pool|
// repeatedly. The returned stream only completes when |fn| returns
// base::nullopt.
//
// The intended usage of this function is to schedule CPU intensive work on a
// background thread pool and receive regular "updates" on the progress by:
// a) breaking the work into chunks
// b) returning some indication of progress/partial results through |T|.
template <typename T>
Stream<T> RunOnThreadPool(ThreadPool* pool,
                          std::function<base::Optional<T>()> fn) {
  class RunOnPoolImpl : public StreamPollable<T> {
   public:
    explicit RunOnPoolImpl(ThreadPool* pool,
                           std::function<base::Optional<T>()> fn)
        : pool_(pool),
          fn_(std::make_shared<std::function<base::Optional<T>()>>(
              std::move(fn))),
          channel_(new Channel<T>(1)),
          channel_stream_(ReadChannelStream(channel_.get())) {
      RunFn();
    }

    StreamPollResult<T> PollNext(PollContext* ctx) override {
      ASSIGN_OR_RETURN_IF_PENDING_STREAM(res, channel_stream_.PollNext(ctx));
      if (res.IsDone()) {
        return DonePollResult();
      }
      RunFn();
      return res;
    }

   private:
    void RunFn() {
      pool_->PostTask([channel = channel_, fn = fn_]() {
        auto opt_value = (*fn)();
        if (!opt_value) {
          channel->Close();
          return;
        }
        auto write_res =
            channel->WriteNonBlocking(std::move(opt_value.value()));
        PERFETTO_CHECK(write_res.success);
        PERFETTO_CHECK(!write_res.is_closed);
      });
    }

    ThreadPool* pool_ = nullptr;
    std::shared_ptr<std::function<base::Optional<T>()>> fn_;
    std::shared_ptr<Channel<T>> channel_;
    base::Stream<T> channel_stream_;
  };
  return MakeStream<RunOnPoolImpl>(pool, std::move(fn));
}

// Creates a Future<T> which yields the result of executing |fn| on |pool|. The
// returned completes with the return value of |fn|.
//
// The intended usage of this function is to schedule CPU intensive work on a
// background thread pool and have the result returned when available.
template <typename T>
Future<T> RunOnceOnThreadPool(ThreadPool* pool, std::function<T()> fn) {
  return RunOnThreadPool<T>(
             pool,
             [done = false, fn = std::move(fn)]() mutable -> base::Optional<T> {
               if (done) {
                 return base::nullopt;
               }
               done = true;
               return fn();
             })
      .Collect(base::ToFutureCheckedCollector<T>());
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_THREADING_UTIL_H_
