/**********************************************************************/
/* --------------------[ Radio's server control ]-------------------- */
/* 	Developed by: Itay Parag  & Nir Rafman 	  */
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

#define NUM_OF_BYTES				1024
#define STDIN_FD					0
#define UDP_RATE_DELAY				62500
#define MAX_CLIENTS					100
#define WELCOME_BUFFER_SIZE			3

#define REPLY_TYPE_WELCOME			0
#define REPLY_TYPE_ANNOUNCE			1
#define REPLY_TYPE_PERMIT_SONG		2
#define REPLY_TYPE_INVALID_CMD		3
#define REPLY_TYPE_NEW_STATIONS		4

typedef struct Station {
	int			multi_address;
	char *		song_name;
	uint32_t	song_size;
	uint8_t		song_name_size;
	FILE *		song;
} Station;

typedef struct StationsArray {
	Station *	station_num;
	uint16_t	used;			
	size_t		size;
} Array;

typedef struct Clients {
	uint8_t		inUse;
	uint8_t		hasThread;
	uint8_t		hasSocket;
	uint8_t		helloReceived;
	int			socket;			/* client's socket */
	pthread_t	thread;
	struct		sockaddr_in addr;
	fd_set		client_fd;
	
} Clients;


void welcomeThread();
void clientThread(int who);
void initArray(Array * stations, size_t initialSize);
void insertArray(Array * stations, int element);
void freeArray(Array * stations);
void open_song(int k);
void error(const char * str);
int  getSlot();
void removeClient(int who);
void invalidCommand(int who, char * errorMsg);
void permitUpload(int who, uint8_t type);
void printMenu();

fd_set		fds;
Clients		clients[MAX_CLIENTS] = {0};			// set place for the maximum clients possible
pthread_t	cThread[MAX_CLIENTS];				// thread for each client
Array		stations;
int			connectedClients = 0, base_multicast_addr, curClient=0, lastStation=0, welcomeFlag=1, mainFlag=1, clientFlag=1;
uint16_t	udp_port_num, tcp_port_num;
struct 		timeval timeout;
volatile int permitSong = 1;					// define mutex for the song upload procedure (1 = can upload, 0 = block)
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {
	int			i, sender_socket_fd, ttl = 15;
	char  		buffer[NUM_OF_BYTES], mCastAddr[15] = {'\0'};
	pthread_t	t_welcome;
	struct		sockaddr_in udp_sender_addr;
	struct		in_addr multi_num;
	struct		stat file_stat;
	
	if(argc < 4) {
		error("usage is ./radio_server <tcpport> <multicastip> <udpport> <file1> <file2> ...");
	}
	else if(argc == 4){		// in case no songs were added
		lastStation = -1;
	}
	
	// set global vars with cmd's input
	tcp_port_num = atoi(argv[1]);
	base_multicast_addr = inet_addr(argv[2]);
	udp_port_num = atoi(argv[3]);
	
	initArray(&stations, argc-4);	// initially start with the given songs
	
	for(i = 4; i < argc; i++) {		// add songs to the stations array
		insertArray(&stations, base_multicast_addr + htonl((i-4)));				// insert the multicast addresses according to the input
		lastStation = i-4;
		stations.station_num[stations.used-1].song_name = (char *) malloc(sizeof(char));	// allocate the song name size
		if(stat(argv[i], &file_stat) < 0) {
			error("can't play the song");
		}
		
		strcpy(stations.station_num[stations.used-1].song_name, argv[i]);		// Insert the song name to the struct
		stations.station_num[stations.used-1].song_name_size = strlen(stations.station_num[stations.used-1].song_name);
		stations.station_num[stations.used-1].song_size = file_stat.st_size;
		
		multi_num.s_addr = stations.station_num[stations.used-1].multi_address;
		sprintf(mCastAddr, "%s", inet_ntoa(multi_num));
	}
	
	for(i=0; i<stations.used; i++) {	// open all songs for reading
		if(!(stations.station_num[i].song = fopen(stations.station_num[i].song_name, "r"))) {
			error("Couldn't open file");
		}
	}
	
	// setup the UDP socket
	sender_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sender_socket_fd < 0) {
		error("Error opening socket");
	}
	
	memset((char *) &udp_sender_addr, 0, sizeof(udp_sender_addr));
	udp_sender_addr.sin_family = AF_INET;
	udp_sender_addr.sin_port = htons(udp_port_num);
	
	setsockopt(sender_socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));	// set TTL so we can reach all our topology's hosts
	
	
	/**********************************************************************/
	/* -----------------[ Listen to the Welcome socket ]----------------- */
	/**********************************************************************/
	
	pthread_create(&t_welcome, NULL, (void *) welcomeThread, NULL);	// start listening for hello messages
	pthread_join((pthread_t)welcomeThread, NULL);
	
	/**********************************************************************/
	/* --------------------------[ UDP sender ]-------------------------- */
	/**********************************************************************/
	
	while(mainFlag) {		// transmit UDP songs to the multicast groups
		for(i=0; i<stations.used; i++) {
			udp_sender_addr.sin_addr.s_addr = stations.station_num[i].multi_address;
			
			if(fread(buffer, NUM_OF_BYTES, 1, stations.station_num[i].song) <= 0) {		// we've reached the end of the song
				fclose(stations.station_num[i].song);
				open_song(i);		// re-open the song after he finished playing, so we can loop it
				fread(buffer, NUM_OF_BYTES, 1, stations.station_num[i].song);	
			}
			
			sendto(sender_socket_fd, buffer, NUM_OF_BYTES, 0, (struct sockaddr *) &udp_sender_addr, sizeof(udp_sender_addr));	
		}
		
		usleep(UDP_RATE_DELAY);		// delay transmission to synchronize the rate
	}
	
	return 0;
}

