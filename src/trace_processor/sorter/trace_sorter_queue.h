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

#ifndef SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_QUEUE_H_
#define SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_QUEUE_H_

#include <cstddef>
#include <deque>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/sorter/trace_sorter_internal.h"

namespace perfetto {
namespace trace_processor {
namespace trace_sorter_internal {

// 1MB is good tradeoff between having big enough memory blocks so that we don't
// need to often append and remove blocks for big traces, but small enough to
// not overuse memory for small traces.
static constexpr uint32_t kDefaultSize = 1 * 1024 * 1024;  // 1MB

// Used for storing the data for all different packet data types.
class VariadicQueue {
 public:
  // Returned by |Append| and should be passed |Evict| to extract the
  // stored state from the queue.
  struct ValueReference {
    uint32_t offset;
    bool blob_compressed;
    bool seq_compressed;
  };

  VariadicQueue() : VariadicQueue(kDefaultSize) {}
  ~VariadicQueue() {
    // These checks verify that we evicted all elements from this queue. This is
    // important as we need to call the destructor to make sure we're not
    // leaking memory.
    FreeMemory();
    PERFETTO_CHECK(mem_blocks_.size() == 1);
    PERFETTO_CHECK(mem_blocks_.back().empty());
  }

  VariadicQueue(const VariadicQueue&) = delete;
  VariadicQueue& operator=(const VariadicQueue&) = delete;

  VariadicQueue(VariadicQueue&&) = default;
  VariadicQueue& operator=(VariadicQueue&&) noexcept = default;

  // Moves packet data type to the end of the queue storage.
  template <typename T>
  ValueReference Append(T value) {
    PERFETTO_DCHECK(!mem_blocks_.empty());

    uint64_t size = Block::AppendSize<T>(value);
    if (PERFETTO_UNLIKELY(!mem_blocks_.back().HasSpace(size))) {
      mem_blocks_.emplace_back(Block(block_size_));
    }
    auto& back_block = mem_blocks_.back();
    PERFETTO_DCHECK(back_block.HasSpace(size));
    return GlobalRefFromLastBlockRef(back_block.Append(std::move(value)));
  }

  // Moves object out of queue storage.
  template <typename T>
  T Evict(ValueReference ref) {
    uint32_t block = (ref.offset / block_size_) - deleted_blocks_;
    uint32_t block_offset = ref.offset % block_size_;
    return mem_blocks_[block].Evict<T>(block_offset, ref.blob_compressed,
                                       ref.seq_compressed);
  }

  // Clears the empty front of queue storage.
  void FreeMemory() {
    while (mem_blocks_.size() > 1 && mem_blocks_.front().empty()) {
      mem_blocks_.pop_front();
      deleted_blocks_++;
    }
  }

  // Returns the offset value in which new element can be stored.
  uint32_t NextOffset() const {
    PERFETTO_DCHECK(!mem_blocks_.empty());
    return GlobalMemOffsetFromLastBlockOffset(mem_blocks_.back().offset());
  }

  static VariadicQueue VariadicQueueForTesting(uint32_t size) {
    return VariadicQueue(size);
  }

 private:
  // Implementation note: this class stores an extra 8 bytes in debug builds to
  // store the size of the type stored inside.
  class Block {
   public:
    explicit Block(uint32_t block_size)
        : size_(block_size),
          storage_(
              base::AlignedAllocTyped<uint64_t>(size_ / sizeof(uint64_t))) {}

    bool HasSpace(uint64_t size) const { return size <= size_ - offset_; }

    template <typename T>
    ValueReference Append(T value) {
      static_assert(alignof(T) <= 8,
                    "Class must have at most 8 byte alignment");
      PERFETTO_DCHECK(offset_ % 8 == 0);
      PERFETTO_DCHECK(HasSpace(AppendSize(value)));

      char* storage_begin_ptr = reinterpret_cast<char*>(storage_.get());
      char* ptr = storage_begin_ptr + offset_;
#if PERFETTO_DCHECK_IS_ON()
      ptr = AppendUnchecked(ptr, TypedMemoryAccessor<T>::AppendSize(value));
#endif

      auto tbv = TypedMemoryAccessor<T>::GetTraceBlobView(value);
      auto seq_state = TypedMemoryAccessor<T>::GetSequenceState(value);
      AppendOptions options =
          CreateAppendOptions(&ptr, std::move(tbv), seq_state);

      ptr = TypedMemoryAccessor<T>::Append(ptr, std::move(value), options);
      num_elements_++;

      auto cur_offset = offset_;
      offset_ = static_cast<uint32_t>(base::AlignUp<8>(static_cast<uint32_t>(
          ptr - reinterpret_cast<char*>(storage_.get()))));
      return ValueReference{cur_offset, options.skip_trace_blob_view,
                            options.skip_sequence_state};
    }

