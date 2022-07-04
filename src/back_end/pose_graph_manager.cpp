#include "cslam/back_end/pose_graph_manager.h"

using namespace cslam;

PoseGraphManager::PoseGraphManager(std::shared_ptr<rclcpp::Node> &node): node_(node), max_waiting_time_sec_(60,0) {

  node_->get_parameter("nb_robots", nb_robots_);
  node_->get_parameter("robot_id", robot_id_);
  node_->get_parameter("pose_graph_manager_process_period_ms", pose_graph_manager_process_period_ms_);
  node_->get_parameter("pose_graph_optimization_loop_period_ms", pose_graph_optimization_loop_period_ms_);

  int max_waiting_param;
  node_->get_parameter("max_waiting_time_sec", max_waiting_param);
  max_waiting_time_sec_ = rclcpp::Duration(max_waiting_param, 0);

  odometry_subscriber_ = node->create_subscription<
        cslam_common_interfaces::msg::KeyframeOdom>(
        "keyframe_odom", 100,
        std::bind(&PoseGraphManager::odometry_callback, this,
                    std::placeholders::_1));

  inter_robot_loop_closure_subscriber_ = node->create_subscription<
        cslam_loop_detection_interfaces::msg::
                                      InterRobotLoopClosure>(
        "/inter_robot_loop_closure", 100,
        std::bind(&PoseGraphManager::inter_robot_loop_closure_callback, this,
                    std::placeholders::_1));

  rotation_default_noise_std_ = 0.01;
  translation_default_noise_std_ = 0.1;
  Eigen::VectorXd sigmas(6);
  sigmas << rotation_default_noise_std_, rotation_default_noise_std_, rotation_default_noise_std_, 
            translation_default_noise_std_, translation_default_noise_std_, translation_default_noise_std_;
  default_noise_model_ = gtsam::noiseModel::Diagonal::Sigmas(sigmas);
  pose_graph_ = boost::make_shared<gtsam::NonlinearFactorGraph>();
  current_pose_estimates_ = boost::make_shared<gtsam::Values>();

  // Optimization timers
  optimization_timer_ = node_->create_wall_timer(
        std::chrono::milliseconds(pose_graph_manager_process_period_ms_), std::bind(&PoseGraphManager::optimization_callback, this));

  optimization_loop_timer_ = node_->create_wall_timer(
        std::chrono::milliseconds(pose_graph_optimization_loop_period_ms_), std::bind(&PoseGraphManager::optimization_loop_callback, this));

  // Publisher for optimization result
  optimization_result_publisher_ =
      node_->create_publisher<cslam_common_interfaces::msg::OptimizationResult>(
          "optimization_result", 100);
          
  optimizer_state_publisher_ =
      node_->create_publisher<cslam_common_interfaces::msg::OptimizerState>(
          "optimizer_state", 100);

  // Initialize inter-robot loop closures measurements
  for (unsigned int i = 0; i < nb_robots_; i++)
  {
    for (unsigned int j = i + 1; j < nb_robots_; j++)
    {
      inter_robot_loop_closures_.insert({{i, j}, std::vector<gtsam::BetweenFactor<gtsam::Pose3>>()});
    }
  }

  // Get neighbors ROS 2 objects
  get_current_neighbors_publisher_ =
    node->create_publisher<std_msgs::msg::String>("get_current_neighbors", 100);

  current_neighbors_subscriber_ = node->create_subscription<
        cslam_common_interfaces::msg::
                                      RobotIds>(
        "current_neighbors", 100,
        std::bind(&PoseGraphManager::current_neighbors_callback, this,
                    std::placeholders::_1));
  
  // PoseGraph ROS 2 objects
  for (unsigned int i = 0; i < nb_robots_; i++)
  {
    get_pose_graph_publishers_.insert({i,
      node->create_publisher<cslam_common_interfaces::msg::RobotIds>("/r" + std::to_string(i) + "/get_pose_graph", 100)});
    received_pose_graphs_.insert({i, false});
  }

  get_pose_graph_subscriber_ = node->create_subscription<
        cslam_common_interfaces::msg::RobotIds>(
        "get_pose_graph", 100,
        std::bind(&PoseGraphManager::get_pose_graph_callback, this,
                    std::placeholders::_1));

  pose_graph_publisher_ =
    node->create_publisher<cslam_common_interfaces::msg::PoseGraph>("/pose_graph", 100);

  pose_graph_subscriber_ = node->create_subscription<
        cslam_common_interfaces::msg::PoseGraph>(
        "/pose_graph", 100,
        std::bind(&PoseGraphManager::pose_graph_callback, this,
                    std::placeholders::_1));

  // Optimizer
  optimizer_state_ = OptimizerState::IDLE;
  is_waiting_ = false;

  RCLCPP_INFO(node_->get_logger(), "Initialization done.");
}

