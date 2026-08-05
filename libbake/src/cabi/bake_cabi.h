#ifndef BAKE_CABI_H
#define BAKE_CABI_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef BAKE_BUILDING_DLL
    #define BAKE_API __declspec(dllexport)
  #else
    #define BAKE_API __declspec(dllimport)
  #endif
#else
  #define BAKE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
  #define BAKE_NOEXCEPT noexcept
#else
  #define BAKE_NOEXCEPT
#endif

#include <stdint.h>

// opaque handles
typedef struct bake_builder bake_builder;
typedef struct bake_target bake_target;
typedef struct bake_step bake_step;
typedef struct bake_dependency bake_dependency;

// Builder
BAKE_API bake_builder* bake_builder_new(void) BAKE_NOEXCEPT;
BAKE_API void          bake_builder_free(bake_builder*) BAKE_NOEXCEPT;
BAKE_API int           bake_builder_build(bake_builder*) BAKE_NOEXCEPT;

BAKE_API bake_target*     bake_builder_executable(bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target*     bake_builder_static_lib(bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target*     bake_builder_shared_lib(bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_step*       bake_builder_step(bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_dependency* bake_builder_dependency(bake_builder*, const char*) BAKE_NOEXCEPT;

BAKE_API int         bake_builder_option_bool(const bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API int64_t     bake_builder_option_int(const bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API const char* bake_builder_option_str(const bake_builder*, const char*) BAKE_NOEXCEPT;
BAKE_API const char* bake_builder_source_dir(const bake_builder*) BAKE_NOEXCEPT;
BAKE_API const char* bake_builder_build_dir(const bake_builder*) BAKE_NOEXCEPT;

// Target
BAKE_API bake_target* bake_target_std(bake_target*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_sources(bake_target*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_include_dirs(bake_target*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_define(bake_target*, const char*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_link(bake_target*, bake_target*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_link_system(bake_target*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_target* bake_target_depends_on_step(bake_target*, bake_step*) BAKE_NOEXCEPT;

// Step
BAKE_API bake_step* bake_step_outputs(bake_step*, const char*) BAKE_NOEXCEPT;
BAKE_API bake_step* bake_step_run(bake_step*, const char*, const char* const*, int) BAKE_NOEXCEPT;

// Dependency
BAKE_API const char* bake_dep_src_dir(const bake_dependency*) BAKE_NOEXCEPT;
BAKE_API void        bake_dep_link_to(bake_dependency*, bake_target*) BAKE_NOEXCEPT;

// Error (thread-local)
BAKE_API const char* bake_last_error(void) BAKE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif // BAKE_CABI_H
