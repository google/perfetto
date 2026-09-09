/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/tracing/tracing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/console_interceptor.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/in_process_tracing_backend.h"
#include "perfetto/tracing/platform.h"
#include "perfetto/tracing/tracing_backend.h"
#include "protos/perfetto/config/interceptor_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/internal/tracing_muxer_impl.h"
#include "src/tracing/test/api_test_support.h"
#include "src/tracing/test/mock_producer.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "src/tracing/test/proxy_producer_endpoint.h"
#include "src/tracing/v2/in_process_tracing_v2_bridge.h"
#include "src/tracing/v2/relay_sequence.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::test {

class TracingMuxerImplV2Test : public testing::Test {
 protected:
  using ProducerImpl = internal::TracingMuxerImpl::ProducerImpl;

  static constexpr uint32_t kTestNumChunks = 32;
  static constexpr uint32_t kTestChunkSize = 256;

  template <typename Callback>
  static void RunOnMuxerAndWait(Callback callback) {
    base::WaitableEvent done;
    TracingMuxerImplInternalsForTest::PostToMuxerSequence(
        [callback = std::move(callback), &done] {
          auto* muxer = static_cast<internal::TracingMuxerImpl*>(
              internal::TracingMuxerImpl::Get());
          callback(muxer);
          done.Notify();
        });
    done.Wait();
  }

  static std::shared_ptr<tracing_v2::RelaySequence> CreateTestRelay() {
    auto runner = std::make_unique<base::ThreadTaskRunner>(
        base::ThreadTaskRunner::CreateAndStart("test.relay"));
    return std::make_shared<tracing_v2::RelaySequence>(std::move(runner));
  }

  static std::shared_ptr<tracing_v2::InProcessTracingV2Bridge> GetBridge() {
    std::shared_ptr<tracing_v2::InProcessTracingV2Bridge> bridge;
    RunOnMuxerAndWait([&](auto* muxer) {
      for (auto& backend : muxer->producer_backends_) {
        if (backend.producer->tracing_v2_connection_)
          bridge = backend.producer->tracing_v2_connection_->bridge;
      }
    });
    return bridge;
  }

  // Number of flush requests the connected producers are still tracking.
  static size_t PendingFlushCount() {
    size_t count = 0;
    RunOnMuxerAndWait([&](auto* muxer) {
      for (auto& backend : muxer->producer_backends_)
        count += backend.producer->pending_flushes_.size();
    });
    return count;
  }

  // Calls ProducerImpl::Flush() for |instance_id| on the muxer thread, as if
  // the service had asked for it. Returns once the producer has handled it.
  static void FlushInstanceOnProducer(internal::TracingBackendId backend_id,
                                      DataSourceInstanceID instance_id,
                                      FlushRequestID flush_id) {
    RunOnMuxerAndWait([&](auto* muxer) {
      muxer->FindProducerBackendById(backend_id)
          ->producer->Flush(flush_id, &instance_id, 1, FlushFlags());
    });
  }

  // A v2 connection for |producer| with a real bridge on its own relay
  // thread. The caller closes and joins the relay when done.
  static std::shared_ptr<tracing_v2::RelaySequence> InstallTracingV2Connection(
      ProducerImpl* producer) {
    auto relay = CreateTestRelay();
    auto connection = std::make_shared<ProducerImpl::TracingV2Connection>();
    connection->endpoint = producer->service_;
    connection->bridge = tracing_v2::InProcessTracingV2Bridge::Create(
        relay, kTestNumChunks, kTestChunkSize);
    std::atomic_store(&producer->tracing_v2_connection_, std::move(connection));
    return relay;
  }

  void CheckConnectionWriterCreation(BufferExhaustedPolicy ring_policy) {
    base::TestTaskRunner muxer_runner;
    auto memory = InProcessSharedMemory::Create(4096);
    auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
    auto arbiter = SharedMemoryArbiter::CreateInstance(
        memory.get(), 4096, SharedMemoryABI::ShmemMode::kDefault,
        endpoint.get(), &muxer_runner);
    auto relay = CreateTestRelay();
    ProducerImpl::TracingV2Connection connection;
    connection.endpoint = endpoint;
    connection.bridge = tracing_v2::InProcessTracingV2Bridge::Create(
        relay, kTestNumChunks, kTestChunkSize);

    TraceWriter* downstream_writer = nullptr;
    EXPECT_CALL(*endpoint, CreateTraceWriter(11, BufferExhaustedPolicy::kDrop))
        .WillOnce([&](BufferID target, BufferExhaustedPolicy policy) {
          auto writer = arbiter->CreateTraceWriter(target, policy);
          downstream_writer = writer.get();
          return writer;
        });
    size_t committed_chunks = 0;
    EXPECT_CALL(*endpoint, CommitData(testing::_, testing::_))
        .WillRepeatedly([&](const CommitDataRequest& req, auto callback) {
          for (const auto& chunk : req.chunks_to_move()) {
            EXPECT_EQ(chunk.target_buffer(), 11u);
            ++committed_chunks;
          }
          if (callback)
            callback();
        });

    auto writer = connection.CreateTraceWriter(11, ring_policy);
    ASSERT_NE(downstream_writer, nullptr);
    EXPECT_NE(writer.get(), downstream_writer);
    EXPECT_EQ(writer->writer_id(), downstream_writer->writer_id());
    // The writer must be registered synchronously, not by a relay task:
    // nothing has been posted yet.
    {
      std::lock_guard<std::mutex> lock(connection.bridge->mutex_);
      auto* entry = connection.bridge->writers_.Find(writer->writer_id());
      ASSERT_NE(entry, nullptr);
      EXPECT_EQ((*entry)->v1_writer.get(), downstream_writer);
      EXPECT_EQ((*entry)->target_buffer, 11u);
    }

    // Write from another thread right away. If the writer had the wrong
    // WriterID or target buffer, the bridge would drop the chunk instead of
    // committing it.
    auto flushed = muxer_runner.CreateCheckpoint("flushed");
    std::thread publisher([&] {
      writer->NewTracePacket()->set_timestamp(123);
      writer->Flush([&] { muxer_runner.PostTask(flushed); });
    });
    publisher.join();
    muxer_runner.RunUntilCheckpoint("flushed");
    EXPECT_EQ(committed_chunks, 1u);

    writer.reset();
    auto writer_destroyed = muxer_runner.CreateCheckpoint("writer_destroyed");
    connection.bridge->DrainPendingData(
        [&] { muxer_runner.PostTask(writer_destroyed); });
    muxer_runner.RunUntilCheckpoint("writer_destroyed");
    relay->Close().reset();
  }

  static void CheckConnectionSizing(uint32_t requested_chunk_size,
                                    size_t capacity,
                                    uint32_t expected_chunk_size,
                                    uint32_t expected_num_chunks) {
    RunOnMuxerAndWait([&](auto* muxer) {
      ProducerImpl producer(muxer, 0, 0, false);
      auto memory = InProcessSharedMemory::Create(capacity);
      auto endpoint =
          std::make_shared<testing::StrictMock<MockProducerEndpoint>>();
      producer.service_ = endpoint;
      EXPECT_CALL(*endpoint, shared_memory())
          .WillOnce(testing::Return(memory.get()));
      EXPECT_CALL(*endpoint, tracing_v2_chunk_size_bytes())
          .WillOnce(testing::Return(requested_chunk_size));
      producer.EnsureTracingV2Connection();
      auto bridge = producer.tracing_v2_connection_->bridge;
      tracing_v2::TraceWriterV2::Delegate& delegate = *bridge;
      EXPECT_EQ(delegate.ring_buffer().chunk_size(), expected_chunk_size);
      EXPECT_EQ(delegate.ring_buffer().num_chunks(), expected_num_chunks);
      // A second call is a no-op and does not touch the endpoint (StrictMock).
      producer.EnsureTracingV2Connection();
      EXPECT_EQ(producer.tracing_v2_connection_->bridge, bridge);
    });
  }

  static void CreateInvalidConnection(uint32_t requested_chunk_size,
                                      size_t capacity = 4096) {
    ProducerImpl producer(nullptr, 0, 0, false);
    auto memory = InProcessSharedMemory::Create(capacity);
    auto endpoint =
        std::make_shared<testing::StrictMock<MockProducerEndpoint>>();
    producer.service_ = endpoint;
    EXPECT_CALL(*endpoint, shared_memory())
        .WillOnce(testing::Return(memory.get()));
    EXPECT_CALL(*endpoint, tracing_v2_chunk_size_bytes())
        .WillOnce(testing::Return(requested_chunk_size));
    producer.EnsureTracingV2Connection();
  }

  static void SetupStartupInstanceOnV2Connection() {
    RunOnMuxerAndWait([](auto* muxer) {
      for (auto& backend : muxer->producer_backends_) {
        if (!backend.producer->tracing_v2_connection_)
          continue;
        for (const auto& rds : muxer->data_sources_) {
          if (rds.descriptor.name() != "tracing_v2_flushing")
            continue;
          DataSourceConfig config;
          config.set_name(rds.descriptor.name());
          muxer->SetupDataSourceImpl(
              rds, backend.id, backend.producer->connection_id_.load(),
              /*instance_id=*/0, config, /*startup_session_id=*/1);
        }
      }
    });
  }

  static void WaitForFullRing(tracing_v2::InProcessTracingV2Bridge* bridge) {
    auto* header =
        static_cast<tracing_v2::RingBufferHeader*>(bridge->ring_memory_.Get());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!header->num_writers_waiting.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    EXPECT_GT(header->num_writers_waiting.load(std::memory_order_relaxed), 0u);
    const uint64_t positions =
        header->rw_positions.load(std::memory_order_acquire);
    EXPECT_EQ(
        tracing_v2::NumOutstandingPositions(tracing_v2::WritePosOf(positions),
                                            tracing_v2::ReadPosOf(positions)),
        bridge->ring_buffer_.num_chunks());
  }
};

