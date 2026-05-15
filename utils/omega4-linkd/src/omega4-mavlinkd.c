#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SERIAL "/dev/ttyS1"
#define DEFAULT_BAUD 57600
#define DEFAULT_UDP_LISTEN 14552
#define DEFAULT_UDP_DEST "127.0.0.1"
#define DEFAULT_UDP_DEST_PORT 14551
#define MAVLINK_BUF_SIZE 512

struct config {
	const char *serial_dev;
	int baud;
	uint16_t udp_listen_port;
	const char *udp_dest_host;
	uint16_t udp_dest_port;
	bool verbose;
};

struct stats {
	uint64_t serial_to_udp;
	uint64_t udp_to_serial;
	uint64_t serial_bytes;
	uint64_t udp_bytes;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
	(void)signo;
	g_stop = 1;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: omega4-mavlinkd [options]\n"
		"\n"
		"options:\n"
		"  --serial DEV             Flight-controller UART (default %s)\n"
		"  --baud RATE              UART baud rate (default %d)\n"
		"  --udp-listen PORT        UDP input written to UART (default %d)\n"
		"  --udp-dest HOST:PORT     UART bytes sent to UDP (default %s:%d)\n"
		"  --verbose                Print transfer counters\n",
		DEFAULT_SERIAL, DEFAULT_BAUD, DEFAULT_UDP_LISTEN,
		DEFAULT_UDP_DEST, DEFAULT_UDP_DEST_PORT);
}

static speed_t baud_to_speed(int baud)
{
	switch (baud) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 921600:
		return B921600;
	default:
		return 0;
	}
}

static int open_serial(const char *dev, int baud)
{
	struct termios tio;
	speed_t speed = baud_to_speed(baud);
	int fd;

	if (!speed) {
		fprintf(stderr, "unsupported baud: %d\n", baud);
		return -1;
	}

	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror("open serial");
		return -1;
	}

	memset(&tio, 0, sizeof(tio));
	if (tcgetattr(fd, &tio) != 0) {
		perror("tcgetattr");
		close(fd);
		return -1;
	}

	cfmakeraw(&tio);
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~CRTSCTS;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		perror("tcsetattr");
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

static int parse_args(int argc, char **argv, struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->serial_dev = DEFAULT_SERIAL;
	cfg->baud = DEFAULT_BAUD;
	cfg->udp_listen_port = DEFAULT_UDP_LISTEN;
	cfg->udp_dest_host = DEFAULT_UDP_DEST;
	cfg->udp_dest_port = DEFAULT_UDP_DEST_PORT;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(stdout);
			exit(0);
		} else if (!strcmp(argv[i], "--serial") && i + 1 < argc) {
			cfg->serial_dev = argv[++i];
		} else if (!strcmp(argv[i], "--baud") && i + 1 < argc) {
			cfg->baud = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--udp-listen") && i + 1 < argc) {
			long port = strtol(argv[++i], NULL, 10);
			if (port <= 0 || port > 65535)
				return -1;
			cfg->udp_listen_port = (uint16_t)port;
		} else if (!strcmp(argv[i], "--udp-dest") && i + 1 < argc) {
			if (parse_host_port(argv[++i], &cfg->udp_dest_host, &cfg->udp_dest_port) != 0)
				return -1;
		} else if (!strcmp(argv[i], "--verbose")) {
			cfg->verbose = true;
		} else {
			return -1;
		}
	}
	return 0;
}

static void print_stats(const struct stats *st)
{
	fprintf(stderr, "stats serial->udp=%llu/%lluB udp->serial=%llu/%lluB\n",
		(unsigned long long)st->serial_to_udp,
		(unsigned long long)st->serial_bytes,
		(unsigned long long)st->udp_to_serial,
		(unsigned long long)st->udp_bytes);
}

int main(int argc, char **argv)
{
	struct sockaddr_in udp_dest;
	struct config cfg;
	struct stats st = {0};
	unsigned char buf[MAVLINK_BUF_SIZE];
	int serial_fd;
	int udp_rx_fd;
	int udp_tx_fd;
	time_t last_stats = 0;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(stderr);
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	serial_fd = open_serial(cfg.serial_dev, cfg.baud);
	if (serial_fd < 0)
		return 1;
	udp_rx_fd = udp_rx_socket(cfg.udp_listen_port);
	if (udp_rx_fd < 0) {
		close(serial_fd);
		return 1;
	}
	udp_tx_fd = udp_tx_socket(cfg.udp_dest_host, cfg.udp_dest_port, &udp_dest);
	if (udp_tx_fd < 0) {
		close(udp_rx_fd);
		close(serial_fd);
		return 1;
	}

	fprintf(stderr, "mavlink: serial=%s baud=%d udp-listen=%u udp-dest=%s:%u\n",
		cfg.serial_dev, cfg.baud, cfg.udp_listen_port,
		cfg.udp_dest_host, cfg.udp_dest_port);

	while (!g_stop) {
		struct pollfd pfds[2] = {
			{.fd = serial_fd, .events = POLLIN},
			{.fd = udp_rx_fd, .events = POLLIN},
		};
		int ret = poll(pfds, 2, 1000);
		time_t now = time(NULL);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if (ret > 0 && (pfds[0].revents & POLLIN)) {
			ssize_t n = read(serial_fd, buf, sizeof(buf));
			if (n > 0) {
				if (sendto(udp_tx_fd, buf, (size_t)n, 0,
					   (struct sockaddr *)&udp_dest, sizeof(udp_dest)) < 0)
					perror("sendto serial udp");
				else {
					st.serial_to_udp++;
					st.serial_bytes += (uint64_t)n;
				}
			}
		}

		if (ret > 0 && (pfds[1].revents & POLLIN)) {
			ssize_t n = recv(udp_rx_fd, buf, sizeof(buf), 0);
			if (n > 0) {
				ssize_t written = write(serial_fd, buf, (size_t)n);
				if (written < 0)
					perror("write serial");
				else {
					st.udp_to_serial++;
					st.udp_bytes += (uint64_t)written;
				}
			}
		}

		if (cfg.verbose && now != last_stats) {
			last_stats = now;
			print_stats(&st);
		}
	}

	close(udp_tx_fd);
	close(udp_rx_fd);
	close(serial_fd);
	return 0;
}
