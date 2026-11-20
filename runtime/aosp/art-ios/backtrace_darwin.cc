// BacktraceCollector for Darwin. The AOSP implementation (backtrace_helper.cc) is built
// on libunwindstack, which only understands Linux process maps and ELF images.

#include "backtrace_helper.h"

#include <execinfo.h>

#include <algorithm>
#include <vector>

namespace art HIDDEN {

void BacktraceCollector::Collect() {
  // One extra frame to skip Collect() itself.
  const size_t skip = skip_count_ + 1;
  std::vector<void*> frames(max_depth_ + skip);
  int captured = backtrace(frames.data(), static_cast<int>(frames.size()));
  for (size_t i = skip; i < static_cast<size_t>(std::max(captured, 0)); ++i) {
    out_frames_[num_frames_++] = reinterpret_cast<uintptr_t>(frames[i]);
  }
}

}  // namespace art
