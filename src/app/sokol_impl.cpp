// ============================================================================
// src/app/sokol_impl.cpp — Sokol single-translation-unit implementation
// ============================================================================
// Must be compiled as Objective-C++ on macOS (the CMakeLists sets .mm or
// uses -x objective-c++ via COMPILE_OPTIONS).
// ============================================================================

// Pick Metal backend on Apple platforms, GL everywhere else.
#if defined(__APPLE__)
#  define SOKOL_METAL
#else
#  define SOKOL_GLCORE
#endif

#define SOKOL_IMPL
#define SOKOL_NO_DEPRECATED

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#pragma clang diagnostic pop
