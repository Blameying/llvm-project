//===- BufferizableOpInterface.cpp - Bufferizable Ops  ---=----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AsmState.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Value.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "llvm/Support/Debug.h"

//===----------------------------------------------------------------------===//
// BufferizableOpInterface
//===----------------------------------------------------------------------===//

namespace mlir {
namespace bufferization {

#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.cpp.inc"

} // namespace bufferization
} // namespace mlir

MLIR_DEFINE_EXPLICIT_TYPE_ID(mlir::bufferization::AnalysisState)

#define DEBUG_TYPE "bufferizable-op-interface"
#define DBGS() (llvm::dbgs() << '[' << DEBUG_TYPE << "] ")
#define LDBG(X) LLVM_DEBUG(DBGS() << (X))

using namespace mlir;
using namespace bufferization;

static bool isRepetitiveRegion(Region *region,
                               const BufferizationOptions &options) {
  Operation *op = region->getParentOp();
  if (auto bufferizableOp = options.dynCastBufferizableOp(op))
    if (bufferizableOp.isRepetitiveRegion(region->getRegionNumber()))
      return true;
  return false;
}

Region *bufferization::getEnclosingRepetitiveRegion(
    Operation *op, const BufferizationOptions &options) {
  if (!op->getBlock())
    return nullptr;
  return getEnclosingRepetitiveRegion(op->getBlock(), options);
}

Region *bufferization::getEnclosingRepetitiveRegion(
    Value value, const BufferizationOptions &options) {
  Region *region = value.getParentRegion();
  while (region) {
    if (isRepetitiveRegion(region, options))
      return region;
    region = region->getParentRegion();
  }
  return nullptr;
}

Region *bufferization::getEnclosingRepetitiveRegion(
    Block *block, const BufferizationOptions &options) {
  Region *region = block->getParent();
  Operation *op = nullptr;
  do {
    op = region->getParentOp();
    if (isRepetitiveRegion(region, options))
      return region;
  } while ((region = op->getParentRegion()));
  return nullptr;
}

Region *bufferization::getNextEnclosingRepetitiveRegion(
    Region *region, const BufferizationOptions &options) {
  assert(isRepetitiveRegion(region, options) && "expected repetitive region");
  while ((region = region->getParentRegion())) {
    if (isRepetitiveRegion(region, options))
      break;
  }
  return region;
}

Operation *bufferization::getOwnerOfValue(Value value) {
  if (auto opResult = llvm::dyn_cast<OpResult>(value))
    return opResult.getDefiningOp();
  return llvm::cast<BlockArgument>(value).getOwner()->getParentOp();
}

bool bufferization::allocationDoesNotEscape(OpResult opResult) {
#ifndef NDEBUG
  auto bufferizableOp = opResult.getDefiningOp<BufferizableOpInterface>();
  assert(bufferizableOp && bufferizableOp.bufferizesToAllocation(opResult) &&
         "expected op that bufferizes to an allocation");
#endif // NDEBUG

  Operation *op = opResult.getDefiningOp();
  // If there is no 'escape' attribute, we cannot say for sure.
  if (!op->hasAttr(BufferizationDialect::kEscapeAttrName))
    return false;
  auto attr =
      op->getAttrOfType<ArrayAttr>(BufferizationDialect::kEscapeAttrName);
  return !llvm::cast<BoolAttr>(attr[opResult.getResultNumber()]).getValue();
}

/// Create an AllocTensorOp for the given shaped value. If `copy` is set, the
/// shaped value is copied. Otherwise, a tensor with undefined contents is
/// allocated.
FailureOr<Value> bufferization::allocateTensorForShapedValue(
    OpBuilder &b, Location loc, Value shapedValue, bool escape,
    const BufferizationOptions &options, bool copy) {
  Value tensor;
  if (llvm::isa<RankedTensorType>(shapedValue.getType())) {
    tensor = shapedValue;
  } else if (llvm::isa<MemRefType>(shapedValue.getType())) {
    tensor = b.create<ToTensorOp>(loc, shapedValue);
  } else if (llvm::isa<UnrankedTensorType>(shapedValue.getType()) ||
             llvm::isa<UnrankedMemRefType>(shapedValue.getType())) {
    return getOwnerOfValue(shapedValue)
        ->emitError("copying of unranked tensors is not implemented");
  } else {
    llvm_unreachable("expected RankedTensorType or MemRefType");
  }
  RankedTensorType tensorType = llvm::cast<RankedTensorType>(tensor.getType());
  SmallVector<Value> dynamicSizes;
  if (!copy) {
    // Compute the dynamic part of the shape.
    // First try to query the shape via ReifyRankedShapedTypeOpInterface.
    bool reifiedShapes = false;
    if (llvm::isa<RankedTensorType>(shapedValue.getType()) &&
        llvm::isa<OpResult>(shapedValue)) {
      ReifiedRankedShapedTypeDims resultDims;
      if (succeeded(
              reifyResultShapes(b, shapedValue.getDefiningOp(), resultDims))) {
        reifiedShapes = true;
        auto &shape =
            resultDims[llvm::cast<OpResult>(shapedValue).getResultNumber()];
        for (const auto &dim : enumerate(tensorType.getShape()))
          if (ShapedType::isDynamic(dim.value()))
            dynamicSizes.push_back(shape[dim.index()].get<Value>());
      }
    }

    // If the shape could not be reified, create DimOps.
    if (!reifiedShapes)
      populateDynamicDimSizes(b, loc, tensor, dynamicSizes);
  }

  // Create AllocTensorOp.
  auto allocTensorOp = b.create<AllocTensorOp>(loc, tensorType, dynamicSizes,
                                               copy ? tensor : Value());
  allocTensorOp->setAttr(BufferizationDialect::kEscapeAttrName,
                         b.getBoolArrayAttr({escape}));

  // Add 'memory_space' attribute. Not needed if 'copy' operand is specified.
  if (copy)
    return allocTensorOp.getResult();
  FailureOr<BaseMemRefType> copyBufferType = getBufferType(tensor, options);
  if (failed(copyBufferType))
    return failure();
  Attribute memorySpace = copyBufferType->getMemorySpace();
  if (!memorySpace)
    memorySpace = b.getI64IntegerAttr(0);
  allocTensorOp.setMemorySpaceAttr(memorySpace);
  return allocTensorOp.getResult();
}

