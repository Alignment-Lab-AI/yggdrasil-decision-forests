/*
 * Copyright 2021 Google LLC.
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

#include "yggdrasil_decision_forests/learner/distributed_gradient_boosted_trees/distributed_gradient_boosted_trees.h"

#include "yggdrasil_decision_forests/dataset/formats.h"
#include "yggdrasil_decision_forests/learner/decision_tree/generic_parameters.h"
#include "yggdrasil_decision_forests/learner/distributed_decision_tree/dataset_cache/dataset_cache.h"
#include "yggdrasil_decision_forests/learner/distributed_decision_tree/dataset_cache/dataset_cache_common.h"
#include "yggdrasil_decision_forests/learner/distributed_decision_tree/training.h"
#include "yggdrasil_decision_forests/learner/distributed_gradient_boosted_trees/common.h"
#include "yggdrasil_decision_forests/learner/distributed_gradient_boosted_trees/worker.pb.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/gradient_boosted_trees.h"
#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.h"
#include "yggdrasil_decision_forests/utils/filesystem.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/utils/snapshot.h"
#include "yggdrasil_decision_forests/utils/uid.h"
#include "yggdrasil_decision_forests/utils/usage.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace distributed_gradient_boosted_trees {

constexpr char DistributedGradientBoostedTreesLearner::kRegisteredName[];
using distributed_decision_tree::dataset_cache::proto::CacheMetadata_Column;

model::proto::LearnerCapabilities
DistributedGradientBoostedTreesLearner::Capabilities() const {
  model::proto::LearnerCapabilities capabilities;
  capabilities.set_resume_training(true);
  capabilities.set_support_partial_cache_dataset_format(true);
  return capabilities;
}

utils::StatusOr<std::unique_ptr<AbstractModel>>
DistributedGradientBoostedTreesLearner::TrainWithStatus(
    const dataset::VerticalDataset& train_dataset,
    absl::optional<std::reference_wrapper<const dataset::VerticalDataset>>
        valid_dataset) const {
  return absl::InvalidArgumentError(
      "The Distributed Gradient Boosted Tree learner does not support training "
      "from in-memory datasets (i.e. VerticalDataset in Yggdrasil Decision "
      "Forests, (non distributed) Dataset in TensorFlow Decision Forests). If "
      "your dataset is small, use the (non distributed) Gradient Boosted Tree "
      "learner (i.e. GRADIENT_BOOSTED_TREES with Yggdrasil Decision Forests, "
      "GradientBoostedTreesModel in TensorFlow Decision Forests). If your "
      "dataset is large, provide the dataset as a path (Yggdrasil Decision "
      "Forests) or use a TF Distribution Strategy (TensorFlow Decision "
      "Forests).");
}

absl::Status DistributedGradientBoostedTreesLearner::SetHyperParametersImpl(
    utils::GenericHyperParameterConsumer* generic_hyper_params) {
  // Use the non-distributed GBT learner to set the configuration.
  gradient_boosted_trees::GradientBoostedTreesLearner gbt_learner(
      training_config_);
  RETURN_IF_ERROR(gbt_learner.SetHyperParametersImpl(generic_hyper_params));
  auto* dgbt_config = training_config_.MutableExtension(
      distributed_gradient_boosted_trees::proto::
          distributed_gradient_boosted_trees_config);
  dgbt_config->mutable_gbt()->MergeFrom(
      gbt_learner.training_config().GetExtension(
          gradient_boosted_trees::proto::gradient_boosted_trees_config));

  return absl::OkStatus();
}

utils::StatusOr<model::proto::GenericHyperParameterSpecification>
DistributedGradientBoostedTreesLearner::GetGenericHyperParameterSpecification()
    const {
  ASSIGN_OR_RETURN(auto hparam_def,
                   AbstractLearner::GetGenericHyperParameterSpecification());

  hparam_def.mutable_documentation()->set_description(
      "Exact distributed version of the Gradient Boosted Tree learning "
      "algorithm. See the documentation of the non-distributed Gradient "
      "Boosted Tree learning algorithm for an introduction to GBTs.");

  using GBTL = gradient_boosted_trees::GradientBoostedTreesLearner;
  GBTL gbt_learner(training_config_);
  ASSIGN_OR_RETURN(const auto gbt_params,
                   gbt_learner.GetGenericHyperParameterSpecification());

  // Extract a subset of supported non-distributed GBT parameters.
  for (const auto& supported_field : {
           GBTL::kHParamNumTrees,
           GBTL::kHParamShrinkage,
           GBTL::kHParamUseHessianGain,
           GBTL::kHParamApplyLinkFunction,
           decision_tree::kHParamMaxDepth,
           decision_tree::kHParamMinExamples,
       }) {
    const auto src_field = gbt_params.fields().find(supported_field);
    if (src_field == gbt_params.fields().end()) {
      return absl::InternalError(
          absl::StrCat("Could not find field ", supported_field));
    }
    hparam_def.mutable_fields()->operator[](supported_field) =
        src_field->second;
  }

  return hparam_def;
}

utils::StatusOr<std::unique_ptr<AbstractModel>>
DistributedGradientBoostedTreesLearner::TrainWithStatus(
    const absl::string_view typed_path,
    const dataset::proto::DataSpecification& data_spec,
    const absl::optional<std::string>& typed_valid_path) const {
  const auto begin_training = absl::Now();

  internal::Monitoring monitoring;

  // Extract and check the configuration.
  auto config = training_config();
  model::proto::TrainingConfigLinking config_link;
  RETURN_IF_ERROR(
      AbstractLearner::LinkTrainingConfig(config, data_spec, &config_link));
  auto& spe_config = *config.MutableExtension(
      proto::distributed_gradient_boosted_trees_config);
  RETURN_IF_ERROR(internal::SetDefaultHyperParameters(config, config_link,
                                                      data_spec, &spe_config));
  RETURN_IF_ERROR(internal::CheckConfiguration(deployment_));

  utils::usage::OnTrainingStart(data_spec, config, config_link,
                                /*num_examples=*/-1);

  // Working directory.
  auto work_directory = deployment().cache_path();
  if (!deployment().try_resume_training()) {
    work_directory = file::JoinPath(
        work_directory, absl::StrCat(std::random_device{}(), "_",
                                     absl::ToUnixMicros(absl::Now())));
  }
  auto updated_deployment = deployment();
  updated_deployment.mutable_distribute()->set_working_directory(
      work_directory);

  // Detect if the training dataset is a stored in the dataset cache format
  // directly, or if the conversion should be done first.
  std::string train_path, train_prefix;
  ASSIGN_OR_RETURN(std::tie(train_prefix, train_path),
                   dataset::SplitTypeAndPath(typed_path));

  std::string dataset_cache_path =
      file::JoinPath(work_directory, kFileNameDatasetCache);
  if (train_prefix == dataset::FORMAT_PARTIAL_DATASET_CACHE) {
    // The dataset is stored in the partially cache format.
    monitoring.BeginDatasetCacheCreation();
    RETURN_IF_ERROR(internal::CreateDatasetCacheFromPartialDatasetCache(
        updated_deployment, train_path, dataset_cache_path, config_link,
        spe_config, data_spec));

    // TODO(gbm): Delete the partial dataset cache.
  } else {
    // The dataset is stored in a generic format.

    // Create / resume the creation of the dataset cache.
    monitoring.BeginDatasetCacheCreation();
    RETURN_IF_ERROR(internal::CreateDatasetCache(
        updated_deployment, dataset_cache_path, config_link, spe_config,
        typed_path, data_spec));
  }

  // Train the model.
  monitoring.BeginTraining();
  ASSIGN_OR_RETURN(
      auto model,
      internal::TrainWithCache(updated_deployment, config, config_link,
                               spe_config, dataset_cache_path, work_directory,
                               data_spec, log_directory(), &monitoring));

  utils::usage::OnTrainingEnd(data_spec, config, config_link,
                              /*num_examples=*/-1, *model,
                              absl::Now() - begin_training);

  return std::move(model);
}

