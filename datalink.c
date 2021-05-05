#include "datalink.h"


boolean no_nak = true;
static boolean between(unsigned char a, unsigned char b, unsigned char c)
{
	if ((a <= b) && (b < c) || ((c < a) && (a <= b)) || ((b < c) && (c < a)))
	{
		return true;
	}
	else
	{
		return false;
	}
}

static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);

	// sizeof (frame + checksum)
	send_frame(frame, len + sizeof(unsigned int));
	dbg_event("len:%d\n", len);
}

static void send_frame_to_physical(unsigned char frame_kind, unsigned char frame_nr, unsigned char frame_expected, unsigned char *packet, unsigned int len)
{
	struct FRAME f;
	f.kind = frame_kind;
	f.seq = frame_nr;

	if (frame_kind == FRAME_DATA)
	{
		memcpy(f.data, packet, len);
	}

	if (frame_kind == FRAME_NAK)
	{
		no_nak = false;
	}
    f.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
	put_frame((unsigned char *)&f, len + 3);

    dbg_frame("Send DATA %d %d len:%d, ID %d,\n", f.seq, f.ack, len, *(short*)f.data);

}

void Go_back_n(int argc, char** argv)
{
	unsigned char next_frame_to_send = 0;
	unsigned char frame_expected = 0;
	unsigned char ack_expected = 0;

	unsigned char nbuffer = 0;
	int event, timeout_seq;

	char buffer[MAX_SEQ + 1][PKT_LEN];

	int pkt_len[MAX_SEQ + 1];

	struct FRAME f;
    int frame_len;

    boolean physical_layer_ready = true; 
    boolean sending_goback_frame = false;
	unsigned char next_goback_frame ;

	protocol_init(argc, argv);
	disable_network_layer();

	while (1)
	{
		event = wait_for_event(&timeout_seq);

		switch (event)
		{
		case NETWORK_LAYER_READY:
			dbg_event("NETWORK_LAYER_READY!\n");
			pkt_len[next_frame_to_send] = get_packet(buffer[next_frame_to_send]);
			nbuffer++;
            send_frame_to_physical(FRAME_DATA, next_frame_to_send,
                frame_expected, buffer[next_frame_to_send],
                pkt_len[next_frame_to_send]);
			//dbg_frame("Send DATA %d %d, ID %d,\n", f.seq, f.ack, *(short*)f.data);
			start_timer(next_frame_to_send, DATA_TIMER);
            stop_ack_timer();
            physical_layer_ready = false;
			dbg_event("PHYSICAL_LAYER_NOT_READY!\n");
            INC(next_frame_to_send);
            break;
        case PHYSICAL_LAYER_READY:
			dbg_event("PHYSICAL_LAYER_READY!\n");
            physical_layer_ready = true;
            //dealing goback frame
            while (sending_goback_frame == true && phl_sq_len() < 30000)
            {
                send_frame_to_physical(FRAME_DATA, next_goback_frame, frame_expected,
                    buffer[next_goback_frame], pkt_len[next_goback_frame]);

				start_timer(next_goback_frame, DATA_TIMER);
                INC(next_goback_frame);
                physical_layer_ready = false;

                //nbuffer--;
                //all done
                if (next_goback_frame == next_frame_to_send)
                {
                    sending_goback_frame = false;
                    enable_network_layer();
                }
            }
            break;
        case FRAME_RECEIVED:
			dbg_event("FRAME_RECEIVE!\n");
            frame_len = recv_frame((unsigned char *)&f, sizeof(struct FRAME));

            //frame receive is broken
            if (frame_len < 5 || crc32((unsigned char*)&f, frame_len) != 0)
            {
                dbg_frame("Receive error! Bad CRC checkcum.seq:%d ack:%d len:%d\n",f.seq,f.ack,frame_len);
                break;
            }

            //arrived frame is expected
            if (f.kind == FRAME_DATA && f.seq == frame_expected)
            {
                dbg_frame("Receive data frame,len:%d,seq:%d,ack:%d,ack_e:%d,ID:%d\n",
					frame_len,f.seq, f.ack,ack_expected, *(short*)f.data);
                put_packet(f.data, frame_len - 3 - sizeof(unsigned int));
                INC(frame_expected);
                start_ack_timer(ACK_TIMER);
            }
            else
            {
                dbg_frame("Receive ack frame,ack:%d len:%d ack_e:%d\n", f.ack,frame_len,ack_expected);
            }
            //ack is expected
			while (between(ack_expected, f.ack, next_frame_to_send) == true)
            {
				if (nbuffer > 0)
				{
					nbuffer--;
				}
                stop_timer(ack_expected);
				sending_goback_frame = false;
                INC(ack_expected);
				if (sending_goback_frame == true)
				{
					INC(next_goback_frame);
				}
            }
            break;
        case DATA_TIMEOUT:
			dbg_event("DATA_TIMEOUT!\n");
            dbg_event("Data timer timeout!Resend frames.\n");
            next_goback_frame = ack_expected;
            sending_goback_frame = true;
			send_frame_to_physical(FRAME_DATA, next_goback_frame, frame_expected,
				buffer[next_goback_frame], pkt_len[next_goback_frame]);
			start_timer(next_frame_to_send, DATA_TIMER);
			INC(next_goback_frame);
			physical_layer_ready = false;
			//nbuffer--;
			//all done
			if (nbuffer == 0)
			{
				sending_goback_frame = false;
				enable_network_layer();
			}
            disable_network_layer();
            break;
        case ACK_TIMEOUT:
			dbg_event("ACK_TIMEOUT!\n");
            dbg_event("ACK timer timeout!Send a ACK frame.\n");
            send_frame_to_physical(FRAME_ACK, next_frame_to_send, frame_expected, buffer[next_frame_to_send], 0);
			stop_ack_timer();
            break;
		}
        if (nbuffer < MAX_SEQ && physical_layer_ready == true && sending_goback_frame == false)
        {
			dbg_event("enable_network_layer\n");
            enable_network_layer();
        }
        else
        {
			dbg_event("%d %d %d\n", nbuffer , physical_layer_ready == true, sending_goback_frame == false);
			dbg_event("disable_network_layer\n");
            disable_network_layer();
        }
	}
}
void selective(int argc,char** argv)
{
	unsigned char ack_expected = 0;
	unsigned char next_frame_to_send = 0;
	unsigned char frame_expected = 0;
	unsigned char too_far = NR_BUF;

	struct FRAME f;

	char out_buf[NR_BUF][PKT_LEN];
	int out_packet_len[NR_BUF];
	char in_buf[NR_BUF][PKT_LEN];
	int in_packet_len[NR_BUF];
	int frame_len;

	boolean arrived[NR_BUF];
	for (int i = 0; i < NR_BUF; i++)
	{
		arrived[i] = false;
	}
	unsigned char nbuffered = 0;
	unsigned char event;
	int timeout_seq;

	boolean phl_ready = true;

	protocol_init(argc, argv);
	disable_network_layer();

	while (1)
	{
		event = wait_for_event(&timeout_seq);

		switch (event)
		{
		case NETWORK_LAYER_READY:
			dbg_event("NETWORK_LAYER_READY!\n");
			out_packet_len[next_frame_to_send % NR_BUF]
				= get_packet(out_buf[next_frame_to_send % NR_BUF]);
			send_frame_to_physical(FRAME_DATA
				, next_frame_to_send, frame_expected
				, out_buf[next_frame_to_send % NR_BUF]
				, out_packet_len[next_frame_to_send % NR_BUF]);
			//dbg_frame("Send DATA %d %d, ID %d,\n", f.seq, f.ack, *(short*)f.data);
			start_timer(next_frame_to_send % NR_BUF, DATA_TIMER);
			phl_ready = false;
			dbg_event("PHYSICAL_LAYER_NOT_READY!\n");
			stop_ack_timer();
			break;
		case PHYSICAL_LAYER_READY:
			phl_ready = true;
			dbg_event("PHYSICAL_LAYER_READY!\n");
			break;
		case FRAME_RECEIVED:
			dbg_event("FRAME_RECEIVE!\n");
			frame_len = recv_frame((unsigned char *)&f, sizeof(struct FRAME));
			in_packet_len[f.seq % NR_BUF] = frame_len;
			if (crc32((unsigned char *)&f, frame_len))
			{
				dbg_frame("Receive error! Bad CRC checkcum.seq:%d ack:%d len:%d\n", f.seq, f.ack, frame_len);
				if (no_nak == true)
				{
					send_frame_to_physical(FRAME_NAK, 0, frame_expected, NULL, 0);
					no_nak = false;
					stop_ack_timer();
				}
				break;
			}
			if (f.kind == FRAME_DATA)
			{
				dbg_frame("Receive data frame,len:%d,seq:%d,ack:%d,ack_e:%d,ID:%d\n",
					frame_len, f.seq, f.ack, ack_expected, *(short*)f.data);
				if ((f.seq != frame_expected) && no_nak == true)
				{
					send_frame_to_physical(FRAME_NAK, 0, frame_expected
						, NULL,0);
				}
				if (between(frame_expected, f.seq, too_far) == true
					&& (arrived[f.seq % NR_BUF]) == false)
				{
					arrived[f.seq % NR_BUF] = true;
					memcpy(in_buf[f.seq % NR_BUF], f.data, PKT_LEN);
					while (arrived[frame_expected % NR_BUF] == true)
					{
						put_frame(in_buf[frame_expected % NR_BUF], in_packet_len[frame_expected % NR_BUF]);
						no_nak = true;
						arrived[frame_expected % NR_BUF] = false;
						INC(frame_expected);
						INC(too_far);
						start_ack_timer(ACK_TIMER);
					}
				}
			}
			if ((f.kind == FRAME_NAK) && 
				between (ack_expected,(f.ack + 1) % (MAX_SEQ + 1),next_frame_to_send) == true)
			{
				dbg_frame("Receive nak frame,ack:%d len:%d ack_e:%d\n", f.ack, frame_len, ack_expected);
				send_frame_to_physical(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected
					, out_buf[(f.ack + 1) % (MAX_SEQ + 1)], out_packet_len[(f.ack + 1) % (MAX_SEQ + 1)]);
			}
			while (between(ack_expected, f.ack, next_frame_to_send) == true)
			{
				nbuffered--;
				stop_timer(ack_expected % NR_BUF);
				INC(ack_expected);
			}
			break;
		case DATA_TIMEOUT:
			dbg_event("Data timer timeout!Resend frames.\n");
			send_frame_to_physical(FRAME_DATA, timeout_seq, frame_expected
				, out_buf[timeout_seq], out_packet_len[timeout_seq]);
			break;
		case ACK_TIMEOUT:
			dbg_event("ACK timer timeout!Send a ACK frame.\n");
			send_frame_to_physical(FRAME_ACK, 0, frame_expected, NULL, 0);
		}
		if (nbuffered < NR_BUF && phl_ready == true)
		{
			dbg_event("enable_network_layer\n");
			enable_network_layer();
		}
		else
		{
			dbg_event("disable_network_layer\n");
			disable_network_layer();
		}
	}
}
int main(int argc, char** argv)
{
	if (argc < 2)
	{
		lprintf("Lack of argument.\n");
		return 0;
	}

	switch (argv[1][strlen(argv[1])-1])
	{
	case 'G':
	case 'g':
        argv[1][strlen(argv[1]) - 1] = 0;
		Go_back_n(argc, argv);
	case 'S':
	case 's':
		selective(argc,argv);
	default:
		lprintf("Please input the right argument.\n-g -G:\tGo_back_n\n-s -S:\tseletive");
	}
}

