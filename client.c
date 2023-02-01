#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <time.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "send_packet.h"
#include "header.h"


/*Function that sends a termination packet to the server when it receives an empty data packet
socket: the client socket to send packet from
senderid: the ID of the client, set as senderid
server: the server address*/
void terminate_connection(int socket, int senderid, struct sockaddr_in server) {
    char* packet = (char *) createHeader(CONN_TERM, 0, 0, htonl(senderid), htonl(0), 0);
    int send = send_packet(socket, packet, sizeof(struct header), 0, (struct sockaddr*)&server, sizeof(server));
    free(packet);
}


/*Help methods to make main less cluttered and more readable*/
char* get_filename(int clientid) {
    char* filename = malloc(sizeof(char) * 20);
    strcpy(filename, "kernel-file-");
    char string[5];
    sprintf(string, "%i", clientid);
    strcat(filename, string);
    return filename;
}

int file_exists(char* filename, int socket, int senderid, struct sockaddr_in server) {
    struct stat buffer;
    if (stat(filename, &buffer) == 0) {
        printf("ERROR: The file %s already exists\n", filename);
        terminate_connection(socket, senderid, server); 
        free(filename);
        return 0;
    }
    return 1;
}

int test_open_file(FILE* file, char* filename, int socket, int senderid, struct sockaddr_in server) {
    if (file == NULL) {
        printf("ERROR: The file %s could not be opened\n", filename);
        terminate_connection(socket, senderid, server); 
        free(filename);
        fclose(file);
        return 0;
    }
    return 1;
}


/*Function that writes to file if the packet sequence has the correct ack
file: pointer to the file we want to write to
packet: the packet containing the payload
ack: the ack number in the sequence we are currently at*/
int rdp_write(FILE* file, char* packet, int ack) {
    struct header* header = (struct header*) packet;
    char* payload = (packet + sizeof(struct header));
    if (header->pktseq == (ack + 1)) {
        printf("Writing to file with payload from pkt nr: %d\n", header->pktseq);
        fwrite(payload, header->metadata, 1, file);
        return 1;
    }
    return 0;
}


/*Function to create an ack packet and send it
socket: the client socket to send packet from
senderid: the ID of the client, set as senderid
server: the server address
ack: the packet it is acking for*/
void send_ack(int socket, int senderid, struct sockaddr_in server, int ack) {
    char* packet = (char *) createHeader(ACK, 0, ack, htonl(senderid), htonl(0), 0);
    int send = send_packet(socket, packet, sizeof(struct header), 0, (struct sockaddr*)&server, sizeof(server));
    free(packet);
}


int main(int argc, char* argv[]) {

    if(argc < 4) {
        printf("3 arguments needed: <IPv4 address / hostname of server> <UDP port of server> <loss probability>\n");
        return 1;
    }


    /*Set arguments*/
    unsigned char* address = argv[1];
    unsigned int port = atoi(argv[2]);
    float prob = atof(argv[3]);
    srand(time(0));
    int senderid = rand() % 10000 + 1;
    set_loss_probability(prob);

    if (prob < 0 || prob > 1 || port == 0) {
        printf("ERROR: The port must be a digit,\n");
        printf(" and the loss probability must be between 0 and 1\n");
        return 2;
    }


    /*Create socket for client*/
    int get_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (get_socket == -1) {
        printf("ERROR: Socket is invalid\n");
        return 3;
    }


    /*Create sockaddr with information about the server*/
    struct sockaddr_in server_address;
    int len = sizeof(struct sockaddr_in);
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = inet_addr(address);


    /*Create socket for server*/
    fd_set set;
    struct timeval timeout;
    int stop;
    /*Reset file descriptors*/
    FD_ZERO(&set);
    FD_CLR(get_socket, &set);
    FD_SET(get_socket, &set);
    /*Set timer to wait for 100 milliseconds*/
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;


    /*Send a connect request*/

    char* packet = (char *) createHeader(CONN_REQ, 0, 0, htonl(senderid), htonl(0), 0);
    int send = send_packet(get_socket, packet, sizeof(struct header), 0, (struct sockaddr*)&server_address, sizeof(server_address));
    free(packet);


    /*Wait one second*/
    stop = select(FD_SETSIZE, &set, NULL, NULL, &timeout);

    /*Check if anything has come in on the socket
    if something came, check if it was an accept or reject packet
    else close the client*/
    if (FD_ISSET(get_socket, &set)) {

        struct header connect_answer;
        int reply = recvfrom(get_socket, &connect_answer, sizeof(struct header), 0, (struct sockaddr*)&server_address, &len);
    
        /*If the connection was accepted, start a loop that receives the packets*/
        if (connect_answer.flags == CONN_ACCP) {

            char* filename = get_filename(senderid);

            /*NOTE: both ifs will send a termination packet,
            so that the entire program does not crash because one client failed here*/
            if (file_exists(filename, get_socket, senderid, server_address) == 0) {
                return 4;
            }
            FILE* file = fopen(filename, "wb");
            if (test_open_file(file, filename, get_socket, senderid, server_address) == 0) {
                return 5;
            }


            int ack = 0;

            while (1) {

                FD_ZERO(&set);
                FD_CLR(get_socket, &set);
                FD_SET(get_socket, &set);
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000;


                stop = select(FD_SETSIZE, &set, NULL, NULL, &timeout);
                if (FD_ISSET(get_socket, &set)) {

                    /*Save packet to a char array with the greates size the packet can have*/
                    char* packet = malloc(PACKET_MAX_SIZE);
                    reply = recvfrom(get_socket, packet, PACKET_MAX_SIZE, 0, (struct sockaddr*)&server_address, &len);

                    /*Create structure for header for easier access*/
                    struct header* header = (struct header*) packet;

                    /*Send packet based on flag and size of payload*/
                    if (header->flags == PKT && header->metadata != 0) {

                        send_ack(get_socket, senderid, server_address, ack);
                        printf("Sending ack-packet: %d\n", ack);

                        /*If the file was written to, increase ack by one*/
                        ack += rdp_write(file, packet, ack);

                        free(packet);

                    }
                    else if (header->flags == PKT && header->metadata == 0) {
                        printf("Sending termination\n");
                        terminate_connection(get_socket, senderid, server_address);
                        free(packet);
                        break;
                    }
                    else {
                        printf("ERROR: Why did the client receive a packet that is not a data packet here?\n");
                        terminate_connection(get_socket, senderid, server_address);
                        fclose(file);
                        free(filename);
                        return 5;
                    }
                }
                else {
                    send_ack(get_socket, senderid, server_address, ack);
                    printf("Sending ack-packet again\n");
                }
            }

            printf("\nFILE %s: download complete\n\n", filename);
            fclose(file);
            free(filename);

        }
        else if (connect_answer.flags == CONN_DENY) {
            printf("Received a reject packet. Terminating\n");
        }
        else {
            /*NOTE: this should never occur, but I added it for testing and as a formality*/
            printf("ERROR: Received a flag this is not an accept or a refuse. Terminating. Flag: %d\n", connect_answer.flags);
        }

    }
    else {
        printf("ERROR: Did not receive a response to request within the time limit. Terminating.\n");
    }

    return 0;
}