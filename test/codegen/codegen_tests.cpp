#include "toyc/codegen.h"

#include "toyc/ast.h"
#include "toyc/diagnostics.h"
#include "toyc/ir.h"
#include "toyc/irgen.h"
#include "toyc/lexer.h"
#include "toyc/parser.h"
#include "toyc/riscv.h"
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
    EXPECT_NE(std::string::npos, asm_text.find("    addi a0, x0, 42\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi a7, x0, 93\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    ecall\n"));
}

TEST(Codegen, CompilesSourceReturnConstMain) {
    const std::string asm_text = compile_source_to_asm("int main() { return 42; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    .section .text\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .globl main\n"));
    EXPECT_NE(std::string::npos, asm_text.find("main:\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi a0, x0, 42\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi a7, x0, 93\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    ecall\n"));
}

TEST(Codegen, CompilesLocalVarReturn) {
    const std::string asm_text = compile_source_to_asm("int main() { int x = 7; return x; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    addi sp, sp, -16\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi t1, x0, 7\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    sw t1, 0(t0)\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    lw t1, 0(t0)\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi sp, sp, 16\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    ecall\n"));
}

TEST(Codegen, CompilesLocalArithmetic) {
    const std::string asm_text =
        compile_source_to_asm("int main() { int x = -5; int y = 3000; return x + y * 2; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    addi t1, x0, -5\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    lui t1, 1\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi t1, t1, -1096\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    mul t2, t0, t1\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    add t2, t0, t1\n"));
}

TEST(Codegen, EmitsGlobalsAndCompilesGlobalReadWrite) {
    const std::string asm_text =
        compile_source_to_asm("int g = 3; const int c = 4; int main() { g = g + c; return g; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    .section .rodata\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .globl c\n"));
    EXPECT_NE(std::string::npos, asm_text.find("c:\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .word 4\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .section .data\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .globl g\n"));
    EXPECT_NE(std::string::npos, asm_text.find("g:\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    .word 3\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    lui t0, %hi(g)\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi t0, t0, %lo(g)\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    lw t1, 0(t0)\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    sw t1, 0(t0)\n"));
}

TEST(Codegen, CompilesLargeReturnConstantWithoutPseudo) {
    const std::string asm_text = compile_source_to_asm("int main() { return 1048577; }\n");
    EXPECT_NE(std::string::npos, asm_text.find("    lui a0, 256\n"));
    EXPECT_NE(std::string::npos, asm_text.find("    addi a0, a0, 1\n"));
    EXPECT_EQ(std::string::npos, asm_text.find("    li "));
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

TEST(Codegen, RejectsControlFlowBeforeP3WithoutPartialOutput) {
    DiagnosticEngine diagnostics;
    std::istringstream input("int main() { if (1) { return 1; } return 0; }\n");
    Lexer lexer(input, diagnostics);
    Parser parser(lexer, diagnostics);
    std::unique_ptr<CompUnit> unit = parser.parse_comp_unit();
    ASSERT_NE(nullptr, unit);
    ASSERT_TRUE(validate_comp_unit(*unit, diagnostics));
    SemaResult sema = analyze(*unit, diagnostics);
    ASSERT_TRUE(sema.ok);
    std::unique_ptr<Module> module = generate(*unit, sema, diagnostics);
    ASSERT_NE(nullptr, module);

    std::ostringstream out;
    CodegenOptions options;
    EXPECT_FALSE(emit_riscv(*module, options, diagnostics, out));
    EXPECT_TRUE(diagnostics.has_errors());
    EXPECT_TRUE(out.str().empty());
}

TEST(Riscv, RegisterNames) {
    EXPECT_STREQ("x0", reg_name(RvReg::Zero));
    EXPECT_STREQ("ra", reg_name(RvReg::Ra));
    EXPECT_STREQ("sp", reg_name(RvReg::Sp));
    EXPECT_STREQ("t0", reg_name(RvReg::T0));
    EXPECT_STREQ("s0", reg_name(RvReg::S0));
    EXPECT_STREQ("a0", reg_name(RvReg::A0));
    EXPECT_STREQ("a7", reg_name(RvReg::A7));
    EXPECT_STREQ("s11", reg_name(RvReg::S11));
    EXPECT_STREQ("t6", reg_name(RvReg::T6));
}

TEST(Riscv, ImmediateAndAlignmentHelpers) {
    EXPECT_TRUE(fits_i12(-2048));
    EXPECT_TRUE(fits_i12(2047));
    EXPECT_FALSE(fits_i12(-2049));
    EXPECT_FALSE(fits_i12(2048));

    EXPECT_EQ(0, align_to(0, 16));
    EXPECT_EQ(16, align_to(1, 16));
    EXPECT_EQ(16, align_to(16, 16));
    EXPECT_EQ(32, align_to(17, 16));
}

TEST(Riscv, LabelHelpers) {
    Module module;
    GlobalVar* global = module.create_global("g", 7, false);
    Function* function = module.create_function("main", FuncRet::Int, 0);
    BasicBlock* entry = function->create_block();
    BasicBlock* next = function->create_block();

    EXPECT_EQ("g", global_label(*global->addr));
    EXPECT_EQ("main", function_label(*function));
    EXPECT_EQ(".Lmain_entry", block_label(*function, *entry));
    EXPECT_EQ(".Lmain_bb1", block_label(*function, *next));
}

TEST(Riscv, AsmWriterFormatsText) {
    std::ostringstream out;
    AsmWriter writer(out);

    writer.section(".text");
    writer.global("main");
    writer.label("main");
    writer.inst("addi", "a0", "x0", "42");
    writer.inst("ecall");
    writer.comment("done");

    EXPECT_EQ("    .section .text\n"
              "    .globl main\n"
              "main:\n"
              "    addi a0, x0, 42\n"
              "    ecall\n"
              "    # done\n",
              out.str());
}

}  // namespace
}  // namespace toyc
