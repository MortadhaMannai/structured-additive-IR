// Copyright 2021 Google LLC
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

#ifndef SAIR_SEQUENCE_H_
#define SAIR_SEQUENCE_H_

#include <map>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/iterator_range.h"
#include "sair_op_interfaces.h"
#include "sair_ops.h"

namespace sair {

// A set of ops of OpTy that preserves the insertion order. Practically, this is
// an llvm::SetVector with additional casting to OpTy since llvm::SetVector
// (precisely, llvm::DenseSet inside it) cannot be constructed for op interface
// classes because their constructors need a non-null operation.
template <typename OpTy>
class ConcreteOpSet {
 public:
  ConcreteOpSet() {}

  // Inserts into the set.
  void insert(OpTy op) { contents_.insert(op.getOperation()); }
  template <typename Iterator>
  void insert(Iterator first, Iterator last) {
    contents_.insert(first, last);
  }

  // Merges the given set of ops into the this set of ops.
  void merge(const ConcreteOpSet<OpTy> &other) {
    contents_.insert(other.contents_.begin(), other.contents_.end());
  }

  // Returns the number of ops in this set.
  size_t size() const { return contents_.size(); }

  // Returns `true` if the set contains the given element.
  bool contains(OpTy op) const { return contents_.contains(op.getOperation()); }

  // Returns an iterator range over the elements.
  auto Ops() const {
    return llvm::map_range(contents_,
                           [](Operation *op) { return cast<OpTy>(op); });
  }

  // Erases the given element from the set.
  void erase(OpTy op) { contents_.remove(op.getOperation()); }

 private:
  llvm::SetVector<Operation *> contents_;
};

using ComputeOpSet = ConcreteOpSet<ComputeOp>;

// An analysis keeping track of Sair compute ops the results of which are used
// as operands in other Sair ops.
class ComputeOpBackwardSliceAnalysis {
 public:
  // Performs the analysis in the given Sair program.
  explicit ComputeOpBackwardSliceAnalysis(SairProgramOp program_op);

  // Returns a set of compute operations the results of which are used in `op`,
  // potentially transformed by non-compute ops only.
  template <typename OpTy>
  const ComputeOpSet &BackwardFrontier(OpTy op) const {
    static_assert(llvm::is_one_of<OpTy, SairOp, ComputeOp>::value,
                  "expected a SairOp or a ComputeOp in BackwardFrontier");
    assert(frontiers_.count(op.getOperation()));
    return frontiers_.find(op.getOperation())->getSecond();
  }

  // Returns a set of compute operations the results of which are transitively
  // use in `op`, that is the backward slice of `op` restricted to compute ops.
  const ComputeOpSet &BackwardSlice(ComputeOp op) const;

 private:
  // Compute the frontier of op and store in in `frontiers_`.
  void ComputeFrontier(SairOp op);

  // If the backward slice of `op` has been computed, merge it into `slice`.
  bool MergeSliceIfAvailable(ComputeOp op, ComputeOpSet &slice) const;

  // Compute ops the results of which are used in `op`, potentially via some
  // non-compute ops.
  llvm::DenseMap<mlir::Operation *, ComputeOpSet> frontiers_;

  // A cache for computed backward slices. These are computed on-demand as both
  // computation and storage are relatively expensive.
  mutable llvm::DenseMap<mlir::Operation *, ComputeOpSet> slice_cache_;
};

// An analysis of the relative positions of Sair operations indicated by their
// sequence attributes.
class SequenceAnalysis {
 public:
  // We use a standard multimap because (a) the sequence numbers can be shared
  // and (b) we need a deterministic increasing order that is provided by this
  // map and not provided by hash table-based maps.
  using MapType = std::multimap<int64_t, ComputeOp>;
  using ConstRangeType = llvm::iterator_range<MapType::const_iterator>;

  // Performs the analysis in the given Sair program.
  explicit SequenceAnalysis(SairProgramOp program_op);

  // Returns an iterator range for traversing operations in their relative
  // order. All operations are given a relative order even if they don't have a
  // sequence attribute attached. The sequence number returned in this iteration
  // may differ from that of the sequence attribute if the Sair program hasn't
  // been canonicalized.
  ConstRangeType Ops() const;

  // Returns an iterator range of all operations sequenced before the given one,
  // in their relative order. All operations are given a relative order even if
  // they don't have a sequence attribute attached. The sequence number returned
  // in this iteration may differ from that of the sequence attribute if the
  // Sair program hasn't been canonicalized.
  ConstRangeType OpsBefore(ComputeOp op) const;

 private:
  // Updates `sequenced_ops_` to have sequence numbers for all compute
  // operations in the program, inferring their relative order from the
  // available sequence attribtues and use-def chains. The relative order is
  // preserved but not the absolute sequence numbers. The traversal order is
  // deterministic but otherwise unspecified for operations that do not have
  // "sequence" attribute and belong to different connected components of the
  // use-def dependency graph.
  void ComputeDefaultSequence(SairProgramOp program);

  MapType sequenced_ops_;
};

}  // namespace sair

#endif  // SAIR_SEQUENCE_H_