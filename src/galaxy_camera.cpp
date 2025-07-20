//
// Created by qiayuan on 6/27/20.
//
#include <pluginlib/class_list_macros.h>
#include <galaxy_camera.h>
#include <utility>
#include <ros/time.h>

#define GX_MAX_STRING_SIZE 1024

namespace galaxy_camera
{
PLUGINLIB_EXPORT_CLASS(galaxy_camera::GalaxyCameraNodelet, nodelet::Nodelet)
GalaxyCameraNodelet::GalaxyCameraNodelet()
{
}

void GalaxyCameraNodelet::onInit()
{
  nh_ = this->getPrivateNodeHandle();

  nh_.param("camera_frame_id", image_.header.frame_id, std::string("camera_optical_frame"));
  nh_.param("camera_name", camera_name_, std::string("camera"));
  nh_.param("imu_name", imu_name_, std::string("gimbal_imu"));
  nh_.param("camera_info_url", camera_info_url_, std::string(""));
  nh_.param("image_width", image_width_, 1280);
  nh_.param("image_height", image_height_, 1024);
  nh_.param("image_offset_x", image_offset_x_, 0);
  nh_.param("image_offset_y", image_offset_y_, 0);
  nh_.param("pixel_format", pixel_format_, std::string("bgr8"));
  nh_.param("frame_id", frame_id_, std::string("camera_optical_frame"));
  nh_.param("camera_sn", camera_sn_, std::string(""));
  nh_.param("enable_imu_trigger", enable_imu_trigger_, false);
  nh_.param("raising_filter_value", raising_filter_value_, 0);
  nh_.param("frame_rate", frame_rate_, 210.0);
  nh_.param("exposure_auto", exposure_auto_, true);
  nh_.param("exposure_value", exposure_value_, std::float_t(2000.));
  nh_.param("stop_grab", stop_grab_, false);
  info_manager_.reset(new camera_info_manager::CameraInfoManager(nh_, camera_name_, camera_info_url_));

  image_transport::ImageTransport it(nh_);
  pub_ = it.advertiseCamera("/galaxy_camera/" + nh_.getNamespace() + "/image_raw", 1);
  // check for default camera info
  if (!info_manager_->isCalibrated())
  {
    info_manager_->setCameraName(camera_name_);
    sensor_msgs::CameraInfo camera_info;
    camera_info.header.frame_id = image_.header.frame_id;
    camera_info.width = image_width_;
    camera_info.height = image_height_;
    info_manager_->setCameraInfo(camera_info);
  }
  ROS_INFO("Starting '%s' at %dx%d", camera_name_.c_str(), image_width_, image_height_);
  info_ = std::move(info_manager_->getCameraInfo());
  info_.header.frame_id = frame_id_;
  image_.header.frame_id = frame_id_;
  image_.height = image_height_;
  image_.width = image_width_;
  image_.step = image_width_ * 3;
  image_.data.resize(image_.height * image_.step);
  image_.encoding = pixel_format_;
  img_ = new char[image_.height * image_.step];

  assert(GXInitLib() == GX_STATUS_SUCCESS);  // Initializes the library.
  uint32_t device_num = 0;
  GXUpdateDeviceList(&device_num, 1000);
  assert(device_num > 0);

  GX_OPEN_PARAM open_param;
  // Opens the device.
  if (device_num > 1)
  {
    assert(!camera_sn_.empty());
    open_param.accessMode = GX_ACCESS_EXCLUSIVE;
    open_param.openMode = GX_OPEN_SN;
    open_param.pszContent = (char*)&camera_sn_[0];
  }
  else
  {
    open_param.accessMode = GX_ACCESS_EXCLUSIVE;
    open_param.openMode = GX_OPEN_INDEX;
    open_param.pszContent = (char*)"1";
  }

  // Get handle
  assert(GXOpenDevice(&open_param, &dev_handle_) == GX_STATUS_SUCCESS);
  ROS_INFO("Camera Opened");

  // Get camera serial number and model name
  char sn[GX_MAX_STRING_SIZE] = {0};
  char model[GX_MAX_STRING_SIZE] = {0};
  size_t sn_size = sizeof(sn);
  size_t model_size = sizeof(model);

  GX_STATUS status_sn = GXGetString(dev_handle_, GX_STRING_DEVICE_SERIAL_NUMBER, sn, &sn_size);
  GX_STATUS status_model = GXGetString(dev_handle_, GX_STRING_DEVICE_MODEL_NAME, model, &model_size);

  if (status_sn == GX_STATUS_SUCCESS) {
    ROS_INFO("Camera Serial Number: %s", sn);
  } else {
    ROS_ERROR("Failed to get camera serial number");
  }

  if (status_model == GX_STATUS_SUCCESS) {
    ROS_INFO("Camera Model Name: %s", model);
  } else {
    ROS_ERROR("Failed to get camera model name");
  }

  int64_t format = 0;
  if (pixel_format_ == "mono8")
    format = GX_PIXEL_FORMAT_MONO8;
  if (pixel_format_ == "mono16")
    format = GX_PIXEL_FORMAT_MONO16;
  if (pixel_format_ == "bgr8")
    format = GX_PIXEL_FORMAT_BAYER_GB8;
  if (pixel_format_ == "rgb8")
    format = GX_PIXEL_FORMAT_BAYER_RG8;
  if (pixel_format_ == "bgra8")
    format = GX_PIXEL_FORMAT_BAYER_BG8;
  if (format == 0)
    static_assert(true, "Illegal format");

  //   assert(GXSetEnum(dev_handle_, GX_ENUM_PIXEL_FORMAT, format) == GX_STATUS_SUCCESS);
  assert(GXSetInt(dev_handle_, GX_INT_WIDTH, image_width_) == GX_STATUS_SUCCESS);
  assert(GXSetInt(dev_handle_, GX_INT_HEIGHT, image_height_) == GX_STATUS_SUCCESS);
  assert(GXSetInt(dev_handle_, GX_INT_OFFSET_X, image_offset_x_) == GX_STATUS_SUCCESS);
  assert(GXSetInt(dev_handle_, GX_INT_OFFSET_Y, image_offset_y_) == GX_STATUS_SUCCESS);
  assert(GXSetEnum(dev_handle_, GX_ENUM_BALANCE_WHITE_AUTO, GX_BALANCE_WHITE_AUTO_CONTINUOUS) == GX_STATUS_SUCCESS);
  assert(GXSetEnum(dev_handle_, GX_ENUM_ACQUISITION_FRAME_RATE_MODE, GX_ENUM_COVER_FRAMESTORE_MODE_ON) ==
         GX_STATUS_SUCCESS);
  assert(GXSetFloat(dev_handle_, GX_FLOAT_ACQUISITION_FRAME_RATE, frame_rate_) == GX_STATUS_SUCCESS);

  if (enable_imu_trigger_)
  {
    assert(GXSetEnum(dev_handle_, GX_ENUM_ACQUISITION_FRAME_RATE_MODE, GX_ACQUISITION_FRAME_RATE_MODE_OFF) ==
           GX_STATUS_SUCCESS);
    assert(GXSetEnum(dev_handle_, GX_ENUM_TRIGGER_MODE, GX_TRIGGER_MODE_ON) == GX_STATUS_SUCCESS);
    assert(GXSetEnum(dev_handle_, GX_ENUM_TRIGGER_SOURCE, GX_TRIGGER_SOURCE_LINE3) == GX_STATUS_SUCCESS);
    assert(GXSetEnum(dev_handle_, GX_ENUM_TRIGGER_ACTIVATION, GX_TRIGGER_ACTIVATION_RISINGEDGE) == GX_STATUS_SUCCESS);
    assert(GXSetFloat(dev_handle_, GX_FLOAT_TRIGGER_FILTER_RAISING, raising_filter_value_) == GX_STATUS_SUCCESS);
    assert(GXSetEnum(dev_handle_, GX_ENUM_LINE_SELECTOR, GX_ENUM_LINE_SELECTOR_LINE3) == GX_STATUS_SUCCESS);
    assert(GXSetEnum(dev_handle_, GX_ENUM_LINE_MODE, GX_ENUM_LINE_MODE_INPUT) == GX_STATUS_SUCCESS);

    trigger_sub_ = nh_.subscribe("/rm_hw/" + imu_name_ + "/trigger_time", 50, &galaxy_camera::GalaxyCameraNodelet::triggerCB, this);
  }
  else
  {
    assert(GXSetEnum(dev_handle_, GX_ENUM_TRIGGER_MODE, GX_TRIGGER_MODE_OFF) == GX_STATUS_SUCCESS);
  }

  GXRegisterCaptureCallback(dev_handle_, nullptr, onFrameCB);

  if (GXStreamOn(dev_handle_) == GX_STATUS_SUCCESS)
  {
    ROS_INFO("Stream On.");
  }

  ros::NodeHandle p_nh("/galaxy_camera" + nh_.getNamespace() + "/galaxy_camera_dy");
  srv_ = new dynamic_reconfigure::Server<CameraConfig>(p_nh);
  dynamic_reconfigure::Server<CameraConfig>::CallbackType cb =
      boost::bind(&GalaxyCameraNodelet::reconfigCB, this, _1, _2);
  srv_->setCallback(cb);

  if (enable_imu_trigger_)
  {
    imu_trigger_client_ = nh_.serviceClient<rm_msgs::EnableImuTrigger>("imu_trigger");
    rm_msgs::EnableImuTrigger imu_trigger_srv;
    imu_trigger_srv.request.imu_name = imu_name_;
    imu_trigger_srv.request.enable_trigger = true;
    while (!imu_trigger_client_.call(imu_trigger_srv))
    {
      ROS_WARN("Failed to call service enable_imu_trigger. Retry now.");
      ros::Duration(1).sleep();
    }
    if (imu_trigger_srv.response.is_success)
      ROS_INFO("Enable imu %s trigger camera successfully", imu_name_.c_str());
    else
      ROS_ERROR("Failed to enable imu %s trigger camera", imu_name_.c_str());
    enable_trigger_timer_ = nh_.createTimer(ros::Duration(0.5), &GalaxyCameraNodelet::enableTriggerCB, this);
  }

  camera_change_sub = nh_.subscribe("/camera_name", 50, &galaxy_camera::GalaxyCameraNodelet::cameraChange, this);
}

void GalaxyCameraNodelet::cameraChange(const std_msgs::String camera_change)
{
  if (strcmp(camera_change.data.c_str(), "galaxy_camera") == 0)
    GXStreamOn(dev_handle_);
  else
    GXStreamOff(dev_handle_);
}

void GalaxyCameraNodelet::enableTriggerCB(const ros::TimerEvent&)
{
  if ((ros::Time::now() - last_trigger_time_).toSec() > 1.0)
  {
    ROS_INFO("Try to enable imu %s to trigger camera.", imu_name_.c_str());
    rm_msgs::EnableImuTrigger imu_trigger_srv;
    imu_trigger_srv.request.imu_name = imu_name_;
    imu_trigger_srv.request.enable_trigger = true;
    imu_trigger_client_.call(imu_trigger_srv);
    if (trigger_not_sync_)
      trigger_not_sync_ = false;
  }
}

void GalaxyCameraNodelet::triggerCB(const sensor_msgs::TimeReference::ConstPtr& time_ref)
{
  last_trigger_time_ = time_ref->time_ref;
  galaxy_camera::TriggerPacket pkt;
  pkt.trigger_time_ = time_ref->time_ref;
  pkt.trigger_counter_ = time_ref->header.seq;
  fifoWrite(pkt);
}

void GalaxyCameraNodelet::fifoWrite(TriggerPacket pkt)
{
  if (fifo_front_ == (fifo_rear_ + 1) % FIFO_SIZE)
  {
    ROS_WARN("FIFO overflow!");
    return;
  }
  fifo_[fifo_rear_] = pkt;
  fifo_rear_ = (fifo_rear_ + 1) % FIFO_SIZE;
}

bool GalaxyCameraNodelet::fifoRead(TriggerPacket& pkt)
{
  if (fifo_front_ == fifo_rear_)
    return false;
  pkt = fifo_[fifo_front_];
  fifo_front_ = (fifo_front_ + 1) % FIFO_SIZE;
  return true;
}

void GalaxyCameraNodelet::onFrameCB(GX_FRAME_CALLBACK_PARAM* pFrame)
{
  if (pFrame->status == GX_FRAME_STATUS_SUCCESS)
  {
    ros::Time now = ros::Time::now();
    if (enable_imu_trigger_)
    {
      if (!trigger_not_sync_)
      {
        TriggerPacket pkt;
        while (!fifoRead(pkt))
        {
          ros::Duration(0.001).sleep();
        }
        if (pkt.trigger_counter_ != receive_trigger_counter_++)
        {
          ROS_WARN("Trigger not in sync!");
          trigger_not_sync_ = true;
        }
        else if ((now - pkt.trigger_time_).toSec() < 0)
        {
          ROS_WARN("Trigger not in sync! Maybe any CAN frames have be dropped?");
          trigger_not_sync_ = true;
        }
        else if ((now - pkt.trigger_time_).toSec() > 0.06)
        {
          ROS_WARN("Trigger not in sync! Maybe imu %s does not actually trigger camera?", imu_name_.c_str());
          trigger_not_sync_ = true;
        }
        else
        {
          image_.header.stamp = pkt.trigger_time_;
          info_.header.stamp = pkt.trigger_time_;
        }
      }
      if (trigger_not_sync_)
      {
        fifo_front_ = fifo_rear_;
        rm_msgs::EnableImuTrigger imu_trigger_srv;
        imu_trigger_srv.request.imu_name = imu_name_;
        imu_trigger_srv.request.enable_trigger = false;
        imu_trigger_client_.call(imu_trigger_srv);
        ROS_INFO("Disable imu %s from triggering camera.", imu_name_.c_str());
        receive_trigger_counter_ = fifo_[fifo_rear_ - 1].trigger_counter_ + 1;
        return;
      }
    }
    else
    {
      image_.header.stamp = now;
      info_.header.stamp = now;
    }
    DxRaw8toRGB24((void*)pFrame->pImgBuf, img_, pFrame->nWidth, pFrame->nHeight, RAW2RGB_NEIGHBOUR, BAYERBG, false);
    //    assert(GXGetFloat(dev_handle_, GX_FLOAT_GAMMA_PARAM, &gamma_param_) == GX_STATUS_SUCCESS);
    //
    //    int nLutLength;
    //    assert(DxGetGammatLut(gamma_param_, NULL, &nLutLength) == DX_OK);
    //    float* pGammaLut = new float[nLutLength];
    //    assert(DxGetGammatLut(gamma_param_, pGammaLut, &nLutLength) == DX_OK);
    //
    //    assert(GXGetInt(dev_handle_, GX_INT_CONTRAST_PARAM, &contrast_param_) == GX_STATUS_SUCCESS);
    //    assert(DxGetContrastLut(contrast_param_, NULL, &nLutLength) == DX_OK);
    //    //      ROS_INFO("%d",nLutLength);
    //    float* pContrastLut = new float[nLutLength];
    //    //      ROS_INFO("%p",pContrastLut);
    //    assert(DxGetContrastLut(contrast_param_, pContrastLut, &nLutLength) == DX_OK);
    //    switch (improve_mode_)
    //    {
    //      case 0:
    //        assert(DxImageImprovment(img_, img_, pFrame->nWidth, pFrame->nHeight, 0, pContrastLut, pGammaLut) ==
    //        DX_OK); break;
    //      case 1:
    //        assert(DxImageImprovment(img_, img_, pFrame->nWidth, pFrame->nHeight, 0, NULL, pGammaLut) == DX_OK);
    //        break;
    //      case 2:
    //        assert(DxImageImprovment(img_, img_, pFrame->nWidth, pFrame->nHeight, 0, pContrastLut, NULL) == DX_OK);
    //        break;
    //      case 3:
    //        break;
    //    }
    //
    //    if (pGammaLut != NULL)
    //      delete[] pGammaLut;
    //    if (pContrastLut != NULL)
    //      delete[] pContrastLut;
    memcpy((char*)(&image_.data[0]), img_, image_.step * image_.height);
    pub_.publish(image_, info_);
  }
  else if (pFrame->status == GX_FRAME_STATUS_INCOMPLETE)
    ROS_ERROR("Frame status incomplete");
  else if (pFrame->status == GX_FRAME_STATUS_INVALID_IMAGE_INFO)
    ROS_ERROR("Frame status invalid");
}

void GalaxyCameraNodelet::reconfigCB(CameraConfig& config, uint32_t level)
{
  (void)level;
  if (!exposure_initialized_flag_)
  {
    config.exposure_value = exposure_value_;
    config.exposure_auto = exposure_auto_;
    config.stop_grab = stop_grab_;
    exposure_initialized_flag_ = true;
  }
  if (!config.stop_grab)
    GXStreamOn(dev_handle_);
  else
    GXStreamOff(dev_handle_);
  // Exposure
  if (config.exposure_auto)
  {
    double value;
    GXSetFloat(dev_handle_, GX_FLOAT_AUTO_EXPOSURE_TIME_MAX, config.exposure_max);
    GXSetFloat(dev_handle_, GX_FLOAT_AUTO_EXPOSURE_TIME_MIN, config.exposure_min);
    GXSetEnum(dev_handle_, GX_ENUM_EXPOSURE_AUTO, GX_EXPOSURE_AUTO_CONTINUOUS);
    GXGetFloat(dev_handle_, GX_FLOAT_EXPOSURE_TIME, &value);
    config.exposure_value = value;
  }
  else
  {
    GXSetEnum(dev_handle_, GX_ENUM_EXPOSURE_AUTO, GX_EXPOSURE_AUTO_OFF);
    GXSetFloat(dev_handle_, GX_FLOAT_EXPOSURE_TIME, config.exposure_value);
  }

  // Gain
  if (config.gain_auto)
  {
    GXSetFloat(dev_handle_, GX_FLOAT_AUTO_GAIN_MIN, config.gain_min);
    GXSetFloat(dev_handle_, GX_FLOAT_AUTO_GAIN_MAX, config.gain_max);
    GXSetEnum(dev_handle_, GX_ENUM_GAIN_AUTO, GX_GAIN_AUTO_CONTINUOUS);
    GXGetFloat(dev_handle_, GX_FLOAT_GAIN, &config.gain_value);
  }
  else
  {
    GXSetEnum(dev_handle_, GX_ENUM_GAIN_AUTO, GX_GAIN_AUTO_OFF);
    GXSetFloat(dev_handle_, GX_FLOAT_GAIN, config.gain_value);
  }

  // Black level
  if (config.black_auto)
  {
    GXSetEnum(dev_handle_, GX_ENUM_BLACKLEVEL_AUTO, GX_BLACKLEVEL_AUTO_CONTINUOUS);
    GXGetFloat(dev_handle_, GX_FLOAT_BLACKLEVEL, &config.black_value);
  }
  else
  {
    GXSetEnum(dev_handle_, GX_ENUM_BLACKLEVEL_AUTO, GX_BLACKLEVEL_AUTO_OFF);
    GXSetFloat(dev_handle_, GX_FLOAT_BLACKLEVEL, config.black_value);
  }
  // Balance White
  switch (config.white_selector)
  {
    case 0:
      GXSetEnum(dev_handle_, GX_ENUM_BALANCE_RATIO_SELECTOR, GX_BALANCE_RATIO_SELECTOR_RED);
      break;
    case 1:
      GXSetEnum(dev_handle_, GX_ENUM_BALANCE_RATIO_SELECTOR, GX_BALANCE_RATIO_SELECTOR_GREEN);
      break;
    case 2:
      GXSetEnum(dev_handle_, GX_ENUM_BALANCE_RATIO_SELECTOR, GX_BALANCE_RATIO_SELECTOR_BLUE);
      break;
  }
  if (last_channel_ != config.white_selector)
  {
    GXGetFloat(dev_handle_, GX_FLOAT_BALANCE_RATIO, &config.white_value);
    last_channel_ = config.white_selector;
  }
  if (config.white_auto)
  {
    GXSetEnum(dev_handle_, GX_ENUM_BALANCE_WHITE_AUTO, GX_BALANCE_WHITE_AUTO_CONTINUOUS);
    GXGetFloat(dev_handle_, GX_FLOAT_BALANCE_RATIO, &config.white_value);
  }
  else
  {
    GXSetEnum(dev_handle_, GX_ENUM_BALANCE_WHITE_AUTO, GX_BALANCE_WHITE_AUTO_OFF);
    GXSetFloat(dev_handle_, GX_FLOAT_BALANCE_RATIO, config.white_value);
  }
  assert(GXGetFloat(dev_handle_, GX_FLOAT_GAMMA_PARAM, &config.gamma_param) == GX_STATUS_SUCCESS);
  improve_mode_ = config.improve_mode;
}

GalaxyCameraNodelet::~GalaxyCameraNodelet()
{
  GXStreamOff(dev_handle_);
  GXUnregisterCaptureCallback(dev_handle_);
  GXCloseDevice(dev_handle_);
  GXCloseLib();
}

GX_DEV_HANDLE GalaxyCameraNodelet::dev_handle_;
char* GalaxyCameraNodelet::img_;
sensor_msgs::Image GalaxyCameraNodelet::image_;
image_transport::CameraPublisher GalaxyCameraNodelet::pub_;
sensor_msgs::CameraInfo GalaxyCameraNodelet::info_;
ros::ServiceClient GalaxyCameraNodelet::imu_trigger_client_;
std::string GalaxyCameraNodelet::imu_name_;
bool GalaxyCameraNodelet::enable_imu_trigger_;
bool GalaxyCameraNodelet::trigger_not_sync_ = false;
const int GalaxyCameraNodelet::FIFO_SIZE = 1023;
int GalaxyCameraNodelet::fifo_front_ = 0;
int GalaxyCameraNodelet::fifo_rear_ = 0;
struct TriggerPacket GalaxyCameraNodelet::fifo_[FIFO_SIZE];
uint32_t GalaxyCameraNodelet::receive_trigger_counter_ = 0;
double GalaxyCameraNodelet::gamma_param_{};
int64_t GalaxyCameraNodelet::contrast_param_{};
int GalaxyCameraNodelet::improve_mode_ = 1;
}  // namespace galaxy_camera
