#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_

#include <deque>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

class DeobfuscationTracker : public Destructible {
 public:
  explicit DeobfuscationTracker();

  static DeobfuscationTracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->deobfuscation_tracker) {
      context->deobfuscation_tracker.reset(new DeobfuscationTracker());
    }
    return static_cast<DeobfuscationTracker*>(
        context->deobfuscation_tracker.get());
  }

  ~DeobfuscationTracker() override;

  void AddDeobfuscationPacket(protozero::ConstBytes);

  class PacketRange {
   public:
    using InternalIterator = std::deque<TraceBlob>::const_iterator;
    PacketRange(InternalIterator begin, InternalIterator end)
        : begin_(begin), end_(end) {}

    class Iterator {
     public:
      using difference_type = InternalIterator::difference_type;
      using value_type = protozero::ConstBytes;
      using pointer = protozero::ConstBytes*;
      using reference = protozero::ConstBytes&;
      using iterator_category = InternalIterator::iterator_category;
      explicit Iterator(InternalIterator it) : it_(it) {}

      bool operator==(const Iterator& other) const { return it_ == other.it_; }
      bool operator!=(const Iterator& other) const { return it_ != other.it_; }
      protozero::ConstBytes operator*() {
        protozero::ConstBytes ret;
        ret.data = it_->data();
        ret.size = it_->size();
        return ret;
      }
      Iterator& operator++() {
        it_++;
        return *this;
      }
      Iterator operator++(int) {
        Iterator prev = *this;
        it_++;
        return prev;
      }

     private:
      InternalIterator it_;
    };

    Iterator begin() const { return Iterator(begin_); }
    Iterator end() const { return Iterator(end_); }

   private:
    InternalIterator begin_;
    InternalIterator end_;
  };

  PacketRange packets() const {
    return PacketRange(packets_.begin(), packets_.end());
  }

 private:
  std::deque<TraceBlob> packets_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_DEOBFUSCATION_TRACKER_H_