LogicalResult BufferizableOpInterface::resolveTensorOpOperandConflicts(
    RewriterBase &rewriter, const AnalysisState &state) {
  OpBuilder::InsertionGuard g(rewriter);
  Operation *op = getOperation();
  SmallVector<OpOperand *> outOfPlaceOpOperands;
  DenseSet<OpOperand *> copiedOpOperands;
  DenseSet<OpOperand *> escapingOpOperandCopies;
  SmallVector<OpResult> outOfPlaceOpResults;
  DenseSet<OpResult> copiedOpResults;
  DenseSet<OpResult> escapingOpResultCopies;

  // Find all out-of-place OpOperands.
  for (OpOperand &opOperand : op->getOpOperands()) {
    Type operandType = opOperand.get().getType();
    if (!llvm::isa<TensorType>(operandType))
      continue;
    if (state.isInPlace(opOperand))
      continue;
    if (llvm::isa<UnrankedTensorType>(operandType))
      return op->emitError("copying of unranked tensors is not implemented");

    AliasingOpResultList aliasingOpResults =
        state.getAliasingOpResults(opOperand);
    // Is the result yielded from a block? Or are deallocations turned off
    // entirely? In either case, mark the allocation as "escaping", so that it
    // will not be deallocated.
    bool escape = !state.getOptions().createDeallocs ||
                  llvm::any_of(aliasingOpResults, [&](AliasingOpResult a) {
                    return state.isTensorYielded(a.opResult);
                  });

    if (aliasingOpResults.getNumAliases() == 1 &&
        !state.bufferizesToMemoryWrite(opOperand) &&
        state.getAliasingOpOperands(aliasingOpResults.getAliases()[0].opResult)
                .getNumAliases() == 1 &&
        !llvm::isa<UnrankedTensorType>(
            aliasingOpResults.getAliases()[0].opResult.getType())) {
      // The op itself does not write but may create exactly one alias. Instead
      // of copying the OpOperand, copy the OpResult. The OpResult can sometimes
      // be smaller than the OpOperand (e.g., in the case of an extract_slice,
      // where the result is usually a smaller part of the source). Do not apply
      // this optimization if the OpResult is an unranked tensor (because those
      // cannot be copied at the moment).
      OpResult opResult = aliasingOpResults.getAliases()[0].opResult;
      outOfPlaceOpResults.push_back(opResult);
      if (!state.canOmitTensorCopy(opOperand))
        copiedOpResults.insert(opResult);
      if (escape)
        escapingOpResultCopies.insert(opResult);
    } else {
      // In all other cases, make a copy of the OpOperand.
      outOfPlaceOpOperands.push_back(&opOperand);
      if (!state.canOmitTensorCopy(opOperand))
        copiedOpOperands.insert(&opOperand);
      if (escape)
        escapingOpOperandCopies.insert(&opOperand);
    }
  }

  // Insert copies of OpOperands.
  rewriter.setInsertionPoint(op);
  for (OpOperand *opOperand : outOfPlaceOpOperands) {
    FailureOr<Value> copy = allocateTensorForShapedValue(
        rewriter, op->getLoc(), opOperand->get(),
        escapingOpOperandCopies.contains(opOperand), state.getOptions(),
        copiedOpOperands.contains(opOperand));
    if (failed(copy))
      return failure();
    rewriter.updateRootInPlace(op, [&]() { opOperand->set(*copy); });
  }

  // Insert copies of OpResults.
  rewriter.setInsertionPointAfter(op);
  for (OpResult opResult : outOfPlaceOpResults) {
    FailureOr<Value> copy = allocateTensorForShapedValue(
        rewriter, op->getLoc(), opResult,
        escapingOpResultCopies.contains(opResult), state.getOptions(),
        copiedOpResults.count(opResult));
    if (failed(copy))
      return failure();
    SmallVector<OpOperand *> uses = llvm::to_vector(llvm::map_range(
        opResult.getUses(), [](OpOperand &use) { return &use; }));
    for (OpOperand *use : uses) {
      // Do not update the alloc_tensor op that we just created.
      if (use->getOwner() == copy->getDefiningOp())
        continue;
      // tensor.dim ops may have been created to be used as alloc_tensor op
      // dynamic extents. Do not update these either.
      if (isa<tensor::DimOp>(use->getOwner()))
        continue;
      rewriter.updateRootInPlace(use->getOwner(), [&]() { use->set(*copy); });
    }
  }

  return success();
}

