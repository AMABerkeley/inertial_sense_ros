#include "inertial_sense.h"
#include <chrono>
#include <stddef.h>
#include <unistd.h>

#include <ros/console.h>

InertialSenseROS::InertialSenseROS() :
  nh_(), nh_private_("~")
{
  nh_private_.param<std::string>("port", port_, "/dev/ttyUSB0");
  nh_private_.param<int>("baudrate", baudrate_, 3000000);
  nh_private_.param<std::string>("frame_id", frame_id_, "body");

  /// Connect to the uINS

  memset(&serial_, 0, sizeof(serial_));
  serialPortPlatformInit(&serial_);
  ROS_INFO("Connecting to serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
  if (serialPortOpen(&serial_, port_.c_str(), baudrate_, true) != 1)
  {
    ROS_FATAL("inertialsense: Unable to open serial port \"%s\", at %d baud", port_.c_str(), baudrate_);
    exit(0);
  }
  else
    ROS_INFO("Connected to uINS on \"%s\", at %d baud", port_.c_str(), baudrate_);
  
  // Initialize the IS parser
  comm_.buffer = message_buffer_;
  comm_.bufferSize = sizeof(message_buffer_);
  is_comm_init(&comm_);
  get_flash_config();

  // Make sure the navigation rate is right, if it's not, then we need to change and reset it.
  int nav_dt_ms;
  if (nh_private_.getParam("navigation_dt_ms", nav_dt_ms))
  {
    if (nav_dt_ms != flash_.startupNavDtMs)
    {
      int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offsetof(nvm_flash_cfg_t, startupNavDtMs), sizeof(uint32_t), &nav_dt_ms);
      serialPortWrite(&serial_, message_buffer_, messageSize);
      serialPortFlush(&serial_);
      ROS_INFO("navigation rate change from %dms to %dms, resetting uINS to make change", flash_.startupNavDtMs, nav_dt_ms);
      reset_device();
      
      // Re-request flash config to confirm change
      get_flash_config();
      if (flash_.startupNavDtMs != nav_dt_ms)
        ROS_ERROR("inertialsense: unable to change navigation rate from %dms to %dms", flash_.startupNavDtMs, nav_dt_ms);
      else
        ROS_INFO("Set navigation rate to %dms", flash_.startupNavDtMs);
    }
  }
  
  /// Start Up ROS service servers
  mag_cal_srv_ = nh_.advertiseService("single_axis_mag_cal", &InertialSenseROS::perform_mag_cal_srv_callback, this);
  multi_mag_cal_srv_ = nh_.advertiseService("multi_axis_mag_cal", &InertialSenseROS::perform_multi_mag_cal_srv_callback, this);
  
  // Stop all broadcasts
  uint32_t messageSize = is_comm_stop_broadcasts(&comm_);
  serialPortWrite(&serial_, message_buffer_, messageSize);

  /////////////////////////////////////////////////////////
  /// PARAMETER CONFIGURATION
  /////////////////////////////////////////////////////////
  set_vector_flash_config<float>("INS_rpy", 3, offsetof(nvm_flash_cfg_t, insRotation));
  set_vector_flash_config<float>("INS_xyz", 3, offsetof(nvm_flash_cfg_t, insOffset));
  set_vector_flash_config<float>("GPS_ant_xyz", 3, offsetof(nvm_flash_cfg_t, gps1AntOffset));
  set_vector_flash_config<double>("GPS_ref_lla", 3, offsetof(nvm_flash_cfg_t, refLla));
  
  set_flash_config<float>("inclination", offsetof(nvm_flash_cfg_t, magInclination), 1.14878541071f);  
  set_flash_config<float>("declination", offsetof(nvm_flash_cfg_t, magDeclination), 0.20007290992f);
  set_flash_config<int>("dynamic_model", offsetof(nvm_flash_cfg_t, insDynModel), 8);
  set_flash_config<int>("ser1_baud_rate", offsetof(nvm_flash_cfg_t, ser1BaudRate), 115200);
  
  /////////////////////////////////////////////////////////
  /// RTK Configuration
  /////////////////////////////////////////////////////////
  bool RTK_rover, RTK_base;
  nh_private_.param<bool>("RTK_rover", RTK_rover, false);
  nh_private_.param<bool>("RTK_base", RTK_base, false);
  ROS_ERROR_COND(RTK_rover && RTK_base, "unable to configure uINS to be both RTK rover and base - default to rover");
  if (RTK_rover)
  {
    ROS_INFO("InertialSense: Configured as RTK Rover");
    RTK_state_ = RTK_ROVER;
    set_flash_config(offsetof(nvm_flash_cfg_t, sysCfgBits), SYS_CFG_BITS_RTK_ROVER);
    RTK_sub_ = nh_.subscribe("RTK", 10, &InertialSenseROS::RTKCorrection_callback, this);
  }
  else if (RTK_base)
  {
    ROS_INFO("InertialSense: Configured as RTK Base");
    RTK_state_ = RTK_BASE;
    set_flash_config(offsetof(nvm_flash_cfg_t, sysCfgBits), SYS_CFG_BITS_ENABLE_COM_MANAGER_PASS_THROUGH_UBLOX_SERIAL_0);
    RTK_pub_ = nh_.advertise<inertial_sense::RTKCorrection>("RTK", 10);
  }


  /////////////////////////////////////////////////////////
  /// DATA STREAMS CONFIGURATION
  /////////////////////////////////////////////////////////
  
  uint32_t rmcBits = RMC_BITS_GPS_NAV; // we always need GPS for time synchronization
  nh_private_.param<bool>("stream_INS", INS_.enabled, true);
  if (INS_.enabled)
  {
    INS_.pub = nh_.advertise<nav_msgs::Odometry>("ins", 1);
    rmcBits |= RMC_BITS_DUAL_IMU | RMC_BITS_INS1 | RMC_BITS_INS2;
  }

  // Set up the IMU ROS stream
  nh_private_.param<bool>("stream_IMU", IMU_.enabled, false);
  if (IMU_.enabled)
  {
    IMU_.pub = nh_.advertise<sensor_msgs::Imu>("imu", 1);
//    IMU_.pub2 = nh_.advertise<sensor_msgs::Imu>("imu2", 1);
    rmcBits |= RMC_BITS_DUAL_IMU | RMC_BITS_INS1 | RMC_BITS_INS2;
  }

  // Set up the GPS ROS stream - we always need GPS information for time sync, just don't always need to publish it
  nh_private_.param<bool>("stream_GPS", GPS_.enabled, false);
  if (GPS_.enabled)
  {
    GPS_.pub = nh_.advertise<inertial_sense::GPS>("gps", 1);    
  }

  // Set up the GPS info ROS stream
  nh_private_.param<bool>("stream_GPS_info", GPS_info_.enabled, false);
  if (GPS_info_.enabled)
  {
    GPS_info_.pub = nh_.advertise<inertial_sense::GPSInfo>("gps/info", 1);
    rmcBits |= RMC_BITS_GPS1_SAT;
  }

  // Set up the magnetometer ROS stream
  nh_private_.param<bool>("stream_mag", mag_.enabled, false);
  if (mag_.enabled)
  {
    mag_.pub = nh_.advertise<sensor_msgs::MagneticField>("mag", 1);
//    mag_.pub2 = nh_.advertise<sensor_msgs::MagneticField>("mag2", 1);
    rmcBits |= RMC_BITS_MAGNETOMETER1;
  }

  // Set up the barometer ROS stream
  nh_private_.param<bool>("stream_baro", baro_.enabled, false);
  if (baro_.enabled)
  {
    baro_.pub = nh_.advertise<sensor_msgs::FluidPressure>("baro", 1);
    rmcBits |= RMC_BITS_BAROMETER;
  }

  // Set up the preintegrated IMU (coning and sculling integral) ROS stream
  nh_private_.param<bool>("stream_preint_IMU", dt_vel_.enabled, false);
  if (dt_vel_.enabled)
  {
    dt_vel_.pub = nh_.advertise<inertial_sense::PreIntIMU>("preint_imu", 1);
    rmcBits |= RMC_BITS_PREINTEGRATED_IMU;
  }
  
  // Set up RTK information streams
  nh_private_.param<bool>("stream_RTK_info", RTK_info_.enabled, false);
  if (RTK_state_ == RTK_ROVER && RTK_info_.enabled)
  {
    RTK_info_.pub = nh_.advertise<inertial_sense::RTKInfo>("RTK/info", 1);
    messageSize = is_comm_get_data(&comm_, DID_GPS_RTK_MISC, 0, 0, 10);
    rmcBits |= RMC_BITS_GPS_RTK_MISC;
    rmcBits |= RMC_BITS_GPS_RTK_NAV;
    rmcBits |= RMC_BITS_PRESET;
    serialPortWrite(&serial_, message_buffer_, messageSize);
  }
  
  messageSize = is_comm_get_data_rmc(&comm_, rmcBits);
  serialPortWrite(&serial_, message_buffer_, messageSize);

  
  /////////////////////////////////////////////////////////
  /// ASCII OUTPUT CONFIGURATION
  /////////////////////////////////////////////////////////
 
  int NMEA_rate = nh_private_.param<int>("NMEA_rate", 0);
  int NMEA_message_configuration = nh_private_.param<int>("NMEA_configuration", 0x00);
  int NMEA_message_ports = nh_private_.param<int>("NMEA_ports", 0x00);
  ascii_msgs_t msgs = {};
  msgs.options = (NMEA_message_ports & NMEA_SER0) ? RMC_OPTIONS_PORT_SER0 : 0; // output on serial 0
  msgs.options |= (NMEA_message_ports & NMEA_SER1) ? RMC_OPTIONS_PORT_SER1 : 0; // output on serial 1
  msgs.gpgga = (NMEA_message_configuration & NMEA_GPGGA) ? NMEA_rate : 0;
  msgs.gpgll = (NMEA_message_configuration & NMEA_GPGLL) ? NMEA_rate : 0;
  msgs.gpgsa = (NMEA_message_configuration & NMEA_GPGSA) ? NMEA_rate : 0;
  msgs.gprmc = (NMEA_message_configuration & NMEA_GPRMC) ? NMEA_rate : 0;
  messageSize = is_comm_set_data(&comm_, DID_ASCII_BCAST_PERIOD, 0, sizeof(ascii_msgs_t), &msgs);
  serialPortWrite(&serial_, message_buffer_, messageSize);  
}

