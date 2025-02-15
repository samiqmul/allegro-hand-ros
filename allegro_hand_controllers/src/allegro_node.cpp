// Common allegro node code used by any node. Each node that implements an
// AllegroNode must define the computeDesiredTorque() method.
//
// Author: Felix Duvallet <felix.duvallet@epfl.ch>

#include "allegro_node.h"
#include "allegro_hand_driver/controlAllegroHand.h"


std::string jointNames[DOF_JOINTS] =
        {
                "joint_0", "joint_1", "joint_2", "joint_3",
                "joint_4", "joint_5", "joint_6", "joint_7",
                "joint_8", "joint_9", "joint_10", "joint_11",
                "joint_12", "joint_13", "joint_14", "joint_15"
        };


AllegroNode::AllegroNode(bool sim /* = false */) {
  mutex = new boost::mutex();

  // Create arrays 16 long for each of the four joint state components
  current_joint_state.position.resize(DOF_JOINTS);
  current_joint_state.velocity.resize(DOF_JOINTS);
  current_joint_state.effort.resize(DOF_JOINTS);
  current_joint_state.name.resize(DOF_JOINTS);

  // Initialize values: joint names should match URDF, desired torque and
  // velocity are both zero.
  for (int i = 0; i < DOF_JOINTS; i++) {
    current_joint_state.name[i] = jointNames[i];
    desired_torque[i] = 0.0;
    current_velocity[i] = 0.0;
    current_position_filtered[i] = 0.0;
    current_velocity_filtered[i] = 0.0;
  }

  // Get Allegro Hand information from parameter server
  // This information is found in the Hand-specific "zero.yaml" file from the allegro_hand_description package
  std::string robot_name, manufacturer, origin, serial;
  double version;
  ros::param::get("~hand_info/robot_name", robot_name);
  ros::param::get("~hand_info/which_hand", whichHand);
  ros::param::get("~hand_info/manufacturer", manufacturer);
  ros::param::get("~hand_info/origin", origin);
  ros::param::get("~hand_info/serial", serial);
  ros::param::get("~hand_info/version", version);

  // Initialize CAN device
  if(!sim) {
    canDevice = new controlAllegroHand();
    canDevice->init();
    usleep(3000);
    updateWriteReadCAN();
  }

  // Start ROS time
  tstart = ros::Time::now();

  // Advertise current joint state publisher and subscribe to desired joint
  // states.
  joint_state_pub = nh.advertise<sensor_msgs::JointState>(JOINT_STATE_TOPIC, 3);
  joint_cmd_sub_ = nh.subscribe(DESIRED_STATE_TOPIC, 1, // queue size
                                &AllegroNode::desiredStateCallback, this);
}

AllegroNode::~AllegroNode() {
  delete canDevice;
  delete mutex;
  nh.shutdown();
}

void AllegroNode::desiredStateCallback(const sensor_msgs::JointState &msg) {
  mutex->lock();
  desired_joint_state_ = msg;
  mutex->unlock();
}

void AllegroNode::publishData() {
  // current position, velocity and effort (torque) published
  current_joint_state.header.stamp = tnow;
  for (int i = 0; i < DOF_JOINTS; i++) {
    current_joint_state.position[i] = current_position_filtered[i];
    current_joint_state.velocity[i] = current_velocity_filtered[i];
    current_joint_state.effort[i] = desired_torque[i];
  }
  joint_state_pub.publish(current_joint_state);
}

void AllegroNode::updateWriteReadCAN() {
  // CAN bus communication.
  canDevice->setTorque(desired_torque);
  lEmergencyStop = canDevice->Update();
  canDevice->getJointInfo(current_position);

  if (lEmergencyStop < 0) {
    // Ignore zero command error
    // ROS_ERROR("Unknown CAN message is ignored!");
    // Stop program when Allegro Hand is switched off
    // ROS_ERROR("Allegro Hand Node is Shutting Down! (Emergency Stop)");
    // ros::shutdown();
  }
}

void AllegroNode::updateController() {
  // Calculate loop time;
  tnow = ros::Time::now();
  dt = 1e-9 * (tnow - tstart).nsec;

  // When running gazebo, sometimes the loop gets called *too* often and dt will
  // be zero. Ensure nothing bad (like divide-by-zero) happens because of this.
  if(dt <= 0) {
    ROS_DEBUG_STREAM_THROTTLE(1, "AllegroNode::updateController dt is zero.");
    return;
  }

  tstart = tnow;

  // save last iteration info
  for (int i = 0; i < DOF_JOINTS; i++) {
    previous_position[i] = current_position[i];
    previous_position_filtered[i] = current_position_filtered[i];
    previous_velocity[i] = current_velocity[i];
  }

  updateWriteReadCAN();

  // Low-pass filtering.
  for (int i = 0; i < DOF_JOINTS; i++) {
    current_position_filtered[i] = (0.6 * current_position_filtered[i]) +
                                   (0.198 * previous_position[i]) +
                                   (0.198 * current_position[i]);
    current_velocity[i] =
            (current_position_filtered[i] - previous_position_filtered[i]) / dt;
    current_velocity_filtered[i] = (0.6 * current_velocity_filtered[i]) +
                                   (0.198 * previous_velocity[i]) +
                                   (0.198 * current_velocity[i]);
    current_velocity[i] = (current_position[i] - previous_position[i]) / dt;
  }

  computeDesiredTorque();

  publishData();

  frame++;
}


// Interrupt-based control is not recommended by SimLab. I have not tested it.
void AllegroNode::timerCallback(const ros::TimerEvent &event) {
  updateController();
}

ros::Timer AllegroNode::startTimerCallback() {
  ros::Timer timer = nh.createTimer(ros::Duration(0.001),
                                    &AllegroNode::timerCallback, this);
  return timer;
}
