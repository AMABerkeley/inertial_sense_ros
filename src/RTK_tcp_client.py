import rospy
import socket
from inertial_sense.msg import RTKCorrection

BUFSIZE = 4096

class tcpComm:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.buf = bytearray(BUFSIZE)

    def connect(self, host, port):
        self.sock.connect((host, port))
        self.sock.settimeout(0.01)

    def receive(self):
        try:
            bytes_read = self.sock.recv_into(self.buf, BUFSIZE)
            return self.buf, bytes_read
        except Exception as e:
            return None

    def close(self):
        self.sock.close()


def correctionCallback(msg):
    print msg.data



if __name__ == '__main__':
    # ip = rospy.get_param("ip")
    # port = rospy.get_param("port")

    msg_pub = rospy.Publisher("RTK", RTKCorrection, queue_size=10)


    rospy.init_node('RTK_tcp_sub')

    comm = tcpComm()
    comm.connect("67.161.249.6", 2000)
    msg = RTKCorrection()
    msg.correction_type = RTKCorrection.RTK_CORRECTION_TYPE_UBLOX
    while not rospy.is_shutdown():
        data = comm.receive()
        if data is not None:
            msg.header.stamp = rospy.Time.now()
            for i in range(data[1]):
                msg.data = list(data[0][:data[1]])
            msg_pub.publish(msg)