template <typename T>
void InertialSenseROS::set_vector_flash_config(std::string param_name, uint32_t size, uint32_t offset){
  std::vector<double> tmp(size,0);
  T v[size];
  if (nh_private_.hasParam(param_name))
    nh_private_.getParam(param_name, tmp);
  for (int i = 0; i < size; i++)
  {
    v[i] = tmp[i];
  }
  
  int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offset, sizeof(v), v);
  serialPortWrite(&serial_, message_buffer_, messageSize);
}

template <typename T>
void InertialSenseROS::set_flash_config(std::string param_name, uint32_t offset, T def)
{
  T tmp;
  nh_private_.param<T>(param_name, tmp, def);
  int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offset, sizeof(T), &tmp);
  serialPortWrite(&serial_, message_buffer_, messageSize);
}

template <typename T>
void InertialSenseROS::set_flash_config(uint32_t offset, T value)
{
  int messageSize = is_comm_set_data(&comm_, DID_FLASH_CONFIG, offset, sizeof(T), &value);
  serialPortWrite(&serial_, message_buffer_, messageSize);
}

void InertialSenseROS::get_flash_config()
{
  got_flash_config = false;
  int messageSize = is_comm_get_data(&comm_, DID_FLASH_CONFIG, 0, 0, 0);
  serialPortWrite(&serial_, message_buffer_, messageSize);
  
  // wait for flash config message to confirm connection
  ros::Time start = ros::Time::now();
  while (!got_flash_config && (ros::Time::now() - start).toSec() <= 3.0)
    update();
  if ((ros::Time::now() - start).toSec() >= 3.0)
    ROS_FATAL("inertialsense: No response when requesting flash configuration from uINS on \"%s\", at %d baud", port_.c_str(), baudrate_);
}