TEST_F(TracingMuxerImplV2Test, InvalidConfiguredChunkSizeIsFatal) {
  if (!tracing_v2::SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(CreateInvalidConnection(252),
                            "tracing_v2_chunk_size_bytes=252 must be a "
                            "multiple of 4 between 256 and 32768 bytes");
  EXPECT_DEATH_IF_SUPPORTED(CreateInvalidConnection(258),
                            "tracing_v2_chunk_size_bytes=258 must be a "
                            "multiple of 4 between 256 and 32768 bytes");
  EXPECT_DEATH_IF_SUPPORTED(CreateInvalidConnection(32772),
                            "tracing_v2_chunk_size_bytes=32772 must be a "
                            "multiple of 4 between 256 and 32768 bytes");
  EXPECT_DEATH_IF_SUPPORTED(
      CreateInvalidConnection(8192),
      "tracing_v2_chunk_size_bytes=8192 requires at least one chunk to fit in "
      "the shared memory buffer, got 4096 bytes");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

TEST_F(TracingMuxerImplV2Test, StartupReservationPreventsV2Connection) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        ProducerImpl producer(nullptr, 0, 0, false);
        // SetupDataSourceImpl increments this before handing out a startup
        // writer. It remains nonzero after adoption on the same connection.
        producer.last_startup_target_buffer_reservation_ = 1;
        producer.EnsureTracingV2Connection();
      },
      "cannot share a producer connection with startup tracing");
}

TEST_F(TracingMuxerImplV2Test, ConnectionCreatesDropWriter) {
  CheckConnectionWriterCreation(BufferExhaustedPolicy::kDrop);
}

TEST_F(TracingMuxerImplV2Test,
       ConnectionCreatesStallingWriterWithDropDownstream) {
  CheckConnectionWriterCreation(BufferExhaustedPolicy::kStall);
}

TEST_F(TracingMuxerImplV2Test,
       ConnectionCreatesStallThenDropWriterWithDropDownstream) {
  CheckConnectionWriterCreation(BufferExhaustedPolicy::kStallThenDrop);
}

TEST_F(TracingMuxerImplV2Test, ConnectionPreservesInvalidDownstreamWriter) {
  auto endpoint = std::make_shared<testing::StrictMock<MockProducerEndpoint>>();
  auto relay = CreateTestRelay();
  ProducerImpl::TracingV2Connection connection;
  connection.endpoint = endpoint;
  connection.bridge = tracing_v2::InProcessTracingV2Bridge::Create(
      relay, kTestNumChunks, kTestChunkSize);
  auto null_writer = std::make_unique<NullTraceWriter>();
  auto* null_writer_ptr = null_writer.get();
  EXPECT_CALL(*endpoint, CreateTraceWriter(22, BufferExhaustedPolicy::kDrop))
      .WillOnce(testing::Return(testing::ByMove(std::move(null_writer))));

  auto writer = connection.CreateTraceWriter(22, BufferExhaustedPolicy::kStall);
  EXPECT_EQ(writer.get(), null_writer_ptr);
  EXPECT_EQ(writer->writer_id(), 0u);
  writer->NewTracePacket()->set_timestamp(123);
  writer->Flush();
  tracing_v2::TraceWriterV2::Delegate& delegate = *connection.bridge;
  EXPECT_EQ(delegate.ring_buffer().LoadWritePos(), 0u);
  writer.reset();
  relay->Close().reset();
}

// With a real arbiter, checks that the v1 commit is posted to the muxer before
// the barrier completion posts its own notification. The muxer runner is only
// run after each barrier has completed.
TEST_F(TracingMuxerImplV2Test,
       BridgeQueuesCommitBeforeControlAndWriterDestruction) {
  base::TestTaskRunner muxer_runner;
  auto memory = InProcessSharedMemory::Create(8192);
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto arbiter = SharedMemoryArbiter::CreateInstance(
      memory.get(), 4096, SharedMemoryABI::ShmemMode::kDefault, endpoint.get(),
      &muxer_runner);
  auto relay = CreateTestRelay();
  ProducerImpl::TracingV2Connection connection;
  connection.endpoint = endpoint;
  connection.bridge = tracing_v2::InProcessTracingV2Bridge::Create(
      relay, kTestNumChunks, kTestChunkSize);
  EXPECT_CALL(*endpoint, CreateTraceWriter(11, BufferExhaustedPolicy::kDrop))
      .WillOnce([&](BufferID target, BufferExhaustedPolicy policy) {
        return arbiter->CreateTraceWriter(target, policy);
      });
  auto writer = connection.CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  muxer_runner.RunUntilIdle();

  std::vector<std::string> operations;
  EXPECT_CALL(*endpoint, CommitData(testing::_, testing::_))
      .Times(2)
      .WillRepeatedly([&](const CommitDataRequest& req, auto callback) {
        ASSERT_EQ(req.chunks_to_move_size(), 1);
        EXPECT_EQ(req.chunks_to_move()[0].target_buffer(), 11u);
        EXPECT_FALSE(callback);
        operations.push_back("commit");
      });
  EXPECT_CALL(*endpoint, NotifyDataSourceStopped(42)).WillOnce([&](auto) {
    operations.push_back("stop");
  });
  EXPECT_CALL(*endpoint, UnregisterTraceWriter(writer->writer_id()))
      .WillOnce([&](auto) { operations.push_back("unregister"); });

  writer->NewTracePacket()->set_timestamp(123);
  writer->FinishTracePacket();
  bool completed = false;
  connection.bridge->DrainPendingData([&] {
    muxer_runner.PostTask([&] { endpoint->NotifyDataSourceStopped(42); });
    completed = true;
  });
  base::WaitableEvent barrier_posted;
  ASSERT_TRUE(relay->PostTask([&] { barrier_posted.Notify(); }));
  barrier_posted.Wait();
  EXPECT_TRUE(completed);
  EXPECT_TRUE(operations.empty());
  muxer_runner.RunUntilIdle();
  EXPECT_THAT(operations, testing::ElementsAre("commit", "stop"));

  writer->NewTracePacket()->set_timestamp(456);
  writer.reset();
  completed = false;
  connection.bridge->DrainPendingData([&] {
    muxer_runner.PostTask([&] { operations.push_back("writer_destroyed"); });
    completed = true;
  });
  base::WaitableEvent destruction_posted;
  ASSERT_TRUE(relay->PostTask([&] { destruction_posted.Notify(); }));
  destruction_posted.Wait();
  EXPECT_TRUE(completed);
  muxer_runner.RunUntilIdle();
  EXPECT_THAT(operations,
              testing::ElementsAre("commit", "stop", "commit", "unregister",
                                   "writer_destroyed"));
  relay->Close().reset();
}

TEST_F(TracingMuxerImplV2Test, PendingFlushReleasesWriterBeforeEndpoint) {
  base::TestTaskRunner service_runner;
  auto service = TracingService::CreateInstance(
      std::make_unique<InProcessSharedMemory::Factory>(), &service_runner,
      TracingService::InitOpts{});
  testing::NiceMock<MockProducer> callbacks(&service_runner);
  auto endpoint = service->ConnectProducer(
      &callbacks, ClientIdentity(0, 0), "lifetime", 4096, true,
      TracingService::ProducerSMBScrapingMode::kEnabled, 4096,
      InProcessSharedMemory::Create(4096));
  auto* arbiter = endpoint->MaybeSharedMemoryArbiter();
  ASSERT_NE(arbiter, nullptr);
  service_runner.RunUntilIdle();

  auto relay = CreateTestRelay();
  auto bridge = tracing_v2::InProcessTracingV2Bridge::Create(
      relay, kTestNumChunks, kTestChunkSize);
  std::weak_ptr<tracing_v2::InProcessTracingV2Bridge> weak_bridge = bridge;
  bool endpoint_destroyed = false;
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = std::shared_ptr<ProducerEndpoint>(
      endpoint.release(), [&](ProducerEndpoint* ep) {
        // The endpoint dies last: the bridge and its v1 writers must be gone.
        EXPECT_TRUE(weak_bridge.expired());
        const bool writers_released =
            ep->MaybeSharedMemoryArbiter()->TryShutdown();
        EXPECT_TRUE(writers_released);
        PERFETTO_CHECK(writers_released);
        delete ep;
        endpoint_destroyed = true;
      });
  producer->tracing_v2_connection_ =
      std::make_shared<ProducerImpl::TracingV2Connection>();
  producer->tracing_v2_connection_->endpoint = producer->service_;
  producer->tracing_v2_connection_->bridge = bridge;
  auto writer = producer->tracing_v2_connection_->CreateTraceWriter(
      1, BufferExhaustedPolicy::kDrop);
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  auto& pending = producer->pending_flushes_[1];
  pending.bridge_to_drain = bridge;
  pending.ring_drain_started = true;
  bool completed = false;
  bridge->DrainPendingData([&] { completed = true; });
  base::WaitableEvent barrier_posted;
  ASSERT_TRUE(relay->PostTask([&] { barrier_posted.Notify(); }));
  barrier_posted.Wait();
  // The service queue still holds the commit task, but the barrier is done.
  EXPECT_TRUE(completed);
  EXPECT_FALSE(arbiter->TryShutdown());

  auto joining_runner = relay->Close();
  writer.reset();  // The v1 writer stays alive until bridge destruction.
  joining_runner.reset();
  bridge.reset();
  EXPECT_FALSE(weak_bridge.expired());
  producer.reset();
  EXPECT_TRUE(endpoint_destroyed);
  EXPECT_TRUE(weak_bridge.expired());
  service_runner.RunUntilIdle();
  EXPECT_TRUE(completed);
}

// Without a v2 connection ProducerImpl acks flushes the way v1 always did.
// Asynchronous completions are acked in order, coalesced into the newest
// completed request. A request whose data sources all completed synchronously
// is acked at once, regardless of older requests still in flight: the service
// takes NotifyFlushComplete(N) as an ack for every request up to N, so those
// older requests are dropped locally too and a late completion for one of
// them is ignored.
TEST_F(TracingMuxerImplV2Test, NeverV2ConnectionKeepsV1FlushAcks) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  // There is no muxer, hence no OnFlush() to run. Set up the bookkeeping of
  // asynchronous flushes the way the v1 Flush() path leaves it.
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->pending_flushes_[2].pending_data_sources.insert(12);

  testing::InSequence in_sequence;
  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  // Request 2 completes first but has to wait for 1.
  producer->NotifyFlushForDataSourceDone(12, 2);
  producer->NotifyFlushForDataSourceDone(11, 1);
  EXPECT_TRUE(producer->pending_flushes_.empty());

  producer->pending_flushes_[3].pending_data_sources.insert(13);
  EXPECT_CALL(*endpoint, NotifyFlushComplete(4));
  producer->Flush(4, nullptr, 0, FlushFlags());
  EXPECT_TRUE(producer->pending_flushes_.empty());
  // Nothing left to ack for request 3.
  producer->NotifyFlushForDataSourceDone(13, 3);
  EXPECT_FALSE(producer->tracing_v2_connection_);
}

