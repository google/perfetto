/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/etw/file_io_tracker.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/etw/etw.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

constexpr uint32_t kFileIoSimpleOpOpcode = 65;
constexpr uint32_t kFileIoReadWriteOpcode = 67;
constexpr uint32_t kFileIoInfoOpcode = 69;
constexpr uint32_t kFileIoDirEnumOpcode = 72;

class FileIoTrackerTest : public ::testing::Test {
 public:
  FileIoTrackerTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(context_.storage.get());
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(context_.storage.get());
    context_.track_compressor = std::make_unique<TrackCompressor>(&context_);
    context_.track_group_idx_state =
        std::make_unique<TrackCompressorGroupIdxState>();
  }

 protected:
  TraceProcessorContext context_;
};

std::vector<uint8_t> CreateFileIoCreateEvent(uint64_t irp_ptr,
                                             uint64_t file_object,
                                             uint32_t ttid,
                                             uint32_t create_options,
                                             uint32_t file_attributes,
                                             uint32_t share_access,
                                             std::string open_path) {
  protozero::HeapBuffered<protos::pbzero::FileIoCreateEtwEvent> event;
  event->set_irp_ptr(irp_ptr);
  event->set_file_object(file_object);
  event->set_ttid(ttid);
  event->set_create_options(create_options);
  event->set_file_attributes(file_attributes);
  event->set_share_access(share_access);
  event->set_open_path(open_path);
  return event.SerializeAsArray();
}

std::vector<uint8_t> CreateFileIoOpEndEvent(uint64_t irp_ptr,
                                            uint64_t extra_info,
                                            uint32_t nt_status) {
  protozero::HeapBuffered<protos::pbzero::FileIoOpEndEtwEvent> event;
  event->set_irp_ptr(irp_ptr);
  event->set_extra_info(extra_info);
  event->set_nt_status(nt_status);
  return event.SerializeAsArray();
}

std::vector<uint8_t> CreateFileIoDirEnumEvent(uint64_t irp_ptr,
                                              uint64_t file_object,
                                              uint64_t file_key,
                                              uint32_t ttid,
                                              uint32_t length,
                                              uint32_t info_class,
                                              uint32_t file_index,
                                              std::string file_name,
                                              uint32_t opcode) {
  protozero::HeapBuffered<protos::pbzero::FileIoDirEnumEtwEvent> event;
  event->set_irp_ptr(irp_ptr);
  event->set_file_object(file_object);
  event->set_file_key(file_key);
  event->set_ttid(ttid);
  event->set_length(length);
  event->set_info_class(info_class);
  event->set_file_index(file_index);
  event->set_file_name(file_name);
  event->set_opcode(opcode);
  return event.SerializeAsArray();
}

std::vector<uint8_t> CreateFileIoInfoEvent(uint64_t irp_ptr,
                                           uint64_t file_object,
                                           uint64_t file_key,
                                           uint64_t extra_info,
                                           uint32_t ttid,
                                           uint32_t info_class,
                                           uint32_t opcode) {
  protozero::HeapBuffered<protos::pbzero::FileIoInfoEtwEvent> event;
  event->set_irp_ptr(irp_ptr);
  event->set_file_object(file_object);
  event->set_file_key(file_key);
  event->set_extra_info(extra_info);
  event->set_ttid(ttid);
  event->set_info_class(info_class);
  event->set_opcode(opcode);
  return event.SerializeAsArray();
}

std::vector<uint8_t> CreateFileIoReadWriteEvent(uint64_t offset,
                                                uint64_t irp_ptr,
                                                uint64_t file_object,
                                                uint64_t file_key,
                                                uint32_t ttid,
                                                uint32_t io_size,
                                                uint32_t io_flags,
                                                uint32_t opcode) {
  protozero::HeapBuffered<protos::pbzero::FileIoReadWriteEtwEvent> event;
  event->set_offset(offset);
  event->set_irp_ptr(irp_ptr);
  event->set_file_object(file_object);
  event->set_file_key(file_key);
  event->set_ttid(ttid);
  event->set_io_size(io_size);
  event->set_io_flags(io_flags);
  event->set_opcode(opcode);
  return event.SerializeAsArray();
}

std::vector<uint8_t> CreateFileIoSimpleOpEvent(uint64_t irp_ptr,
                                               uint64_t file_object,
                                               uint64_t file_key,
                                               uint32_t ttid,
                                               uint32_t opcode) {
  protozero::HeapBuffered<protos::pbzero::FileIoSimpleOpEtwEvent> event;
  event->set_irp_ptr(irp_ptr);
  event->set_file_object(file_object);
  event->set_file_key(file_key);
  event->set_ttid(ttid);
  event->set_opcode(opcode);
  return event.SerializeAsArray();
}

