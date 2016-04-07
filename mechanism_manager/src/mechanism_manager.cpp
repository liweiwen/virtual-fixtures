#include "mechanism_manager/mechanism_manager.h"

////////// Function Approximator
#include <functionapproximators/FunctionApproximatorGMR.hpp>
#include <functionapproximators/MetaParametersGMR.hpp>
#include <functionapproximators/ModelParametersGMR.hpp>

namespace mechanism_manager
{

  using namespace virtual_mechanism_gmr;
  using namespace virtual_mechanism_interface;
  using namespace DmpBbo;
  using namespace tool_box;
  using namespace Eigen;

  typedef FunctionApproximatorGMR fa_t;
  

bool MechanismManager::ReadConfig(std::string file_path) // FIXME Switch to ros param server
{
	YAML::Node main_node;
	
	try
	{
	    main_node = YAML::LoadFile(file_path);
	}
	catch(...)
	{
	    return false;
	}
	
	// Retrain the parameters from yaml file
	std::vector<std::string> model_names;
    //std::vector<std::vector<double> > quat_start;
    //std::vector<std::vector<double> > quat_end;
	std::string models_path(pkg_path_+"/models/");
	std::string prob_mode_string;
    int position_dim;
    double K;
    double B;
    bool normalize;
	
	main_node["models"] >> model_names;
    main_node["quat_start"] >> quat_start_;
    main_node["quat_end"] >> quat_end_;   
	main_node["prob_mode"] >> prob_mode_string;
	main_node["use_weighted_dist"] >> use_weighted_dist_;
	main_node["use_active_guide"] >> use_active_guide_;
    main_node["position_dim"] >> position_dim;
    main_node["K"] >> K;
    main_node["B"] >> B;
    main_node["normalize"] >> normalize;

    assert(position_dim == 2 || position_dim == 3);
    position_dim_ = position_dim;

    assert(K >= 0.0);
    assert(B >= 0.0);
	
	if (prob_mode_string == "hard")
	    prob_mode_ = HARD;
	else if (prob_mode_string == "potential")
	    prob_mode_ = POTENTIAL;
	else if (prob_mode_string == "soft")
	    prob_mode_ = SOFT;
	else
	    prob_mode_ = POTENTIAL; // Default

	// Create the virtual mechanisms starting from the GMM models
 	for(int i=0;i<model_names.size();i++)
	{

	    std::vector<std::vector<double> > data;
	    ReadTxtFile((models_path+model_names[i]).c_str(),data);
	    ModelParametersGMR* model_parameters_gmr = ModelParametersGMR::loadGMMFromMatrix(models_path+model_names[i]);
	    boost::shared_ptr<fa_t> fa_tmp_shr_ptr(new FunctionApproximatorGMR(model_parameters_gmr)); // Convert to shared pointer

        if(normalize)
        {
            vm_vector_.push_back(new VirtualMechanismGmrNormalized<VirtualMechanismInterfaceFirstOrder>(position_dim_,K,B,fa_tmp_shr_ptr)); // NOTE the vm always works in xyz so we use position_dim_
        }
        else
        {
            vm_vector_.push_back(new VirtualMechanismGmr<VirtualMechanismInterfaceFirstOrder>(position_dim_,K,B,fa_tmp_shr_ptr));
        }
    }
	return true;
}
  
MechanismManager::MechanismManager()
{

      //position_dim_ = 2; // NOTE position dimension is fixed, xyz
      orientation_dim_ = 4; // NOTE orientation dimension is fixed, quaternion (w q1 q2 q3)
      user_force_applied_ = true; // NOTE we assume the guide is passive, so the user is in contact with the robot

#ifdef INCLUDE_ROS_CODE
      pkg_path_ = ros::package::getPath("mechanism_manager");
      std::string config_file_path(pkg_path_+"/config/cfg.yml");
      if(ReadConfig(config_file_path))
      {
        ROS_INFO("Loaded config file: %s",config_file_path.c_str());
      }
      else
      {
        ROS_ERROR("Can not load config file: %s",config_file_path.c_str());
      }
#else
      pkg_path_ = "/home/sybot/workspace/virtual-fixtures/mechanism_manager"; // FIXME
      //pkg_path_ = PATH_TO_PKG;
      std::string config_file_path(pkg_path_+"/config/cfg.yml");
      bool file_read = ReadConfig(config_file_path);
      assert(file_read);
#endif

      // Number of virtual mechanisms
      vm_nb_ = vm_vector_.size();
      assert(vm_nb_ >= 1);

      // Initialize the virtual mechanisms and support vectors
      for(int i=0; i<vm_nb_;i++)
      {
         //vm_vector_[i]->Init();
         vm_vector_[i]->Init(quat_start_[i],quat_end_[i]);
         vm_vector_[i]->setWeightedDist(use_weighted_dist_[i]);
         vm_state_.push_back(VectorXd(position_dim_));
         vm_state_dot_.push_back(VectorXd(position_dim_));
         vm_quat_.push_back(VectorXd(orientation_dim_));
      }

      // Some Initializations
      tmp_eigen_vector_.resize(position_dim_);
      use_orientation_ = false;
      active_guide_.resize(vm_nb_,false);
      scales_.resize(vm_nb_);
      phase_.resize(vm_nb_);
      Kf_.resize(vm_nb_);
      phase_dot_.resize(vm_nb_);
      robot_position_.resize(position_dim_);
      robot_velocity_.resize(position_dim_);
      robot_orientation_.resize(orientation_dim_);
      orientation_error_.resize(3); // rpy
      cross_prod_.resize(3);
      prev_orientation_error_.resize(3);
      orientation_integral_.resize(3);
      orientation_derivative_.resize(3);
      f_pos_.resize(position_dim_);
      f_ori_.resize(3); // NOTE The dimension is always 3 for rpy

      // Clear
      tmp_eigen_vector_.fill(0.0);
      scales_.fill(0.0);
      phase_.fill(0.0);
      Kf_.fill(0.0);
      phase_dot_.fill(0.0);
      robot_position_.fill(0.0);
      robot_velocity_.fill(0.0);
      orientation_error_.fill(0.0);
      cross_prod_.fill(0.0);
      prev_orientation_error_.fill(0.0);
      orientation_integral_.fill(0.0);
      orientation_derivative_.fill(0.0);
      robot_orientation_ << 1.0, 0.0, 0.0, 0.0;
      f_pos_.fill(0.0);
      f_ori_.fill(0.0);

      #ifdef USE_ROS_RT_PUBLISHER
      try
      {
          ros_node_.Init("mechanism_manager");
          rt_publishers_values_.AddPublisher(ros_node_.GetNode(),"phase",phase_.size(),&phase_);
          rt_publishers_values_.AddPublisher(ros_node_.GetNode(),"Kf",Kf_.size(),&Kf_);
          rt_publishers_values_.AddPublisher(ros_node_.GetNode(),"phase_dot",phase_dot_.size(),&phase_dot_);
          rt_publishers_values_.AddPublisher(ros_node_.GetNode(),"scales",scales_.size(),&scales_);
          //rt_publishers_pose_.AddPublisher(ros_node_.GetNode(),"tracking_reference",tracking_reference_.size(),&tracking_reference_);
          rt_publishers_path_.AddPublisher(ros_node_.GetNode(),"robot_pos",robot_position_.size(),&robot_position_);
          for(int i=0; i<vm_nb_;i++)
          {
            std::string topic_name = "vm_pos_" + std::to_string(i+1);
            rt_publishers_path_.AddPublisher(ros_node_.GetNode(),topic_name,vm_state_[i].size(),&vm_state_[i]);
            //topic_name = "vm_ker_" + std::to_string(i+1);
            //boost::shared_ptr<RealTimePublisherMarkers> tmp_ptr = boost::make_shared<RealTimePublisherMarkers>(ros_node_.GetNode(),topic_name,root_name_);
            //rt_publishers_markers_.AddPublisher(tmp_ptr,&vm_kernel_[i]);
          }
      }
      catch(const std::runtime_error& e)
      {
        ROS_ERROR("Failed to create the real time publishers: %s",e.what());
      }
      #endif

      // Define the scale threshold to check when a guide is more "probable"
      scale_threshold_ = 1.0/static_cast<double>(vm_nb_);
      
      if(vm_nb_>1)
          scale_threshold_ = scale_threshold_ + 0.2;
}
  
MechanismManager::~MechanismManager()
{
      for(int i=0;i<vm_vector_.size();i++)
        delete vm_vector_[i];
}

/*void MechanismManager::MoveForward()
{
    for(int i=0; i<vm_nb_;i++)
      vm_vector_[i].move_forward_ = true;
}

void MechanismManager::MoveBackward()
{
    for(int i=0; i<vm_nb_;i++)
      vm_vector_[i].move_forward_ = false;
}*/

/*void MechanismManager::Update(const VectorXd& robot_pose, const VectorXd& robot_velocity, double dt, VectorXd& f_out, bool force_applied, bool move_forward)
{
    for(int i=0; i<vm_nb_;i++)
    {
      if(move_forward)
        vm_vector_[i]->moveForward();
      else
        vm_vector_[i]->moveBackward();
    }
    Update(robot_pose,robot_velocity,dt,f_out,force_applied);
}*/

/*void MechanismManager::Update(const VectorXd& robot_pose, const VectorXd& robot_velocity, double dt, VectorXd& f_out, bool force_applied)
{   
    for(int i=0; i<vm_nb_;i++)
    {
      //if (force_applied == false && scales_(i) >= scale_threshold_ && use_active_guide_[i] == true)
      if (force_applied == false && use_active_guide_[i] == true)
      {
	      vm_vector_[i]->setActive(true);
	      active_guide_[i] = true;
	      //std::cout << "Active " <<i<< std::endl;
      }
      else
	  {
	      vm_vector_[i]->setActive(false);
	      active_guide_[i] = false;
	      //std::cout << "Deactive "<<i<< std::endl;
	  }
    }
    Update(robot_pose,robot_velocity,dt,f_out);
}*/

void MechanismManager::GetVmPosition(const int idx, const double* position_ptr)
{
    assert(idx <= vm_vector_.size());
    tmp_eigen_vector_ = VectorXd::Map(position_ptr, position_dim_);
    vm_vector_[idx]->getState(tmp_eigen_vector_);
}
void MechanismManager::GetVmVelocity(const int idx, const double* velocity_ptr)
{
    assert(idx <= vm_vector_.size());
    tmp_eigen_vector_ = VectorXd::Map(velocity_ptr, position_dim_);
    vm_vector_[idx]->getStateDot(tmp_eigen_vector_);
}

void MechanismManager::Update(const double* robot_position_ptr, const double* robot_velocity_ptr, double dt, double* f_out_ptr, const bool user_force_applied)
{
    user_force_applied_ = user_force_applied;
    Update(robot_position_ptr,robot_velocity_ptr,dt,f_out_ptr);
}

void MechanismManager::Update(const double* robot_position_ptr, const double* robot_velocity_ptr, double dt, double* f_out_ptr)
{
    assert(dt > 0.0);
    dt_ = dt;

    // FIXME We assume no orientation
    use_orientation_ = false;

    robot_position_ = VectorXd::Map(robot_position_ptr, position_dim_);
    robot_velocity_ = VectorXd::Map(robot_velocity_ptr, position_dim_);

    Update();

    VectorXd::Map(f_out_ptr, position_dim_) = f_pos_;
}

void MechanismManager::Update(const VectorXd& robot_pose, const VectorXd& robot_velocity, double dt, VectorXd& f_out, const bool user_force_applied)
{
    user_force_applied_ = user_force_applied;
    Update(robot_pose,robot_velocity,dt,f_out);
}

void MechanismManager::Update(const VectorXd& robot_pose, const VectorXd& robot_velocity, double dt, VectorXd& f_out)
{
    assert(dt > 0.0);
    dt_ = dt;

    assert(robot_velocity.size() == position_dim_);
    robot_velocity_ = robot_velocity;

    if(robot_pose.size() == position_dim_)
    {
        assert(f_out.size() == position_dim_);
        use_orientation_ = false;
        robot_position_ = robot_pose;
    }
    else if(robot_pose.size() == position_dim_ + orientation_dim_)
    {
        assert(f_out.size() == position_dim_ + 3);
        use_orientation_ = true;
        robot_position_ = robot_pose.segment(0,position_dim_);
        robot_orientation_ = robot_pose.segment(position_dim_,4);
    }

    Update();

    if(use_orientation_)
        f_out << f_pos_, f_ori_;
    else
        f_out = f_pos_;
}
void MechanismManager::Update()
{
	// Update the virtual mechanisms states, compute single probabilities
	for(int i=0; i<vm_nb_;i++)
	{
         // Check if there are external forces applied, activate the auto-completion
        //if (force_applied == false && scales_(i) >= scale_threshold_ && use_active_guide_[i] == true)
        if (user_force_applied_ == false && use_active_guide_[i] == true)
        {
            vm_vector_[i]->setActive(true);
            //active_guide_[i] = true;
            //std::cout << "Active" <<std::endl;
        }
        else
        {
            vm_vector_[i]->setActive(false);
            //active_guide_[i] = false;
            //std::cout << "Deactive" <<std::endl;
        }

      vm_vector_[i]->Update(robot_position_,robot_velocity_,dt_);
	  switch(prob_mode_) 
	  {
	    case HARD:
	      scales_(i) = vm_vector_[i]->getProbability(robot_position_);
	      break;
	    case POTENTIAL:
	      scales_(i) = std::exp(-10*vm_vector_[i]->getDistance(robot_position_));
	      break;
	    case SOFT:
	      scales_(i) = vm_vector_[i]->getProbability(robot_position_);
	      break;
	    default:
	      break;
	  }
	  
	  // Take the phase for each vm (For plots)
	  phase_(i) = vm_vector_[i]->getPhase();
      phase_dot_(i) = vm_vector_[i]->getPhaseDot();
      Kf_(i)= vm_vector_[i]->getKf();
	}

	sum_ = scales_.sum();
    f_pos_.fill(0.0); // Reset the force
    f_ori_.fill(0.0); // Reset the force
	
	for(int i=0; i<vm_nb_;i++)
	{
	  // Compute the conditional probabilities
	  switch(prob_mode_) 
	  {
	    case HARD:
	      scales_(i) =  scales_(i)/sum_;
	      break;
	    case POTENTIAL:
	      scales_(i) = scales_(i);
	      break;
	    case SOFT:
	      scales_(i) = std::exp(-10*vm_vector_[i]->getDistance(robot_position_)) * scales_(i)/sum_;
	      break;
	    default:
	      break;
	  }
	  // Compute the force from the vms
	  vm_vector_[i]->getState(vm_state_[i]);
	  vm_vector_[i]->getStateDot(vm_state_dot_[i]);
      vm_vector_[i]->getQuaternion(vm_quat_[i]);
          
	  //vm_vector_[i]->getLocalKernel(vm_kernel_[i]);
	  //K_ = vm_vector_[i]->getK();
	  //B_ = vm_vector_[i]->getB();
	  
      if(use_orientation_)
      {

        cross_prod_.noalias() = vm_quat_[i].segment<3>(1).cross(robot_orientation_.segment<3>(1));

        orientation_error_(0) = robot_orientation_(0) * vm_quat_[i](1)- vm_quat_[i](0) * robot_orientation_(1) - cross_prod_(0);
        orientation_error_(1) = robot_orientation_(0) * vm_quat_[i](2)- vm_quat_[i](0) * robot_orientation_(2) - cross_prod_(1);
        orientation_error_(2) = robot_orientation_(0) * vm_quat_[i](3)- vm_quat_[i](0) * robot_orientation_(3) - cross_prod_(2);

        orientation_integral_ = orientation_integral_ + orientation_error_ * dt_;
        orientation_derivative_ = (orientation_error_ - prev_orientation_error_)/dt_;

        f_ori_ += scales_(i) * (5.0 * orientation_error_ + 0.0 * orientation_integral_ + 0.0 * orientation_derivative_);

        prev_orientation_error_ = orientation_error_;

        //f_ori_(0)  += scales_(i) * 5.0 * (robot_orientation_(0) * vm_quat_[i](1)- vm_quat_[i](0) * robot_orientation_(1));
        //f_ori_(1)  += scales_(i) * 5.0 * (robot_orientation_(0) * vm_quat_[i](2)- vm_quat_[i](0) * robot_orientation_(2));
        //f_ori_(2)  += scales_(i) * 5.0 * (robot_orientation_(0) * vm_quat_[i](3)- vm_quat_[i](0) * robot_orientation_(3));
        f_pos_ += scales_(i) * (vm_vector_[i]->getK() * (vm_state_[i] - robot_position_) + vm_vector_[i]->getB() * (vm_state_dot_[i] - robot_velocity_)); // Sum over all the vms
      }
      else
        f_pos_ += scales_(i) * (vm_vector_[i]->getK() * (vm_state_[i] - robot_position_) + vm_vector_[i]->getB() * (vm_state_dot_[i] - robot_velocity_)); // Sum over all the vms

      /*if(loopCnt%100==0)
      {
          //std::cout << "** MV POS **" <<std::endl;
          //std::cout << vm_state_[i] <<std::endl;
          std::cout << "** MV VEL **" <<std::endl;
          std::cout << vm_state_dot_[i] <<std::endl;

          //std::cout << "DEACTIVE" <<std::endl;
          //std::cout << user_force_applied_ <<std::endl;
      }*/

    }	   

    //loopCnt++;

	//UpdateTrackingReference(robot_position);
	
	#ifdef USE_ROS_RT_PUBLISHER
        rt_publishers_values_.PublishAll();
        //rt_publishers_pose_.PublishAll();
        rt_publishers_path_.PublishAll();
	#endif
}
  
}