// A v1 request that a newer synchronous one has already acked (cumulatively)
// must not stall the ordered queue once the connection selects v2, and its
// late completion is a no-op.
TEST_F(TracingMuxerImplV2Test, CumulativelyAckedV1FlushDoesNotBlockV2Queue) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);

  testing::InSequence in_sequence;
  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  EXPECT_CALL(*endpoint, NotifyFlushComplete(3));
  producer->Flush(2, nullptr, 0, FlushFlags());

  auto relay = InstallTracingV2Connection(producer.get());
  producer->Flush(3, nullptr, 0, FlushFlags());
  EXPECT_TRUE(producer->pending_flushes_.empty());
  producer->NotifyFlushForDataSourceDone(11, 1);
  producer.reset();
  relay->Close().reset();
}

// Requests of a disposed connection can't be acked anymore and must not be
// carried over to the next connection, with or without a v2 bridge.
TEST_F(TracingMuxerImplV2Test, DisposeConnectionDropsPendingFlushes) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->DisposeConnection();
  EXPECT_TRUE(producer->pending_flushes_.empty());
  EXPECT_FALSE(producer->service_);

  producer->service_ = endpoint;
  auto relay = InstallTracingV2Connection(producer.get());
  std::weak_ptr<tracing_v2::InProcessTracingV2Bridge> weak_bridge =
      producer->tracing_v2_connection_->bridge;
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->pending_flushes_[2].bridge_to_drain = weak_bridge.lock();
  producer->DisposeConnection();
  EXPECT_TRUE(producer->pending_flushes_.empty());
  EXPECT_FALSE(producer->tracing_v2_connection_);
  EXPECT_TRUE(weak_bridge.expired());
  relay->Close().reset();
}

// A v1 asynchronous flush still pending when the connection selects v2 is
// consumed by the ordered path: a later request queues behind it instead of
// overtaking it, and its completion acks both.
TEST_F(TracingMuxerImplV2Test, PendingV1FlushIsConsumedByTheV2Queue) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);

  auto relay = InstallTracingV2Connection(producer.get());
  EXPECT_CALL(*endpoint, NotifyFlushComplete(testing::_)).Times(0);
  producer->Flush(2, nullptr, 0, FlushFlags());
  EXPECT_EQ(producer->pending_flushes_.size(), 2u);
  testing::Mock::VerifyAndClearExpectations(endpoint.get());

  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  producer->NotifyFlushForDataSourceDone(11, 1);
  EXPECT_TRUE(producer->pending_flushes_.empty());
  producer.reset();
  relay->Close().reset();
}

}  // namespace perfetto::test

namespace perfetto {
namespace internal {
namespace {

using Internals = test::TracingMuxerImplInternalsForTest;

// End-to-end through the in-process backend. The bridge emits exactly the
// packets a v1 writer would, so these tests look at the muxer's per-instance
// state to tell v1 from v2.

class TracingV2TestDataSource
    : public perfetto::DataSource<TracingV2TestDataSource> {
 public:
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
};

// Writes 2 MiB from OnFlush(), i.e. on the muxer thread, which the bridge's v1
// writers also need for their commits.
class TracingV2PressureDataSource
    : public perfetto::DataSource<TracingV2PressureDataSource> {
 public:
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;
  static std::atomic<uint64_t> drops;
  static std::atomic<base::WaitableEvent*> completed;
  void OnFlush(const FlushArgs&) override {
    const std::string payload(4096, 'p');
    for (uint32_t i = 0; i < 512; ++i) {
      Trace([&](TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str(payload);
      });
    }
    Trace([](TraceContext ctx) { drops.store(ctx.drop_count()); });
    if (auto* done = completed.exchange(nullptr))
      done->Notify();
  }
};
std::atomic<uint64_t> TracingV2PressureDataSource::drops{0};
std::atomic<base::WaitableEvent*> TracingV2PressureDataSource::completed{
    nullptr};

// No OnFlush() override, so Register() marks it no_flush.
class TracingV2NoFlushDataSource
    : public perfetto::DataSource<TracingV2NoFlushDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// Counts OnFlush() calls and writes a packet from inside the callback.
class TracingV2FlushingDataSource
    : public perfetto::DataSource<TracingV2FlushingDataSource> {
 public:
  static std::atomic<uint32_t> flushes;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
  void OnFlush(const FlushArgs&) override {
    ++flushes;
    Trace([](TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("on flush");
    });
    if (base::WaitableEvent* done = on_flush_done.load())
      done->Notify();
  }

  // Set by a test that needs to know the producer has run this callback.
  static std::atomic<base::WaitableEvent*> on_flush_done;
};
std::atomic<uint32_t> TracingV2FlushingDataSource::flushes{0};
std::atomic<base::WaitableEvent*> TracingV2FlushingDataSource::on_flush_done{
    nullptr};

// Overrides OnFlush() but is registered with no_flush: the service must never
// call it.
class TracingV2DeclaredNoFlushDataSource
    : public perfetto::DataSource<TracingV2DeclaredNoFlushDataSource> {
 public:
  static std::atomic<uint32_t> flushes;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
  void OnFlush(const FlushArgs&) override { ++flushes; }
};
std::atomic<uint32_t> TracingV2DeclaredNoFlushDataSource::flushes{0};

// Completes its stop asynchronously, when the test says so.
class TracingV2AsyncStopDataSource
    : public perfetto::DataSource<TracingV2AsyncStopDataSource> {
 public:
  static std::atomic<base::WaitableEvent*> stop_started;
  static std::mutex mutex;

  // Function-local so this file needs no exit-time destructor.
  static std::function<void()>& pending_stop() {
    static base::NoDestructor<std::function<void()>> instance;
    return instance.ref();
  }

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs& args) override { HoldStop(args); }

  static void HoldStop(const StopArgs& args) {
    base::WaitableEvent* started = stop_started.load();
    if (!started)
      return;
    std::function<void()> done = args.HandleStopAsynchronously();
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending_stop() = std::move(done);
    }
    started->Notify();
  }

  static void CompleteHeldStop() {
    stop_started.store(nullptr);
    std::function<void()> done;
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = std::move(pending_stop());
      pending_stop() = nullptr;
    }
    ASSERT_TRUE(done);
    done();
  }
};
std::atomic<base::WaitableEvent*> TracingV2AsyncStopDataSource::stop_started{
    nullptr};
std::mutex TracingV2AsyncStopDataSource::mutex;

// Holds one OnFlush() until the test releases it, and writes a packet right
// before completing it. Only the flush the test armed is held.
class TracingV2AsyncFlushDataSource
    : public perfetto::DataSource<TracingV2AsyncFlushDataSource> {
 public:
  // Armed by the test. While null, OnFlush() completes synchronously.
  static std::atomic<base::WaitableEvent*> hold_next_flush;
  static std::mutex mutex;

  // Function-local so this file needs no exit-time destructor.
  static std::function<void()>& pending_flush() {
    static base::NoDestructor<std::function<void()>> instance;
    return instance.ref();
  }

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs& args) override {
    TracingV2AsyncStopDataSource::HoldStop(args);
  }
  void OnFlush(const FlushArgs& args) override {
    base::WaitableEvent* started = hold_next_flush.load();
    if (!started)
      return;
    std::function<void()> done = args.HandleFlushAsynchronously();
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending_flush() = std::move(done);
    }
    started->Notify();
  }

  static void CompleteHeldFlush() {
    hold_next_flush.store(nullptr);
    // An asynchronous callback is allowed to write right up to the moment it
    // reports completion, so this packet has to be in the flushed trace.
    Trace([](TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("async flush");
    });
    std::function<void()> done;
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = std::move(pending_flush());
      pending_flush() = nullptr;
    }
    ASSERT_TRUE(done);
    done();
  }
};
std::atomic<base::WaitableEvent*>
    TracingV2AsyncFlushDataSource::hold_next_flush{nullptr};
std::mutex TracingV2AsyncFlushDataSource::mutex;

// Number of instances of |DerivedDataSource| that the muxer put on v2.
template <typename DerivedDataSource>
size_t CountInstancesUsingTracingV2() {
  DataSourceStaticState* static_state =
      perfetto::DataSourceHelper<DerivedDataSource>::type().static_state();
  size_t count = 0;
  for (size_t i = 0; i < kMaxDataSourceInstances; ++i) {
    DataSourceState* state = static_state->TryGet(i);
    if (state && state->use_tracing_v2)
      ++count;
  }
  return count;
}

