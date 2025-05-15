#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_TYPES_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace perfetto::trace_processor::dataframe {

// Represents an index to speed up operations on the dataframe.
struct Index {
 public:
  Index(std::vector<uint32_t> _columns,
        std::shared_ptr<std::vector<uint32_t>> _permutation_vector)
      : columns_(std::move(_columns)),
        permutation_vector_(std::move(_permutation_vector)) {}

  Index Copy() const { return *this; }

  const std::vector<uint32_t>& columns() const { return columns_; }

  const std::shared_ptr<std::vector<uint32_t>>& permutation_vector() const {
    return permutation_vector_;
  }

 private:
  std::vector<uint32_t> columns_;
  std::shared_ptr<std::vector<uint32_t>> permutation_vector_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_TYPES_H_
