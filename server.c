#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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


/*Global variables
n: the total number of files to be served
size and packets_num: number of elements in arrays
connections: connections to clients
packets: packets with data from file*/
int n;
int size;
struct rdp_connection** connections;
int packets_num;
char** packets;


/*Struct for a connection
client: address for client
senderid: client id (unique)
active: if the connection has ended or not*/
struct rdp_connection {
    struct sockaddr_in client;
    int id;
    unsigned char active;
    int expected_ack;
};


/*Function that set the amount of packets we need to send the file,
and return size of the last packet
file: file to be made into packets*/
int find_packet_num(FILE* file) {
    fseek(file, 0L, SEEK_END);
    long int result = ftell(file);
    
    packets_num = 0;
    while (result > 0) { //reduce result with the size of one packet, until we have the min amount of packets needed
        result = result - PAYLOAD_MAX_SIZE;
        packets_num++;
    }
    int last_pkt = result + PAYLOAD_MAX_SIZE;
    fseek(file, 0L, SEEK_SET);
    return last_pkt;
}


/*Function that allocates the memory for the entire packet,
puts the payload into the packet, and adds them to array
file: file to be made into payload packets
last_pkt_size: size of the last packet, as all before will contain the max size
Note: The header will be added when the server sends the packet
Note: I chose to save the file to memory, as I felt it was both easier to handle fixed
memory pointers than having to move a pointer back and forth when a packet is dropped.
While it is less efficient memory-wise the larger the file is,
it is beneficial when the number of files to be served increases*/
void create_payloadpacket(FILE* file, int last_pkt_size) {
    char* packet;
    int i;
    for (i = 0; i < packets_num - 1; i++) {
        packet = malloc(PACKET_MAX_SIZE);
        fread(packet + sizeof(struct header), PAYLOAD_MAX_SIZE, 1, file);
        packets[i] = packet;
    }
    packet = malloc(sizeof(struct header) + (sizeof(char) * last_pkt_size));
    fread(packet + sizeof(struct header), last_pkt_size, 1, file);
    packets[packets_num - 1] = packet;
}


/*Function that checks if the packet is a connect request,
and determines wether to add or refuse the connect request
If the new connection has the same ID as an active connection, or size is n,
add a marker that says it is to be rejected
else increase the size of the connections array to fit it, copy all the other connections over and add it
Return the connection object if it was a request, NULL if it was not
packet: the packet that was sent and contains the flags
client: the client socket that we want to save and/or return*/
struct rdp_connection* rdp_accept(struct header packet, struct sockaddr_in client) {
    if (packet.flags == CONN_REQ) {
        struct rdp_connection* new_connect = malloc(sizeof(struct rdp_connection));
        new_connect->client = client;
        new_connect->id = ntohl(packet.senderid);
        new_connect->active = 0;
        new_connect->expected_ack = 0;
        //If the server is not serving the final file at the moment
        if (size < n) {
            /*Check if the ID is already connected,
            if not, resize the connections array to fit the new connection*/
            int num_already_there = 0;
            int i;
            for (i = 0; i < size; i++) {
                if (connections[i]->id == new_connect->id) {
                    num_already_there = 1;
                    break;
                }
            }
            if (num_already_there == 0) {
                new_connect->active = 1;
                size++;
                struct rdp_connection** new_connections = malloc(sizeof(struct rdp_connection*) * size);
                memcpy(new_connections, connections, sizeof(struct rdp_connection*) * (size - 1));
                free(connections);
                connections = new_connections;
                connections[size - 1] = new_connect;
            }
        }
        return new_connect;
    }
    return NULL;
}


