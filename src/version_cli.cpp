#include "qeeg/version.hpp"

#include "qeeg/utils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace qeeg;

namespace {

struct Args {
  bool json{false};
  bool full{false};
};

static void print_help() {
  std::cout
    << "qeeg_version_cli\n\n"
    << "Print the qeeg project version (and optional build/compiler/git info).\n\n"
    << "Usage:\n"
    << "  qeeg_version_cli\n"
    << "  qeeg_version_cli --full\n"
    << "  qeeg_version_cli --json\n\n"
    << "Options:\n"
    << "  --full          Print additional build details\n"
    << "  --json          Output JSON (useful for scripts)\n"
    << "  -h, --help      Show this help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--json") {
      a.json = true;
    } else if (arg == "--full") {
      a.full = true;
    } else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }
  return a;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const Args a = parse_args(argc, argv);

    if (a.json) {
      std::cout
        << "{\"version\":\"" << json_escape(version_string()) << "\""
        << ",\"version_major\":" << version_major()
        << ",\"version_minor\":" << version_minor()
        << ",\"version_patch\":" << version_patch()
        << ",\"git\":\"" << json_escape(git_describe_string()) << "\""
        << ",\"build_type\":\"" << json_escape(build_type_string()) << "\""
        << ",\"compiler\":\"" << json_escape(compiler_string()) << "\""
        << ",\"cpp_standard\":\"" << json_escape(cpp_standard_string()) << "\"}"
        << "\n";
      return 0;
    }

    if (a.full) {
      std::cout
        << "version: " << version_string() << "\n"
        << "version_major: " << version_major() << "\n"
        << "version_minor: " << version_minor() << "\n"
        << "version_patch: " << version_patch() << "\n"
        << "git: " << git_describe_string() << "\n"
        << "build_type: " << build_type_string() << "\n"
        << "compiler: " << compiler_string() << "\n"
        << "cpp_standard: " << cpp_standard_string() << "\n";
      return 0;
    }

    // Default output is just the version string (script-friendly).
    std::cout << version_string() << "\n";
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Run with --help for usage.\n";
    return 1;
  }
}
