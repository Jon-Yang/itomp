#include <itomp_cio_planner/cost/trajectory_cost.h>
#include <itomp_cio_planner/contact/ground_manager.h>
#include <itomp_cio_planner/util/exponential_map.h>

namespace itomp_cio_planner
{

TrajectoryCost::TrajectoryCost(int index, std::string name, double weight) :
		index_(index), name_(name), weight_(weight)
{

}

TrajectoryCost::~TrajectoryCost()
{

}

bool TrajectoryCostSmoothness::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	TIME_PROFILER_START_TIMER(Smoothness);

	const FullTrajectoryConstPtr trajectory =
			evaluation_manager->getFullTrajectory();
	ROS_ASSERT(trajectory->hasAcceleration());

	cost = 0;
	double value;

	const Eigen::MatrixXd mat_acc = trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_JOINT,
			Trajectory::TRAJECTORY_TYPE_ACCELERATION);
	for (int i = 0; i < mat_acc.cols(); ++i)
	{
		value = mat_acc(point, i);
		cost += value * value;
	}

	// normalize cost (independent to # of joints)
	cost /= trajectory->getComponentSize(
			FullTrajectory::TRAJECTORY_COMPONENT_JOINT);

	/*
	 const Eigen::VectorXd pos = trajectory->getComponentTrajectory(
	 FullTrajectory::TRAJECTORY_COMPONENT_JOINT).row(point);
	 for (int i = 6; i < pos.rows(); ++i)
	 {
	 value = std::abs((double) pos(i));
	 value = std::max(0.0, value - 0.3);
	 //cost += value * value;
	 }*/

	// normalize cost (independent to # of joints)
	//cost /= trajectory->getComponentSize(
	//	FullTrajectory::TRAJECTORY_COMPONENT_JOINT);
	TIME_PROFILER_END_TIMER(Smoothness);

	return true;
}

bool TrajectoryCostObstacle::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	TIME_PROFILER_START_TIMER(Obstacle);

	bool is_feasible = true;
	cost = 0;

	const FullTrajectoryConstPtr trajectory =
			evaluation_manager->getFullTrajectory();
	robot_state::RobotStatePtr robot_state = evaluation_manager->getRobotState(
			point);
	const planning_scene::PlanningSceneConstPtr planning_scene =
			evaluation_manager->getPlanningScene();

	ROS_ASSERT(
			robot_state->getVariableCount() == trajectory->getComponentSize(FullTrajectory::TRAJECTORY_COMPONENT_JOINT));

	collision_detection::CollisionRequest collision_request;
	collision_detection::CollisionResult collision_result;
	collision_request.verbose = false;
	collision_request.contacts = true;
	collision_request.max_contacts = 1000;

	const Eigen::MatrixXd mat = trajectory->getTrajectory(
			Trajectory::TRAJECTORY_TYPE_POSITION).row(point);
	robot_state->setVariablePositions(mat.data());

	const double self_collision_scale = 0.1;

	planning_scene->checkCollisionUnpadded(collision_request, collision_result,
			*robot_state);

	const collision_detection::CollisionResult::ContactMap& contact_map =
			collision_result.contacts;
	for (collision_detection::CollisionResult::ContactMap::const_iterator it =
			contact_map.begin(); it != contact_map.end(); ++it)
	{
		const collision_detection::Contact& contact = it->second[0];

		if (contact.body_type_1 != collision_detection::BodyTypes::WORLD_OBJECT
				&& contact.body_type_2
						!= collision_detection::BodyTypes::WORLD_OBJECT)
			cost += self_collision_scale * contact.depth;
		else
			cost += contact.depth;
	}
	collision_result.clear();

	is_feasible = (cost == 0.0);

	TIME_PROFILER_END_TIMER(Obstacle);

	return is_feasible;
}

bool TrajectoryCostValidity::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

	return is_feasible;
}

