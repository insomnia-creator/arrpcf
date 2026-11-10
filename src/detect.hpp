#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct DetectableExecutable {
  std::string name;       // lowercased, may contain '/' (e.g. "osx64/dota2.app")
  std::string arguments;  // required substring of the command line, empty when unused
  bool is_launcher = false;
  bool basename_only = false;  // Discord stores these with a leading '>'
};

struct DetectableApp {
  std::string id;
  std::string name;
  std::vector<DetectableExecutable> executables;
};

struct DetectableMatch {
  const DetectableApp *app = nullptr;
  const DetectableExecutable *exe = nullptr;
};

// Discord's "detectable" game list, restricted to darwin executables for the
// builtin copy (the full upstream database is 11 MB; ours is ~7 KB).
class DetectableDb {
 public:
  bool load_builtin();
  bool load_file(const std::string &path, std::string *err);

  // Every entry whose executable matches `path_lower`; the caller still has to
  // check `exe->arguments` against the process command line.
  std::vector<DetectableMatch> match_path(const std::string &path_lower) const;

  size_t size() const { return apps_.size(); }

 private:
  bool load_memory(const char *data, size_t len, std::string *err);

  std::vector<DetectableApp> apps_;
};