void InertialSenseROS::flash_config_callback(const nvm_flash_cfg_t * const msg)
{
  got_flash_config = true;
  flash_ = (*msg);
}

void InertialSenseROS::INS1_callback(const ins_1_t * const msg)
{
  odom_msg.header.frame_id = frame_id_;

  odom_msg.pose.pose.position.x = msg->ned[0];
  odom_msg.pose.pose.position.y = msg->ned[1];
  odom_msg.pose.pose.position.z = msg->ned[2];
}


void InertialSenseROS::INS2_callback(const ins_2_t * const msg)
{
  ros::Time ins_time(0,0);
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t seconds = UNIX_TO_GPS_OFFSET + msg->week*7*24*3600 + floor(msg->timeOfWeek);
    uint64_t nsec = (msg->timeOfWeek - floor(msg->timeOfWeek))*1e9;
    ins_time = ros::Time(seconds, nsec);
  }
  else
  {
    ins_time = ros::Time(INS_local_offset_ + msg->timeOfWeek);
  }
  
  odom_msg.header.stamp = ins_time;

  odom_msg.header.frame_id = frame_id_;

  odom_msg.pose.pose.orientation.w = msg->qn2b[0];
  odom_msg.pose.pose.orientation.x = msg->qn2b[1];
  odom_msg.pose.pose.orientation.y = msg->qn2b[2];
  odom_msg.pose.pose.orientation.z = msg->qn2b[3];

  odom_msg.twist.twist.linear.x = msg->uvw[0];
  odom_msg.twist.twist.linear.y = msg->uvw[1];
  odom_msg.twist.twist.linear.z = msg->uvw[2];

  odom_msg.twist.twist.angular.x = imu1_msg.angular_velocity.x;
  odom_msg.twist.twist.angular.y = imu1_msg.angular_velocity.y;
  odom_msg.twist.twist.angular.z = imu1_msg.angular_velocity.z;
  if (INS_.enabled)
    INS_.pub.publish(odom_msg);
}