// Runs |body| in a fresh copy of this process: the death test re-executes the
// binary for this one test, so the child's producer connection has never seen
// v2, whatever ran before in the parent. It is also the only way to test
// Tracing::Shutdown(), which is terminal for the process. A failed assertion
// in |body| fails the child, and is repeated on stderr because that is the
// only output of the child the parent shows.
void RunInFreshProcess(std::function<void()> body) {
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_EXIT(
      {
        body();
        const testing::TestResult* result =
            testing::UnitTest::GetInstance()->current_test_info()->result();
        for (int i = 0; i < result->total_part_count(); ++i) {
          const testing::TestPartResult& part = result->GetTestPartResult(i);
          if (part.failed()) {
            PERFETTO_ELOG("%s:%d: %s", part.file_name(), part.line_number(),
                          part.message());
          }
        }
        std::_Exit(result->Failed() ? 1 : 0);
      },
      testing::ExitedWithCode(0), "");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

// Initialize()/ResetForTesting() per test does not work with the fake backend
// in this binary, so the suite initializes once.
class TracingV2InProcessTest : public test::TracingMuxerImplV2Test {
 protected:
  static void SetUpTestSuite() {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      return;
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    dsd.set_name("tracing_v2_pressure");
    TracingV2PressureDataSource::Register(dsd);
    dsd.set_name("tracing_v2_no_flush");
    TracingV2NoFlushDataSource::Register(dsd);
    dsd.set_name("tracing_v2_flushing");
    TracingV2FlushingDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_flush");
    TracingV2AsyncFlushDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_stop");
    TracingV2AsyncStopDataSource::Register(dsd);
    dsd.set_name("tracing_v2_declared_no_flush");
    dsd.set_no_flush(true);
    TracingV2DeclaredNoFlushDataSource::Register(dsd);
    // TracingMuxerImpl only accepts a fixed set of interceptor names, and
    // "test_interceptor" already belongs to another suite in this binary.
    perfetto::ConsoleInterceptor::Register();
    perfetto::test::SyncProducers();
    perfetto::test::DisableReconnectLimit();
  }

  static void TearDownTestSuite() {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      return;
    // The data source's thread-local state points at muxer state that
    // ResetForTesting() is about to destroy.
    Internals::ClearDataSourceTlsStateOnReset<TracingV2TestDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2NoFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2FlushingDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2AsyncFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2AsyncStopDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<
        TracingV2DeclaredNoFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2PressureDataSource>();
    perfetto::Tracing::ResetForTesting();
  }

  void SetUp() override {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";
  }

  void TearDown() override {
    TracingV2AsyncFlushDataSource::hold_next_flush.store(nullptr);
    TracingV2FlushingDataSource::on_flush_done.store(nullptr);
    TracingV2AsyncStopDataSource::stop_started.store(nullptr);
  }

  static bool PostToRelay(std::function<void()> task) {
    // Establish that setup completed before obtaining the atomic relay
    // snapshot.
    WaitForMuxerSequence();
    return Internals::PostToTracingV2Relay(std::move(task));
  }

  // Returns once everything already queued on the muxer sequence has run.
  static void WaitForMuxerSequence() {
    base::WaitableEvent done;
    Internals::PostToMuxerSequence([&done] { done.Notify(); });
    done.Wait();
  }

  static bool HasRelay() {
    WaitForMuxerSequence();
    return Internals::HasTracingV2Relay();
  }

  enum class WriterSelection { kAbsent, kV1, kV2 };

  // All tests share one producer connection, and a connection that once
  // selected v2 stays on the v2 control path until it disconnects. Tests that
  // rely on that path call this first, so that they don't depend on the tests
  // that ran before them.
  static void PutConnectionOnTracingV2() {
    StopAndParse(
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2))
            .get());
  }

  // Flushes the only active instance of |DerivedDataSource| the way an older
  // service would, i.e. without looking at no_flush.
  template <typename DerivedDataSource>
  static void FlushInstanceLikeAnOlderService(FlushRequestID flush_id) {
    DataSourceStaticState* static_state =
        perfetto::DataSourceHelper<DerivedDataSource>::type().static_state();
    DataSourceState* state = nullptr;
    for (size_t i = 0; i < kMaxDataSourceInstances; ++i) {
      if (DataSourceState* candidate = static_state->TryGet(i)) {
        ASSERT_EQ(state, nullptr);
        state = candidate;
      }
    }
    ASSERT_NE(state, nullptr);
    FlushInstanceOnProducer(state->backend_id, state->data_source_instance_id,
                            flush_id);
  }

  static perfetto::TraceConfig MakeConfigFor(
      std::initializer_list<const char*> data_source_names,
      WriterSelection selection) {
    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024);
    for (const char* name : data_source_names) {
      auto* ds_cfg = cfg.add_data_sources()->mutable_config();
      ds_cfg->set_name(name);
      if (selection != WriterSelection::kAbsent)
        ds_cfg->set_use_tracing_v2(selection == WriterSelection::kV2);
    }
    return cfg;
  }

  static perfetto::TraceConfig MakeConfig() {
    return MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2);
  }

  static std::unique_ptr<perfetto::TracingSession> StartSession(
      const perfetto::TraceConfig& cfg) {
    auto session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
    session->Setup(cfg);
    session->StartBlocking();
    return session;
  }

  static std::vector<protos::gen::TracePacket> ReadTestPackets(
      perfetto::TracingSession* session) {
    const std::vector<char> bytes = session->ReadTraceBlocking();
    protos::gen::Trace trace;
    EXPECT_TRUE(trace.ParseFromArray(bytes.data(), bytes.size()));
    return TestPackets(trace);
  }

  static perfetto::TraceConfig MakeStallingConfig() {
    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024);
    auto* ds_cfg = cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("tracing_v2_test");
    ds_cfg->set_use_tracing_v2(true);
    ds_cfg->set_buffer_exhausted_policy(
        perfetto::protos::gen::DataSourceConfig::
            BUFFER_EXHAUSTED_STALL_THEN_DROP);
    return cfg;
  }

  static protos::gen::Trace StopAndParse(perfetto::TracingSession* session) {
    session->StopBlocking();
    const std::vector<char> bytes = session->ReadTraceBlocking();
    protos::gen::Trace trace;
    EXPECT_TRUE(trace.ParseFromArray(bytes.data(), bytes.size()));
    return trace;
  }

  static std::vector<protos::gen::TracePacket> TestPackets(
      const protos::gen::Trace& trace) {
    std::vector<protos::gen::TracePacket> out;
    for (const protos::gen::TracePacket& packet : trace.packet()) {
      if (packet.has_for_testing())
        out.push_back(packet);
    }
    return out;
  }
  static void CheckFlushFifo(WriterSelection selection) {
    base::WaitableEvent older_flush_held;
    base::WaitableEvent later_flush_ran;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&older_flush_held);
    TracingV2FlushingDataSource::on_flush_done.store(&later_flush_ran);
    auto older =
        StartSession(MakeConfigFor({"tracing_v2_async_flush"}, selection));
    auto later =
        StartSession(MakeConfigFor({"tracing_v2_flushing"}, selection));

    std::mutex mutex;
    std::vector<std::string> completions;  // Guarded by |mutex|.
    bool older_succeeded = false;          // Guarded by |mutex|.
    bool later_succeeded = false;          // Guarded by |mutex|.
    base::WaitableEvent older_done;
    base::WaitableEvent later_done;

    older->Flush(
        [&](bool success) {
          std::lock_guard<std::mutex> lock(mutex);
          completions.push_back("older");
          older_succeeded = success;
          older_done.Notify();
        },
        /*timeout_ms=*/30000);
    // The older request is now inside its data source's OnFlush(), which will
    // not report completion until this test says so.
    older_flush_held.Wait();

    later->Flush(
        [&](bool success) {
          std::lock_guard<std::mutex> lock(mutex);
          completions.push_back("later");
          later_succeeded = success;
          later_done.Notify();
        },
        /*timeout_ms=*/30000);
    // The later request's OnFlush() is done, but its ring barrier cannot start
    // while the older request remains at the front of the queue.
    later_flush_ran.Wait();

    // Round-trip the producer and the in-process service sequence so anything
    // that was going to complete has.
    perfetto::test::SyncProducers();
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_TRUE(completions.empty());
    }

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    older_done.Wait();
    later_done.Wait();
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_EQ(completions, (std::vector<std::string>{"older", "later"}));
      EXPECT_TRUE(older_succeeded);
      EXPECT_TRUE(later_succeeded);
    }

    // The packet the held callback wrote just before completing is in the trace
    // its flush was waiting for.
    const auto packets = ReadTestPackets(older.get());
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].for_testing().str(), "async flush");
    older->StopBlocking();
    later->StopBlocking();
  }

  static void CheckStoppedFlush(WriterSelection selection) {
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    auto older =
        StartSession(MakeConfigFor({"tracing_v2_async_flush"}, selection));
    std::atomic<bool> older_done{false};
    older->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          older_done.store(true);
        },
        30000);
    held.Wait();
    older->StopBlocking();

    auto later =
        StartSession(MakeConfigFor({"tracing_v2_flushing"}, selection));
    EXPECT_TRUE(later->FlushBlocking(30000));
    // The held OnFlush() has not completed. Stopping the instance must release
    // the flush on its own.
    EXPECT_TRUE(older_done.load());
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
    EXPECT_TRUE(later->FlushBlocking(30000));
    later->StopBlocking();
  }
  static void CheckMuxerPressure(
      protos::gen::DataSourceConfig::BufferExhaustedPolicy policy) {
    auto cfg = MakeConfigFor({"tracing_v2_pressure"}, WriterSelection::kV2);
    cfg.mutable_data_sources()
        ->at(0)
        .mutable_config()
        ->set_buffer_exhausted_policy(policy);
    auto session = StartSession(cfg);
    auto bridge = GetBridge();
    ASSERT_TRUE(bridge);
    base::WaitableEvent relay_held;
    base::WaitableEvent release_relay;
    ASSERT_TRUE(PostToRelay([&] {
      relay_held.Notify();
      release_relay.Wait();
    }));
    relay_held.Wait();
    base::WaitableEvent produced;
    TracingV2PressureDataSource::completed.store(&produced);
    base::WaitableEvent flushed;
    session->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          flushed.Notify();
        },
        30000);
    if (policy == protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_DROP) {
      // A dropping SDK writer finishes its callback while the relay is held.
      produced.Wait();
      EXPECT_GT(TracingV2PressureDataSource::drops.load(), 0u);
    } else {
      // With the relay blocked, the muxer thread stalls on the full ring.
      WaitForFullRing(bridge.get());
    }
    release_relay.Notify();
    // OnFlush() writes 2 MiB without returning to the muxer thread, so the
    // 256 KiB v1 SMB fills up too. The relay must keep making progress
    // regardless, dropping on v1.
    flushed.Wait();
    WaitForMuxerSequence();
    if (policy ==
        protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_ABORT)
      EXPECT_EQ(TracingV2PressureDataSource::drops.load(), 0u);
    base::WaitableEvent tail_flushed;
    Internals::PostToMuxerSequence([&] {
      TracingV2PressureDataSource::Trace(
          [&](TracingV2PressureDataSource::TraceContext ctx) {
            ctx.NewTracePacket()->set_for_testing()->set_str("after pressure");
            ctx.Flush([&] { tail_flushed.Notify(); });
          });
    });
    tail_flushed.Wait();
    session->StopBlocking();
    const auto packets = ReadTestPackets(session.get());
    bool saw_loss = false;
    bool saw_tail = false;
    for (size_t i = 0; i < packets.size(); ++i) {
      const auto& packet = packets[i];
      // The service marks the first packet of every sequence as preceded by a
      // loss. Only a later marker proves recovery from this test's pressure.
      if (i != 0) {
        saw_loss |= (packet.previous_packet_dropped() &
                     protos::gen::TracePacket::DATA_LOSS_PRESENT) != 0;
      }
      saw_tail |= packet.for_testing().str() == "after pressure";
    }
    // The v1 service appends its own previous_packet_dropped value when it sees
    // a chunk gap. Protobuf last-value-wins decoding therefore hides the
    // bridge's reason bits (the bridge unittest checks those). Only check that
    // a loss was reported.
    EXPECT_TRUE(saw_loss);
    EXPECT_TRUE(saw_tail);
  }
};