    template <typename T>
    T Evict(uint32_t offset, bool blob_compressed, bool seq_compressed) {
      PERFETTO_DCHECK(offset < size_);
      PERFETTO_DCHECK(offset % 8 == 0);

      char* ptr = reinterpret_cast<char*>(storage_.get()) + offset;
      uint64_t size = 0;
#if PERFETTO_DCHECK_IS_ON()
      size = EvictUnchecked<uint64_t>(&ptr);
#endif

      EvictSkippedFields fields =
          CreateEvictSkippedFields(&ptr, blob_compressed, seq_compressed);
      T value = TypedMemoryAccessor<T>::Evict(ptr, std::move(fields));
      PERFETTO_DCHECK(size == TypedMemoryAccessor<T>::AppendSize(value));
      num_elements_evicted_++;
      return value;
    }

    template <typename T>
    static uint64_t AppendSize(const T& value) {
#if PERFETTO_DCHECK_IS_ON()
      // On debug runs for each append of T we also append the sizeof(T) to the
      // queue for sanity check, which we later evict and compare with object
      // size. This value needs to be added to general size of an object.
      return sizeof(uint64_t) + TypedMemoryAccessor<T>::AppendSize(value);
#else
      return TypedMemoryAccessor<T>::AppendSize(value);
#endif
    }

    uint32_t offset() const { return offset_; }
    bool empty() const { return num_elements_ == num_elements_evicted_; }

   private:
    struct CompressionDescriptor {
     public:
      // Most rightmost 28 bits are used to store lenght, next are 28 bits for
      // offset and the left 8 bits are for index.
      // [2b - blob index][6b - sequence index][28b - length][28b - offset]
      static constexpr uint8_t kBitsForOffset = 28;
      static constexpr uint8_t kBitsForLength = 28;
      static constexpr uint8_t kBitsForSequenceIndex = 6;
      static constexpr uint8_t kBitsForBlobIndex = 2;
      static constexpr uint8_t kBitsTotal = kBitsForBlobIndex +
                                            kBitsForSequenceIndex +
                                            kBitsForOffset + kBitsForLength;

      static constexpr uint8_t kOffsetShift = 0;
      static constexpr uint8_t kLengthShift = kOffsetShift + kBitsForOffset;
      static constexpr uint8_t kSequenceIndexShift =
          kLengthShift + kBitsForLength;
      static constexpr uint8_t kBlobIndexShift =
          kSequenceIndexShift + kBitsForSequenceIndex;

      CompressionDescriptor(uint8_t blob_index,
                            uint8_t seq_index,
                            uint32_t offset,
                            uint32_t length)
          : packed_(ComputePacked(blob_index, seq_index, offset, length)) {}

      uint8_t blob_index() const {
        return ExtractFromPacked<uint8_t>(kBitsForBlobIndex, kBlobIndexShift);
      }
      uint8_t seq_index() const {
        return ExtractFromPacked<uint8_t>(kBitsForSequenceIndex,
                                          kSequenceIndexShift);
      }
      uint32_t length() const {
        return ExtractFromPacked<uint32_t>(kBitsForLength, kLengthShift);
      }
      uint32_t offset() const {
        return ExtractFromPacked<uint32_t>(kBitsForOffset, kOffsetShift);
      }

     private:
      static uint64_t ComputePacked(uint8_t blob_index,
                                    uint8_t seq_index,
                                    uint32_t offset,
                                    uint32_t length) {
        static_assert(kBitsTotal == 64, "Wrong bitpacking sizes.");

        PERFETTO_DCHECK(FitsInBits(blob_index, kBitsForBlobIndex));
        PERFETTO_DCHECK(FitsInBits(seq_index, kBitsForSequenceIndex));
        PERFETTO_DCHECK(FitsInBits(offset, kBitsForOffset));
        PERFETTO_DCHECK(FitsInBits(length, kBitsForLength));

        uint64_t packed = 0;
        packed |= static_cast<uint64_t>(blob_index) << kBlobIndexShift;
        packed |= static_cast<uint64_t>(seq_index) << kSequenceIndexShift;
        packed |= static_cast<uint64_t>(offset) << kOffsetShift;
        packed |= static_cast<uint64_t>(length) << kLengthShift;
        return packed;
      }

      template <typename T>
      T ExtractFromPacked(uint8_t bits, uint8_t shift) const {
        return static_cast<T>(packed_ >> shift & ((1ull << bits) - 1));
      }

