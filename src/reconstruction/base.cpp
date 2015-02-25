#include "reconstruction/base.h"
#include <boost/filesystem.hpp>
#include <tf_conversions/tf_eigen.h>
#include <pcl_ros/transforms.h>

#include <pcl/common/common.h>
#include <pcl/common/transforms.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/filter.h>
#include <pcl/filters/passthrough.h>
#include <pcl/point_types.h>
#include <pcl/io/vtk_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/features/normal_3d.h>
#include <pcl/surface/gp3.h>
#include <pcl/surface/grid_projection.h>
#include <pcl/surface/vtk_smoothing/vtk_mesh_smoothing_laplacian.h>
#include <pcl/surface/concave_hull.h>
#include <pcl/surface/convex_hull.h>

namespace fs=boost::filesystem;


/** \brief Catches the Ctrl+C signal.
  */
void stopHandler(int s)
{
  printf("Caught signal %d\n",s);
  exit(1);
}


/** \brief Class constructor. Reads node parameters and initialize some properties.
  * @return
  * \param nh public node handler
  * \param nhp private node handler
  */
reconstruction::ReconstructionBase::ReconstructionBase(
  ros::NodeHandle nh, ros::NodeHandle nhp) : nh_(nh), nh_private_(nhp)
{
  // Setup the signal handler
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = stopHandler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);
}

/** \brief Make a greedy triangulation
  */
pcl::PolygonMesh::Ptr reconstruction::ReconstructionBase::greedyProjection(PointCloudRGB::Ptr cloud)
{
  // Normal estimation*
  pcl::NormalEstimation<PointRGB, pcl::Normal> n;
  pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
  pcl::search::KdTree<PointRGB>::Ptr tree(new pcl::search::KdTree<PointRGB>);
  tree->setInputCloud(cloud);
  n.setInputCloud(cloud);
  n.setSearchMethod(tree);
  n.setKSearch(50);
  n.compute(*normals);
  //* normals should not contain the point normals + surface curvatures

  // Concatenate the XYZ and normal fields
  pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr cloud_with_normals(new pcl::PointCloud<pcl::PointXYZRGBNormal>);
  pcl::concatenateFields(*cloud, *normals, *cloud_with_normals);

  // Create search tree*
  pcl::search::KdTree<pcl::PointXYZRGBNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointXYZRGBNormal>);
  tree2->setInputCloud(cloud_with_normals);

  // Greedy Projection
  pcl::PolygonMesh::Ptr triangles(new pcl::PolygonMesh());
  pcl::GreedyProjectionTriangulation<pcl::PointXYZRGBNormal> gp3;
  gp3.setSearchRadius(0.2);
  gp3.setMu(2.5);
  gp3.setMaximumNearestNeighbors(50);
  gp3.setMaximumSurfaceAngle(M_PI/2); // 90 degrees
  gp3.setMinimumAngle(M_PI/18); // 10 degrees
  gp3.setMaximumAngle(2*M_PI/3); // 120 degrees
  gp3.setNormalConsistency(false);
  gp3.setInputCloud(cloud_with_normals);
  gp3.setSearchMethod(tree2);
  gp3.reconstruct(*triangles);
  return triangles;
}

/** \brief Build the 3D
  */