bool bufferization::shouldDeallocateOpResult(
    OpResult opResult, const BufferizationOptions &options) {
  Operation *op = opResult.getOwner();
  assert(options.dynCastBufferizableOp(op).bufferizesToAllocation(opResult) &&
         "expected that op allocates");

  AnalysisState analysisState(options);
  if (op->hasAttr(BufferizationDialect::kEscapeAttrName)) {
    // AllocTensorOp has one result.
    ArrayAttr escapeAttr = llvm::cast<ArrayAttr>(
        op->getAttr(BufferizationDialect::kEscapeAttrName));
    return !llvm::cast<BoolAttr>(escapeAttr[0]).getValue();
  }

  // No "escape" annotation found.
  if (options.createDeallocs) {
    // Perform an ad-hoc analysis.
    return !analysisState.isTensorYielded(opResult);
  }

  return false;
}

//===----------------------------------------------------------------------===//
// OpFilter
//===----------------------------------------------------------------------===//

bool OpFilter::isOpAllowed(Operation *op) const {
  // All other ops: Allow/disallow according to filter.
  bool isAllowed = !hasAllowRule();
  for (const Entry &entry : entries) {
    bool filterResult = entry.fn(op);
    switch (entry.type) {
    case Entry::ALLOW:
      isAllowed |= filterResult;
      break;
    case Entry::DENY:
      if (filterResult)
        // DENY filter matches. This op is no allowed. (Even if other ALLOW
        // filters may match.)
        return false;
    };
  }
  return isAllowed;
}

//===----------------------------------------------------------------------===//
// BufferizationOptions
//===----------------------------------------------------------------------===//

namespace {

/// Default function arg type converter: Use a fully dynamic layout map.
BaseMemRefType
defaultFunctionArgTypeConverter(TensorType type, Attribute memorySpace,
                                func::FuncOp funcOp,
                                const BufferizationOptions &options) {
  return getMemRefTypeWithFullyDynamicLayout(type, memorySpace);
}
/// Default unknown type converter: Use a fully dynamic layout map.
BaseMemRefType
defaultUnknownTypeConverter(Value value, Attribute memorySpace,
                            const BufferizationOptions &options) {
  return getMemRefTypeWithFullyDynamicLayout(
      llvm::cast<TensorType>(value.getType()), memorySpace);
}

} // namespace

// Default constructor for BufferizationOptions.
BufferizationOptions::BufferizationOptions()
    : functionArgTypeConverterFn(defaultFunctionArgTypeConverter),
      unknownTypeConverterFn(defaultUnknownTypeConverter) {}

bool BufferizationOptions::isOpAllowed(Operation *op) const {
  // Special case: If function boundary bufferization is deactivated, do not
  // allow ops that belong to the `func` dialect.
  bool isFuncBoundaryOp = isa_and_nonnull<func::FuncDialect>(op->getDialect());
  if (!bufferizeFunctionBoundaries && isFuncBoundaryOp)
    return false;

  return opFilter.isOpAllowed(op);
}

BufferizableOpInterface
BufferizationOptions::dynCastBufferizableOp(Operation *op) const {
  auto bufferizableOp = dyn_cast<BufferizableOpInterface>(op);
  if (!bufferizableOp)
    return nullptr;
  if (!isOpAllowed(op))
    return nullptr;
  return bufferizableOp;
}

BufferizableOpInterface
BufferizationOptions::dynCastBufferizableOp(Value value) const {
  return dynCastBufferizableOp(getOwnerOfValue(value));
}

void BufferizationOptions::setFunctionBoundaryTypeConversion(
    LayoutMapOption layoutMapOption) {
  functionArgTypeConverterFn = [=](TensorType tensorType, Attribute memorySpace,
                                   func::FuncOp funcOp,
                                   const BufferizationOptions &options) {
    if (layoutMapOption == LayoutMapOption::IdentityLayoutMap)
      return bufferization::getMemRefTypeWithStaticIdentityLayout(tensorType,
                                                                  memorySpace);
    return bufferization::getMemRefTypeWithFullyDynamicLayout(tensorType,
                                                              memorySpace);
  };
  inferFunctionResultLayout =
      layoutMapOption == LayoutMapOption::InferLayoutMap;
}

//===----------------------------------------------------------------------===//
// Helper functions for BufferizableOpInterface
//===----------------------------------------------------------------------===//

static void setInsertionPointAfter(OpBuilder &b, Value value) {
  if (auto bbArg = llvm::dyn_cast<BlockArgument>(value)) {
    b.setInsertionPointToStart(bbArg.getOwner());
  } else {
    b.setInsertionPointAfter(value.getDefiningOp());
  }
}