// Tests that a single FileIo_Create event and a matching end event results in a
// single slice.
TEST_F(FileIoTrackerTest, SingleEvent) {
  FileIoTracker tracker(&context_);
  const auto& slices = context_.storage->slice_table();
  ASSERT_EQ(slices.row_count(), 0u);

  // Create a `FileIo_Create` event.
  const uint64_t irp = 0x1234567812345678;
  std::vector<uint8_t> create_event_buffer =
      CreateFileIoCreateEvent(irp, 1, 2, 3, 4, 5, "/path/to/file");
  protozero::ConstBytes create_event{create_event_buffer.data(),
                                     create_event_buffer.size()};
  tracker.ParseFileIoCreate(100, create_event);

  // Create a matching end event.
  std::vector<uint8_t> end_event_buffer = CreateFileIoOpEndEvent(irp, 0, 0);
  protozero::ConstBytes end_event{end_event_buffer.data(),
                                  end_event_buffer.size()};
  tracker.ParseFileIoOpEnd(150, end_event);

  // One slice should have been created.
  ASSERT_EQ(slices.row_count(), 1u);
  auto slice = slices[0];
  EXPECT_EQ(slice.ts(), 100);
  EXPECT_EQ(slice.dur(), 50);
}

// Tests that neither a start event without a matching end nor an end event
// without a matching start result in a slice being created.
TEST_F(FileIoTrackerTest, NonMatchingEvents) {
  FileIoTracker tracker(&context_);
  const auto& slices = context_.storage->slice_table();
  ASSERT_EQ(slices.row_count(), 0u);

  // Create a `FileIo_Create` event.
  const uint64_t start_irp = 0x1234567812345678;
  std::vector<uint8_t> create_event_buffer =
      CreateFileIoCreateEvent(start_irp, 1, 2, 3, 4, 5, "/path/to/file");
  protozero::ConstBytes create_event{create_event_buffer.data(),
                                     create_event_buffer.size()};
  tracker.ParseFileIoCreate(100, create_event);

  // Create an end event with a different IRP.
  const uint64_t end_irp = 0xABCDEFABCDEFABCD;
  std::vector<uint8_t> end_event_buffer = CreateFileIoOpEndEvent(end_irp, 1, 1);
  protozero::ConstBytes end_event{end_event_buffer.data(),
                                  end_event_buffer.size()};
  tracker.ParseFileIoOpEnd(200, end_event);

  // No slices should have been created.
  ASSERT_EQ(slices.row_count(), 0u);
}

// If an end event was dropped, two start events in a row may use the same IRP.
// In this case, only one slice should be created (as the first event's end must
// have been dropped so it should be ignored).
TEST_F(FileIoTrackerTest, IrpReused) {
  FileIoTracker tracker(&context_);
  const auto& slices = context_.storage->slice_table();
  ASSERT_EQ(slices.row_count(), 0u);

  // Create a `FileIo_Create` event.
  const uint64_t irp = 0x1234567812345678;
  std::vector<uint8_t> create_event_buffer_1 =
      CreateFileIoCreateEvent(irp, 1, 10, 3, 4, 5, "/path/to/file_v1");
  protozero::ConstBytes create_event_1{create_event_buffer_1.data(),
                                       create_event_buffer_1.size()};
  tracker.ParseFileIoCreate(100, create_event_1);

  // Create a second `FileIo_Create` event with the same IRP.
  std::vector<uint8_t> create_event_buffer_2 =
      CreateFileIoCreateEvent(irp, 1, 20, 3, 4, 5, "/path/to/file_v2");
  protozero::ConstBytes create_event_2{create_event_buffer_2.data(),
                                       create_event_buffer_2.size()};
  tracker.ParseFileIoCreate(150, create_event_2);

  // Create a matching end event.
  std::vector<uint8_t> end_event_buffer = CreateFileIoOpEndEvent(irp, 0, 0);
  protozero::ConstBytes end_event{end_event_buffer.data(),
                                  end_event_buffer.size()};
  tracker.ParseFileIoOpEnd(200, end_event);

  // One slice should have been created.
  const auto row_count = slices.row_count();
  ASSERT_EQ(row_count, 1u);

  // Verify that the created slice corresponds to the second event.
  auto slice = slices[0];
  EXPECT_EQ(slice.ts(), 150);
  EXPECT_EQ(slice.dur(), 50);
}