namespace internal {
using ::yggdrasil_decision_forests::model::gradient_boosted_trees::
    GradientBoostedTreesModel;

absl::Status SetDefaultHyperParameters(
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const dataset::proto::DataSpecification& data_spec,
    proto::DistributedGradientBoostedTreesTrainingConfig* const spe_config) {
  RETURN_IF_ERROR(gradient_boosted_trees::internal::SetDefaultHyperParameters(
      spe_config->mutable_gbt()));

  // TODO(gbm): Call "SetDefaultHyperParameters" of GBT.

  // Select the loss function.
  if (spe_config->mutable_gbt()->loss() ==
      gradient_boosted_trees::proto::Loss::DEFAULT) {
    ASSIGN_OR_RETURN(
        const auto default_loss,
        gradient_boosted_trees::internal::DefaultLoss(
            config.task(), data_spec.columns(config_link.label())));
    spe_config->mutable_gbt()->set_loss(default_loss);
    LOG(INFO) << "Default loss set to "
              << gradient_boosted_trees::proto::Loss_Name(
                     spe_config->mutable_gbt()->loss());
  }

  return absl::OkStatus();
}

absl::Status CheckConfiguration(
    const model::proto::DeploymentConfig& deployment) {
  if (deployment.cache_path().empty()) {
    return absl::InvalidArgumentError(
        "deployment.cache_path is empty. Please provide a cache directory with "
        "ensemble distributed training.");
  }
  if (!deployment.distribute().working_directory().empty()) {
    return absl::InvalidArgumentError(
        "deployment.distribute.working_directory should be empty. Use "
        "deployment.cache_path to specify the cache directory.");
  }
  return absl::OkStatus();
}

absl::Status CreateDatasetCacheFromPartialDatasetCache(
    const model::proto::DeploymentConfig& deployment,
    absl::string_view partial_cache_path, absl::string_view final_cache_path,
    const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const dataset::proto::DataSpecification& data_spec) {
  auto create_cache_config = spe_config.create_cache();
  create_cache_config.set_label_column_idx(config_link.label());
  if (config_link.has_weight_definition()) {
    if (!config_link.weight_definition().has_numerical()) {
      return absl::InvalidArgumentError(
          "Only the weighting with a numerical column is supported");
    }
    create_cache_config.set_weight_column_idx(
        config_link.weight_definition().attribute_idx());
  }

  return distributed_decision_tree::dataset_cache::
      CreateDatasetCacheFromPartialDatasetCache(
          data_spec, partial_cache_path, final_cache_path, create_cache_config,
          deployment.distribute(), /*delete_source_file=*/true);
}

absl::Status CreateDatasetCache(
    const model::proto::DeploymentConfig& deployment,
    const absl::string_view cache_path,
    const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const absl::string_view typed_path,
    const dataset::proto::DataSpecification& data_spec) {
  auto create_cache_config = spe_config.create_cache();
  create_cache_config.set_label_column_idx(config_link.label());
  if (config_link.has_weight_definition()) {
    if (!config_link.weight_definition().has_numerical()) {
      return absl::InvalidArgumentError(
          "Only the weighting with a numerical column is supported");
    }
    create_cache_config.set_weight_column_idx(
        config_link.weight_definition().attribute_idx());
  }
  std::vector<int> columns{config_link.features().begin(),
                           config_link.features().end()};
  RETURN_IF_ERROR(distributed_decision_tree::dataset_cache::
                      CreateDatasetCacheFromShardedFiles(
                          typed_path, data_spec, &columns, cache_path,
                          create_cache_config, deployment.distribute()));
  return absl::OkStatus();
}

utils::StatusOr<
    std::unique_ptr<gradient_boosted_trees::GradientBoostedTreesModel>>
TrainWithCache(
    const model::proto::DeploymentConfig& deployment,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const absl::string_view cache_path, const absl::string_view work_directory,
    const dataset::proto::DataSpecification& data_spec,
    const absl::string_view& log_directory, internal::Monitoring* monitoring) {
  RETURN_IF_ERROR(InitializeDirectoryStructure(work_directory));

  // Loss to optimize.
  ASSIGN_OR_RETURN(const auto loss, gradient_boosted_trees::CreateLoss(
                                        spe_config.gbt().loss(), config.task(),
                                        data_spec.columns(config_link.label()),
                                        spe_config.gbt()));

  // Allocate each feature to a worker.
  ASSIGN_OR_RETURN(
      const auto cache_metadata,
      distributed_decision_tree::dataset_cache::LoadCacheMetadata(cache_path));
  std::vector<int> input_features = {config_link.features().begin(),
                                     config_link.features().end()};
  ASSIGN_OR_RETURN(const auto num_workers,
                   distribute::NumWorkers(deployment.distribute()));
  ASSIGN_OR_RETURN(const auto feature_ownership,
                   AssignFeaturesToWorkers(spe_config, input_features,
                                           num_workers, cache_metadata));

  // Initializer the distribute manager.
  ASSIGN_OR_RETURN(auto distribute_manager,
                   InitializeDistributionManager(
                       deployment, config, config_link, spe_config, cache_path,
                       work_directory, data_spec, feature_ownership));

  // Warn the workers that the training will start.
  RETURN_IF_ERROR(EmitStartTraining(distribute_manager.get(), monitoring));

  utils::RandomEngine random(config.random_seed());

  // Initializer or restore the model.
  int iter_idx = 0;
  std::unique_ptr<gradient_boosted_trees::GradientBoostedTreesModel> model;
  decision_tree::proto::LabelStatistics label_statistics;

  // Minimum iter index for the creation of a new checkpoint.
  int minimum_iter_for_new_checkpoint = -1;

  auto last_checkpoint_idx =
      utils::GetGreatestSnapshot(SnapshotDirectory(work_directory));
  if (last_checkpoint_idx.ok()) {
    // Restoring the model from the checkpoint.
    iter_idx = last_checkpoint_idx.value();
    LOG(INFO) << "Resume training from iteration #" << iter_idx;
    minimum_iter_for_new_checkpoint = iter_idx + 1;
    proto::Checkpoint checkpoint_metadata;
    RETURN_IF_ERROR(RestoreManagerCheckpoint(
        last_checkpoint_idx.value(), work_directory, &model, &label_statistics,
        &checkpoint_metadata));
    model->set_data_spec(data_spec);
    InitializeModelWithAbstractTrainingConfig(config, config_link, model.get());
    RETURN_IF_ERROR(EmitRestoreCheckpoint(
        last_checkpoint_idx.value(), checkpoint_metadata.num_shards(),
        model->num_trees_per_iter(), distribute_manager.get(), monitoring));
  } else {
    // Initializing a new model.
    ASSIGN_OR_RETURN(model, InitializeModel(config, config_link, spe_config,
                                            data_spec, *loss));

    // TODO(gbm): Send a ping to all the workers to make sure they all start
    // loading the dataset cache immediately (instead of waiting the first
    // request).

    LOG(INFO) << "Asking one worker for the initial label statistics";
    ASSIGN_OR_RETURN(
        label_statistics,
        EmitGetLabelStatistics(distribute_manager.get(), monitoring));
    LOG(INFO) << "Training dataset label statistics:\n"
              << label_statistics.DebugString();

    ASSIGN_OR_RETURN(const auto initial_predictions,
                     loss->InitialPredictions(label_statistics));
    model->set_initial_predictions(initial_predictions);
    model->set_num_trees_per_iter(initial_predictions.size());

    RETURN_IF_ERROR(EmitSetInitialPredictions(
        label_statistics, distribute_manager.get(), monitoring));
  }

  // Name of the evaluated metrics.
  const auto metric_names = loss->SecondaryMetricNames();

  // The weak learners are predicting the loss's gradient.
  auto weak_learner_train_config = config;
  weak_learner_train_config.set_task(model::proto::Task::REGRESSION);

  ASSIGN_OR_RETURN(const auto set_leaf_functor,
                   loss->SetLeafFunctorFromLabelStatistics());

  Evaluation training_evaluation;
  auto time_last_checkpoint = absl::Now();

  LOG(INFO) << "Start training";
  for (; iter_idx < spe_config.gbt().num_trees(); iter_idx++) {
    // Create a checkpoint.
    if (iter_idx >= minimum_iter_for_new_checkpoint &&
        ShouldCreateCheckpoint(iter_idx, time_last_checkpoint, spe_config) &&
        (!last_checkpoint_idx.ok() || iter_idx > last_checkpoint_idx.value())) {
      time_last_checkpoint = absl::Now();
      last_checkpoint_idx = iter_idx;
      RETURN_IF_ERROR(CreateCheckpoint(iter_idx, *model, work_directory,
                                       label_statistics,
                                       distribute_manager.get(), monitoring));
    }

    const auto iter_status = RunIteration(
        iter_idx, config_link, spe_config, weak_learner_train_config,
        set_leaf_functor, feature_ownership, data_spec, metric_names,
        input_features, log_directory, model.get(), &training_evaluation,
        distribute_manager.get(), &random, monitoring);
    if (!iter_status.ok()) {
      LOG(WARNING) << "Iteration issue: " << iter_status.message();
    }

    if (absl::IsDataLoss(iter_status)) {
      // A worker was restarted and is missing data.
      LOG(WARNING) << "Re-synchronizing the workers";

      auto resync_iter_idx_status =
          utils::GetGreatestSnapshot(SnapshotDirectory(work_directory));
      if (!resync_iter_idx_status.ok()) {
        LOG(WARNING) << "No existing snapshot. Restart training from start.";
        // TODO(gbm): Restart training without rebooting the trainer.
      }
      auto resync_iter_idx = resync_iter_idx_status.value();

      iter_idx = resync_iter_idx;
      proto::Checkpoint checkpoint_metadata;
      RETURN_IF_ERROR(RestoreManagerCheckpoint(resync_iter_idx, work_directory,
                                               &model, &label_statistics,
                                               &checkpoint_metadata));
      model->set_data_spec(data_spec);
      InitializeModelWithAbstractTrainingConfig(config, config_link,
                                                model.get());
      RETURN_IF_ERROR(EmitRestoreCheckpoint(
          resync_iter_idx, checkpoint_metadata.num_shards(),
          model->num_trees_per_iter(), distribute_manager.get(), monitoring));

      minimum_iter_for_new_checkpoint = iter_idx + 1;
      // Restart this iteration.
      iter_idx--;
    } else if (!iter_status.ok()) {
      return iter_status;
    }
  }

  if (!last_checkpoint_idx.ok() || iter_idx > last_checkpoint_idx.value()) {
    // Create the final checkpoint
    RETURN_IF_ERROR(CreateCheckpoint(iter_idx, *model, work_directory,
                                     label_statistics, distribute_manager.get(),
                                     monitoring));
  }

  // Display the final training logs.
  LOG(INFO) << "Training done. Final model: "
            << TrainingLog(*model, training_evaluation, spe_config,
                           metric_names, monitoring);

  // Export training logs.
  if (!log_directory.empty()) {
    RETURN_IF_ERROR(gradient_boosted_trees::internal::ExportTrainingLogs(
        model->training_logs(), log_directory));
  }

  // Stop the workers.
  RETURN_IF_ERROR(distribute_manager->Done());
  return std::move(model);
}

absl::Status SkipAsyncAnswers(int num_skip,
                              distribute::AbstractManager* distribute_manager) {
  for (int i = 0; i < num_skip; i++) {
    RETURN_IF_ERROR(distribute_manager->NextAsynchronousAnswer().status());
  }
  return absl::OkStatus();
}

std::string TrainingLog(
    const gradient_boosted_trees::GradientBoostedTreesModel& model,
    const Evaluation& training_evaluation,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const std::vector<std::string>& metric_names,
    internal::Monitoring* monitoring) {
  auto log = absl::Substitute(
      "num-trees:$0/$1 train-loss:$2",
      model.decision_trees().size() / model.num_trees_per_iter(),
      spe_config.gbt().num_trees(), training_evaluation.loss);
  for (int metric_idx = 0; metric_idx < training_evaluation.metrics.size();
       metric_idx++) {
    absl::StrAppendFormat(&log, " train-%s:%f", metric_names[metric_idx],
                          training_evaluation.metrics[metric_idx]);
  }
  absl::StrAppend(&log, " ", monitoring->InlineLogs());
  return log;
}

absl::Status RunIteration(
    const int iter_idx, const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const model::proto::TrainingConfig& weak_learner_train_config,
    const decision_tree::SetLeafValueFromLabelStatsFunctor& set_leaf_functor,
    const FeatureOwnership& feature_ownership,
    const dataset::proto::DataSpecification& data_spec,
    const std::vector<std::string>& metric_names,
    const std::vector<int>& features, const absl::string_view& log_directory,
    gradient_boosted_trees::GradientBoostedTreesModel* model,
    Evaluation* training_evaluation,
    distribute::AbstractManager* distribute_manager, utils::RandomEngine* rnd,
    internal::Monitoring* monitoring) {
  monitoring->NewIter();
  ASSIGN_OR_RETURN(
      const auto weak_learner_label_statistics,
      EmitStartNewIter(iter_idx, random(), distribute_manager, monitoring));

  WeakModels weak_models(model->num_trees_per_iter());
  for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
       weak_model_idx++) {
    auto& weak_model = weak_models[weak_model_idx];
    ASSIGN_OR_RETURN(
        weak_model.tree_builder,
        distributed_decision_tree::TreeBuilder::Create(
            weak_learner_train_config, config_link,
            spe_config.gbt().decision_tree(),
            distributed_decision_tree::LabelAccessorType::kAutomatic,
            set_leaf_functor));

    RETURN_IF_ERROR(weak_model.tree_builder->SetRootValue(
        weak_learner_label_statistics[weak_model_idx]));
  }

