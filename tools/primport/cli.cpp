// rs-primport: the fighter import from the command line.
//   rs-primport scan <folder>
//   rs-primport import <folder> <real steel game dir> <out dir>
#include <cstdio>
#include <string>

#include "primport.h"

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "scan") {
    const auto found = rs::primport::Scan(argv[2]);
    if (found.game.empty()) {
      std::printf("no supported game found\n");
      return 1;
    }
    std::printf("%s:", found.game.c_str());
    for (const auto& f : found.fighters) std::printf(" [%s]", f.c_str());
    std::printf("\n");
    return 0;
  }
  if (argc >= 5 && std::string(argv[1]) == "import") {
    std::string error;
    const bool ok = rs::primport::Import(
        argv[2], argv[3], argv[4], [](const std::string& s) { std::printf("%s\n", s.c_str()); },
        [](double f, const std::string& what) { std::printf("[%3d%%] %s\n", int(f * 100), what.c_str()); }, &error);
    if (!ok) std::fprintf(stderr, "import failed: %s\n", error.c_str());
    return ok ? 0 : 1;
  }
  std::fprintf(stderr, "usage: rs-primport scan <folder> | import <folder> <real steel game dir> <out dir>\n");
  return 2;
}
