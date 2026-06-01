#define _GNU_SOURCE

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define O4L_MAGIC 0x4f344c4bu
#define O4L_VERSION 1
#define O4L_MAX_PAYLOAD 1400
#define O4L_MAX_FRAME 1800
#define O4L_DEFAULT_IFACE "wlan0"
#define O4L_DEFAULT_FREQ_MHZ 5180
#define O4L_DEFAULT_HT_MODE "HT20"
#define O4L_DEFAULT_UDP_LISTEN 5601
#define O4L_DEFAULT_UDP_DEST_PORT 5602
#define IEEE80211_FTYPE_MGMT_ACTION 0x00d0
#define O4L_FLAG_FEC_DATA 0x0001
#define O4L_FLAG_FEC_PARITY 0x0002
#define O4L_FEC_MAX_DATA 16

enum stream_id {
	STREAM_VIDEO = 1,
	STREAM_MAVLINK_AIR_TO_GROUND = 2,
	STREAM_MAVLINK_GROUND_TO_AIR = 3,
	STREAM_CONTROL = 4,
	STREAM_TEST = 255,
};

struct radiotap_hdr {
	uint8_t version;
	uint8_t pad;
	uint16_t len;
	uint32_t present;
} __attribute__((packed));

struct ieee80211_hdr {
	uint16_t frame_control;
	uint16_t duration_id;
	uint8_t addr1[6];
	uint8_t addr2[6];
	uint8_t addr3[6];
	uint16_t seq_ctrl;
} __attribute__((packed));

struct o4l_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t stream_id;
	uint16_t header_len;
	uint32_t sequence;
	uint64_t timestamp_us;
	uint16_t payload_len;
	uint16_t reserved;
	uint32_t crc32;
} __attribute__((packed));

struct o4l_fec_hdr {
	uint32_t block_id;
	uint16_t orig_len;
	uint16_t len_xor;
	uint8_t data_shards;
	uint8_t parity_shards;
	uint8_t shard_index;
	uint8_t reserved;
} __attribute__((packed));

struct config {
	const char *mode;
	const char *iface;
	const char *freq_mhz;
	const char *ht_mode;
	const char *udp_dest_host;
	uint16_t udp_listen_port;
	uint16_t udp_dest_port;
	uint8_t stream_id;
	int interval_ms;
	int count;
	int fec_data;
	int fec_parity;
	bool setup_wifi;
	bool verbose;
};

struct stats {
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t rx_bad;
	uint64_t fec_recovered;
	uint64_t fec_unusable;
	uint32_t last_rx_sequence;
	bool have_rx_sequence;
};

struct fec_tx_state {
	uint32_t block_id;
	int shards;
	uint16_t len_xor;
	size_t parity_len;
	uint8_t parity[O4L_MAX_PAYLOAD];
};

struct fec_rx_block {
	bool used;
	bool got_parity;
	bool recovered;
	uint32_t block_id;
	uint8_t data_shards;
	uint16_t len_xor;
	size_t parity_len;
	uint8_t parity[O4L_MAX_PAYLOAD];
	bool got_data[O4L_FEC_MAX_DATA];
	uint16_t data_len[O4L_FEC_MAX_DATA];
	uint8_t data[O4L_FEC_MAX_DATA][O4L_MAX_PAYLOAD];
};

struct fec_rx_state {
	struct fec_rx_block blocks[O4L_FEC_MAX_DATA];
};

static volatile sig_atomic_t g_stop;

static const uint8_t addr_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t addr_src[6] = {0x02, 0x04, 0x4c, 0x00, 0x00, 0x01};
static const uint8_t addr_bssid[6] = {0x02, 0x04, 0x4c, 0x4f, 0x34, 0x00};

static void on_signal(int signo)
{
	(void)signo;
	g_stop = 1;
}

