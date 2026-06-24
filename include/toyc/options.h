#pragma once

#include "toyc/diagnostics.h"

#include <iosfwd>

namespace toyc {

struct CompilerOptions {
    bool dump_tokens = false;
    bool dump_ast = false;
    bool dump_ir = false;
    bool opt_mode = false;
    bool mem2reg_only = false;  // test-only: run mem2reg without optim passes
};

CompilerOptions parse_options(int argc, char* argv[], DiagnosticEngine& diagnostics);
void print_usage(std::ostream& output);

}  // namespace toyc
