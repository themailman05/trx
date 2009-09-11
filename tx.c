#include <netdb.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <celt/celt.h>
#include <ortp/ortp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "defaults.h"
#include "device.h"
#include "sched.h"

static unsigned int verbose = DEFAULT_VERBOSE;

static RtpSession* create_rtp_send(const char *addr_desc, const int port)
{
	RtpSession *session;

	session = rtp_session_new(RTP_SESSION_SENDONLY);
	assert(session != NULL);

	rtp_session_set_scheduling_mode(session, 0);
	rtp_session_set_blocking_mode(session, 0);
	rtp_session_set_connected_mode(session, FALSE);
	if (rtp_session_set_remote_addr(session, addr_desc, port) != 0)
		abort();
	if (rtp_session_set_payload_type(session, 0) != 0)
		abort();
	if (rtp_session_set_multicast_ttl(session, 16) != 0)
		abort();

	return session;
}

static int send_one_frame(snd_pcm_t *snd,
		const unsigned int channels,
		const snd_pcm_uframes_t samples,
		CELTEncoder *encoder,
		const size_t bytes_per_frame,
		RtpSession *session)
{
	float *pcm;
	void *packet;
	ssize_t z;
	snd_pcm_sframes_t f;
	static int ts = 0;

	pcm = alloca(sizeof(float) * samples * channels);
	packet = alloca(bytes_per_frame);

	f = snd_pcm_readi(snd, pcm, samples);
	if (f < 0) {
		aerror("snd_pcm_readi", f);
		return -1;
	}
	if (f < samples)
		fprintf(stderr, "Short read, %ld\n", f);

	z = celt_encode_float(encoder, pcm, NULL, packet, bytes_per_frame);
	if (z < 0) {
		fputs("celt_encode_float failed\n", stderr);
		return -1;
	}

	rtp_session_send_with_ts(session, packet, z, ts);
	ts += samples;

	return 0;
}

static int run_tx(snd_pcm_t *snd,
		const unsigned int channels,
		const snd_pcm_uframes_t frame,
		CELTEncoder *encoder,
		const size_t bytes_per_frame,
		RtpSession *session)
{
	for (;;) {
		int r;

		r = send_one_frame(snd, channels, frame,
				encoder, bytes_per_frame,
				session);
		if (r == -1)
			return -1;

		if (verbose > 1)
			fputc('>', stderr);
	}
}

static void usage(FILE *fd)
{
	fprintf(fd, "Usage: tx [<parameters>]\n");

	fprintf(fd, "\nAudio device (ALSA) parameters:\n");
	fprintf(fd, "  -d <dev>    Device name (default '%s')\n",
		DEFAULT_DEVICE);
	fprintf(fd, "  -m <ms>     Buffer time (milliseconds, default %d)\n",
		DEFAULT_BUFFER);

	fprintf(fd, "\nNetwork parameters:\n");
	fprintf(fd, "  -h <addr>   IP address to send to (default %s)\n",
		DEFAULT_ADDR);
	fprintf(fd, "  -p <port>   UDP port number (default %d)\n",
		DEFAULT_PORT);

	fprintf(fd, "\nEncoding parameters:\n");
	fprintf(fd, "  -r <rate>   Sample rate (default %d)\n",
		DEFAULT_RATE);
	fprintf(fd, "  -c <n>      Number of channels (default %d)\n",
		DEFAULT_CHANNELS);
	fprintf(fd, "  -f <bytes>  Frame size (default %d)\n",
		DEFAULT_FRAME);
	fprintf(fd, "  -b <kbps>   Bitrate (approx., default %d)\n",
		DEFAULT_BITRATE);

	fprintf(fd, "\nDisplay parameters:\n");
	fprintf(fd, "  -v <n>      Verbosity level (default %d)\n",
		DEFAULT_VERBOSE);
}

int main(int argc, char *argv[])
{
	int r;
	size_t bytes_per_frame;
	snd_pcm_t *snd;
	CELTMode *mode;
	CELTEncoder *encoder;
	RtpSession *session;

	/* command-line options */
	const char *device = DEFAULT_DEVICE,
		*addr = DEFAULT_ADDR;
	unsigned int buffer = DEFAULT_BUFFER,
		rate = DEFAULT_RATE,
		channels = DEFAULT_CHANNELS,
		frame = DEFAULT_FRAME,
		kbps = DEFAULT_BITRATE,
		port = DEFAULT_PORT;

	for (;;) {
		int c;

		c = getopt(argc, argv, "b:c:d:f:h:m:p:r:v:");
		if (c == -1)
			break;

		switch (c) {
		case 'b':
			kbps = atoi(optarg);
			break;
		case 'c':
			channels = atoi(optarg);
			break;
		case 'd':
			device = optarg;
			break;
		case 'f':
			frame = atol(optarg);
			break;
		case 'h':
			addr = optarg;
			break;
		case 'm':
			buffer = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 'v':
			verbose = atoi(optarg);
			break;
		default:
			usage(stderr);
			return -1;
		}
	}

	mode = celt_mode_create(rate, channels, frame, NULL);
	if (mode == NULL) {
		fputs("celt_mode_create failed\n", stderr);
		return -1;
	}
	encoder = celt_encoder_create(mode);
	if (encoder == NULL) {
		fputs("celt_encoder_create failed\n", stderr);
		return -1;
	}
	if (celt_encoder_ctl(encoder, CELT_SET_PREDICTION(2)) != CELT_OK) {
		fputs("CELT_SET_PREDICTION failed\n", stderr);
		return -1;
	}

	bytes_per_frame = kbps * 1024 * frame / rate / 8;
	fprintf(stderr, "bytes_per_frame = %d\n", bytes_per_frame);

	if (go_realtime() != 0)
		return -1;

	ortp_init();
	ortp_scheduler_init();
	ortp_set_log_level_mask(ORTP_MESSAGE|ORTP_WARNING|ORTP_ERROR);
	session = create_rtp_send(addr, port);
	assert(session != NULL);

	r = snd_pcm_open(&snd, device, SND_PCM_STREAM_CAPTURE, 0);
	if (r < 0) {
		aerror("snd_pcm_open", r);
		return -1;
	}
	if (set_alsa_hw(snd, rate, channels, buffer * 1000) == -1)
		return -1;
	if (set_alsa_sw(snd) == -1)
		return -1;

	r = run_tx(snd, channels, frame, encoder, bytes_per_frame, session);

	if (snd_pcm_close(snd) < 0)
		abort();

	rtp_session_destroy(session);
	ortp_exit();
	ortp_global_stats_display();

	celt_encoder_destroy(encoder);
	celt_mode_destroy(mode);

	return r;
}