// A previous test may already have created the process-wide relay. Check
// that selecting v1 does not create one and that its instance stays on v1.
TEST_F(TracingV2InProcessTest, V1ConfigDoesNotCreateRelayOrRing) {
  const bool had_relay = HasRelay();
  auto absent = StartSession(
      MakeConfigFor({"tracing_v2_test"}, WriterSelection::kAbsent));
  auto off =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV1));

  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
  EXPECT_EQ(HasRelay(), had_relay);

  StopAndParse(absent.get());
  StopAndParse(off.get());
}

// The check is on the config, before the interceptor name is even resolved.
TEST_F(TracingV2InProcessTest, TracingV2WithAnInterceptorIsFatal) {
  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("tracing_v2_test");
  ds_cfg->set_use_tracing_v2(true);
  ds_cfg->mutable_interceptor_config()->set_name("unknown");

  // The abort happens on the muxer sequence, so the death test has to re-exec
  // rather than fork: a forked child has no muxer thread to run the setup on.
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(StartSession(cfg),
                            "cannot be combined with an interceptor");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

// Interceptors without v2 keep working as before.
TEST_F(TracingV2InProcessTest, AnInterceptedInstanceWithoutTracingV2IsFine) {
  const bool had_relay = HasRelay();
  base::TempFile console_output = base::TempFile::Create();
  perfetto::ConsoleInterceptor::SetOutputFdForTesting(console_output.fd());

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("tracing_v2_test");
  ds_cfg->mutable_interceptor_config()->set_name("console");
  auto session = StartSession(cfg);

  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
  EXPECT_EQ(HasRelay(), had_relay);

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("intercepted");
    ctx.Flush();
  });

  // The packet went to the interceptor, not to the session's buffer.
  EXPECT_TRUE(TestPackets(StopAndParse(session.get())).empty());
  perfetto::ConsoleInterceptor::SetOutputFdForTesting(STDERR_FILENO);
}

TEST_F(TracingV2InProcessTest, V2ConnectionRejectsLaterStartupInstance) {
  const std::string previous_style = testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(
      {
        PutConnectionOnTracingV2();
        SetupStartupInstanceOnV2Connection();
      },
      "Startup tracing cannot share a producer connection");
  testing::GTEST_FLAG(death_test_style) = previous_style;
}

TEST_F(TracingV2InProcessTest, AV2ConfigCreatesTheRelayOnDemand) {
  auto session =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 1u);
  EXPECT_TRUE(HasRelay());
  StopAndParse(session.get());
}

TEST_F(TracingV2InProcessTest, TheConfigSelectsTheWriterPerInstance) {
  auto absent = StartSession(
      MakeConfigFor({"tracing_v2_test"}, WriterSelection::kAbsent));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);

  auto off =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV1));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);

  auto on =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 1u);

  // One Trace() call fans out to all three instances, each through the writer
  // its own config asked for and into its own session's buffer.
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("mixed");
    ctx.Flush();
  });

  for (perfetto::TracingSession* session :
       {absent.get(), off.get(), on.get()}) {
    const auto packets = TestPackets(StopAndParse(session));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].for_testing().str(), "mixed");
  }
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
}

class TracingV2ChunkSizeTest : public TracingV2InProcessTest {};

TEST_F(TracingV2ChunkSizeTest, ConfiguredSizeReachesRingAndRemainsFixed) {
  // Size resolution first, through the friend seam: default, explicit,
  // non-power-of-two and max chunk size.
  CheckConnectionSizing(0, 4096, 256, 16);
  CheckConnectionSizing(256, 4096, 256, 16);
  CheckConnectionSizing(260, 4096, 260, 8);
  CheckConnectionSizing(1024, 4096, 1024, 4);
  CheckConnectionSizing(32768, 32768, 32768, 1);

  auto cfg = MakeConfig();
  auto* producer_config = cfg.add_producers();
  producer_config->set_producer_name(
      Platform::GetDefaultPlatform()->GetCurrentProcessName());
  producer_config->set_shm_size_kb(64);
  producer_config->set_tracing_v2_chunk_size_bytes(1024);
  auto session = StartSession(cfg);
  auto bridge = GetBridge();
  ASSERT_NE(bridge, nullptr);
  tracing_v2::TraceWriterV2::Delegate& delegate = *bridge;
  EXPECT_EQ(delegate.ring_buffer().chunk_size(), 1024u);
  EXPECT_EQ(delegate.ring_buffer().num_chunks(), 64u);
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str(std::string(4096, 'c'));
  });
  auto packets = TestPackets(StopAndParse(session.get()));
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), std::string(4096, 'c'));
  session.reset();

  producer_config->set_tracing_v2_chunk_size_bytes(2048);
  producer_config->set_shm_size_kb(128);
  session = StartSession(cfg);
  EXPECT_EQ(GetBridge(), bridge);
  EXPECT_EQ(delegate.ring_buffer().chunk_size(), 1024u);
  EXPECT_EQ(delegate.ring_buffer().num_chunks(), 64u);
  StopAndParse(session.get());
}

TEST_F(TracingV2InProcessTest, EnabledProducesAValidTraceThroughTheRing) {
  auto session = StartSession(MakeConfig());
  auto bridge = GetBridge();
  ASSERT_NE(bridge, nullptr);
  tracing_v2::TraceWriterV2::Delegate& delegate = *bridge;
  EXPECT_EQ(delegate.ring_buffer().chunk_size(), 256u);

  for (uint32_t i = 0; i < 32; ++i) {
    TracingV2TestDataSource::Trace(
        [i](TracingV2TestDataSource::TraceContext ctx) {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(i);
          auto* event = packet->set_for_testing();
          event->set_str("v2");
          // Exercise the proto-group rewrite with a nested message.
          auto* payload = event->set_payload();
          payload->set_single_int(static_cast<int32_t>(i));
          payload->add_str("nested");
        });
  }
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), 32u);
  for (uint32_t i = 0; i < 32; ++i) {
    EXPECT_EQ(packets[i].timestamp(), i) << i;
    EXPECT_EQ(packets[i].for_testing().str(), "v2") << i;
    EXPECT_EQ(packets[i].for_testing().payload().single_int(),
              static_cast<int32_t>(i))
        << i;
    ASSERT_EQ(packets[i].for_testing().payload().str().size(), 1u) << i;
    EXPECT_EQ(packets[i].for_testing().payload().str()[0], "nested") << i;
  }
}

TEST_F(TracingV2InProcessTest, FlushCallbackWaitsForTheService) {
  auto session = StartSession(MakeConfig());

  base::WaitableEvent flush_complete;
  TracingV2TestDataSource::Trace(
      [&flush_complete](TracingV2TestDataSource::TraceContext ctx) {
        {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(1234);
          packet->set_for_testing()->set_str("flush");
        }
        ctx.Flush([&flush_complete] { flush_complete.Notify(); });
      });
  flush_complete.Wait();

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 1234u);
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, MuxerStallWithBothBuffersFullMakesProgress) {
  CheckMuxerPressure(
      protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_ABORT);
}

TEST_F(TracingV2InProcessTest,
       MuxerStallThenDropWithBothBuffersFullMakesProgress) {
  CheckMuxerPressure(
      protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_DROP);
}

TEST_F(TracingV2InProcessTest, MuxerDropUnderPressureMakesProgress) {
  CheckMuxerPressure(protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_DROP);
}

// The v1 hop drops, so the large packet may or may not make it. This only
// checks that we make progress and recover.
// PacketLargerThanRingReassemblesExactly checks the reassembly itself, with a
// fake v1 writer that never drops.
TEST_F(TracingV2InProcessTest, PacketLargerThanRingMakesProgressAndRecovers) {
  auto session = StartSession(MakeStallingConfig());

  // Establish the sequence before pressure: a loss marker on its first packet
  // could come from service initialization and would not prove recovery.
  base::WaitableEvent initial_flushed;
  TracingV2TestDataSource::Trace(
      [&](TracingV2TestDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("before large packet");
        ctx.Flush([&] { initial_flushed.Notify(); });
      });
  initial_flushed.Wait();

  // Twice the 256 KiB ring.
  const std::string payload(512 * 1024, 'z');
  TracingV2TestDataSource::Trace(
      [&payload](TracingV2TestDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(11);
        packet->set_for_testing()->set_str(payload);
      });
  base::WaitableEvent flushed;
  TracingV2TestDataSource::Trace(
      [&](TracingV2TestDataSource::TraceContext ctx) {
        ctx.Flush([&] { flushed.Notify(); });
      });
  flushed.Wait();
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("after large packet");
  });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_GE(packets.size(), 2u);
  EXPECT_EQ(packets.front().for_testing().str(), "before large packet");
  EXPECT_EQ(packets.back().for_testing().str(), "after large packet");
  if (packets.size() == 3) {
    EXPECT_EQ(packets[1].timestamp(), 11u);
    EXPECT_EQ(packets[1].for_testing().str(), payload);
  } else {
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_NE(packets[1].previous_packet_dropped() &
                  protos::gen::TracePacket::DATA_LOSS_PRESENT,
              0u);
  }

  // No reassembly error may show up. Not a full check: the v1 hop can drop the
  // packet carrying the reason bits.
  constexpr uint32_t kV2ReassemblyLoss =
      protos::gen::TracePacket::DATA_LOSS_ORPHAN_CONTINUATION |
      protos::gen::TracePacket::DATA_LOSS_REASSEMBLY_GAP |
      protos::gen::TracePacket::DATA_LOSS_CHUNK_CORRUPTED;
  for (const protos::gen::TracePacket& packet : trace.packet())
    EXPECT_EQ(packet.previous_packet_dropped() & kV2ReassemblyLoss, 0u);
}

