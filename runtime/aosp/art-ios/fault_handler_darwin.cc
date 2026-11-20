// arm64 fault handling for Darwin.
//
// On Android, compiled code omits null, stack-overflow and suspend checks and lets the
// resulting SIGSEGV be turned into the right Java behaviour by these handlers. The
// interpreter performs all three checks explicitly, and the host starts the runtime
// without a fault manager, so the handlers only need to decline faults.

#include "fault_handler.h"

#include <sys/ucontext.h>

namespace art HIDDEN {

uintptr_t FaultManager::GetFaultPc([[maybe_unused]] siginfo_t* siginfo, void* context) {
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(context);
  return __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
}

uintptr_t FaultManager::GetFaultSp(void* context) {
  ucontext_t* uc = reinterpret_cast<ucontext_t*>(context);
  return __darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss);
}

bool NullPointerHandler::Action([[maybe_unused]] int sig,
                                [[maybe_unused]] siginfo_t* info,
                                [[maybe_unused]] void* context) {
  return false;
}

bool SuspensionHandler::Action([[maybe_unused]] int sig,
                               [[maybe_unused]] siginfo_t* info,
                               [[maybe_unused]] void* context) {
  return false;
}

bool StackOverflowHandler::Action([[maybe_unused]] int sig,
                                  [[maybe_unused]] siginfo_t* info,
                                  [[maybe_unused]] void* context) {
  return false;
}

}  // namespace art