bool TrajectoryCostContactInvariant::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	TIME_PROFILER_START_TIMER(ContactInvariant);

	bool is_feasible = true;
	cost = 0;

	const FullTrajectoryConstPtr full_trajectory =
			evaluation_manager->getFullTrajectory();
	const ItompPlanningGroupConstPtr& planning_group =
			evaluation_manager->getPlanningGroup();
	const RigidBodyDynamics::Model& model = evaluation_manager->getRBDLModel(
			point);

	bool has_positive = false;
	double negative_cost = 1.0;

	const std::vector<ContactVariables>& contact_variables =
			evaluation_manager->contact_variables_[point];
	int num_contacts = contact_variables.size();
	for (int i = 0; i < num_contacts; ++i)
	{
		int rbdl_body_id = planning_group->contact_points_[i].getRBDLBodyId();
		RigidBodyDynamics::Math::SpatialTransform contact_body_transform =
				model.X_base[rbdl_body_id];

		double raw_contact_variable = contact_variables[i].getRawVariable();
		if (raw_contact_variable > 0.0)
			has_positive = true;
		negative_cost *= raw_contact_variable;

		double contact_v = contact_variables[i].getVariable();

		Eigen::Vector3d body_position = contact_body_transform.r;
		Eigen::Vector3d body_orientation =
				exponential_map::RotationToExponentialMap(
						contact_body_transform.E);

		Eigen::Vector3d position_diff = body_position
				- contact_variables[i].projected_position_;
		Eigen::Vector3d orientation_diff = body_orientation
				- contact_variables[i].projected_orientation_;

		double position_diff_cost = position_diff.squaredNorm()
				+ orientation_diff.squaredNorm();

		double contact_body_velocity_cost = model.v[rbdl_body_id].squaredNorm()
				* 10;

		const double k1 = 0.01; //10.0;
		const double k2 = 3; //3.0;

		cost += contact_v * (position_diff_cost + contact_body_velocity_cost);
	}

	if (!has_positive)
	{
		cost += negative_cost;
	}

	TIME_PROFILER_END_TIMER(ContactInvariant);

	return is_feasible;
}

bool TrajectoryCostPhysicsViolation::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	TIME_PROFILER_START_TIMER(PhysicsViolation);

	const RigidBodyDynamics::Model& model = evaluation_manager->getRBDLModel(
			point);
	double mass = 0;
	for (int i = 0; i < model.mBodies.size(); ++i)
		mass += model.mBodies[i].mMass;
	double dt = evaluation_manager->getFullTrajectory()->getDiscretization();
	double normalizer = 1.0 / mass * dt;

	for (int i = 0; i < 6; ++i)
	{
		// non-actuated root joints
		double joint_torque = evaluation_manager->tau_[point](i);
		joint_torque *= normalizer;
		cost += joint_torque * joint_torque;
	}
	cost /= 6.0;

	TIME_PROFILER_END_TIMER(PhysicsViolation);

	return is_feasible;
}

bool TrajectoryCostGoalPose::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	TIME_PROFILER_START_TIMER(GoalPose);

	// TODO
	Eigen::Vector3d goal_foot_pos[3];
	goal_foot_pos[0] = Eigen::Vector3d(-0.1, 2.0, 0.0);
	goal_foot_pos[1] = Eigen::Vector3d(0.1, 2.0, 0.0);
	goal_foot_pos[2] = Eigen::Vector3d(0.0, 2.0, 1.12);

	Eigen::Vector3d goal_ori = exponential_map::RotationToExponentialMap(
			Eigen::Matrix3d::Identity());

	bool is_feasible = true;
	cost = 0;

	// implement

	if (point == evaluation_manager->getFullTrajectory()->getNumPoints() - 1)
	{
		Eigen::Vector3d cur_foot_pos[3];
		unsigned body_ids[3];

		// TODO
		body_ids[0] = evaluation_manager->getRBDLModel(point).GetBodyId(
				"left_foot_endeffector_link");
		body_ids[1] = evaluation_manager->getRBDLModel(point).GetBodyId(
				"right_foot_endeffector_link");
		body_ids[2] = evaluation_manager->getRBDLModel(point).GetBodyId(
				"pelvis_link");
		//std::cout << "bodyid2 : " << body_ids[2] << std::endl;
		body_ids[2] = 6;
		cur_foot_pos[0] =
				evaluation_manager->getRBDLModel(point).X_base[body_ids[0]].r;
		cur_foot_pos[1] =
				evaluation_manager->getRBDLModel(point).X_base[body_ids[1]].r;
		cur_foot_pos[2] =
				evaluation_manager->getRBDLModel(point).X_base[body_ids[2]].r;

		cost += (goal_foot_pos[0] - cur_foot_pos[0]).squaredNorm();
		cost += (goal_foot_pos[1] - cur_foot_pos[1]).squaredNorm();
		cost += (goal_foot_pos[2] - cur_foot_pos[2]).squaredNorm();

		Eigen::Vector3d cur_ori[3];
		cur_ori[0] = exponential_map::RotationToExponentialMap(
				evaluation_manager->getRBDLModel(point).X_base[body_ids[0]].E);
		cur_ori[1] = exponential_map::RotationToExponentialMap(
				evaluation_manager->getRBDLModel(point).X_base[body_ids[1]].E);
		cur_ori[2] = exponential_map::RotationToExponentialMap(
				evaluation_manager->getRBDLModel(point).X_base[body_ids[2]].E);

		cost += (goal_ori - cur_ori[0]).squaredNorm();
		//cost += (goal_ori - cur_ori[1]).squaredNorm();
		//cost += (goal_ori - cur_ori[2]).squaredNorm();
	}

	TIME_PROFILER_END_TIMER(GoalPose);

	return is_feasible;
}

