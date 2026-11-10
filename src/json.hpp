#pragma once

// Thin, dependency-light wrapper around yyjson's mutable DOM.
// Only what arRPC's protocol needs: parse, build, copy, serialize.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "yyjson.h"

namespace js {

using D = yyjson_mut_doc;
using V = yyjson_mut_val;

D *new_doc();
void free_doc(D *doc);

// Parses `s` into a new mutable document (root set). Returns nullptr on error.
D *parse(std::string_view s);

// --- constructors -----------------------------------------------------------
V *obj(D *doc);
V *arr(D *doc);
V *str(D *doc, std::string_view s);
V *uint(D *doc, uint64_t n);
V *sint(D *doc, int64_t n);
V *real(D *doc, double n);
V *boolean(D *doc, bool b);
V *null(D *doc);

// --- mutation ---------------------------------------------------------------
// Set object member `key` to `val`, replacing an existing member.
void put(D *doc, V *object, std::string_view key, V *val);
void push(D *doc, V *array, V *val);

// --- reading ----------------------------------------------------------------
V *get(V *object, std::string_view key);
const char *get_str(V *object, std::string_view key);  // nullptr if absent/not a string
bool get_int(V *object, std::string_view key, int64_t *out);
bool is_obj(V *v);
bool is_arr(V *v);
bool is_str(V *v);
bool is_num(V *v);
bool is_null(V *v);
size_t arr_size(V *arr);
V *arr_at(V *arr, size_t index);

// Iterate an object or array (mutable DOM).
void for_each(V *object, const std::function<void(V *key, V *val)> &cb);
void for_each_arr(V *array, const std::function<void(V *val)> &cb);

// Deep copy `val` into `doc`.
V *copy(D *doc, V *val);

// Serialize `val` (must belong to `doc`).
std::string dump(D *doc, V *val);
std::string dump_pretty(D *doc, V *val);

}  // namespace js