/*Function that sends a packet connect or refuse packet
If the connection is inactive due to n or the connection ID is taken, send a refuse packet
Else send a confirm packet
socket: the socket of the server
connect: the connect object containing the socket of the client*/
int confirm_or_reject(int socket, struct rdp_connection* connect) {
    int confirmed = 1;
    unsigned char flag = CONN_ACCP;
    int senderid = connect->id;
    struct sockaddr_in client = connect->client;

    if (connect->active == 0) {
        flag = CONN_DENY;
        free(connect);
        confirmed = 0;
    }

    char* packet = (char *) createHeader(flag, 0, 0, htonl(0), htonl(senderid), 0);
    int send = send_packet(socket, packet, sizeof(struct header), 0, (struct sockaddr*)&client, sizeof(struct sockaddr_in));
    
    if (flag == CONN_DENY) {
        printf("\nNOT ");
    }
    else {
        printf("\n");
    }
    printf("CONNECTED %i %i\n\n", senderid, 0);
    
    free(packet);
    return confirmed;
}


/*Function that
allocates new memory to the connections array,
adds the connections exept the terminated, that is freed,
and frees the previous array, setting the new one as global
senderid: the connection that is to be removed*/
void terminate_connection(int senderid) {
    size--;
    struct rdp_connection** new_connections = malloc(sizeof(struct rdp_connection*) * size);
    int i;
    int x = 0;
    for (i = 0; i < (size + 1); i++) {
        if (connections[i]->id != senderid) {
            new_connections[x] = connections[i];
            x++;
        }
        if (connections[i]->id == senderid) {
            free(connections[i]);
        }
    }
    free(connections);
    connections = new_connections;
    printf("\nDISCONNECTED %i %i\n\n", senderid, 0);
}


/*Function that sends the packet that is on the same index as the ack it received.
If the ack is packets_num, it will send an empty packet
socket: the server socket
senderid: the id to attach to the header. Also used for finding the connection
ackseq: index of the packet to be sent. Used as pktseq
last_pkt_size: only the last packet has a different size, so it needs to be handled separately*/
void send_payloadpacket(int socket, int senderid, int ackseq, int last_pkt_size) {
    struct rdp_connection* client;
    struct sockaddr_in client_address;
    char* packet;
    struct header* header;
    int pkt_size;
    int packet_sequence;
    /*Finds the socket of the client from connections*/
    int i;
    for (i = 0; i < size; i++) {
        if (connections[i]->id == senderid) {
            client = connections[i];
            break;
        }
    }
    client_address = client->client;

    /*Each connection contains the ack it is expecting,
    if the ack received matches, send next packet, and increase expected packet nr with 1
    else set packet nr to be the expected ack*/
    if (ackseq == client->expected_ack) {
        packet_sequence = ackseq + 1;
        client->expected_ack++;
    }
    else {
        packet_sequence = client->expected_ack;
    }

    /*Packets are 0 -> n - 1, n == the empty packet*/
    if (ackseq == packets_num) {
        packet = (char *) createHeader(PKT, packet_sequence, 0, htonl(0), htonl(senderid), 0);
        pkt_size = sizeof(struct header);
    }
    else {
        /*If last packet the packet size is different*/
        packet = packets[packet_sequence - 1];
        if (ackseq == (packets_num - 1)) {
            header = createHeader(PKT, packet_sequence, 0, htonl(0), htonl(senderid), last_pkt_size);
            memcpy(packet, header, sizeof(struct header));
            pkt_size = sizeof(struct header) + (sizeof(char) * last_pkt_size);
        }
        else {
            header = createHeader(PKT, packet_sequence, 0, htonl(0), htonl(senderid), PAYLOAD_MAX_SIZE);
            memcpy(packet, header, sizeof(struct header));
            pkt_size = PACKET_MAX_SIZE;
        }
        free(header);
    }
    
    printf("Sending packet nr: %d\n", packet_sequence);
    int send = send_packet(socket, packet, pkt_size, 0, (struct sockaddr*)&client_address, sizeof(struct sockaddr_in));
    if (ackseq == packets_num) {
        free(packet);
    }
}


