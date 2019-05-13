welcome to our Radio app:

In order to try our radio app just download the files and type 'make' at your terminal.


To use the Server proparly just type in the command line:

./radio_server <tcpport> <mulitcastip> <udpport> <file1 > <file2 > ...

<tcpport> is a port number on which the server will listen. <mulitcastip> is the IP on which the
server send station number 0. < udpport > is a port number on which the server will stream the
music, followed by a list of one or more files.


To use the Client proparly just type in the command line:

./radio_control <servername> <serverport>

<servername> represents the IP address (e.g. 132.72.38.158) or hostname (e.g. localhost)
which the control client should connect to, and <serverport> is the port to connect to.

### For the app to work it must accept ONLY 128Kbps rate songs!! ###