void welcomeThread() {
	char	welcome_buffer[10]={'\0'}, action, mCastAddr[15]={'\0'};
	struct	sockaddr_in welcome_addr;
	struct	in_addr multi_num;
	int		i, recLen, countedClients, welcome_socket, selectValue, getUserInput, count=0, ByteCount;
	
	welcome_socket = socket(AF_INET, SOCK_STREAM, 0);		// open the welcome socket
	if(welcome_socket < 0) {
		error("Error opening socket");
	}
	
	if(setsockopt(welcome_socket, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int)) < 0) {		// allow re-using of the previous addresses
		error("Couldn't set reuse of address");
	}
	
	memset((char *) &welcome_addr, 0, sizeof(welcome_addr));
	welcome_addr.sin_family = AF_INET;
	welcome_addr.sin_addr.s_addr = INADDR_ANY;
	welcome_addr.sin_port = htons(tcp_port_num);
	
	// bind the socket to an address
	if(bind(welcome_socket, (struct sockaddr *) &welcome_addr, sizeof(welcome_addr)) < 0) {
		error("Error on binding welcome socket");
	}
	
	// listen for incoming connections: accept at most MAX_CLIENTS connections before refusing them
	listen(welcome_socket, MAX_CLIENTS);
	
	FD_CLR(welcome_socket, &fds);
	FD_SET(welcome_socket, &fds);
	
	printf("Welcome to our RADIO SERVER!\n\n");
	printMenu();
	while(welcomeFlag) {
		FD_ZERO(&fds);
		FD_SET(welcome_socket, &fds);
		FD_SET(STDIN_FD, &fds);
		
		selectValue = select(FD_SETSIZE, &fds, NULL, NULL, NULL);		// wait for new connection
		if((selectValue < 0) && (errno != EINTR)) {				
			error("WelcomeThread's select error");
		}
		
		// ========================= [ User Input listener ] =========================
		if(FD_ISSET(STDIN_FD, &fds)) {	// stdin has changed
			getUserInput = 1;
			while(getUserInput) {
				getUserInput = 0;
				count = 0;
				action = fgetc(stdin);
				while(getchar() != '\n') {
					count++;
				}
				if(count > 0) action = 'x';		// mark invalid input
				
				fflush(stdin);
				
				switch(action) {
					case 'p':
						printf("We have %d stations:\n", stations.used);
						for(i=0; i<stations.used; i++) {
							multi_num.s_addr = stations.station_num[i].multi_address;
							sprintf(mCastAddr, "%s", inet_ntoa(multi_num));
							
							printf("\tStation %d: %s\n\t%s\n", i, mCastAddr, stations.station_num[i].song_name);
						}
					
						countedClients = 0;
						printf("\nWe have %d clients:\n", connectedClients);
						for(i=0; i<MAX_CLIENTS; i++) {
							if(clients[i].inUse) {
								countedClients++;
								
								sprintf(mCastAddr, "%s", inet_ntoa(clients[i].addr.sin_addr));
								printf("\tClient %d with an ip address: %s\n", countedClients, mCastAddr);
							}
						
							if(countedClients == connectedClients) break;	// stop searcing for clients on the list in case we've found them all
						}
					break;
					
					case 'q':
						for(i=0; i<MAX_CLIENTS; i++) {
							if(clients[i].inUse) {
								countedClients++;								
								removeClient(i);	// close the client's socket, thread and remove from the list
							}
							if(countedClients == connectedClients) break;	// stop searching for clients on the list in case we've found them all
						}
						
						printf("We're terminating the server!\n"
								"• Closing all threads & sockets\n"
								"• Freeing all allocated memory\n");
						
						freeArray(&stations);
						
						// notify all threads to stop their run
						welcomeFlag = 0;	// stop the welcome thread from running
						clientFlag = 0;		// stop the client thread from running
						mainFlag = 0;		// stop the main from running
					break;
				
					default:
						printf("Wrong input!\n");
						printMenu();
						getUserInput = 1;	// get user input again
					break;
				}
			}
		}
		
		// ========================= [ Welcome Socket listener ] =========================
		else {	// welcome socket has changed
			curClient = getSlot();		// find the lowest free slot on the array
			if(curClient < 0) {
				error("Couldn't find free slot for the new client");
			}
			recLen = sizeof(clients[curClient].addr);
			
			// block until there is a new connection. When one is received, accept it
			clients[curClient].socket = accept(welcome_socket, (struct sockaddr *) &clients[curClient].addr, &recLen);
			if(clients[curClient].socket < 0) {
				removeClient(curClient);
				error("Error accepting new client");
			}
			clients[curClient].hasSocket = 1;		// mark that socket has been opened
			
			FD_CLR(clients[curClient].socket, &fds);
			FD_SET(clients[curClient].socket, &fds);	// add the socket to the FD Set so we can wait on it with timeout
			
			timeout.tv_sec = 0;
			timeout.tv_usec = 300000;
			
			selectValue = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);	// wait for hello
			if((selectValue < 0) && (errno != EINTR)) {
				error("Select error while waiting for hello");
			}
			else if(selectValue == 0) {
				invalidCommand(curClient, "Error: didn't sent hello within 300 milliseconds");
				printf("Error receiving hello message from client #%d at the desired time\n", curClient);
				removeClient(curClient);
				continue;
			}
			FD_CLR(clients[curClient].socket, &fds);
			FD_SET(clients[curClient].socket, &fds);
			
			// receive the hello message --> check it and send welcome
			if((ByteCount = read(clients[curClient].socket, welcome_buffer, NUM_OF_BYTES)) < 0) {
				error("Error reading from welcome socket");
			}
			
			if(ByteCount != WELCOME_BUFFER_SIZE) {
				invalidCommand(curClient, "Error: Hello command must be at a size of 3 bytes");
				removeClient(curClient);
				printf("Disconnecting client #%d - a command sent with unexpected length (hello)\n",curClient);
			}
			
			if(welcome_buffer[0] != 0) {	// check that we've got the right command type
				invalidCommand(curClient, "Error: Hello command must be sent before any other command");
				printf("Error receiving hello message from client #%d\n", curClient);
				removeClient(curClient);
				continue;
			}
			
			// prepare the welcome message
			welcome_buffer[0] = REPLY_TYPE_WELCOME;
			*(uint16_t *) (welcome_buffer+1) = htons(stations.used);
			*(uint32_t *) (welcome_buffer+3) = htonl(base_multicast_addr);
			*(uint16_t *) (welcome_buffer+7) = htons(udp_port_num);
			
			write(clients[curClient].socket, &welcome_buffer, 9);	// send the welcome message
			
			// move user to his own TCP socket's thread
			pthread_create(&clients[curClient].thread, NULL, (void *) clientThread, (void *)curClient);		// start TCP communication specific for the new client		
		}
	}
}