void PoseGraphManager::reinitialize_received_pose_graphs(){
  for (unsigned int i = 0; i < nb_robots_; i ++)
  {
    received_pose_graphs_[i] = false;
  }
  other_robots_graph_and_estimates_.clear();
  received_pose_graphs_connectivity_.clear();
}

bool PoseGraphManager::check_received_pose_graphs(){
  bool received_all = true;
  for (auto id : current_neighbors_ids_.ids)
  {
    received_all &= received_pose_graphs_[id];
  }
  return received_all;
}

void PoseGraphManager::odometry_callback(const cslam_common_interfaces::msg::KeyframeOdom::ConstSharedPtr msg) {

  gtsam::Pose3 current_estimate = odometry_msg_to_pose3(msg->odom);
  gtsam::LabeledSymbol symbol(GRAPH_LABEL, ROBOT_LABEL(robot_id_), msg->id);
  current_pose_estimates_->insert(symbol, current_estimate);

  if (latest_local_symbol_ != gtsam::LabeledSymbol())
  {
    gtsam::Pose3 odom_diff = current_estimate * latest_local_pose_.inverse();
    gtsam::BetweenFactor<gtsam::Pose3> factor(latest_local_symbol_, symbol, odom_diff, default_noise_model_);
    pose_graph_->push_back(factor);
  }

  // Update latest pose
  latest_local_pose_ = current_estimate;
  latest_local_symbol_ = symbol;
}

void PoseGraphManager::inter_robot_loop_closure_callback(const cslam_loop_detection_interfaces::msg::
  InterRobotLoopClosure::ConstSharedPtr msg) {
    if (msg->success){
      gtsam::Pose3 measurement = transform_msg_to_pose3(msg->transform);

      unsigned char robot0_c = ROBOT_LABEL(msg->robot0_id);
      gtsam::LabeledSymbol symbol_from(GRAPH_LABEL, robot0_c, msg->robot0_image_id);
      unsigned char robot1_c = ROBOT_LABEL(msg->robot1_id);
      gtsam::LabeledSymbol symbol_to(GRAPH_LABEL, robot1_c, msg->robot1_image_id);

      gtsam::BetweenFactor<gtsam::Pose3> factor = gtsam::BetweenFactor<gtsam::Pose3>(symbol_from, symbol_to, measurement, default_noise_model_);
      
      inter_robot_loop_closures_[{std::min(msg->robot0_id, msg->robot1_id), std::max(msg->robot0_id, msg->robot1_id)}].push_back(factor);
    }
  }

void PoseGraphManager::current_neighbors_callback(const cslam_common_interfaces::msg::
                                      RobotIds::ConstSharedPtr msg)
{
  current_neighbors_ids_ = *msg;
  optimizer_state_ = OptimizerState::POSEGRAPH_COLLECTION;
  end_waiting();
}

