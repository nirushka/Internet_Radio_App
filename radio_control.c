/**********************************************************************/
/* --------------------[ Radio's client control ]-------------------- */
/* 	Developed by: Itay Parag - 201606886 & Nir Rafman - 307941732	  */
/**********************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>

#define Num_of_bytes		1024
#define stdin_fd			0


typedef struct Hello {
	uint8_t		commandType;	/*HELLO*/
	uint16_t	reserved;
} Hello;

typedef struct AskSong {
	uint8_t		commandType;	/*ASK SONG*/
	uint16_t	stationNumber;
} AskSong;

typedef struct UpSong {
	uint8_t		commandType;	/*UP SONG*/
	uint32_t	songSize;
	uint8_t		songNameSize;
	char *		songName;
} UpSong;

typedef struct UDP_Args {
	uint32_t multicastGroup;
	uint16_t portNumber;
} UDP_Args;


void Main_listener();
void UDP_listener();
void error(const char * str);
void clearBuffer();

fd_set fds;
int tcp_socket_fd, udp_socket_fd, total_bytes_rec = 0, userAction=-1, change_station=0; 
uint16_t numStations, tcpFlag=1, udpFlag=1;
uint32_t newMulticastAddr=0;
char tcp_buffer[Num_of_bytes]/* Receiver buffer */, buffer[Num_of_bytes]/* Transmitter buffer */; 
char * invalidStr;
pthread_t udp_thread, tcp_thread, std_thread;
struct UDP_Args UDPs;
FILE * fp;

int main(int argc, char *argv[]) {	
	int port_num, BytesCount=0;
	struct sockaddr_in sender_addr;
	Hello hello = {0,0};
	struct timeval timeout;
	struct in_addr multi_num; 
	
	if(argc < 3) {
		error("usage is ./radio_control <servername> <serverport>");
	}		
	timeout.tv_sec = 0;
	timeout.tv_usec = 300000;		// set 300 milliseconds
	
	// setup the socket
	tcp_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(tcp_socket_fd < 0) {			// check if the socket was created successfully. If it wasn't, display an error and exit
		error("Error opening socket");
	}
	
	memset(&sender_addr, '0', sizeof(sender_addr));		// clear our the sender_addr tcp_buffer
	
	// set up the socket
	port_num = atoi(argv[2]);		// Convert arguments to their appropriate data types
	sender_addr.sin_family = AF_INET;
	sender_addr.sin_addr.s_addr = inet_addr(argv[1]);
	sender_addr.sin_port = htons(port_num);
	
	// try to connect to the sender
	if(connect(tcp_socket_fd,(struct sockaddr *) &sender_addr, sizeof(sender_addr)) < 0) {
		error("Error connecting");
	}
	
	/**********************************************************************/
	/* --------[ Connection succeeded - start transmitting data ]-------- */
	/**********************************************************************/
	
	clearBuffer();
	
	tcp_buffer[0] = hello.commandType;					// set HELLO message fields
	*(uint16_t *) (&(tcp_buffer)[1]) = hello.reserved;
	
	send(tcp_socket_fd, &tcp_buffer, 3, 0);				// send the first HELLO message
	
	FD_CLR(tcp_socket_fd, &fds);
	FD_SET(tcp_socket_fd, &fds);
	
	if(select(FD_SETSIZE, &fds, NULL, NULL, &timeout) <= 0 && (errno != EINTR)) {	// wait for WELCOME message
		error("Welcome timed out");
	}
	
	memset(tcp_buffer, '0', sizeof(tcp_buffer));		// clear the buffer
	if((BytesCount = read(tcp_socket_fd, tcp_buffer, Num_of_bytes)) < 0) {			// WELCOME length = 9 bytes
		error("Welcome socket error");
	}
	if(BytesCount != 9) {
		error("Error, welcome message length is wrong, terminating client");
	}
	else if(tcp_buffer[0] == 3) {
		invalidStr = (char *) malloc(tcp_buffer[1]);
		strncpy(invalidStr, (tcp_buffer+2), tcp_buffer[1]);
		error(invalidStr);
	}
	else if(tcp_buffer[0] != hello.commandType) {			// check the reply type
		error("Error receiving welcome reply, terminating client");
	}
	
	/**********************************************************************/
	/* ---------[ Welcome received - start UDP & TCP listeners ]--------- */
	/**********************************************************************/
	
	numStations			= htons(*(uint16_t *) (&(tcp_buffer)[1]));			// save the input
	UDPs.multicastGroup	= htonl(*(uint32_t *) (&(tcp_buffer)[3]));
	UDPs.portNumber		= htons(*(uint16_t *) (&(tcp_buffer)[7]));
	
	multi_num.s_addr	= UDPs.multicastGroup;
	
	printf("Welcome to our radio station!\nthe number of stations is %d\n"
		   "Multicast group is %s\nportNumber: %d\n", numStations, inet_ntoa(multi_num), UDPs.portNumber);
	clearBuffer();
	
	pthread_create(&udp_thread, NULL, (void *) UDP_listener, NULL);		// start UDP listener with the received parameters	
	pthread_create(&tcp_thread, NULL, (void *) Main_listener, NULL);	// start listening for std-in and TCP changes

	pthread_join(udp_thread, NULL);			// wait for threads termination
	pthread_join(tcp_thread, NULL);
}

