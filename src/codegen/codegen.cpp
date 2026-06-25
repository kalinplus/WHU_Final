#include "toyc/codegen.h"

#include "toyc/diagnostics.h"
#include "toyc/ir.h"
#include "toyc/riscv.h"

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

std::string offset_addr(int offset, RvReg base) {
    return std::to_string(offset) + "(" + reg_name(base) + ")";
}

bool is_result_slot_inst(const Instruction& inst) {
    return inst.has_result() && inst.opcode() != Opcode::Alloca;
}

class FunctionFrame {
public:
    explicit FunctionFrame(const Function& function) {
        for (const std::unique_ptr<BasicBlock>& block : function.blocks()) {
            for (const std::unique_ptr<Instruction>& inst : block->insts()) {
                if (inst->opcode() == Opcode::Alloca || is_result_slot_inst(*inst)) {
                    slots_.emplace(inst.get(), next_offset_);
                    next_offset_ += 4;
                }
            }
        }
        frame_size_ = align_to(next_offset_, 16);
    }

    int frame_size() const { return frame_size_; }

    bool has_slot(const Value* value) const {
        return slots_.find(value) != slots_.end();
    }

    int slot_offset(const Value* value) const {
        auto found = slots_.find(value);
        return found == slots_.end() ? -1 : found->second;
    }

private:
    std::unordered_map<const Value*, int> slots_;
    int next_offset_ = 0;
    int frame_size_ = 0;
};

class FunctionLowerer {
public:
    FunctionLowerer(const Function& function, const CodegenOptions& options,
                    DiagnosticEngine& diagnostics, AsmWriter& writer)
        : function_(function), options_(options), diagnostics_(diagnostics),
          writer_(writer), frame_(function) {}

    bool lower() {
        if (!validate_function_shape()) {
            return false;
        }
        emit_prologue();
        for (const std::unique_ptr<BasicBlock>& block : function_.blocks()) {
            if (block.get() != function_.entry()) {
                writer_.label(block_label(function_, *block));
            }
            for (const std::unique_ptr<Instruction>& inst : block->insts()) {
                if (!lower_inst(*inst)) {
                    return false;
                }
            }
        }
        if (!emitted_exit_) {
            emit_epilogue();
        }
        return true;
    }

private:
    bool validate_function_shape() {
        if (function_.params().size() > 8) {
            return fail("codegen does not support more than 8 function parameters yet");
        }
        for (const std::unique_ptr<BasicBlock>& block : function_.blocks()) {
            if (block.get() != function_.entry()) {
                return fail("codegen does not support control flow before P3");
            }
        }
        return true;
    }

    bool lower_inst(const Instruction& inst) {
        switch (inst.opcode()) {
            case Opcode::Alloca:
                return true;
            case Opcode::Load:
                return lower_load(inst);
            case Opcode::Store:
                return lower_store(inst);
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
                return lower_binary(inst);
            case Opcode::Ret:
                return lower_ret(inst);
            case Opcode::Sdiv:
            case Opcode::Srem:
            case Opcode::Neg:
            case Opcode::ICmpEq:
            case Opcode::ICmpNe:
            case Opcode::ICmpSlt:
            case Opcode::ICmpSgt:
            case Opcode::ICmpSle:
            case Opcode::ICmpSge:
            case Opcode::Br:
            case Opcode::CondBr:
            case Opcode::Call:
            case Opcode::Phi:
            case Opcode::Shl:
            case Opcode::Shr:
                return fail("codegen does not support " + std::string(opcode_name(inst.opcode())) +
                            " before later lowering phases");
        }
        return fail("codegen found an unknown instruction");
    }

    bool lower_load(const Instruction& inst) {
        if (inst.num_operands() != 1) {
            return fail("malformed load instruction");
        }
        if (!load_address(inst.operand(0), RvReg::T0)) {
            return false;
        }
        writer_.inst("lw", reg_name(RvReg::T1), offset_addr(0, RvReg::T0));
        return spill_result(inst, RvReg::T1);
    }

    bool lower_store(const Instruction& inst) {
        if (inst.num_operands() != 2) {
            return fail("malformed store instruction");
        }
        if (!load_i32(inst.operand(1), RvReg::T1)) {
            return false;
        }
        if (!store_i32(RvReg::T1, inst.operand(0))) {
            return false;
        }
        return true;
    }

    bool lower_binary(const Instruction& inst) {
        if (inst.num_operands() != 2) {
            return fail("malformed binary instruction");
        }
        if (!load_i32(inst.operand(0), RvReg::T0) ||
            !load_i32(inst.operand(1), RvReg::T1)) {
            return false;
        }
        if (inst.opcode() == Opcode::Add) {
            writer_.inst("add", reg_name(RvReg::T2), reg_name(RvReg::T0), reg_name(RvReg::T1));
        } else if (inst.opcode() == Opcode::Sub) {
            writer_.inst("sub", reg_name(RvReg::T2), reg_name(RvReg::T0), reg_name(RvReg::T1));
        } else {
            writer_.inst("mul", reg_name(RvReg::T2), reg_name(RvReg::T0), reg_name(RvReg::T1));
        }
        return spill_result(inst, RvReg::T2);
    }

    bool lower_ret(const Instruction& inst) {
        if (inst.num_operands() > 1) {
            return fail("malformed ret instruction");
        }
        if (inst.num_operands() == 1 && !load_i32(inst.operand(0), RvReg::A0)) {
            return false;
        }
        emit_epilogue();
        return true;
    }

    void emit_prologue() {
        writer_.global(function_label(function_));
        writer_.label(function_label(function_));
        if (frame_.frame_size() > 0) {
            writer_.inst("addi", reg_name(RvReg::Sp), reg_name(RvReg::Sp),
                         std::to_string(-frame_.frame_size()));
        }
    }

