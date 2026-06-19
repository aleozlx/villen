// glibc symbol-version pin (force-included into every TU via -include).
//
// This PC's glibc (2.43) re-versioned a few long-standing single-precision math
// functions, so a freshly-built binary binds e.g. sqrtf@GLIBC_2.43 — which the
// Steam Deck's older glibc (2.41) does not export, and the loader aborts before
// main() (the "crash-quit"). These functions have existed at the 2.2.5 baseline
// node forever and that node is present on both glibcs, so pin the references to
// it. ImGui's geometry code is what pulls these in.
#pragma once
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver acosf,  acosf@GLIBC_2.2.5");
__asm__(".symver atan2f, atan2f@GLIBC_2.2.5");
__asm__(".symver sqrtf,  sqrtf@GLIBC_2.2.5");
#endif