TEST_F(TracingV2InProcessTest, SeveralThreadsWriteConcurrently) {
  auto session = StartSession(MakeConfig());

  constexpr uint32_t kNumThreads = 4;
  constexpr uint32_t kPacketsPerThread = 100;
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([t] {
      for (uint32_t i = 0; i < kPacketsPerThread; ++i) {
        TracingV2TestDataSource::Trace(
            [t, i](TracingV2TestDataSource::TraceContext ctx) {
              auto packet = ctx.NewTracePacket();
              packet->set_timestamp(t * 1000 + i);
              packet->set_for_testing()->set_str("mt");
            });
      }
      // Each thread has its own TLS writer. Flush so its open chunk gets
      // published.
      TracingV2TestDataSource::Trace(
          [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });
    });
  }
  for (std::thread& thread : threads)
    thread.join();

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), kNumThreads * kPacketsPerThread);
  // Per writer the order is guaranteed; across writers it is not, so check the
  // set rather than the sequence.
  std::set<uint64_t> timestamps;
  for (const protos::gen::TracePacket& packet : packets)
    timestamps.insert(packet.timestamp());
  EXPECT_EQ(timestamps.size(), kNumThreads * kPacketsPerThread);
}

TEST_F(TracingV2InProcessTest, TwoSessionsWithDifferentBuffersStayApart) {
  auto first = StartSession(MakeConfig());
  auto second = StartSession(MakeConfig());
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 2u);

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_timestamp(42);
    packet->set_for_testing()->set_str("both");
  });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  // Two instances of the data source means two writers with different target
  // buffers; each session must see its own copy and nothing else.
  const protos::gen::Trace second_trace = StopAndParse(second.get());
  const protos::gen::Trace first_trace = StopAndParse(first.get());
  ASSERT_EQ(TestPackets(first_trace).size(), 1u);
  ASSERT_EQ(TestPackets(second_trace).size(), 1u);
  EXPECT_EQ(TestPackets(first_trace)[0].for_testing().str(), "both");
  EXPECT_EQ(TestPackets(second_trace)[0].for_testing().str(), "both");
}

// The service normally scrapes no_flush data sources instead of flushing them.
// It can't scrape the ring, so for v2 it asks the producer anyway
// (RequiresProducerFlush()), and the tail must be there when the flush
// completes.
TEST_F(TracingV2InProcessTest,
       NoFlushDataSourceTailIsPresentWhenFlushCompletes) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV2));

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(1);
        packet->set_for_testing()->set_str("tail");
      });
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

  // Read before stopping: stopping would flush again on its own.
  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "tail");
  session->StopBlocking();
}

// A no_flush data source that stayed on v1 keeps its existing behaviour: the
// service never asks the producer, and the SMB scrape recovers the tail.
TEST_F(TracingV2InProcessTest, NoFlushDataSourceOnV1IsStillScraped) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV1));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2NoFlushDataSource>(), 0u);

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(1);
        packet->set_for_testing()->set_str("scraped tail");
      });
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "scraped tail");
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, FlushAndCloneSessionIncludesNoFlushTail) {
  perfetto::TraceConfig cfg =
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV2);
  cfg.set_unique_session_name("tracing_v2_no_flush_clone");
  auto session = StartSession(cfg);

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(2);
        packet->set_for_testing()->set_str("cloned tail");
      });

  // Cloning flushes the same set of instances a consumer flush would.
  auto clone = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  base::WaitableEvent cloned;
  bool clone_succeeded = false;
  perfetto::TracingSession::CloneTraceArgs clone_args;
  clone_args.unique_session_name = "tracing_v2_no_flush_clone";
  clone->CloneTrace(clone_args,
                    [&](perfetto::TracingSession::CloneTraceCallbackArgs args) {
                      clone_succeeded = args.success;
                      cloned.Notify();
                    });
  cloned.Wait();
  ASSERT_TRUE(clone_succeeded);

  const auto packets = ReadTestPackets(clone.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "cloned tail");
  session->StopBlocking();
}

// Flushing a v2 instance for the ring's sake must not call OnFlush() on a
// no_flush data source. One consumer flush: one OnFlush() for the flushing
// data source, none for the no_flush one, and all the data (including what
// OnFlush() wrote) in the service when it completes.
TEST_F(TracingV2InProcessTest, DeclaredNoFlushDataSourceGetsNoOnFlushCallback) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_flushing", "tracing_v2_declared_no_flush"},
                    WriterSelection::kV2));

  TracingV2FlushingDataSource::Trace(
      [](TracingV2FlushingDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("flushing");
      });
  TracingV2DeclaredNoFlushDataSource::Trace(
      [](TracingV2DeclaredNoFlushDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("declared");
      });

  const uint32_t flushing_before = TracingV2FlushingDataSource::flushes;
  const uint32_t declared_before = TracingV2DeclaredNoFlushDataSource::flushes;
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));
  EXPECT_EQ(TracingV2FlushingDataSource::flushes - flushing_before, 1u);
  EXPECT_EQ(TracingV2DeclaredNoFlushDataSource::flushes - declared_before, 0u);

  std::vector<std::string> strings;
  for (const protos::gen::TracePacket& packet : ReadTestPackets(session.get()))
    strings.push_back(packet.for_testing().str());
  std::sort(strings.begin(), strings.end());
  EXPECT_EQ(strings,
            (std::vector<std::string>{"declared", "flushing", "on flush"}));
  session->StopBlocking();
}

// Acking a flush implicitly acks all the older ones. So a request whose data
// sources complete synchronously must still wait behind an older request that
// is stuck in an async OnFlush(), or the older consumer flush would complete
// without the packets that callback is about to write.
TEST_F(TracingV2InProcessTest, LaterFlushDoesNotAcknowledgeAnOlderOne) {
  CheckFlushFifo(WriterSelection::kV2);
}

// Once the connection is on v2, requests made of v1 instances only take part
// in the same ordered queue. (A connection that never selected v2 keeps the
// v1 behaviour, where a synchronously completed request is acked at once; see
// TracingMuxerImplV2Test.NeverV2ConnectionKeepsV1FlushAcks.)
// The consumer timing out does not cancel the producer's outstanding callback.
// Later requests remain queued until that callback completes (or its source
// stops).
TEST_F(TracingV2InProcessTest, TimedOutFlushStillBlocksLaterProducerRequests) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV2));
    auto newer = StartSession(
        MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV2));
    base::WaitableEvent held;
    base::WaitableEvent timed_out;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    older->Flush(
        [&](bool success) {
          EXPECT_FALSE(success);
          timed_out.Notify();
        },
        /*timeout_ms=*/1);
    held.Wait();
    timed_out.Wait();
    EXPECT_EQ(PendingFlushCount(), 1u);

    base::WaitableEvent newer_handled;
    base::WaitableEvent newer_completed;
    std::atomic<bool> acknowledged{false};
    TracingV2FlushingDataSource::on_flush_done.store(&newer_handled);
    newer->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          acknowledged.store(true);
          newer_completed.Notify();
        },
        /*timeout_ms=*/30000);
    newer_handled.Wait();
    WaitForMuxerSequence();
    EXPECT_EQ(PendingFlushCount(), 2u);
    EXPECT_FALSE(acknowledged.load());
    TracingV2FlushingDataSource::on_flush_done.store(nullptr);

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    newer_completed.Wait();
    EXPECT_EQ(PendingFlushCount(), 0u);
    newer->StopBlocking();
    older->StopBlocking();
  });
}

TEST_F(TracingV2InProcessTest, V1FlushesOnAV2ConnectionAreFifo) {
  PutConnectionOnTracingV2();
  CheckFlushFifo(WriterSelection::kV1);
}

TEST_F(TracingV2InProcessTest, StoppedAsyncFlushDoesNotBlockLaterFlush) {
  CheckStoppedFlush(WriterSelection::kV2);
}

// A stopped v1 instance can be what an older request, queued in front of a v2
// barrier, is waiting for. Its stop must release that request.
TEST_F(TracingV2InProcessTest,
       StoppedV1AsyncFlushOnAV2ConnectionDoesNotBlockLaterFlush) {
  PutConnectionOnTracingV2();
  CheckStoppedFlush(WriterSelection::kV1);
}

// A request made of v1 instances only doesn't need the ring, but on a v2
// connection it is acked in order: block the relay so that an older v2 request
// cannot complete, and check that a later v1 request waits for it.
TEST_F(TracingV2InProcessTest, V1FlushOnAV2ConnectionWaitsBehindAV2Barrier) {
  auto v2 = StartSession(
      MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV2));
  auto v1 = StartSession(
      MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));

  base::WaitableEvent relay_held;
  base::WaitableEvent release_relay;
  ASSERT_TRUE(PostToRelay([&] {
    relay_held.Notify();
    release_relay.Wait();
  }));
  relay_held.Wait();

  std::mutex mutex;
  std::vector<std::string> completions;  // Guarded by |mutex|.
  base::WaitableEvent v2_done;
  base::WaitableEvent v1_done;
  v2->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        std::lock_guard<std::mutex> lock(mutex);
        completions.push_back("v2");
        v2_done.Notify();
      },
      /*timeout_ms=*/30000);
  v1->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        std::lock_guard<std::mutex> lock(mutex);
        completions.push_back("v1");
        v1_done.Notify();
      },
      /*timeout_ms=*/30000);
  // Both OnFlush() callbacks have run (they are synchronous). The v2 request
  // is waiting for the ring drain, the v1 request for the v2 one.
  perfetto::test::SyncProducers();
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_TRUE(completions.empty());
  }

  release_relay.Notify();
  v2_done.Wait();
  v1_done.Wait();
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(completions, (std::vector<std::string>{"v2", "v1"}));
  }
  v2->StopBlocking();
  v1->StopBlocking();
}