bool TrajectoryCostCOM::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	TIME_PROFILER_START_TIMER(COM);

	// implement

	// TODO: contact regulation cost for foot contacts
	const std::vector<ContactVariables>& contact_variables =
			evaluation_manager->contact_variables_[point];
	int num_contacts = contact_variables.size();
	num_contacts = 2;
	for (int i = 0; i < num_contacts; ++i)
	{
		double contact_variable = contact_variables[i].getVariable();
		Eigen::Vector3d force_sum = Eigen::Vector3d::Zero();
		for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
		{
			force_sum += contact_variables[i].getPointForce(c);
		}
		const double k_1 = 1e-5;
		const double k_2 = 1e-7;
		const double regulation_factor = ((i < 2) ? 1.0 : 10.0) * k_1
				/ (contact_variable * contact_variable + k_2);
		const double force_magnitude = force_sum.norm() / 15000.0;
		cost += regulation_factor * force_magnitude * force_magnitude;
	}

	TIME_PROFILER_END_TIMER(COM);

	return is_feasible;
}

bool TrajectoryCostEndeffectorVelocity::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement
	TIME_PROFILER_START_TIMER(EndeffectorVelocity);

	// implement

	// TODO: contact regulation cost for hand contacts
	const std::vector<ContactVariables>& contact_variables =
			evaluation_manager->contact_variables_[point];
	int num_contacts = contact_variables.size();
	for (int i = 2; i < num_contacts; ++i)
	{
		double contact_variable = contact_variables[i].getVariable();
		Eigen::Vector3d force_sum = Eigen::Vector3d::Zero();
		for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
		{
			force_sum += contact_variables[i].getPointForce(c);
		}
		const double k_1 = 1e-5;
		const double k_2 = 1e-7;
		const double regulation_factor = ((i < 2) ? 1.0 : 10.0) * k_1
				/ (contact_variable * contact_variable + k_2);
		const double force_magnitude = force_sum.norm() / 15000.0;
		cost += regulation_factor * force_magnitude * force_magnitude;
	}

	TIME_PROFILER_END_TIMER(EndeffectorVelocity);

	return is_feasible;
}

bool TrajectoryCostTorque::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	TIME_PROFILER_START_TIMER(Torque);

	const RigidBodyDynamics::Model& model = evaluation_manager->getRBDLModel(
			point);
	double mass = 0;
	for (int i = 0; i < model.mBodies.size(); ++i)
		mass += model.mBodies[i].mMass;
	double dt = evaluation_manager->getFullTrajectory()->getDiscretization();
	double normalizer = 1.0 / mass * dt; // * dt;

	for (int i = 6; i < evaluation_manager->tau_[point].rows(); ++i)
	{
		// actuated joints
		double joint_torque = evaluation_manager->tau_[point](i);
		joint_torque *= normalizer;
		cost += joint_torque * joint_torque;
	}
	cost /= (double) (evaluation_manager->tau_[point].rows() - 6);

	TIME_PROFILER_END_TIMER(Torque);

	return is_feasible;
}

bool TrajectoryCostRVO::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

	return is_feasible;
}

