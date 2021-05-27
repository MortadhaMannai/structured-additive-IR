// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transforms/default_lowering_attributes.h"

#include <iterator>
#include <memory>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "sair_attributes.h"
#include "sair_op_interfaces.h"
#include "sair_ops.h"
#include "sequence.h"
#include "storage.h"

namespace sair {
namespace {

// Include passes base class declaration generated by MLIR. This file should not
// be included anywhere else with GEN_PASS_CLASSES set. The #define in front
// selects the part of the file to include (pass base class declaration or pass
// registration). See
// https://mlir.llvm.org/docs/PassManagement/#declarative-pass-specification for
// more information.
#define GEN_PASS_CLASSES
#include "transforms/default_lowering_attributes.h.inc"

// Writes the storage information infered by the storage analysis pass to
// Compute operations.
mlir::LogicalResult CommitStorage(
    ComputeOp op, const IterationSpaceAnalysis &iteration_spaces,
    const StorageAnalysis &storage_analysis) {
  mlir::MLIRContext *context = op.getContext();
  const IterationSpace &iter_space = iteration_spaces.Get(op.getOperation());

  for (int i = 0, e = op->getNumResults(); i < e; ++i) {
    const ValueStorage &storage = storage_analysis.GetStorage(op->getResult(i));

    NamedMappingAttr layout;
    if (storage.layout() != nullptr) {
      llvm::SmallBitVector indexed_loops = storage.layout().DependencyMask();
      auto none = MappingNoneExpr::get(context);
      llvm::SmallVector<MappingExpr> renaming(iter_space.mapping().size(),
                                              none);
      llvm::SmallVector<mlir::StringAttr> loop_names;
      for (int loop : indexed_loops.set_bits()) {
        renaming[loop] = MappingDimExpr::get(loop_names.size(), context);
        loop_names.push_back(iter_space.loop_names()[loop]);
      }

      layout = NamedMappingAttr::get(loop_names, renaming, context)
                   .Compose(storage.layout());
    }
    auto attr = BufferAttr::get(storage.space(), storage.buffer_name(), layout,
                                context);
    op.SetStorage(i, attr);
  }
  return mlir::success();
}

// Indicates if an operand can use the value from registers.
bool FitsInRegisters(const ValueOperand &operand,
                     const IterationSpaceAnalysis &iteration_spaces) {
  SairOp defining_op = operand.value().getDefiningOp();
  MappingAttr mapping = iteration_spaces.TranslateMapping(
      operand.getOwner(), defining_op,
      operand.Mapping().Resize(defining_op.domain().size()));
  int common_loops = iteration_spaces.Get(operand.getOwner())
                         .NumCommonLoops(iteration_spaces.Get(defining_op));
  // Test if the operand is only accessed along common loops.
  return mapping.MinDomainSize() <= common_loops;
}

// Initialize storage for value with default values if needed. Memory space is
// initialized with `register` and layout is initialized with `?`
// expressions.
void InitializeStorage(mlir::Value value,
                       const LoopFusionAnalysis &fusion_analysis,
                       const IterationSpaceAnalysis &iteration_spaces,
                       StorageAnalysis &storage_analysis) {
  mlir::MLIRContext *context = value.getContext();
  auto *sair_dialect = context->getLoadedDialect<SairDialect>();
  ValueStorage storage = storage_analysis.GetStorage(value);

  // Set memory space to register.
  if (storage.space() == nullptr) {
    AssertSuccess(storage.MergeSpace(sair_dialect->register_attr()));
  }

  // Initialize layout.
  if (storage.layout() == nullptr) {
    int num_dimensions = 0;
    if (storage.buffer_name() != nullptr) {
      const Buffer &buffer = storage_analysis.GetBuffer(storage.buffer_name());
      num_dimensions = buffer.rank();
    }
    const IterationSpace &iter_space =
        iteration_spaces.Get(value.getDefiningOp());

    auto unknown_expr = MappingUnknownExpr::get(context);
    llvm::SmallVector<MappingExpr> exprs(num_dimensions, unknown_expr);
    auto layout = MappingAttr::get(context, iter_space.mapping().size(), exprs);
    AssertSuccess(storage.MergeLayout(layout));
  }
  storage_analysis.MergeStorage(value, storage, fusion_analysis,
                                iteration_spaces);
}

// Adds new dimensions to the operand value layout so that the operand has
// access to the data it needs.
mlir::LogicalResult ExtendLayout(ValueOperand operand,
                                 const IterationSpaceAnalysis &iteration_spaces,
                                 const LoopFusionAnalysis &fusion_analysis,
                                 StorageAnalysis &storage_analysis) {
  mlir::MLIRContext *context = operand.value().getContext();
  const ValueStorage &storage = storage_analysis.GetStorage(operand.value());
  SairOp defining_op = operand.value().getDefiningOp();
  const IterationSpace &def_iter_space = iteration_spaces.Get(defining_op);
  const IterationSpace &use_iter_space =
      iteration_spaces.Get(operand.getOwner());

  // Check what dimensions of communication volume are covered by the layout.
  int operand_rank = operand.Mapping().size();
  MappingAttr communication_volume =
      CommunicationVolume(operand_rank, def_iter_space, use_iter_space);

  MappingAttr layout_to_operand =
      def_iter_space.mapping().Compose(storage.layout()).Inverse();
  MappingAttr layout_to_communication_volume =
      layout_to_operand.Compose(communication_volume);

  if (layout_to_communication_volume.IsSurjective()) return mlir::success();

  assert(storage.buffer_name() != nullptr &&
         "-default-storage-attribute pass should have added buffer names "
         "before reaching this point.");
  const Buffer &buffer = storage_analysis.GetBuffer(storage.buffer_name());
  if (buffer.is_external()) {
    return operand.value().getDefiningOp()->emitError()
           << "specifying value layout would require to increase the rank of "
              "an external buffer";
  }

  // Extend layout to cover comunication volume and permute dimensions so that
  // new dimensions are in front of the domain.
  MappingAttr extended_layout = layout_to_communication_volume.MakeSurjective();
  int num_new_dims = extended_layout.UseDomainSize() - buffer.rank();
  auto new_dims_identity = MappingAttr::GetIdentity(context, num_new_dims);
  auto permutation = MappingAttr::GetIdentity(context, buffer.rank())
                         .ShiftRight(num_new_dims)
                         .AddSuffix(new_dims_identity.Dimensions());
  extended_layout = permutation.Compose(extended_layout);

  // Unify extended_layout with the old layout as some mapping expressions of
  // the old mapping will not appear in the extended one if they do not map to
  // dimensions of communication_volume.
  auto none = MappingNoneExpr::get(context);
  llvm::SmallVector<MappingExpr> none_exprs(num_new_dims, none);
  MappingAttr extended_old_layout = storage.layout().AddPrefix(none_exprs);
  MappingAttr new_layout = def_iter_space.mapping()
                               .Inverse()
                               .Compose(communication_volume)
                               .Compose(extended_layout.Inverse())
                               .Unify(extended_old_layout);
  storage_analysis.AddDimensionsToBuffer(storage.buffer_name(), defining_op,
                                         def_iter_space, fusion_analysis,
                                         new_layout);

  // Set the value layout.
  ValueStorage new_storage = storage;
  AssertSuccess(new_storage.MergeLayout(new_layout));
  storage_analysis.MergeStorage(operand.value(), new_storage, fusion_analysis,
                                iteration_spaces);
  return mlir::success();
}

// Converts unknown expressions from value layout to `none` expressions.
void MakeLayoutFullySpecified(mlir::Value value,
                              const LoopFusionAnalysis &fusion_analysis,
                              const IterationSpaceAnalysis &iteration_spaces,
                              StorageAnalysis &storage_analysis) {
  ValueStorage storage = storage_analysis.GetStorage(value);
  AssertSuccess(storage.MergeLayout(storage.layout().MakeFullySpecified()));
  storage_analysis.MergeStorage(value, storage, fusion_analysis,
                                iteration_spaces);
}

// Assings a buffer name to the operand if it cannot fit in registers.
static mlir::LogicalResult CreateBufferIfNeeded(
    const ValueOperand &operand, const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces,
    StorageAnalysis &storage_analysis) {
  const ValueStorage &storage = storage_analysis.GetStorage(operand.value());
  if (storage.space() != nullptr) return mlir::success();
  if (FitsInRegisters(operand, iteration_spaces)) return mlir::success();
  mlir::Type element_type = operand.GetType().ElementType();
  if (element_type.isa<mlir::IndexType>()) {
    return operand.value().getDefiningOp()->emitError()
           << "cannot generate default storage for multi-dimensional index "
              "values";
  }

  const IterationSpace iter_space = iteration_spaces.Get(operand.getOwner());
  storage_analysis.CreateBuffer(operand.value(), iter_space.loop_names(),
                                fusion_analysis, iteration_spaces);
  return mlir::success();
}

// Assigns the default storage to sair values. This uses registers when possible
// and materializes the minimum amount of dimensions in RAM otherwise. Fails if
// the sub-domain of dimensions to materialize is a dependent domain.
class DefaultStorage : public DefaultStoragePassBase<DefaultStorage> {
 public:
  void runOnFunction() override {
    auto result = getFunction().walk([](ComputeOp op) -> mlir::WalkResult {
      if (!op.loop_nest().hasValue()) {
        return op.emitError() << "expected a loop-nest attribute";
      }
      return mlir::success();
    });
    if (result.wasInterrupted()) {
      signalPassFailure();
      return;
    }

    getFunction().walk([&](SairProgramOp program) -> mlir::WalkResult {
      mlir::LogicalResult result = RunOnProgram(program);
      if (mlir::failed(result)) signalPassFailure();
      return result;
    });
  }