void PoseGraphManager::get_pose_graph_callback(const cslam_common_interfaces::msg::
                                      RobotIds::ConstSharedPtr msg)
{
  cslam_common_interfaces::msg::PoseGraph out_msg;
  out_msg.robot_id = robot_id_;
  out_msg.values = gtsam_values_to_msg(current_pose_estimates_);
  auto graph = boost::make_shared<gtsam::NonlinearFactorGraph>();
  graph->push_back(pose_graph_->begin(), pose_graph_->end());

  std::set<unsigned int> connected_robots;

  for (unsigned int i = 0; i < msg->ids.size(); i++)
  {
    for (unsigned int j = i+1; j < msg->ids.size(); j++)
    {
      unsigned int min_robot_id = std::min(msg->ids[i], msg->ids[j]);
      unsigned int max_robot_id = std::max(msg->ids[i], msg->ids[j]);
      if (inter_robot_loop_closures_[{min_robot_id, max_robot_id}].size() > 0)
      {
          connected_robots.insert(max_robot_id);
          if (min_robot_id == robot_id_)
          {
            graph->push_back(inter_robot_loop_closures_[{min_robot_id, max_robot_id}].begin(), inter_robot_loop_closures_[{min_robot_id, max_robot_id}].end());
          }
      }
    }
  }

  out_msg.edges = gtsam_factors_to_msg(graph);
  for (auto id : connected_robots)
  {
    out_msg.connected_robots.ids.push_back(id);
  }
  pose_graph_publisher_->publish(out_msg);
}

void PoseGraphManager::pose_graph_callback(const cslam_common_interfaces::msg::
                                      PoseGraph::ConstSharedPtr msg)
{
  other_robots_graph_and_estimates_.insert({msg->robot_id, {edges_msg_to_gtsam(msg->edges), values_msg_to_gtsam(msg->values)}});
  received_pose_graphs_[msg->robot_id] = true;
  received_pose_graphs_connectivity_.insert({msg->robot_id, msg->connected_robots.ids});
  if (check_received_pose_graphs())
  {
    end_waiting();
    optimizer_state_ = OptimizerState::OPTIMIZATION;
  }
}

std::map<unsigned int, bool> PoseGraphManager::connected_robot_pose_graph()
{
  std::map<unsigned int, bool> is_robot_connected;
  is_robot_connected.insert({robot_id_, true});
  for (auto id : current_neighbors_ids_.ids) {
    is_robot_connected.insert({id, false});
  }

  // Breadth First Search 
  bool *visited = new bool[current_neighbors_ids_.ids.size()];
  for (unsigned int i = 0; i < current_neighbors_ids_.ids.size(); i++)
      visited[i] = false;

  std::list<unsigned int> queue;

  unsigned int current_id = robot_id_;
  visited[current_id] = true;
  queue.push_back(current_id);

  while (!queue.empty())
  {
      current_id = queue.front();
      queue.pop_front();

      for (auto id : received_pose_graphs_connectivity_[current_id])
      {
          is_robot_connected[id] = true;

          if (!visited[id])
          {
              visited[id] = true;
              queue.push_back(id);
          }
      }
  }
  return is_robot_connected;
}

void PoseGraphManager::resquest_current_neighbors(){
  get_current_neighbors_publisher_->publish(std_msgs::msg::String());  
}

void PoseGraphManager::start_waiting(){
  optimizer_state_ = OptimizerState::WAITING;
  is_waiting_ = true;
  start_waiting_time_ = node_->now();
}

void PoseGraphManager::end_waiting(){
  is_waiting_ = false;
}

bool PoseGraphManager::check_waiting_timeout(){
  if ((node_->now() - start_waiting_time_) > max_waiting_time_sec_)
  {
    end_waiting();
    optimizer_state_ = OptimizerState::IDLE;
  }
  return is_waiting_;
}

void PoseGraphManager::optimization_callback(){
  if (optimizer_state_ == OptimizerState::IDLE){
      reinitialize_received_pose_graphs();
      resquest_current_neighbors();
      start_waiting();
  }
}