void InertialSenseROS::IMU_callback(const dual_imu_t* const msg)
{
  ros::Time imu_time(0, 0);
  //  If we have a GPS fix, then use it to timestamp IMU messages
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(msg->time + GPS_towOffset_) + GPS_week_*7*24*3600;
    uint64_t nsec = (msg->time + GPS_towOffset_ - floor(msg->time + GPS_towOffset_))*1e9;
    imu_time = ros::Time(sec, nsec);
  }
  else
  {
    if (!got_first_message_)
    {
      got_first_message_ = true;
      INS_local_offset_ = ros::Time::now().toSec() - msg->time;
    }
    else // low-pass filter offset to account for drift
    {
      double y_offset = ros::Time::now().toSec() - msg->time;
      INS_local_offset_ = 0.005 * y_offset + 0.995 * INS_local_offset_;
    }
    // Publish with ROS time
    imu_time = ros::Time(INS_local_offset_ + msg->time);
  }
  
  imu1_msg.header.stamp = imu2_msg.header.stamp = imu_time;
  imu1_msg.header.frame_id = imu2_msg.header.frame_id = frame_id_;

  imu1_msg.angular_velocity.x = msg->I[0].pqr[0];
  imu1_msg.angular_velocity.y = msg->I[0].pqr[1];
  imu1_msg.angular_velocity.z = msg->I[0].pqr[2];
  imu1_msg.linear_acceleration.x = msg->I[0].acc[0];
  imu1_msg.linear_acceleration.y = msg->I[0].acc[1];
  imu1_msg.linear_acceleration.z = msg->I[0].acc[2];

//  imu2_msg.angular_velocity.x = msg->I[1].pqr[0];
//  imu2_msg.angular_velocity.y = msg->I[1].pqr[1];
//  imu2_msg.angular_velocity.z = msg->I[1].pqr[2];
//  imu2_msg.linear_acceleration.x = msg->I[1].acc[0];
//  imu2_msg.linear_acceleration.y = msg->I[1].acc[1];
//  imu2_msg.linear_acceleration.z = msg->I[1].acc[2];

  if (IMU_.enabled)
  {
    IMU_.pub.publish(imu1_msg);
//    IMU_.pub2.publish(imu2_msg);
  }
}