bool TrajectoryCostFTR::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	const FullTrajectoryConstPtr full_trajectory =
			evaluation_manager->getFullTrajectory();
	const ItompPlanningGroupConstPtr& planning_group =
			evaluation_manager->getPlanningGroup();
	const RigidBodyDynamics::Model& model = evaluation_manager->getRBDLModel(
			point);

	const Eigen::VectorXd& q = full_trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_JOINT).row(point);

	// TODO:
	const char* endeffector_chain_group_names[] =
	{ "left_leg", "right_leg", "left_arm", "right_arm" };

	std::vector<double> positions;
	int num_joints = q.cols();
	positions.resize(num_joints);

	robot_state::RobotStatePtr robot_state = evaluation_manager->getRobotState(
			point);
	robot_state->setVariablePositions(q.data());

	const std::vector<ContactVariables>& contact_variables =
			evaluation_manager->contact_variables_[point];
	int num_contacts = contact_variables.size();
	for (int i = 0; i < num_contacts; ++i)
	{
		std::string chain_name = endeffector_chain_group_names[i];
		Eigen::MatrixXd jacobianFull =
				(robot_state->getJacobian(
						evaluation_manager->getItompRobotModel()->getMoveitRobotModel()->getJointModelGroup(
								chain_name)));
		Eigen::MatrixXd jacobian = jacobianFull.block(0, 0, 3,
				jacobianFull.cols());
		Eigen::MatrixXd jacobian_transpose = jacobian.transpose();

		int rbdl_body_id = planning_group->contact_points_[i].getRBDLBodyId();
		RigidBodyDynamics::Math::SpatialTransform contact_body_transform =
				model.X_base[rbdl_body_id];

		Eigen::Vector3d orientation =
				contact_variables[i].projected_orientation_;
		Eigen::Vector3d contact_normal =
				orientation.block(0, 2, 3, 1).transpose();
		Eigen::Vector3d contact_force_sum = Eigen::Vector3d::Zero();
		for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
		{
			contact_force_sum += contact_variables[i].getPointForce(c);
		}

		// computing direction, first version as COM velocity between poses
		Eigen::Vector3d direction = contact_force_sum;
		if (direction.norm() != 0)
		{
			direction.normalize();
			double ftr = 1
					/ std::sqrt(
							direction.transpose()
									* (jacobian * jacobian_transpose)
									* direction);
			KDL::Vector position, unused, normal;

			ftr *= -direction.dot(contact_normal);
			// bound value btw -10 and 10, then 0 and 1
			ftr = (ftr < -10) ? -10 : ftr;
			ftr = (ftr > 10) ? 10 : ftr;
			ftr = (ftr + 10) / 20;

			cost += std::max(0.0, contact_force_sum.norm() - ftr);
		}
	}

	return is_feasible;
}

bool TrajectoryCostCartesianTrajectory::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

	return is_feasible;
}

bool TrajectoryCostSingularity::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

	return is_feasible;
}

bool TrajectoryCostFrictionCone::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	TIME_PROFILER_START_TIMER(FrictionCone);

	bool is_feasible = true;
	cost = 0;

	const FullTrajectoryConstPtr full_trajectory =
			evaluation_manager->getFullTrajectory();
	const ItompPlanningGroupConstPtr& planning_group =
			evaluation_manager->getPlanningGroup();
	const RigidBodyDynamics::Model& model = evaluation_manager->getRBDLModel(
			point);

	const std::vector<ContactVariables>& contact_variables =
			evaluation_manager->contact_variables_[point];
	int num_contacts = contact_variables.size();
	for (int i = 0; i < num_contacts; ++i)
	{
		double contact_variable = contact_variables[i].getVariable();

		Eigen::Matrix3d orientation = exponential_map::ExponentialMapToRotation(
				contact_variables[i].projected_orientation_);
		Eigen::Vector3d contact_normal =
				orientation.block(0, 2, 3, 1).transpose();

		for (int c = 0; c < NUM_ENDEFFECTOR_CONTACT_POINTS; ++c)
		{
			Eigen::Vector3d point_force = contact_variables[i].getPointForce(c);

			double angle = 0.0;
			double norm = point_force.norm();
			if (point_force.norm() > 1e-7)
			{
				point_force.normalize();
				angle = (point_force.norm() < 1e-7) ?
						0.0 : acos(contact_normal.dot(point_force));
				angle = std::max(0.0, std::abs(angle) - 0.25 * M_PI);
			}

			cost += contact_variable * angle * angle * norm * norm;
		}
	}

	TIME_PROFILER_END_TIMER(FrictionCone);

	return is_feasible;
}

}