std::pair<gtsam::NonlinearFactorGraph::shared_ptr, gtsam::Values::shared_ptr> PoseGraphManager::aggregate_pose_graphs(){
  // Check connectivity
  auto is_pose_graph_connected = connected_robot_pose_graph();
  // Aggregate graphs
  auto graph = boost::make_shared<gtsam::NonlinearFactorGraph>();
  auto estimates = boost::make_shared<gtsam::Values>();
  // Local graph
  graph->push_back(pose_graph_->begin(), pose_graph_->end());
  auto included_robots_ids = current_neighbors_ids_;
  included_robots_ids.ids.push_back(robot_id_);
  for (unsigned int i = 0; i < included_robots_ids.ids.size(); i++)
  {
    for (unsigned int j = i + 1; j < included_robots_ids.ids.size(); j++)
    {
      if (is_pose_graph_connected[included_robots_ids.ids[i]] && is_pose_graph_connected[included_robots_ids.ids[j]])
      {
        unsigned int min_id = std::min(included_robots_ids.ids[i], included_robots_ids.ids[j]);
        unsigned int max_id = std::max(included_robots_ids.ids[i], included_robots_ids.ids[j]);
        for (const auto &factor: inter_robot_loop_closures_[{min_id, max_id}])
        {
          graph->push_back(factor);
        }
      }
    }
  }
  estimates->insert(*current_pose_estimates_);
  // Add other robots graphs
  for (auto id : current_neighbors_ids_.ids)
  { 
    if (is_pose_graph_connected[id])
    {
      estimates->insert(*other_robots_graph_and_estimates_[id].second);
    }
  }
  for (auto id : current_neighbors_ids_.ids)
  {
    for (const auto &factor_: *other_robots_graph_and_estimates_[id].first)
    {
      auto factor = boost::dynamic_pointer_cast<gtsam::BetweenFactor<gtsam::Pose3>>(factor_);
      unsigned int robot0_id = ROBOT_ID(gtsam::LabeledSymbol(factor->key1()).label());
      unsigned int robot1_id = ROBOT_ID(gtsam::LabeledSymbol(factor->key2()).label());
      if (is_pose_graph_connected[robot0_id] && is_pose_graph_connected[robot1_id])
      {
        graph->push_back(factor);
      }
    }
  }
  return {graph, estimates};
}

void PoseGraphManager::perform_optimization(){
  
  // Build global pose graph
  auto graph_and_estimates = aggregate_pose_graphs();

  // Add prior 
  gtsam::LabeledSymbol first_symbol(GRAPH_LABEL, ROBOT_LABEL(robot_id_), 0);
  graph_and_estimates.first->addPrior(first_symbol, gtsam::Pose3(), default_noise_model_);

  // // Optimize graph
  gtsam::GncParams<gtsam::LevenbergMarquardtParams> params;
  gtsam::GncOptimizer<gtsam::GncParams<gtsam::LevenbergMarquardtParams>> optimizer(*graph_and_estimates.first, *graph_and_estimates.second, params);
  gtsam::Values result = optimizer.optimize();

  // // TODO: Share results

  // // TODO: print result
  // // TODO: publish a TF

  // // Publish result info for monitoring
  cslam_common_interfaces::msg::OptimizationResult msg;
  msg.success = true;
  msg.factors = gtsam_factors_to_msg(graph_and_estimates.first);// TODO: Do not fill, unless debugging mode
  msg.estimates = gtsam_values_to_msg(result);// TODO: Do not fill, unless debugging mode
  optimization_result_publisher_->publish(msg); // TODO: publish on debug mode
}

void PoseGraphManager::optimization_loop_callback(){
  if (!current_pose_estimates_->empty())
  {
    if (optimizer_state_ == OptimizerState::POSEGRAPH_COLLECTION) // TODO: Document
    {
      if (current_neighbors_ids_.ids.size() > 0)
      {
        for (auto id : current_neighbors_ids_.ids)
        {
          auto current_robots_ids = current_neighbors_ids_;
          current_robots_ids.ids.push_back(robot_id_);
          get_pose_graph_publishers_[id]->publish(current_robots_ids);
        }
        start_waiting();
      }
      else
      {
        optimizer_state_ = OptimizerState::IDLE;
      }
    }
    else if (optimizer_state_ == OptimizerState::OPTIMIZATION)
    {
      // Call optimization
      perform_optimization();
      // TODO: Send updates
      optimizer_state_ = OptimizerState::IDLE;
    }
    else if (optimizer_state_ == OptimizerState::WAITING)
    {
      check_waiting_timeout();
    }
  }
  cslam_common_interfaces::msg::OptimizerState state_msg;
  state_msg.state = optimizer_state_;
  optimizer_state_publisher_->publish(state_msg);
}