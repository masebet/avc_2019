//navigation node
//this node controls autonomous navigation
#include <iostream> //dependency for fstream (must be included first)
#include <fstream>
#include <sstream>
#include <string>
#include <errno.h>
#include <math.h>
#include <pid_controller.hpp>
#include <ros/console.h>
#include <ros/ros.h>
#include <avc_msgs/ChangeControlMode.h>
#include <avc_msgs/Control.h>
#include <avc_msgs/ESC.h>
#include <avc_msgs/Heading.h>
#include <avc_msgs/SteeringServo.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Joy.h>
#include <sensor_msgs/NavSatFix.h>
#include <signal.h>
#include <wiringPi.h>

//math constants
const double EARTH_RADIUS = 6371008.7714;
const double PI = 3.1415926535897;

//global variables
bool accel_delay_flag = false;
bool autonomous_control = false;
bool autonomous_running = false;
bool mode_change_requested = false;

//global controller variables
std::vector<int> controller_buttons(13, 0);

//global GPS and heading variables
double heading = 0; //[deg]
std::vector<double> gpsFix(2, 0);
std::vector< std::vector<double> > gpsWaypoints;

//pin variables
//must be global so that they can be accessed by callback function
int indicator_LED;


//----------------------------SIGINT HANDLER------------------------------------

//callback function called to process SIGINT command
void sigintHandler(int sig)
{

  //set all pins LOW
  digitalWrite(indicator_LED, LOW);

  //call the default shutdown function
  ros::shutdown();

}

//callback function to process accel delay timer
void accelDelayTimerCallback(const ros::TimerEvent& event)
{

  //clear acceleration delay flag
  accel_delay_flag = false;

}

//--------------------------CALLBACK FUNCTIONS----------------------------------

//callback function called to process messages on control topic
void controlCallback(const avc_msgs::Control::ConstPtr& msg)
{

  //change local control mode to match message
  autonomous_control = msg->autonomous_control;

  //set mode change requested flag to true
  mode_change_requested = true;

}

//callback function called to process messages on joy topic
void controllerCallback(const sensor_msgs::Joy::ConstPtr& msg)
{

  //set local values to match message values
  controller_buttons = msg->buttons;

  //if autonomous running button on controller is pressed then toggle autonomous running status
  if ((controller_buttons[1] == 1) && (autonomous_control))
  {

    //set autonomous running status to opposite of current status
    autonomous_running = !autonomous_running;

    //turn on indicator LED during autonomous running
    if (autonomous_running)
      digitalWrite(indicator_LED, HIGH);
    else
      digitalWrite(indicator_LED, LOW);

    //notify that autonomous running is being enabled/disabled
    if (autonomous_running)
      ROS_INFO("[navigation_node] enabling autonomous running");
    else
      ROS_INFO("[navigation_node] disabling autonomous running");

    //reset controller button if pressed to prevent status from toggling twice on one button press
    controller_buttons[1] = 0;

  }

}

//callback function called to process service requests on the disable navigation mode topic
bool disableNavigationCallback(avc_msgs::ChangeControlMode::Request& req, avc_msgs::ChangeControlMode::Response& res)
{

  //if node isn't currently busy then ready to change modes, otherwise not ready to change
  res.ready_to_change = !autonomous_running;

  //output ROS INFO message to inform of mode change request and reply status
  if (req.mode_change_requested && res.ready_to_change)
    ROS_INFO("[navigation_node] mode change requested; changing control modes");
  else if (!req.mode_change_requested && res.ready_to_change)
    ROS_INFO("[navigation_node] ready to change modes status requested; indicating ready to change");
  else if (req.mode_change_requested && !res.ready_to_change)
    ROS_INFO("[navigation_node] mode change requested; indicating node is busy");
  else
    ROS_INFO("[navigation_node] ready to change modes status requested; indicating node is busy");

  //return true to indicate service processing is complete
  return true;

}

//callback function called to process messages on heading topic
void headingCallback(const avc_msgs::Heading::ConstPtr& msg)
{

  //set local values to match message values [deg]
  heading = msg->heading_angle;

}

