# Copyright 2022 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The value / prediction of a leaf."""

import abc
import dataclasses
import math
from typing import Optional, Sequence
import numpy as np
from yggdrasil_decision_forests.model.decision_tree import decision_tree_pb2


# TODO: 310218604 - Use kw_only with default value num_examples = 1.
@dataclasses.dataclass
class AbstractValue(metaclass=abc.ABCMeta):
  """A generic value/prediction/output.

  Attrs:
    num_examples: Number of example in the node.
  """

  num_examples: float


@dataclasses.dataclass
class RegressionValue(AbstractValue):
  """The regression value of a regressive tree.

  Can also be used in gradient-boosted-trees for classification and ranking.

  Attrs:
    value: Value of the tree. The semantic depends on the tree: For Regression
      Random Forest and Regression GBDT, this value is a regressive value in the
      same unit as the label. For classification and ranking GBDTs, this value
      is a logit.
    standard_deviation: Optional standard deviation attached to the value.
  """

  value: float
  standard_deviation: Optional[float] = None


@dataclasses.dataclass
class ProbabilityValue(AbstractValue):
  """A probability distribution value.

  Used for random Forest / CART classification trees.

  Attrs:
    probability: An array of probabilities of the label classes i.e. the i-th
      value is the probability of the "label_value_idx_to_value(..., i)" class.
      Note that the first value is reserved for the Out-of-vocabulary
  """

  probability: Sequence[float]


@dataclasses.dataclass
class UpliftValue(AbstractValue):
  """The uplift value of a classification or regression uplift tree.

  Attrs:
    treatment_effect: An array of the effects on the treatment groups. The i-th
      element of this array is the effect of the "i+1"th treatment compared to
      the control group.
  """

  treatment_effect: Sequence[float]


def to_value(proto_node: decision_tree_pb2.Node) -> AbstractValue:
  """Extracts the "value" part of a proto node."""

  if proto_node.HasField("classifier"):
    dist = proto_node.classifier.distribution
    # Note: The first value (out-of-dictionary) is removed.
    probabilities = np.array(dist.counts[1:]) / dist.sum
    return ProbabilityValue(
        probability=probabilities.tolist(), num_examples=dist.sum
    )

  if proto_node.HasField("regressor"):
    dist = proto_node.regressor.distribution
    standard_deviation = None
    if dist.HasField("sum_squares") and dist.count > 0:
      variance = dist.sum_squares / dist.count - dist.sum**2 / dist.count**2
      if variance >= 0:
        standard_deviation = math.sqrt(variance)
    return RegressionValue(
        value=proto_node.regressor.top_value,
        num_examples=dist.count,
        standard_deviation=standard_deviation,
    )

  if proto_node.HasField("uplift"):
    return UpliftValue(
        treatment_effect=proto_node.uplift.treatment_effect[:],
        num_examples=proto_node.uplift.sum_weights,
    )

  raise ValueError("Unsupported value")
