#include "toyc/ir.h"
#include "toyc/ir_builder.h"
#include "toyc/ir_printer.h"

#include <gtest/gtest.h>
#include <sstream>

using namespace toyc;

namespace {

TEST(IR, UseListWiring) {
    Module m;
    Value* a = m.create_register(Type::I32);
    Value* b = m.create_register(Type::I32);
    auto u = std::make_unique<StoreInst>(a, b);  // 2 operands: a(ptr), b(val)
    User* user = u.get();

    EXPECT_EQ(2, user->num_operands());
    EXPECT_EQ(a, user->operand(0));
    EXPECT_EQ(b, user->operand(1));
    EXPECT_EQ(1, a->uses().size());
    EXPECT_EQ(1, b->uses().size());

    user->set_operand(0, b);
    EXPECT_TRUE(a->uses().empty());
    EXPECT_EQ(2, b->uses().size());

    b->replace_all_uses_with(a);
    EXPECT_EQ(a, user->operand(0));
    EXPECT_EQ(a, user->operand(1));
    EXPECT_TRUE(b->uses().empty());
}

TEST(IR, ConstantPoolAndGlobals) {
    Module m;
    ConstantInt* c1 = m.get_constant(42);
    ConstantInt* c2 = m.get_constant(42);
    ConstantInt* c3 = m.get_constant(7);

    EXPECT_EQ(c1, c2);
    EXPECT_NE(c1, c3);
    EXPECT_EQ(42, c1->value());
    EXPECT_EQ("42", c1->name());

    GlobalVar* g = m.create_global("g_count", 0, /*is_const=*/false);
    EXPECT_EQ("@g_count", g->addr->name());
    EXPECT_EQ(Type::Ptr, g->addr->type());
    EXPECT_EQ(0, g->init->value());
    EXPECT_FALSE(g->is_const);

    Value* r = m.create_register(Type::I32);
    EXPECT_EQ("%v.0", r->name());
    EXPECT_EQ(Type::I32, r->type());
}

TEST(IR, InstructionConstruction) {
    Module m;
    Value* a = m.create_register(Type::I32);
    Value* b = m.create_register(Type::I32);

    auto add = std::make_unique<BinaryInst>(Opcode::Add, a, b, m.fresh_id());
    EXPECT_EQ(Opcode::Add, add->opcode());
    EXPECT_EQ(Type::I32, add->type());
    EXPECT_TRUE(add->has_result());
    EXPECT_EQ(a, add->operand(0));
    EXPECT_EQ(b, add->operand(1));
    EXPECT_EQ(2, add->num_operands());
    EXPECT_FALSE(add->is_terminator());
    EXPECT_EQ(1, a->uses().size());

    auto slt = std::make_unique<ICmpInst>(Opcode::ICmpSlt, a, b, m.fresh_id());
    EXPECT_EQ(Opcode::ICmpSlt, slt->opcode());
    EXPECT_EQ(Type::I32, slt->type());

    ConstantInt* one = m.get_constant(1);
    auto ret = std::make_unique<RetInst>(one);
    EXPECT_TRUE(ret->is_terminator());
    EXPECT_FALSE(ret->has_result());
    EXPECT_EQ(one, ret->operand(0));

    auto ret_void = std::make_unique<RetInst>(/*value=*/nullptr);
    EXPECT_EQ(0, ret_void->num_operands());

    auto store = std::make_unique<StoreInst>(a, b);
    EXPECT_EQ(Opcode::Store, store->opcode());
    EXPECT_FALSE(store->has_result());
    EXPECT_EQ(a, store->operand(0));
    EXPECT_EQ(b, store->operand(1));
}

TEST(IR, FunctionAndBlocks) {
    Module m;
    Function* f = m.create_function("add", FuncRet::Int, /*params=*/2);

    EXPECT_EQ("@add", f->name());
    EXPECT_EQ(FuncRet::Int, f->ret_type());
    EXPECT_EQ(2, f->params().size());
    EXPECT_EQ("%arg.0", f->param(0)->name());
    EXPECT_EQ("%arg.1", f->param(1)->name());

    BasicBlock* entry = f->create_block();
    BasicBlock* bb1 = f->create_block();
    EXPECT_EQ("entry", entry->name());
    EXPECT_EQ("bb1", bb1->name());
    EXPECT_EQ(entry, f->entry());

    Value* a = f->param(0);
    Value* one = m.get_constant(1);
    auto add = std::make_unique<BinaryInst>(Opcode::Add, a, one, m.fresh_id());
    auto ret = std::make_unique<RetInst>(nullptr);
    Instruction* add_raw = add.get();
    entry->push_back(std::move(add));
    entry->push_back(std::move(ret));
    EXPECT_EQ(entry, add_raw->parent());
    ASSERT_NE(nullptr, entry->terminator());
    EXPECT_EQ(Opcode::Ret, entry->terminator()->opcode());
    EXPECT_EQ("%v.0", add_raw->name());
}

TEST(IR, PrinterBasic) {
    Module m;
    m.create_global("g_count", 0, /*is_const=*/false);
    Function* f = m.create_function("f", FuncRet::Int, /*params=*/1);
    BasicBlock* entry = f->create_block();
    Value* a = f->param(0);

    auto alloca = std::make_unique<AllocaInst>(m.fresh_id());
    Value* slot = alloca.get();
    auto store = std::make_unique<StoreInst>(slot, a);
    auto load = std::make_unique<LoadInst>(slot, m.fresh_id());
    Value* loaded = load.get();
    ConstantInt* one = m.get_constant(1);
    auto add = std::make_unique<BinaryInst>(Opcode::Add, loaded, one, m.fresh_id());
    Value* addv = add.get();
    auto ret = std::make_unique<RetInst>(addv);
    entry->push_back(std::move(alloca));
    entry->push_back(std::move(store));
    entry->push_back(std::move(load));
    entry->push_back(std::move(add));
    entry->push_back(std::move(ret));

    std::ostringstream out;
    print_module(m, out);

    std::string expected =
        "@g_count = global i32 0\n"
        "\n"
        "define i32 @f(i32 %arg.0) {\n"
        "entry:\n"
        "  %v.0 = alloca i32\n"
        "  store %v.0, %arg.0\n"
        "  %v.1 = load %v.0\n"
        "  %v.2 = add %v.1, 1\n"
        "  ret %v.2\n"
        "}\n";
    EXPECT_EQ(expected, out.str());
}

TEST(IR, BuilderMatchesPrinter) {
    Module m;
    Function* f = m.create_function("f", FuncRet::Int, 1);
    BasicBlock* entry = f->create_block();
    IRBuilder b(m, entry);

    Value* slot = b.create_alloca();
    b.create_store(slot, f->param(0));
    Value* loaded = b.create_load(slot);
    Value* sum = b.create_binary(Opcode::Add, loaded, m.get_constant(1));
    b.create_ret(sum);

    std::ostringstream out;
    print_module(m, out);

    std::string expected =
        "define i32 @f(i32 %arg.0) {\n"
        "entry:\n"
        "  %v.0 = alloca i32\n"
        "  store %v.0, %arg.0\n"
        "  %v.1 = load %v.0\n"
        "  %v.2 = add %v.1, 1\n"
        "  ret %v.2\n"
        "}\n";
    EXPECT_EQ(expected, out.str());
}

}  // namespace