  for (int layer_idx = 0;
       layer_idx < spe_config.gbt().decision_tree().max_depth() - 1;
       layer_idx++) {
    ASSIGN_OR_RETURN(
        const auto splits_per_weak_models,
        EmitFindSplits(spe_config, features, feature_ownership, weak_models,
                       distribute_manager, rnd, monitoring));

    // Check if there is at least one open node.
    bool has_open_node = false;
    for (const auto& split_per_node : splits_per_weak_models) {
      if (NumValidSplits(split_per_node) > 0) {
        has_open_node = true;
        break;
      }
    }
    if (!has_open_node) {
      break;
    }

    // Update the tree structure and update the label statistics.
    for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
         weak_model_idx++) {
      auto& weak_model = weak_models[weak_model_idx];
      RETURN_IF_ERROR(
          weak_model.tree_builder
              ->ApplySplitToTree(splits_per_weak_models[weak_model_idx])
              .status());
    }

    // Request for the workers to evaluate the splits.
    ASSIGN_OR_RETURN(
        const auto active_workers,
        EmitEvaluateSplits(splits_per_weak_models, feature_ownership,
                           distribute_manager, rnd, monitoring));

    // Request for the workers to share the evaluation results,
    // update the tree structures, example->node mapping and label
    // statistics
    RETURN_IF_ERROR(EmitShareSplits(splits_per_weak_models, active_workers,
                                    distribute_manager, monitoring));
  }

  RETURN_IF_ERROR(EmitEndIter(iter_idx, distribute_manager, training_evaluation,
                              monitoring));

  // Move the new trees in the model.
  for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
       weak_model_idx++) {
    auto& weak_model = weak_models[weak_model_idx];
    model->mutable_decision_trees()->push_back(
        absl::make_unique<decision_tree::DecisionTree>(
            std::move(*weak_model.tree_builder->mutable_tree())));
  }

  // TODO(gbm): Validation.
  // TODO(gbm): Early stopping.
  // TODO(gbm): Maximum training time.
  // TODO(gbm): Training interruption.

  // Display training logs.
  if (monitoring->ShouldDisplayLogs()) {
    LOG(INFO) << TrainingLog(*model, *training_evaluation, spe_config,
                             metric_names, monitoring);
  }

  // Record training logs.
  auto* log_entry = model->mutable_training_logs()->mutable_entries()->Add();
  log_entry->set_number_of_trees(iter_idx + 1);
  log_entry->set_training_loss(training_evaluation->loss);
  *log_entry->mutable_training_secondary_metrics() = {
      training_evaluation->metrics.begin(), training_evaluation->metrics.end()};
  log_entry->mutable_validation_secondary_metrics()->Resize(
      model->training_logs().secondary_metric_names_size(), 0);

  // Export training logs.
  if (!log_directory.empty() &&
      spe_config.gbt().export_logs_during_training_in_trees() > 0 &&
      ((iter_idx + 1) %
       spe_config.gbt().export_logs_during_training_in_trees()) == 0) {
    const auto begin = absl::Now();
    RETURN_IF_ERROR(gradient_boosted_trees::internal::ExportTrainingLogs(
        model->training_logs(), log_directory));
    LOG(INFO) << "Training logs exported in " << (absl::Now() - begin);
  }

  return absl::OkStatus();
}