void reconstruction::ReconstructionBase::build3D()
{
  // Read the graph poses
  vector< pair<string, tf::Transform> > cloud_poses;
  if (!readPoses(cloud_poses)) return;

  // Voxel size
  float voxel_size = 0.005;

  // Maximum distance from point to voxel (plus 5% to improve the borders)
  float max_dist = sqrt( (voxel_size*voxel_size)/2 );

  // Total of points processed
  int total_points = 0;

  // Load, convert and accumulate every pointcloud
  PointCloudXYZW::Ptr acc(new PointCloudXYZW);
  for (uint i=0; i<cloud_poses.size(); i++)
  {
    string file_idx = cloud_poses[i].first;
    ROS_INFO_STREAM("[Reconstruction:] Processing cloud " << file_idx.substr(0,file_idx.length()-4) << "/" << cloud_poses.size()-1);

    // Read the current pointcloud.
    string cloud_filename = params_.clouds_dir + cloud_poses[i].first;
    PointCloudRGB::Ptr in_cloud(new PointCloudRGB);
    if (pcl::io::loadPCDFile<PointRGB> (cloud_filename, *in_cloud) == -1)
    {
      ROS_WARN_STREAM("[Reconstruction:] Couldn't read the file: " << cloud_poses[i].first);
      continue;
    }

    // Increase the total of points processed
    total_points += in_cloud->points.size();

    ROS_INFO("Filtering");

    // Remove nans
    vector<int> indices;
    PointCloudRGB::Ptr cloud(new PointCloudRGB);
    pcl::removeNaNFromPointCloud(*in_cloud, *cloud, indices);
    indices.clear();

    // Voxel grid filter (used as x-y surface extraction. Note that leaf in z is very big)
    pcl::ApproximateVoxelGrid<PointRGB> grid;
    grid.setLeafSize(voxel_size, voxel_size, 0.5);
    grid.setDownsampleAllData(true);
    grid.setInputCloud(cloud);
    grid.filter(*cloud);

    // Remove isolated points
    pcl::RadiusOutlierRemoval<PointRGB> outrem;
    outrem.setInputCloud(cloud);
    outrem.setRadiusSearch(0.04);
    outrem.setMinNeighborsInRadius(50);
    outrem.filter(*cloud);
    pcl::StatisticalOutlierRemoval<PointRGB> sor;
    sor.setInputCloud(cloud);
    sor.setMeanK(40);
    sor.setStddevMulThresh(2.0);
    sor.filter(*cloud);

    ROS_INFO("Merging");

    // First iteration
    if (acc->points.size() == 0)
    {
      // Make this the accumulated
      pcl::copyPointCloud(*cloud, *acc);
      continue;
    }

    // Get the current cloud limits
    PointRGB min_pt, max_pt;
    pcl::getMinMax3D(*cloud, min_pt, max_pt);

    // Transform the accumulated cloud to the new cloud frame
    tf::Transform tf0 = cloud_poses[0].second;
    tf::Transform tfn0 = cloud_poses[i].second.inverse()*tf0;
    Eigen::Affine3d tfn0_eigen;
    transformTFToEigen(tfn0, tfn0_eigen);
    pcl::transformPointCloud(*acc, *acc, tfn0_eigen);

    // Convert the accumulated cloud to PointXY. So, we are supposing
    // the robot is navigating parallel to the surface and the camera line of sight is
    // perpendicular to the surface.
    PointCloudXY::Ptr acc_xy(new PointCloudXY);
    pcl::copyPointCloud(*acc, *acc_xy);

    // Extract the contour of accumulated cloud
    PointCloudXYZW::Ptr acc_for_contour(new PointCloudXYZW);
    pcl::ApproximateVoxelGrid<PointXYZRGBW> grid_contour;
    grid_contour.setLeafSize(voxel_size*10, voxel_size*10, voxel_size*10);
    grid_contour.setDownsampleAllData(true);
    grid_contour.setInputCloud(acc);
    grid_contour.filter(*acc_for_contour);

    PointCloudXY::Ptr acc_xy_for_contour(new PointCloudXY);
    pcl::copyPointCloud(*acc_for_contour, *acc_xy_for_contour);
    PointCloudXYZ::Ptr acc_xyz_for_contour(new PointCloudXYZ);
    pcl::copyPointCloud(*acc_xy_for_contour, *acc_xyz_for_contour);

    PointCloudXYZ::Ptr acc_contour_xyz(new PointCloudXYZ);
    pcl::ConcaveHull<PointXYZ> concave;
    concave.setAlpha(0.1);
    concave.setInputCloud(acc_xyz_for_contour);
    concave.reconstruct(*acc_contour_xyz);

    PointCloudXY::Ptr acc_contour_xy(new PointCloudXY);
    pcl::copyPointCloud(*acc_contour_xyz, *acc_contour_xy);

    // To search the closest neighbor in the projected accumulated cloud.
    pcl::KdTreeFLANN<PointXY> kdtree_neighbors;
    kdtree_neighbors.setInputCloud(acc_xy);

    // To search the closest point of the contour of the accumulated cloud.
    pcl::KdTreeFLANN<PointXY> kdtree_contour;
    kdtree_contour.setInputCloud(acc_contour_xy);

    // To search the closes point of the acc to the current cloud
    PointCloudXY::Ptr cloud_xy(new PointCloudXY);
    pcl::copyPointCloud(*cloud, *cloud_xy);
    pcl::KdTreeFLANN<PointXY> kdtree_cloud;
    kdtree_cloud.setInputCloud(cloud_xy);

    // Set all the accumulated processed to zero
    for (uint n=0; n<acc->points.size(); n++)
      acc->points[n].w = 0.0;

    // Get the maximum distance
    float max_contour_dist = 0.0;
    for (uint n=0; n<cloud->points.size(); n++)
    {
      // Get the cloud point (XY)
      PointXY sp;
      sp.x = cloud->points[n].x;
      sp.y = cloud->points[n].y;

      int K = 1;
      vector<int> acc_overlap_idx;
      vector<int> neighbor_idx(K);
      vector<float> neighbor_squared_dist(K);
      int num_neighbors = kdtree_neighbors.radiusSearch(sp, 2*max_dist, neighbor_idx, neighbor_squared_dist, K);
      if (num_neighbors > 0)
      {
        int N = 1;
        vector<int> contour_idx(N);
        vector<float> contour_squared_dist(N);
        if (kdtree_contour.nearestKSearch(sp, N, contour_idx, contour_squared_dist) > 0)
        {
          float dist = sqrt(contour_squared_dist[0]);
          if (dist > max_contour_dist)
            max_contour_dist = dist;
        }
      }
    }

    // Merge the current cloud with the accumulated
    for (uint n=0; n<cloud->points.size(); n++)
    {
      // Get the cloud point (XY)
      PointXY sp;
      sp.x = cloud->points[n].x;
      sp.y = cloud->points[n].y;

      // Extract the point
      PointXYZRGBW p;
      p.x = cloud->points[n].x;
      p.y = cloud->points[n].y;
      p.z = cloud->points[n].z;
      p.rgb = cloud->points[n].rgb;

      // Check if this point is inside or in the border of the current accumulated cloud
      int K = 10;
      vector<int> acc_overlap_idx;
      vector<int> neighbor_idx(K);
      vector<float> neighbor_squared_dist(K);
      int num_neighbors = kdtree_neighbors.radiusSearch(sp, 2*max_dist, neighbor_idx, neighbor_squared_dist, K);
      if (num_neighbors > 0)
      {
        // Filter the z and rgb parts of the point accordingly
        for (int h=0; h<num_neighbors; h++)
        {
          // Get the corresponding point into the accumulated
          PointXYZRGBW p_acc = acc->points[ neighbor_idx[h] ];

          // Save the acc value;
          acc_overlap_idx.push_back(neighbor_idx[h]);

          // Filter the z part
          p.z = (p.z + p_acc.z) / 2;
        }

        // Color blending
        // 1. Get the closest acc point
        int min_index = min_element(neighbor_squared_dist.begin(), neighbor_squared_dist.end()) - neighbor_squared_dist.begin();
        PointXYZRGBW p_acc = acc->points[ neighbor_idx[min_index] ];
        // 2. Get the contour closest point
        int N = 1;
        vector<int> contour_idx(N);
        vector<float> contour_squared_dist(N);
        if (kdtree_contour.nearestKSearch(sp, N, contour_idx, contour_squared_dist) > 0)
        {
          // 3. Extract the rgb parts of accumulated and current points
          int acc_rgb = *reinterpret_cast<const int*>(&(p_acc.rgb));
          uint8_t acc_r = (acc_rgb >> 16) & 0x0000ff;
          uint8_t acc_g = (acc_rgb >> 8) & 0x0000ff;
          uint8_t acc_b = (acc_rgb) & 0x0000ff;
          int cloud_rgb = *reinterpret_cast<const int*>(&(p.rgb));
          uint8_t cloud_r = (cloud_rgb >> 16) & 0x0000ff;
          uint8_t cloud_g = (cloud_rgb >> 8) & 0x0000ff;
          uint8_t cloud_b = (cloud_rgb) & 0x0000ff;

          // Apply the blending
          float alpha = ( max_contour_dist - sqrt(contour_squared_dist[0]) ) / max_contour_dist;
          cloud_r = (1-alpha)*acc_r + alpha*cloud_r;
          cloud_g = (1-alpha)*acc_g + alpha*cloud_g;
          cloud_b = (1-alpha)*acc_b + alpha*cloud_b;

          int32_t new_rgb = (cloud_r << 16) | (cloud_g << 8) | cloud_b;
          p.rgb = *reinterpret_cast<float*>(&new_rgb);
        }
        else
        {
          // TODO!
          ROS_WARN("[Reconstruction:] Impossible to find contour neighbors!");
        }

        // Determine if it is a point on the border or not.
        bool is_border = true;
        for (int h=0; h<num_neighbors; h++)
        {
          if (neighbor_squared_dist[h] < max_dist*max_dist)
          {
            is_border = false;
            break;
          }
        }

        // Is this the border?
        if (is_border)
        {
          // This point is in the border, add the filtered point.

          // Build the new point
          PointXYZRGBW p_new;
          p_new.x = cloud->points[n].x;
          p_new.y = cloud->points[n].y;
          p_new.rgb = p.rgb;
          p_new.z = p.z;
          p_new.w = 1.0;

          // Add the point
          acc->push_back(p_new);
        }
        else
        {
          // This point is inside the accumulated cloud, update.
          p_acc.z = p.z;
          p_acc.rgb = p.rgb;
          p_acc.w = 1.0;
          acc->points[ neighbor_idx[min_index] ] = p_acc;
        }
      }
      else
      {
        // This points is not inside the voxels of the accumulated cloud. So, may be
        // the point is a new point, far away from the accumulated borders, or may be
        // the point is inside a hole of the accumulated cloud.

        // Add the point
        acc->push_back(p);
      }

      // Fix the color of all internal accumulated points that has not been blended.
      for (uint h=0; h<acc_overlap_idx.size(); h++)
      {
        // Check if the color of this point has already been processed
        PointXYZRGBW p_acc = acc->points[ acc_overlap_idx[h] ];
        if (p_acc.w == 1.0) continue;

        // The point in xy
        PointXY sp;
        sp.x = p_acc.x;
        sp.y = p_acc.y;

        // Search the input cloud closest point.
        int N = 1;
        vector<int> cloud_idx(N);
        vector<float> cloud_squared_dist(N);
        if (kdtree_cloud.nearestKSearch(sp, N, cloud_idx, cloud_squared_dist) > 0)
        {
          PointRGB p_cloud = cloud->points[ cloud_idx[0] ];

          vector<int> contour_idx(N);
          vector<float> contour_squared_dist(N);
          if (kdtree_contour.nearestKSearch(sp, N, contour_idx, contour_squared_dist) > 0)
          {

            int acc_rgb = *reinterpret_cast<const int*>(&(p_acc.rgb));
            uint8_t acc_r = (acc_rgb >> 16) & 0x0000ff;
            uint8_t acc_g = (acc_rgb >> 8) & 0x0000ff;
            uint8_t acc_b = (acc_rgb) & 0x0000ff;
            int cloud_rgb = *reinterpret_cast<const int*>(&(p_cloud.rgb));
            uint8_t cloud_r = (cloud_rgb >> 16) & 0x0000ff;
            uint8_t cloud_g = (cloud_rgb >> 8) & 0x0000ff;
            uint8_t cloud_b = (cloud_rgb) & 0x0000ff;

            // Apply the blending
            float alpha = ( max_contour_dist - sqrt(contour_squared_dist[0]) ) / max_contour_dist;
            cloud_r = (1-alpha)*acc_r + alpha*cloud_r;
            cloud_g = (1-alpha)*acc_g + alpha*cloud_g;
            cloud_b = (1-alpha)*acc_b + alpha*cloud_b;

            int32_t new_rgb = (cloud_r << 16) | (cloud_g << 8) | cloud_b;
            p_acc.rgb = *reinterpret_cast<float*>(&new_rgb);
            p_acc.w = 1.0;
            acc->points[ acc_overlap_idx[h] ] = p_acc;
          }
        }
      }
    }

    // Return the acc to its original pose
    pcl::transformPointCloud(*acc, *acc, tfn0_eigen.inverse());
  }

  ROS_INFO("Filtering output cloud");

  // Remove the weight field
  PointCloudRGB::Ptr acc_rgb(new PointCloudRGB);
  pcl::copyPointCloud(*acc, *acc_rgb);

  // Filter the accumulated cloud
  pcl::ApproximateVoxelGrid<PointRGB> grid;
  grid.setLeafSize(voxel_size, voxel_size, voxel_size);
  grid.setDownsampleAllData(true);
  grid.setInputCloud(acc_rgb);
  grid.filter(*acc_rgb);
  pcl::RadiusOutlierRemoval<PointRGB> outrem_acc;
  outrem_acc.setInputCloud(acc_rgb);
  outrem_acc.setRadiusSearch(0.04);
  outrem_acc.setMinNeighborsInRadius(50);
  outrem_acc.filter(*acc_rgb);
  pcl::StatisticalOutlierRemoval<PointRGB> sor_acc;
  sor_acc.setInputCloud(acc_rgb);
  sor_acc.setMeanK(40);
  sor_acc.setStddevMulThresh(2.0);
  sor_acc.filter(*acc_rgb);

  // Generate a greedy projection
  /*
  ROS_INFO("[Reconstruction:] Generating Greedy projection...");
  pcl::PolygonMesh::Ptr triangles(new pcl::PolygonMesh());
  triangles = greedyProjection(acc_rgb);
  */

  // Save accumulated cloud
  ROS_INFO("[Reconstruction:] Saving pointclouds...");
  pcl::io::savePCDFile(params_.work_dir + "reconstruction.pcd", *acc_rgb);
  //pcl::io::saveVTKFile(params_.work_dir + "reconstruction.vtk", *triangles);
  ROS_INFO("[Reconstruction:] Accumulated clouds saved.");
  ROS_INFO_STREAM("[Reconstruction:] Points processed: " << total_points);

}

