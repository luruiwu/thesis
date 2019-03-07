/* particle_filter.cpp */

#include "particle_filter/particle_filter.h"

namespace pf
{
/******************************/
/*        Constructor         */
/******************************/

Particles::Particles() : _tfBuffer(ros::Duration(10), false)
{
  _initialized = 0;  // System has not been initialized yet
  _receivedSensorData = 0;
  _firstRun = 1;

  // Get the parameters from Parameter Server
  _nh.param<int>("/particles", _numParticles, 500);

  _nh.param<std::string>("/mapFrame", _mapFrameID, "map");
  _nh.param<std::string>("/worldFrame", _worldFrameID, "world");
  _nh.param<std::string>("/baseFootprintFrame", _baseFootprintFrameID, "base_footprint");
  _nh.param<std::string>("/baseStabilizedFrame", _baseStabilizedFrameID, "base_stabilized");
  _nh.param<std::string>("/baseLinkFrame", _baseLinkFrameID, "base_link");

  _nh.param<double>("/max_range", _filterMaxRange, 14);
  _nh.param<double>("/min_range", _filterMinRange, 0.05);
  _nh.param<double>("/observation_threshold_trans", _observationThresholdTranslation, 0.3);
  _nh.param<double>("/observation_threshold_rot", _observationThresholdRotation, 0.4);
  _nh.param<double>("/sensor_sample_distance", _sensorSampleDist, 0.2);
  _nh.param<double>("/transform_tolerance_time", _transformTolerance, 1.0);

  // Initial std deviations
  _nh.param<double>("/movement/x_std_dev", _XStdDev, 0.2);
  _nh.param<double>("/movement/y_std_dev", _YStdDev, 0.2);
  _nh.param<double>("/movement/z_std_dev", _ZStdDev, 0.2);

  _nh.param<double>("/movement/roll_std_dev", _RollStdDev, 0.2);
  _nh.param<double>("/movement/pitch_std_dev", _PitchStdDev, 0.2);
  _nh.param<double>("/movement/yaw_std_dev", _YawStdDev, 0.2);

  // Initialize Models
  // Movement model
  _mm = new DroneMovementModel(&_nh, &_tfBuffer, _worldFrameID, _baseFootprintFrameID);

  _mapModel = std::shared_ptr<MapModel>(new OccupancyMap(&_nh));
  // octomap_server must have already provided the map to proceed

  _om = std::shared_ptr<libPF::ObservationModel<DroneState> >(new DroneObservationModel(&_nh, _mapModel));

  _pf = new libPF::ParticleFilter<DroneState>(_numParticles, _om.get(), _mm);

  // TF listener / Broadcaster
  // tf2_ros::Buffer _tfBuffer(ros::Duration(10), false);
  _tfBuffer.clear();
  _tfListener = new tf2_ros::TransformListener(_tfBuffer);
  _tfBroadcaster = new tf2_ros::TransformBroadcaster();

  // set to identity the map to world transform
  _latestTransform.setIdentity();

  // Init PoseArray
  _poseArray.header.frame_id = _mapFrameID;
  _poseArray.poses.resize(_numParticles);

  // publishers can be advertised first, before needed:
  _posePublisher = _nh.advertise<geometry_msgs::PoseStamped>("/amcl/pose", 50);
  _poseArrayPublisher = _nh.advertise<geometry_msgs::PoseArray>("/amcl/particlecloud", 50);
  _filteredPointCloudPublisher = _nh.advertise<sensor_msgs::PointCloud2>("amcl/filtered_cloud", 5);

  // ROS subscriptions last:
  _globalLocalizationService =
      _nh.advertiseService("/global_localization", &Particles::globalLocalizationCallback, this);

  _initPoseService = _nh.advertiseService("/initialize_pose", &Particles::initialPoseSrvCallback, this);

  // Timer for sending the latest transform
  _latestTransformTimer =
      _nh.createTimer(ros::Duration(_transformTolerance), &Particles::latestTransformTimerCallback, this);

  // subscription on laser, tf message filter
  _scanListener = new message_filters::Subscriber<sensor_msgs::LaserScan>(_nh, "/scan", 100);

  // Use tf2_ros::MessageFilter to take a subscription to LaserScan msg and cache it until it is possible to transform
  // it into the target frame.
  _scanFilter = new tf2_ros::MessageFilter<sensor_msgs::LaserScan>(*_scanListener, _tfBuffer, _worldFrameID, 100, _nh);
  _scanFilter->registerCallback(boost::bind(&Particles::scanCallback, this, _1));

  // subscription on point cloud, tf message filter
  // TODO

  // subscription on init pose, tf message filter
  _initialPoseListener =
      new message_filters::Subscriber<geometry_msgs::PoseWithCovarianceStamped>(_nh, "/amcl/initial_pose", 5);

  _initialPoseFilter = new tf2_ros::MessageFilter<geometry_msgs::PoseWithCovarianceStamped>(
      *_initialPoseListener, _tfBuffer, _mapFrameID, 5, _nh);

  _initialPoseFilter->registerCallback(boost::bind(&Particles::initialPoseCallback, this, _1));

  pcl::console::setVerbosityLevel(pcl::console::L_ALWAYS);

  ROS_INFO("Particle filter created with %d particles!\n", _pf->numParticles());
}

/******************************/
/*         Destructor         */
/******************************/

Particles::~Particles()
{
  delete _scanFilter;
  delete _scanListener;
  delete _initialPoseFilter;
  delete _initialPoseListener;
  ROS_INFO("Particles object destroyed");
}

/******************************/
/*       scanCallback         */
/******************************/

void Particles::scanCallback(const sensor_msgs::LaserScanConstPtr& msg)
{
  ROS_DEBUG(" Laser received(time : % f) ", msg->header.stamp.toSec());

  if (!_initialized)
  {
    ROS_WARN("Localization not initialized yet, skipping laser callback.");
    ROS_INFO("Call /initialize_pose service to initialize it.");
    return;
  }

  double timediff = (msg->header.stamp - _lastLaserTime).toSec();
  if (_receivedSensorData && timediff < 0)
  {
    ROS_WARN("Ignoring received laser data that is %f s older than previous data!", timediff);
    return;
  }

  geometry_msgs::PoseStamped odomPose;
  // check if odometry available, skip scan if not.
  if (!_mm->lookupOdomPose(msg->header.stamp, odomPose))
  {
    ROS_WARN_ONCE("Odometry not available, skipping scan. (This warning will be printed only once)\n");
    return;
  }

  if (!_firstRun)
  {
    ros::Time start = ros::Time::now();
    double dt = (odomPose.header.stamp - _mm->getLastOdomPose().header.stamp).toSec();
    if (!_receivedSensorData || isAboveMotionThreshold(odomPose))
    // QUESTION what does it give me??
    {
      pcl::PointCloud<pcl::PointXYZ> pcFiltered;
      std::vector<float> laserRanges;
      prepareLaserPointCloud(msg, pcFiltered, laserRanges);

      ros::Time t = msg->header.stamp;
      geometry_msgs::TransformStamped sensorToBase;

      if (!_mm->lookupTargetToBaseTransform(pcFiltered.header.frame_id, t, sensorToBase))
      {
        return;
      }
      tf2::Transform baseToSensor;
      tf2::convert(sensorToBase.transform, baseToSensor);
      baseToSensor = baseToSensor.inverse();

      _filteredPointCloudPublisher.publish(pcFiltered);

      DroneObservationModel* laser = (DroneObservationModel*)_om.get();
      laser->setBaseToSensorTransform(baseToSensor);
      laser->setObservedMeasurements(pcFiltered, laserRanges);

      _pf->setObservationModel(laser);

      // run one filter step
      _pf->filter(dt);
      double tdiff = (ros::Time::now() - start).toSec();
      ROS_INFO("Laser filter done in %f s", tdiff);

      if (_publishUpdated)
        publishPoseEstimate(msg->header.stamp);
      _lastLocalizedPose = odomPose.pose;
      _receivedSensorData = true;
    }
    else
    {
      _pf->drift(dt);
    }
  }
  else
  {
    _lastLocalizedPose = odomPose.pose;
  }

  _mm->setLastOdomPose(odomPose);
  _firstRun = false;
  _lastLaserTime = msg->header.stamp;
  if (!_publishUpdated)
  {
    publishPoseEstimate(_lastLaserTime);
  }
}

/******************************/
/*latestTransformTimerCallback*/
/******************************/

void Particles::latestTransformTimerCallback(const ros::TimerEvent& timer_event)
{
  geometry_msgs::TransformStamped transform;
  transform.header.frame_id = _mapFrameID;
  transform.header.stamp = timer_event.current_real + ros::Duration(_transformTolerance);
  transform.child_frame_id = _worldFrameID;
  transform.transform = tf2::toMsg(_latestTransform.inverse());
  _tfBroadcaster->sendTransform(transform);
}

/******************************/
/*    initialPoseCallback     */
/******************************/

void Particles::initialPoseCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg)
{
  tf2::Transform transform;
  tf2::convert(msg->pose.pose, transform);

  if (msg->header.frame_id != _mapFrameID)
  {
    ROS_WARN("Frame ID of amcl/initialpose (%s) is different from the global frame %s", msg->header.frame_id.c_str(),
             _mapFrameID.c_str());
  }

  ROS_INFO("Set pose position around (x : %f, y : %f, z : %f) ", transform.getOrigin().getX(),
           transform.getOrigin().getY(), transform.getOrigin().getZ());
  double roll, pitch, yaw;
  tf2::getEulerYPR(transform.getRotation().normalize(), roll, pitch, yaw);
  ROS_INFO("Set pose orientation around (roll : %f, pitch : %f, yaw : %f) ", roll, pitch, yaw);

  // Create Gaussian distribution for particles
  DroneStateDistribution distribution(transform.getOrigin().getX(), transform.getOrigin().getY(),
                                      transform.getOrigin().getZ(), roll, pitch, yaw);
  distribution.setUniform(false);
  distribution.setStdDev(_XStdDev, _YStdDev, _ZStdDev, _RollStdDev, _PitchStdDev, _YawStdDev);
  _pf->drawAllFromDistribution(distribution);
  /*
  * The ParticleFilter has the following resampling modes:
  * @li RESAMPLE_NEVER skip resampling,
  * @li RESAMPLE_ALWAYS does a resampling in every filter step whenever
  *     filter() is called,
  * @li RESAMPLE_NEFF does a resampling in filter() only if the number of
  *     effective particles falls below the half of the total number of
  *     particles (see getNumEffectiveParticles() for details).
  */
  _pf->setResamplingMode(libPF::RESAMPLE_NEFF);
  _pf->resetTimer();
  _mm->reset();

  _initialized = true;
  _receivedSensorData = true;
  _firstRun = true;

  publishPoseEstimate(msg->header.stamp);
}

