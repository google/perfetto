/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/protozero/filtering/filter_bytecode_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/fnv_hash.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/compiler.h"
#include "src/protozero/filtering/filter_bytecode_common.h"

namespace protozero {

namespace {

// Parses varint-encoded bytecode and verifies checksum.
// Returns true on success, with the checksum removed from |words|.
bool ParseAndVerifyChecksum(const uint8_t* data,
                            size_t len,
                            std::vector<uint32_t>* words,
                            bool suppress_logs) {
  bool packed_parse_err = false;
  words->reserve(len);  // An overestimation, but avoids reallocations.
  using BytecodeDecoder =
      PackedRepeatedFieldIterator<proto_utils::ProtoWireType::kVarInt,
                                  uint32_t>;
  for (BytecodeDecoder it(data, len, &packed_parse_err); it; ++it)
    words->emplace_back(*it);

  if (packed_parse_err || words->empty())
    return false;

  perfetto::base::FnvHasher hasher;
  for (size_t i = 0; i < words->size() - 1; ++i)
    hasher.Update((*words)[i]);

  uint32_t expected_csum = static_cast<uint32_t>(hasher.digest());
  if (expected_csum != words->back()) {
    if (!suppress_logs) {
      PERFETTO_ELOG("Filter bytecode checksum failed. Expected: %x, actual: %x",
                    expected_csum, words->back());
    }
    return false;
  }

  words->pop_back();  // Pop the checksum.
  return true;
}

// Returns the size (in words) of an overlay entry based on its opcode.
// Returns 0 if the opcode is invalid for overlays.
size_t GetOverlayEntrySize(uint32_t opcode) {
  switch (opcode) {
    case kFilterOpcode_SimpleField:
    case kFilterOpcode_FilterString:
      return 2;  // msg_index + field_word
    default:
      return 0;  // Invalid opcode for overlay
  }
}

// Returns the msg_id to store for an overlay entry's opcode.
uint32_t GetOverlayMsgId(uint32_t opcode) {
  switch (opcode) {
    case kFilterOpcode_SimpleField:
      return FilterBytecodeParser::kSimpleField;
    case kFilterOpcode_FilterString:
      return FilterBytecodeParser::kFilterStringField;
    default:
      return 0;
  }
}

}  // namespace

void FilterBytecodeParser::Reset() {
  bool suppress = suppress_logs_for_fuzzer_;
  *this = FilterBytecodeParser();
  suppress_logs_for_fuzzer_ = suppress;
}

bool FilterBytecodeParser::Load(const void* filter_data,
                                size_t len,
                                const void* overlay_data,
                                size_t overlay_len) {
  Reset();
  bool res =
      LoadInternal(static_cast<const uint8_t*>(filter_data), len,
                   static_cast<const uint8_t*>(overlay_data), overlay_len);
  // If load fails, don't leave the parser in a half broken state.
  if (!res)
    Reset();
  return res;
}

bool FilterBytecodeParser::LoadInternal(const uint8_t* filter_data,
                                        size_t len,
                                        const uint8_t* overlay_data,
                                        size_t overlay_len) {
  // First unpack the varints into a plain uint32 vector, so it's easy to
  // iterate through them and look ahead.
  std::vector<uint32_t> words;
  if (!ParseAndVerifyChecksum(filter_data, len, &words,
                              suppress_logs_for_fuzzer_))
    return false;

  // Parse the overlay (if provided).
  std::vector<uint32_t> overlay;
  if (overlay_data && overlay_len > 0) {
    if (!ParseAndVerifyChecksum(overlay_data, overlay_len, &overlay,
                                suppress_logs_for_fuzzer_)) {
      return false;
    }
  }

  // Temporary storage for each message. Cleared on every END_OF_MESSAGE.
  std::vector<uint32_t> direct_indexed_fields;
  std::vector<uint32_t> ranges;
  uint32_t max_msg_index = 0;
  uint32_t current_msg_index = 0;
  size_t overlay_idx = 0;

  auto add_directly_indexed_field = [&](uint32_t field_id, uint32_t msg_id) {
    PERFETTO_DCHECK(field_id > 0 && field_id < kDirectlyIndexLimit);
    direct_indexed_fields.resize(std::max(direct_indexed_fields.size(),
                                          static_cast<size_t>(field_id) + 1));
    direct_indexed_fields[field_id] = kAllowed | msg_id;
  };

  auto add_range = [&](uint32_t id_start, uint32_t id_end, uint32_t msg_id) {
    PERFETTO_DCHECK(id_end > id_start);
    PERFETTO_DCHECK(id_start >= kDirectlyIndexLimit);
    ranges.emplace_back(id_start);
    ranges.emplace_back(id_end);
    ranges.emplace_back(kAllowed | msg_id);
  };

  // Merges overlay entries into the current message being built.
  //
  // This function processes overlay entries for the current message up to (and
  // including) the given |field_id|. Since both base bytecode and overlay are
  // sorted by (msg_index, field_id), we can use a two-pointer merge approach:
  // - Entries with field_id < the given field_id are ADDed as new fields
  // - An entry with field_id == the given field_id is an UPGRADE (returned)
  // - Entries with field_id > the given field_id are left for later
  //
  // Pass std::numeric_limits<uint32_t>::max() as field_id to drain all
  // remaining entries for the current message (called at EndOfMessage).
  //
  // Returns:
  // - The msg_id to use if there's an exact match (upgrade case)
  // - 0 if no match (use the base bytecode's msg_id)
  // - std::numeric_limits<uint32_t>::max() on error
  constexpr uint32_t kOverlayError = std::numeric_limits<uint32_t>::max();
  auto process_overlay = [&](uint32_t field_id) -> uint32_t {
    uint32_t matched_msg_id = 0;
    while (overlay_idx < overlay.size()) {
      // Each overlay entry starts with [msg_index, field_word, ...].
      // We need at least 2 words to read the header.
      if (PERFETTO_UNLIKELY(overlay_idx + 1 >= overlay.size())) {
        PERFETTO_DLOG("overlay error: truncated entry at index %zu",
                      overlay_idx);
        return kOverlayError;
      }

      // Parse the entry header.
      uint32_t entry_msg = overlay[overlay_idx];
      uint32_t entry_word = overlay[overlay_idx + 1];
      uint32_t entry_opcode = entry_word & 0x7u;
      uint32_t entry_field = entry_word >> 3;

      // Validate the opcode and ensure we have enough words for this entry.
      size_t entry_size = GetOverlayEntrySize(entry_opcode);
      if (PERFETTO_UNLIKELY(entry_size == 0)) {
        PERFETTO_DLOG("overlay error: invalid opcode %u at index %zu",
                      entry_opcode, overlay_idx);
        return kOverlayError;
      }
      if (PERFETTO_UNLIKELY(overlay_idx + entry_size > overlay.size())) {
        PERFETTO_DLOG("overlay error: entry at index %zu exceeds size",
                      overlay_idx);
        return kOverlayError;
      }

      // Stop if this entry is for a later message or a later field.
      if (entry_msg > current_msg_index ||
          (entry_msg == current_msg_index && entry_field > field_id)) {
        break;
      }

      // Entries for earlier messages indicate the overlay is not sorted.
      if (PERFETTO_UNLIKELY(entry_msg < current_msg_index)) {
        PERFETTO_DLOG(
            "overlay error: entry for msg %u at index %zu, "
            "but current msg is %u (overlay not sorted?)",
            entry_msg, overlay_idx, current_msg_index);
        return kOverlayError;
      }

      // At this point: entry_msg == current_msg_index && entry_field <=
      // field_id
      uint32_t msg_id = GetOverlayMsgId(entry_opcode);

      if (entry_field == field_id) {
        // Exact match - this is an upgrade. Return the msg_id for the caller.
        matched_msg_id = msg_id;
        overlay_idx += entry_size;
        break;
      }

      // entry_field < field_id: This is a new field to ADD.
      if (entry_field > 0) {
        if (entry_field < kDirectlyIndexLimit) {
          add_directly_indexed_field(entry_field, msg_id);
        } else {
          add_range(entry_field, entry_field + 1, msg_id);
        }
      }
      overlay_idx += entry_size;
    }
    return matched_msg_id;
  };

  bool is_eom = true;
  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t word = words[i];
    const bool has_next_word = i < words.size() - 1;
    const uint32_t opcode = word & 0x7u;
    const uint32_t field_id = word >> 3;

    is_eom = opcode == kFilterOpcode_EndOfMessage;
    if (field_id == 0 && opcode != kFilterOpcode_EndOfMessage) {
      PERFETTO_DLOG("bytecode error @ word %zu, invalid field id (0)", i);
      return false;
    }

    if (opcode == kFilterOpcode_SimpleField ||
        opcode == kFilterOpcode_NestedField ||
        opcode == kFilterOpcode_FilterString) {
      // Field words are organized as follow:
      // MSB: 1 if allowed, 0 if not allowed.
      // Remaining bits:
      //   Message index in the case of nested (non-simple) messages.
      //   0x7f..e in the case of string fields which need filtering.
      //   0x7f..f in the case of simple fields.
      uint32_t msg_id;
      if (opcode == kFilterOpcode_SimpleField) {
        msg_id = kSimpleField;
      } else if (opcode == kFilterOpcode_FilterString) {
        msg_id = kFilterStringField;
      } else {  // FILTER_OPCODE_NESTED_FIELD
        // The next word in the bytecode contains the message index.
        if (!has_next_word) {
          PERFETTO_DLOG("bytecode error @ word %zu: unterminated nested field",
                        i);
          return false;
        }
        msg_id = words[++i];
        max_msg_index = std::max(max_msg_index, msg_id);
      }

      // Process overlay: add any fields that come before this one, and check
      // if this field should be upgraded.
      uint32_t overlay_msg_id = process_overlay(field_id);
      if (overlay_msg_id == kOverlayError) {
        return false;
      }
      if (overlay_msg_id != 0) {
        msg_id = overlay_msg_id;
      }

      if (field_id < kDirectlyIndexLimit) {
        add_directly_indexed_field(field_id, msg_id);
      } else {
        // In the case of a large field id (rare) we waste an extra word and
        // represent it as a range. Doesn't make sense to introduce extra
        // complexity to deal with rare cases like this.
        add_range(field_id, field_id + 1, msg_id);
      }
    } else if (opcode == kFilterOpcode_SimpleFieldRange) {
      if (!has_next_word) {
        PERFETTO_DLOG("bytecode error @ word %zu: unterminated range", i);
        return false;
      }
      const uint32_t range_len = words[++i];
      const uint32_t range_end = field_id + range_len;  // STL-style, excl.
      uint32_t id = field_id;

      // Here's the subtle complexity: at the bytecode level, we don't know
      // anything about the kDirectlyIndexLimit. It is legit to define a range
      // that spans across the direct-indexing threshold (e.g. 126-132). In that
      // case we want to add all the elements < the indexing to the O(1) bucket
      // and add only the remaining range as a non-indexed range.
      for (; id < range_end && id < kDirectlyIndexLimit; ++id)
        add_directly_indexed_field(id, kAllowed | kSimpleField);
      PERFETTO_DCHECK(id >= kDirectlyIndexLimit || id == range_end);
      if (id < range_end)
        add_range(id, range_end, kSimpleField);
    } else if (opcode == kFilterOpcode_EndOfMessage) {
      // Drain any remaining overlay entries for this message.
      if (process_overlay(std::numeric_limits<uint32_t>::max()) ==
          kOverlayError) {
        return false;
      }

      // For each message append:
      // 1. The "header" word telling how many directly indexed fields there
      //    are.
      // 2. The words for the directly indexed fields (id < 128).
      // 3. The rest of the fields, encoded as ranges.
      // Also update the |message_offset_| index to remember the word offset for
      // the current message.
      message_offset_.emplace_back(static_cast<uint32_t>(words_.size()));
      words_.emplace_back(static_cast<uint32_t>(direct_indexed_fields.size()));
      words_.insert(words_.end(), direct_indexed_fields.begin(),
                    direct_indexed_fields.end());
      words_.insert(words_.end(), ranges.begin(), ranges.end());
      direct_indexed_fields.clear();
      ranges.clear();
      ++current_msg_index;
    } else {
      PERFETTO_DLOG("bytecode error @ word %zu: invalid opcode (%x)", i, word);
      return false;
    }
  }  // (for word in bytecode).