// The counterpart: with no older v2 request in the queue, a v1-only request
// on a v2 connection doesn't touch the relay at all.
TEST_F(TracingV2InProcessTest, V1FlushOnAV2ConnectionDoesNotWaitForTheRelay) {
  PutConnectionOnTracingV2();
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV1));

  base::WaitableEvent relay_held;
  base::WaitableEvent release_relay;
  ASSERT_TRUE(PostToRelay([&] {
    relay_held.Notify();
    release_relay.Wait();
  }));
  relay_held.Wait();

  // Must complete while the relay is still blocked.
  EXPECT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));
  release_relay.Notify();
  session->StopBlocking();
}

// The service never flushes a v1 no_flush instance, but an older service that
// does not know no_flush does. The muxer must then call OnFlush() as it always
// did. Only a v2 instance, which the service flushes for the ring's sake, is
// exempt (DeclaredNoFlushDataSourceGetsNoOnFlushCallback covers that through
// the real service).
TEST_F(TracingV2InProcessTest,
       OlderServiceFlushOfAV1NoFlushInstanceCallsOnFlush) {
  // Acks are cumulative: NotifyFlushComplete(N) completes every request of
  // this producer up to N. The synthetic id must therefore be below any real
  // one. The service numbers requests from 1, so an ack for 0 completes
  // nothing.
  constexpr FlushRequestID kFlushIdBelowAnyReal = 0;
  for (WriterSelection selection :
       {WriterSelection::kV1, WriterSelection::kV2}) {
    auto session = StartSession(
        MakeConfigFor({"tracing_v2_declared_no_flush"}, selection));
    const uint32_t before = TracingV2DeclaredNoFlushDataSource::flushes;
    FlushInstanceLikeAnOlderService<TracingV2DeclaredNoFlushDataSource>(
        kFlushIdBelowAnyReal);
    const uint32_t expected_calls = selection == WriterSelection::kV1 ? 1 : 0;
    EXPECT_EQ(TracingV2DeclaredNoFlushDataSource::flushes - before,
              expected_calls);
    session->StopBlocking();
    // The instance slot must be free before the next iteration looks it up.
    WaitForMuxerSequence();
  }
}

TEST_F(TracingV2InProcessTest, StoppedAsyncFlushKeepsRingBarrier) {
  auto older = StartSession(
      MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV2));
  auto later = StartSession(
      MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV2));
  base::WaitableEvent held;
  TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
  std::atomic<bool> older_done{false};
  std::atomic<bool> later_done{false};
  older->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        older_done.store(true);
      },
      30000);
  held.Wait();

  base::WaitableEvent relay_held;
  base::WaitableEvent release_relay;
  ASSERT_TRUE(PostToRelay([&] {
    relay_held.Notify();
    release_relay.Wait();
  }));
  relay_held.Wait();
  TracingV2AsyncFlushDataSource::Trace(
      [](TracingV2AsyncFlushDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("stopped flush tail");
      });
  base::WaitableEvent stop_held;
  TracingV2AsyncStopDataSource::stop_started.store(&stop_held);
  base::WaitableEvent stopped;
  older->SetOnStopCallback([&] { stopped.Notify(); });
  older->Stop();
  stop_held.Wait();
  TracingV2AsyncStopDataSource::CompleteHeldStop();
  WaitForMuxerSequence();

  base::WaitableEvent later_flush_ran;
  TracingV2FlushingDataSource::on_flush_done.store(&later_flush_ran);
  base::WaitableEvent flushed;
  later->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        later_done.store(true);
        flushed.Notify();
      },
      30000);
  later_flush_ran.Wait();
  perfetto::test::SyncProducers();
  EXPECT_FALSE(older_done.load());
  EXPECT_FALSE(later_done.load());
  release_relay.Notify();
  flushed.Wait();
  stopped.Wait();
  EXPECT_TRUE(older_done.load());
  const auto packets = ReadTestPackets(older.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "stopped flush tail");
  TracingV2AsyncFlushDataSource::CompleteHeldFlush();
  WaitForMuxerSequence();
  EXPECT_TRUE(later->FlushBlocking(30000));
  later->StopBlocking();
}

// Block the relay until StopDataSource_AsyncEnd() has sampled the ring
// position, then check that the stop still delivers what was in the ring.
//
// That the service waits for the ack is covered by
// OnTracingDisabledWaitsForTracingV2StopAck, the drain ordering by the bridge
// tests. This test cannot assert that the consumer's stop callback is still
// pending: it goes through more muxer tasks than the checkpoint covers.
TEST_F(TracingV2InProcessTest, StopDeliversWhatWasStillInTheRing) {
  base::WaitableEvent stop_started;
  TracingV2AsyncStopDataSource::stop_started.store(&stop_started);
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_async_stop"}, WriterSelection::kV2));

  base::WaitableEvent relay_blocked;
  base::WaitableEvent release_relay;
  ASSERT_TRUE(PostToRelay([&relay_blocked, &release_relay] {
    relay_blocked.Notify();
    release_relay.Wait();
  }));
  relay_blocked.Wait();

  TracingV2AsyncStopDataSource::Trace(
      [](TracingV2AsyncStopDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("stop tail");
        ctx.Flush();
      });

  base::WaitableEvent stopped;
  session->SetOnStopCallback([&stopped] { stopped.Notify(); });
  session->Stop();
  stop_started.Wait();

  // CompleteHeldStop() posts the rest of the stop path to the muxer thread.
  // Our checkpoint is posted after it, so once it returns the position has
  // been sampled and the drain is queued behind the blocked relay task.
  TracingV2AsyncStopDataSource::CompleteHeldStop();
  WaitForMuxerSequence();

  release_relay.Notify();
  stopped.Wait();

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "stop tail");
}

// The same data source on v1 keeps the old behaviour: it never declared
// will_notify_on_stop, so the service does not wait for it and a held relay
// cannot delay its stop.
TEST_F(TracingV2InProcessTest, StopOfAV1InstanceDoesNotWaitForTheRelay) {
  // The relay only exists once something has selected v2, so make one.
  PutConnectionOnTracingV2();

  base::WaitableEvent relay_blocked;
  base::WaitableEvent release_relay;
  ASSERT_TRUE(PostToRelay([&relay_blocked, &release_relay] {
    relay_blocked.Notify();
    release_relay.Wait();
  }));
  relay_blocked.Wait();

  auto session =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV1));
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("v1 tail");
    ctx.Flush();
  });
  // Must complete while the relay is still blocked.
  session->StopBlocking();
  release_relay.Notify();

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "v1 tail");
}

// A process that never selects v2 gets no ring, bridge or relay. Its writers
// come straight from the endpoint, its flushes (synchronous and asynchronous)
// complete without any of that, and Shutdown() takes the v1 path.
TEST_F(TracingV2InProcessTest, NeverSelectingV2StaysOnV1ThroughShutdown) {
  RunInFreshProcess([] {
    EXPECT_FALSE(HasRelay());
    EXPECT_FALSE(GetBridge());
    auto session = StartSession(
        MakeConfigFor({"tracing_v2_flushing", "tracing_v2_async_flush"},
                      WriterSelection::kV1));
    TracingV2FlushingDataSource::Trace(
        [](TracingV2FlushingDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str("v1");
        });
    EXPECT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    base::WaitableEvent flushed;
    std::atomic<bool> flush_succeeded{false};
    session->Flush(
        [&](bool success) {
          flush_succeeded.store(success);
          flushed.Notify();
        },
        /*timeout_ms=*/30000);
    held.Wait();
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    flushed.Wait();
    EXPECT_TRUE(flush_succeeded.load());

    // Each of the two flushes ran the flushing data source's OnFlush() once.
    std::vector<std::string> strings;
    for (const auto& packet : ReadTestPackets(session.get()))
      strings.push_back(packet.for_testing().str());
    std::sort(strings.begin(), strings.end());
    EXPECT_EQ(strings, (std::vector<std::string>{"async flush", "on flush",
                                                 "on flush", "v1"}));
    session->StopBlocking();
    EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2FlushingDataSource>(), 0u);
    EXPECT_FALSE(HasRelay());
    EXPECT_FALSE(GetBridge());

    // Shutdown() needs the consumer gone.
    session.reset();
    WaitForMuxerSequence();
    perfetto::Tracing::Shutdown();
  });
}

// The v1 flush path does not clean up after a stopped instance: a request
// waiting for its OnFlush() stays queued. Once the connection selects v2 that
// leftover is dropped and the request acked right there, without waiting for
// a later flush to come along, so the ordered queue starts out empty.
TEST_F(TracingV2InProcessTest,
       StoppedV1AsyncFlushBeforeSelectingV2DoesNotBlockV2Flushes) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    older->Flush([](bool) {}, /*timeout_ms=*/30000);
    held.Wait();
    TracingV2AsyncFlushDataSource::hold_next_flush.store(nullptr);
    older->StopBlocking();
    EXPECT_EQ(PendingFlushCount(), 1u);

    auto later =
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
    EXPECT_EQ(PendingFlushCount(), 0u);
    EXPECT_TRUE(later->FlushBlocking(/*timeout_ms=*/5000));
    later->StopBlocking();
    // Runs the stale completion, which finds its instance gone.
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
  });
}

// The service takes NotifyFlushComplete(N) as an ack for every request up to
// N. So when a newer v1 request completes synchronously, the older one, still
// held in its OnFlush() here, is complete as far as the service is concerned,
// and the producer must not keep it either: it would otherwise sit in front
// of the ordered queue once the connection selects v2. Its completion, when it
// finally arrives, is ignored.
TEST_F(TracingV2InProcessTest,
       CumulativeV1AckBeforeSelectingV2DoesNotBlockV2Flushes) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    auto newer = StartSession(
        MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV1));
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    base::WaitableEvent older_flushed;
    std::atomic<bool> older_succeeded{false};
    older->Flush(
        [&](bool success) {
          older_succeeded.store(success);
          older_flushed.Notify();
        },
        /*timeout_ms=*/30000);
    held.Wait();
    EXPECT_EQ(PendingFlushCount(), 1u);

    EXPECT_TRUE(newer->FlushBlocking(/*timeout_ms=*/30000));
    older_flushed.Wait();
    EXPECT_TRUE(older_succeeded.load());
    EXPECT_EQ(PendingFlushCount(), 0u);

    auto v2 =
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
    EXPECT_TRUE(v2->FlushBlocking(/*timeout_ms=*/5000));

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
    EXPECT_EQ(PendingFlushCount(), 0u);
    v2->StopBlocking();
    newer->StopBlocking();
    older->StopBlocking();
  });
}

