#ifndef HEADER_H
#define HEADER_H

#define PAYLOAD_MAX_SIZE 999
#define PACKET_MAX_SIZE sizeof(struct header) + (sizeof(char) * PAYLOAD_MAX_SIZE)
#define CONN_REQ 0x01
#define CONN_TERM 0x02
#define CONN_ACCP 0x10
#define CONN_DENY 0x20
#define PKT 0x04
#define ACK 0x08

struct header {
    unsigned char flags;
    unsigned char pktseq;
    unsigned char ackseq;
    unsigned char unassigned;
    int senderid;
    int recvid;
    int metadata;
};


struct header* createHeader(unsigned char flags, unsigned char pktseq, unsigned char ackseq, int senderid, int recvid, int metadata) {
    struct header* h = malloc(sizeof(struct header));
    h->flags = flags;
    h->pktseq = pktseq;
    h->ackseq = ackseq;
    h->unassigned = 0;
    h->senderid = senderid;
    h->recvid = recvid;
    h->metadata = metadata;
    return h;
}


#endif