/// Determine which OpOperand* will alias with `opResult` if the op is
/// bufferized in place. Return all tensor OpOperand* if the op is not
/// bufferizable.
AliasingOpOperandList
AnalysisState::getAliasingOpOperands(OpResult opResult) const {
  if (Operation *op = opResult.getDefiningOp())
    if (auto bufferizableOp = getOptions().dynCastBufferizableOp(op))
      return bufferizableOp.getAliasingOpOperands(opResult, *this);

  // The op is not bufferizable.
  return detail::unknownGetAliasingOpOperands(opResult);
}

/// Determine which OpResult will alias with `opOperand` if the op is bufferized
/// in place. Return all tensor OpResults if the op is not bufferizable.
AliasingOpResultList
AnalysisState::getAliasingOpResults(OpOperand &opOperand) const {
  if (auto bufferizableOp =
          getOptions().dynCastBufferizableOp(opOperand.getOwner()))
    return bufferizableOp.getAliasingOpResults(opOperand, *this);

  // The op is not bufferizable.
  return detail::unknownGetAliasingOpResults(opOperand);
}

/// Return true if `opOperand` bufferizes to a memory read. Return `true` if the
/// op is not bufferizable.
bool AnalysisState::bufferizesToMemoryRead(OpOperand &opOperand) const {
  if (auto bufferizableOp =
          getOptions().dynCastBufferizableOp(opOperand.getOwner()))
    return bufferizableOp.bufferizesToMemoryRead(opOperand, *this);

  // Unknown op that returns a tensor. The inplace analysis does not support it.
  // Conservatively return true.
  return true;
}

/// Return true if `opOperand` bufferizes to a memory write. Return
/// `true` if the op is not bufferizable.
bool AnalysisState::bufferizesToMemoryWrite(OpOperand &opOperand) const {
  if (auto bufferizableOp =
          getOptions().dynCastBufferizableOp(opOperand.getOwner()))
    return bufferizableOp.bufferizesToMemoryWrite(opOperand, *this);

  // Unknown op that returns a tensor. The inplace analysis does not support it.
  // Conservatively return true.
  return true;
}

/// Return true if `opOperand` does neither read nor write but bufferizes to an
/// alias. Return false if the op is not bufferizable.
bool AnalysisState::bufferizesToAliasOnly(OpOperand &opOperand) const {
  if (auto bufferizableOp =
          getOptions().dynCastBufferizableOp(opOperand.getOwner()))
    return bufferizableOp.bufferizesToAliasOnly(opOperand, *this);

  // Unknown op that returns a tensor. The inplace analysis does not support it.
  // Conservatively return false.
  return false;
}

bool AnalysisState::bufferizesToMemoryWrite(Value value) const {
  auto opResult = llvm::dyn_cast<OpResult>(value);
  if (!opResult)
    return true;
  auto bufferizableOp = getOptions().dynCastBufferizableOp(value);
  if (!bufferizableOp)
    return true;
  return bufferizableOp.resultBufferizesToMemoryWrite(opResult, *this);
}

/// Return true if the given value is read by an op that bufferizes to a memory
/// read. Also takes into account ops that create an alias but do not read by
/// themselves (e.g., ExtractSliceOp).
bool AnalysisState::isValueRead(Value value) const {
  assert(llvm::isa<TensorType>(value.getType()) && "expected TensorType");
  SmallVector<OpOperand *> workingSet;
  for (OpOperand &use : value.getUses())
    workingSet.push_back(&use);

  while (!workingSet.empty()) {
    OpOperand *uMaybeReading = workingSet.pop_back_val();
    // Skip over all ops that neither read nor write (but create an alias).
    if (bufferizesToAliasOnly(*uMaybeReading))
      for (AliasingOpResult alias : getAliasingOpResults(*uMaybeReading))
        for (OpOperand &use : alias.opResult.getUses())
          workingSet.push_back(&use);
    if (bufferizesToMemoryRead(*uMaybeReading))
      return true;
  }

  return false;
}

