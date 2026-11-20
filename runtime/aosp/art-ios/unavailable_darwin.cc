// Definitions for pieces of the Android platform that libart links against but that do
// not exist in this port. Each one reports "not available" in the way its caller expects.

#include <string>

#include "jit_create.h"
#include "odr_statslog/odr_statslog.h"

namespace art {

namespace jit {
// The JIT compiler lives in libart-compiler. iOS forbids generating code at run time,
// so there is none; the runtime is started with the JIT disabled and never asks.
JitCompilerInterface* jit_create() { return nullptr; }
}  // namespace jit

namespace odrefresh {
// On-device recompilation statistics for statsd: nothing is ever recorded.
bool UploadStatsIfAvailable([[maybe_unused]] /*out*/ std::string* error_msg) { return true; }
}  // namespace odrefresh

}  // namespace art

extern "C" {

// ART calls these when present (HWASan builds); ELF resolves the weak references to
// null, Mach-O needs real definitions, so provide the no-tagging behaviour.
void* __hwasan_tag_pointer(const volatile void* p, [[maybe_unused]] unsigned char tag) {
  return const_cast<void*>(p);
}
void __hwasan_handle_longjmp([[maybe_unused]] const void* sp_dst) {}

}  // extern "C"