// Relay work accepted before Shutdown() closes the relay still runs, during
// the join, while the muxer and its endpoints are alive: a drain that writes
// into a retained v1 writer, and a stop completion that posts back to the
// muxer. Only after that is the muxer deleted.
TEST_F(TracingV2InProcessTest, ShutdownAfterV2UseJoinsAcceptedRelayWork) {
  RunInFreshProcess([] {
    base::WaitableEvent stop_started;
    TracingV2AsyncStopDataSource::stop_started.store(&stop_started);
    auto session = StartSession(
        MakeConfigFor({"tracing_v2_async_stop"}, WriterSelection::kV2));
    ASSERT_TRUE(HasRelay());

    base::WaitableEvent relay_held;
    base::WaitableEvent release_relay;
    ASSERT_TRUE(PostToRelay([&] {
      relay_held.Notify();
      release_relay.Wait();
    }));
    relay_held.Wait();
    // Queued behind the held task: the drain for this packet, ...
    TracingV2AsyncStopDataSource::Trace(
        [](TracingV2AsyncStopDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str("shutdown");
        });
    // ... the stop's drain (destroying the session stops the data source
    // without waiting for it), ...
    session.reset();
    stop_started.Wait();
    TracingV2AsyncStopDataSource::CompleteHeldStop();
    WaitForMuxerSequence();
    // ... and a marker that proves the queue ran to the end.
    std::atomic<bool> marker_ran{false};
    ASSERT_TRUE(PostToRelay([&] { marker_ran.store(true); }));

    // Shutdown() blocks on the relay join, so release the relay from another
    // thread once the relay stops accepting tasks, i.e. once Shutdown() has
    // closed it.
    std::thread releaser([&] {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (Internals::PostToTracingV2Relay([] {})) {
        if (std::chrono::steady_clock::now() >= deadline) {
          ADD_FAILURE() << "Shutdown did not close the tracing v2 relay";
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      release_relay.Notify();
    });
    perfetto::Tracing::Shutdown();
    releaser.join();
    EXPECT_TRUE(marker_ran.load());
  });
}

// The in-process and IPC endpoints don't send a flush ack on its own: the
// arbiter puts it on the next CommitData, behind whatever chunks are queued.
// That hides the order in which the muxer asked for the two, which is what the
// ProducerEndpoint contract is about. This endpoint acks directly and records
// the order.
//
// The arbiter has to be this endpoint's own, so that the commits pass through
// here. It shares the SMB with the wrapped in-process endpoint, whose own
// arbiter is then only ever used to forward the acks.
class DirectAckProducerEndpoint : public ProxyProducerEndpoint {
 public:
  DirectAckProducerEndpoint(std::unique_ptr<ProducerEndpoint> wrapped,
                            base::TaskRunner* task_runner,
                            std::mutex* log_mutex,
                            std::vector<std::string>* log)
      : wrapped_(std::move(wrapped)),
        task_runner_(task_runner),
        log_mutex_(log_mutex),
        log_(log) {
    set_backend(wrapped_.get());
  }

  // First called by OnTracingSetup() on the muxer thread, before any trace
  // writer exists. From then on only read.
  SharedMemoryArbiter* MaybeSharedMemoryArbiter() override {
    if (!arbiter_) {
      SharedMemory* shm = backend()->shared_memory();
      if (!shm)
        return nullptr;
      arbiter_ = SharedMemoryArbiter::CreateInstance(
          shm, backend()->shared_buffer_page_size_kb() * 1024,
          SharedMemoryABI::ShmemMode::kDefault, this, task_runner_);
      arbiter_->SetDirectSMBPatchingSupportedByService();
    }
    return arbiter_.get();
  }

  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy buffer_exhausted_policy) override {
    return MaybeSharedMemoryArbiter()->CreateTraceWriter(
        target_buffer, buffer_exhausted_policy);
  }

  void CommitData(const CommitDataRequest& req,
                  CommitDataCallback callback) override {
    if (req.chunks_to_move_size() > 0)
      Log("commit");
    ProxyProducerEndpoint::CommitData(req, std::move(callback));
  }

  void NotifyFlushComplete(FlushRequestID id) override {
    Log("ack " + std::to_string(id));
    MaybeSharedMemoryArbiter()->NotifyFlushComplete(id);
  }

 private:
  void Log(std::string entry) {
    std::lock_guard<std::mutex> lock(*log_mutex_);
    log_->push_back(std::move(entry));
  }

  std::unique_ptr<ProducerEndpoint> wrapped_;
  base::TaskRunner* const task_runner_;
  std::mutex* const log_mutex_;
  std::vector<std::string>* const log_;
  std::unique_ptr<SharedMemoryArbiter> arbiter_;
};

// The in-process backend, with the producer side wrapped as above.
class DirectAckBackend : public TracingBackend {
 public:
  std::unique_ptr<ProducerEndpoint> ConnectProducer(
      const ConnectProducerArgs& args) override {
    return std::make_unique<DirectAckProducerEndpoint>(
        InProcessTracingBackend::GetInstance()->ConnectProducer(args),
        args.task_runner, &log_mutex_, &log_);
  }

  std::unique_ptr<ConsumerEndpoint> ConnectConsumer(
      const ConnectConsumerArgs& args) override {
    return InProcessTracingBackend::GetInstance()->ConnectConsumer(args);
  }

  std::vector<std::string> TakeLog() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return std::move(log_);
  }

 private:
  std::mutex log_mutex_;
  std::vector<std::string> log_;  // Guarded by |log_mutex_|.
};

// Runs each test in a fresh process that it initializes itself, on
// DirectAckBackend.
class TracingV2DirectAckTest : public TracingV2InProcessTest {
 protected:
  static void SetUpTestSuite() {}
  static void TearDownTestSuite() {}

  static std::unique_ptr<perfetto::TracingSession> StartCustomBackendSession(
      const perfetto::TraceConfig& cfg) {
    auto session = perfetto::Tracing::NewTrace(perfetto::kCustomBackend);
    session->Setup(cfg);
    session->StartBlocking();
    return session;
  }
};

// On a v2 connection, stopping a v1 instance can ack a flush that was waiting
// for it. Whatever that instance wrote last is queued in the arbiter (commits
// are batched here so that it stays there), and must be committed before that
// ack goes out, or the service completes the flush without it.
TEST_F(TracingV2DirectAckTest, StoppedV1InstanceCommitsBeforeItsFlushIsAcked) {
  RunInFreshProcess([] {
    // Leaked: the muxer keeps using it until the process exits.
    auto* backend = new DirectAckBackend();
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kCustomBackend;
    args.custom_backend = backend;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_flush");
    TracingV2AsyncFlushDataSource::Register(dsd);
    perfetto::test::SyncProducers();

    // Put the connection on v2, then keep completed chunks queued in the
    // arbiter instead of sending them right away.
    StartCustomBackendSession(
        MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2))
        ->StopBlocking();
    WaitForMuxerSequence();
    perfetto::test::SetBatchCommitsDuration(60000, perfetto::kCustomBackend);

    auto session = StartCustomBackendSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    base::WaitableEvent flush_held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&flush_held);
    session->Flush([](bool) {}, /*timeout_ms=*/30000);
    flush_held.Wait();

    base::WaitableEvent stop_started;
    TracingV2AsyncStopDataSource::stop_started.store(&stop_started);
    session->Stop();
    stop_started.Wait();
    // Ignore setup traffic. From here on, record the packet commit and ack.
    backend->TakeLog();

    // The instance is still up while its stop is held. Write more than a
    // chunk, so that at least one completed chunk is queued for commit.
    TracingV2AsyncFlushDataSource::Trace(
        [](TracingV2AsyncFlushDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str(
              std::string(16 * 1024, 'd'));
        });

    // Completing the stop drops the instance from the pending flush, which
    // acks it.
    TracingV2AsyncStopDataSource::CompleteHeldStop();
    WaitForMuxerSequence();
    const std::vector<std::string> log = backend->TakeLog();
    EXPECT_THAT(log, testing::ElementsAre("commit", "ack 1"));
  });
}

TEST(TracingV2StartupTest, ExplicitSelectionFailsLoudly) {
  if (!PERFETTO_BUILDFLAG(PERFETTO_IPC))
    GTEST_SKIP() << "Startup tracing requires the system backend";
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH(
      {
        TracingInitArgs args;
        args.backends = kSystemBackend;
        Tracing::Initialize(args);
        DataSourceDescriptor descriptor;
        descriptor.set_name("startup_v2");
        TracingV2TestDataSource::Register(descriptor);
        TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(1024);
        auto* ds = cfg.add_data_sources()->mutable_config();
        ds->set_name("startup_v2");
        ds->set_use_tracing_v2(true);
        Tracing::SetupStartupTracingOpts opts;
        opts.backend = kSystemBackend;
        Tracing::SetupStartupTracingBlocking(cfg, std::move(opts));
      },
      "not supported for startup tracing");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

TEST(TracingV2UnsupportedPlatformTest, ExplicitSelectionFailsLoudly) {
  if (tracing_v2::SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "Requires a platform without writer waiting";
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH(
      {
        TracingInitArgs args;
        args.backends = kInProcessBackend;
        Tracing::Initialize(args);
        DataSourceDescriptor descriptor;
        descriptor.set_name("unsupported_v2");
        TracingV2TestDataSource::Register(descriptor);
        TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(1024);
        auto* ds = cfg.add_data_sources()->mutable_config();
        ds->set_name("unsupported_v2");
        ds->set_use_tracing_v2(true);
        auto session = Tracing::NewTrace(kInProcessBackend);
        session->Setup(cfg);
        session->StartBlocking();
      },
      "use_tracing_v2 needs a futex");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
