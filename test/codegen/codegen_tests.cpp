#include "toyc/codegen.h"

#include "toyc/ast.h"
#include "toyc/diagnostics.h"
#include "toyc/ir.h"
#include "toyc/irgen.h"
#include "toyc/lexer.h"
#include "toyc/parser.h"
#include "toyc/sema.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

namespace toyc {
namespace {

Module make_return_const_module(int value) {
    Module module;
    Function* main = module.create_function("main", FuncRet::Int, 0);
    BasicBlock* entry = main->create_block();
    entry->push_back(std::make_unique<RetInst>(module.get_constant(value)));
    return module;
}

std::string compile_source_to_asm(const std::string& source) {
    DiagnosticEngine diagnostics;
    std::istringstream input(source);
    Lexer lexer(input, diagnostics);
    Parser parser(lexer, diagnostics);
    std::unique_ptr<CompUnit> unit = parser.parse_comp_unit();
    if (diagnostics.has_errors() || parser.has_error() || !unit) {
        std::ostringstream d;
        diagnostics.emit_all(d);
        ADD_FAILURE() << "parse failed; diagnostics:\n" << d.str();
        return {};
    }

    if (!validate_comp_unit(*unit, diagnostics)) {
        std::ostringstream d;
        diagnostics.emit_all(d);
        ADD_FAILURE() << "validate_comp_unit failed; diagnostics:\n" << d.str();
        return {};
    }

    SemaResult sema = analyze(*unit, diagnostics);
    if (diagnostics.has_errors() || !sema.ok) {
        std::ostringstream d;
        diagnostics.emit_all(d);
        ADD_FAILURE() << "analyze failed; diagnostics:\n" << d.str();
        return {};
    }

    std::unique_ptr<Module> module = generate(*unit, sema, diagnostics);
    if (diagnostics.has_errors() || !module) {
        std::ostringstream d;
        diagnostics.emit_all(d);
        ADD_FAILURE() << "IRGen failed; diagnostics:\n" << d.str();
        return {};
    }

    std::ostringstream out;
    CodegenOptions options;
    if (!emit_riscv(*module, options, diagnostics, out)) {
        std::ostringstream d;
        diagnostics.emit_all(d);
        ADD_FAILURE() << "codegen failed; diagnostics:\n" << d.str();
        return {};
    }
    return out.str();
}

TEST(Codegen, EmitsReturnConstMain) {
    Module module = make_return_const_module(42);
    DiagnosticEngine diagnostics;
    std::ostringstream out;

    CodegenOptions options;
    EXPECT_TRUE(emit_riscv(module, options, diagnostics, out));
    EXPECT_FALSE(diagnostics.has_errors());

    const std::string asm_text = out.str();
    EXPECT_NE(std::string::npos, asm_text.find("    .section .text\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .globl main\n"));
    EXPECT_NE(std::string::npos, asm_text.find("main:\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    li a0, 42\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    ret\n"));
}

TEST(Codegen, CompilesSourceReturnConstMain) {
    const std::string asm_text = compile_source_to_asm("int main() { return 42; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    .section .text\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .globl main\n"));
    EXPECT_NE(std::string::npos, asm_text.find("main:\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    li a0, 42\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    ret\n"));
}

TEST(Codegen, RejectsUnsupportedMainBody) {
    Module module;
    Function* main = module.create_function("main", FuncRet::Int, 0);
    BasicBlock* entry = main->create_block();
    Value* sum = module.create_register(Type::I32);
    entry->push_back(std::make_unique<RetInst>(sum));

    DiagnosticEngine diagnostics;
    std::ostringstream out;
    CodegenOptions options;

    EXPECT_FALSE(emit_riscv(module, options, diagnostics, out));
    EXPECT_TRUE(diagnostics.has_errors());
    ASSERT_EQ(1u, diagnostics.diagnostics().size());
    EXPECT_EQ(DiagnosticStage::Codegen, diagnostics.diagnostics().front().stage);
    EXPECT_TRUE(out.str().empty());
}

}  // namespace
}  // namespace toyc
