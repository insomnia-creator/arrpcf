#include "detect.hpp"

#include <cstring>

#include "detectable_darwin.hpp"
#include "net.hpp"
#include "yyjson.h"

namespace {

std::string remove_first(std::string text, const std::string &needle) {
  size_t pos = text.find(needle);
  if (pos != std::string::npos) text.erase(pos, needle.size());
  return text;
}

bool app_bundle_match(const std::string &path_lower, const std::string &exe_name) {
  if (!net::ends_with(exe_name, ".app")) return false;
  // macOS reports the inner binary of a bundle, so compare the bundle directory
  // name anywhere in the path (e.g. .../Brawlhalla.app/Contents/MacOS/Brawlhalla).
  return path_lower.find("/" + exe_name + "/") != std::string::npos;
}

// Walks Discord's detectable-DB JSON into DetectableApp records, keeping only
// darwin executables (the shape is identical for the full DB and our subset).
void parse_entries(yyjson_val *root, std::vector<DetectableApp> *out) {
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(root, &iter);
  yyjson_val *entry = nullptr;
  while ((entry = yyjson_arr_iter_next(&iter)) != nullptr) {
    if (!yyjson_is_obj(entry)) continue;
    yyjson_val *id = yyjson_obj_get(entry, "id");
    yyjson_val *name = yyjson_obj_get(entry, "name");
    yyjson_val *exes = yyjson_obj_get(entry, "executables");
    if (!yyjson_is_str(id) || !yyjson_is_str(name) || !yyjson_is_arr(exes)) continue;

    DetectableApp app;
    app.id = yyjson_get_str(id);
    app.name = yyjson_get_str(name);

    yyjson_arr_iter ex_iter;
    yyjson_arr_iter_init(exes, &ex_iter);
    yyjson_val *ex = nullptr;
    while ((ex = yyjson_arr_iter_next(&ex_iter)) != nullptr) {
      if (!yyjson_is_obj(ex)) continue;

      yyjson_val *ex_name = yyjson_obj_get(ex, "name");
      if (!yyjson_is_str(ex_name)) continue;

      yyjson_val *os = yyjson_obj_get(ex, "os");
      if (yyjson_is_str(os) && std::strcmp(yyjson_get_str(os), "darwin") != 0) continue;

      yyjson_val *launcher = yyjson_obj_get(ex, "is_launcher");
      if (yyjson_is_bool(launcher) && yyjson_get_bool(launcher)) continue;

      DetectableExecutable executable;
      std::string raw = yyjson_get_str(ex_name);
      if (!raw.empty() && raw[0] == '>') {
        executable.basename_only = true;
        raw.erase(0, 1);
      }
      executable.name = net::to_lower(raw);

      yyjson_val *arguments = yyjson_obj_get(ex, "arguments");
      if (yyjson_is_str(arguments)) executable.arguments = yyjson_get_str(arguments);

      app.executables.push_back(std::move(executable));
    }

    if (!app.executables.empty()) out->push_back(std::move(app));
  }
}

bool parse_root(yyjson_doc *doc, std::vector<DetectableApp> *out, std::string *err) {
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_arr(root)) {
    if (err) *err = "detectable DB root must be a JSON array";
    return false;
  }
  out->clear();
  parse_entries(root, out);
  return true;
}

}  // namespace

bool DetectableDb::load_builtin() {
  yyjson_doc *doc = yyjson_read(kDetectableDarwinJson, std::strlen(kDetectableDarwinJson), 0);
  if (!doc) return false;
  bool ok = parse_root(doc, &apps_, nullptr);
  yyjson_doc_free(doc);
  return ok;
}

bool DetectableDb::load_file(const std::string &path, std::string *err) {
  yyjson_read_err read_err;
  std::memset(&read_err, 0, sizeof(read_err));
  yyjson_doc *doc = yyjson_read_file(path.c_str(), 0, nullptr, &read_err);
  if (!doc) {
    if (err) {
      *err = std::string("cannot parse ") + path + ": " + (read_err.msg ? read_err.msg : "invalid JSON");
    }
    return false;
  }
  bool ok = parse_root(doc, &apps_, err);
  yyjson_doc_free(doc);
  return ok;
}

std::vector<DetectableMatch> DetectableDb::match_path(const std::string &path_lower) const {
  std::vector<DetectableMatch> out;
  if (path_lower.empty()) return out;

  // arRPC compares the executable name against every suffix of the process
  // path, plus variants with 64-bit markers stripped.
  std::vector<std::string> segments;
  {
    size_t start = 0;
    while (start <= path_lower.size()) {
      size_t slash = path_lower.find('/', start);
      if (slash == std::string::npos) slash = path_lower.size();
      segments.push_back(path_lower.substr(start, slash - start));
      if (slash == path_lower.size()) break;
      start = slash + 1;
    }
  }

  std::vector<std::string> candidates;
  candidates.reserve(segments.size() * 5);
  for (size_t i = 1; i <= segments.size(); i++) {
    std::string suffix;
    for (size_t j = segments.size() - i; j < segments.size(); j++) {
      if (!suffix.empty()) suffix.push_back('/');
      suffix += segments[j];
    }
    if (suffix.empty()) continue;
    candidates.push_back(suffix);
    candidates.push_back(remove_first(suffix, "64"));
    candidates.push_back(remove_first(suffix, ".x64"));
    candidates.push_back(remove_first(suffix, "x64"));
    candidates.push_back(remove_first(suffix, "_64"));
  }

  const std::string basename = segments.empty() ? std::string() : segments.back();

  for (const auto &app : apps_) {
    for (const auto &exe : app.executables) {
      bool matched = false;
      if (exe.basename_only) {
        matched = basename == exe.name;
      } else {
        for (const auto &candidate : candidates) {
          if (candidate == exe.name) {
            matched = true;
            break;
          }
        }
        if (!matched) matched = app_bundle_match(path_lower, exe.name);
      }
      if (matched) out.push_back(DetectableMatch{&app, &exe});
    }
  }

  return out;
}
