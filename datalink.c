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

	send_frame(frame, len + sizeof(unsigned int));
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

	if (f.kind == FRAME_DATA)
	{
		lprintf("Send DATA seq:%d ack:%d len:%d, ID %d,\n", f.seq, f.ack, len, *(short*)f.data);
	}
	else if (f.kind == FRAME_ACK)
	{
		lprintf("Send ACK ack:%d\n",f.ack);
	}
	else
	{
		lprintf("Send NAK ack:%d\n", f.ack);
	}

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
			pkt_len[next_frame_to_send] = get_packet(buffer[next_frame_to_send]);
			nbuffer++;
            send_frame_to_physical(FRAME_DATA, next_frame_to_send,
                frame_expected, buffer[next_frame_to_send],
                pkt_len[next_frame_to_send]);
			start_timer(next_frame_to_send, DATA_TIMER);
            stop_ack_timer();
            physical_layer_ready = false;
            INC(next_frame_to_send);
            break;
        case PHYSICAL_LAYER_READY:
            physical_layer_ready = true;
            //dealing goback frame
            while (sending_goback_frame == true && phl_sq_len() < 30000)
            {
                send_frame_to_physical(FRAME_DATA, next_goback_frame, frame_expected,
                    buffer[next_goback_frame], pkt_len[next_goback_frame]);

				start_timer(next_goback_frame, DATA_TIMER);
                INC(next_goback_frame);
                physical_layer_ready = false;

                //all done
                if (next_goback_frame == next_frame_to_send)
                {
                    sending_goback_frame = false;
                    enable_network_layer();
                }
            }
            break;
        case FRAME_RECEIVED:
            frame_len = recv_frame((unsigned char *)&f, sizeof(struct FRAME));

            //frame receive is broken
            if (frame_len < 5 || crc32((unsigned char*)&f, frame_len) != 0)
            {
                lprintf("Receive error! Bad CRC checkcum.seq:%d ack:%d len:%d\n",f.seq,f.ack,frame_len);
                break;
            }

            //arrived frame is expected
            if (f.kind == FRAME_DATA && f.seq == frame_expected)
            {
                lprintf("Receive data frame,len:%d,seq:%d,ack:%d,ack_e:%d,ID:%d\n",
					frame_len,f.seq, f.ack,ack_expected, *(short*)f.data);
                put_packet(f.data, frame_len - 3 - sizeof(unsigned int));
                INC(frame_expected);
                start_ack_timer(ACK_TIMER);
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

            next_goback_frame = ack_expected;
            sending_goback_frame = true;
			send_frame_to_physical(FRAME_DATA, next_goback_frame, frame_expected,
				buffer[next_goback_frame], pkt_len[next_goback_frame]);
			start_timer(next_frame_to_send, DATA_TIMER);
			INC(next_goback_frame);
			physical_layer_ready = false;
			//all done
			if (nbuffer == 0)
			{
				sending_goback_frame = false;
				enable_network_layer();
			}
            disable_network_layer();
            break;
        case ACK_TIMEOUT:
            send_frame_to_physical(FRAME_ACK, next_frame_to_send, frame_expected, buffer[next_frame_to_send], 0);
			stop_ack_timer();
            break;
		}
        if (nbuffer < MAX_SEQ && physical_layer_ready == true && sending_goback_frame == false)
        {
            enable_network_layer();
        }
        else
        {
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

	boolean phl_ready = false;

	protocol_init(argc, argv);
	disable_network_layer();

	while (1)
	{
		event = wait_for_event(&timeout_seq);

		switch (event)
		{
		case NETWORK_LAYER_READY:
			out_packet_len[next_frame_to_send % NR_BUF]
				= get_packet(out_buf[next_frame_to_send % NR_BUF]);
			send_frame_to_physical(FRAME_DATA
				, next_frame_to_send, frame_expected
				, out_buf[next_frame_to_send % NR_BUF]
				, out_packet_len[next_frame_to_send % NR_BUF]);
			start_timer(next_frame_to_send % NR_BUF, DATA_TIMER);
			stop_ack_timer();
			INC(next_frame_to_send);
			nbuffered++;
			break;
		case PHYSICAL_LAYER_READY:
			phl_ready = true;
			break;
		case FRAME_RECEIVED:
			frame_len = recv_frame((unsigned char *)&f, sizeof(struct FRAME));
			if (crc32((unsigned char *)&f, frame_len))
			{
				lprintf("Receive error! Bad CRC checkcum.seq:%d ack:%d len:%d\n", f.seq, f.ack, frame_len);
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
				in_packet_len[f.seq % NR_BUF] = frame_len;
				lprintf("Receive data frame,len:%d,seq:%d,ack:%d,ack_e:%d,ID:%d\n",
					frame_len, f.seq, f.ack, ack_expected, *(short*)f.data);
				if ((f.seq != frame_expected) && no_nak == true)
				{
					send_frame_to_physical(FRAME_NAK, 0, frame_expected
						, NULL,0);
					no_nak = false;
					stop_ack_timer();
				}
				else
				{
					start_ack_timer(ACK_TIMER);
				}

				if (between(frame_expected, f.seq, too_far) == true
					&& (arrived[f.seq % NR_BUF]) == false)
				{
					arrived[f.seq % NR_BUF] = true;
					memcpy(in_buf[f.seq % NR_BUF], f.data, PKT_LEN);
					while (arrived[frame_expected % NR_BUF] == true)
					{
						put_packet(in_buf[frame_expected % NR_BUF], in_packet_len[frame_expected % NR_BUF] - 3 - sizeof(unsigned int));;
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
				send_frame_to_physical(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected
					, out_buf[(f.ack + 1) % NR_BUF], out_packet_len[(f.ack + 1) % NR_BUF]);
				start_timer((f.ack + 1) % NR_BUF,DATA_TIMER);
				stop_ack_timer();
			}
			while (between(ack_expected, f.ack, next_frame_to_send) == true)
			{
				nbuffered--;
				stop_timer(ack_expected % NR_BUF);
				INC(ack_expected);
			}
			break;
		case DATA_TIMEOUT:
			send_frame_to_physical(FRAME_DATA, timeout_seq , frame_expected
				, out_buf[timeout_seq % NR_BUF], out_packet_len[timeout_seq % NR_BUF]);
			start_timer(timeout_seq % NR_BUF, DATA_TIMER);
			stop_ack_timer();
			break;
		case ACK_TIMEOUT:
			send_frame_to_physical(FRAME_ACK, 0, frame_expected, NULL, 0);
			stop_ack_timer();
		}
		if (nbuffered < NR_BUF && phl_ready == true)
		{
			enable_network_layer();
		}
		else
		{
			disable_network_layer();
		}
	}
}
int main(int argc, char** argv)
{
	
	lprintf("Designed by Liu Mingzhe, build: " __DATE__"  "__TIME__"\n");
	if (argc < 2)
	{
		lprintf("Lack of argument.\n");
		return 0;
	}
	/*Please add the characher of g(G) or s(S)
	at the last of the second argument to select
	Go-back-n or selective mode to run*/
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