// Starting from `value`, follow the use-def chain in reverse, always selecting
// the aliasing OpOperands. Find and return Values for which `condition`
// evaluates to true. OpOperands of such matching Values are not traversed any
// further.
llvm::SetVector<Value> AnalysisState::findValueInReverseUseDefChain(
    Value value, llvm::function_ref<bool(Value)> condition,
    TraversalConfig config) const {
  llvm::SetVector<Value> result, workingSet;
  workingSet.insert(value);

  while (!workingSet.empty()) {
    Value value = workingSet.pop_back_val();
    if (condition(value)) {
      result.insert(value);
      continue;
    }

    if (llvm::isa<BlockArgument>(value)) {
      if (config.alwaysIncludeLeaves)
        result.insert(value);
      continue;
    }

    OpResult opResult = llvm::cast<OpResult>(value);
    BufferizableOpInterface bufferizableOp =
        options.dynCastBufferizableOp(opResult.getDefiningOp());
    if (!config.followUnknownOps && !bufferizableOp) {
      // Stop iterating if `followUnknownOps` is unset and the op is either
      // not bufferizable or excluded in the OpFilter.
      if (config.alwaysIncludeLeaves)
        result.insert(value);
      continue;
    }

    AliasingOpOperandList aliases = getAliasingOpOperands(opResult);
    if (aliases.getNumAliases() == 0) {
      // The traversal ends naturally if there are no more OpOperands that
      // could be followed.
      if (config.alwaysIncludeLeaves)
        result.insert(value);
      continue;
    }

    for (AliasingOpOperand a : aliases) {
      if (config.followEquivalentOnly &&
          a.relation != BufferRelation::Equivalent) {
        // Stop iterating if `followEquivalentOnly` is set but the alias is not
        // equivalent.
        if (config.alwaysIncludeLeaves)
          result.insert(value);
      } else {
        workingSet.insert(a.opOperand->get());
      }

      if (config.followInPlaceOnly && !isInPlace(*a.opOperand)) {
        // Stop iterating if `followInPlaceOnly` is set but the alias is
        // out-of-place.
        if (config.alwaysIncludeLeaves)
          result.insert(value);
        continue;
      }

      workingSet.insert(a.opOperand->get());
    }
  }

  return result;
}

// Find the values that define the contents of the given value.
llvm::SetVector<Value> AnalysisState::findDefinitions(Value value) const {
  TraversalConfig config;
  config.alwaysIncludeLeaves = false;
  return findValueInReverseUseDefChain(
      value, [&](Value v) { return this->bufferizesToMemoryWrite(v); }, config);
}

AnalysisState::AnalysisState(const BufferizationOptions &options)
    : AnalysisState(options, TypeID::get<AnalysisState>()) {}

AnalysisState::AnalysisState(const BufferizationOptions &options, TypeID type)
    : options(options), type(type) {
  for (const BufferizationOptions::AnalysisStateInitFn &fn :
       options.stateInitializers)
    fn(*this);
}

bool AnalysisState::canOmitTensorCopy(OpOperand &opOperand) const {
  // Do not copy if the tensor has undefined contents.
  if (hasUndefinedContents(&opOperand))
    return true;

  // Do not copy if the buffer of the tensor is entirely overwritten (with
  // values that do not depend on the old tensor).
  if (bufferizesToMemoryWrite(opOperand) && !bufferizesToMemoryRead(opOperand))
    return true;

  // Do not copy if the tensor is never read.
  AliasingOpResultList aliases = getAliasingOpResults(opOperand);
  if (!bufferizesToMemoryRead(opOperand) &&
      llvm::none_of(
          aliases, [&](AliasingOpResult a) { return isValueRead(a.opResult); }))
    return true;

  // Default: Cannot omit the copy.
  return false;
}

bool AnalysisState::isInPlace(OpOperand &opOperand) const {
  // ToMemrefOps are always in-place.
  if (isa<ToMemrefOp>(opOperand.getOwner()))
    return true;

  // In the absence of analysis information, OpOperands that bufferize to a
  // memory write are out-of-place, i.e., an alloc and copy is inserted.
  return !bufferizesToMemoryWrite(opOperand);
}

bool AnalysisState::areEquivalentBufferizedValues(Value v1, Value v2) const {
  // In the absence of analysis information, we do not know if the values are
  // equivalent. The conservative answer is "false".
  return false;
}

bool AnalysisState::areAliasingBufferizedValues(Value v1, Value v2) const {
  // In the absence of analysis information, we do not know if the values may be
  // aliasing. The conservative answer is "true".
  return true;
}

bool AnalysisState::hasUndefinedContents(OpOperand *opOperand) const {
  // In the absence of analysis information, the conservative answer is "false".
  return false;
}

bool AnalysisState::isTensorYielded(Value tensor) const {
  // In the absence of analysis information, the conservative answer is "true".
  if (!tensor.getDefiningOp<AllocTensorOp>())
    return true;

  // For AllocTensorOp results, we can do better: They do not alias with any
  // preceding value, so we can follow SSA use-def chains and do a simple
  // analysis.
  SmallVector<OpOperand *> worklist;
  for (OpOperand &use : tensor.getUses())
    worklist.push_back(&use);

  while (!worklist.empty()) {
    OpOperand *operand = worklist.pop_back_val();
    Operation *op = operand->getOwner();

    // If the op is not bufferizable, we can safely assume that the value is not
    // yielded. (When bufferizing that op, it must handle such cases.)
    if (!options.dynCastBufferizableOp(op))
      continue;

    // We cannot analyze through ToMemrefOps, so we have to conservatively
    // assume that the value is yielded.
    if (isa<ToMemrefOp>(op))
      return true;

    // Check if the op is returning/yielding.
    if (isRegionReturnLike(op))
      return true;

    // Add all aliasing OpResults to the worklist.
    // Note: In the absence of detailed analysis information (e.g., there may be
    // no function call analysis information), this `getAliasingOpResult` is
    // conservative and may report additional OpResults as potentially aliasing.
    for (AliasingOpResult alias : getAliasingOpResults(*operand))
      for (OpOperand &use : alias.opResult.getUses())
        worklist.push_back(&use);
  }

  // No ReturnLike op found: The value is not yielded.
  return false;
}