    void emit_epilogue() {
        if (emitted_exit_) {
            return;
        }
        if (frame_.frame_size() > 0) {
            writer_.inst("addi", reg_name(RvReg::Sp), reg_name(RvReg::Sp),
                         std::to_string(frame_.frame_size()));
        }
        if (function_.short_name() == "main" && options_.emit_exit_syscall) {
            materialize_i32(93, RvReg::A7);
            writer_.inst("ecall");
        } else {
            writer_.inst("jalr", reg_name(RvReg::Zero), offset_addr(0, RvReg::Ra));
        }
        emitted_exit_ = true;
    }

    bool load_i32(Value* value, RvReg dst) {
        if (value->value_kind() == ValueKind::Constant) {
            materialize_i32(static_cast<const ConstantInt*>(value)->value(), dst);
            return true;
        }
        if (value->value_kind() == ValueKind::Param) {
            const unsigned id = value->id();
            if (id >= arg_regs_.size()) {
                return fail("codegen does not support stack parameters yet");
            }
            const RvReg source = arg_regs_[id];
            if (source != dst) {
                writer_.inst("add", reg_name(dst), reg_name(source), reg_name(RvReg::Zero));
            }
            return true;
        }
        if (frame_.has_slot(value)) {
            writer_.inst("lw", reg_name(dst), offset_addr(frame_.slot_offset(value), RvReg::Sp));
            return true;
        }
        return fail("codegen cannot materialize value " + value->name());
    }

    bool store_i32(RvReg src, Value* destination_ptr) {
        if (!load_address(destination_ptr, RvReg::T0)) {
            return false;
        }
        writer_.inst("sw", reg_name(src), offset_addr(0, RvReg::T0));
        return true;
    }

    bool load_address(Value* ptr, RvReg dst) {
        if (ptr->value_kind() == ValueKind::GlobalAddr) {
            const std::string label = global_label(*static_cast<const GlobalAddr*>(ptr));
            writer_.inst("lui", reg_name(dst), "%hi(" + label + ")");
            writer_.inst("addi", reg_name(dst), reg_name(dst), "%lo(" + label + ")");
            return true;
        }
        if (frame_.has_slot(ptr)) {
            writer_.inst("addi", reg_name(dst), reg_name(RvReg::Sp),
                         std::to_string(frame_.slot_offset(ptr)));
            return true;
        }
        return fail("codegen cannot materialize address " + ptr->name());
    }

    bool spill_result(const Instruction& inst, RvReg src) {
        if (!frame_.has_slot(&inst)) {
            return fail("codegen has no result slot for " + inst.name());
        }
        writer_.inst("sw", reg_name(src), offset_addr(frame_.slot_offset(&inst), RvReg::Sp));
        return true;
    }

    void materialize_i32(int value, RvReg dst) {
        if (options_.allow_pseudo) {
            writer_.inst("li", reg_name(dst), std::to_string(value));
            return;
        }
        if (fits_i12(value)) {
            writer_.inst("addi", reg_name(dst), reg_name(RvReg::Zero), std::to_string(value));
            return;
        }
        const int64_t rounded = static_cast<int64_t>(value) + 0x800;
        const int64_t hi = rounded >> 12;
        const int64_t lo = static_cast<int64_t>(value) - (hi << 12);
        writer_.inst("lui", reg_name(dst), std::to_string(hi));
        if (lo != 0) {
            writer_.inst("addi", reg_name(dst), reg_name(dst), std::to_string(lo));
        }
    }

    bool fail(const std::string& message) {
        diagnostics_.error(DiagnosticStage::Codegen, SourceLoc{0, 0}, message);
        return false;
    }

    const Function& function_;
    const CodegenOptions& options_;
    DiagnosticEngine& diagnostics_;
    AsmWriter& writer_;
    FunctionFrame frame_;
    bool emitted_exit_ = false;
    const std::vector<RvReg> arg_regs_ = {
        RvReg::A0, RvReg::A1, RvReg::A2, RvReg::A3,
        RvReg::A4, RvReg::A5, RvReg::A6, RvReg::A7,
    };
};

void emit_globals(const Module& module, AsmWriter& writer) {
    bool emitted_rodata = false;
    for (const std::unique_ptr<GlobalVar>& global : module.globals()) {
        if (!global->is_const) {
            continue;
        }
        if (!emitted_rodata) {
            writer.section(".rodata");
            emitted_rodata = true;
        }
        writer.global(global_label(*global->addr));
        writer.label(global_label(*global->addr));
        writer.inst(".word", std::to_string(global->init->value()));
    }

    bool emitted_data = false;
    for (const std::unique_ptr<GlobalVar>& global : module.globals()) {
        if (global->is_const) {
            continue;
        }
        if (!emitted_data) {
            writer.section(".data");
            emitted_data = true;
        }
        writer.global(global_label(*global->addr));
        writer.label(global_label(*global->addr));
        writer.inst(".word", std::to_string(global->init->value()));
    }
}

bool emit_functions(const Module& module, const CodegenOptions& options,
                    DiagnosticEngine& diagnostics, AsmWriter& writer) {
    writer.section(".text");
    for (const std::unique_ptr<Function>& function : module.functions()) {
        FunctionLowerer lowerer(*function, options, diagnostics, writer);
        if (!lowerer.lower()) {
            return false;
        }
    }
    return true;
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

    std::ostringstream buffer;
    AsmWriter writer(buffer);
    emit_globals(module, writer);
    if (!emit_functions(module, options, diagnostics, writer)) {
        return false;
    }
    out << buffer.str();
    return true;
}

}  // namespace toyc
