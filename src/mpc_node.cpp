#include "ros/ros.h"
#include "geometry_msgs/TwistStamped.h"
#include "std_msgs/Bool.h"
#include "std_msgs/String.h"
#include "marine_msgs/Helm.h"
#include "marine_msgs/NavEulerStamped.h"
#include <vector>
#include "project11/gz4d_geo.h"
#include <path_planner_common/UpdateReferenceTrajectory.h>
#include <fstream>
#include <geometry_msgs/PoseStamped.h>
#include "controller.h"
#include <path_planner_common/DubinsPlan.h>
#include <mpc/mpcConfig.h>
#include <dynamic_reconfigure/server.h>
#include <geographic_msgs/GeoPoint.h>
#include <path_planner_common/TrajectoryDisplayerHelper.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma ide diagnostic ignored "OCInconsistentNamingInspection"
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"

/**
 * ROS node which manages a model-predictive controller.
 */
class MPCNode: public ControlReceiver
{
public:
    /**
     * Construct a MPCNode, setting up all the ROS publishers, subscribers and services, and initialize a Controller
     * instance.
     */
    explicit MPCNode()
    {
        m_helm_pub = m_node_handle.advertise<marine_msgs::Helm>("/helm",1);
        m_display_pub = m_node_handle.advertise<geographic_visualization_msgs::GeoVizItem>("/project11/display",1);

        m_controller_msgs_sub = m_node_handle.subscribe("/controller_msgs", 10, &MPCNode::controllerMsgsCallback, this);
        m_position_sub = m_node_handle.subscribe("/position_map", 10, &MPCNode::positionCallback, this);
        m_heading_sub = m_node_handle.subscribe("/heading", 10, &MPCNode::headingCallback, this);
        m_speed_sub = m_node_handle.subscribe("/sog", 10, &MPCNode::speedCallback, this);

        m_update_reference_trajectory_service = m_node_handle.advertiseService("/mpc/update_reference_trajectory", &MPCNode::updateReferenceTrajectory, this);

        m_Controller = new Controller(this);

        dynamic_reconfigure::Server<mpc::mpcConfig>::CallbackType f;
        f = boost::bind(&MPCNode::reconfigureCallback, this, _1, _2);

        m_Dynamic_Reconfigure_Server.setCallback(f);

        m_TrajectoryDisplayer = TrajectoryDisplayerHelper(m_node_handle, &m_display_pub);
    }

    /**
     * Tell the Controller instance to terminate before freeing it.
     */
    ~MPCNode() final
    {
        m_Controller->terminate(); // make sure its thread can exit
        delete m_Controller;
    }

    /**
     * Process a message for the controller.
     * @param inmsg the string message
     */
    void controllerMsgsCallback(const std_msgs::String::ConstPtr &inmsg)
    {
        std::string message = inmsg->data;
        std::cerr << "MPC node received message to: " << message << std::endl;
        if (message == "start running") {
            /* Deprecated */
        } else if (message == "start sending controls") {
            /* Deprecated */
        } else if (message == "terminate") {
            m_Controller->terminate();
        } else if (message == "stop sending controls") {
            /* Deprecated */
        }
    }

    /**
     * Update the controller's idea of the vehicle's heading.
     * @param inmsg a message containing the new heading
     */
    void headingCallback(const marine_msgs::NavEulerStamped::ConstPtr& inmsg)
    {
        m_current_heading = inmsg->orientation.heading * M_PI / 180.0;
    }

    /**
     * Update the controller's idea of the vehicle's speed.
     * @param inmsg a message containing the new speed
     */
    void speedCallback(const geometry_msgs::TwistStamped::ConstPtr& inmsg)
    {
        m_current_speed = inmsg->twist.linear.x; // this will change once /sog is a vector
    }

    /**
     * Update the controller's idea of the vehicle's position.
     * @param inmsg a message containing the new position
     */
    void positionCallback(const geometry_msgs::PoseStamped::ConstPtr &inmsg)
    {
        m_Controller->updatePosition(State(
                inmsg->pose.position.x,
                inmsg->pose.position.y,
                m_current_heading,
                m_current_speed,
                getTime()));
    }