// bufferization.to_memref is not allowed to change the rank.
static void ensureToMemrefOpIsValid(Value tensor, Type memrefType) {
#ifndef NDEBUG
  auto rankedTensorType = llvm::dyn_cast<RankedTensorType>(tensor.getType());
  assert((!rankedTensorType || llvm::cast<MemRefType>(memrefType).getRank() ==
                                   rankedTensorType.getRank()) &&
         "to_memref would be invalid: mismatching ranks");
#endif
}

FailureOr<Value> bufferization::getBuffer(RewriterBase &rewriter, Value value,
                                          const BufferizationOptions &options) {
#ifndef NDEBUG
  auto tensorType = llvm::dyn_cast<TensorType>(value.getType());
  assert(tensorType && "unexpected non-tensor type");
#endif // NDEBUG

  // Replace "%t = to_tensor %m" with %m.
  if (auto toTensorOp = value.getDefiningOp<bufferization::ToTensorOp>())
    return toTensorOp.getMemref();

  // Insert to_memref op.
  OpBuilder::InsertionGuard g(rewriter);
  setInsertionPointAfter(rewriter, value);
  FailureOr<BaseMemRefType> memrefType = getBufferType(value, options);
  if (failed(memrefType))
    return failure();
  ensureToMemrefOpIsValid(value, *memrefType);
  return rewriter
      .create<bufferization::ToMemrefOp>(value.getLoc(), *memrefType, value)
      .getResult();
}

/// Return the buffer type for a given Value (tensor) after bufferization.
FailureOr<BaseMemRefType>
bufferization::getBufferType(Value value, const BufferizationOptions &options) {
  DenseMap<Value, BaseMemRefType> fixedTypes;
  return getBufferType(value, options, fixedTypes);
}

/// Return the buffer type for a given Value (tensor) after bufferization.
FailureOr<BaseMemRefType> bufferization::getBufferType(
    Value value, const BufferizationOptions &options,
    const DenseMap<Value, BaseMemRefType> &fixedTypes) {
  assert(llvm::isa<TensorType>(value.getType()) &&
         "unexpected non-tensor type");

  // If the `value` is in `fixedTypes`, return the mapped type.
  const auto &it = fixedTypes.find(value);
  if (it != fixedTypes.end())
    return it->second;

  // Try querying BufferizableOpInterface.
  Operation *op = getOwnerOfValue(value);
  auto bufferizableOp = options.dynCastBufferizableOp(op);
  if (bufferizableOp)
    return bufferizableOp.getBufferType(value, options, fixedTypes);

  // Op is not bufferizable.
  if (!options.defaultMemorySpace.has_value())
    return op->emitError("could not infer memory space");

  return getMemRefType(value, options, /*layout=*/{},
                       *options.defaultMemorySpace);
}

void bufferization::replaceOpWithBufferizedValues(RewriterBase &rewriter,
                                                  Operation *op,
                                                  ValueRange values) {
  assert(values.size() == op->getNumResults() &&
         "expected one value per OpResult");
  OpBuilder::InsertionGuard g(rewriter);

  // Replace all OpResults with the given values.
  SmallVector<Value> replacements;
  for (OpResult opResult : op->getOpResults()) {
    Value replacement = values[opResult.getResultNumber()];
    if (llvm::isa<TensorType>(opResult.getType())) {
      // The OpResult is a tensor. Such values are replaced with memrefs during
      // bufferization.
      assert((llvm::isa<MemRefType>(replacement.getType()) ||
              llvm::isa<UnrankedMemRefType>(replacement.getType())) &&
             "tensor op result should be replaced with a memref value");
      // The existing uses of the OpResult still expect a tensor. Insert a
      // ToTensorOp. Throughout bufferization, this ToTensorOp will gradually
      // loose all of its users and eventually DCE away.
      rewriter.setInsertionPointAfter(op);
      replacement = rewriter.create<bufferization::ToTensorOp>(
          replacement.getLoc(), replacement);
    }
    replacements.push_back(replacement);
  }

  rewriter.replaceOp(op, replacements);
}

//===----------------------------------------------------------------------===//
// Bufferization-specific scoped alloc/dealloc insertion support.
//===----------------------------------------------------------------------===//

/// Create a memref allocation with the given type and dynamic extents.
FailureOr<Value> BufferizationOptions::createAlloc(OpBuilder &b, Location loc,
                                                   MemRefType type,
                                                   ValueRange dynShape) const {
  if (allocationFn)
    return (*allocationFn)(b, loc, type, dynShape, bufferAlignment);

  // Default bufferallocation via AllocOp.
  if (bufferAlignment != 0)
    return b
        .create<memref::AllocOp>(loc, type, dynShape,
                                 b.getI64IntegerAttr(bufferAlignment))
        .getResult();
  return b.create<memref::AllocOp>(loc, type, dynShape).getResult();
}