int main(int argc, char* argv[]) {
    if(argc < 5) {
        printf("4 arguments needed: <UDP port> <filename> <N number of clients to serve> <loss probability>\n");
        return 1;
    }


    /*Set arguments*/
    unsigned int port = atoi(argv[1]);
    unsigned char* filename = argv[2];
    n = atoi(argv[3]);
    float prob = atof(argv[4]);
    set_loss_probability(prob);

    /*Test that all values that are to be in a certain range are so*/
    if (n < 1 || prob < 0 || prob > 1 || port == 0) {
        printf("ERROR: The port must be a digit,\n");
        printf(" the number of clients to serve must be a positive number >= 1,\n");
        printf(" and the loss probability must be between 0 and 1\n");
        return 2;
    }


    /*Read file to memory as binary, so that we can partition its bytes into packets*/
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        printf("ERROR: The file does not exist or could not be opened\n");
        return 3;
    }


    /*Set global variables*/
    /*the max number of connections is the number of files to serve*/
    /*Initially set to only fit one connection, will be expanded as more connections come in*/
    size = 0;
    connections = malloc(sizeof(struct rdp_connection*) * size);
    /*Find last packet size and assign space in array for all packets to be made*/
    int last_pkt_size = find_packet_num(file);
    int packet_size = (sizeof(struct header) + (sizeof(char)) * PAYLOAD_MAX_SIZE);
    int last_packet_size = (sizeof(struct header) + (sizeof(char)) * last_pkt_size);
    packets = malloc((packet_size * packets_num - 1) + last_packet_size);


    /*Create packets containing data*/
    create_payloadpacket(file, last_pkt_size);


    /*Create socket for server*/
    int get_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (get_socket == -1) {
            printf("ERROR: Socket is invalid\n");
            return 4;
        }

    /*Set information about socket*/
    struct sockaddr_in server_socket;
    server_socket.sin_family = AF_INET;
    server_socket.sin_port = htons(port);
    server_socket.sin_addr.s_addr = INADDR_ANY;

    /*Bind address to socket*/
    int assign_address = bind(get_socket, (struct sockaddr*)&server_socket, sizeof(server_socket));
    if (assign_address == -1) {
        printf("ERROR: Could not establish bind point\n");
        return 5;
    }


    /*Create set of file descriptor and time structure*/
    fd_set set;
    struct timeval timeout;
    int stop;


    int served = 0;
    while (1) {

        /*Reset file descriptors*/
        FD_ZERO(&set);
        FD_CLR(get_socket, &set);
        FD_SET(get_socket, &set);
        /*Set timer to wait for 100 milliseconds*/
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;


        /*Wait for the set time, and check if something has arrived at the socket*/
        stop = select(FD_SETSIZE, &set, NULL, NULL, &timeout);
        if (FD_ISSET(get_socket, &set)) {

            /*Store information about sender*/
            struct sockaddr_in client_socket;
            int len = sizeof(struct sockaddr_in);

            struct header packet;
            /*Receive connection request*/
            int receive = recvfrom(get_socket, &packet, sizeof(struct header), 0, (struct sockaddr*)&client_socket, &len);

            int pkt_senderid = ntohl(packet.senderid);

            /*1. if connect request: Send response to request, and id accept, send first packet*/
            struct rdp_connection* connect = rdp_accept(packet, client_socket);
            if (connect != NULL) {
                int confirmed = confirm_or_reject(get_socket, connect);
                if (confirmed == 1) {
                    send_payloadpacket(get_socket, pkt_senderid, 0, last_pkt_size);
                }
            }

            //2. if ack: Send next packet to the sender
            if (packet.flags == ACK) {
                printf("Received ack: %d from sender %d\n", packet.ackseq, pkt_senderid);
                send_payloadpacket(get_socket, pkt_senderid, packet.ackseq, last_pkt_size);
            }

            //3. if termination packet: Remove client from connections
            if (packet.flags == CONN_TERM) {
                served++;
                terminate_connection(pkt_senderid);
            }

            /*End loop when the number of files to be served and files served are the same*/
            if (served == n) {
                break;
            }
        }
    
    }


    /*Free all connections and packets in arrays, and the arrays themselves*/
    int i;
    for (i = 0; i < size; i++) {
        free(connections[i]);
    }
    free(connections);
    for (i = 0; i < packets_num; i++) {
        free(packets[i]);
    }
    free(packets);
    fclose(file);

    return 0;
}