void Main_listener() {
	AskSong askSong = {1};
	UpSong upSong = {2};
	int j, newStationTimeOut=0, bytes_rec = 0, selectValue, wait=0, waitAnnounce=0, waitPermit=0, song_size=0, song_name_size=0, temp_song_size=0, sent_bytes=0;
	uint16_t read_data, wantedStation, percents=0;
	char stdBuffer[4] = {'0'}, songName[100] = {0}, * songTitle, * invalidStr;
	struct timeval timeout;
	struct stat file_stat;
	FILE * song;
	
	clearBuffer();
	while(tcpFlag) {
		timeout.tv_sec = 0;
		timeout.tv_usec = 300000;		// set 300 milliseconds
		if(newStationTimeOut) {
			timeout.tv_usec = 2000000;	// set new timeout value foe the upload procedure
			newStationTimeOut = 0;
		}
		printf("Please enter a station number, 's' to send or 'q'  to quit.\n");
		FD_ZERO(&fds);
		FD_SET(tcp_socket_fd, &fds);
		FD_SET(stdin_fd, &fds);
		
		if(wait) {
			selectValue = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);  
		}
		else {
			selectValue = select(FD_SETSIZE, &fds, NULL, NULL, NULL); 
		}		
		
		if(selectValue < 0 && (errno != EINTR)) {
			error("Main timeout error");
		}
		else if(selectValue == 0) {
			error("Timeout occurred! disconnecting");
		}
		else {
			wait = 0;		// disable setting timeout
		}
		
		// ============================ [ User input listener ] ============================
		if(FD_ISSET(stdin_fd, &fds)) {
			memset(stdBuffer, '0', sizeof(stdBuffer));	// clear the buffer
			fgets(stdBuffer, 4, stdin);	// read input from the user
			
			// check if the input is a valid command
			if((stdBuffer[0] >= '0' && stdBuffer[0] <= '9') || ((stdBuffer[0] >= '0' && stdBuffer[0] <= '9') && (stdBuffer[1] >= '0' && stdBuffer[1] <= '9'))) {
				userAction = 1;		// ask song - user selected a station
				if(stdBuffer[1] >= '0' && stdBuffer[1] <= '9' && (stdBuffer[2] != '\n')) {
					printf("Wrong number entered! '%c', '%c'\n", stdBuffer[1], stdBuffer[2]);
					userAction = 4;
				}
				else if(stdBuffer[1] != '\n') {
					printf("Wrong number entered! '%c'\n", stdBuffer[1]);
					userAction = 4;
				}
			}
			else if(stdBuffer[0] == 's') {
				userAction = 2;		// up song - user wants to upload a song, waiting for permit
				if(stdBuffer[1] != '\n') {
					printf("Wrong input entered (s) '%c'!\n", stdBuffer[1]);
					userAction = 4;
				}
			}
			else if(stdBuffer[0] == 'q') {
				userAction = 3;		// exit program
				if(stdBuffer[1] != '\n') {
					printf("Wrong input entered (q) '%c'!\n", stdBuffer[1]);
					userAction = 4;
				}
			}
			else {
				userAction = 4;		// invalid command - terminate the client
			}
			
			switch(userAction) {
				case 1: /* Ask_Song -- change station */
						tcp_buffer[0] = askSong.commandType;
						wantedStation = atoi(stdBuffer);		// change from char * to int
						if(wantedStation >= numStations) {
							printf("Please retry by entering a valid station number, or 'q' to quit.\n");
						}
						else {
							*((uint16_t*)(tcp_buffer+1)) = htons(wantedStation);
						
							send(tcp_socket_fd, &tcp_buffer, 3, 0);				// send the AskSong request
							
							waitAnnounce = 1;	// mark that we're waiting for station announce
							wait = 1;			// set timeout
							
							memset(stdBuffer, '0', sizeof(stdBuffer));			// clear the buffer
						}
					break;
				
				case 2: /* upSong */
						printf("Please enter a valid song name to transmit (in the form 'song.mp3'), if you want quit enter anything else\n"
							   "Please enter less than 200 characters and press enter\n");
						
						fgets(songName, 100, stdin);		// get song name from user
 					  	songName[strlen(songName)-1] = '\0';
 					  	
						if(stat(songName, &file_stat) < 0) {
							printf("No such file\n");
							break;
						}
					   	song_size = file_stat.st_size;
						
						tcp_buffer[0] = upSong.commandType;
						*(uint32_t *) (&(tcp_buffer)[1]) = htonl(song_size);	
						tcp_buffer[5] = strlen(songName);
						strncpy(((char*) tcp_buffer + 6), songName, strlen(songName));
						
						send(tcp_socket_fd, &tcp_buffer, (6 + strlen(songName)), 0);
						
						waitPermit = 1;		// mark that we're waiting to permission to upload
						wait = 1;			// set timeout
						
						memset(stdBuffer, '0', sizeof(stdBuffer));	// clear the buffer
				break;
				
				case 3: /* Quit */
					tcpFlag = 0;	// notify all threads to stop their run
					udpFlag = 0;
					
					printf("We are closing the communication, Bye Bye!\n");
					//exit(0);
				break;
				
				case 4: /* Invalid Command */
					printf("Invalid Command... try again\n");
				break;
			}
		}
		// ============================ [ TCP socket listener ] ============================
		else if(FD_ISSET(tcp_socket_fd, &fds)) {
			bytes_rec = read(tcp_socket_fd, tcp_buffer, Num_of_bytes);
			if(bytes_rec < 0) {
				error("Error reading from socket OR Sender disconnected");
			}
			else if(bytes_rec == 0) {
				error("The server has closed the connection!\nBye Bye\n");
			}
			
			switch(tcp_buffer[0]) {
				case 1:	// ---------- [ Announce - reply for askSong ] ----------
					if(!waitAnnounce) {
						error("Error: server sent an announce with no reason");
					}
					else {
						waitAnnounce = 0;
						
						song_name_size = tcp_buffer[1];
						songTitle = (char *) malloc(sizeof(char *) * song_name_size);
						strncpy(songTitle, tcp_buffer+2, song_name_size);
						
						if(bytes_rec != 2 + song_name_size) {
							error("Error: anounce message has wrong length, terminating the client");
						}
						
						change_station = 1;				// set flag to change station
						
						newMulticastAddr = wantedStation;
						printf("station changed!\n%s is playing!\n", songTitle);
						free(songTitle);
					}
				break;
				
				case 2:	// ---------- [ Permit - reply for upSong ] ----------
					if(waitAnnounce) {
						error("Error: waiting for announce, server sent permit");
					}
					else {
						waitPermit = 0;
						if(bytes_rec != 2) {
							error("Error: permit message has wrong length, terminating the client");
						}
						if(tcp_buffer[1] == 1) {	// permit to upload
							printf("Upload procedure permitted!\nStarting to transmit song..\n");
					
							if(!(song = fopen(songName, "r"))) {	// open the desired mp3 file
								error("Couldn't open file");
							}
							temp_song_size = song_size;
							
							while((read_data = fread(buffer, 1, Num_of_bytes, song))) {
								sent_bytes = write(tcp_socket_fd, &buffer, read_data);
								temp_song_size -= sent_bytes;
								
								percents = (int)((50*(song_size - temp_song_size))/song_size);	// scale the size to 50 chars
								
								// display percents progress bar
								printf("\r[");
								for(j=0; j<percents; j++) { printf("="); }
								for(j=percents; j<50; j++) { printf(" "); }	// 50 = the scale % of the percents bar
								printf("] %d%% - %d Bytes left", ((int)(2*percents)), temp_song_size);
								
								usleep(8000);
							}
							printf("\n\n");
							
							if(temp_song_size > 0) {
								error("Upload procedure failed");
							}
							else {
								printf("Upload procedure is finished\n");
								newStationTimeOut = 1;
								wait = 1;					// set timeout
							}
						}
						else {
							printf("Upload was not permitted! try again later.\n");
						}
					}
				break;
				
				case 3:	// ---------- [ Invalid Command ] ----------
					invalidStr = (char *) malloc(tcp_buffer[1]);
					strncpy(invalidStr, (tcp_buffer+2), tcp_buffer[1]);
					error(invalidStr);					
				break;
				
				case 4:	// ----------- [ New Stations ] ------------
					numStations++;
					if(bytes_rec != 3) {
						error("Error: new station message has wrong length, terminating the client");
					}
					printf("New station added!\n");
				break;
				
				case 0:	// ------- [ Double Welcome message ] -------
					error("Error: The server sent more than 1 WELCOME message. terminating client");
				break;
				
				default: // -------- [ Unknown message type ] --------
					error("Invalid reply type from server");
				break;
			}
		}
		fflush(stdin);
	}
}

