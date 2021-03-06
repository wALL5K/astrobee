/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <config_reader/config_reader.h>
#include <graph_localizer/graph_localizer_wrapper.h>
#include <graph_localizer/parameter_reader.h>
#include <graph_localizer/utilities.h>
#include <imu_integration/utilities.h>
#include <localization_common/logger.h>
#include <localization_common/utilities.h>
#include <localization_measurements/measurement_conversions.h>
#include <msg_conversions/msg_conversions.h>

#include <Eigen/Core>

namespace graph_localizer {
namespace lc = localization_common;
namespace lm = localization_measurements;
namespace mc = msg_conversions;

GraphLocalizerWrapper::GraphLocalizerWrapper(const std::string& graph_config_path_prefix) : reset_world_T_dock_(false) {
  config_reader::ConfigReader config;
  lc::LoadGraphLocalizerConfig(config, graph_config_path_prefix);
  config.AddFile("transforms.config");
  config.AddFile("cameras.config");
  config.AddFile("geometry.config");

  if (!config.ReadFiles()) {
    LogFatal("Failed to read config files.");
  }

  if (!config.GetBool("publish_localization_graph", &publish_localization_graph_)) {
    LogFatal("Failed to load publish_localization_graph.");
  }

  if (!config.GetBool("save_localization_graph_dot_file", &save_localization_graph_dot_file_)) {
    LogFatal("Failed to load save_localization_graph_dot_file.");
  }

  position_cov_log_det_lost_threshold_ = mc::LoadDouble(config, "position_cov_log_det_lost_threshold");
  orientation_cov_log_det_lost_threshold_ = mc::LoadDouble(config, "orientation_cov_log_det_lost_threshold");

  graph_localizer_initializer_.LoadGraphLocalizerParams(config);
  SanityCheckerParams sanity_checker_params;
  LoadSanityCheckerParams(config, sanity_checker_params);
  sanity_checker_.reset(new SanityChecker(sanity_checker_params));
  // Initialize with config.  Optionally update during localization
  // TODO(rsoussan): Make graph localizer wrapper config file and load fcn in parameter reader
  estimated_world_T_dock_ = lc::LoadTransform(config, "world_dock_transform");
  estimate_world_T_dock_using_loc_ = mc::LoadBool(config, "estimate_world_T_dock_using_loc");
  sparse_mapping_min_num_landmarks_ = mc::LoadInt(config, "loc_adder_min_num_matches");
  ar_min_num_landmarks_ = mc::LoadInt(config, "ar_tag_loc_adder_min_num_matches");
}

bool GraphLocalizerWrapper::Initialized() const { return (graph_localizer_.get() != nullptr); }

void GraphLocalizerWrapper::Update() {
  if (graph_localizer_) {
    graph_localizer_->Update();
    // Sanity check covariances after updates
    if (!CheckCovarianceSanity()) {
      LogError("OpticalFlowCallback: Covariance sanity check failed, resetting localizer.");
      ResetLocalizer();
      return;
    }
  }
}

void GraphLocalizerWrapper::OpticalFlowCallback(const ff_msgs::Feature2dArray& feature_array_msg) {
  if (graph_localizer_) {
    if (graph_localizer_->AddOpticalFlowMeasurement(lm::MakeFeaturePointsMeasurement(feature_array_msg))) {
      feature_counts_.of = graph_localizer_->NumOFFactors();
    }
  }
}

void GraphLocalizerWrapper::ResetLocalizer() {
  LogInfo("ResetLocalizer: Resetting localizer.");
  graph_localizer_initializer_.ResetStartPose();
  if (!latest_biases_) {
    LogError(
      "ResetLocalizer: Trying to reset localizer when no biases "
      "are available.");
    return;
  }
  // TODO(rsoussan): compare current time with latest bias timestamp and print
  // warning if it is too old
  graph_localizer_initializer_.SetBiases(latest_biases_->first, true);
  graph_localizer_.reset();
  sanity_checker_->Reset();
}

void GraphLocalizerWrapper::ResetBiasesAndLocalizer() {
  LogInfo("ResetBiasAndLocalizer: Resetting biases and localizer.");
  graph_localizer_initializer_.ResetBiasesAndStartPose();
  graph_localizer_.reset();
  sanity_checker_->Reset();
}

void GraphLocalizerWrapper::ResetBiasesFromFileAndResetLocalizer() {
  LogInfo("ResetBiasAndLocalizer: Resetting biases from file and resetting localizer.");
  graph_localizer_initializer_.ResetBiasesFromFileAndResetStartPose();
  graph_localizer_.reset();
  sanity_checker_->Reset();
}

void GraphLocalizerWrapper::VLVisualLandmarksCallback(const ff_msgs::VisualLandmarks& visual_landmarks_msg) {
  if (!ValidVLMsg(visual_landmarks_msg, sparse_mapping_min_num_landmarks_)) return;
  if (graph_localizer_) {
    graph_localizer_->AddSparseMappingMeasurement(lm::MakeMatchedProjectionsMeasurement(visual_landmarks_msg));
    feature_counts_.vl = visual_landmarks_msg.landmarks.size();
  }

  const gtsam::Pose3 sparse_mapping_global_T_body =
    lc::GtPose(visual_landmarks_msg, graph_localizer_initializer_.params().calibration.body_T_nav_cam.inverse());
  const lc::Time timestamp = lc::TimeFromHeader(visual_landmarks_msg.header);
  sparse_mapping_pose_ = std::make_pair(sparse_mapping_global_T_body, timestamp);

  // Sanity Check
  if (graph_localizer_ && !CheckPoseSanity(sparse_mapping_global_T_body, timestamp)) {
    LogError("VLVisualLandmarksCallback: Sanity check failed, resetting localizer.");
    ResetLocalizer();
    return;
  }

  if (!graph_localizer_) {
    // Set or update initial pose if a new one is available before the localizer
    // has started running.
    graph_localizer_initializer_.SetStartPose(sparse_mapping_pose_->first, sparse_mapping_pose_->second);
  }
}

bool GraphLocalizerWrapper::CheckPoseSanity(const gtsam::Pose3& sparse_mapping_pose, const lc::Time timestamp) const {
  if (!graph_localizer_) return true;
  const auto latest_extrapolated_pose_time = graph_localizer_->LatestExtrapolatedPoseTime();
  if (!latest_extrapolated_pose_time) {
    LogDebug("CheckPoseSanity: Failed to get latest extrapolated pose time.");
    return true;
  }
  if (timestamp > *latest_extrapolated_pose_time) {
    LogDebug("CheckPoseSanity: Timestamp occurs after latest extrapolated pose time");
    return true;
  }
  const auto combined_nav_state = graph_localizer_->GetCombinedNavState(timestamp);
  if (!combined_nav_state) {
    LogDebugEveryN(50, "CheckPoseSanity: Failed to get combined nav state.");
    return true;
  }
  return sanity_checker_->CheckPoseSanity(sparse_mapping_pose, combined_nav_state->pose());
}

bool GraphLocalizerWrapper::CheckCovarianceSanity() const {
  if (!graph_localizer_) return true;
  const auto combined_nav_state_and_covariances = graph_localizer_->LatestCombinedNavStateAndCovariances();
  if (!combined_nav_state_and_covariances) {
    LogDebugEveryN(50, "CheckCovarianceSanity: No combined nav state and covariances available.");
    return true;
  }

  return sanity_checker_->CheckCovarianceSanity(combined_nav_state_and_covariances->second);
}

void GraphLocalizerWrapper::ARVisualLandmarksCallback(const ff_msgs::VisualLandmarks& visual_landmarks_msg) {
  if (!ValidVLMsg(visual_landmarks_msg, ar_min_num_landmarks_)) return;
  if (graph_localizer_) {
    if (reset_world_T_dock_) {
      ResetWorldTDockUsingLoc(visual_landmarks_msg);
      reset_world_T_dock_ = false;
    }
    const auto frame_changed_ar_measurements = lm::FrameChangeMatchedProjectionsMeasurement(
      lm::MakeMatchedProjectionsMeasurement(visual_landmarks_msg), estimated_world_T_dock_);
    graph_localizer_->AddARTagMeasurement(frame_changed_ar_measurements);
    ar_tag_pose_ = std::make_pair(frame_changed_ar_measurements.global_T_cam *
                                    graph_localizer_initializer_.params().calibration.body_T_dock_cam.inverse(),
                                  frame_changed_ar_measurements.timestamp);
    // TODO(rsoussan): Make seperate ar count, update GraphState and EkfState
    feature_counts_.vl = visual_landmarks_msg.landmarks.size();
  }
}

void GraphLocalizerWrapper::ImuCallback(const sensor_msgs::Imu& imu_msg) {
  if (graph_localizer_) {
    graph_localizer_->AddImuMeasurement(lm::ImuMeasurement(imu_msg));
    latest_biases_ = graph_localizer_->LatestBiases();
    if (!latest_biases_) {
      LogError("ImuCallback: Failed to get latest biases.");
    }
  } else if (graph_localizer_initializer_.EstimateBiases()) {
    graph_localizer_initializer_.EstimateAndSetImuBiases(lm::ImuMeasurement(imu_msg));
  }

  if (!graph_localizer_ && graph_localizer_initializer_.ReadyToInitialize()) {
    InitializeGraph();
    LogDebug("ImuCallback: Initialized Graph.");
  }
}

void GraphLocalizerWrapper::InitializeGraph() {
  if (!graph_localizer_initializer_.ReadyToInitialize()) {
    LogDebug("InitializeGraph: Trying to initialize graph when not ready.");
    return;
  }

  graph_localizer_.reset(new graph_localizer::GraphLocalizer(graph_localizer_initializer_.params()));
}

boost::optional<const FeatureTrackMap&> GraphLocalizerWrapper::feature_tracks() const {
  if (!graph_localizer_) return boost::none;
  return graph_localizer_->feature_tracks();
}

void GraphLocalizerWrapper::MarkWorldTDockForResettingIfNecessary() {
  if (estimate_world_T_dock_using_loc_) reset_world_T_dock_ = true;
}

void GraphLocalizerWrapper::ResetWorldTDockUsingLoc(const ff_msgs::VisualLandmarks& visual_landmarks_msg) {
  const auto latest_combined_nav_state = LatestCombinedNavState();
  if (!latest_combined_nav_state) {
    LogError("ResetWorldTDockIfNecessary: Failed to get latest combined nav state.");
    return;
  }
  // TODO(rsoussan): Extrapolate latest world_T_body loc estimate with imu data?
  const gtsam::Pose3 dock_T_body =
    lc::GtPose(visual_landmarks_msg, graph_localizer_initializer_.params().calibration.body_T_dock_cam.inverse());
  estimated_world_T_dock_ = latest_combined_nav_state->pose() * dock_T_body.inverse();
}

gtsam::Pose3 GraphLocalizerWrapper::estimated_world_T_dock() const { return estimated_world_T_dock_; }

boost::optional<geometry_msgs::PoseStamped> GraphLocalizerWrapper::LatestSparseMappingPoseMsg() const {
  if (!sparse_mapping_pose_) {
    LogWarningEveryN(50, "LatestSparseMappingPoseMsg: Failed to get latest sparse mapping pose msg.");
    return boost::none;
  }

  return PoseMsg(sparse_mapping_pose_->first, sparse_mapping_pose_->second);
}

boost::optional<geometry_msgs::PoseStamped> GraphLocalizerWrapper::LatestARTagPoseMsg() const {
  if (!ar_tag_pose_) {
    LogWarningEveryN(50, "LatestARTagPoseMsg: Failed to get latest ar tag pose msg.");
    return boost::none;
  }

  return PoseMsg(ar_tag_pose_->first, ar_tag_pose_->second);
}

boost::optional<lc::CombinedNavState> GraphLocalizerWrapper::LatestCombinedNavState() const {
  if (!graph_localizer_) {
    LogWarningEveryN(50, "LatestCombinedNavState: Graph localizater not initialized yet.");
    return boost::none;
  }
  const auto latest_combined_nav_state = graph_localizer_->LatestCombinedNavState();
  if (!latest_combined_nav_state) {
    LogWarningEveryN(50, "LatestCombinedNavState: No combined nav state available.");
    return boost::none;
  }
  return latest_combined_nav_state;
}

boost::optional<ff_msgs::GraphState> GraphLocalizerWrapper::LatestLocalizationStateMsg() {
  if (!graph_localizer_) {
    LogDebugEveryN(50, "LatestLocalizationMsg: Graph localizater not initialized yet.");
    return boost::none;
  }
  const auto combined_nav_state_and_covariances = graph_localizer_->LatestCombinedNavStateAndCovariances();
  if (!combined_nav_state_and_covariances) {
    LogDebugEveryN(50, "LatestLocalizationMsg: No combined nav state and covariances available.");
    return boost::none;
  }
  const auto graph_state_msg = GraphStateMsg(
    combined_nav_state_and_covariances->first, combined_nav_state_and_covariances->second, feature_counts_.of,
    feature_counts_.vl, graph_localizer_initializer_.EstimateBiases(), position_cov_log_det_lost_threshold_,
    orientation_cov_log_det_lost_threshold_, graph_localizer_->standstill(), graph_localizer_->graph_stats());
  feature_counts_.Reset();
  return graph_state_msg;
}

boost::optional<ff_msgs::LocalizationGraph> GraphLocalizerWrapper::LatestLocalizationGraphMsg() const {
  if (!graph_localizer_) {
    LogWarningEveryN(50, "LatestGraphMsg: Graph localizater not initialized yet.");
    return boost::none;
  }
  return GraphMsg(*graph_localizer_);
}

void GraphLocalizerWrapper::SaveLocalizationGraphDotFile() const {
  if (graph_localizer_) graph_localizer_->SaveGraphDotFile();
}

boost::optional<const GraphStats&> GraphLocalizerWrapper::graph_stats() const {
  if (!graph_localizer_) {
    LogDebug("GraphStats: Failed to get graph stats.");
    return boost::none;
  }
  return graph_localizer_->graph_stats();
}

bool GraphLocalizerWrapper::publish_localization_graph() const { return publish_localization_graph_; }

bool GraphLocalizerWrapper::save_localization_graph_dot_file() const { return save_localization_graph_dot_file_; }

}  // namespace graph_localizer
