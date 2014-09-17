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
	/*
	 const Eigen::MatrixXd mat_acc = trajectory->getTrajectory(
	 Trajectory::TRAJECTORY_TYPE_ACCELERATION);
	 for (int i = 0; i < mat_acc.cols(); ++i)
	 {
	 value = std::abs(mat_acc(point, i));
	 cost += value * value;
	 }

	 // normalize cost (independent to # of joints)
	 cost /= trajectory->getComponentSize(
	 FullTrajectory::TRAJECTORY_COMPONENT_JOINT);
	 */

	const Eigen::VectorXd pos = trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_JOINT).row(point);
	for (int i = 6; i < pos.rows(); ++i)
	{
		value = std::abs((double) pos(i));
		value = std::max(0.0, value - 0.2);
		cost += value * value;
	}

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
	robot_state::RobotStatePtr robot_state = evaluation_manager->robot_state_;
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

	const Eigen::VectorXd& r = full_trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_CONTACT_POSITION,
			Trajectory::TRAJECTORY_TYPE_POSITION).row(point);

	const Eigen::VectorXd& f = full_trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_CONTACT_FORCE,
			Trajectory::TRAJECTORY_TYPE_POSITION).row(point);

	int num_contacts = r.rows() / 3;

	bool has_positive = false;
	double negative_cost = 1.0;

	for (int i = 0; i < num_contacts; ++i)
	{
		int rbdl_body_id = planning_group->contact_points_[i].getRBDLBodyId();
		RigidBodyDynamics::Math::SpatialTransform contact_body_transform =
				model.X_base[rbdl_body_id];

		Eigen::Vector3d contact_position;
		Eigen::Vector3d contact_normal;
		GroundManager::getInstance()->getNearestGroundPosition(
				r.block(3 * i, 0, 3, 1), contact_position, contact_normal);
		Eigen::Vector3d contact_force = full_trajectory->getContactForce(point,
				i);

		// test
		int foot_index = i / 4 * 4;
		int ee_index = i % 4;
		contact_position = r.block(3 * foot_index, 0, 3, 1);

		double contact_v = contact_position(2);
		if (contact_v > 0.5)
			has_positive = true;
		negative_cost *= contact_v;

		contact_v = 0.5 * tanh(4 * contact_v - 2) + 0.5;

		contact_position(2) = 0;
		switch (ee_index)
		{
		case 0:
			contact_position(0) -= 0.05;
			contact_position(1) -= 0.05;
			break;
		case 1:
			contact_position(0) += 0.05;
			contact_position(1) -= 0.05;
			break;
		case 2:
			contact_position(0) += 0.05;
			contact_position(1) += 0.2;
			break;
		case 3:
			contact_position(0) -= 0.05;
			contact_position(1) += 0.2;
			break;
		}

		Eigen::Vector3d body_contact_position = contact_body_transform.r;

		Eigen::Vector3d body_rot = exponential_map::RotationToExponentialMap(
				contact_body_transform.E);
		Eigen::Vector3d ground_rot = exponential_map::AngleAxisToExponentialMap(
				Eigen::AngleAxisd(0.0, contact_normal));
		Eigen::Vector3d orientation_diff = body_rot - ground_rot;

		Eigen::Vector3d position_diff = body_contact_position
				- contact_position;
		double position_diff_cost = position_diff.squaredNorm()
				+ orientation_diff.squaredNorm();

		double contact_body_velocity_cost = model.v[rbdl_body_id].squaredNorm()
				* 10;

		const double k1 = 0.01; //10.0;
		const double k2 = 3; //3.0;
		const double force_normal = std::max(0.0,
				contact_normal.dot(contact_force));
		double contact_variable = contact_v; //0.5 * std::tanh(k1 * force_normal - k2) + 0.5;

		cost += contact_variable
				* (position_diff_cost + contact_body_velocity_cost);
	}

	if (!has_positive)
		cost += negative_cost;

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

	return is_feasible;
}

bool TrajectoryCostCOM::evaluate(const NewEvalManager* evaluation_manager,
		int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

	return is_feasible;
}

bool TrajectoryCostEndeffectorVelocity::evaluate(
		const NewEvalManager* evaluation_manager, int point, double& cost) const
{
	bool is_feasible = true;
	cost = 0;

	// implement

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

	// implement

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

	const Eigen::VectorXd& r = full_trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_CONTACT_POSITION,
			Trajectory::TRAJECTORY_TYPE_POSITION).row(point);

	const Eigen::VectorXd& f = full_trajectory->getComponentTrajectory(
			FullTrajectory::TRAJECTORY_COMPONENT_CONTACT_FORCE,
			Trajectory::TRAJECTORY_TYPE_POSITION).row(point);

	int num_contacts = r.rows() / 3;

	for (int i = 0; i < num_contacts; ++i)
	{
		int rbdl_body_id = planning_group->contact_points_[i].getRBDLBodyId();

		Eigen::Vector3d contact_position;
		Eigen::Vector3d contact_normal;
		GroundManager::getInstance()->getNearestGroundPosition(
				r.block(3 * i, 0, 3, 1), contact_position, contact_normal);
		Eigen::Vector3d contact_force = full_trajectory->getContactForce(point,
				i);

		// active contacts only
		const double k1 = 10.0;
		const double k2 = 3.0;
		const double force_normal = std::max(0.0,
				contact_normal.dot(contact_force));
		double contact_variable = 0.5 * std::tanh(k1 * force_normal - k2) + 0.5;

		double angle = 0.0;
		double norm = contact_force.norm();
		if (contact_force.norm() > 1e-7)
		{
			contact_force.normalize();
			angle = (contact_force.norm() < 1e-7) ?
					0.0 : acos(contact_normal.dot(contact_force));
			angle = std::max(0.0, std::abs(angle) - 0.25 * M_PI);
		}

		cost += contact_variable * angle * angle * norm * norm;
	}

	TIME_PROFILER_END_TIMER(FrictionCone);

	return is_feasible;
}

}