utils::StatusOr<
    std::unique_ptr<gradient_boosted_trees::GradientBoostedTreesModel>>
InitializeModel(
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const dataset::proto::DataSpecification& data_spec,
    const gradient_boosted_trees::AbstractLoss& loss) {
  auto model =
      absl::make_unique<gradient_boosted_trees::GradientBoostedTreesModel>();
  model->set_data_spec(data_spec);
  model->set_loss(spe_config.gbt().loss());
  InitializeModelWithAbstractTrainingConfig(config, config_link, model.get());

  const auto secondary_metric_names = loss.SecondaryMetricNames();
  *model->mutable_training_logs()->mutable_secondary_metric_names() = {
      secondary_metric_names.begin(), secondary_metric_names.end()};

  if (model->task() == model::proto::Task::CLASSIFICATION &&
      !spe_config.gbt().apply_link_function()) {
    // The model output might not be a probability.
    model->set_classification_outputs_probabilities(false);
  }
  model->set_output_logits(!spe_config.gbt().apply_link_function());

  return std::move(model);
}

absl::Status InitializeDirectoryStructure(
    const absl::string_view work_directory) {
  // Create the directory structure.
  RETURN_IF_ERROR(file::RecursivelyCreateDir(work_directory, file::Defaults()));
  RETURN_IF_ERROR(file::RecursivelyCreateDir(
      file::JoinPath(work_directory, kFileNameCheckPoint, kFileNameSnapshot),
      file::Defaults()));
  RETURN_IF_ERROR(file::RecursivelyCreateDir(
      file::JoinPath(work_directory, kFileNameTmp), file::Defaults()));
  return absl::OkStatus();
}

absl::Status CreateCheckpoint(
    const int iter_idx,
    const gradient_boosted_trees::GradientBoostedTreesModel& model,
    const absl::string_view work_directory,
    const decision_tree::proto::LabelStatistics& label_statistics,
    distribute::AbstractManager* distribute_manager,
    internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kCreateCheckpoint);
  LOG(INFO) << "Start creating checkpoint for iteration " << iter_idx;
  const auto begin_create_checkpoint = absl::Now();

  proto::Checkpoint checkpoint;
  *checkpoint.mutable_label_statistics() = label_statistics;
  // Number of workers participating in the creation of the checkpoint.
  // A larger value reduces the cost of per worker, but increase the overhead
  // cost as well the chance to send a request to an interrupted worker.
  checkpoint.set_num_shards(std::max(1, distribute_manager->NumWorkers() / 4));

  const auto checkpoint_dir = file::JoinPath(
      work_directory, kFileNameCheckPoint, absl::StrCat(iter_idx));
  RETURN_IF_ERROR(file::RecursivelyCreateDir(checkpoint_dir, file::Defaults()));

  // Save the model structure.
  RETURN_IF_ERROR(model.Save(file::JoinPath(checkpoint_dir, "model")));

  // Save the worker-side checkpoint content.
  RETURN_IF_ERROR(EmitCreateCheckpoint(
      iter_idx, label_statistics.num_examples(), checkpoint.num_shards(),
      work_directory, distribute_manager, monitoring));

  RETURN_IF_ERROR(
      file::SetBinaryProto(file::JoinPath(checkpoint_dir, "checkpoint"),
                           checkpoint, file::Defaults()));

  // Record the snapshot.
  RETURN_IF_ERROR(
      utils::AddSnapshot(SnapshotDirectory(work_directory), iter_idx));

  LOG(INFO) << "Checkpoint created in " << absl::Now() - begin_create_checkpoint
            << " for iteration " << iter_idx;
  monitoring->EndStage(internal::Monitoring::kCreateCheckpoint);
  return absl::OkStatus();
}

absl::Status RestoreManagerCheckpoint(
    int iter_idx, absl::string_view work_directory,
    std::unique_ptr<gradient_boosted_trees::GradientBoostedTreesModel>* model,
    decision_tree::proto::LabelStatistics* label_statistics,
    proto::Checkpoint* checkpoint) {
  LOG(INFO) << "Restoring model from checkpoint at iteration " << iter_idx;
  const auto checkpoint_dir = file::JoinPath(
      work_directory, kFileNameCheckPoint, absl::StrCat(iter_idx));
  RETURN_IF_ERROR(
      file::GetBinaryProto(file::JoinPath(checkpoint_dir, "checkpoint"),
                           checkpoint, file::Defaults()));
  *label_statistics = checkpoint->label_statistics();
  *model =
      absl::make_unique<gradient_boosted_trees::GradientBoostedTreesModel>();
  RETURN_IF_ERROR(model->get()->Load(file::JoinPath(checkpoint_dir, "model")));
  return absl::OkStatus();
}

bool ShouldCreateCheckpoint(
    int iter_idx, const absl::Time& time_last_checkpoint,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config) {
  if (spe_config.checkpoint_interval_trees() >= 0 &&
      (iter_idx % spe_config.checkpoint_interval_trees()) == 0) {
    return true;
  }

  if (spe_config.checkpoint_interval_seconds() >= 0 &&
      (absl::Now() - time_last_checkpoint >=
       absl::Seconds(spe_config.checkpoint_interval_seconds()))) {
    return true;
  }

  return false;
}

