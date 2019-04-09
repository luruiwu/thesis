#include "drone_coverage/coverage_finder.h"

namespace drone_coverage
{
CoverageFinder::CoverageFinder()
{
  ros::WallTime startTime = ros::WallTime::now();

  ROS_INFO("Wall Finder object created\n");
  _octomap_loaded = 0;
  _map_sub = _nh.subscribe<octomap_msgs::Octomap>("/octomap_binary", 1, &CoverageFinder::octomapCallback, this);

  _covered_pub = _nh.advertise<octomap_msgs::Octomap>("/covered_surface", 1);
  _vis_pub = _nh.advertise<visualization_msgs::MarkerArray>("/visualization_marker_array", 10);

  // Get initial positions from the Parameter Server
  _nh.param<double>("/x_pos", _init_pose[0], 0);
  _nh.param<double>("/y_pos", _init_pose[1], 0);
  _nh.param<double>("/z_pos", _init_pose[2], 0);

  // Get configurations
  _nh.param<double>("/rfid/range", _sensor_range, 1);
  _nh.param<double>("/uav/radius", _uav_radius, 0.5);
  _nh.param<double>("/uav/safety_offset", _uav_safety_offset, 0.3);
  _uav_safety_offset += _uav_radius;
  _nh.param<double>("/world/min_obstacle_height", _min_obstacle_height, 0.3);

  while (!_octomap_loaded)
  {
    ROS_INFO_ONCE("Waiting to load octomap, cannot procceed.......\n");
    ros::spinOnce();
    ros::Rate(10).sleep();
  }

  // Offline - we can start in the beginning of the bounds plus the safety distance of the drone
  _init_pose[0] = _min_bounds[0];
  _init_pose[1] = _min_bounds[1];
  _init_pose[2] = _min_bounds[2];

  // Reset some variables that will be filled later
  _walls = new octomap::OcTree(_octomap->getResolution());

  // The initial position of the sensor/drone is not ON the bounds, but we start with an offset that allows the sensor
  // to ray around
  _sensor_position.x() = _init_pose[0];
  _sensor_position.y() = _init_pose[1];
  _sensor_position.z() = _init_pose[2];

  // Locate the walls in the octomap
  CoverageFinder::findCoveredSurface();

  // Publish the points as an Octomap
  CoverageFinder::publishCoveredSurface();

  // Publish sensor positions / waypoints
  CoverageFinder::publishWaypoints();

  double dt = (ros::WallTime::now() - startTime).toSec();
  ROS_INFO_STREAM("Coverage Finder took " << dt << " seconds.");
}

CoverageFinder::~CoverageFinder()
{
  if (_octomap != NULL)
    delete _octomap;
  if (_walls != NULL)
    delete _walls;
}

void CoverageFinder::octomapCallback(const octomap_msgs::OctomapConstPtr& msg)
{
  // Load octomap msg
  octomap::AbstractOcTree* abstract = octomap_msgs::msgToMap(*msg);
  if (abstract)
  {
    octomap::ColorOcTree* coloroctree = dynamic_cast<octomap::ColorOcTree*>(abstract);
    _octomap = reinterpret_cast<octomap::OcTree*>(coloroctree);

    if (_octomap == NULL)
    {
      ROS_WARN("Octomap message does not contain an OcTree\n");
      return;
    }
    else
    {
      ROS_INFO("Octomap successfully loaded\n");
    }
  }
  else
  {
    ROS_WARN("Could not deserialize message to OcTree");
    return;
  }

  _octomap->getMetricMin(_min_bounds[0], _min_bounds[1], _min_bounds[2]);
  _octomap->getMetricMax(_max_bounds[0], _max_bounds[1], _max_bounds[2]);

  // We don't want points that are under the ground --> bound < 0 -->convert them to 0
  _min_bounds[2] = (_min_bounds[2] < 0) ? 0 : _min_bounds[2];

  _octomap_loaded = 1;

  ROS_INFO("Octomap bounds are (x,y,z) : \n [min]  %f, %f, %f\n [max]  %f, %f, %f", _min_bounds[0], _min_bounds[1],
           _min_bounds[2], _max_bounds[0], _max_bounds[1], _max_bounds[2]);
}

void CoverageFinder::findCoveredSurface()
{
  octomap::point3d center;
  bool wall_found, point_inserted;
  octomap::point3d direction(1, 0, 0);

  // For every valid point inside the world, raycast
  // The step between each position is the half coverage of the sensor
  // NOTE we are starting from the mean bounds, reaching the max bounds
  // If someone wants to start from a different place, more checks need to be done, to ensure safety distance are OK
  while (_sensor_position.z() < _max_bounds[2])
  {
    while (_sensor_position.x() < _max_bounds[0])
    {
      while (_sensor_position.y() < _max_bounds[1])
      {
        // is_point_safe = true;
        // 360 degrees horizontally
        for (double horizontal = -M_PI; horizontal <= M_PI; horizontal += M_PI / 8)
        {
          // 360 degrees vertically
          for (double vertical = -M_PI; vertical <= M_PI; vertical += M_PI / 8)
          {
            // Get every point on the direction vector
            wall_found = _octomap->castRay(_sensor_position, direction.rotate_IP(0, vertical, horizontal), center, true,
                                           _sensor_range);

            // Skip the points of the floor.
            if (center.z() < _min_obstacle_height)
              continue;

            if (wall_found)
            {
              ROS_DEBUG("Covered point at %f %f %f\n", center.x(), center.y(), center.z());
              point_inserted = _walls->insertRay(_sensor_position, center, _sensor_range);

              // Keep the positions, where the sensor must be in order to locate the wall points
              _points.push_back(_sensor_position);
            }
          }
        }
        // Next point in y
        _sensor_position.y() += 0.5 * _sensor_range;
      }

      // Next point in x
      _sensor_position.x() += 0.5 * _sensor_range;

      // Reinitialize z position
      _sensor_position.y() = _init_pose[1];
    }

    // Next point in z
    _sensor_position.z() += 0.5 * _sensor_range;

    // Reinitialize x and y position
    _sensor_position.x() = _init_pose[0];
    _sensor_position.y() = _init_pose[1];
  }
}

void CoverageFinder::publishCoveredSurface()
{
  ROS_INFO("Publishing covered surface. Use RViz to visualize it..\n");
  _walls->toMaxLikelihood();
  _walls->prune();
  octomap_msgs::Octomap msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "/map";
  msg.binary = true;
  msg.id = _walls->getTreeType();
  ROS_DEBUG("Tree class type: %s", msg.id.c_str());
  msg.resolution = _octomap->getResolution();
  if (octomap_msgs::binaryMapToMsg(*_walls, msg))
    _covered_pub.publish(msg);
}

void CoverageFinder::publishWaypoints()
{
  ROS_INFO("Publishing waypoints..\n");
  // Publish path as markers
  visualization_msgs::MarkerArray marker_array;

  marker_array.markers.resize(_points.size());

  for (std::size_t idx = 0; idx < _points.size(); idx++)
  {
    visualization_msgs::Marker marker;
    marker.header.frame_id = "/map";
    marker.header.stamp = ros::Time();
    marker.ns = "coverage_path_planning";
    marker.id = idx;
    marker.type = visualization_msgs::Marker::CUBE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.position.x = _points.at(idx).x();
    marker.pose.position.y = _points.at(idx).y();
    marker.pose.position.z = _points.at(idx).z();
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.scale.x = 0.15;
    marker.scale.y = 0.15;
    marker.scale.z = 0.15;
    marker.color.a = 1.0;
    marker.color.r = 0;
    marker.color.g = 0;
    marker.color.b = 1;

    marker_array.markers.push_back(marker);
  }

  _vis_pub.publish(marker_array);
  ROS_INFO("Finished!\n");
}

bool CoverageFinder::safeCheck(octomap::point3d center, octomap::point3d sensor_position)
{
  // Calculate distance for every axis
  double x_dist, y_dist, z_dist;
  x_dist = fabs(center.x() - sensor_position.x());
  y_dist = fabs(center.y() - sensor_position.y());
  z_dist = fabs(center.z() - sensor_position.z());

  // If any of the distances is smaller than the allowed, it is not safe
  if (x_dist < _uav_safety_offset || y_dist < _uav_safety_offset || z_dist < _uav_safety_offset)
    return 0;

  // It is safe, go on
  return 1;
}

}  // namespace drone_coverage