void clientThread(int who) {
	uint8_t		userAction = -1, songNameSize=0, songValid=1;
	uint16_t	i, stationNum = -1, countedClients=0;
	uint32_t	songSize, receivedBytes=0, recBytes=0, percent;
	char		client_buffer[NUM_OF_BYTES] = {'\0'}, cmdString[30], *songName;
	FILE *		recSong;
	
	clients[who].hasThread = 1;		// mark that thread has been opened
	
	while(clientFlag) {
		FD_CLR(clients[who].socket, &clients[who].client_fd);
		FD_SET(clients[who].socket, &clients[who].client_fd);
		memset(client_buffer, '\0', sizeof(client_buffer));
		
		if((select(FD_SETSIZE, &clients[who].client_fd, NULL, NULL, NULL) < 0) && (errno != EINTR)) {
			error("Client socket's select error");
		}
		
		if((recBytes = read(clients[who].socket, client_buffer, NUM_OF_BYTES)) < 0) {
			printf("Error reading from client #%d socket\n", who+1);
			removeClient(who);
			break;	// get out of the while and client's thread
		}
		
		else if(recBytes == 0) {
			printf("Client #%d closed the connection.\n", who+1);
			removeClient(who);
			break;	// get out of the while and client's thread
		}
		
		userAction = client_buffer[0];
		switch(userAction) {
			// ========================= [ Announce Handler ] =========================
			case REPLY_TYPE_ANNOUNCE:
				if(recBytes != 3) {
					invalidCommand(who, "Error: AskSong command must be at a size of 3 bytes");
					printf("Disconnecting client #%d - a command sent with unexpected length (askSong)\n",who+1);
					removeClient(who);
					break;
				}
				
				stationNum = htons(*(uint16_t *) (client_buffer+1));
				memset(client_buffer, '\0', sizeof(client_buffer));		// clear buffer before using it
				
				if(stationNum <= stations.used) {	
					client_buffer[0] = REPLY_TYPE_ANNOUNCE;
					client_buffer[1] = stations.station_num[stationNum].song_name_size;
					
					strncpy((char *) (client_buffer+2), stations.station_num[stationNum].song_name, stations.station_num[stationNum].song_name_size);
					
					write(clients[who].socket, &client_buffer, (2 + stations.station_num[stationNum].song_name_size));
				}
				else {		// Invalid station asked
					sprintf(cmdString, "Station %d does not exist\n", stationNum);
					invalidCommand(who, cmdString);
					printf("Client #%d asked station %d that does not exist\n", who+1, stationNum);
					removeClient(who);
					return;
				}
			break;
			
			// ========================= [ Permit Handler ] =========================
			case REPLY_TYPE_PERMIT_SONG:
				pthread_mutex_lock(&mtx);		// LOCK
				if(permitSong == 0) {
					pthread_mutex_unlock(&mtx);	// UN-LOCK
					memset(client_buffer, '\0', sizeof(client_buffer));	// clear buffer before using it
					
					permitUpload(who, 0);
					
					continue;	// go back to the start
				}
				else {
					permitSong = 0;		// block further upload requests
					pthread_mutex_unlock(&mtx);	// UN-LOCK
					
					songNameSize = client_buffer[5];
					songSize = htonl(*(uint32_t *) (client_buffer+1));
					songName = (char *) malloc(songNameSize);
					strncpy(songName, (client_buffer+6), songNameSize);	// copy song's name
					
					if(recBytes != 6 + songNameSize) {
						free(songName);		// remove the allocation
						invalidCommand(who, "Error: UpSong command must be at a size of 3 bytes");
						printf("Disconnecting client #%d - a command sent with unexpected length (UpSong)\n", who+1);
						removeClient(who);
						break;
					}
					
					if(access(songName, F_OK) != -1) {		// check if the song already exists
						permitUpload(who, 0);	// do not permit
						printf("Client #%d tried to upload a file already exists ('%s'), sending disapproval\n", who+1, songName);
					}
					else if(songSize < 2000 || songSize > 10485760) {
						invalidCommand(who, "Error: The size of the song is Illegal");
						
						if(songSize < 2000) {
							printf("Client #%d tried to upload a file smaller than 2000 B, sending disapproval\n", who+1);
						}
						else {
							printf("Client #%d tried to upload a file larger than 10 MB, sending disapproval\n", who+1);
						}
						removeClient(who);
					}
					else {
						if(!(recSong = fopen(songName, "w"))) {			// open and create the new song with writing permissions
							printf("Can't open file '%s'\n", songName);
							
							permitUpload(who, 0);	// do not permit
						}
						else {
							permitUpload(who, 1);	// we can receive the song!! permit the upload
							
							printf("Song download has started!\n");
							
							memset(client_buffer, '\0', sizeof(client_buffer));	// clear buffer before using it
							songValid = 1;	// initial assumption
							
							while(receivedBytes < songSize) {			// receive song from client
								FD_CLR(clients[who].socket, &clients[who].client_fd);
								FD_SET(clients[who].socket, &clients[who].client_fd);
								
								timeout.tv_sec = 0;
								timeout.tv_usec = 3000000;
							
								if((select(FD_SETSIZE, &clients[who].client_fd, NULL, NULL, &timeout) <= 0) && (errno != EINTR)) {	
									invalidCommand(who, "Error: didn't sent data to upload within 3 seconds.");
									printf("\nError while uploading... terminating the client\n");
									songValid = 0 ;
									removeClient(who);
									break;
								}
								
								if((recBytes = read(clients[who].socket, client_buffer, NUM_OF_BYTES)) < 0) {
									printf("Error downloading song from client #%d\n", who);
									songValid = 0;
									break;
								}
								else if(recBytes == 0) {
									printf("Finished receiving the song\n");
									break;
								}
								fwrite(client_buffer, 1, recBytes, recSong);	// save the received data
								receivedBytes += recBytes;
								
								percent = (int)((100*receivedBytes)/songSize);
								printf("\r %d%% Downloading.. bytes received: %d / %d", percent, receivedBytes, songSize);
							}
							printf("\n\n");
							fclose(recSong);
							
							receivedBytes = 0;		// initialize to prepare for the next time
							recBytes = 0;
							
							if(songValid) {
								lastStation++;
								insertArray(&stations, base_multicast_addr + htonl(lastStation));	// create new station
								
								stations.station_num[stations.used-1].song_name = (char *) malloc(songNameSize);	// allocate memory for the new song
								strncpy(stations.station_num[stations.used-1].song_name, songName, songNameSize);	// copy song name to the right struct
								stations.station_num[stations.used-1].song_name[songNameSize] = '\0';
								stations.station_num[stations.used-1].song_name_size = songNameSize;
								stations.station_num[stations.used-1].song_size = songSize;
								open_song(stations.used-1);		// re-open the song in the right struct
								free(songName);		// free the temporary data
								
								// prepare the new stations update message
								memset(client_buffer, '\0', sizeof(client_buffer));	// clear buffer before using it
								countedClients = 0;
								client_buffer[0] = REPLY_TYPE_NEW_STATIONS;
								*(uint16_t *) (client_buffer+1) = stations.used;
								for(i=0; i<MAX_CLIENTS; i++) {		// notify all clients
									if(clients[i].inUse) {
										countedClients++;
										write(clients[i].socket, &client_buffer, 3);	// send to the i-th client
									}
									
									if(countedClients == connectedClients) break;		// stop searching for clients on the list in case we've found them all
								}
								
								printf("Song download has finished! all clients have been updated\n");
							}
							
							printMenu();	// show the menu after the upload procedure
						}
					}
					
					pthread_mutex_lock(&mtx);	// LOCK
					permitSong = 1;
					pthread_mutex_unlock(&mtx);	// UN-LOCK
				}
			break;
			
			// ========================= [ Double Welcome Handler ] =========================
			case REPLY_TYPE_WELCOME:	// shouldn't get here - if it does, disconnect the client
				invalidCommand(who, "More than 1 hello was sent.. disconnecting from server");
				printf("Client #%d has left the radio station - sent more than 1 hello\n", who+1);
				removeClient(who);
				return;			// exit the thread
			break;
			
			// ========================= [ Unknown Types Handler ] =========================
			default:	// invalid user's command type:: REPLY_TYPE_INVALID_CMD
				invalidCommand(who, "Wrong command was sent.. disconnecting from server");
				printf("Client #%d has left the radio station\n", who+1);
				removeClient(who);
				return;			// exit the thread
			break;
		}
	}
}

