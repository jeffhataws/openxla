/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/all_gather_decomposer.h"

#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/str_join.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/literal_util.h"
#include "xla/service/collective_decomposer_utils.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/shape_util.h"
#include "xla/status_macros.h"
#include "xla/types.h"
#include "tsl/platform/logging.h"

namespace xla {

// Creates a computation of x + y.
HloComputation* MakeBinaryAdd(PrimitiveType type, HloModule* module) {
  HloComputation::Builder sum_b("add");
  auto x = sum_b.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/0, ShapeUtil::MakeShape(type, {}), "x"));
  auto y = sum_b.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/1, ShapeUtil::MakeShape(type, {}), "y"));
  if (type == PRED) {
    sum_b.AddInstruction(HloInstruction::CreateBinary(
        ShapeUtil::MakeShape(type, {}), HloOpcode::kOr, x, y));
  } else {
    sum_b.AddInstruction(HloInstruction::CreateBinary(
        ShapeUtil::MakeShape(type, {}), HloOpcode::kAdd, x, y));
  }
  HloComputation* reduction = module->AddEmbeddedComputation(sum_b.Build());
  return reduction;
}

HloInstruction* TranslateAllGatherToAllReducePerOperand(
    CollectiveOpGroupMode group_mode, const HloAllGatherInstruction& ag,
    const Shape& output_shape, HloInstruction* operand, HloComputation* comp) {
  std::vector<HloInstruction*> start_indices =
      CreateStartIndicesForCollectiveDecomposition(
          group_mode, ag.replica_groups(), operand->shape(),
          ag.all_gather_dimension(), comp)
          .value();

  auto zero = comp->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(output_shape.element_type())));
  zero = comp->AddInstruction(
      HloInstruction::CreateBroadcast(output_shape, zero, {}));

  auto dus = comp->AddInstruction(HloInstruction::CreateDynamicUpdateSlice(
      zero->shape(), zero, operand, start_indices));
  auto ar = comp->AddInstruction(HloInstruction::CreateAllReduce(
      dus->shape(), {dus},
      MakeBinaryAdd(dus->shape().element_type(), comp->parent()),
      ag.replica_groups(),
      /*constrain_layout=*/ag.constrain_layout(), ag.channel_id(),
      ag.use_global_device_ids()));
  return ar;
}

Status DecomposeAllGather(HloAllGatherInstruction* ag, HloComputation* comp) {
  TF_ASSIGN_OR_RETURN(CollectiveOpGroupMode group_mode,
                      GetCollectiveOpGroupMode(ag->channel_id().has_value(),
                                               ag->use_global_device_ids()));
  if (ag->operand_count() > 1) {
    HloInstruction* token = ag->mutable_operands().back();
    std::vector<HloInstruction*> tuple_inputs;
    for (int i = 0; i < ag->operand_count() - 1; ++i) {
      auto* input_operand = ag->mutable_operand(i);
      const auto& output_shape = ag->shape().tuple_shapes(i);
      auto* ar = TranslateAllGatherToAllReducePerOperand(
          group_mode, *ag, output_shape, input_operand, comp);
      tuple_inputs.push_back(ar);
    }
    tuple_inputs.push_back(token);
    auto tup = comp->AddInstruction(HloInstruction::CreateTuple(tuple_inputs));
    TF_RETURN_IF_ERROR(ag->ReplaceAllUsesWith(tup));
  } else {
    auto* ar = TranslateAllGatherToAllReducePerOperand(
        group_mode, *ag, ag->shape(), ag->mutable_operand(0), comp);
    TF_RETURN_IF_ERROR(ag->ReplaceAllUsesWith(ar));
  }
  TF_RETURN_IF_ERROR(comp->RemoveInstructionAndUnusedOperands(ag));
  return OkStatus();
}

StatusOr<bool> AllGatherDecomposer::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  for (auto comp : module->MakeNonfusionComputations(execution_threads)) {
    for (auto hlo : comp->MakeInstructionPostOrder()) {
      if (hlo->opcode() != HloOpcode::kAllGather) {
        continue;
      }
      auto ag = Cast<HloAllGatherInstruction>(hlo);
      if (should_decompose_(*ag)) {
        TF_RETURN_IF_ERROR(DecomposeAllGather(ag, comp));
        changed = true;
      }
    }
  }
  return changed;
}

}  // namespace xla
