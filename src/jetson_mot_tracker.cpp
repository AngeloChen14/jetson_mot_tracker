#include "jetson_mot_tracker/MotTracker.hpp"
// STD
#include <string>

namespace jetson_mot_tracker {
MotTracker::MotTracker(ros::NodeHandle& nodeHandle)
 : nodeHandle_(nodeHandle), imageTransport_(nodeHandle), tf2_(buffer_) //,tf2_filter_(detect_sub_, buffer_, "camera_base", 10, 0)
{
  if (!readParameters()) {
    ROS_ERROR("Could not read parameters.");
    ros::requestShutdown();
  }
  // tf2_filter_.setTargetFrame(detectGlobalFrame_);
  // detect_sub_.subscribe(nodeHandle_,detectSubTopic_,10);
  // tf2_filter_.registerCallback(boost::bind(&MotTracker::detectCallback,this,_1));
  // tf2_filter_.registerCallback(&MotTracker::detectCallback,this);
  detect_sub_ = nodeHandle_.subscribe(detectSubTopic_, 10,
                                      &MotTracker::detectCallback, this);
  depth_sub_  = imageTransport_.subscribe(depthSubTopic_, 10,
                                      &MotTracker::depthCallback, this);
  caminfo_sub_ = nodeHandle_.subscribe(caminfoSubTopic_, 5,
                                      &MotTracker::caminfoCallback, this);
  body_marker_publisher_ = nodeHandle_.advertise<visualization_msgs::MarkerArray>("body_tracking_data", 10);
  detection_publisher_ = nodeHandle_.advertise<geometry_msgs::PoseArray>("detection",10);
  // angle_pub_ = nodeHandle_.advertise<std_msgs::Float64>("/camera_angle", 100, true);
  timer1_ = nodeHandle_.createTimer(ros::Duration(0.05),&MotTracker::timer1Callback,this);  

  ROS_INFO("Successfully launched jetson mot tracker node.");
}

MotTracker::~MotTracker()
{

}

bool MotTracker::readParameters()
{
  if (!nodeHandle_.getParam("detect_subscriber_topic", detectSubTopic_)) return false;
  if (!nodeHandle_.getParam("depthimage_subscriber_topic", depthSubTopic_)) return false;
  if (!nodeHandle_.getParam("caminfo_subscriber_topic", caminfoSubTopic_)) return false;
  if (!nodeHandle_.getParam("detect_global_frame", detectGlobalFrame_)) return false;
  return true;
}

void MotTracker::detectCallback(const vision_msgs::Detection2DArray& msg_raw)
{
  //ros::Time start_time=ros::Time::now();
  // ROS_INFO_STREAM("detectCallback entered");
  vision_msgs::Detection2DArray msg_updated;
  std::vector<geometry_msgs::Point> detects;
  // double duration = (msg_raw.header.stamp - lastUpdateTime_).toSec();
  // ROS_INFO_STREAM("Update duration is: "<<duration);
  // lastUpdateTime_ = msg_raw.header.stamp;

  positionCalculator(msg_raw, msg_updated,depth_image_,cam_model_); //Update detections with 3D positions
  detects = detectPreprocessing(msg_updated);

  trackers_.Update(detects);
  //ROS_INFO_STREAM("Track Duration is:"<<(ros::Time::now()-start_time).toNSec()<<"ns");
}

void MotTracker::depthCallback(const sensor_msgs::ImageConstPtr& msg)
{
  // ROS_INFO_STREAM("depthCallback entered");
  depth_image_ = *msg;
}

void MotTracker::caminfoCallback(const sensor_msgs::CameraInfo& message)
{
  // ROS_INFO_STREAM("caminfoCallback entered");
  cam_model_.fromCameraInfo(message);
  // ROS_INFO_STREAM("camera initial successfully!");
  // rgb_cam_info_ = message;
}

void MotTracker::timer1Callback(const ros::TimerEvent& e)
{
  double duration = (e.current_real -e.last_real).toSec();
  trackers_.Predict(duration);
  if(body_marker_publisher_.getNumSubscribers() > 0 && trackers_.GetTracks().size()>0)
    bodyMarkerPublish(trackers_);
}

bool MotTracker::positionCalculator(const vision_msgs::Detection2DArray& detects,vision_msgs::Detection2DArray& detects_out,\
                                                     const sensor_msgs::Image& depthimage, image_geometry::PinholeCameraModel& cam_model)
{
  // ROS_INFO_STREAM("rgb and depth time diff is"<<(depthimage.header.stamp - detects.header.stamp).toSec());
  bool detect_flag =false;
  if(cam_model.initialized() && !depthimage.encoding.empty())
  {
     detects_out = detects;
     cv_bridge::CvImagePtr cv_ptr;
    try
    {
      cv_ptr = cv_bridge::toCvCopy(depthimage,depthimage.encoding);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("cv_bridge exception: %s", e.what());
    }
    cv_bridge::CvImage cv_img_blur;
    cv::medianBlur(cv_ptr->image,cv_img_blur.image,3);
    cv::Point2d uv;
    cv::Point3d pt_cv;
    for(auto& detect:detects_out.detections)
    {
      assert(detect.results.size()==1);
      int x = detect.bbox.center.x;
      int y = detect.bbox.center.y;
      float depth = cv_img_blur.image.at<float>(y,x);
      if(depth > 0)
      {
        uv.x=x;uv.y=y;
        pt_cv = cam_model.projectPixelTo3dRay(uv);
        detect.results.back().pose.pose.position.x = pt_cv.x*depth;
        detect.results.back().pose.pose.position.y = pt_cv.y*depth;
        detect.results.back().pose.pose.position.z = depth;
        detect.header.frame_id = depthimage.header.frame_id;
        detect.header.stamp = detects.header.stamp;
        detect_flag = true;
        // ROS_INFO_STREAM("Target Position at "<<x<<','<<y<<"is: "<<detect);
      }
      // else
      // {
      //   // detect = detects_out.detections.erase(detect);
      // }  
    }
    // ROS_INFO_STREAM(detects_out);
  }
  else
  {
    ROS_ERROR("Camera Model Not Initialized!");
  }

  return detect_flag;
}


std::vector<geometry_msgs::Point> MotTracker::detectPreprocessing(const vision_msgs::Detection2DArray& detArray)
{
  float kMinDistance = 0.2; //minimal distance between detections to avoid repeation
  std::vector<geometry_msgs::Point> dets;
  geometry_msgs::PointStamped point_in,point_out;
  geometry_msgs::TransformStamped transformStamped;
  geometry_msgs::PoseArray detection_msg;
  geometry_msgs::Pose    pos_msg;
  try {
     transformStamped = buffer_.lookupTransform(detectGlobalFrame_, detArray.header.frame_id, detArray.header.stamp,ros::Duration(0.1));
  }
  catch (tf2::TransformException &ex) 
  {
    ROS_WARN("Failure %s\n", ex.what()); //Print exception which was caught
  }
  
  // ROS_INFO_STREAM("Transform duration is:"<<(transformStamped.header.stamp - detArray.header.stamp).toSec());
  for(auto& det:detArray.detections)
  {
    bool minDistanceFlag = true;
    if(fabs(det.results.back().pose.pose.position.z) > 0 && det.results.back().score > kMinConfidence )
    {
      point_in.header = det.header;
      point_in.point = det.results.back().pose.pose.position;
      try 
      {
        tf2::doTransform(point_in,point_out,transformStamped);
        // buffer_.transform(point_in, point_out, detectGlobalFrame_,ros::Duration(0.1));
        // ROS_INFO_STREAM("Before:"<<point_in<<"\nAfter:"<<point_out<<"\nTransform:"<<transformStamped);
        // ROS_INFO("point of detect in global frame of odom is: (x:%f y:%f z:%f)\n", 
        //       point_out.point.x,
        //       point_out.point.y,
        //       point_out.point.z);
        
      }
      catch (tf2::TransformException &ex) 
      {
        ROS_WARN("Failure %s\n", ex.what()); //Print exception which was caught
        break;
      }
      for(auto& point:dets)
      {
        auto dis = sqrt(pow((point_out.point.x - point.x),2) + pow((point_out.point.y - point.y),2));
        if(dis < kMinDistance)
        {
          minDistanceFlag = false;
          break;
        }
      }
      if(minDistanceFlag)
      { 
        dets.push_back(point_out.point);
        pos_msg.position = point_out.point;
        detection_msg.poses.push_back(pos_msg);
      }
    }
  }
  if(detection_msg.poses.size()>0)
  {
    detection_msg.header = point_out.header;
    detection_publisher_.publish(detection_msg);
  }
  return dets;
}

void MotTracker::bodyMarkerPublish(Tracker& trackers)
{
  std::map<int, Track> tracks = trackers.GetTracks();
  visualization_msgs::MarkerArray markerArray;
  visualization_msgs::Marker marker;
  for(auto& track:tracks)
  {
    if(track.second.state == 1)
    {
      marker.header.frame_id = detectGlobalFrame_;
      marker.header.stamp = ros::Time::now();

      marker.lifetime = ros::Duration(0.25);
      marker.id = track.first * 100;
      marker.type  = visualization_msgs::Marker::SPHERE;
      Color color = BODY_COLOR_PALETTE[(track.first) % BODY_COLOR_PALETTE.size()];

      marker.color.a = color.a;
      marker.color.r = color.r;
      marker.color.g = color.g;
      marker.color.b = color.b;

      marker.scale.x = 0.25;
      marker.scale.y = 0.25;
      marker.scale.z = 0.25;

      marker.pose.position = track.second.GetStateAsPoint();
      marker.pose.orientation.w = 1.0f;
      marker.points.push_back(track.second.GetVelAsPoint());
      markerArray.markers.push_back(marker);
      
      marker.id = track.first;
      marker.type  = visualization_msgs::Marker::TEXT_VIEW_FACING;

      marker.color.a = 1.0;
      marker.color.r = 1.0;
      marker.color.g = 1.0;
      marker.color.b = 1.0;

      marker.scale.x = 0.5;
      marker.scale.y = 0.5;
      marker.scale.z = 0.5;
      marker.pose.position.z += 0.5f;
      marker.text = std::to_string(track.first);
      
      markerArray.markers.push_back(marker);
    }
  }
  if(markerArray.markers.size()>0)
    body_marker_publisher_.publish(markerArray);
}

} /* namespace */