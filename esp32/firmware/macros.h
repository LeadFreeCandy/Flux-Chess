#pragma once

// ── FLUX_ENUM: define an enum class with automatic toJson ────
//
// Usage:
//   FLUX_ENUM(MyError, OK, BAD_INPUT, TIMEOUT)
//
// Generates:
//   enum class MyError : uint8_t { OK, BAD_INPUT, TIMEOUT };
//   inline const char* toJson(MyError e) { ... }

#define FLUX_ENUM(Name, ...)                                          \
  enum class Name : uint8_t { __VA_ARGS__ };                          \
  inline const char* toJson(Name e) {                                 \
    static const char* _names[] = { _FLUX_ENUM_STRINGS(__VA_ARGS__) };\
    uint8_t i = (uint8_t)e;                                           \
    if (i >= sizeof(_names)/sizeof(_names[0])) return "\"UNKNOWN\"";  \
    return _names[i];                                                 \
  }

// ── Internal macro machinery (ignore) ────────────────────────

#define _FLUX_STR(x) "\"" #x "\""
#define _FLUX_ENUM_STRINGS(...) _FLUX_APPLY(_FLUX_STR, __VA_ARGS__)
#define _FLUX_EXPAND(x) x
#define _FLUX_APPLY(m, ...) _FLUX_EXPAND(_FLUX_APPLY_N(__VA_ARGS__, \
  16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)(m, __VA_ARGS__))
#define _FLUX_APPLY_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) _FLUX_APPLY_##N
#define _FLUX_APPLY_1(m,a) m(a)
#define _FLUX_APPLY_2(m,a,...) m(a), _FLUX_APPLY_1(m,__VA_ARGS__)
#define _FLUX_APPLY_3(m,a,...) m(a), _FLUX_APPLY_2(m,__VA_ARGS__)
#define _FLUX_APPLY_4(m,a,...) m(a), _FLUX_APPLY_3(m,__VA_ARGS__)
#define _FLUX_APPLY_5(m,a,...) m(a), _FLUX_APPLY_4(m,__VA_ARGS__)
#define _FLUX_APPLY_6(m,a,...) m(a), _FLUX_APPLY_5(m,__VA_ARGS__)
#define _FLUX_APPLY_7(m,a,...) m(a), _FLUX_APPLY_6(m,__VA_ARGS__)
#define _FLUX_APPLY_8(m,a,...) m(a), _FLUX_APPLY_7(m,__VA_ARGS__)
#define _FLUX_APPLY_9(m,a,...) m(a), _FLUX_APPLY_8(m,__VA_ARGS__)
#define _FLUX_APPLY_10(m,a,...) m(a), _FLUX_APPLY_9(m,__VA_ARGS__)
#define _FLUX_APPLY_11(m,a,...) m(a), _FLUX_APPLY_10(m,__VA_ARGS__)
#define _FLUX_APPLY_12(m,a,...) m(a), _FLUX_APPLY_11(m,__VA_ARGS__)
#define _FLUX_APPLY_13(m,a,...) m(a), _FLUX_APPLY_12(m,__VA_ARGS__)
#define _FLUX_APPLY_14(m,a,...) m(a), _FLUX_APPLY_13(m,__VA_ARGS__)
#define _FLUX_APPLY_15(m,a,...) m(a), _FLUX_APPLY_14(m,__VA_ARGS__)
#define _FLUX_APPLY_16(m,a,...) m(a), _FLUX_APPLY_15(m,__VA_ARGS__)