/// Creates a memref deallocation. The given memref buffer must have been
/// allocated using `createAlloc`.
LogicalResult BufferizationOptions::createDealloc(OpBuilder &b, Location loc,
                                                  Value allocatedBuffer) const {
  if (deallocationFn)
    return (*deallocationFn)(b, loc, allocatedBuffer);

  // Default buffer deallocation via DeallocOp.
  b.create<memref::DeallocOp>(loc, allocatedBuffer);
  return success();
}

/// Create a memory copy between two memref buffers.
LogicalResult BufferizationOptions::createMemCpy(OpBuilder &b, Location loc,
                                                 Value from, Value to) const {
  if (memCpyFn)
    return (*memCpyFn)(b, loc, from, to);

  b.create<memref::CopyOp>(loc, from, to);
  return success();
}

//===----------------------------------------------------------------------===//
// Bufferization-specific IRMapping support with debugging.
//===----------------------------------------------------------------------===//

bool bufferization::isFunctionArgument(Value value) {
  auto bbArg = llvm::dyn_cast<BlockArgument>(value);
  if (!bbArg)
    return false;
  return isa<func::FuncOp>(bbArg.getOwner()->getParentOp());
}

BaseMemRefType bufferization::getMemRefType(Value value,
                                            const BufferizationOptions &options,
                                            MemRefLayoutAttrInterface layout,
                                            Attribute memorySpace) {
  auto tensorType = llvm::cast<TensorType>(value.getType());

  // Case 1: Unranked memref type.
  if (auto unrankedTensorType =
          llvm::dyn_cast<UnrankedTensorType>(tensorType)) {
    assert(!layout && "UnrankedTensorType cannot have a layout map");
    return UnrankedMemRefType::get(unrankedTensorType.getElementType(),
                                   memorySpace);
  }

  // Case 2: Ranked memref type with specified layout.
  auto rankedTensorType = llvm::cast<RankedTensorType>(tensorType);
  if (layout) {
    return MemRefType::get(rankedTensorType.getShape(),
                           rankedTensorType.getElementType(), layout,
                           memorySpace);
  }

  return options.unknownTypeConverterFn(value, memorySpace, options);
}

BaseMemRefType
bufferization::getMemRefTypeWithFullyDynamicLayout(TensorType tensorType,
                                                   Attribute memorySpace) {
  // Case 1: Unranked memref type.
  if (auto unrankedTensorType =
          llvm::dyn_cast<UnrankedTensorType>(tensorType)) {
    return UnrankedMemRefType::get(unrankedTensorType.getElementType(),
                                   memorySpace);
  }

  // Case 2: Ranked memref type.
  auto rankedTensorType = llvm::cast<RankedTensorType>(tensorType);
  int64_t dynamicOffset = ShapedType::kDynamic;
  SmallVector<int64_t> dynamicStrides(rankedTensorType.getRank(),
                                      ShapedType::kDynamic);
  auto stridedLayout = StridedLayoutAttr::get(tensorType.getContext(),
                                              dynamicOffset, dynamicStrides);
  return MemRefType::get(rankedTensorType.getShape(),
                         rankedTensorType.getElementType(), stridedLayout,
                         memorySpace);
}

/// Return a MemRef type with a static identity layout (i.e., no layout map). If
/// the given tensor type is unranked, return an unranked MemRef type.
BaseMemRefType
bufferization::getMemRefTypeWithStaticIdentityLayout(TensorType tensorType,
                                                     Attribute memorySpace) {
  // Case 1: Unranked memref type.
  if (auto unrankedTensorType =
          llvm::dyn_cast<UnrankedTensorType>(tensorType)) {
    return UnrankedMemRefType::get(unrankedTensorType.getElementType(),
                                   memorySpace);
  }

  // Case 2: Ranked memref type.
  auto rankedTensorType = llvm::cast<RankedTensorType>(tensorType);
  MemRefLayoutAttrInterface layout = {};
  return MemRefType::get(rankedTensorType.getShape(),
                         rankedTensorType.getElementType(), layout,
                         memorySpace);
}

//===----------------------------------------------------------------------===//
// Default implementations of interface methods
//===----------------------------------------------------------------------===//

