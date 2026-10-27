sudo chmod 666 /dev/ttyACM0
stty -F /dev/ttyACM0 38400 cs8 -cstopb -parenb raw clocal -echo
cat /dev/ttyACM0