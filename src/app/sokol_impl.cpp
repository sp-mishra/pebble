// ============================================================================
// src/app/sokol_impl.cpp — Sokol single-translation-unit implementation
// ============================================================================
// Must be compiled as Objective-C++ on macOS (the CMakeLists sets .mm or
// uses -x objective-c++ via COMPILE_OPTIONS).
// ============================================================================

#if defined(__APPLE__)
#  if !defined(SOKOL_METAL)
#    define SOKOL_METAL
#  endif
#else
#  if !defined(SOKOL_GLCORE)
#    define SOKOL_GLCORE
#  endif
#endif

#define SOKOL_IMPL
#define SOKOL_NO_DEPRECATED

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wenum-enum-conversion"
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#pragma clang diagnostic pop
