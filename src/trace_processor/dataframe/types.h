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
  Index Copy() const { return *this; }

 private:
  friend class Dataframe;

  Index(std::vector<uint32_t> _columns,
        std::shared_ptr<std::vector<uint32_t>> _permutation_vector)
      : columns(std::move(_columns)),
        permutation_vector(std::move(_permutation_vector)) {}

  std::vector<uint32_t> columns;
  std::shared_ptr<std::vector<uint32_t>> permutation_vector;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_TYPES_H_
