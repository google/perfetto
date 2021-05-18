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

#ifndef SRC_TRACE_PROCESSOR_UTIL_INTERNED_MESSAGE_VIEW_H_
#define SRC_TRACE_PROCESSOR_UTIL_INTERNED_MESSAGE_VIEW_H_

#include "src/trace_processor/util/trace_blob_view.h"

#include <unordered_map>

namespace perfetto {
namespace trace_processor {

#if PERFETTO_DCHECK_IS_ON()
// When called from GetOrCreateDecoder(), should include the stringified name of
// the MessageType.
#define PERFETTO_TYPE_IDENTIFIER PERFETTO_DEBUG_FUNCTION_IDENTIFIER()
#else  // PERFETTO_DCHECK_IS_ON()
#define PERFETTO_TYPE_IDENTIFIER nullptr
#endif  // PERFETTO_DCHECK_IS_ON()

// Entry in an interning index, refers to the interned message.
class InternedMessageView {
 public:
  InternedMessageView(TraceBlobView msg) : message_(std::move(msg)) {}

  InternedMessageView(InternedMessageView&&) = default;
  InternedMessageView& operator=(InternedMessageView&&) = default;

  // Allow copy by cloning the TraceBlobView. This is required for
  // UpdateTracePacketDefaults().
  InternedMessageView(const InternedMessageView& view)
      : message_(view.message_.slice(0, view.message_.length())) {}
  InternedMessageView& operator=(const InternedMessageView& view) {
    this->message_ = view.message_.slice(0, view.message_.length());
    this->decoder_ = nullptr;
    this->decoder_type_ = nullptr;
    this->submessages_.clear();
    return *this;
  }

  // Lazily initializes and returns the decoder object for the message. The
  // decoder is stored in the InternedMessageView to avoid having to parse the
  // message multiple times.
  template <typename MessageType>
  typename MessageType::Decoder* GetOrCreateDecoder() {
    if (!decoder_) {
      // Lazy init the decoder and save it away, so that we don't have to
      // reparse the message every time we access the interning entry.
      decoder_ = std::unique_ptr<void, std::function<void(void*)>>(
          new typename MessageType::Decoder(message_.data(), message_.length()),
          [](void* obj) {
            delete reinterpret_cast<typename MessageType::Decoder*>(obj);
          });
      decoder_type_ = PERFETTO_TYPE_IDENTIFIER;
    }
    // Verify that the type of the decoder didn't change.
    if (PERFETTO_TYPE_IDENTIFIER &&
        strcmp(decoder_type_,
               // GCC complains if this arg can be null.
               PERFETTO_TYPE_IDENTIFIER ? PERFETTO_TYPE_IDENTIFIER : "") != 0) {
      PERFETTO_FATAL(
          "Interning entry accessed under different types! previous type: "
          "%s. new type: %s.",
          decoder_type_, PERFETTO_DEBUG_FUNCTION_IDENTIFIER());
    }
    return reinterpret_cast<typename MessageType::Decoder*>(decoder_.get());
  }

  // Lookup a submessage of the interned message, which is then itself stored
  // as InternedMessageView, so that we only need to parse it once. Returns
  // nullptr if the field isn't set.
  // TODO(eseckler): Support repeated fields.
  template <typename MessageType, uint32_t FieldId>
  InternedMessageView* GetOrCreateSubmessageView() {
    auto it = submessages_.find(FieldId);
    if (it != submessages_.end())
      return it->second.get();
    auto* decoder = GetOrCreateDecoder<MessageType>();
    // Calls the at() template method on the decoder.
    auto field = decoder->template at<FieldId>().as_bytes();
    if (!field.data)
      return nullptr;
    const size_t offset = message_.offset_of(field.data);
    TraceBlobView submessage = message_.slice(offset, field.size);
    InternedMessageView* submessage_view =
        new InternedMessageView(std::move(submessage));
    submessages_.emplace_hint(
        it, FieldId, std::unique_ptr<InternedMessageView>(submessage_view));
    return submessage_view;
  }

  const TraceBlobView& message() { return message_; }

 private:
  using SubMessageViewMap =
      std::unordered_map<uint32_t /*field_id*/,
                         std::unique_ptr<InternedMessageView>>;

  TraceBlobView message_;

  // Stores the decoder for the message_, so that the message does not have to
  // be re-decoded every time the interned message is looked up. Lazily
  // initialized in GetOrCreateDecoder(). Since we don't know the type of the
  // decoder until GetOrCreateDecoder() is called, we store the decoder as a
  // void* unique_pointer with a destructor function that's supplied in
  // GetOrCreateDecoder() when the decoder is created.
  std::unique_ptr<void, std::function<void(void*)>> decoder_;

  // Type identifier for the decoder. Only valid in debug builds and on
  // supported platforms. Used to verify that GetOrCreateDecoder() is always
  // called with the same template argument.
  const char* decoder_type_ = nullptr;

  // Views of submessages of the interned message. Submessages are lazily
  // added by GetOrCreateSubmessageView(). By storing submessages and their
  // decoders, we avoid having to decode submessages multiple times if they
  // looked up often.
  SubMessageViewMap submessages_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_INTERNED_MESSAGE_VIEW_H_