/******************************/
/* globalLocalizationCallback */
/******************************/

bool Particles::globalLocalizationCallback(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
{
  ROS_INFO("Initialize Global Localization with Uniform Distribution");

  DroneStateDistribution distribution(_mapModel);
  distribution.setUniform(true);
  _pf->drawAllFromDistribution(distribution);
  _pf->setResamplingMode(libPF::RESAMPLE_NEFF);
  _pf->resetTimer();
  _mm->reset();

  // Do not integrate measurements until moved(??)
  _receivedSensorData = true;
  _initialized = true;
  _firstRun = true;
  publishPoseEstimate(ros::Time::now());

  return true;
}

/******************************/
/*  initialPoseSrvCallback    */
/******************************/

bool Particles::initialPoseSrvCallback(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
{
  ROS_INFO("Initialize position using position parameters!");

  double x_pos, y_pos, z_pos, roll, pitch, yaw;

  _nh.param<double>("/x_pos", x_pos, 0);
  _nh.param<double>("/y_pos", y_pos, 0);
  _nh.param<double>("/z_pos", z_pos, 0);

  _nh.param<double>("/roll", roll, 0);
  _nh.param<double>("/pitch", pitch, 0);
  _nh.param<double>("/yaw", yaw, 0);

  tf2::Quaternion quat;
  quat.setRPY(roll, pitch, yaw);

  tf2::Transform transform(quat.normalize(), tf2::Vector3(x_pos, y_pos, z_pos));

  ROS_INFO("Set pose position around (x : %f, y : %f, z : %f) ", transform.getOrigin().getX(),
           transform.getOrigin().getY(), transform.getOrigin().getZ());

  tf2::getEulerYPR(transform.getRotation().normalize(), roll, pitch, yaw);
  ROS_INFO("Set pose orientation around (roll : %f, pitch : %f, yaw : %f) ", roll, pitch, yaw);

  // Create Gaussian distribution for particles
  DroneStateDistribution distribution(transform.getOrigin().getX(), transform.getOrigin().getY(),
                                      transform.getOrigin().getZ(), roll, pitch, yaw);
  distribution.setUniform(false);
  distribution.setStdDev(_XStdDev, _YStdDev, _ZStdDev, _RollStdDev, _PitchStdDev, _YawStdDev);
  _pf->drawAllFromDistribution(distribution);
  /*
  * The ParticleFilter has the following resampling modes:
  * @li RESAMPLE_NEVER skip resampling,
  * @li RESAMPLE_ALWAYS does a resampling in every filter step whenever
  *     filter() is called,
  * @li RESAMPLE_NEFF does a resampling in filter() only if the number of
  *     effective particles falls below the half of the total number of
  *     particles (see getNumEffectiveParticles() for details).
  */
  _pf->setResamplingMode(libPF::RESAMPLE_NEFF);
  _pf->resetTimer();
  _mm->reset();

  _initialized = true;
  _receivedSensorData = true;
  _firstRun = true;

  publishPoseEstimate(ros::Time::now());

  return true;
}

/******************************/
/*       publishPoses         */
/******************************/

void Particles::publishPoseEstimate(const ros::Time& t)
{
  _poseArray.header.stamp = t;
  if (_poseArray.poses.size() != _pf->numParticles())
  {
    _poseArray.poses.resize(_pf->numParticles());
  }

  // Fill in the pose array
  // CAN BE PARALLELIZED from here ***
  tf::Quaternion temp_pose_orien;  // declare it only once and use it later
#pragma omp parallel for
  for (unsigned i = 0; i < _pf->numParticles(); i++)
  {
    // Create a Pose object, fill it with x,y,z,r,p,y and then pass it on
    geometry_msgs::Pose temp_pose;
    temp_pose.position.x = _pf->getState(i).getXPos();
    temp_pose.position.y = _pf->getState(i).getYPos();
    temp_pose.position.z = _pf->getState(i).getZPos();

    temp_pose_orien =
        tf::createQuaternionFromRPY(_pf->getState(i).getRoll(), _pf->getState(i).getPitch(), _pf->getState(i).getYaw());
    // Convert tf::quaternion to std_msgs::quaternion to be accepted in the odom msg
    tf::quaternionTFToMsg(temp_pose_orien.normalize(), temp_pose.orientation);

    _poseArray.poses[i] = temp_pose;
  }
  // **** up to here

  // Publish
  _poseArrayPublisher.publish(_poseArray);

  // Send best particle as pose and one array
  geometry_msgs::PoseStamped bestPose;
  bestPose.header.frame_id = _mapFrameID;
  bestPose.header.stamp = t;

  // QUESTION is 0 really the best particle?
  bestPose.pose.position.x = _pf->getState(0).getXPos();
  bestPose.pose.position.y = _pf->getState(0).getYPos();
  bestPose.pose.position.z = _pf->getState(0).getZPos();

  temp_pose_orien =
      tf::createQuaternionFromRPY(_pf->getState(0).getRoll(), _pf->getState(0).getPitch(), _pf->getState(0).getYaw());
  temp_pose_orien.normalize();
  // Convert tf::quaternion to std_msgs::quaternion to be accepted in the odom msg
  tf::quaternionTFToMsg(temp_pose_orien.normalize(), bestPose.pose.orientation);

  // Publish
  _posePublisher.publish(bestPose);

  // Compute best transform : _latestTransform, base_footprint -> world
  geometry_msgs::PoseStamped worldToMap;
  try
  {
    tf2::Transform temp_tf2Transform;
    geometry_msgs::Transform temp_geomTransform;

    temp_geomTransform.translation.x = _pf->getBestState().getXPos();
    temp_geomTransform.translation.y = _pf->getBestState().getYPos();
    temp_geomTransform.translation.z = _pf->getBestState().getZPos();

    tf::Quaternion temp_pose_orien;
    // Instead of best I can use the MMSE
    temp_pose_orien = tf::createQuaternionFromRPY(_pf->getBestState().getRoll(), _pf->getBestState().getPitch(),
                                                  _pf->getBestState().getYaw());
    temp_pose_orien.normalize();
    tf::quaternionTFToMsg(temp_pose_orien, temp_geomTransform.rotation);

    tf2::fromMsg(temp_geomTransform, temp_tf2Transform);

    geometry_msgs::PoseStamped temp_poseStamped;
    temp_poseStamped.header.frame_id = _baseFootprintFrameID;
    temp_poseStamped.header.stamp = t;
    tf2::toMsg(temp_tf2Transform.inverse(), temp_poseStamped.pose);

    _tfBuffer.transform(temp_poseStamped, worldToMap, _worldFrameID);
  }
  catch (const tf2::TransformException& e)
  {
    ROS_WARN("Failed to subtract world to map transform, will not publish pose estimate: %s", e.what());
    return;
  }

  tf2::convert(worldToMap.pose, _latestTransform);

  // We want to send a transform that is good up until a tolerance time so that odom can be used
  ros::Time transform_expiration = (t + ros::Duration(_transformTolerance));
  geometry_msgs::TransformStamped tmp_tf_stamped;
  tmp_tf_stamped.header.frame_id = _mapFrameID;
  tmp_tf_stamped.header.stamp = transform_expiration;
  tmp_tf_stamped.child_frame_id = _worldFrameID;
  tf2::convert(_latestTransform.inverse(), tmp_tf_stamped.transform);

  _tfBroadcaster->sendTransform(tmp_tf_stamped);
}

/******************************/
/*   prepareLaserPointCloud   */
/******************************/

void Particles::prepareLaserPointCloud(const sensor_msgs::LaserScanConstPtr& scan, pcl::PointCloud<pcl::PointXYZ>& pc,
                                       std::vector<float>& ranges) const
{
  unsigned int numBeams = scan->ranges.size();
  // Get every n-th scan;
  unsigned int step = 1;

  // Prepare Laser Message
  unsigned int numBeamsSkipped = 0;

  double laserMin = std::max(double(scan->range_min), _filterMinRange);

  pcl_conversions::toPCL(scan->header, pc.header);

  // Allocate some capacity for vector
  ranges.reserve(50);
  pc.points.reserve(50);

  for (int beamId = 0; beamId < numBeams; beamId += step)
  {
    double range = scan->ranges[beamId];
    if (range >= laserMin && range <= _filterMaxRange)
    {
      double laserAngle = scan->angle_min + beamId * scan->angle_increment;

      tf2::Transform laserAngleRotation(tf2::Quaternion(tf2::Vector3(0.0, 0.0, 1.0), laserAngle));
      tf2::Vector3 laserEndpointTransform(range, 0.0, 0.0);
      tf2::Vector3 pt(laserAngleRotation * laserEndpointTransform);

      pc.points.push_back(pcl::PointXYZ(pt.x(), pt.y(), pt.z()));
      ranges.push_back(range);
    }
    else
    {
      numBeamsSkipped++;
    }
  }

  pc.is_dense = true;

  // Uniform Sampling
  pcl::UniformSampling<pcl::PointXYZ> uniformSampling;
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloudPtr;
  cloudPtr.reset(new pcl::PointCloud<pcl::PointXYZ>(pc));
  uniformSampling.setInputCloud(cloudPtr);

  uniformSampling.setRadiusSearch(_sensorSampleDist);
  pcl::PointCloud<int> sampledIndices;
  uniformSampling.compute(sampledIndices);

  pcl::copyPointCloud(*cloudPtr, sampledIndices.points, pc);

  // Adjust "ranges" to contain the same points
  std::vector<float> rangesSparse;
  rangesSparse.resize(sampledIndices.size());
  for (unsigned int i = 0; i < rangesSparse.size(); i++)
  {
    rangesSparse[i] = ranges[sampledIndices.points[i]];
  }
  ranges = rangesSparse;

  ROS_INFO("Laser PointCloud subsampled: %zu from %zu (%u out of valid range)", pc.size(), cloudPtr->size(),
           numBeamsSkipped);
}

/******************************/
/*   isAboveMotionThreshold   */
/******************************/

bool Particles::isAboveMotionThreshold(const geometry_msgs::PoseStamped& odom_pose) const
{
  // Calculate transform between odom pose and last localized pose
  tf2::Transform lastLocalizedPose;
  tf2::convert(_lastLocalizedPose, lastLocalizedPose);

  tf2::Transform odomPose;
  tf2::convert(odom_pose.pose, odomPose);

  tf2::Transform odomTransform = lastLocalizedPose.inverseTimes(odomPose);

  double yaw, pitch, roll;
  odomTransform.getBasis().getRPY(roll, pitch, yaw);

  bool isAbove;
  isAbove = (odomTransform.getOrigin().length() >= _observationThresholdTranslation ||
             std::abs(yaw) >= _observationThresholdRotation);

  return isAbove;
}

}  // namespace pf