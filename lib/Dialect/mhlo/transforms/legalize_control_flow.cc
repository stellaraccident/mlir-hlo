/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for lowering MHLO dialect to Standard dialect.

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Casting.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/transforms/PassDetail.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // TF:llvm-project
#include "mlir/IR/Block.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Support/LogicalResult.h"

namespace mlir {
namespace mhlo {
namespace {
struct LegalizeControlFlowPass
    : public LegalizeControlFlowPassBase<LegalizeControlFlowPass> {
  // Perform the lowering to MLIR control flow.
  void runOnFunction() override;
};

// Replaces terminators for the newly created blocks from a targe region.
// These terminators are replaced with branch operations to a target block.
LogicalResult ReplaceTerminators(Region* region, Block* target_block,
                                 Location loc,
                                 const BlockAndValueMapping& mapper,
                                 OpBuilder* builder) {
  for (auto& old_block : region->getBlocks()) {
    Block* block = mapper.lookup(&old_block);
    auto return_op = dyn_cast<mhlo::ReturnOp>(block->getTerminator());
    if (!return_op) continue;
    builder->setInsertionPointToEnd(block);
    builder->create<mlir::BranchOp>(loc, target_block, return_op.getOperands());
    return_op.erase();
  }

  return success();
}

LogicalResult LowerIfOp(mlir::mhlo::IfOp if_op) {
  Operation* op_inst = if_op.getOperation();
  mlir::OpBuilder builder(if_op);
  auto orig_block = op_inst->getBlock();
  auto* tail_block = orig_block->splitBlock(op_inst);
  auto loc = if_op.getLoc();

  // Duplicate the true and false regions in the block between the sections
  // before and after the conditional.
  BlockAndValueMapping mapper;
  if_op.true_branch().cloneInto(orig_block->getParent(),
                                Region::iterator(tail_block), mapper);
  if_op.false_branch().cloneInto(orig_block->getParent(),
                                 Region::iterator(tail_block), mapper);

  // Determine the blocks for the start of the true and false regions.
  Block* true_block = mapper.lookup(&if_op.true_branch().front());
  Block* false_block = mapper.lookup(&if_op.false_branch().front());

  // Perform the conditional branch into the true/false cases.
  builder.setInsertionPointToEnd(orig_block);

  // Extract the predicate for checking branching, then branch to the true and
  // false regions appropriately.
  auto cond_value = builder.create<mlir::tensor::ExtractOp>(loc, if_op.pred());
  builder.create<mlir::CondBranchOp>(loc, cond_value, true_block,
                                     if_op.true_arg(), false_block,
                                     if_op.false_arg());

  // Replace the true case's return operations with a branch to the tail of
  // the condition.
  if (failed(ReplaceTerminators(&if_op.true_branch(), tail_block, loc, mapper,
                                &builder)))
    return failure();
  if (failed(ReplaceTerminators(&if_op.false_branch(), tail_block, loc, mapper,
                                &builder)))
    return failure();

  tail_block->addArguments(if_op.getResult().getType());
  if_op.getResult().replaceAllUsesWith(tail_block->getArgument(0));

  op_inst->erase();
  return success();
}

LogicalResult LowerWhileOp(mlir::mhlo::WhileOp while_op) {
  // TODO(jpienaar): Support multi-operand while op.
  if (while_op.arg().size() != 1) return failure();

  // Converts a MHLO while loop into control flow. This generates a set of MLIR
  // blocks and branches, along with inlining the regions provided by the MHLO
  // while loop. The structure should be similar to below:
  //
  //   <prior operations>
  //   %0 = "mhlo.while"(%arg0) {^cond(...){...}, ^body(...){...}}
  //   <post operations>
  auto* op_inst = while_op.getOperation();
  mlir::OpBuilder builder(while_op);
  auto loc = while_op.getLoc();

  // Break the block into four sections:
  // orig_block - operations before the while and the branch into looping check.
  // tail_block - operations after the while loop completes.
  // cond_block - check the looping condition, then conditionally branch into
  //              the loop or, if condition is false, jump to the tail branch.
  // body_block - inlined loop body, then jump back to the condition block.
  auto* orig_block = op_inst->getBlock();
  auto* tail_block = orig_block->splitBlock(op_inst);

  BlockAndValueMapping mapper;
  while_op.cond().cloneInto(orig_block->getParent(),
                            Region::iterator(tail_block), mapper);
  while_op.body().cloneInto(orig_block->getParent(),
                            Region::iterator(tail_block), mapper);

  // Lookup the entry blocks for both condition and body.
  auto* cond_block = mapper.lookup(&while_op.cond().front());
  auto* body_block = mapper.lookup(&while_op.body().front());

  // Setup the end of the original block:
  //     <prior operations>
  //     br ^cond(%arg0) // Jumps to the condition statement.
  builder.setInsertionPointToEnd(orig_block);
  // TODO(jpienaar): Support multi-operand while op.
  builder.create<mlir::BranchOp>(loc, cond_block, while_op.arg()[0]);

  // Updates the inlined condition blocks by replacing the return op with an
  // tensor.extract and conditional branch. This changes the block below:
  //   ^cond(%0):
  //     <inlined conditional region>
  //    "mhlo".return(%1)
  //
  //  Into:
  //   ^cond(%0):
  //     <inlined conditional region>
  //     %2 = tensor.extract %1[] : tensor<i1> // Extract the condition value.
  //     cond_br %2, ^body(%0), ^tail(%0) // Branch.
  builder.setInsertionPointToStart(cond_block);

  // Replace the mhlo::ReturnOp with a branch back to the condition block.
  // This is required as the mhlo::ReturnOp is used to mark the end of a
  // block for regions nested inside of a operations (MLIR ReturnOp cannot be
  // nested within an non-function region).
  for (auto& block : while_op.cond()) {
    auto new_block = mapper.lookup(&block);

    auto return_op = dyn_cast<mhlo::ReturnOp>(new_block->getTerminator());
    if (!return_op) continue;
    builder.setInsertionPointToEnd(new_block);

    auto return_value = return_op.getOperand(0);
    auto cond_value =
        builder.create<mlir::tensor::ExtractOp>(loc, return_value);

    // Get the body block arguments.
    llvm::SmallVector<Value, 4> successor_args(cond_block->args_begin(),
                                               cond_block->args_end());
    builder.create<mlir::CondBranchOp>(loc, cond_value, body_block,
                                       successor_args, tail_block,
                                       successor_args);
    return_op.erase();
  }

  // Updates the body blocks by replace the return op with an branch to the
  // conditional block. This changes the block below:
  //   ^body(%0):
  //     <inlined body block>
  //    "mhlo".return(%1)
  //
  //  Into:
  //   ^body(%0):
  //     <inlined body block>
  //     br ^cond(%0) // Branch.
  for (auto& block : while_op.body()) {
    auto new_block = mapper.lookup(&block);
    auto return_op = dyn_cast<mlir::mhlo::ReturnOp>(new_block->getTerminator());
    if (!return_op) continue;
    builder.setInsertionPointToEnd(new_block);
    builder.create<mlir::BranchOp>(loc, cond_block, return_op.getOperands());
    return_op.erase();
  }

  // Erase the original while loop.
  // TODO(jpienaar): Support multi-operand while op.
  tail_block->addArgument(while_op.arg().getType()[0]);
  while_op.getResult(0).replaceAllUsesWith(tail_block->getArgument(0));
  op_inst->erase();

  return success();
}

// Lowers to a cascaded conditional with structure:
//   ^entry_block:
//     %target_index = ...
//   ^cond_0:
//     ...
//     cond_br %is_target, ^case0, ^cond_1
//   ^cond_n_minus_1:
//     ...
//     cond_br %is_target, ^case_n_minus_1, ^case_n
//   ^case_0:
//     ...
//     br ^tail_block(...)
//   ^case_n:
//     ...
//     br ^tail_block(...)
//   ^tail_block(%result : ...):
//
// In the HLO version, each case body block receives an argument, but these are
// trivially resolved via the original dominating values in the CFG form, so
// they are just mapped appropriately prior to inlining.
LogicalResult LowerCaseOp(mlir::mhlo::CaseOp case_op) {
  Operation *op_inst = case_op.getOperation();
  mlir::OpBuilder builder(case_op);
  Block *orig_block = op_inst->getBlock();
  Block *tail_block = orig_block->splitBlock(op_inst);
  Location loc = case_op.getLoc();
  BlockAndValueMapping mapper;

  // The tail block has phi arguments for each result.
  TypeRange result_types = case_op.getResultTypes();
  tail_block->addArguments(result_types);
  for (auto it : llvm::zip(op_inst->getResults(), tail_block->getArguments())) {
    Value orig_result = std::get<0>(it);
    Value new_value = std::get<1>(it);
    orig_result.replaceAllUsesWith(new_value);
  }

  // Create a block for each branch condition check (using the entry block for
  // the first). Pre-create so that we can populate them as we inline the
  // branches. There is one fewer cond blocks than branches because the final
  // one can branch directly to its target in the preceding.
  int branch_count = case_op.branches().size();
  int cond_count = branch_count - 1;
  SmallVector<Block *> cond_blocks(cond_count);
  cond_blocks[0] = orig_block;
  for (int i = 1; i < cond_count - 1; ++i) {
    Block *cond_block = new Block();
    cond_block->insertBefore(tail_block);
    cond_blocks[i] = cond_block;
  }

  // Extract the branch number.
  builder.setInsertionPointToEnd(orig_block);
  auto selected_branch_index = builder.create<mlir::tensor::ExtractOp>(loc, case_op.index());

  // Clone each branch into the parent, remapping the branch operand as we
  // go.
  for (auto it : llvm::zip(case_op.branches(), case_op.branch_operands())) {
    Region &branch_region = std::get<0>(it);
    Value incoming_branch_operand = std::get<1>(it);
    Block *branch_block = &branch_region.front();

    // Clone the branch body.
    mapper.map(branch_block->getArgument(0), incoming_branch_operand);
    branch_region.cloneInto(orig_block->getParent(), Region::iterator(tail_block), mapper);
    if (failed(ReplaceTerminators(&branch_region, tail_block, loc, mapper, &builder))) {
      return failure();
    }
  }

  // Populate condition blocks.
  for (int i = 0; i < cond_count; ++i) {
    Block *cond_block = cond_blocks[i];

    // Emit the condition check.
    builder.setInsertionPointToEnd(cond_block);
    Value current_branch_index = builder.create<mlir::arith::ConstantOp>(loc, builder.getI32IntegerAttr(i));
    Value pred = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq, selected_branch_index, current_branch_index);

    bool is_last = i == (cond_count - 1);
    Block *true_block = mapper.lookup(&case_op.branches()[i].front());
    Block *false_block = !is_last ?
      cond_blocks[i + 1] :
      mapper.lookup(&case_op.branches()[i + 1].front());
    builder.create<CondBranchOp>(loc, pred, true_block, ValueRange{}, false_block, ValueRange{});
  }

  return success();
}

void LegalizeControlFlowPass::runOnFunction() {
  auto func = getFunction();
  llvm::SmallVector<IfOp, 4> if_ops;
  func.walk([&](IfOp op) { if_ops.push_back(op); });
  for (auto& op : if_ops) {
    if (failed(LowerIfOp(op))) return signalPassFailure();
  }

  llvm::SmallVector<WhileOp, 4> while_ops;
  func.walk([&](WhileOp op) { while_ops.push_back(op); });
  for (auto& op : while_ops) {
    if (failed(LowerWhileOp(op))) return signalPassFailure();
  }

  llvm::SmallVector<CaseOp> case_ops;
  func.walk([&](CaseOp op) { case_ops.push_back(op); });
  for (auto &op : case_ops) {
    if (failed(LowerCaseOp(op))) return signalPassFailure();
  }
}
}  // namespace
}  // namespace mhlo
}  // namespace mlir

std::unique_ptr<mlir::OperationPass<mlir::FuncOp>>
mlir::mhlo::createLegalizeControlFlowPass() {
  return std::make_unique<LegalizeControlFlowPass>();
}