static uint64_t monotonic_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: omega4-linkd --mode tx|rx|ping [options]\n"
		"\n"
		"options:\n"
		"  --iface IFACE            Wi-Fi interface (default %s)\n"
		"  --setup                  Put interface in monitor mode and set frequency\n"
		"  --freq MHZ               Frequency for --setup (default %d)\n"
		"  --ht-mode MODE           HT mode for --setup (default %s)\n"
		"  --udp-listen PORT        UDP input for tx mode (default %d)\n"
		"  --udp-dest HOST:PORT     UDP output for rx mode (default 127.0.0.1:%d)\n"
		"  --stream ID              Stream id, 1 video, 2 mav a2g, 3 mav g2a, 4 control\n"
		"  --fec DATA:PARITY        XOR FEC shards per block, PARITY must be 1 for now\n"
		"  --interval-ms MS         Ping interval (default 1000)\n"
		"  --count N                Stop ping mode after N packets (default unlimited)\n"
		"  --verbose                Print each accepted packet\n",
		O4L_DEFAULT_IFACE, O4L_DEFAULT_FREQ_MHZ, O4L_DEFAULT_HT_MODE,
		O4L_DEFAULT_UDP_LISTEN, O4L_DEFAULT_UDP_DEST_PORT);
}

static int run_cmd(const char *const argv[])
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int setup_wifi(const struct config *cfg)
{
	const char *ip_down[] = {"ip", "link", "set", "dev", cfg->iface, "down", NULL};
	const char *iw_monitor[] = {"iw", "dev", cfg->iface, "set", "type", "monitor", NULL};
	const char *ip_up[] = {"ip", "link", "set", "dev", cfg->iface, "up", NULL};
	const char *iw_freq[] = {"iw", "dev", cfg->iface, "set", "freq", cfg->freq_mhz, cfg->ht_mode, NULL};

	if (run_cmd(ip_down) != 0 && errno != EINVAL)
		perror("ip link down");
	if (run_cmd(iw_monitor) != 0) {
		perror("iw monitor");
		return -1;
	}
	if (run_cmd(ip_up) != 0) {
		perror("ip link up");
		return -1;
	}
	if (run_cmd(iw_freq) != 0) {
		perror("iw freq");
		return -1;
	}
	return 0;
}

static int raw_socket_bind(const char *iface)
{
	struct sockaddr_ll addr;
	int ifindex = if_nametoindex(iface);
	int fd;

	if (ifindex == 0) {
		perror("if_nametoindex");
		return -1;
	}

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket(AF_PACKET)");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_ifindex = ifindex;
	addr.sll_protocol = htons(ETH_P_ALL);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind(AF_PACKET)");
		close(fd);
		return -1;
	}
	return fd;
}

static int udp_rx_socket(uint16_t port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	int yes = 1;

	if (fd < 0) {
		perror("socket udp rx");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind udp rx");
		close(fd);
		return -1;
	}
	return fd;
}

static int udp_tx_socket(const char *host, uint16_t port, struct sockaddr_in *dest)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		perror("socket udp tx");
		return -1;
	}
	memset(dest, 0, sizeof(*dest));
	dest->sin_family = AF_INET;
	dest->sin_port = htons(port);
	if (inet_pton(AF_INET, host, &dest->sin_addr) != 1) {
		fprintf(stderr, "invalid --udp-dest host: %s\n", host);
		close(fd);
		return -1;
	}
	return fd;
}

static size_t build_frame(uint8_t *frame, uint8_t stream_id, uint16_t flags,
			  uint32_t sequence,
			  const uint8_t *payload, size_t payload_len)
{
	struct radiotap_hdr *rt = (struct radiotap_hdr *)frame;
	struct ieee80211_hdr *wifi = (struct ieee80211_hdr *)(frame + sizeof(*rt));
	struct o4l_hdr hdr;
	size_t off = sizeof(*rt) + sizeof(*wifi);
	uint32_t crc;

	rt->version = 0;
	rt->pad = 0;
	rt->len = htole16(sizeof(*rt));
	rt->present = htole32(0);

	memset(wifi, 0, sizeof(*wifi));
	wifi->frame_control = htole16(IEEE80211_FTYPE_MGMT_ACTION);
	memcpy(wifi->addr1, addr_broadcast, sizeof(addr_broadcast));
	memcpy(wifi->addr2, addr_src, sizeof(addr_src));
	memcpy(wifi->addr3, addr_bssid, sizeof(addr_bssid));
	wifi->seq_ctrl = htole16((sequence & 0xfff) << 4);

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = htonl(O4L_MAGIC);
	hdr.version = O4L_VERSION;
	hdr.stream_id = stream_id;
	hdr.header_len = htons(sizeof(hdr));
	hdr.sequence = htonl(sequence);
	hdr.timestamp_us = htobe64(monotonic_us());
	hdr.payload_len = htons((uint16_t)payload_len);
	hdr.reserved = htons(flags);
	crc = crc32_update(0, &hdr, offsetof(struct o4l_hdr, crc32));
	crc = crc32_update(crc, payload, payload_len);
	hdr.crc32 = htonl(crc);

	memcpy(frame + off, &hdr, sizeof(hdr));
	off += sizeof(hdr);
	memcpy(frame + off, payload, payload_len);
	off += payload_len;
	return off;
}