      static bool FitsInBits(uint64_t value, uint8_t bits) {
        PERFETTO_DCHECK(bits > 0);
        return value < (1ull << bits);
      }

      uint64_t packed_ = 0;
    };

    static constexpr uint8_t kMaxBlobVectorSize =
        1 << CompressionDescriptor::kBitsForBlobIndex;
    static constexpr uint8_t kMaxSequenceVectorSize =
        1 << CompressionDescriptor::kBitsForSequenceIndex;

    AppendOptions CreateAppendOptions(
        char** ptr,
        base::Optional<TraceBlobView> tbv,
        base::Optional<RefPtr<PacketSequenceStateGeneration>> seq_state) {
      base::Optional<uint8_t> blob_idx =
          tbv ? FindBlobIndex(tbv->blob()) : base::nullopt;
      base::Optional<uint8_t> seq_index =
          seq_state ? FindSequenceIndex(*seq_state) : base::nullopt;
      if (blob_idx || seq_index) {
        uint32_t tbv_offset = tbv ? tbv->offset() : 0;
        uint32_t tbv_length = tbv ? tbv->length() : 0;
        CompressionDescriptor descriptor(blob_idx.value_or(0),
                                         seq_index.value_or(0), tbv_offset,
                                         tbv_length);
        *ptr =
            AppendUnchecked<CompressionDescriptor>(*ptr, std::move(descriptor));
      }
      return AppendOptions{blob_idx.has_value(), seq_index.has_value()};
    }

    EvictSkippedFields CreateEvictSkippedFields(char** ptr,
                                                bool blob_compressed,
                                                bool seq_compressed) {
      base::Optional<CompressionDescriptor> compression_descriptor;
      if (blob_compressed || seq_compressed) {
        compression_descriptor = EvictUnchecked<CompressionDescriptor>(ptr);
      }

      EvictSkippedFields fields;
      if (blob_compressed) {
        RefPtr<TraceBlob> blob = blobs_[compression_descriptor->blob_index()];
        fields.skipped_trace_blob_view =
            TraceBlobView(blob, compression_descriptor->offset(),
                          compression_descriptor->length());
      }
      if (seq_compressed) {
        fields.skipped_sequence_state =
            sequences_[compression_descriptor->seq_index()];
      }
      return fields;
    }

    base::Optional<uint8_t> FindBlobIndex(const RefPtr<TraceBlob>& tb) {
      if (!blobs_.empty() && blobs_.back() == tb) {
        return static_cast<uint8_t>(blobs_.size() - 1);
      }
      if (blobs_.size() >= decltype(blobs_)::kInlineSize) {
        return base::nullopt;
      }
      blobs_.emplace_back(tb);
      return static_cast<uint8_t>(blobs_.size() - 1);
    }

    base::Optional<uint8_t> FindSequenceIndex(
        const RefPtr<PacketSequenceStateGeneration>& seq) {
      auto it = std::find(sequences_.begin(), sequences_.end(), seq);
      if (it != sequences_.end()) {
        return static_cast<uint8_t>(std::distance(sequences_.begin(), it));
      }
      if (sequences_.size() >= decltype(sequences_)::kInlineSize) {
        return base::nullopt;
      }
      sequences_.emplace_back(seq);
      return static_cast<uint8_t>(sequences_.size() - 1);
    }

    uint32_t size_;
    uint32_t offset_ = 0;

    uint32_t num_elements_ = 0;
    uint32_t num_elements_evicted_ = 0;

    base::SmallVector<RefPtr<TraceBlob>, kMaxBlobVectorSize> blobs_;
    base::SmallVector<RefPtr<PacketSequenceStateGeneration>,
                      kMaxSequenceVectorSize>
        sequences_;
    base::AlignedUniquePtr<uint64_t> storage_;
  };

  explicit VariadicQueue(uint32_t block_size) : block_size_(block_size) {
    mem_blocks_.emplace_back(Block(block_size_));
  }

  uint32_t GlobalMemOffsetFromLastBlockOffset(uint32_t block_offset) const {
    return (deleted_blocks_ + static_cast<uint32_t>(mem_blocks_.size()) - 1) *
               block_size_ +
           block_offset;
  }

  ValueReference GlobalRefFromLastBlockRef(ValueReference ref) const {
    uint32_t global_offset = GlobalMemOffsetFromLastBlockOffset(ref.offset);
    return ValueReference{global_offset, ref.blob_compressed,
                          ref.seq_compressed};
  }

  std::deque<Block> mem_blocks_;

  uint32_t block_size_ = kDefaultSize;
  uint32_t deleted_blocks_ = 0;
};

}  // namespace trace_sorter_internal
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_QUEUE_H_