bool bufferization::detail::defaultResultBufferizesToMemoryWrite(
    OpResult opResult, const AnalysisState &state) {
  auto bufferizableOp = cast<BufferizableOpInterface>(opResult.getDefiningOp());
  AliasingOpOperandList opOperands =
      bufferizableOp.getAliasingOpOperands(opResult, state);

  // Case 1: OpResults that have no aliasing OpOperand usually bufferize to
  // memory writes.
  if (opOperands.getAliases().empty())
    return true;

  // Case 2: If an aliasing OpOperand bufferizes to a memory write, the OpResult
  // may bufferize to a memory write.
  if (llvm::any_of(opOperands, [&](AliasingOpOperand alias) {
        return state.bufferizesToMemoryWrite(*alias.opOperand);
      }))
    return true;

  // Case 3: Check if a nested aliasing OpOperand value bufferizes to a memory
  // write. (Or: The reverse SSA use-def chain ends inside the reigon.) In that
  // case, the OpResult bufferizes to a memory write. E.g.:
  //
  // %0 = "some_writing_op" : tensor<?xf32>
  // %r = scf.if ... -> tensor<?xf32> {
  //   scf.yield %0 : tensor<?xf32>
  // } else {
  //   %1 = "another_writing_op"(%0) : tensor<?xf32>
  //   scf.yield %1 : tensor<?xf32>
  // }
  // "some_reading_op"(%r)
  //
  // %r bufferizes to a memory write because an aliasing OpOperand value (%1)
  // bufferizes to a memory write and the defining op is inside the scf.if.
  //
  // Note: This treatment of surrouding ops is useful for ops that have a
  // region but no OpOperand such as scf.if or scf.execute_region. It simplifies
  // the analysis considerably.
  //
  // "another_writing_op" in the above example should be able to bufferize
  // inplace in the absence of another read of %0. However, if the scf.if op
  // would not be considered a "write", the analysis would detect the
  // following conflict:
  //
  // * read = some_reading_op
  // * lastWrite = %0  (Note: The last write of %r would be a set: {%0, %1}.)
  // * conflictingWrite = %1
  //
  auto isMemoryWriteInsideOp = [&](Value v) {
    Operation *op = getOwnerOfValue(v);
    if (!opResult.getDefiningOp()->isAncestor(op))
      return false;
    return state.bufferizesToMemoryWrite(v);
  };
  TraversalConfig config;
  config.alwaysIncludeLeaves = false;
  for (AliasingOpOperand alias : opOperands) {
    if (!state
             .findValueInReverseUseDefChain(alias.opOperand->get(),
                                            isMemoryWriteInsideOp, config)
             .empty())
      return true;
  }
  return false;
}

// Compute the AliasingOpOperandList for a given OpResult based on
// getAliasingOpResults.
AliasingOpOperandList bufferization::detail::defaultGetAliasingOpOperands(
    OpResult opResult, const AnalysisState &state) {
  Operation *op = opResult.getDefiningOp();
  SmallVector<AliasingOpOperand> result;
  for (OpOperand &opOperand : op->getOpOperands()) {
    if (!llvm::isa<TensorType>(opOperand.get().getType()))
      continue;
    AliasingOpResultList aliasingOpResults =
        state.getAliasingOpResults(opOperand);
    for (const auto &it : aliasingOpResults)
      if (it.opResult == opResult)
        result.emplace_back(&opOperand, it.relation, it.isDefinite);
  }
  return AliasingOpOperandList(std::move(result));
}

FailureOr<BaseMemRefType> bufferization::detail::defaultGetBufferType(
    Value value, const BufferizationOptions &options,
    const DenseMap<Value, BaseMemRefType> &fixedTypes) {
  assert(llvm::isa<TensorType>(value.getType()) && "expected tensor type");

  // No further analysis is possible for a block argument.
  if (llvm::isa<BlockArgument>(value))
    return bufferization::getMemRefType(value, options);

  // Value is an OpResult.
  Operation *op = getOwnerOfValue(value);
  auto opResult = llvm::cast<OpResult>(value);
  AnalysisState state(options);
  AliasingOpOperandList aliases = state.getAliasingOpOperands(opResult);
  if (aliases.getNumAliases() > 0 &&
      aliases.getAliases()[0].relation == BufferRelation::Equivalent) {
    // If the OpResult has an equivalent OpOperand, both OpResult and
    // OpOperand bufferize to the exact same buffer type.
    Value equivalentOperand = aliases.getAliases().front().opOperand->get();
    return getBufferType(equivalentOperand, options, fixedTypes);
  }

  // If we do not know the memory space and there is no default memory space,
  // report a failure.
  if (!options.defaultMemorySpace.has_value())
    return op->emitError("could not infer memory space");

  return getMemRefType(value, options, /*layout=*/{},
                       *options.defaultMemorySpace);
}

bool bufferization::detail::defaultIsRepetitiveRegion(
    BufferizableOpInterface bufferizableOp, unsigned index) {
  assert(index < bufferizableOp->getNumRegions() && "invalid region index");
  auto regionInterface =
      dyn_cast<RegionBranchOpInterface>(bufferizableOp.getOperation());
  if (!regionInterface)
    return false;
  return regionInterface.isRepetitiveRegion(index);
}

AliasingOpOperandList
bufferization::detail::unknownGetAliasingOpOperands(OpResult opResult) {
  // Conservatively assume that everything may be aliasing.
  AliasingOpOperandList r;
  for (OpOperand &operand : opResult.getDefiningOp()->getOpOperands())
    if (llvm::isa<TensorType>(operand.get().getType()))
      r.addAlias({&operand, BufferRelation::Unknown, /*isDefinite=*/false});
  return r;
}

AliasingOpResultList
bufferization::detail::unknownGetAliasingOpResults(OpOperand &opOperand) {
  // Conservatively assume that everything may be aliasing.
  AliasingOpResultList r;
  for (OpResult result : opOperand.getOwner()->getOpResults())
    if (llvm::isa<TensorType>(result.getType()))
      r.addAlias({result, BufferRelation::Unknown, /*isDefinite=*/false});
  return r;
}