/** \brief Reads the reconstruction node parameters
  */
void reconstruction::ReconstructionBase::setParameters(string work_dir)
{
  Params params;

  // Operational directories
  if (work_dir[work_dir.length()-1] != '/')
    work_dir += "/";
  params.work_dir = work_dir;
  params.clouds_dir = work_dir + "clouds/";
  params.output_dir = work_dir + "clouds/output/";
  params.graph_file = work_dir + "graph_vertices.txt";
  setParams(params);

  // Create the output directory
  if (fs::is_directory(params.output_dir))
    fs::remove_all(params.output_dir);
  fs::path dir(params.output_dir);
  if (!fs::create_directory(dir))
    ROS_ERROR("[Reconstruction:] ERROR -> Impossible to create the output directory.");
}

/** \brief Reads the poses file and return the vector with the cloud names and the transformations
  * @return true if poses read correctly, false otherwise
  * \param the vector with the cloud filenames and the transformations
  */
bool reconstruction::ReconstructionBase::readPoses(vector< pair<string, tf::Transform> > &cloud_poses)
{
  // Init
  cloud_poses.clear();

  // Wait until poses file is unblocked
  while(fs::exists(params_.work_dir + ".graph.block"));

  // Get the pointcloud poses file
  ifstream poses_file(params_.graph_file.c_str());
  string line;
  while (getline(poses_file, line))
  {
    int i = 0;
    string cloud_name, value;
    double x, y, z, qx, qy, qz, qw;
    istringstream ss(line);
    while(getline(ss, value, ','))
    {
      if (i == 1)
        cloud_name = value + ".pcd";
      else if (i == 5)
        x = boost::lexical_cast<double>(value);
      else if (i == 6)
        y = boost::lexical_cast<double>(value);
      else if (i == 7)
        z = boost::lexical_cast<double>(value);
      else if (i == 8)
        qx = boost::lexical_cast<double>(value);
      else if (i == 9)
        qy = boost::lexical_cast<double>(value);
      else if (i == 10)
        qz = boost::lexical_cast<double>(value);
      else if (i == 11)
        qw = boost::lexical_cast<double>(value);
      i++;
    }
    // Build the pair and save
    tf::Vector3 t(x, y, z);
    tf::Quaternion q(qx, qy, qz, qw);
    tf::Transform transf(q, t);
    cloud_poses.push_back(make_pair(cloud_name, transf));
  }
  return true;
}