void initArray(Array * stations, size_t initialSize) {
	if(initialSize <= 0) {
		initialSize = 1;
	}
	
	stations->station_num = (Station *) malloc(initialSize * sizeof(Station));
	stations->used = 0;
	stations->size = initialSize;
}

void insertArray(Array * stations, int element) {
	if(stations->used == stations->size) {		// stations->used mark the number of used stations
		stations->size += 2;
		stations->station_num = (Station *) realloc(stations->station_num, stations->size * sizeof(Station));
	}
	
	stations->station_num[stations->used++].multi_address = element;	// add the new multicast address to the new index
}

void freeArray(Array * stations) {
	int k;
	for (k = 0; k < stations->used; k++) {		
		if(fclose(stations.station_num[k].song) < 0) {
			printf("Error closing song file\n");
		}
	}
	
	free(stations->station_num->song_name);
	free(stations->station_num);
	
	stations->station_num = NULL;
	stations->used = stations->size = 0;
}

void error(const char * str) {
	perror(str);
	printf("\n");
}

void open_song(int k) {
	if(!(stations.station_num[k].song = fopen(stations.station_num[k].song_name, "r"))) {
		error("Couldn't open song file");
	}
}

int getSlot() {
	int i;
	
	for(i=0; i<MAX_CLIENTS; i++) {
		if(!clients[i].inUse) {
			clients[i].inUse = 1;
			connectedClients++;
			return i;
		}
	}
	
	return -1;
}

