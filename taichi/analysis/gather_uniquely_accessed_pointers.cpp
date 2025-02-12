#include "taichi/analysis/gather_uniquely_accessed_pointers.h"
#include "taichi/ir/ir.h"
#include "taichi/ir/analysis.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/visitors.h"
#include <algorithm>

TLANG_NAMESPACE_BEGIN

class LoopUniqueStmtSearcher : public BasicStmtVisitor {
 private:
  // Constant values that don't change in the loop.
  std::unordered_set<Stmt *> loop_invariant_;

  // If loop_unique_[stmt] is -1, the value of stmt is unique among the
  // top-level loop.
  // If loop_unique_[stmt] is x >= 0, the value of stmt is unique to
  // the x-th loop index.
  std::unordered_map<Stmt *, int> loop_unique_;

 public:
  // The number of loop indices of the top-level loop.
  // -1 means uninitialized.
  int num_different_loop_indices{-1};
  using BasicStmtVisitor::visit;

  LoopUniqueStmtSearcher() {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
  }

  void visit(LoopIndexStmt *stmt) override {
    if (stmt->loop->is<OffloadedStmt>())
      loop_unique_[stmt] = stmt->index;
  }

  void visit(LoopUniqueStmt *stmt) override {
    loop_unique_[stmt] = -1;
  }

  void visit(ConstStmt *stmt) override {
    loop_invariant_.insert(stmt);
  }

  void visit(UnaryOpStmt *stmt) override {
    if (loop_invariant_.count(stmt->operand) > 0) {
      loop_invariant_.insert(stmt);
    }

    // op loop-unique -> loop-unique
    if (loop_unique_.count(stmt->operand) > 0 &&
        (stmt->op_type == UnaryOpType::neg)) {
      // TODO: Other injective unary operations
      loop_unique_[stmt] = loop_unique_[stmt->operand];
    }
  }

  void visit(BinaryOpStmt *stmt) override {
    if (loop_invariant_.count(stmt->lhs) > 0 &&
        loop_invariant_.count(stmt->rhs) > 0) {
      loop_invariant_.insert(stmt);
    }

    // loop-unique op loop-invariant -> loop-unique
    if ((loop_unique_.count(stmt->lhs) > 0 &&
         loop_invariant_.count(stmt->rhs) > 0) &&
        (stmt->op_type == BinaryOpType::add ||
         stmt->op_type == BinaryOpType::sub ||
         stmt->op_type == BinaryOpType::bit_xor)) {
      // TODO: Other operations
      loop_unique_[stmt] = loop_unique_[stmt->lhs];
    }

    // loop-invariant op loop-unique -> loop-unique
    if ((loop_invariant_.count(stmt->lhs) > 0 &&
         loop_unique_.count(stmt->rhs) > 0) &&
        (stmt->op_type == BinaryOpType::add ||
         stmt->op_type == BinaryOpType::sub ||
         stmt->op_type == BinaryOpType::bit_xor)) {
      loop_unique_[stmt] = loop_unique_[stmt->rhs];
    }
  }

  bool is_ptr_indices_loop_unique(GlobalPtrStmt *stmt) const {
    // Check if the address is loop-unique, i.e., stmt contains
    // either a loop-unique index or all top-level loop indices.
    TI_ASSERT(num_different_loop_indices != -1);
    std::vector<int> loop_indices;
    loop_indices.reserve(stmt->indices.size());
    for (auto &index : stmt->indices) {
      auto loop_unique_index = loop_unique_.find(index);
      if (loop_unique_index != loop_unique_.end()) {
        if (loop_unique_index->second == -1) {
          // LoopUniqueStmt
          return true;
        } else {
          // LoopIndexStmt
          loop_indices.push_back(loop_unique_index->second);
        }
      }
    }
    std::sort(loop_indices.begin(), loop_indices.end());
    auto current_num_different_loop_indices =
        std::unique(loop_indices.begin(), loop_indices.end()) -
        loop_indices.begin();
    // for i, j in x:
    //     a[j, i] is loop-unique
    //     b[i, i] is not loop-unique (because there's no j)
    return current_num_different_loop_indices == num_different_loop_indices;
  }
};

class UniquelyAccessedSNodeSearcher : public BasicStmtVisitor {
 private:
  LoopUniqueStmtSearcher loop_unique_stmt_searcher_;

  // Search SNodes that are uniquely accessed, i.e., accessed by
  // one GlobalPtrStmt (or by definitely-same-address GlobalPtrStmts),
  // and that GlobalPtrStmt's address is loop-unique.
  std::unordered_map<const SNode *, GlobalPtrStmt *> accessed_pointer_;

 public:
  using BasicStmtVisitor::visit;