  if (!is_eom) {
    PERFETTO_DLOG(
        "bytecode error: end of message not the last word in the bytecode");
    return false;
  }

  if (max_msg_index > 0 && max_msg_index >= message_offset_.size()) {
    PERFETTO_DLOG(
        "bytecode error: a message index (%u) is out of range "
        "(num_messages=%zu)",
        max_msg_index, message_offset_.size());
    return false;
  }

  // Add a final entry to |message_offset_| so we can tell where the last
  // message ends without an extra branch in the Query() hotpath.
  message_offset_.emplace_back(static_cast<uint32_t>(words_.size()));

  return true;
}

FilterBytecodeParser::QueryResult FilterBytecodeParser::Query(
    uint32_t msg_index,
    uint32_t field_id) const {
  FilterBytecodeParser::QueryResult res{false, 0u};
  if (static_cast<uint64_t>(msg_index) + 1 >=
      static_cast<uint64_t>(message_offset_.size())) {
    return res;
  }
  const uint32_t start_offset = message_offset_[msg_index];
  // These are DCHECKs and not just CHECKS because the |words_| is populated
  // by the LoadInternal call above. These cannot be violated with a malformed
  // bytecode.
  PERFETTO_DCHECK(start_offset < words_.size());
  const uint32_t* word = &words_[start_offset];
  const uint32_t end_off = message_offset_[msg_index + 1];
  const uint32_t* const end = words_.data() + end_off;
  PERFETTO_DCHECK(end > word && end <= words_.data() + words_.size());
  const uint32_t num_directly_indexed = *(word++);
  PERFETTO_DCHECK(num_directly_indexed <= kDirectlyIndexLimit);
  PERFETTO_DCHECK(word + num_directly_indexed <= end);
  uint32_t field_state = 0;
  if (PERFETTO_LIKELY(field_id < num_directly_indexed)) {
    PERFETTO_DCHECK(&word[field_id] < end);
    field_state = word[field_id];
  } else {
    for (word = word + num_directly_indexed; word + 2 < end;) {
      const uint32_t range_start = *(word++);
      const uint32_t range_end = *(word++);
      const uint32_t range_state = *(word++);
      if (field_id >= range_start && field_id < range_end) {
        field_state = range_state;
        break;
      }
    }  // for (word in ranges)
  }  // if (field_id >= num_directly_indexed)

  res.allowed = (field_state & kAllowed) != 0;
  res.nested_msg_index = field_state & ~kAllowed;
  PERFETTO_DCHECK(!res.nested_msg_field() ||
                  res.nested_msg_index < message_offset_.size() - 1);
  return res;
}

}  // namespace protozero