static bool parse_frame(const uint8_t *frame, size_t frame_len, struct o4l_hdr *hdr,
			const uint8_t **payload)
{
	uint16_t rt_len;
	size_t off;
	uint16_t payload_len;
	uint32_t got_crc;
	uint32_t calc_crc;
	struct o4l_hdr tmp;

	if (frame_len < sizeof(struct radiotap_hdr) + sizeof(struct ieee80211_hdr) + sizeof(*hdr))
		return false;
	rt_len = le16toh(*(const uint16_t *)(frame + 2));
	if (rt_len < sizeof(struct radiotap_hdr) || rt_len > frame_len)
		return false;
	off = rt_len + sizeof(struct ieee80211_hdr);
	if (off + sizeof(*hdr) > frame_len)
		return false;

	memcpy(hdr, frame + off, sizeof(*hdr));
	if (ntohl(hdr->magic) != O4L_MAGIC || hdr->version != O4L_VERSION)
		return false;
	if (ntohs(hdr->header_len) != sizeof(*hdr))
		return false;

	payload_len = ntohs(hdr->payload_len);
	if (payload_len > O4L_MAX_PAYLOAD || off + sizeof(*hdr) + payload_len > frame_len)
		return false;

	got_crc = ntohl(hdr->crc32);
	memcpy(&tmp, hdr, sizeof(tmp));
	tmp.crc32 = 0;
	calc_crc = crc32_update(0, &tmp, offsetof(struct o4l_hdr, crc32));
	calc_crc = crc32_update(calc_crc, frame + off + sizeof(*hdr), payload_len);
	if (got_crc != calc_crc)
		return false;

	*payload = frame + off + sizeof(*hdr);
	return true;
}

static void print_stats(const struct stats *st)
{
	fprintf(stderr,
		"stats tx=%llu/%lluB rx=%llu/%lluB bad=%llu fec_recovered=%llu fec_unusable=%llu\n",
		(unsigned long long)st->tx_packets,
		(unsigned long long)st->tx_bytes,
		(unsigned long long)st->rx_packets,
		(unsigned long long)st->rx_bytes,
		(unsigned long long)st->rx_bad,
		(unsigned long long)st->fec_recovered,
		(unsigned long long)st->fec_unusable);
}

static int tx_payload_flags(int raw_fd, const struct config *cfg, struct stats *st,
			    uint32_t *sequence, uint16_t flags, const uint8_t *payload,
			    size_t len)
{
	uint8_t frame[O4L_MAX_FRAME];
	size_t frame_len;
	ssize_t sent;

	if (len > O4L_MAX_PAYLOAD)
		len = O4L_MAX_PAYLOAD;
	frame_len = build_frame(frame, cfg->stream_id, flags, (*sequence)++, payload, len);
	sent = send(raw_fd, frame, frame_len, 0);
	if (sent < 0) {
		perror("send raw");
		return -1;
	}
	st->tx_packets++;
	st->tx_bytes += (uint64_t)len;
	return 0;
}

static int tx_payload(int raw_fd, const struct config *cfg, struct stats *st,
		      uint32_t *sequence, const uint8_t *payload, size_t len)
{
	return tx_payload_flags(raw_fd, cfg, st, sequence, 0, payload, len);
}

