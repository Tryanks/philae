// libunwindstack walks Linux /proc maps and ELF unwind tables, neither of which exists
// on iOS. ART only threads this object through to its native stack dumper, which has a
// Darwin implementation that does not use it (see art-ios/native_stack_darwin.cc).
#pragma once
#include <string>
#include <vector>

namespace unwindstack {

class AndroidLocalUnwinder {
  public:
    AndroidLocalUnwinder() = default;
    AndroidLocalUnwinder(const std::vector<std::string>& /*initial_map_names_to_skip*/,
                         const std::vector<std::string>& /*map_suffixes_to_ignore*/) {}
};

}  // namespace unwindstack