 private:
  mlir::LogicalResult RunOnProgram(SairProgramOp program) {
    auto &iteration_spaces = getChildAnalysis<IterationSpaceAnalysis>(program);
    auto &fusion_analysis = getChildAnalysis<LoopFusionAnalysis>(program);
    auto &storage_analysis = getChildAnalysis<StorageAnalysis>(program);

    // Assign memory space and buffer names to values that won't fit in
    // register.
    auto result = program.walk([&](SairOp op) -> mlir::WalkResult {
      for (ValueOperand operand : op.ValueOperands()) {
        if (mlir::failed(CreateBufferIfNeeded(operand, fusion_analysis,
                                              iteration_spaces,
                                              storage_analysis))) {
          return mlir::failure();
        }
      }
      return mlir::success();
    });
    if (result.wasInterrupted()) return mlir::failure();

    // Assign all remaining values to register and intialize layout fields.
    program.walk([&](SairOp op) {
      for (mlir::Value value : op->getResults()) {
        if (!value.getType().isa<ValueType>()) continue;
        InitializeStorage(value, fusion_analysis, iteration_spaces,
                          storage_analysis);
      }
    });

    // Add layout dimensions when necessary.
    result = program.walk([&](SairOp op) -> mlir::WalkResult {
      for (ValueOperand operand : op.ValueOperands()) {
        if (mlir::failed(ExtendLayout(operand, iteration_spaces,
                                      fusion_analysis, storage_analysis))) {
          return mlir::failure();
        }
      }
      return mlir::success();
    });
    if (result.wasInterrupted()) return mlir::failure();

    // Convert unknown expressions to none expressions. Unknown expressions
    // occure when adding dimensions to buffers. When the buffer is used in
    // multiple places, only the place where the dimension is added will have
    // the layout set for the new dimensions and other places will be unknown.
    program.walk([&](SairOp op) {
      for (mlir::Value value : op->getResults()) {
        if (!value.getType().isa<ValueType>()) continue;
        MakeLayoutFullySpecified(value, fusion_analysis, iteration_spaces,
                                 storage_analysis);
      }
    });

    if (mlir::failed(storage_analysis.VerifyAndMinimizeBufferLoopNests(
            fusion_analysis, iteration_spaces)) ||
        mlir::failed(VerifyValuesNotOverwritten(
            fusion_analysis, iteration_spaces, storage_analysis))) {
      return program.emitError()
             << "unable to generate storage attributes, see other "
                "errors for more information";
    }

    // Commit storage decisions.
    result = program.walk([&](ComputeOp op) -> mlir::WalkResult {
      if (mlir::failed(CommitStorage(op, iteration_spaces, storage_analysis))) {
        return mlir::failure();
      }
      return mlir::success();
    });
    return mlir::failure(result.wasInterrupted());
  }
};

// Generates the default `loop_nest` attribute for an operation with the given
// number of dimensions. The loop nest will start with the given prefix.
mlir::ArrayAttr GetDefaultLoopNest(int num_dimensions,
                                   llvm::ArrayRef<mlir::Attribute> prefix,
                                   LoopFusionAnalysis &fusion_analysis) {
  mlir::MLIRContext *context = fusion_analysis.getContext();
  llvm::SmallVector<MappingExpr, 4> iter_exprs;
  for (mlir::Attribute attr : prefix) {
    LoopAttr loop = attr.cast<LoopAttr>();
    iter_exprs.push_back(loop.iter());
  }

  // Inverse iter expressions and complete the resulting mapping by
  // allocating new loops. Then inverse again to obtain loop iterators.
  MappingAttr partial_inverse =
      MappingAttr::get(context, num_dimensions, iter_exprs).Inverse();
  MappingAttr full_inverse = partial_inverse.MakeSurjective();
  MappingAttr new_iter_exprs = full_inverse.Inverse();

  llvm::SmallVector<mlir::Attribute, 8> loop_nest(prefix.begin(), prefix.end());
  for (MappingExpr expr :
       new_iter_exprs.Dimensions().drop_front(prefix.size())) {
    mlir::StringAttr name = fusion_analysis.GetFreshLoopName();
    loop_nest.push_back(LoopAttr::get(name, expr, context));
  }

  return mlir::ArrayAttr::get(context, loop_nest);
}

// Sets the `loop_nest` attribute to its default value. The default loop nest
// iterates over each dimension of the domain, in order, without
// rematerialization or strip-mining.
class DefaultLoopNest : public DefaultLoopNestPassBase<DefaultLoopNest> {
 public:
  void runOnFunction() override {
    getFunction().walk([&](ComputeOp op) {
      if (op.loop_nest().hasValue()) return;
      SairOp sair_op = cast<SairOp>(op.getOperation());
      SairProgramOp program_op = cast<SairProgramOp>(op->getParentOp());
      auto &fusion_analysis = getChildAnalysis<LoopFusionAnalysis>(program_op);
      int num_dimensions = sair_op.shape().NumDimensions();
      op.setLoopNest(GetDefaultLoopNest(num_dimensions, {}, fusion_analysis));
    });
  }
};

// Modifies the "sequence" attribute of all compute ops in the given program to
// be the canonical sequence value inferred from use-def dependencies of Sair
// values and available sequence attributes. The relative order is preserved but
// not the absolute sequence numbers. The traversal order is deterministic but
// otherwise unspecified for operations that do not have "sequence" attribute
// and belong to different connected components of the use-def dependency graph.
void UpdateSequence(SairProgramOp program) {
  SequenceAnalysis sequence_analysis(program);
  for (auto [index, op] : sequence_analysis.Ops()) {
    op.SetSequence(index);
  }
}

class DefaultSequencePass
    : public DefaultSequencePassBase<DefaultSequencePass> {
 public:
  void runOnFunction() override {
    getFunction().walk(
        [](SairProgramOp program_op) { UpdateSequence(program_op); });
  }
};

}  // namespace

std::unique_ptr<mlir::Pass> CreateDefaultLoopNestPass() {
  return std::make_unique<DefaultLoopNest>();
}

std::unique_ptr<mlir::Pass> CreateDefaultSequencePass() {
  return std::make_unique<DefaultSequencePass>();
}

std::unique_ptr<mlir::Pass> CreateDefaultStoragePass() {
  return std::make_unique<DefaultStorage>();
}

void CreateDefaultLoweringAttributesPipeline(mlir::OpPassManager *pm) {
  pm->addPass(CreateDefaultSequencePass());
  pm->addPass(CreateDefaultLoopNestPass());
  pm->addPass(CreateDefaultStoragePass());
}

}  // namespace sair