static int tx_fec_data(int raw_fd, const struct config *cfg, struct stats *st,
		       uint32_t *sequence, struct fec_tx_state *fec,
		       const uint8_t *payload, size_t len)
{
	uint8_t wrapped[O4L_MAX_PAYLOAD];
	struct o4l_fec_hdr fh;
	size_t max_data = O4L_MAX_PAYLOAD - sizeof(fh);

	if (len > max_data)
		len = max_data;
	if (fec->shards == 0) {
		fec->block_id++;
		fec->len_xor = 0;
		fec->parity_len = 0;
		memset(fec->parity, 0, sizeof(fec->parity));
	}

	memset(&fh, 0, sizeof(fh));
	fh.block_id = htonl(fec->block_id);
	fh.orig_len = htons((uint16_t)len);
	fh.data_shards = (uint8_t)cfg->fec_data;
	fh.parity_shards = 1;
	fh.shard_index = (uint8_t)fec->shards;

	memcpy(wrapped, &fh, sizeof(fh));
	memcpy(wrapped + sizeof(fh), payload, len);

	for (size_t i = 0; i < len; i++)
		fec->parity[i] ^= payload[i];
	if (len > fec->parity_len)
		fec->parity_len = len;
	fec->len_xor ^= (uint16_t)len;
	fec->shards++;

	if (tx_payload_flags(raw_fd, cfg, st, sequence, O4L_FLAG_FEC_DATA,
			     wrapped, sizeof(fh) + len) != 0)
		return -1;

	if (fec->shards == cfg->fec_data) {
		memset(&fh, 0, sizeof(fh));
		fh.block_id = htonl(fec->block_id);
		fh.len_xor = htons(fec->len_xor);
		fh.data_shards = (uint8_t)cfg->fec_data;
		fh.parity_shards = 1;
		fh.shard_index = (uint8_t)cfg->fec_data;

		memcpy(wrapped, &fh, sizeof(fh));
		memcpy(wrapped + sizeof(fh), fec->parity, fec->parity_len);
		if (tx_payload_flags(raw_fd, cfg, st, sequence, O4L_FLAG_FEC_PARITY,
				     wrapped, sizeof(fh) + fec->parity_len) != 0)
			return -1;
		fec->shards = 0;
		fec->len_xor = 0;
		fec->parity_len = 0;
		memset(fec->parity, 0, sizeof(fec->parity));
	}
	return 0;
}

static int tx_loop(int raw_fd, const struct config *cfg)
{
	struct stats st = {0};
	struct fec_tx_state fec = {0};
	uint32_t sequence = 0;
	uint8_t payload[O4L_MAX_PAYLOAD];
	int udp_fd = udp_rx_socket(cfg->udp_listen_port);
	time_t last_stats = time(NULL);

	if (udp_fd < 0)
		return 1;
	fprintf(stderr, "tx: iface=%s udp-listen=%u stream=%u fec=%d:%d\n",
		cfg->iface, cfg->udp_listen_port, cfg->stream_id,
		cfg->fec_data, cfg->fec_parity);

	while (!g_stop) {
		struct pollfd pfd = {.fd = udp_fd, .events = POLLIN};
		int ret = poll(&pfd, 1, 1000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll tx");
			break;
		}
		if (ret > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = recv(udp_fd, payload, sizeof(payload), 0);
			if (n > 0) {
				if (cfg->fec_data > 0)
					tx_fec_data(raw_fd, cfg, &st, &sequence, &fec,
						    payload, (size_t)n);
				else
					tx_payload(raw_fd, cfg, &st, &sequence, payload, (size_t)n);
			}
		}
		if (time(NULL) != last_stats) {
			last_stats = time(NULL);
			print_stats(&st);
		}
	}
	close(udp_fd);
	return 0;
}

static int ping_loop(int raw_fd, const struct config *cfg)
{
	struct stats st = {0};
	uint32_t sequence = 0;
	time_t last_stats = time(NULL);

	fprintf(stderr, "ping: iface=%s interval-ms=%d stream=%u\n",
		cfg->iface, cfg->interval_ms, cfg->stream_id);
	while (!g_stop && (cfg->count <= 0 || (int)sequence < cfg->count)) {
		char payload[96];
		int len = snprintf(payload, sizeof(payload), "omega4-link ping seq=%u t=%llu",
				   sequence, (unsigned long long)monotonic_us());
		tx_payload(raw_fd, cfg, &st, &sequence, (const uint8_t *)payload, (size_t)len);
		if (time(NULL) != last_stats) {
			last_stats = time(NULL);
			print_stats(&st);
		}
		usleep((useconds_t)cfg->interval_ms * 1000u);
	}
	return 0;
}