void InertialSenseROS::GPS_callback(const gps_nav_t * const msg)
{
  if (GPS_.enabled)
  {
    uint64_t seconds = UNIX_TO_GPS_OFFSET + msg->week*7*24*3600 + floor(msg->timeOfWeekMs/1e3);
    uint64_t nsec = (msg->timeOfWeekMs/1e3 - floor(msg->timeOfWeekMs/1e3))*1e9;
    GPS_week_seconds = msg->week*7*24*3600;
    gps_msg.header.stamp = ros::Time(seconds, nsec);
    gps_msg.fix_type = msg->status & GPS_STATUS_FIX_STATUS_MASK;
    gps_msg.header.frame_id =frame_id_;
    gps_msg.num_sat = (uint8_t)(msg->status & GPS_STATUS_NUM_SATS_USED_MASK);
    gps_msg.cno = msg->cnoMean;
    gps_msg.latitude = msg->lla[0];
    gps_msg.longitude = msg->lla[1];
    gps_msg.altitude = msg->lla[2];
    gps_msg.hMSL = msg->hMSL;
    gps_msg.hAcc = msg->hAcc;
    gps_msg.vAcc = msg->vAcc;
    gps_msg.pDop = msg->pDop;
    gps_msg.linear_velocity.x = msg->velNed[0];
    gps_msg.linear_velocity.y = msg->velNed[1];
    gps_msg.linear_velocity.z = msg->velNed[2];
    GPS_.pub.publish(gps_msg);
  }
  GPS_week_ = msg->week;
  GPS_towOffset_ = msg->towOffset;
}

void InertialSenseROS::update()
{
  uint8_t buffer[512];
  int bytes_read = serialPortReadTimeout(&serial_, buffer, 512, 1);

  for (int i = 0; i < bytes_read; i++)
  {
    uint32_t message_type = is_comm_parse(&comm_, buffer[i]);
    switch (message_type)
    {
    case DID_NULL:
      // no valid message yet
      break;
    case DID_FLASH_CONFIG:
      flash_config_callback((nvm_flash_cfg_t*) message_buffer_);
      break;
    case DID_INS_1:
      INS1_callback((ins_1_t*) message_buffer_);
      break;
    case DID_INS_2:
      INS2_callback((ins_2_t*) message_buffer_);
      break;
    case DID_DUAL_IMU:
      IMU_callback((dual_imu_t*) message_buffer_);
      break;
    case DID_GPS_NAV:
	case DID_GPS_RTK_NAV:
      GPS_callback((gps_nav_t*) message_buffer_);
      break;
    case DID_GPS1_SAT:
      GPS_Info_callback((gps_sat_t*) message_buffer_);
      break;
    case DID_MAGNETOMETER_1:
      mag_callback((magnetometer_t*) message_buffer_, 1);
      break;
    case DID_MAGNETOMETER_2:
      mag_callback((magnetometer_t*) message_buffer_, 2);
      break;
    case DID_BAROMETER:
      baro_callback((barometer_t*) message_buffer_);
      break;
    case DID_PREINTEGRATED_IMU:
      preint_IMU_callback((preintegrated_imu_t*) message_buffer_);
      break;
    case DID_GPS_RTK_MISC:
      RTK_MISC_callback((gps_rtk_misc_t*) message_buffer_);
      break;
    case _DID_EXTERNAL:
      ublox_callback(message_buffer_);
      break;
    default:
      ROS_INFO("Unhandled IS message %d", message_type);
      break;
    }
  }
}

