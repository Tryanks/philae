// Signal chaining for ART on Darwin.
//
// ART gets the first look at fault signals through "special handlers" and passes on the
// ones it does not own. On Android, libsigchain does that by interposing sigaction() for
// the whole process; that interposition is specific to the ELF loader. Here ART installs a
// real handler and chains to whatever was installed before it, which is sufficient as long
// as the runtime registers before the app's own native code does.

#include "sigchain.h"

#include <signal.h>
#include <string.h>

namespace art {

namespace {

struct Chain {
  SigchainAction special;      // sc_sigaction == nullptr when unclaimed
  struct sigaction previous;   // handler that was in place before ART's
};

Chain g_chains[NSIG];

void InvokePrevious(int signal, siginfo_t* info, void* context) {
  const struct sigaction& previous = g_chains[signal].previous;
  if ((previous.sa_flags & SA_SIGINFO) != 0) {
    previous.sa_sigaction(signal, info, context);
  } else if (previous.sa_handler == SIG_DFL) {
    // Restore the default disposition; returning re-executes the faulting instruction,
    // which then terminates the process the normal way (and produces a crash report).
    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    sigaction(signal, &action, nullptr);
  } else if (previous.sa_handler != SIG_IGN) {
    previous.sa_handler(signal);
  }
}

void Dispatch(int signal, siginfo_t* info, void* context) {
  bool (*special)(int, siginfo_t*, void*) = g_chains[signal].special.sc_sigaction;
  if (special != nullptr && special(signal, info, context)) {
    return;
  }
  InvokePrevious(signal, info, context);
}

void Install(int signal, struct sigaction* previous) {
  struct sigaction action = {};
  action.sa_sigaction = Dispatch;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  action.sa_mask = g_chains[signal].special.sc_mask;
  sigaction(signal, &action, previous);
}

}  // namespace

extern "C" void AddSpecialSignalHandlerFn(int signal, SigchainAction* sa) {
  if (signal <= 0 || signal >= NSIG) {
    return;
  }
  g_chains[signal].special = *sa;
  Install(signal, &g_chains[signal].previous);
}

extern "C" void RemoveSpecialSignalHandlerFn(int signal, bool (*fn)(int, siginfo_t*, void*)) {
  if (signal <= 0 || signal >= NSIG || g_chains[signal].special.sc_sigaction != fn) {
    return;
  }
  g_chains[signal].special.sc_sigaction = nullptr;
  sigaction(signal, &g_chains[signal].previous, nullptr);
}

extern "C" void EnsureFrontOfChain(int signal) {
  if (signal <= 0 || signal >= NSIG || g_chains[signal].special.sc_sigaction == nullptr) {
    return;
  }
  struct sigaction current = {};
  sigaction(signal, nullptr, &current);
  if ((current.sa_flags & SA_SIGINFO) == 0 || current.sa_sigaction != Dispatch) {
    // Someone installed a handler over ART's: go back in front and chain to theirs.
    Install(signal, &g_chains[signal].previous);
  }
}

extern "C" void SkipAddSignalHandler([[maybe_unused]] bool value) {}

}  // namespace art
