#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace js {

D *new_doc() {
  D *doc = yyjson_mut_doc_new(nullptr);
  if (!doc) return nullptr;
  return doc;
}

void free_doc(D *doc) { yyjson_mut_doc_free(doc); }

D *parse(std::string_view s) {
  yyjson_doc *immut = yyjson_read(s.data(), s.size(), 0);
  if (!immut) return nullptr;
  D *doc = yyjson_doc_mut_copy(immut, nullptr);
  yyjson_doc_free(immut);
  return doc;
}

V *obj(D *doc) { return yyjson_mut_obj(doc); }
V *arr(D *doc) { return yyjson_mut_arr(doc); }
// Copy into the document's pool: callers routinely pass temporaries or strings
// owned by another document, and yyjson's *_strn() would keep a borrowed pointer.
V *str(D *doc, std::string_view s) { return yyjson_mut_strncpy(doc, s.data(), s.size()); }
V *uint(D *doc, uint64_t n) { return yyjson_mut_uint(doc, n); }
V *sint(D *doc, int64_t n) { return yyjson_mut_sint(doc, n); }
V *real(D *doc, double n) { return yyjson_mut_real(doc, n); }
V *boolean(D *doc, bool b) { return yyjson_mut_bool(doc, b); }
V *null(D *doc) { return yyjson_mut_null(doc); }

void put(D *doc, V *object, std::string_view key, V *val) {
  if (!object || !val) return;
  yyjson_mut_obj_put(object, yyjson_mut_strncpy(doc, key.data(), key.size()), val);
}

void push(D *doc, V *array, V *val) {
  if (!array || !val) return;
  (void)doc;
  yyjson_mut_arr_add_val(array, val);
}

V *get(V *object, std::string_view key) {
  if (!object || !yyjson_mut_is_obj(object)) return nullptr;
  return yyjson_mut_obj_getn(object, key.data(), key.size());
}

const char *get_str(V *object, std::string_view key) {
  V *v = get(object, key);
  if (!v || !yyjson_mut_is_str(v)) return nullptr;
  return yyjson_mut_get_str(v);
}

bool get_int(V *object, std::string_view key, int64_t *out) {
  V *v = get(object, key);
  if (!v) return false;
  if (yyjson_mut_is_uint(v)) {
    *out = static_cast<int64_t>(yyjson_mut_get_uint(v));
    return true;
  }
  if (yyjson_mut_is_sint(v)) {
    *out = yyjson_mut_get_sint(v);
    return true;
  }
  if (yyjson_mut_is_real(v)) {
    *out = static_cast<int64_t>(yyjson_mut_get_real(v));
    return true;
  }
  return false;
}

bool is_obj(V *v) { return v && yyjson_mut_is_obj(v); }
bool is_arr(V *v) { return v && yyjson_mut_is_arr(v); }
bool is_str(V *v) { return v && yyjson_mut_is_str(v); }
bool is_num(V *v) { return v && yyjson_mut_is_num(v); }
bool is_null(V *v) { return !v || yyjson_mut_is_null(v); }

size_t arr_size(V *a) { return a && yyjson_mut_is_arr(a) ? yyjson_mut_arr_size(a) : 0; }

V *arr_at(V *a, size_t i) { return a && yyjson_mut_is_arr(a) ? yyjson_mut_arr_get(a, i) : nullptr; }

void for_each(V *object, const std::function<void(V *, V *)> &cb) {
  if (!object || !yyjson_mut_is_obj(object)) return;
  yyjson_mut_obj_iter iter;
  yyjson_mut_obj_iter_init(object, &iter);
  V *key = nullptr;
  while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
    V *val = yyjson_mut_obj_iter_get_val(key);
    cb(key, val);
  }
}

void for_each_arr(V *array, const std::function<void(V *)> &cb) {
  if (!array || !yyjson_mut_is_arr(array)) return;
  yyjson_mut_arr_iter iter;
  yyjson_mut_arr_iter_init(array, &iter);
  V *val = nullptr;
  while ((val = yyjson_mut_arr_iter_next(&iter)) != nullptr) cb(val);
}

V *copy(D *doc, V *val) {
  if (!val) return nullptr;
  return yyjson_mut_val_mut_copy(doc, val);
}

std::string dump(D *doc, V *val) {
  if (!doc || !val) return {};
  V *saved = yyjson_mut_doc_get_root(doc);
  yyjson_mut_doc_set_root(doc, val);

  size_t len = 0;
  char *buf = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_set_root(doc, saved);
  if (!buf) return {};

  std::string out(buf, len);
  std::free(buf);
  return out;
}

std::string dump_pretty(D *doc, V *val) {
  if (!doc || !val) return {};
  V *saved = yyjson_mut_doc_get_root(doc);
  yyjson_mut_doc_set_root(doc, val);

  size_t len = 0;
  char *buf = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
  yyjson_mut_doc_set_root(doc, saved);
  if (!buf) return {};

  std::string out(buf, len);
  std::free(buf);
  return out;
}

}  // namespace js