static int udp_emit(int udp_fd, const struct sockaddr_in *udp_dest,
		    const uint8_t *payload, size_t payload_len)
{
	if (sendto(udp_fd, payload, payload_len, 0,
		   (const struct sockaddr *)udp_dest, sizeof(*udp_dest)) < 0) {
		perror("sendto udp");
		return -1;
	}
	return 0;
}

static struct fec_rx_block *fec_rx_block_get(struct fec_rx_state *fec,
					     uint32_t block_id, uint8_t data_shards)
{
	struct fec_rx_block *slot = &fec->blocks[0];

	for (size_t i = 0; i < O4L_FEC_MAX_DATA; i++) {
		if (fec->blocks[i].used && fec->blocks[i].block_id == block_id)
			return &fec->blocks[i];
		if (!fec->blocks[i].used) {
			slot = &fec->blocks[i];
			break;
		}
		if ((int32_t)(fec->blocks[i].block_id - slot->block_id) < 0)
			slot = &fec->blocks[i];
	}

	memset(slot, 0, sizeof(*slot));
	slot->used = true;
	slot->block_id = block_id;
	slot->data_shards = data_shards;
	return slot;
}

static int fec_try_recover(struct fec_rx_block *blk, int udp_fd,
			   const struct sockaddr_in *udp_dest,
			   struct stats *st, const struct config *cfg)
{
	uint8_t recovered[O4L_MAX_PAYLOAD];
	uint16_t recovered_len;
	int missing = -1;
	int missing_count = 0;

	if (!blk->got_parity || blk->recovered)
		return 0;
	for (uint8_t i = 0; i < blk->data_shards; i++) {
		if (!blk->got_data[i]) {
			missing = i;
			missing_count++;
		}
	}
	if (missing_count == 0) {
		blk->recovered = true;
		return 0;
	}
	if (missing_count != 1) {
		st->fec_unusable++;
		return 0;
	}

	if (blk->parity_len > sizeof(recovered))
		return -1;
	memcpy(recovered, blk->parity, blk->parity_len);
	recovered_len = blk->len_xor;
	for (uint8_t i = 0; i < blk->data_shards; i++) {
		if (i == missing)
			continue;
		recovered_len ^= blk->data_len[i];
		for (size_t j = 0; j < blk->parity_len; j++)
			recovered[j] ^= blk->data[i][j];
	}
	if (recovered_len > blk->parity_len) {
		st->fec_unusable++;
		return 0;
	}
	if (udp_emit(udp_fd, udp_dest, recovered, recovered_len) != 0)
		return -1;
	st->rx_packets++;
	st->rx_bytes += recovered_len;
	st->fec_recovered++;
	blk->recovered = true;
	if (cfg->verbose) {
		fprintf(stderr, "fec recovered block=%u shard=%d len=%u\n",
			blk->block_id, missing, recovered_len);
	}
	return 0;
}

