#include "qeeg/utils.hpp"

#include "test_support.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

int main() {
  using namespace qeeg;

  const std::string csv_path = "tmp_csv_to_tsv.csv";
  const std::string tsv_path = "tmp_csv_to_tsv.tsv";

  // CSV covers:
  // - quoted cell containing a comma
  // - escaped quote ("")
  // - a cell containing a literal tab (should be replaced with a space)
  {
    std::ofstream o(csv_path, std::ios::binary);
    o << "col1,col2,col3\n";
    o << "1,2,3\n";
    o << "\"a,b\",4,\"5\"\"6\"\n";
    o << "\"x\ty\",8,9\n";
  }

  convert_csv_file_to_tsv(csv_path, tsv_path);

  const std::string got = slurp(tsv_path);
  const std::string expect =
      "col1\tcol2\tcol3\n"
      "1\t2\t3\n"
      "a,b\t4\t5\"6\n"
      "x y\t8\t9\n";

  if (got != expect) {
    std::cerr << "CSV->TSV conversion mismatch.\n";
    std::cerr << "Expected:\n" << expect << "\n";
    std::cerr << "Got:\n" << got << "\n";
  }
  assert(got == expect);

  std::remove(csv_path.c_str());
  std::remove(tsv_path.c_str());

  std::cout << "test_csv_to_tsv passed\n";
  return 0;
}
