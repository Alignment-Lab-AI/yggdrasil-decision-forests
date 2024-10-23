/*
 * Copyright 2022 Google LLC.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "yggdrasil_decision_forests/dataset/data_spec.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/utils/logging.h"

namespace yggdrasil_decision_forests::dataset {

// TODO: Move column implementations here.

VerticalDataset::NumericalVectorSequenceColumn::NumericalVectorSequenceColumn(
    int vector_length)
    : vector_length_(vector_length) {}

std::string
VerticalDataset::NumericalVectorSequenceColumn::ToStringWithDigitPrecision(
    const row_t row, const proto::Column& col_spec, int digit_precision) const {
  std::string rep;
  if (IsNa(row)) {
    return "NA";
  }
  const auto num_sequences = SequenceLength(row);

  absl::StrAppend(&rep, "[");
  for (uint32_t sequence_idx = 0; sequence_idx < num_sequences;
       sequence_idx++) {
    if (sequence_idx != 0) {
      absl::StrAppend(&rep, ", ");
    }
    absl::StrAppend(&rep, "[");
    const auto sequence = GetVector(row, sequence_idx).value();
    for (size_t vector_idx = 0; vector_idx < sequence.size(); vector_idx++) {
      if (vector_idx != 0) {
        absl::StrAppend(&rep, ", ");
      }
      absl::StrAppendFormat(&rep, "%.*g", digit_precision,
                            sequence[vector_idx]);
    }
    absl::StrAppend(&rep, "]");
  }
  absl::StrAppend(&rep, "]");
  return rep;
}

bool VerticalDataset::NumericalVectorSequenceColumn::IsNa(
    const row_t row) const {
  return items_[row].size == -1;
}

void VerticalDataset::NumericalVectorSequenceColumn::AddNA() {
  items_.emplace_back(0, -1);
}

void VerticalDataset::NumericalVectorSequenceColumn::SetNA(const row_t row) {
  items_[row] = {0, -1};
}

void VerticalDataset::NumericalVectorSequenceColumn::Resize(
    const row_t num_rows) {
  items_.resize(num_rows, {0, -1});
}

void VerticalDataset::NumericalVectorSequenceColumn::Reserve(const row_t row) {
  items_.reserve(row);
}

VerticalDataset::row_t VerticalDataset::NumericalVectorSequenceColumn::nrows()
    const {
  return items_.size();
}

void VerticalDataset::NumericalVectorSequenceColumn::Add(
    absl::Span<const float> values) {
  DCHECK_EQ(values.size() % vector_length_, 0);
  const int32_t num_values = values.size() / vector_length_;
  items_.emplace_back(values_.size(), num_values);
  values_.insert(values_.end(), values.begin(), values.end());
}

void VerticalDataset::NumericalVectorSequenceColumn::Set(
    row_t row, absl::Span<const float> values) {
  DCHECK_EQ(values.size() % vector_length_, 0);
  const int32_t num_values = values.size() / vector_length_;
  items_[row] = {values_.size(), num_values};
  values_.insert(values_.end(), values.begin(), values.end());
}

void VerticalDataset::NumericalVectorSequenceColumn::AddFromExample(
    const proto::Example::Attribute& attribute) {
  if (ABSL_PREDICT_FALSE(dataset::IsNa(attribute))) {
    AddNA();
  } else {
    DCHECK_EQ(attribute.type_case(),
              proto::Example::Attribute::TypeCase::kNumericalVectorSequence);
    items_.emplace_back(values_.size(),
                        attribute.numerical_vector_sequence().vectors_size());
    for (const auto& src_vector :
         attribute.numerical_vector_sequence().vectors()) {
      const auto src_values = src_vector.values();
      DCHECK_EQ(src_values.size(), vector_length_);
      values_.insert(values_.end(), src_values.begin(), src_values.end());
    }
  }
}

void VerticalDataset::NumericalVectorSequenceColumn::Set(
    row_t example_idx, const proto::Example::Attribute& attribute) {
  if (dataset::IsNa(attribute)) {
    SetNA(example_idx);
  } else {
    DCHECK_EQ(attribute.type_case(),
              proto::Example::Attribute::TypeCase::kNumericalVectorSequence);
    items_[example_idx] = {
        values_.size(), attribute.numerical_vector_sequence().vectors_size()};
    for (const auto& src_vector :
         attribute.numerical_vector_sequence().vectors()) {
      const auto src_values = src_vector.values();
      DCHECK_EQ(src_values.size(), vector_length_);
      values_.insert(values_.end(), src_values.begin(), src_values.end());
    }
  }
}

void VerticalDataset::NumericalVectorSequenceColumn::ExtractExample(
    row_t example_idx, proto::Example::Attribute* attribute) const {
  if (IsNa(example_idx)) {
    return;
  }
  auto* dst = attribute->mutable_numerical_vector_sequence()->mutable_vectors();
  const auto num_sequences = SequenceLength(example_idx);
  for (uint32_t sequence_idx = 0; sequence_idx < num_sequences;
       sequence_idx++) {
    const auto src = GetVector(example_idx, sequence_idx).value();
    *dst->Add()->mutable_values() = {src.begin(), src.end()};
  }
}

absl::Status
VerticalDataset::NumericalVectorSequenceColumn::ConvertToGivenDataspec(
    AbstractColumn* dst, const proto::Column& src_spec,
    const proto::Column& dst_spec) const {
  return absl::InvalidArgumentError(
      "Dataspec conversion is not supported for vector sequence columns.");
}

std::pair<uint64_t, uint64_t>
VerticalDataset::NumericalVectorSequenceColumn::memory_usage() const {
  return std::pair<uint64_t, uint64_t>(
      items_.size() * sizeof(PerExample) + values_.size() * sizeof(float),
      items_.capacity() * sizeof(PerExample) +
          values_.capacity() * sizeof(float));
}

void VerticalDataset::NumericalVectorSequenceColumn::ShrinkToFit() {
  items_.shrink_to_fit();
}

}  // namespace yggdrasil_decision_forests::dataset