static int rx_fec_payload(uint16_t flags, const uint8_t *payload, uint16_t payload_len,
			  int udp_fd, const struct sockaddr_in *udp_dest,
			  struct fec_rx_state *fec, struct stats *st,
			  const struct config *cfg)
{
	struct o4l_fec_hdr fh;
	struct fec_rx_block *blk;
	uint32_t block_id;
	uint16_t body_len;

	if (payload_len < sizeof(fh)) {
		st->rx_bad++;
		return 0;
	}
	memcpy(&fh, payload, sizeof(fh));
	block_id = ntohl(fh.block_id);
	body_len = payload_len - sizeof(fh);
	if (fh.data_shards == 0 || fh.data_shards > O4L_FEC_MAX_DATA ||
	    fh.parity_shards != 1) {
		st->rx_bad++;
		return 0;
	}
	if (cfg->fec_data > 0 && fh.data_shards != cfg->fec_data) {
		st->rx_bad++;
		return 0;
	}

	blk = fec_rx_block_get(fec, block_id, fh.data_shards);
	if (flags & O4L_FLAG_FEC_DATA) {
		uint16_t orig_len = ntohs(fh.orig_len);

		if (fh.shard_index >= fh.data_shards || orig_len > body_len) {
			st->rx_bad++;
			return 0;
		}
		if (!blk->got_data[fh.shard_index]) {
			blk->got_data[fh.shard_index] = true;
			blk->data_len[fh.shard_index] = orig_len;
			memcpy(blk->data[fh.shard_index], payload + sizeof(fh), body_len);
		}
		if (udp_emit(udp_fd, udp_dest, payload + sizeof(fh), orig_len) == 0) {
			st->rx_packets++;
			st->rx_bytes += orig_len;
		}
		return fec_try_recover(blk, udp_fd, udp_dest, st, cfg);
	}

	if (flags & O4L_FLAG_FEC_PARITY) {
		if (fh.shard_index != fh.data_shards || body_len > O4L_MAX_PAYLOAD) {
			st->rx_bad++;
			return 0;
		}
		blk->got_parity = true;
		blk->len_xor = ntohs(fh.len_xor);
		blk->parity_len = body_len;
		memcpy(blk->parity, payload + sizeof(fh), body_len);
		return fec_try_recover(blk, udp_fd, udp_dest, st, cfg);
	}

	st->rx_bad++;
	return 0;
}

static int rx_loop(int raw_fd, const struct config *cfg)
{
	struct sockaddr_in udp_dest;
	struct stats st = {0};
	struct fec_rx_state fec = {0};
	uint8_t frame[O4L_MAX_FRAME];
	int udp_fd = udp_tx_socket(cfg->udp_dest_host, cfg->udp_dest_port, &udp_dest);
	time_t last_stats = time(NULL);

	if (udp_fd < 0)
		return 1;
	fprintf(stderr, "rx: iface=%s udp-dest=%s:%u stream=%u fec=%d:%d\n",
		cfg->iface, cfg->udp_dest_host, cfg->udp_dest_port, cfg->stream_id,
		cfg->fec_data, cfg->fec_parity);

	while (!g_stop) {
		struct pollfd pfd = {.fd = raw_fd, .events = POLLIN};
		int ret = poll(&pfd, 1, 1000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll rx");
			break;
		}
		if (ret > 0 && (pfd.revents & POLLIN)) {
			struct o4l_hdr hdr;
			const uint8_t *payload = NULL;
			ssize_t n = recv(raw_fd, frame, sizeof(frame), 0);
			uint16_t payload_len;
			uint32_t sequence;
			uint16_t flags;

			if (n <= 0)
				continue;
			if (!parse_frame(frame, (size_t)n, &hdr, &payload)) {
				st.rx_bad++;
				continue;
			}
			if (hdr.stream_id != cfg->stream_id && cfg->stream_id != 0)
				continue;
			payload_len = ntohs(hdr.payload_len);
			sequence = ntohl(hdr.sequence);
			flags = ntohs(hdr.reserved);
			if (cfg->verbose) {
				fprintf(stderr, "rx stream=%u seq=%u len=%u flags=0x%04x\n",
					hdr.stream_id, sequence, payload_len, flags);
			}
			if (st.have_rx_sequence && sequence != st.last_rx_sequence + 1)
				fprintf(stderr, "rx gap: last=%u now=%u\n", st.last_rx_sequence, sequence);
			st.have_rx_sequence = true;
			st.last_rx_sequence = sequence;
			if (flags & (O4L_FLAG_FEC_DATA | O4L_FLAG_FEC_PARITY)) {
				if (cfg->fec_data <= 0) {
					st.rx_bad++;
					continue;
				}
				rx_fec_payload(flags, payload, payload_len, udp_fd, &udp_dest,
					       &fec, &st, cfg);
				continue;
			}
			if (udp_emit(udp_fd, &udp_dest, payload, payload_len) == 0) {
				st.rx_packets++;
				st.rx_bytes += payload_len;
			}
		}
		if (time(NULL) != last_stats) {
			last_stats = time(NULL);
			print_stats(&st);
		}
	}
	close(udp_fd);
	return 0;
}

