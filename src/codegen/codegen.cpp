#include "toyc/codegen.h"

#include "toyc/diagnostics.h"
#include "toyc/ir.h"

#include <ostream>

namespace toyc {

namespace {

const Function* find_main(const Module& module) {
    for (const std::unique_ptr<Function>& function : module.functions()) {
        if (function->short_name() == "main") {
            return function.get();
        }
    }
    return nullptr;
}

const ConstantInt* return_constant_from_main(const Function& function) {
    if (function.ret_type() != FuncRet::Int || !function.params().empty()) {
        return nullptr;
    }
    const BasicBlock* entry = function.entry();
    if (!entry || entry->insts().size() != 1) {
        return nullptr;
    }
    const Instruction* inst = entry->insts().front().get();
    if (inst->opcode() != Opcode::Ret || inst->num_operands() != 1) {
        return nullptr;
    }
    Value* value = inst->operand(0);
    if (value->value_kind() != ValueKind::Constant) {
        return nullptr;
    }
    return static_cast<const ConstantInt*>(value);
}

}  // namespace

bool emit_riscv(const Module& module, const CodegenOptions& options,
                DiagnosticEngine& diagnostics, std::ostream& out) {
    const Function* main = find_main(module);
    if (!main) {
        diagnostics.error(DiagnosticStage::Codegen, SourceLoc{0, 0},
                          "codegen requires an int main() function");
        return false;
    }

    // P0 bring-up supports only raw IR for `int main() { return const; }`.
    // Later phases replace this recognizer with full lowering.
    const ConstantInt* result = return_constant_from_main(*main);
    if (!result) {
        diagnostics.error(DiagnosticStage::Codegen, SourceLoc{0, 0},
                          "codegen only supports int main() returning a constant for now");
        return false;
    }

    out << "    .section .text\n";
    out << "    .globl main\n";
    out << "main:\n";
    if (options.allow_pseudo) {
        out << "    li a0, " << result->value() << "\n";
    } else {
        out << "    addi a0, x0, " << result->value() << "\n";
    }
    if (options.emit_exit_syscall) {
        out << "    li a7, 93\n";
        out << "    ecall\n";
    } else {
        out << "    ret\n";
    }
    return true;
}

}  // namespace toyc
