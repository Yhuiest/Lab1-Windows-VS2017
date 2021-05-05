#include <stdio.h>
#include <string.h>

#include "protocol.h"
/* FRAME kind */
#define FRAME_DATA 1
#define FRAME_ACK  2
#define FRAME_NAK  3

#define DATA_TIMER  2000
#define ACK_TIMER   500
#define MAX_SEQ  7
#define INC(n) (n = n < MAX_SEQ ? n+1 : 0) 
#define NR_BUF ((MAX_SEQ + 1) / 2)

struct FRAME
{
    unsigned char kind;
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN];
    unsigned int  padding;
};

typedef enum { true, false } boolean;

/*  
    DATA Frame
    +=========+========+========+===============+========+
    | KIND(1) | SEQ(1) | ACK(1) | DATA(240~256) | CRC(4) |
    +=========+========+========+===============+========+

    ACK Frame
    +=========+========+========+
    | KIND(1) | ACK(1) | CRC(4) |
    +=========+========+========+

    NAK Frame
    +=========+========+========+
    | KIND(1) | ACK(1) | CRC(4) |
    +=========+========+========+
*/