void InertialSenseROS::RTK_MISC_callback(const gps_rtk_misc_t * const msg)
{
  if (RTK_info_.enabled)
  {
    inertial_sense::RTKInfo rtk_info_msg;
    uint64_t seconds = UNIX_TO_GPS_OFFSET + msg->week*7*24*3600 + floor(msg->timeOfWeekMs/1e3);
    uint64_t nsec = (msg->timeOfWeekMs - floor(msg->timeOfWeekMs))*1e6;
    
    rtk_info_msg.header.stamp = ros::Time(seconds, nsec);
    rtk_info_msg.cycle_slip_count = msg->cycleSlipCount;
    rtk_info_msg.BaseLLA[0] = msg->baseLla[0];
    rtk_info_msg.BaseLLA[1] = msg->baseLla[1];
    rtk_info_msg.BaseLLA[2] = msg->baseLla[2];
    rtk_info_msg.baseObs = msg->baseGpsObservationCount
                           + msg->baseGlonassObservationCount
                           + msg->baseGalileoObservationCount
                           + msg->baseQzsObservationCount
                           + msg->baseBeidouObservationCount;
    rtk_info_msg.roverObs = msg->roverGpsObservationCount
                           + msg->roverGlonassObservationCount
                           + msg->roverGalileoObservationCount
                           + msg->roverQzsObservationCount
                           + msg->roverBeidouObservationCount;
    rtk_info_msg.baseEph = msg->baseGpsEphemerisCount
                           + msg->baseGlonassEphemerisCount
                           + msg->baseGalileoEphemerisCount
                           + msg->baseQzsEphemerisCount
                           + msg->baseBeidouEphemerisCount;
    rtk_info_msg.roverEph = msg->roverGpsEphemerisCount
                           + msg->roverGlonassEphemerisCount
                           + msg->roverGalileoEphemerisCount
                           + msg->roverQzsEphemerisCount
                           + msg->roverBeidouEphemerisCount;
    rtk_info_msg.baseAntcount = msg->baseAntennaCount; 
    RTK_info_.pub.publish(rtk_info_msg);
  }  
}

void InertialSenseROS::GPS_Info_callback(const gps_sat_t* const msg)
{
  uint64_t seconds = UNIX_TO_GPS_OFFSET + GPS_week_seconds + floor(msg->timeOfWeekMs/1e3);
  uint64_t nsec = (msg->timeOfWeekMs - floor(msg->timeOfWeekMs))*1e6;
  gps_info_msg.header.stamp = ros::Time(seconds, nsec);
  gps_info_msg.header.frame_id = frame_id_;
  gps_info_msg.num_sats = msg->numSats;
  for (int i = 0; i < 50; i++)
  {
    gps_info_msg.sattelite_info[i].sat_id = msg->sat[i].svId;
    gps_info_msg.sattelite_info[i].cno = msg->sat[i].cno;
  }
  GPS_info_.pub.publish(gps_info_msg);
}


void InertialSenseROS::mag_callback(const magnetometer_t* const msg, int mag_number)
{
  sensor_msgs::MagneticField mag_msg;
  ros::Time mag_time(0, 0);
  //  If we have a GPS fix, then use it to timestamp mag messages
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(msg->time + GPS_towOffset_);
    uint64_t nsec = (msg->time + GPS_towOffset_ - floor(msg->time + GPS_towOffset_))*1e9;
    mag_time = ros::Time(sec, nsec);
  }
  else
  {
    // Publish with ROS time
    mag_time = ros::Time::now();
  }
  mag_msg.header.stamp = mag_time;
  mag_msg.header.frame_id = frame_id_;
  mag_msg.magnetic_field.x = msg->mag[0];
  mag_msg.magnetic_field.y = msg->mag[1];
  mag_msg.magnetic_field.z = msg->mag[2];
  
  if(mag_number == 1)
  {
    mag_.pub.publish(mag_msg);
  }
//  else
//  {
//    mag_.pub2.publish(mag_msg);
//  }
}