utils::StatusOr<std::unique_ptr<distribute::AbstractManager>>
InitializeDistributionManager(
    const model::proto::DeploymentConfig& deployment,
    const model::proto::TrainingConfig& config,
    const model::proto::TrainingConfigLinking& config_link,
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    absl::string_view cache_path, absl::string_view work_directory,
    const dataset::proto::DataSpecification& data_spec,
    const FeatureOwnership& feature_ownership) {
  proto::WorkerWelcome welcome;
  welcome.set_work_directory(std::string(work_directory));
  welcome.set_cache_path(std::string(cache_path));
  *welcome.mutable_train_config() = config;
  *welcome.mutable_train_config_linking() = config_link;
  *welcome.mutable_deployment_config() = deployment;
  *welcome.mutable_dataspec() = data_spec;

  // Copy "feature_ownership" to "welcome.feature_ownership()".
  welcome.mutable_owned_features()->Reserve(
      feature_ownership.worker_to_feature.size());
  for (const auto& src : feature_ownership.worker_to_feature) {
    auto* dst = welcome.mutable_owned_features()->Add();
    *dst->mutable_features() = {src.begin(), src.end()};
  }

  return distribute::CreateManager(
      deployment.distribute(),
      /*worker_name=*/"DISTRIBUTED_GRADIENT_BOOSTED_TREES",
      /*welcome_blob=*/welcome.SerializeAsString(),
      // Number of evaluation split sharing at the same time.
      /*parallel_execution_per_worker=*/10);
}

utils::StatusOr<FeatureOwnership> AssignFeaturesToWorkers(
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const std::vector<int>& features, int num_workers,
    const distributed_decision_tree::dataset_cache::proto::CacheMetadata&
        cache_metadata) {
  FeatureOwnership ownership;

  int max_feature_idx = 0;
  for (const int feature : features) {
    max_feature_idx = std::max(feature, max_feature_idx);
  }

  ownership.worker_to_feature.resize(num_workers);
  ownership.feature_to_worker.resize(max_feature_idx + 1);

  // Assign all the features to all the workers.
  if (spe_config.internal().duplicate_computation_on_all_workers()) {
    LOG(WARNING) << "Assigning all the features to all the workers. This "
                    "option should only be used for debugging.";
    for (const auto feature : features) {
      ownership.feature_to_worker[feature].push_back(0);
      for (int worker_idx = 0; worker_idx < num_workers; worker_idx++) {
        ownership.worker_to_feature[worker_idx].push_back(feature);
      }
    }
    return ownership;
  }

  // Score of each feature.
  // The score is: boolean < categorical==discretized numerical < numerical.
  std::vector<std::pair<int64_t, int>> feature_and_scores;
  feature_and_scores.reserve(features.size());
  for (const auto feature : features) {
    const auto col_metadata = cache_metadata.columns(feature);
    int64_t score;
    switch (col_metadata.type_case()) {
      case CacheMetadata_Column::kNumerical:
        if (col_metadata.numerical().discretized()) {
          score =
              col_metadata.numerical().num_discretized_values() + (1ll << 32);
        } else {
          score = col_metadata.numerical().num_unique_values() + (2ll << 32);
        }
        break;
      case CacheMetadata_Column::kCategorical:
        score = col_metadata.categorical().num_values() + (1ll << 32);
        break;
      case CacheMetadata_Column::kBoolean:
        score = 0;
        break;
      case CacheMetadata_Column::TYPE_NOT_SET:
        break;
    }
    feature_and_scores.push_back({score, feature});
  }
  std::sort(feature_and_scores.begin(), feature_and_scores.end(),
            std::greater<>());

  int cur = 0;
  for (const auto feature_and_score : feature_and_scores) {
    const auto worker_idx = (cur++) % num_workers;
    ownership.worker_to_feature[worker_idx].push_back(feature_and_score.second);
    ownership.feature_to_worker[feature_and_score.second].push_back(worker_idx);
  }

  return ownership;
}

utils::StatusOr<decision_tree::proto::LabelStatistics> EmitGetLabelStatistics(
    distribute::AbstractManager* distribute, internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kGetLabelStatistics);
  proto::WorkerRequest generic_request;
  // The request has not payload.
  generic_request.mutable_get_label_statistics();

  // Select one worker at random.
  ASSIGN_OR_RETURN(
      auto generic_answer,
      distribute->BlockingProtoRequest<proto::WorkerResult>(generic_request));
  if (!generic_answer.has_get_label_statistics()) {
    return absl::InternalError(
        "Unexpected answer. Expecting GetLabelStatistics");
  }
  monitoring->EndStage(internal::Monitoring::kGetLabelStatistics);
  return std::move(*(generic_answer.mutable_get_label_statistics()
                         ->mutable_label_statistics()));
}

absl::Status EmitSetInitialPredictions(
    const decision_tree::proto::LabelStatistics& label_statistics,
    distribute::AbstractManager* distribute, internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kSetInitialPredictions);
  proto::WorkerRequest generic_request;
  auto& request = *generic_request.mutable_set_initial_predictions();
  *request.mutable_label_statistics() = label_statistics;

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (!generic_result.has_set_initial_predictions()) {
      return absl::InternalError(
          "Unexpected answer. Expecting SetInitialPredictions");
    }
  }
  monitoring->EndStage(internal::Monitoring::kSetInitialPredictions);
  return absl::OkStatus();
}

utils::StatusOr<std::vector<decision_tree::proto::LabelStatistics>>
EmitStartNewIter(const int iter_idx,
                 const utils::RandomEngine::result_type seed,
                 distribute::AbstractManager* distribute,
                 internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kStartNewIter);
  std::vector<decision_tree::proto::LabelStatistics> root_label_statistics;

  proto::WorkerRequest generic_request;
  auto& request = *generic_request.mutable_start_new_iter();
  request.set_iter_idx(iter_idx);
  request.set_iter_uid(utils::GenUniqueId());
  request.set_seed(seed);

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());

    if (generic_result.request_restart_iter()) {
      RETURN_IF_ERROR(SkipAsyncAnswers(distribute->NumWorkers() - reply_idx - 1,
                                       distribute));
      return absl::DataLossError("");
    }
    if (!generic_result.has_start_new_iter()) {
      return absl::InternalError("Unexpected answer. Expecting StartNewIter");
    }
    const auto& result = generic_result.start_new_iter();

    if (root_label_statistics.empty()) {
      root_label_statistics = {result.label_statistics().begin(),
                               result.label_statistics().end()};
    }
  }
  monitoring->EndStage(internal::Monitoring::kStartNewIter);
  return root_label_statistics;
}

utils::StatusOr<std::vector<distributed_decision_tree::SplitPerOpenNode>>
EmitFindSplits(
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const std::vector<int>& features, const FeatureOwnership& feature_ownership,
    const WeakModels& weak_models, distribute::AbstractManager* distribute,
    utils::RandomEngine* rnd, internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kFindSplits);
  const auto begin = absl::Now();

  FeaturesPerWorkerWeakModelAndNode sampled_features;
  RETURN_IF_ERROR(SampleInputFeatures(spe_config, distribute->NumWorkers(),
                                      features, feature_ownership, weak_models,
                                      &sampled_features, rnd));

  // Send the requests.
  int num_requests = 0;
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    proto::WorkerRequest generic_request;
    auto& request = *generic_request.mutable_find_splits();

    int num_selected_features;
    RETURN_IF_ERROR(ExactSampledFeaturesForWorker(
        sampled_features, worker_idx, &request, &num_selected_features));

    // TODO(gbm): Only ask for splits is num_selected_features>0. Note: The
    // worker's code for FindSplit is responsible to clear the local split
    // evaluation.

    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
    num_requests++;
  }

  // Allocates the merged split objects.
  std::vector<distributed_decision_tree::SplitPerOpenNode>
      splits_per_weak_models(weak_models.size());
  for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
       weak_model_idx++) {
    splits_per_weak_models[weak_model_idx].resize(
        weak_models[weak_model_idx].tree_builder->num_open_nodes());
  }
  // Parse the replies.
  for (int reply_idx = 0; reply_idx < num_requests; reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (generic_result.request_restart_iter()) {
      RETURN_IF_ERROR(SkipAsyncAnswers(distribute->NumWorkers() - reply_idx - 1,
                                       distribute));
      return absl::DataLossError("");
    }
    monitoring->FindSplitWorkerReplyTime(generic_result.worker_idx(),
                                         absl::Now() - begin);
    if (!generic_result.has_find_splits()) {
      return absl::InternalError("Unexpected answer. Expecting FindSplits");
    }
    const auto& result = generic_result.find_splits();
    if (result.split_per_weak_model_size() != weak_models.size()) {
      return absl::InternalError("Unexpected number of weak model splits");
    }

    for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
         weak_model_idx++) {
      distributed_decision_tree::SplitPerOpenNode worker_splits;
      ConvertFromProto(result.split_per_weak_model(weak_model_idx),
                       &worker_splits);

      RETURN_IF_ERROR(distributed_decision_tree::MergeBestSplits(
          worker_splits, &splits_per_weak_models[weak_model_idx]));
    }
  }

  monitoring->EndStage(internal::Monitoring::kFindSplits);
  return splits_per_weak_models;
}