  UniquelyAccessedSNodeSearcher() {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
  }

  void visit(GlobalPtrStmt *stmt) override {
    for (auto &snode : stmt->snodes.data) {
      auto accessed_ptr = accessed_pointer_.find(snode);
      if (accessed_ptr == accessed_pointer_.end()) {
        if (loop_unique_stmt_searcher_.is_ptr_indices_loop_unique(stmt)) {
          accessed_pointer_[snode] = stmt;
        } else {
          accessed_pointer_[snode] = nullptr;  // not loop-unique
        }
      } else {
        if (!irpass::analysis::definitely_same_address(accessed_ptr->second,
                                                       stmt)) {
          accessed_ptr->second = nullptr;  // not uniquely accessed
        }
      }
    }
  }

  static std::unordered_map<const SNode *, GlobalPtrStmt *> run(IRNode *root) {
    TI_ASSERT(root->is<OffloadedStmt>());
    auto offload = root->as<OffloadedStmt>();
    UniquelyAccessedSNodeSearcher searcher;
    if (offload->task_type == OffloadedTaskType::range_for ||
        offload->task_type == OffloadedTaskType::mesh_for) {
      searcher.loop_unique_stmt_searcher_.num_different_loop_indices = 1;
    } else if (offload->task_type == OffloadedTaskType::struct_for) {
      searcher.loop_unique_stmt_searcher_.num_different_loop_indices =
          offload->snode->num_active_indices;
    } else {
      // serial
      searcher.loop_unique_stmt_searcher_.num_different_loop_indices = 0;
    }
    root->accept(&searcher.loop_unique_stmt_searcher_);
    root->accept(&searcher);
    return searcher.accessed_pointer_;
  }
};

class UniquelyAccessedBitStructGatherer : public BasicStmtVisitor {
 private:
  std::unordered_map<OffloadedStmt *,
                     std::unordered_map<const SNode *, GlobalPtrStmt *>>
      result_;

 public:
  using BasicStmtVisitor::visit;

  UniquelyAccessedBitStructGatherer() {
    allow_undefined_visitor = true;
    invoke_default_visitor = false;
  }

  void visit(OffloadedStmt *stmt) override {
    if (stmt->task_type == OffloadedTaskType::range_for ||
        stmt->task_type == OffloadedTaskType::mesh_for ||
        stmt->task_type == OffloadedTaskType::struct_for) {
      auto &loop_unique_bit_struct = result_[stmt];
      auto loop_unique_ptr =
          irpass::analysis::gather_uniquely_accessed_pointers(stmt);
      for (auto &it : loop_unique_ptr) {
        auto *snode = it.first;
        auto *ptr1 = it.second;
        if (snode->is_bit_level) {
          // Find the nearest non-bit-level ancestor
          while (snode->is_bit_level) {
            snode = snode->parent;
          }
          // Check whether uniquely accessed
          auto accessed_ptr = loop_unique_bit_struct.find(snode);
          if (accessed_ptr == loop_unique_bit_struct.end()) {
            loop_unique_bit_struct[snode] = ptr1;
          } else {
            if (ptr1 == nullptr) {
              accessed_ptr->second = nullptr;
              continue;
            }
            auto *ptr2 = accessed_ptr->second;
            TI_ASSERT(ptr1->indices.size() == ptr2->indices.size());
            for (int id = 0; id < (int)ptr1->indices.size(); id++) {
              if (!irpass::analysis::same_value(ptr1->indices[id],
                                                ptr2->indices[id])) {
                accessed_ptr->second = nullptr;  // not uniquely accessed
              }
            }
          }
        }
      }
    }
    // Do not dive into OffloadedStmt
  }

  static std::unordered_map<OffloadedStmt *,
                            std::unordered_map<const SNode *, GlobalPtrStmt *>>
  run(IRNode *root) {
    UniquelyAccessedBitStructGatherer gatherer;
    root->accept(&gatherer);
    return gatherer.result_;
  }
};

const std::string GatherUniquelyAccessedBitStructsPass::id =
    "GatherUniquelyAccessedBitStructsPass";

namespace irpass::analysis {
std::unordered_map<const SNode *, GlobalPtrStmt *>
gather_uniquely_accessed_pointers(IRNode *root) {
  // TODO: What about SNodeOpStmts?
  return UniquelyAccessedSNodeSearcher::run(root);
}

void gather_uniquely_accessed_bit_structs(IRNode *root, AnalysisManager *amgr) {
  amgr->put_pass_result<GatherUniquelyAccessedBitStructsPass>(
      {UniquelyAccessedBitStructGatherer::run(root)});
}
}  // namespace irpass::analysis

TLANG_NAMESPACE_END