//TEMPORARY UNTIL ODOMETRY NODE IS FINISHED
//callback function called to process messages on odometry topic
void navSatFixCallback(const sensor_msgs::NavSatFix::ConstPtr& msg)
{

  //set local values to match new message values
  gpsFix[0] = msg->latitude / 180 * PI;
  gpsFix[1] = msg->longitude / 180 * PI;

}

//callback function called to process messages on odometry topic
void odometryCallback(const nav_msgs::Odometry::ConstPtr& msg)
{

  //set local values to match message values
  //odometry

}

int main(int argc, char **argv)
{

  //send notification that node is launching
  ROS_INFO("[NODE LAUNCH]: starting navigation_node");

  //initialize node and create node handler
  ros::init(argc, argv, "navigation_node");
  ros::NodeHandle node_private("~");
  ros::NodeHandle node_public;

  //override the default SIGINT handler
  signal(SIGINT, sigintHandler);

  //retrieve acceleration delay time value from parameter server [s]
  float accel_delay_time;
  if (!node_private.getParam("/navigation/navigation_node/accel_delay_time", accel_delay_time))
  {
    ROS_ERROR("[navigation_node] acceleration delay time value not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve button input pin from parameter server
  int button_pin;
  if (!node_private.getParam("/button/input_pin", button_pin))
  {
    ROS_ERROR("[navigation_node] button input pin not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve indicator LED pin from parameter server
  if (!node_private.getParam("/led/indicator_pin", indicator_LED))
  {
    ROS_ERROR("[navigation_node] indicator LED pin not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve throttle decay constant value from parameter server
  float k_throttle_decay;
  if (!node_private.getParam("/driving/k_throttle_decay", k_throttle_decay))
  {
    ROS_ERROR("[navigation_node] minimum throttle not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve max acceleration value from parameter server
  float maximum_acceleration;
  if (!node_private.getParam("/driving/maximum_acceleration", maximum_acceleration))
  {
    ROS_ERROR("[navigation_node] ESC maximum acceleration not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve max acceleration value from parameter server
  float maximum_deceleration;
  if (!node_private.getParam("/driving/maximum_deceleration", maximum_deceleration))
  {
    ROS_ERROR("[navigation_node] ESC maximum deceleration not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve maximum throttle value from parameter server [%]
  float maximum_throttle;
  if (!node_private.getParam("/driving/maximum_throttle", maximum_throttle))
  {
    ROS_ERROR("[navigation_node] minimum throttle not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve minimum throttle value from parameter server [%]
  float minimum_throttle;
  if (!node_private.getParam("/driving/minimum_throttle", minimum_throttle))
  {
    ROS_ERROR("[navigation_node] minimum throttle not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve minimum distance value from parameter server [m]
  float min_distance_value;
  if (!node_private.getParam("/driving/min_distance_value", min_distance_value))
  {
    ROS_ERROR("[navigation_node] minimum distance value not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve minimum distance throttle value from parameter server [%]
  float min_distance_throttle;
  if (!node_private.getParam("/driving/min_distance_throttle", min_distance_throttle))
  {
    ROS_ERROR("[navigation_node] minimum distance throttle value not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve map waypoint delay from parameter server [ms]
  std::string output_file_path;
  if (!node_private.getParam("/mapping/output_file_path", output_file_path))
  {
    ROS_ERROR("[navigation_node] GPS waypoint output file path not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve PID derivative constant value from parameter server
  float pidKd;
  if (!node_private.getParam("/navigation/navigation_node/pidKd", pidKd))
  {
    ROS_ERROR("[navigation_node] PID derivative constant value not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve PID integral constant value from parameter server
  float pidKi;
  if (!node_private.getParam("/navigation/navigation_node/pidKi", pidKi))
  {
    ROS_ERROR("[navigation_node] PID integral constant value not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve PID proportional constant value from parameter server
  float pidKp;
  if (!node_private.getParam("/navigation/navigation_node/pidKp", pidKp))
  {
    ROS_ERROR("[navigation_node] PID proportional constant value value not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve refresh rate of node in hertz from parameter server
  float refresh_rate;
  if (!node_private.getParam("/navigation/navigation_node/refresh_rate", refresh_rate))
  {
    ROS_ERROR("[navigation_node] navigation node refresh rate not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve steering servo max rotation angle from parameter server
  float servo_max_angle;
  if (!node_private.getParam("/steering_servo/max_rotation_angle", servo_max_angle))
  {
    ROS_ERROR("[navigation_node] steering servo max rotation angle not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //retrieve waypoint radius value from parameter server
  float waypoint_radius;
  if (!node_private.getParam("/navigation/navigation_node/waypoint_radius", waypoint_radius))
  {
    ROS_ERROR("[navigation_node] waypoint radius not defined in config file: avc_navigation/config/navigation.yaml");
    ROS_BREAK();
  }

  //retrieve threshold distance value from parameter server [m]
  float threshold_distance_value;
  if (!node_private.getParam("/driving/threshold_distance_value", threshold_distance_value))
  {
    ROS_ERROR("[navigation_node] threshold distance value not defined in config file: avc_bringup/config/global.yaml");
    ROS_BREAK();
  }

  //create ESC message object and set default parameters
  avc_msgs::ESC esc_msg;
  esc_msg.header.frame_id = "0";

  //create steering servo message object and set default parameters
  avc_msgs::SteeringServo steering_servo_msg;
  steering_servo_msg.header.frame_id = "0";

  //create publisher to publish ESC message status with buffer size 1, and latch set to false
  ros::Publisher esc_pub = node_public.advertise<avc_msgs::ESC>("esc_raw", 1, false);

  //create publisher to publish steering servo message status with buffer size 1, and latch set to false
  ros::Publisher steering_servo_pub = node_public.advertise<avc_msgs::SteeringServo>("steering_servo_raw", 1, false);

  //create service to process service requests on the disable manual control topic
  ros::ServiceServer disable_navigation_srv = node_public.advertiseService("/control/disable_navigation", disableNavigationCallback);

  //create subscriber to subscribe to control messages topic with queue size set to 1000
  ros::Subscriber control_sub = node_public.subscribe("/control/control", 1000, controlCallback);

  //create subscriber to subscribe to joy messages topic with queue size set to 1000
  ros::Subscriber controller_sub = node_public.subscribe("/control/joy", 1000, controllerCallback);

  //create subscriber to subscribe to heading messages topic with queue size set to 1
  ros::Subscriber heading_sub = node_public.subscribe("/sensor/heading", 1, headingCallback);

  //create subscriber to subscribe to GPS location messages topic with queue size set to 1
  ros::Subscriber nav_sat_fix_sub = node_public.subscribe("/sensor/fix", 1, navSatFixCallback);

  //create subscriber to subscribe to odometry messages topic with queue size set to 1000
  ros::Subscriber odometry_sub = node_public.subscribe("odometry", 1000, odometryCallback);

  //run wiringPi GPIO setup function and set pin modes
  wiringPiSetup();
  pinMode(indicator_LED, OUTPUT);
  pinMode(button_pin, INPUT);

  //create PID controller object for steering output control
  PIDController steering_controller(pidKp, pidKi, pidKd, -servo_max_angle, servo_max_angle, 1 / refresh_rate);

  //create timer object for clearing acceleration delay flag
  ros::Timer accel_delay_timer;

  //initialie variable for recording last throttle percent value
  double last_throttle_value = 0;

  //set loop rate in Hz
  ros::Rate loop_rate(refresh_rate);

  while (ros::ok())
  {

    //handle mode change request
    if (mode_change_requested)
    {

      //set mode change requested to false to prevent two mode changes on change request
      mode_change_requested = false;

      //if autonomous control is enabled then load most recent GPS waypoint list from file
      if (autonomous_control)
      {

        //clear GPS waypoint list of previously loaded waypoints
        gpsWaypoints.clear();

        //create local variables to store file line data
        std::string line, latitude, longitude;

        //open input file to read GPS waypoints
        std::fstream input_file(output_file_path.c_str(), std::fstream::in);

        //if file was read successfully then skip header line
        if (input_file && input_file.good())
          std::getline(input_file, latitude);

        //read waypoints into list from file line by line
        while (input_file.good())
        {

          //read next line in file
          std::getline(input_file, line);

          //convert string read from file to istringstream for parsing by comma
          std::istringstream line_stream(line);

          //get latitude and longitude from current line in file
          std::getline(line_stream, latitude, ',');
          std::getline(line_stream, longitude, '\n');

          //store waypoint in list of waypoints if latitude and longitude values are valid
          if (!latitude.empty() && !longitude.empty())
          {

            //convert latitude and longitude strings into doubles in units of radians [rad]
            std::vector<double> waypoint(2);
            waypoint[0] = std::stod(latitude) / 180 * PI;
            waypoint[1] = std::stod(longitude) / 180 * PI;

            //add waypoint from current line to GPS waypoints list
            gpsWaypoints.push_back(waypoint);

            //output waypoint read from file
            ROS_INFO("[navigation_node] waypoint read from file (latitude, longitude): %lf, %lf", waypoint[0], waypoint[1]);
          }

        }

        //output result of file read
        if (!gpsWaypoints.empty())
          ROS_INFO("[navigation_node] GPS waypoint list read from file, %d total waypoints", int(gpsWaypoints.size()));
        else
          ROS_INFO("[navigation_node] GPS waypoint list read from file but no waypoints found; switch to mapping mode to record waypoints");

      }

    }
    //if mode change wasn't requested and in autonomous mode, check if button is pressed
    else if (autonomous_control && !autonomous_running && digitalRead(button_pin))
    {

      //enable autonomous running
      autonomous_running = true;

      //turn on indicator LED during autonomous running
      digitalWrite(indicator_LED, HIGH);

      //notify that autonomous running is being enabled
      ROS_INFO("[navigation_node] enabling autonomous running");

    }

    //autonomous (navigation) mode handling
    if (autonomous_control && autonomous_running)
    {

      //if there are a non-zero number of GPS waypoints remaining then run navigation algorithm
      if (!gpsWaypoints.empty())
      {

        //------------------------HEADING TO NEXT WAYPOINT CALCULATION--------------------------

        //calculate delta of latitude from current position to next waypoint in radians
        double delta_longitude = (gpsWaypoints[0][1] - gpsFix[1]);

        //calculate target heading angle from vector pointing from current position to next target waypoint
        float bearing_y = cos(gpsWaypoints[0][0]) * sin(delta_longitude);
        float bearing_x = (cos(gpsFix[0]) * sin(gpsWaypoints[0][0])) - (sin(gpsFix[0]) * cos(gpsWaypoints[0][0]) * cos(delta_longitude));
        float target_heading = atan2(bearing_y, bearing_x);

        //normalize target heading to compass bearing in degrees (0 - 360 deg)
        target_heading = fmod((target_heading / PI * 180) + 360, 360);

        //calculate output to steering servo using PID controller; positive output values indicate CCW rotation needed
        double output = steering_controller.calculate(target_heading, heading);

        //correct output so robot turns the smallest angle possible to reach target heading
        if (output > 180)
          output -= 360;
        else if (output < -180)
          output += 360;

        //set steering servo message steering angle value to current output value
        steering_servo_msg.steering_angle = output;

        //set time of steering servo message and publish
        steering_servo_msg.header.stamp = ros::Time::now();
        steering_servo_pub.publish(steering_servo_msg);

        //output debug data to log
        ROS_DEBUG("[navigation_node] target heading: %lf, current heading: %lf , output: %f", target_heading, heading, output);

        //calculate throttle percent from resulting steering angle
        //DEPRECATED: replaced with distance-based throttle control below
        //esc_msg.throttle_percent = maximum_throttle * exp(steering_servo_msg.steering_angle / servo_max_angle * k_throttle_decay);

        //------------------------DISTANCE TO NEXT WAYPOINT CALCULATION-------------------------

        //calculate x and y values of vector from current position to next target waypoint in meters (s = r * theta)
        double target_delta_x = (gpsWaypoints[0][1] - gpsFix[1]) * EARTH_RADIUS; //longitude
        double target_delta_y = (gpsWaypoints[0][0] - gpsFix[0]) * EARTH_RADIUS; //latitude

        //calculate distance to next waypoint
        float distance_to_next = sqrt(pow(target_delta_x, 2) + pow(target_delta_y, 2));

        //create throttle percent variable
        double throttle_percent;

        //if distance is above threshold value, set throttle to maximum value
        if (!accel_delay_flag && (distance_to_next > threshold_distance_value))
          throttle_percent = maximum_throttle;
        //if within threshold distance, reduce throttle from maximum inverse-proportionally to distance to minimum distance value
        else if (!accel_delay_flag && ((distance_to_next < threshold_distance_value) && (distance_to_next > min_distance_value)))
          throttle_percent = min_distance_throttle + (maximum_throttle - min_distance_throttle) * ((distance_to_next - min_distance_value) / (threshold_distance_value - min_distance_value));
        //if below minimum distance value, set throttle to minimum value
        else
          throttle_percent = min_distance_throttle;

        //limit acceleration to defined maximum
        if ((throttle_percent - last_throttle_value) > (maximum_acceleration / refresh_rate))
          throttle_percent = last_throttle_value + (maximum_acceleration / refresh_rate);
        //limit deceleration to defined maximum
        else if ((last_throttle_value - throttle_percent)  > (maximum_deceleration / refresh_rate))
          throttle_percent = last_throttle_value - (maximum_deceleration / refresh_rate);

        //if throttle percent is requested below minimum value, set to minimum value
        if (throttle_percent < minimum_throttle)
          throttle_percent = minimum_throttle;

        //set time and throttle percent value of ESC message and publish
        esc_msg.throttle_percent = throttle_percent;
        esc_msg.header.stamp = ros::Time::now();
        esc_pub.publish(esc_msg);

        //set last throttle value to current throttle value
        last_throttle_value = throttle_percent;

        //check if robot is within defined distance of waypoint, notify and set target to next waypoint if true
        if (distance_to_next < waypoint_radius)
        {

          //notify that waypoint has been reached
          ROS_INFO("[navigation_node] target reached; navigating to next waypoint (%d remaining)", int(gpsWaypoints.size()));

          //remove current waypoint from list
          gpsWaypoints.erase(gpsWaypoints.begin());

          //set acceleration delay flag
          accel_delay_flag = true;

          //start timer
          accel_delay_timer = node_private.createTimer(ros::Duration(accel_delay_time), accelDelayTimerCallback, true);

        }

      }
      //end autonomous running and notify if there are no waypoints remaining in list
      else
      {

        //end autonomous running
        autonomous_running = false;

        //flash LED twice to indicate goal has been reached
        for (int i = 0; i < 2; i++)
        {

          //flash LED
          digitalWrite(indicator_LED, LOW);
          delay(500);
          digitalWrite(indicator_LED, HIGH);

          //delay between flashes or turn off LED if flashing is finished
          if (i < 1)
            delay(500);
          else
            digitalWrite(indicator_LED, LOW);

        }

        //inform that there are no remaining GPS waypoints to navigate to
        ROS_INFO("[navigation_node] no GPS waypoints remaining in list; navigation complete");
        ROS_INFO("[navigation_node] switch to mapping mode and back to reload GPS waypoints");

      }

    }
    //if autonomous running is disabled then stop robot and reset steering angle
    else if (autonomous_control && !autonomous_running)
    {

      //reset ESC msg, set time, and publish
      esc_msg.header.stamp = ros::Time::now();
      esc_msg.throttle_percent = 0;
      esc_pub.publish(esc_msg);

      //reset steering msg, set time, and publish
      steering_servo_msg.header.stamp = ros::Time::now();
      steering_servo_msg.steering_angle = 0;
      steering_servo_pub.publish(steering_servo_msg);

    }

    //process callback functions
    ros::spinOnce();

    //sleep until next sensor reading
    loop_rate.sleep();

  }
  return 0;
}
