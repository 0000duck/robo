#include "../include/kinematics.hpp"
#include <Eigen/Dense>
#include <iostream>

namespace robo{

	// Constructors
	Kinematics::Kinematics(const Chain& chain_, int max_iter_, double eps_, double eps_joints_):
    chain(chain_), f_end(), joint_roots(chain_.nr_joints), joint_tips(chain_.nr_joints), link_tips(chain.nr_links),
	jacobian(6, chain_.nr_joints), svd(6, chain_.nr_joints,Eigen::ComputeThinU | Eigen::ComputeThinV),
    max_iter(max_iter_), eps(eps_), eps_joints(eps_joints_)
	{
		weights_IK << 1, 1, 1, 0.1, 0.1, 0.1;
	}


	Kinematics::Kinematics(const Chain& chain_, Vector6d weights_IK_, int max_iter_, double eps_, double eps_joints_):
    chain(chain_), f_end(), joint_roots(chain_.nr_joints), joint_tips(chain_.nr_joints), link_tips(chain.nr_links),
	jacobian(6, chain_.nr_joints), svd(6, chain_.nr_joints,Eigen::ComputeThinU | Eigen::ComputeThinV),
    max_iter(max_iter_), eps(eps_), eps_joints(eps_joints_), weights_IK(weights_IK_){}
	
	// Member functions
	void Kinematics::joint_to_cartesian(const Eigen::VectorXd& q){
		int iter_joint = 0;
		f_end = Frame();
		for(int iter_link=0; iter_link<chain.nr_links; iter_link++){
			if(chain.links[iter_link].has_joint()){
				joint_roots[iter_joint] = f_end;
				f_end = f_end * chain.links[iter_link].pose(q[iter_joint]);
				joint_tips[iter_joint] = f_end;
				iter_joint++;
			}
			else{
				f_end = f_end * chain.links[iter_link].pose(0.0);
			}
			link_tips[iter_link] = f_end;
		}
	}

	int Kinematics::cartesian_to_joint(const Frame& f_in, const Eigen::VectorXd& q_init){
		Vector6d delta_frame;
		Vector6d delta_frame_new;
		q = q_init;
		joint_to_cartesian(q);
		delta_frame = f_end - f_in;
		delta_frame = weights_IK.asDiagonal() * delta_frame;
		double norm_delta_frame = delta_frame.norm();
		calculate_jacobian(q);
		
		// check if position already within specified tolerance
		if(norm_delta_frame < eps){
			delta_frame = f_end - f_in;
			q_out = q;
			return (error = E_NO_ERROR);
		}
		
		Eigen::MatrixXd jacobian_weighted = weights_IK.asDiagonal() * jacobian;

		double step_multiplier = 2;
		double lambda = 10;
		double dnorm = 1;
		double norm_delta_frame_new;
		double rho;

		for(int i=0; i<max_iter; ++i){
			svd.compute(jacobian_weighted); // TODO Profiling showed this function call is very expensive is there a cheaper alternative? 
			singular_vals = svd.singularValues();
			for(int j=0; j<singular_vals.rows(); ++j){
				singular_vals(j) = singular_vals(j) / (singular_vals(j) * singular_vals(j) + lambda);
			}
			tmp_q = svd.matrixU().transpose() * delta_frame;
			tmp_q = singular_vals.cwiseProduct(tmp_q);
			delta_q = svd.matrixV() * tmp_q;
			grad = jacobian_weighted.transpose() * delta_frame;
			dnorm = delta_q.lpNorm<Eigen::Infinity>();

			q_new = q + delta_q;
            joint_to_cartesian(q_new);
			delta_twist_new = f_end - f_in;
			norm_delta_twist_new = delta_twist_new.norm();

			// check for errors
			if(dnorm < eps_joints){
				error_norm_IK = norm_delta_twist_new;
				return (error = E_JOINTS_INCREMENT_TOO_SMALL);
			}
			if(grad.transpose() * grad < eps_joints * eps_joints){
				error_norm_IK = norm_delta_twist_new;
				return (error = E_JOINTS_GRADIENT_TOO_SMALL);
			}
			rho = norm_delta_frame * norm_delta_frame - norm_delta_frame_new * norm_delta_frame_new;
			rho /= delta_q.transpose() * (lambda * delta_q + grad);
			if (rho > 0) {
				q = q_new;
				delta_frame = delta_frame_new;
				norm_delta_frame = norm_delta_frame_new;
				std::cout << "DEBUG: Norm delta_twist: " << norm_delta_frame << " at iter: " << i << std::endl;
				if (norm_delta_frame < eps) {
					error_norm_IK = norm_delta_twist;
					q_out = q;
					return (error = E_NO_ERROR);
				}
				calculate_jacobian(q_new);
				jacobian_weighted = weights_IK.asDiagonal() * jacobian;
				double tmp = 2 * rho - 1;
				lambda = lambda * std::max(1 / 3.0, 1-tmp*tmp*tmp);
				step_multiplier = 2;
			} 
			else {
				lambda = lambda * step_multiplier;
				step_multiplier = 2 * step_multiplier;
			}
			q_out = q;
		}
        q_out = q;
        error_norm_IK = norm_delta_twist;
        return (error = E_MAX_ITERATIONS);
	}

	int Kinematics::cartesian_to_joint_sugihara(const Frame& f_in, const Eigen::VectorXd& q_init){
		Vector6d residual;
		Vector6d gk; // TODO find out how this is called
		Eigen::MatrixXd jacobian_weighted;

		q = q_init;

		for(int i=0; i<max_iter; ++i){
			joint_to_cartesian(q);
			calculate_jacobian(q);
			residual = f_end - f_in;
			jacobian_weighted = weights_IK.asDiagonal() * jacobian;
			gk = jacobian_weighted * residual;
			weigths_N = lambda_chan * Ek + biases; // (EK = eT * weights * e)
			H = jacobian_weighted*jacobian + weigths_N;
			q_new = q + H.inverse()*gk;
		}

	    return;
	}

	void Kinematics::calculate_jacobian(const Eigen::VectorXd& q){
		int iter_joint = 0;
		for(int iter_link=0; iter_link<chain.nr_links; ++iter_link){
			if (chain.links[iter_link].has_joint()) {
				// compute twist of the end effector motion caused by joint [jointndx]; expressed in base frame, with vel. ref. point equal to the end effector
				Twist unit_twist = rotate_twist(joint_roots[iter_joint].orientation, chain.links[iter_link].twist(q(iter_joint), 1.0));
				Twist end_twist = change_twist_reference(unit_twist, f_end.origin - joint_tips[iter_joint].origin);
				jacobian.block<3,1>(0, iter_joint) << end_twist.linear;
				jacobian.block<3,1>(3, iter_joint) << end_twist.rotation;
				++iter_joint;
			}
		}
	}
}	