    void reconfigureCallback(mpc::mpcConfig &config, uint32_t level)
    {
        m_Controller->updateConfig(
                config.rudders, config.throttles,
                config.distance_weight, config.heading_weight, config.speed_weight,
                config.achievable_threshold);
    }

    /**
     * Publish a rudder and throttle to /helm.
     * @param rudder rudder command
     * @param throttle throttle command
     */
    void receiveControl(double rudder, double throttle) final
    {
        marine_msgs::Helm helm;
        helm.rudder = rudder;
        helm.throttle = throttle;
        helm.header.stamp = ros::Time::now();
        m_helm_pub.publish(helm);
    }

    /**
     * Display a predicted trajectory to /project11/display.
     * @param trajectory
     * @param plannerTrajectory
     */
    void displayTrajectory(const std::vector<State>& trajectory, bool plannerTrajectory, bool achievable) final
    {
        m_TrajectoryDisplayer.displayTrajectory(trajectory, plannerTrajectory, achievable);
    }

    /**
     * Fulful the service by calling MPC once and returning a state 1s into the future. The controller's reference
     * trajectory is updated (obviously) and MPC is set to run for some time.
     * @param req
     * @param res
     * @return
     */
    bool updateReferenceTrajectory(path_planner_common::UpdateReferenceTrajectory::Request &req, path_planner_common::UpdateReferenceTrajectory::Response &res) {
//        std::cerr << "Controller received reference trajectory of length: " << req.trajectory.states.size() << std::endl;
//        std::vector<State> states;
//        for (const auto &s : req.plan.paths) {
//            states.push_back(getState(s)); // TODO -- not done with this yet just changed to see if it worked
//        }
        auto s = m_Controller->updateReferenceTrajectory(getPlan(req.plan), m_TrajectoryNumber++);
        res.state = getStateMsg(s);
        return s.time() != -1;
    }

    static DubinsPlan getPlan(path_planner_common::Plan plan) {
        DubinsPlan result;
        for (const auto& d : plan.paths) {
            DubinsWrapper wrapper;
            DubinsPath path;
            path.qi[0] = d.x;
            path.qi[1] = d.y;
            path.qi[2] = d.yaw;
            path.param[0] = d.param0;
            path.param[1] = d.param1;
            path.param[2] = d.param2;
            path.rho = d.rho;
            path.type = (DubinsPathType)d.type;
            wrapper.fill(path, d.speed, d.start_time);
            if (wrapper.getEndTime() > plan.endtime){
                wrapper.updateEndTime(plan.endtime);
            }
            result.append(wrapper);
        }
        return result;
    }


    /**
     * @return the current time in seconds
     */
    double getTime() const override {
        return m_TrajectoryDisplayer.getTime();
    }

    path_planner_common::StateMsg getStateMsg(const State& state) {
        return m_TrajectoryDisplayer.getStateMsg(state);
    }

    State getState(const path_planner_common::StateMsg& stateMsg) {
        return m_TrajectoryDisplayer.getState(stateMsg);
    }

    geographic_msgs::GeoPoint convertToLatLong(const State& state) {
        return m_TrajectoryDisplayer.convertToLatLong(state);
    }

private:
    ros::NodeHandle m_node_handle;

    TrajectoryDisplayerHelper m_TrajectoryDisplayer;

    double m_current_speed = 0; // marginally better than having it initialized with junk
    double m_current_heading = 0;

    ros::Publisher m_helm_pub;
    ros::Publisher m_display_pub;

    ros::Subscriber m_controller_msgs_sub;
    ros::Subscriber m_position_sub;
    ros::Subscriber m_heading_sub;
    ros::Subscriber m_speed_sub;

    ros::ServiceServer m_update_reference_trajectory_service;

    long m_TrajectoryNumber = 0;

    Controller* m_Controller;
    dynamic_reconfigure::Server<mpc::mpcConfig> m_Dynamic_Reconfigure_Server;
};

int main(int argc, char **argv)
{
    std::cerr << "Starting MPC node" << std::endl;
    ros::init(argc, argv, "mpc");
    MPCNode node;
    ros::spin();

    return 0;
}

#pragma clang diagnostic pop