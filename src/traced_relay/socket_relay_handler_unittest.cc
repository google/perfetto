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

#include "src/traced_relay/socket_relay_handler.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/unix_socket.h"

#include "test/gtest_and_gmock.h"

using testing::Values;

namespace perfetto {
namespace {

using RawSocketPair = std::pair<base::UnixSocketRaw, base::UnixSocketRaw>;
using RngValueType = std::minstd_rand0::result_type;

struct TestClient {
  RawSocketPair endpoint_sockets;
  std::minstd_rand0 data_prng;
  std::thread client_thread;
};

class SocketRelayHandlerTest : public ::testing::TestWithParam<uint32_t> {
 protected:
  void SetUp() override {
    socket_relay_handler_ = std::make_unique<SocketRelayHandler>();

    for (uint32_t i = 0; i < GetParam(); i++) {
      TestClient client{SetUpEndToEndSockets(), std::minstd_rand0(i), {}};
      test_clients_.push_back(std::move(client));
    }
  }
  void TearDown() override { socket_relay_handler_ = nullptr; }

  RawSocketPair SetUpEndToEndSockets() {
    // Creates 2 SocketPairs:
    // sock1 <-> sock2 <-> SocketRelayHandler <-> sock3 <-> sock4.
    // sock2 and sock3 are transferred to the SocketRelayHandler.
    // We test by reading and writing bidirectionally using sock1 and sock4.
    auto [sock1, sock2] = base::UnixSocketRaw::CreatePairPosix(
        base::SockFamily::kUnix, base::SockType::kStream);
    sock2.SetBlocking(false);

    auto [sock3, sock4] = base::UnixSocketRaw::CreatePairPosix(
        base::SockFamily::kUnix, base::SockType::kStream);
    sock3.SetBlocking(false);

    auto socket_pair = std::make_unique<SocketPair>();
    socket_pair->first.sock = std::move(sock2);
    socket_pair->second.sock = std::move(sock3);

    socket_relay_handler_->AddSocketPair(std::move(socket_pair));

    RawSocketPair endpoint_sockets;
    endpoint_sockets.first = std::move(sock1);
    endpoint_sockets.second = std::move(sock4);

    return endpoint_sockets;
  }

  std::unique_ptr<SocketRelayHandler> socket_relay_handler_;
  std::vector<TestClient> test_clients_;
  // Use fewer receiver threads than sender threads.
  base::ThreadPool receiver_thread_pool_{1 + GetParam() / 10};
};

TEST(SocketWithBufferTest, EnqueueDequeue) {
  SocketWithBuffer socket_with_buffer;
  // No data initially.
  EXPECT_EQ(0u, socket_with_buffer.data_size());

  // Has room for writing some bytes into.
  std::string data = "12345678901234567890";
  EXPECT_GT(socket_with_buffer.available_bytes(), data.size());

  memcpy(socket_with_buffer.buffer(), data.data(), data.size());
  socket_with_buffer.EnqueueData(data.size());
  EXPECT_EQ(data.size(), socket_with_buffer.data_size());

  // Dequeue some bytes.
  socket_with_buffer.DequeueData(5);
  EXPECT_EQ(socket_with_buffer.data_size(), data.size() - 5);
  std::string buffered_data(reinterpret_cast<char*>(socket_with_buffer.data()),
                            socket_with_buffer.data_size());
  EXPECT_EQ(buffered_data, "678901234567890");
}

// Test the SocketRelayHander with randomized request and response data.
TEST_P(SocketRelayHandlerTest, RandomizedRequestResponse) {
  // The max message size in the number of RNG calls.
  constexpr size_t kMaxMsgSizeRng = 1 << 20;

  // Create the threads for sending and receiving data through the
  // SocketRelayHandler.
  for (auto& client : test_clients_) {
    auto* thread_pool = &receiver_thread_pool_;

    auto thread_func = [&client, thread_pool]() {
      auto& rng = client.data_prng;

      // The max number of requests.
      const size_t num_requests = rng() % 50;

      for (size_t j = 0; j < num_requests; j++) {
        auto& send_endpoint = client.endpoint_sockets.first;
        auto& receive_endpoint = client.endpoint_sockets.second;

        auto req_size = rng() % kMaxMsgSizeRng;

        // Generate the random request.
        std::vector<RngValueType> request;
        request.reserve(req_size);
        for (size_t r = 0; r < req_size; r++) {
          request.emplace_back(rng());
        }

        // Create a buffer for receiving the request.
        std::vector<RngValueType> received_request(request.size());

        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        bool done = false;

        // Blocking receive on the thread pool.
        thread_pool->PostTask([&]() {
          const size_t bytes_to_receive =
              received_request.size() * sizeof(RngValueType);
          uint8_t* receive_buffer =
              reinterpret_cast<uint8_t*>(received_request.data());
          size_t bytes_received = 0;

          // Perform a blocking read until we received the expected bytes.
          while (bytes_received < bytes_to_receive) {
            ssize_t rsize = PERFETTO_EINTR(
                receive_endpoint.Receive(receive_buffer + bytes_received,
                                         bytes_to_receive - bytes_received));
            if (rsize <= 0)
              break;
            bytes_received += static_cast<size_t>(rsize);

            std::this_thread::yield();  // Adds some scheduling randomness.
          }

          std::lock_guard<std::mutex> inner_lock(mutex);
          done = true;
          cv.notify_one();
        });

        // Perform a blocking send of the request data.
        PERFETTO_EINTR(send_endpoint.Send(
            request.data(), request.size() * sizeof(RngValueType)));

        // Wait until the request is fully received.
        cv.wait(lock, [&done] { return done; });

        // Check data integrity.
        EXPECT_EQ(request, received_request);

        // Add some randomness to timing.
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1000));

        // Emulate the response by reversing the data flow direction.
        std::swap(send_endpoint, receive_endpoint);
      }
    };

    client.client_thread = std::thread(std::move(thread_func));
  }

  for (auto& client : test_clients_) {
    client.client_thread.join();
  }
}

INSTANTIATE_TEST_SUITE_P(ByConnections,
                         SocketRelayHandlerTest,
                         Values(1, 5, 50));

}  // namespace
}  // namespace perfetto