void InertialSenseROS::baro_callback(const barometer_t * const msg)
{
  sensor_msgs::FluidPressure baro_msg;
  ros::Time baro_time(0, 0);
  //  If we have a GPS fix, then use it to timestamp baro messages
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(msg->time + GPS_towOffset_);
    uint64_t nsec = (msg->time + GPS_towOffset_ - floor(msg->time + GPS_towOffset_))*1e9;
    baro_time = ros::Time(sec, nsec);
  }
  else
  {
    // Publish with ROS time
    baro_time = ros::Time::now();
  }
  
  baro_msg.header.stamp = baro_time;
  baro_msg.header.frame_id = frame_id_;
  baro_msg.fluid_pressure = msg->bar;

  baro_.pub.publish(baro_msg);
}

void InertialSenseROS::preint_IMU_callback(const preintegrated_imu_t * const msg)
{
  inertial_sense::PreIntIMU preintIMU_msg;

  ros::Time imu_time(0, 0);
  //  If we have a GPS fix, then use it to timestamp imu messages (convert to UNIX time)
  if (GPS_towOffset_ > 0.001)
  {
    uint64_t sec = UNIX_TO_GPS_OFFSET + floor(msg->time + GPS_towOffset_);
    uint64_t nsec = (msg->time + GPS_towOffset_ - floor(msg->time + GPS_towOffset_))*1e9;
    imu_time = ros::Time(sec, nsec);
  }
  else
  {
    // Publish with ROS time
    imu_time = ros::Time::now();
  }
   
  preintIMU_msg.header.stamp = imu_time;
  preintIMU_msg.header.frame_id = frame_id_;
  preintIMU_msg.dtheta.x = msg->theta1[0];
  preintIMU_msg.dtheta.y = msg->theta1[1];
  preintIMU_msg.dtheta.z = msg->theta1[2];

  preintIMU_msg.dvel.x = msg->vel1[0];
  preintIMU_msg.dvel.y = msg->vel1[1];
  preintIMU_msg.dvel.z = msg->vel1[2];

  preintIMU_msg.dt = msg->dt;

  dt_vel_.pub.publish(preintIMU_msg);
}

bool InertialSenseROS::perform_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
  (void)req;
  res.success = true; 
  uint32_t single_axis_command = 1;
  int messageSize = is_comm_set_data(&comm_, DID_MAG_CAL, offsetof(mag_cal_t, enMagRecal), sizeof(uint32_t), &single_axis_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);  
}

bool InertialSenseROS::perform_multi_mag_cal_srv_callback(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
  (void)req;
  res.success = true; 
  uint32_t multi_axis_command = 0;
  int messageSize = is_comm_set_data(&comm_, DID_MAG_CAL, offsetof(mag_cal_t, enMagRecal), sizeof(uint32_t), &multi_axis_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);  
}

void InertialSenseROS::reset_device()
{
  // send reset command
  uint32_t reset_command = 99;
  int messageSize = is_comm_set_data(&comm_, DID_CONFIG, offsetof(config_t, system), sizeof(uint32_t), &reset_command);
  serialPortWrite(&serial_, message_buffer_, messageSize);
  sleep(3);
}

void InertialSenseROS::ublox_callback(uint8_t *buffer)
{
  // Collect the length out of the array, but also include 6 bytes for the UBLOX message header
  uint32_t len = (buffer[4] | buffer[5] << 8) + 6;
  
  inertial_sense::RTKCorrection msg;
  msg.correction_type = inertial_sense::RTKCorrection::RTK_CORRECTION_TYPE_UBLOX;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = frame_id_;
  msg.data.resize(len);
  // copy the data buffer into the message
  for (int i = 0; i < len; i++)
  {
    msg.data[i] = buffer[i];
  } 
  RTK_pub_.publish(msg);
}

void InertialSenseROS::RTKCorrection_callback(const inertial_sense::RTKCorrectionConstPtr &msg)
{
  uint32_t len = (msg->data[5] << 8 | msg->data[4]) + 6;
  // copy the data to our local buffer
  for (int i =0; i < len; i++)
  {
    message_buffer_[i] = msg->data[i];
  }
  serialPortWrite(&serial_, message_buffer_, len);
}



int main(int argc, char**argv)
 {
  ros::init(argc, argv, "inertial_sense_node");
  InertialSenseROS thing;
  while (ros::ok())
  {
    ros::spinOnce();
    thing.update();
  }
  return 0;
}