/*

网络层接口
#define PKT_LEN 256
void enable_network_layer(void);
void disable_network_layer(void);
int get_packet(unsigned char *packet);
void put_packet(unsigned char *packet, int len);

事件驱动
int wait_for_event(int *arg);
#define NETWORK_LAYER_READY 0
#define PHYSICAL_LAYER_READY 1
#define FRAME_RECEIVED 2
#define DATA_TIMEOUT 3
#define ACK_TIMEOUT 4

物理层接口
void send_frame(unsigned char *frame, int len);
int recv_frame(unsigned char *buf, int size);
int phl_sq_len(void);

CRC校验和
unsigned int crc32(unsigned char *buf, int len);

定时器
unsigned int get_ms(void);
void start_timer(unsigned int nr, unsigned int ms);
void stop_timer(unsigned int nr);
void start_ack_timer(unsigned int ms);
void stop_ack_timer(void);

过程跟踪
extern void dbg_event(char *fmt, ...);
extern void dbg_frame(char *fmt, ...);
extern void dbg_warning(char *fmt, ...);
char *station_name(void);
*/
/*
struct FRAME { 
    unsigned char kind; 
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
};

static unsigned char frame_nr = 0, buffer[PKT_LEN], nbuffered;
static unsigned char frame_expected = 0;
static int phl_ready = 0;

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = 1 - frame_expected;
    memcpy(s.data, buffer, PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = 1 - frame_expected;

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;

    protocol_init(argc, argv); 
    lprintf("Designed by Jiang Yanjun, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(buffer);
            nbuffered++;
            send_data_frame();
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }
            if (f.kind == FRAME_ACK) 
                dbg_frame("Recv ACK  %d\n", f.ack);
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq == frame_expected) {
                    put_packet(f.data, len - 7);
                    frame_expected = 1 - frame_expected;
                }
                send_ack_frame();
            } 
            if (f.ack == frame_nr) {
                stop_timer(frame_nr);
                nbuffered--;
                frame_nr = 1 - frame_nr;
            }
            break; 

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg); 
            send_data_frame();
            break;
        }

        if (nbuffered < 1 && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}
*/