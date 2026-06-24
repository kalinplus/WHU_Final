#pragma once

#include "toyc/ir.h"

#include <unordered_map>
#include <vector>

namespace toyc {

class DominatorTree {
public:
    void analyze(Function& fn);

    BasicBlock* idom(BasicBlock* bb) const { return idom_.at(bb); }
    const std::vector<BasicBlock*>& preds(BasicBlock* bb) const { return preds_.at(bb); }
    const std::vector<BasicBlock*>& succs(BasicBlock* bb) const { return succs_.at(bb); }
    const std::vector<BasicBlock*>& dom_frontier(BasicBlock* bb) const { return df_.at(bb); }
    const std::vector<BasicBlock*>& children(BasicBlock* bb) const { return children_.at(bb); }

private:
    void build_cfg(Function& fn);

    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> preds_;
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> succs_;
    std::unordered_map<BasicBlock*, BasicBlock*> idom_;
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> df_;
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> children_;
};

void mem2reg(Function& fn);
void mem2reg(Module& module);

}  // namespace toyc
