#pragma once

/**
 * SKSE type compatibility shim for use with CommonLibSSE projects.
 * Provides the basic type definitions that SKSE headers expect.
 * Include this BEFORE any SKSE headers.
 */

#include <cstdint>
#include <cstdlib>

// SKSE integer types
typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;
typedef int8_t   SInt8;
typedef int16_t  SInt16;
typedef int32_t  SInt32;
typedef int64_t  SInt64;

// SKSE macros that we stub out since we don't need the actual implementations
#ifndef STATIC_ASSERT
#define STATIC_ASSERT(x) static_assert(x, #x)
#endif

#ifndef MEMBER_FN_PREFIX
#define MEMBER_FN_PREFIX(cls)
#endif

#ifndef DEFINE_MEMBER_FN
#define DEFINE_MEMBER_FN(name, ret, addr, ...)
#endif

#ifndef CALL_MEMBER_FN
#define CALL_MEMBER_FN(obj, fn) ((obj)->*(&fn))
#endif

#ifndef DEFINE_STATIC_HEAP
#define DEFINE_STATIC_HEAP(alloc, free)
#endif

#ifndef ASSERT
#define ASSERT(x)
#endif

// Memory functions
inline void* Heap_Allocate(size_t size) { return malloc(size); }
inline void Heap_Free(void* ptr) { free(ptr); }

// Logging stub
inline void _MESSAGE(const char*, ...) {}
