// ART's fast interpreter ("nterp") is hand-written assembly and is not built for Darwin;
// IsNterpSupported() reports that, so the runtime uses the portable C++ interpreter.
// nterp.cc is still compiled for its C++ helpers, which the switch interpreter shares,
// and it takes the addresses of the assembly's entry points. These satisfy the linker.

extern "C" {

void ExecuteNterpImpl() {}
void EndExecuteNterpImpl() {}
void ExecuteNterpWithClinitImpl() {}
void EndExecuteNterpWithClinitImpl() {}

void* artNterpAsmInstructionStart[] = {nullptr};
void* artNterpAsmInstructionEnd[] = {nullptr};

}  // extern "C"