// Tests that multiple events, each with a matching end event, create one slice
// per event. This test also ensures that the parsing code for each event slice
// is exercised.
TEST_F(FileIoTrackerTest, MultipleEvents) {
  FileIoTracker tracker(&context_);
  const auto& slices = context_.storage->slice_table();
  ASSERT_EQ(slices.row_count(), 0u);

  // Start and end a `FileIo_Create` event.
  uint64_t irp_1 = 1;
  std::vector<uint8_t> create_event_buffer =
      CreateFileIoCreateEvent(irp_1, 1, 2, 3, 4, 5, "/path/to/file");
  protozero::ConstBytes create_event{create_event_buffer.data(),
                                     create_event_buffer.size()};
  tracker.ParseFileIoCreate(100, create_event);

  std::vector<uint8_t> end_create_event_buffer =
      CreateFileIoOpEndEvent(irp_1, 0, 0);
  protozero::ConstBytes end_create_event{end_create_event_buffer.data(),
                                         end_create_event_buffer.size()};
  tracker.ParseFileIoOpEnd(200, end_create_event);

  // Start and end a `FileIo_DirEnum` event with the same IRP.
  std::vector<uint8_t> dir_enum_event_buffer = CreateFileIoDirEnumEvent(
      irp_1, 1, 2, 3, 4, 5, 6, "file", kFileIoDirEnumOpcode);
  protozero::ConstBytes dir_enum_event{dir_enum_event_buffer.data(),
                                       dir_enum_event_buffer.size()};
  tracker.ParseFileIoDirEnum(250, dir_enum_event);

  std::vector<uint8_t> end_dir_enum_event_buffer =
      CreateFileIoOpEndEvent(irp_1, 0, 0);
  protozero::ConstBytes end_dir_enum_event{end_dir_enum_event_buffer.data(),
                                           end_dir_enum_event_buffer.size()};
  tracker.ParseFileIoOpEnd(300, end_dir_enum_event);

  // Start a `FileIo_Info` event.
  std::vector<uint8_t> info_event_buffer =
      CreateFileIoInfoEvent(irp_1, 1, 2, 3, 4, 5, kFileIoInfoOpcode);
  protozero::ConstBytes info_event{info_event_buffer.data(),
                                   info_event_buffer.size()};
  tracker.ParseFileIoInfo(300, info_event);

  // Start a `FileIo_ReadWrite` event with a different IRP.
  uint64_t irp_2 = 2;
  std::vector<uint8_t> read_write_event_buffer = CreateFileIoReadWriteEvent(
      1, irp_2, 2, 3, 4, 5, 6, kFileIoReadWriteOpcode);
  protozero::ConstBytes read_write_event{read_write_event_buffer.data(),
                                         read_write_event_buffer.size()};
  tracker.ParseFileIoReadWrite(400, read_write_event);

  // End the `FileIo_Info` and `FileIo_ReadWrite` events.
  std::vector<uint8_t> end_info_event_buffer =
      CreateFileIoOpEndEvent(irp_1, 0, 0);
  protozero::ConstBytes end_info_event{end_info_event_buffer.data(),
                                       end_info_event_buffer.size()};
  tracker.ParseFileIoOpEnd(500, end_info_event);

  std::vector<uint8_t> end_read_write_event_buffer =
      CreateFileIoOpEndEvent(irp_2, 0, 0);
  protozero::ConstBytes end_read_write_event{
      end_read_write_event_buffer.data(), end_read_write_event_buffer.size()};
  tracker.ParseFileIoOpEnd(600, end_read_write_event);

  // Start and end a `FileIo_SimpleOp` event.
  std::vector<uint8_t> simple_op_event_buffer =
      CreateFileIoSimpleOpEvent(irp_1, 1, 2, 3, kFileIoSimpleOpOpcode);
  protozero::ConstBytes simple_op_event{simple_op_event_buffer.data(),
                                        simple_op_event_buffer.size()};
  tracker.ParseFileIoSimpleOp(700, simple_op_event);

  std::vector<uint8_t> end_simple_op_event_buffer =
      CreateFileIoOpEndEvent(irp_1, 5, 5);
  protozero::ConstBytes end_simple_op_event{end_simple_op_event_buffer.data(),
                                            end_simple_op_event_buffer.size()};
  tracker.ParseFileIoOpEnd(800, end_simple_op_event);

  // Five slices should have been created, one per event type.
  ASSERT_EQ(slices.row_count(), 5u);
}

}  // namespace
}  // namespace perfetto::trace_processor
