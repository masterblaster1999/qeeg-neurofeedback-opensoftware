// Thin wrapper for the neurofeedback CLI.
//
// The heavy implementation (including embedded HTML/JS) lives in nf_cli.cpp as
// qeeg_nf_cli_run(). This wrapper exists so we can:
//   1) build the standalone executable (main)
//   2) build the offline toolbox entrypoint by compiling this file with a
//      preprocessor rename: main -> qeeg_nf_cli_entry
//
// This avoids compiling the large nf_cli.cpp translation unit twice.

int qeeg_nf_cli_run(int argc, char** argv);

int main(int argc, char** argv) {
  return qeeg_nf_cli_run(argc, argv);
}