utils::StatusOr<WorkerIdxs> EmitEvaluateSplits(
    const std::vector<distributed_decision_tree::SplitPerOpenNode>&
        splits_per_weak_models,
    const FeatureOwnership& feature_ownership,
    distribute::AbstractManager* distribute, utils::RandomEngine* rnd,
    internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kEvaluateSplits);

  // Mapping worker_idx -> weak_model_idx -> split_dx.
  ASSIGN_OR_RETURN(
      const auto active_workers,
      BuildActiveWorkers(splits_per_weak_models, feature_ownership, rnd));

  WorkerIdxs active_worker_idxs;
  active_worker_idxs.reserve(active_workers.size());
  for (const auto& active_worker : active_workers) {
    active_worker_idxs.push_back(active_worker.first);
  }

  // Emit the requests.
  for (const auto& active_worker : active_workers) {
    proto::WorkerRequest generic_request;
    auto& request = *generic_request.mutable_evaluate_splits();
    for (int weak_model_idx = 0; weak_model_idx < splits_per_weak_models.size();
         weak_model_idx++) {
      auto* dst_splits = request.add_split_per_weak_model();
      const auto& active_split_idxs = active_worker.second[weak_model_idx];
      distributed_decision_tree::ConvertToProto(
          splits_per_weak_models[weak_model_idx], active_split_idxs,
          dst_splits);
    }
    RETURN_IF_ERROR(distribute->AsynchronousProtoRequest(generic_request,
                                                         active_worker.first));
  }

  for (int reply_idx = 0; reply_idx < active_workers.size(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (generic_result.request_restart_iter()) {
      RETURN_IF_ERROR(
          SkipAsyncAnswers(active_workers.size() - reply_idx - 1, distribute));
      return absl::DataLossError("");
    }
    if (!generic_result.has_evaluate_splits()) {
      return absl::InternalError("Unexpected answer. Expecting EvaluateSplits");
    }
  }

  monitoring->EndStage(internal::Monitoring::kEvaluateSplits);
  return active_worker_idxs;
}

absl::Status EmitShareSplits(
    const std::vector<distributed_decision_tree::SplitPerOpenNode>&
        splits_per_weak_models,
    const WorkerIdxs& active_workers, distribute::AbstractManager* distribute,
    internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kShareSplits);

  proto::WorkerRequest generic_request;
  auto& request = *generic_request.mutable_share_splits();
  for (const auto& splits : splits_per_weak_models) {
    distributed_decision_tree::ConvertToProto(
        splits, request.add_split_per_weak_model());
  }
  *request.mutable_active_workers() = {active_workers.begin(),
                                       active_workers.end()};

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (generic_result.request_restart_iter()) {
      RETURN_IF_ERROR(SkipAsyncAnswers(distribute->NumWorkers() - reply_idx - 1,
                                       distribute));
      return absl::DataLossError("Worker requested to restart the iteration.");
    }
    if (!generic_result.has_share_splits()) {
      return absl::InternalError("Unexpected answer. Expecting ShareSplits");
    }
  }

  monitoring->EndStage(internal::Monitoring::kShareSplits);
  return absl::OkStatus();
}

absl::Status EmitEndIter(int iter_idx, distribute::AbstractManager* distribute,
                         absl::optional<Evaluation*> training_evaluation,
                         internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kEndIter);

  proto::WorkerRequest generic_request;
  auto& request = *generic_request.mutable_end_iter();
  request.set_iter_idx(iter_idx);

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    if (training_evaluation.has_value()) {
      // The first worker is in charge of computing the training loss.
      request.set_compute_training_loss(worker_idx == 0);
    }

    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (generic_result.request_restart_iter()) {
      RETURN_IF_ERROR(SkipAsyncAnswers(distribute->NumWorkers() - reply_idx - 1,
                                       distribute));
      return absl::DataLossError("");
    }
    if (!generic_result.has_end_iter()) {
      return absl::InternalError("Unexpected answer. Expecting EndIter");
    }

    // Get the loss value.
    const auto& result = generic_result.end_iter();
    if (result.has_training_loss()) {
      if (!training_evaluation.has_value()) {
        return absl::InternalError("Receiving a non requested loss");
      }

      training_evaluation.value()->loss = result.training_loss();
      training_evaluation.value()->metrics = {result.training_metrics().begin(),
                                              result.training_metrics().end()};
    }
  }

  monitoring->EndStage(internal::Monitoring::kEndIter);
  return absl::OkStatus();
}

absl::Status EmitRestoreCheckpoint(const int iter_idx, const int num_shards,
                                   const int num_weak_models,
                                   distribute::AbstractManager* distribute,
                                   internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kRestoreCheckpoint);

  proto::WorkerRequest generic_request;
  auto& request = *generic_request.mutable_restore_checkpoint();
  request.set_iter_idx(iter_idx);
  request.set_num_shards(num_shards);
  request.set_num_weak_models(num_weak_models);

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (!generic_result.has_restore_checkpoint()) {
      return absl::InternalError(
          "Unexpected answer. Expecting RestoreManagerCheckpoint. Got " +
          generic_result.DebugString());
    }
  }
  monitoring->EndStage(internal::Monitoring::kRestoreCheckpoint);
  return absl::OkStatus();
}

