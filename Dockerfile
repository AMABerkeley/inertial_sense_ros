# This is an auto generated Dockerfile for ros:desktop-full
# generated from docker_images/create_ros_image.Dockerfile.em
FROM osrf/ros:kinetic-desktop-xenial

# install ros packages
RUN apt-get update && apt-get install -y \
    ros-kinetic-desktop-full=1.3.2-0* \
    && rm -rf /var/lib/apt/lists/*

COPY . /home/ros_workspace/src/

RUN bash -c "source /opt/ros/kinetic/setup.bash && \
             cd /home/ros_workspace && \
             catkin_make && \
             source /home/ros_workspace/devel/setup.bash"
