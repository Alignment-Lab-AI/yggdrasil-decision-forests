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

#ifndef YGGDRASIL_DECISION_FORESTS_UTILS_EVALUATION_H_
#define YGGDRASIL_DECISION_FORESTS_UTILS_EVALUATION_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/dataset/example.pb.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/model/prediction.pb.h"

namespace yggdrasil_decision_forests {
namespace utils {

// The format of predictions exported in a tabular format.
//
// Details: Predictions are represented by the proto proto::Prediction. The user
// can choose to export the predictions to a tabular format (e.g. csv file). In
// this case, 'PredictionFormat' specifies how this conversion is done.
enum class PredictionFormat {
  // Raw prediction data. Contains all the information about the original
  // prediction. Well suited to process the predictions programmatically.
  //
  // For classification, outputs one probability column for each of the classes.
  // For regression, ranking and uplift, outputs the predicted value.
  kRaw,

  // Simplified predictions. Well suited for simple user reading.
  //
  // For classification, outputs the predicted class. For regression, ranking
  // and uplift, outputs the predicted value.
  kSimple,

  // Like kSimple, but with more details.
  //
  // For classification, outputs the predicted class and its probability. For
  // regression, ranking and uplift, outputs the predicted value.
  kRich,

  // Like kRich, but with more details.
  //
  // For classification, outputs the predicted class and one probability column
  // for each of the classes. For regression, ranking and uplift, outputs the
  // predicted value.
  kFull,
};

// Parses a string representation of "PredictionFormat".
absl::StatusOr<PredictionFormat> ParsePredictionFormat(absl::string_view value);

// Exports a list of predictions to disk.
absl::Status ExportPredictions(
    const std::vector<model::proto::Prediction>& predictions,
    model::proto::Task task, const dataset::proto::Column& label_column,
    absl::string_view typed_prediction_path,
    int num_records_by_shard_in_output = -1,
    absl::optional<std::string> prediction_key = {},
    PredictionFormat format = PredictionFormat::kRaw);

// Dataspec of the dataset containing the predictions generated by
// "ExportPredictions".
absl::StatusOr<dataset::proto::DataSpecification> PredictionDataspec(
    model::proto::Task task, const dataset::proto::Column& label_col,
    absl::optional<std::string> prediction_key = {},
    PredictionFormat format = PredictionFormat::kRaw);

// Converts a prediction stored as a proto::Prediction into a prediction stored
// in a proto::Example following the dataspec generated by "PredictionDataspec";
//
// Predictions stored as "proto::Example" can be used to interact with other ML
// frameworks (as proto::Example can be exported to file).
//
// Note: This method does not generate predictions from a model. Instead, it
// converts predictions from one format to another.
absl::Status PredictionToExample(
    model::proto::Task task, const dataset::proto::Column& label_col,
    const model::proto::Prediction& prediction,
    dataset::proto::Example* prediction_as_example,
    absl::optional<std::string> prediction_key = {},
    PredictionFormat format = PredictionFormat::kRaw);

// Converts a prediction stored as a proto::Example into prediction stored as a
// proto::Prediction following the dataspec generated by "PredictionDataspec";
//
// Note: This method does not generate predictions from a model. Instead, it
// converts predictions from one format to another.
absl::Status ExampleToPrediction(
    model::proto::Task task, const dataset::proto::Column& label_col,
    const dataset::proto::Example& prediction_as_example,
    model::proto::Prediction* prediction);

}  // namespace utils
}  // namespace yggdrasil_decision_forests

#endif  // THIRD_PARTY_YGGDRASIL_DECISION_FORESTS_UTILS_EVALUATION_H_
