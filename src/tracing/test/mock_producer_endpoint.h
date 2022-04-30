/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACING_TEST_MOCK_PRODUCER_ENDPOINT_H_
#define SRC_TRACING_TEST_MOCK_PRODUCER_ENDPOINT_H_

#include "perfetto/ext/tracing/core/tracing_service.h"
#include "protos/perfetto/common/data_source_descriptor.gen.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {

class MockProducerEndpoint : public TracingService::ProducerEndpoint {
 public:
  MOCK_METHOD1(RegisterDataSource, void(const DataSourceDescriptor&));
  MOCK_METHOD1(UpdateDataSource, void(const DataSourceDescriptor&));
  MOCK_METHOD1(UnregisterDataSource, void(const std::string&));
  MOCK_METHOD2(RegisterTraceWriter, void(uint32_t, uint32_t));
  MOCK_METHOD1(UnregisterTraceWriter, void(uint32_t));
  MOCK_METHOD2(CommitData, void(const CommitDataRequest&, CommitDataCallback));
  MOCK_CONST_METHOD0(shared_memory, SharedMemory*());
  MOCK_CONST_METHOD0(shared_buffer_page_size_kb, size_t());
  MOCK_METHOD2(CreateTraceWriter,
               std::unique_ptr<TraceWriter>(BufferID, BufferExhaustedPolicy));
  MOCK_METHOD0(MaybeSharedMemoryArbiter, SharedMemoryArbiter*());
  MOCK_CONST_METHOD0(IsShmemProvidedByProducer, bool());
  MOCK_METHOD1(NotifyFlushComplete, void(FlushRequestID));
  MOCK_METHOD1(NotifyDataSourceStarted, void(DataSourceInstanceID));
  MOCK_METHOD1(NotifyDataSourceStopped, void(DataSourceInstanceID));
  MOCK_METHOD1(ActivateTriggers, void(const std::vector<std::string>&));
  MOCK_METHOD1(Sync, void(std::function<void()>));
};

}  // namespace perfetto

#endif  // SRC_TRACING_TEST_MOCK_PRODUCER_ENDPOINT_H_