static int parse_host_port(char *arg, const char **host, uint16_t *port)
{
	char *colon = strrchr(arg, ':');
	long parsed_port;

	if (!colon)
		return -1;
	*colon = '\0';
	parsed_port = strtol(colon + 1, NULL, 10);
	if (parsed_port <= 0 || parsed_port > 65535)
		return -1;
	*host = arg;
	*port = (uint16_t)parsed_port;
	return 0;
}

static int parse_fec(char *arg, int *data, int *parity)
{
	char *colon = strchr(arg, ':');
	long parsed_data;
	long parsed_parity;

	if (!colon)
		return -1;
	*colon = '\0';
	parsed_data = strtol(arg, NULL, 10);
	parsed_parity = strtol(colon + 1, NULL, 10);
	if (parsed_data <= 0 || parsed_data > O4L_FEC_MAX_DATA || parsed_parity != 1)
		return -1;
	*data = (int)parsed_data;
	*parity = (int)parsed_parity;
	return 0;
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->mode = "rx";
	cfg->iface = O4L_DEFAULT_IFACE;
	cfg->freq_mhz = "5180";
	cfg->ht_mode = O4L_DEFAULT_HT_MODE;
	cfg->udp_dest_host = "127.0.0.1";
	cfg->udp_listen_port = O4L_DEFAULT_UDP_LISTEN;
	cfg->udp_dest_port = O4L_DEFAULT_UDP_DEST_PORT;
	cfg->stream_id = STREAM_TEST;
	cfg->interval_ms = 1000;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(stdout);
			exit(0);
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			cfg->mode = argv[++i];
		} else if (!strcmp(argv[i], "--iface") && i + 1 < argc) {
			cfg->iface = argv[++i];
		} else if (!strcmp(argv[i], "--setup")) {
			cfg->setup_wifi = true;
		} else if (!strcmp(argv[i], "--freq") && i + 1 < argc) {
			cfg->freq_mhz = argv[++i];
		} else if (!strcmp(argv[i], "--ht-mode") && i + 1 < argc) {
			cfg->ht_mode = argv[++i];
		} else if (!strcmp(argv[i], "--udp-listen") && i + 1 < argc) {
			long port = strtol(argv[++i], NULL, 10);
			if (port <= 0 || port > 65535)
				return -1;
			cfg->udp_listen_port = (uint16_t)port;
		} else if (!strcmp(argv[i], "--udp-dest") && i + 1 < argc) {
			if (parse_host_port(argv[++i], &cfg->udp_dest_host, &cfg->udp_dest_port) != 0)
				return -1;
		} else if (!strcmp(argv[i], "--stream") && i + 1 < argc) {
			long stream = strtol(argv[++i], NULL, 10);
			if (stream < 0 || stream > 255)
				return -1;
			cfg->stream_id = (uint8_t)stream;
		} else if (!strcmp(argv[i], "--fec") && i + 1 < argc) {
			if (parse_fec(argv[++i], &cfg->fec_data, &cfg->fec_parity) != 0)
				return -1;
		} else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
			cfg->interval_ms = atoi(argv[++i]);
			if (cfg->interval_ms <= 0)
				return -1;
		} else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
			cfg->count = atoi(argv[++i]);
			if (cfg->count < 0)
				return -1;
		} else if (!strcmp(argv[i], "--verbose")) {
			cfg->verbose = true;
		} else {
			return -1;
		}
	}

	if (strcmp(cfg->mode, "tx") && strcmp(cfg->mode, "rx") && strcmp(cfg->mode, "ping"))
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	struct config cfg;
	int raw_fd;
	int ret;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(stderr);
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (cfg.setup_wifi && setup_wifi(&cfg) != 0)
		return 1;

	raw_fd = raw_socket_bind(cfg.iface);
	if (raw_fd < 0)
		return 1;

	if (!strcmp(cfg.mode, "tx"))
		ret = tx_loop(raw_fd, &cfg);
	else if (!strcmp(cfg.mode, "ping"))
		ret = ping_loop(raw_fd, &cfg);
	else
		ret = rx_loop(raw_fd, &cfg);

	close(raw_fd);
	return ret;
}
