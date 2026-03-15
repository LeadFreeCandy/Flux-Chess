#pragma once
#include <Arduino.h>

// ══════════════════════════════════════════════════════════════
// FLUX_ENUM — define an enum class with automatic toJson
//
// Usage:
//   FLUX_ENUM(MyError, OK, BAD_INPUT, TIMEOUT)
//
// Generates:
//   enum class MyError : uint8_t { OK, BAD_INPUT, TIMEOUT };
//   const char* toJson(MyError e) → "\"OK\"", "\"BAD_INPUT\"", etc.
// ══════════════════════════════════════════════════════════════

#define FLUX_ENUM(Name, ...)                                          \
  enum class Name : uint8_t { __VA_ARGS__ };                          \
  inline const char* toJson(Name e) {                                 \
    static const char* _names[] = { _FLUX_ENUM_STRINGS(__VA_ARGS__) };\
    uint8_t i = (uint8_t)e;                                           \
    if (i >= sizeof(_names)/sizeof(_names[0])) return "\"UNKNOWN\"";  \
    return _names[i];                                                 \
  }

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

// ══════════════════════════════════════════════════════════════
// Json — fluent JSON builder with automatic type handling
//
// Usage:
//   Json().add("success", true).add("error", myEnum).build()
//
// Handles: bool, integers, const char*, String, FLUX_ENUMs
// For raw JSON (arrays, nested objects): use addRaw()
// ══════════════════════════════════════════════════════════════

class Json {
public:
  Json() { buf_ = "{"; }

  Json& add(const char* key, bool val) {
    sep(); key_(key); buf_ += val ? "true" : "false";
    return *this;
  }

  Json& add(const char* key, int val) {
    sep(); key_(key); buf_ += String(val);
    return *this;
  }

  Json& add(const char* key, uint8_t val) {
    sep(); key_(key); buf_ += String(val);
    return *this;
  }

  Json& add(const char* key, uint16_t val) {
    sep(); key_(key); buf_ += String(val);
    return *this;
  }

  Json& add(const char* key, uint32_t val) {
    sep(); key_(key); buf_ += String(val);
    return *this;
  }

  // Enum types (anything with a toJson() free function returning const char*)
  // The toJson() returns pre-quoted strings like "\"NONE\""
  template<typename T>
  auto add(const char* key, T val) -> decltype(toJson(val), *this) {
    sep(); key_(key); buf_ += toJson(val);
    return *this;
  }

  // Raw JSON value (already formatted — arrays, objects, etc.)
  Json& addRaw(const char* key, const String& val) {
    sep(); key_(key); buf_ += val;
    return *this;
  }

  // String value (will be quoted)
  Json& addStr(const char* key, const char* val) {
    sep(); key_(key); buf_ += "\""; buf_ += val; buf_ += "\"";
    return *this;
  }

  String build() { return buf_ + "}"; }

private:
  String buf_;
  bool first_ = true;
  void sep() { if (!first_) buf_ += ","; first_ = false; }
  void key_(const char* k) { buf_ += "\""; buf_ += k; buf_ += "\":"; }
};

// ══════════════════════════════════════════════════════════════
// JSON parsing helpers
// ══════════════════════════════════════════════════════════════

inline String jsonGet(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int idx = json.indexOf(search);
  if (idx < 0) return "";
  idx = json.indexOf(':', idx);
  if (idx < 0) return "";
  idx++;
  while (idx < (int)json.length() && json[idx] == ' ') idx++;
  if (json[idx] == '"') {
    int end = json.indexOf('"', idx + 1);
    return json.substring(idx + 1, end);
  }
  int end = idx;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}') end++;
  return json.substring(idx, end);
}

inline String jsonGetObj(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int idx = json.indexOf(search);
  if (idx < 0) return "";
  idx = json.indexOf('{', idx);
  if (idx < 0) return "";
  int depth = 0;
  for (int i = idx; i < (int)json.length(); i++) {
    if (json[i] == '{') depth++;
    if (json[i] == '}') depth--;
    if (depth == 0) return json.substring(idx, i + 1);
  }
  return "";
}
