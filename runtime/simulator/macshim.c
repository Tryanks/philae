// Simulator-only shim for the prebuilt macOS build of Android's host runtime
// (libandroid_runtime.dylib, see scripts/prepare-prebuilt-runtime.sh).
//
// That library links CoreServices, which does not exist in the iOS simulator, for the
// classic Text Encoding Converter (reached by ICU's fallback for platform code pages).
// Its CoreServices reference is redirected to this library, which answers the converter
// calls with an error; ICU then reports that the conversion is unavailable.

#include <stdint.h>

enum { kParamErr = -50 };

#define UNAVAILABLE(name) \
    int32_t name(void) { return kParamErr; }

UNAVAILABLE(ConvertFromTextToUnicode)
UNAVAILABLE(ConvertFromUnicodeToText)
UNAVAILABLE(CreateTextEncoding)
UNAVAILABLE(CreateTextToUnicodeInfo)
UNAVAILABLE(CreateUnicodeToTextInfo)
UNAVAILABLE(DisposeTextToUnicodeInfo)
UNAVAILABLE(DisposeUnicodeToTextInfo)
UNAVAILABLE(UCCompareTextDefault)
UNAVAILABLE(UpgradeScriptInfoToTextEncoding)
