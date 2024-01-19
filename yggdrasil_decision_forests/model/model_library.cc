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

#include "yggdrasil_decision_forests/model/model_library.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/strings/substitute.h"
#include "yggdrasil_decision_forests/model/abstract_model.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/utils/filesystem.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/utils/status_macros.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace {
constexpr char kModelHeaderFileName[] = "header.pb";
constexpr char kModelDataSpecFileName[] = "data_spec.pb";

// Name of the subdirectory containing an YDF model in a TF-DF model.
constexpr char kTensorFlowDecisionForestsAssets[] = "assets";
// Name of the file that identifies a Tensorflow SavedModel.
constexpr char kTensorFlowSavedModelProtoFileName[] = "saved_model.pb";

// Last file created in the model directory when a model is exported.
//
// Note: This file is only used by YDF to delay and retry loading a model.
constexpr char kModelDoneFileName[] = "done";

// Add changes to the model path to improve loading performance here.
std::string ImproveModelReadingPath(const absl::string_view path) {
  return std::string(path);
}
}  // namespace

std::vector<std::string> AllRegisteredModels() {
  return AbstractModelRegisterer::GetNames();
}

absl::Status CreateEmptyModel(const absl::string_view model_name,
                              std::unique_ptr<AbstractModel>* model) {
  ASSIGN_OR_RETURN(*model, AbstractModelRegisterer::Create(model_name));
  if (model->get()->name() != model_name) {
    return absl::AbortedError(
        absl::Substitute("The model registration key does not match the model "
                         "exposed key. $0 vs $1",
                         model_name, model->get()->name()));
  }
  return absl::OkStatus();
}

absl::Status SaveModel(absl::string_view directory, const AbstractModel& mdl,
                       ModelIOOptions io_options) {
  return SaveModel(directory, &mdl, io_options);
}

absl::Status SaveModel(absl::string_view directory,
                       const AbstractModel* const mdl,
                       ModelIOOptions io_options) {
  RETURN_IF_ERROR(mdl->Validate());
  RETURN_IF_ERROR(file::RecursivelyCreateDir(directory, file::Defaults()));
  proto::AbstractModel header;
  AbstractModel::ExportProto(*mdl, &header);
  io_options.file_prefix = io_options.file_prefix.value_or("");
  RETURN_IF_ERROR(file::SetBinaryProto(
      file::JoinPath(directory, absl::StrCat(io_options.file_prefix.value(),
                                             kModelHeaderFileName)),
      header, file::Defaults()));
  RETURN_IF_ERROR(file::SetBinaryProto(
      file::JoinPath(directory, absl::StrCat(io_options.file_prefix.value(),
                                             kModelDataSpecFileName)),
      mdl->data_spec(), file::Defaults()));
  RETURN_IF_ERROR(mdl->Save(directory, io_options));

  RETURN_IF_ERROR(file::SetContent(
      file::JoinPath(directory, absl::StrCat(io_options.file_prefix.value(),
                                             kModelDoneFileName)),
      ""));
  return absl::OkStatus();
}

absl::Status LoadModel(absl::string_view directory,
                       std::unique_ptr<AbstractModel>* model,
                       ModelIOOptions io_options) {
  proto::AbstractModel header;
  std::string effective_directory = ImproveModelReadingPath(directory);

  ASSIGN_OR_RETURN(const bool is_tensorflow_saved_model,
                   IsTensorFlowSavedModel(effective_directory));
  if (is_tensorflow_saved_model) {
    effective_directory =
        file::JoinPath(effective_directory, kTensorFlowDecisionForestsAssets);
    YDF_LOG(INFO)
        << "Detected `" << kTensorFlowSavedModelProtoFileName
        << "` in directory " << directory
        << ". Loading a TensorFlow Decision Forests model from C++ YDF or CLI "
           "is brittle and should not be relied upon. Use the Python API of "
           "YDF to convert the model to a regular YDF model with "
           "`ydf.from_tensorflow_decision_forests(model_path)`";
  }
  if (!io_options.file_prefix) {
    ASSIGN_OR_RETURN(io_options.file_prefix,
                     DetectFilePrefix(effective_directory));
  }

  RETURN_IF_ERROR(file::GetBinaryProto(
      file::JoinPath(
          effective_directory,
          absl::StrCat(io_options.file_prefix.value(), kModelHeaderFileName)),
      &header, file::Defaults()));
  RETURN_IF_ERROR(CreateEmptyModel(header.name(), model));
  AbstractModel::ImportProto(header, model->get());
  RETURN_IF_ERROR(file::GetBinaryProto(
      file::JoinPath(
          effective_directory,
          absl::StrCat(io_options.file_prefix.value(), kModelDataSpecFileName)),
      model->get()->mutable_data_spec(), file::Defaults()));
  RETURN_IF_ERROR(model->get()->Load(effective_directory, io_options));
  return model->get()->Validate();
}

absl::StatusOr<bool> ModelExists(absl::string_view directory,
                                 const ModelIOOptions& io_options) {
  if (io_options.file_prefix) {
    return file::FileExists(file::JoinPath(
        directory,
        absl::StrCat(io_options.file_prefix.value(), kModelDataSpecFileName)));
  }
  return DetectFilePrefix(directory).ok();
}

absl::StatusOr<std::string> DetectFilePrefix(absl::string_view directory) {
  std::vector<std::string> done_files;
  RETURN_IF_ERROR(file::Match(
      file::JoinPath(directory, absl::StrCat("*", kModelDataSpecFileName)),
      &done_files, file::Defaults()));
  if (done_files.size() != 1) {
    return absl::FailedPreconditionError(
        absl::Substitute("File prefix cannot be autodetected: $0 models exist "
                         "in $1",
                         done_files.size(), directory));
  }
  return file::GetBasename(
      absl::StripSuffix(done_files[0], kModelDataSpecFileName));
}

absl::StatusOr<bool> IsTensorFlowSavedModel(absl::string_view model_directory) {
  return file::FileExists(
      file::JoinPath(model_directory, kTensorFlowSavedModelProtoFileName));
}

}  // namespace model
}  // namespace yggdrasil_decision_forests