absl::Status EmitCreateCheckpoint(const int iter_idx, const size_t num_examples,
                                  const int num_shards,
                                  const absl::string_view work_directory,
                                  distribute::AbstractManager* distribute,
                                  internal::Monitoring* monitoring) {
  const int max_retries = 3 * num_shards;
  int retries = 0;

  // Examples contained in the "shard_idx" shard of a checkpoint.
  const auto shard_idx_to_example_idx_range =
      [&](const int shard_idx) -> std::pair<size_t, size_t> {
    const auto num_example_per_shard =
        (num_examples + num_shards - 1) / num_shards;
    return {shard_idx * num_example_per_shard,
            std::min(num_examples, (shard_idx + 1) * num_example_per_shard)};
  };

  for (int shard_idx = 0; shard_idx < num_shards; shard_idx++) {
    proto::WorkerRequest generic_request;
    auto& request = *generic_request.mutable_create_checkpoint();
    const auto example_range = shard_idx_to_example_idx_range(shard_idx);
    request.set_begin_example_idx(example_range.first);
    request.set_end_example_idx(example_range.second);
    request.set_shard_idx(shard_idx);
    generic_request.set_request_id(shard_idx);
    RETURN_IF_ERROR(distribute->AsynchronousProtoRequest(generic_request));
  }

  const auto checkpoint_dir = file::JoinPath(
      work_directory, kFileNameCheckPoint, absl::StrCat(iter_idx));

  for (int answer_idx = 0; answer_idx < num_shards; answer_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());

    if (generic_result.request_restart_iter()) {
      const int new_worker_idx =
          (generic_result.worker_idx() + 1) % distribute->NumWorkers();
      // The worker was restarted and it misses the data required to create the
      // checkpoint. Re-send the request to another worker.
      LOG(WARNING) << "Worker #" << generic_result.worker_idx()
                   << " does not have the right data to create the "
                      "checkpoint. Trying worker #"
                   << new_worker_idx << " instead";

      retries++;
      if (retries > max_retries) {
        return absl::DataLossError(
            absl::Substitute("Impossible to create a checkpoint for iter #$0 "
                             "because none of the workers are available.",
                             iter_idx));
      }

      // Send the request to another worker.
      proto::WorkerRequest generic_request;
      auto& request = *generic_request.mutable_create_checkpoint();
      const auto example_range = shard_idx_to_example_idx_range(
          generic_result.create_checkpoint().shard_idx());
      request.set_begin_example_idx(example_range.first);
      request.set_end_example_idx(example_range.second);
      request.set_shard_idx(generic_result.request_id());
      generic_request.set_request_id(generic_result.request_id());
      RETURN_IF_ERROR(distribute->AsynchronousProtoRequest(generic_request,
                                                           new_worker_idx));
      answer_idx--;
      continue;
    }

    if (!generic_result.has_create_checkpoint()) {
      return absl::InternalError(
          "Unexpected answer. Expecting CreateCheckpoint");
    }
    const auto& result = generic_result.create_checkpoint();
    RETURN_IF_ERROR(file::Rename(
        result.path(),
        file::JoinPath(checkpoint_dir,
                       distributed_decision_tree::dataset_cache::ShardFilename(
                           "predictions", result.shard_idx(), num_shards)),
        file::Defaults()));
  }
  return absl::OkStatus();
}

absl::Status EmitStartTraining(distribute::AbstractManager* distribute,
                               internal::Monitoring* monitoring) {
  monitoring->BeginStage(internal::Monitoring::kStartTraining);
  const auto begin = absl::Now();

  proto::WorkerRequest generic_request;
  generic_request.mutable_start_training();

  // TODO(gbm): Implement multicast operations.
  for (int worker_idx = 0; worker_idx < distribute->NumWorkers();
       worker_idx++) {
    RETURN_IF_ERROR(
        distribute->AsynchronousProtoRequest(generic_request, worker_idx));
  }

  // TODO(gbm): No need for an answer.
  for (int reply_idx = 0; reply_idx < distribute->NumWorkers(); reply_idx++) {
    ASSIGN_OR_RETURN(
        const auto generic_result,
        distribute->NextAsynchronousProtoAnswer<proto::WorkerResult>());
    if (!generic_result.has_start_training()) {
      return absl::InternalError(
          "Unexpected answer. Expecting StartTraining. Got " +
          generic_result.DebugString());
    }
    // Most of the time is used for the workers to load the dataset.
    LOG_INFO_EVERY_N_SEC(60, _ << "\tLoading dataset in workers "
                               << (reply_idx + 1) << " / "
                               << distribute->NumWorkers()
                               << " [duration: " << absl::Now() - begin << "]");
  }
  LOG(INFO) << "Worker ready to train in " << absl::Now() - begin;

  monitoring->EndStage(internal::Monitoring::kStartTraining);
  return absl::OkStatus();
}

absl::Status SampleInputFeatures(
    const proto::DistributedGradientBoostedTreesTrainingConfig& spe_config,
    const int num_workers, const std::vector<int>& features,
    const FeatureOwnership& feature_ownership, const WeakModels& weak_models,
    FeaturesPerWorkerWeakModelAndNode* samples, utils::RandomEngine* rnd) {
  const auto& dt_config = spe_config.gbt().decision_tree();

  // How many features to select for each split.
  int num_sampled_features = features.size();
  if (dt_config.has_num_candidate_attributes() &&
      dt_config.num_candidate_attributes() > 0) {
    // Note: Default behavior (num_candidate_attributes=0) is to select all the
    // features.
    num_sampled_features = dt_config.num_candidate_attributes();
  } else if (dt_config.has_num_candidate_attributes_ratio() &&
             dt_config.num_candidate_attributes_ratio() > 0) {
    num_sampled_features = static_cast<int>(std::ceil(
        dt_config.num_candidate_attributes_ratio() * features.size()));
  }

  // Allocate output structure.
  samples->resize(num_workers);
  for (auto& per_worker : *samples) {
    per_worker.resize(weak_models.size());
    for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
         weak_model_idx++) {
      const auto& weak_model = weak_models[weak_model_idx];
      auto& per_weak_model = per_worker[weak_model_idx];
      per_weak_model.resize(weak_model.tree_builder->num_open_nodes());
    }
  }

  // Sample for each weak learner and open node.
  std::vector<int> sampled_features;
  for (int weak_model_idx = 0; weak_model_idx < weak_models.size();
       weak_model_idx++) {
    const auto& weak_model = weak_models[weak_model_idx];
    for (int node_idx = 0; node_idx < weak_model.tree_builder->num_open_nodes();
         node_idx++) {
      // Sample
      RETURN_IF_ERROR(SampleFeatures(features, num_sampled_features,
                                     &sampled_features, rnd));

      // Export the sample for each worker.
      for (const auto feature : sampled_features) {
        if (spe_config.internal().duplicate_computation_on_all_workers()) {
          for (int worker_idx = 0; worker_idx < num_workers; worker_idx++) {
            (*samples)[worker_idx][weak_model_idx][node_idx].push_back(feature);
          }
        } else {
          ASSIGN_OR_RETURN(const int worker_idx,
                           SelectOwnerWorker(feature_ownership, feature, rnd));
          (*samples)[worker_idx][weak_model_idx][node_idx].push_back(feature);
        }
      }
    }
  }

  return absl::OkStatus();
}

absl::Status SampleFeatures(const std::vector<int>& features,
                            int num_sampled_features,
                            std::vector<int>* sampled_features,
                            utils::RandomEngine* rnd) {
  DCHECK_GE(num_sampled_features, 0);
  if (num_sampled_features > features.size()) {
    return absl::InternalError(
        absl::Substitute("Cannot sample $0 features from $1",
                         num_sampled_features, features.size()));
  } else if (num_sampled_features == features.size()) {
    *sampled_features = features;
    return absl::OkStatus();
  }

  // TODO(gbm): Use std::sample when available.
  *sampled_features = features;
  std::shuffle(sampled_features->begin(), sampled_features->end(), *rnd);
  sampled_features->resize(num_sampled_features);

  return absl::OkStatus();
}

utils::StatusOr<int> SelectOwnerWorker(
    const FeatureOwnership& feature_ownership, int feature,
    utils::RandomEngine* rnd) {
  const auto& candidate_workers = feature_ownership.feature_to_worker[feature];
  if (candidate_workers.empty()) {
    return absl::InternalError("No owning worker for feature");
  } else if (candidate_workers.size() == 1) {
    return candidate_workers.front();
  } else {
    return candidate_workers[std::uniform_int_distribution<int>(
        0, candidate_workers.size() - 1)(*rnd)];
  }
}

