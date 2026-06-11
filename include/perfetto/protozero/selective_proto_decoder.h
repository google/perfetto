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

#ifndef INCLUDE_PERFETTO_PROTOZERO_SELECTIVE_PROTO_DECODER_H_
#define INCLUDE_PERFETTO_PROTOZERO_SELECTIVE_PROTO_DECODER_H_

#include <stdint.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>

#include "perfetto/base/export.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"

namespace protozero {

namespace internal {

template <size_t kNumWords>
constexpr std::array<uint64_t, kNumWords> MakeFieldBitmap(
    std::initializer_list<uint32_t> ids) {
  std::array<uint64_t, kNumWords> bitmap{};
  for (uint32_t id : ids)
    bitmap[id >> 6] |= uint64_t(1) << (id & 63);
  return bitmap;
}

template <size_t kNumWords>
constexpr uint32_t CountBits(const std::array<uint64_t, kNumWords>& bitmap) {
  uint32_t count = 0;
  for (uint64_t word : bitmap) {
    for (; word; word &= word - 1)
      count++;
  }
  return count;
}

}  // namespace internal

// A hand-written alternative to TypedProtoDecoder for messages that can be
// extended with out-of-tree fields (e.g. TracePacket, TrackEvent,
// InternedData). The caller explicitly lists the field ids it wants decoded
// into O(1) dense storage; every other field ("unknown", typically an
// extension) is either dropped or collected, in wire order, into a small
// array that can be iterated switching on Field::id().
//
// Usage:
//   using PacketDecoder = protozero::SelectiveProtoDecoder<
//       /*kStoreUnknownFields=*/true,
//       TracePacket::kTimestampFieldNumber,
//       TracePacket::kTrackEventFieldNumber>;
//   PacketDecoder dec(start, size);
//   dec.at<TracePacket::kTimestampFieldNumber>();
//   for (const Field& f : dec.unknown_fields()) {
//     switch (f.id()) { ... }
//   }
//
// Unlike TypedProtoDecoder, fields in the explicit set have singular
// semantics: the last occurrence wins. Repeated fields should be left out of
// the explicit set and consumed via unknown_fields(), which preserves
// multiplicity and wire order.
//
// The explicit set may be empty, turning this into a generic field collector:
//   SelectiveProtoDecoder<true> dec(start, size);
//   dec.FindUnknownField(SomeExtension::kFieldNumber);
// This is the way to read extension fields, whose ids (e.g. TracePacket's
// `extensions 1000 to 1999`) are too large for the dense explicit storage.
class PERFETTO_EXPORT_COMPONENT SelectiveProtoDecoderBase
    : public DenseProtoDecoderBase {
 public:
  struct UnknownFieldRange {
    const Field* range_begin;
    const Field* range_end;
    const Field* begin() const { return range_begin; }
    const Field* end() const { return range_end; }
    size_t size() const { return static_cast<size_t>(range_end - range_begin); }
  };

  // Returns the first unknown field with the given id, or an invalid field if
  // no such field was seen while decoding.
  const Field& FindUnknownField(uint32_t id) const {
    for (uint32_t i = 0; i < unknown_size_; ++i) {
      if (unknown_[i].id() == id)
        return unknown_[i];
    }
    return kInvalidField;
  }

 protected:
  SelectiveProtoDecoderBase(const uint8_t* buffer,
                            size_t length,
                            const uint64_t* explicit_bitmap,
                            Field* fields,
                            uint64_t* presence,
                            uint32_t num_fields,
                            Field* unknown,
                            uint32_t unknown_capacity)
      : DenseProtoDecoderBase(buffer, length, fields, presence, num_fields),
        explicit_bitmap_(explicit_bitmap),
        unknown_(unknown),
        unknown_capacity_(unknown_capacity) {}

  SelectiveProtoDecoderBase(const SelectiveProtoDecoderBase&) = delete;
  SelectiveProtoDecoderBase& operator=(const SelectiveProtoDecoderBase&) =
      delete;

  bool IsExplicit(uint32_t id) const {
    return id < num_fields_ && ((explicit_bitmap_[id >> 6] >> (id & 63)) & 1u);
  }

  void ParseAllFields();

  // Called when the inline unknown-field storage is exhausted.
  void ExpandUnknownStorage();

  // Membership bitmap for the explicit set, static storage in the template
  // specialization.
  const uint64_t* explicit_bitmap_;

  // Unknown fields, in wire order. nullptr if kStoreUnknownFields is false.
  Field* unknown_;
  uint32_t unknown_size_ = 0;
  uint32_t unknown_capacity_;
  std::unique_ptr<Field[]> heap_unknown_;
};

template <bool kStoreUnknownFields, uint32_t... kFieldIds>
class SelectiveProtoDecoder : public SelectiveProtoDecoderBase {
 public:
  // {0u, ...} so an empty explicit set (generic field collector) is allowed.
  static constexpr uint32_t kMaxFieldId = std::max({0u, kFieldIds...});
  static_assert(((kFieldIds >= 1) && ...), "Field ids must be >= 1");
  static_assert(kMaxFieldId <= 1024,
                "Explicit field ids must be small: the dense storage is "
                "sizeof(Field) * (max id + 1) bytes, on the stack");

  SelectiveProtoDecoder(const uint8_t* buffer, size_t length)
      : SelectiveProtoDecoderBase(
            buffer,
            length,
            kExplicitBitmap.data(),
            fields_storage_,
            presence_storage_,
            kMaxFieldId + 1,
            kStoreUnknownFields ? unknown_storage_ : nullptr,
            kStoreUnknownFields ? kUnknownInlineCapacity : 0) {
    ParseAllFields();
  }

  explicit SelectiveProtoDecoder(protozero::ConstBytes blob)
      : SelectiveProtoDecoder(blob.data, blob.size) {}

  // True if |id| is in the explicit set declared by this instantiation.
  static constexpr bool ContainsField(uint32_t id) {
    return ((id == kFieldIds) || ...);
  }

  // O(1) accessor for a field in the explicit set. Passing an undeclared id
  // is a compile-time error.
  template <uint32_t kFieldId>
  const Field& at() const {
    static_assert(ContainsField(kFieldId),
                  "kFieldId is not in the declared field set");
    return HasField(kFieldId) ? fields_[kFieldId] : kInvalidField;
  }

  // Iterable view over the unknown fields, in wire order, with repeated
  // occurrences preserved.
  UnknownFieldRange unknown_fields() const {
    static_assert(kStoreUnknownFields,
                  "Instantiated with kStoreUnknownFields=false");
    return {unknown_, unknown_ + unknown_size_};
  }

 private:
  // Speculates that few unknown fields exist; spills to the heap beyond.
  static constexpr uint32_t kUnknownInlineCapacity = 16;
  static constexpr size_t kNumWords = (kMaxFieldId + 64) / 64;

  static constexpr std::array<uint64_t, kNumWords> kExplicitBitmap =
      internal::MakeFieldBitmap<kNumWords>({kFieldIds...});
  static_assert(internal::CountBits(kExplicitBitmap) == sizeof...(kFieldIds),
                "Duplicate field ids in the declared set");

  Field fields_storage_[kMaxFieldId + 1];
  uint64_t presence_storage_[kNumWords];
  Field unknown_storage_[kStoreUnknownFields ? kUnknownInlineCapacity : 1];
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_SELECTIVE_PROTO_DECODER_H_
