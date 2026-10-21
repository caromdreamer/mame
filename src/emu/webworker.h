// license:BSD-3-Clause
// Browser-only host selection. Native SDL remains unchanged.
#pragma once
#include <cstdlib>
#include <cstring>
inline bool kn_web_worker()
{
#if defined(__EMSCRIPTEN__)
 static bool const enabled = [] { char const *value = std::getenv("KN_MAME_WORKER"); return value && !std::strcmp(value, "1"); }();
 return enabled;
#else
 return false;
#endif
}