void UDP_listener() {
	char udp_buffer[Num_of_bytes], mCastAddr[15]={'\0'};
	int receiver_addr_len, bytes_rec;
	struct timeval timeout;
	struct sockaddr_in receiver_addr;
	struct ip_mreq mreq;
	struct in_addr multi_num; 
	uint16_t portNumber = UDPs.portNumber;
	
	fp = popen("play -t mp3 -> /dev/null 2>&1", "w");	// open pipe and redirect Play's output to the garbage
	
	timeout.tv_sec = 0;
	timeout.tv_usec = 300000;	// set 300 milliseconds
	
	memset(udp_buffer, '0', sizeof(udp_buffer));
	
	// setup the socket
	udp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(udp_socket_fd < 0) {		// check if the socket was created successfully. If it wasn't, display an error and exit
		error("Error opening socket");
	}
	
	// clear our the receiver_addr buffer
	memset((char *) &receiver_addr, 0, sizeof(receiver_addr));
	
	// setup socket's parameters
	multi_num.s_addr = UDPs.multicastGroup;
	receiver_addr.sin_family = AF_INET;
	receiver_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	receiver_addr.sin_port = htons(portNumber);
	receiver_addr_len = sizeof(receiver_addr);

	// bind the socket to an address
	if(bind(udp_socket_fd, (struct sockaddr *) &receiver_addr, sizeof(receiver_addr)) < 0) {
		error("Error on binding");
	}
	
	// set multicast request addresses
	sprintf(mCastAddr, "%s", inet_ntoa(multi_num));
	mreq.imr_multiaddr.s_addr = inet_addr(mCastAddr);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	if(setsockopt(udp_socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		error("Error setting UDP's multicast socket options");
	}
	while(udpFlag) {
		if(change_station) {	// check if we need to change station, if so - send IGMP commands
			printf("change of station is detected\n");
			if(setsockopt(udp_socket_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
				error("Error dropping multicast group");
			}
			
			// set the new multicast request addresses
			multi_num.s_addr = (UDPs.multicastGroup) + htonl(newMulticastAddr); 			
			sprintf(mCastAddr, "%s", inet_ntoa(multi_num));			
			mreq.imr_multiaddr.s_addr = inet_addr(mCastAddr);
			
			if(setsockopt(udp_socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
				error("Error joining to a new multicast group");
			}
			change_station = 0;
		}
		
		bytes_rec = recvfrom(udp_socket_fd, udp_buffer, Num_of_bytes, 0, (struct sockaddr *) &receiver_addr, &receiver_addr_len);
		if(bytes_rec <= 0) {
			error("Error reading from socket OR the server has disconnected");
		}
		
		fwrite(udp_buffer, 1, Num_of_bytes, fp);		// write buffer's data to the "play" program
		
		total_bytes_rec += bytes_rec;
	}
}

void clearBuffer() {
	memset(buffer, '0', sizeof(buffer));	// clear the buffer
}

void error(const char * str) {
	tcpFlag = 0;	// notify all threads to stop their run
	udpFlag = 0;
	
	perror(str);
	printf("\n");
	
	close(udp_socket_fd);
	close(tcp_socket_fd);
	pclose(fp);
	
	exit(1);
}