absl::Status ExactSampledFeaturesForWorker(
    const FeaturesPerWorkerWeakModelAndNode& sampled_features,
    const int worker_idx, proto::WorkerRequest::FindSplits* request,
    int* num_selected_features) {
  *num_selected_features = 0;
  const auto& src_per_weak_model = sampled_features[worker_idx];
  request->mutable_features_per_weak_models()->Clear();
  request->mutable_features_per_weak_models()->Reserve(
      src_per_weak_model.size());

  // TODO: implement if internal.duplicate.

  for (int weak_model_idx = 0; weak_model_idx < src_per_weak_model.size();
       weak_model_idx++) {
    const auto& src_per_node = src_per_weak_model[weak_model_idx];
    auto* request_features_per_node =
        request->add_features_per_weak_models()->mutable_features_per_node();
    request_features_per_node->Reserve(src_per_node.size());
    for (int node_idx = 0; node_idx < src_per_node.size(); node_idx++) {
      const auto& features = src_per_node[node_idx];
      *request_features_per_node->Add()->mutable_features() = {features.begin(),
                                                               features.end()};
      *num_selected_features += features.size();
    }
  }
  return absl::OkStatus();
}

utils::StatusOr<ActiveWorkerMap> BuildActiveWorkers(
    const std::vector<distributed_decision_tree::SplitPerOpenNode>&
        splits_per_weak_models,
    const FeatureOwnership& feature_ownership, utils::RandomEngine* rnd) {
  absl::flat_hash_map<int, std::vector<std::vector<int>>> active_workers;

  for (int weak_model_idx = 0; weak_model_idx < splits_per_weak_models.size();
       weak_model_idx++) {
    const auto& splits = splits_per_weak_models[weak_model_idx];
    for (int split_idx = 0; split_idx < splits.size(); split_idx++) {
      const auto& split = splits[split_idx];
      if (!IsSplitValid(split)) {
        continue;
      }
      ASSIGN_OR_RETURN(const auto worker_idx,
                       SelectOwnerWorker(feature_ownership,
                                         split.condition.attribute(), rnd));
      auto& worker_eval_splits = active_workers[worker_idx];
      if (worker_eval_splits.empty()) {
        worker_eval_splits.assign(splits_per_weak_models.size(), {});
      }
      worker_eval_splits[weak_model_idx].push_back(split_idx);
    }
  }
  return active_workers;
}

void Monitoring::BeginTraining() {}

void Monitoring::BeginDatasetCacheCreation() {}

bool Monitoring::ShouldDisplayLogs() {
  const auto now = absl::Now();
  if (!logs_already_displayed_) {
    logs_already_displayed_ = true;
    last_display_logs_ = now;
    return true;
  }
  if (now - last_display_logs_ >= absl::Seconds(30)) {
    last_display_logs_ = now;
    return true;
  }
  return false;
}

void Monitoring::BeginStage(Monitoring::Stages stage) {
  if (current_stage_ != -1) {
    LOG(WARNING) << "Starting stage " << StageName(stage)
                 << " before the previous stage "
                 << StageName(static_cast<Monitoring::Stages>(current_stage_))
                 << " was marked as completed.";
    return;
  }
  current_stage_ = stage;
  begin_current_stage_ = absl::Now();

  if (verbose_) {
    LOG(INFO) << "Starting stage " << StageName(stage);
  }
}

void Monitoring::EndStage(Monitoring::Stages stage) {
  DCHECK_GE(current_stage_, 0);
  if (current_stage_ < 0) {
    LOG(WARNING) << "Invalid BeginStage > EndStage. stage=" << stage;
    return;
  }
  const auto duration_current_stage = absl::Now() - begin_current_stage_;
  stage_stats_[stage].count++;
  stage_stats_[stage].sum_duration += duration_current_stage;

  if (stage == kFindSplits && !last_min_split_reply_times_.empty()) {
    std::sort(last_min_split_reply_times_.begin(),
              last_min_split_reply_times_.end(),
              [](const auto& a, const auto b) { return a.second < b.second; });
    const auto median =
        last_min_split_reply_times_[last_min_split_reply_times_.size() / 2]
            .second;

    last_min_split_reply_time_ = last_min_split_reply_times_.front().second;
    last_max_split_reply_time_ = last_min_split_reply_times_.back().second;
    last_fastest_worker_idx_ = last_min_split_reply_times_.front().first;
    last_slowest_worker_idx_ = last_min_split_reply_times_.back().first;

    sum_min_split_reply_time_ += last_min_split_reply_times_.front().second;
    sum_max_split_reply_time_ += last_min_split_reply_times_.back().second;

    sum_median_split_reply_time_ += median;
    last_median_split_reply_time_ = median;

    last_min_split_reply_times_.clear();
    count_reply_times_++;
  }

  if (verbose_) {
    LOG(INFO) << "Finishing stage " << StageName(stage) << " in "
              << duration_current_stage;
  }
  current_stage_ = -1;
}

void Monitoring::NewIter() {
  if (num_iters_ == 0) {
    time_first_iter_ = absl::Now();
  }
  num_iters_++;
}

void Monitoring::FindSplitWorkerReplyTime(int worker_idx,
                                          absl::Duration delay) {
  if (verbose_) {
    LOG(INFO) << "\tWorker #" << worker_idx << " replied to FindSplits in "
              << delay;
  }
  last_min_split_reply_times_.push_back({worker_idx, delay});
}

absl::string_view Monitoring::StageName(Monitoring::Stages stage) {
  switch (stage) {
    case Monitoring::kGetLabelStatistics:
      return "GetLabelStatistics";
    case Monitoring::kSetInitialPredictions:
      return "SetInitialPredictions";
    case Monitoring::kStartNewIter:
      return "StartNewIter";
    case Monitoring::kFindSplits:
      return "FindSplits";
    case Monitoring::kEvaluateSplits:
      return "EvaluateSplits";
    case Monitoring::kShareSplits:
      return "ShareSplits";
    case Monitoring::kEndIter:
      return "EndIter";
    case Monitoring::kRestoreCheckpoint:
      return "RestoreCheckpoint";
    case Monitoring::kCreateCheckpoint:
      return "CreateCheckpoint";
    case Monitoring::kStartTraining:
      return "StartTraining";
    case Monitoring::kNumStages:
      return "NumStages";
  }
  return "UNKNOWN";
}

std::string Monitoring::InlineLogs() {
  std::string logs;
  if (num_iters_ > 0) {
    const auto time_per_iters = (absl::Now() - time_first_iter_) / num_iters_;
    absl::SubstituteAndAppend(&logs, "time-per-iter:$0",
                              FormatDuration(time_per_iters));
  }
  absl::SubstituteAndAppend(&logs, " last-{min,median,max}-split-time:$0 $1 $2",
                            FormatDuration(last_min_split_reply_time_),
                            FormatDuration(last_median_split_reply_time_),
                            FormatDuration(last_max_split_reply_time_));
  absl::SubstituteAndAppend(&logs, " last-{slowest,fastest}-worker:$0 $1",
                            last_slowest_worker_idx_, last_fastest_worker_idx_);

  if (count_reply_times_ > 0) {
    absl::SubstituteAndAppend(
        &logs, " mean-{min,median,max}-split-time:$0 $1 $2",
        FormatDuration(sum_min_split_reply_time_ / count_reply_times_),
        FormatDuration(sum_median_split_reply_time_ / count_reply_times_),
        FormatDuration(sum_max_split_reply_time_ / count_reply_times_));
  }

  for (int stage_idx = 0; stage_idx < kNumStages; stage_idx++) {
    const auto& stage_stat = stage_stats_[stage_idx];
    if (stage_stat.count > 0) {
      absl::SubstituteAndAppend(
          &logs, "\n\t\t$0: avg:$1 count:$2",
          Monitoring::StageName(static_cast<Stages>(stage_idx)),
          FormatDuration(stage_stat.sum_duration / stage_stat.count),
          stage_stat.count);
    }
  }

  return logs;
}

}  // namespace internal
}  // namespace distributed_gradient_boosted_trees
}  // namespace model
}  // namespace yggdrasil_decision_forests