void removeClient(int who) {
	clients[who].inUse = 0;
	if(connectedClients > 0) connectedClients--;
	
	if(clients[who].hasSocket) {
		close(clients[who].socket);
		clients[who].hasSocket = 0;
	}
	
	if(clients[who].hasThread) {
		pthread_cancel(clients[who].thread); 
		clients[who].hasThread = 0;
	}

}

void invalidCommand(int who, char * errorMsg) {
	int	msgSize = strlen(errorMsg);
	char invalid_buffer[msgSize+3];
	
	invalid_buffer[0] = REPLY_TYPE_INVALID_CMD;
	invalid_buffer[1] = msgSize;
	strncpy(invalid_buffer+2, errorMsg, msgSize);

	if(clients[who].hasSocket) {	// check that there is an opened socket before using it, because invalid commands might be sent before socket has opened
		write(clients[who].socket, &invalid_buffer, sizeof(invalid_buffer));
	}
}

void permitUpload(int who, uint8_t type) {
	char buffer[2] = {'\0'};
	
	buffer[0] = REPLY_TYPE_PERMIT_SONG;
	buffer[1] = type;
	write(clients[who].socket, &buffer, 2);
}

void printMenu() {
	printf("==================== [ Menu ] ====================\n"
			"- p:  Display the stations and connected clients\n"
			"- q:  Quit the server\n"
			"==================================================\n\n");
}
