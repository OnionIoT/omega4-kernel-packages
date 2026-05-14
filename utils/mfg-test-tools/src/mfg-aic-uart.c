#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define CPMEOF 0x1a

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage:\n"
		"  mfg-aic-uart probe [-d DEV] [-b BAUD] [-t MS] [command]\n"
		"  mfg-aic-uart load [-d DEV] [-b BAUD] [-a ADDR] [-t MS] firmware.bin\n"
		"\n"
		"Defaults: DEV=/dev/ttyS2, BAUD=921600, ADDR=160000, timeout=3000 ms\n");
}

static speed_t baud_to_speed(int baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default:
		fprintf(stderr, "unsupported baud: %d\n", baud);
		exit(1);
	}
}

static int open_uart(const char *dev, int baud)
{
	struct termios tio;
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0)
		die(dev);

	if (tcgetattr(fd, &tio) < 0)
		die("tcgetattr");

	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~CRTSCTS;
	tio.c_cflag &= ~CSTOPB;
	tio.c_cflag &= ~PARENB;
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	cfsetispeed(&tio, baud_to_speed(baud));
	cfsetospeed(&tio, baud_to_speed(baud));

	if (tcsetattr(fd, TCSANOW, &tio) < 0)
		die("tcsetattr");
	tcflush(fd, TCIOFLUSH);

	return fd;
}

static void print_byte(unsigned char c)
{
	if (c == '\r')
		printf("\\r");
	else if (c == '\n')
		printf("\\n\n");
	else if (c >= 32 && c < 127)
		putchar(c);
	else
		printf("\\x%02x", c);
}

static int read_byte_timeout(int fd, unsigned char *c, int timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	int ret;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (ret < 0) {
		if (errno == EINTR)
			return 0;
		die("select");
	}
	if (ret == 0)
		return 0;
	return read(fd, c, 1) == 1;
}

static void drain_print(int fd, int timeout_ms)
{
	unsigned char c;
	int got = 0;

	while (read_byte_timeout(fd, &c, timeout_ms)) {
		print_byte(c);
		got = 1;
		timeout_ms = 50;
	}
	if (got)
		fflush(stdout);
}

static void write_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			die("write");
		}
		p += n;
		len -= (size_t)n;
	}
	tcdrain(fd);
}

static uint16_t crc16_ccitt(const unsigned char *buf, size_t len)
{
	uint16_t crc = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		int bit;
		crc ^= (uint16_t)buf[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

static int wait_receiver(int fd, int timeout_ms)
{
	unsigned char c;

	while (read_byte_timeout(fd, &c, timeout_ms)) {
		print_byte(c);
		fflush(stdout);
		if (c == 'C' || c == NAK)
			return c;
		if (c == CAN)
			return CAN;
		timeout_ms = 1000;
	}
	return 0;
}

static int send_xmodem_1k(int fd, const char *path)
{
	FILE *fp;
	unsigned char data[1024];
	unsigned char pkt[1029];
	int blk = 1;
	size_t n;

	fp = fopen(path, "rb");
	if (!fp)
		die(path);

	for (;;) {
		n = fread(data, 1, sizeof(data), fp);
		if (n == 0) {
			unsigned char c = EOT;
			int tries;

			if (ferror(fp))
				die("fread");
			for (tries = 0; tries < 10; tries++) {
				write_all(fd, &c, 1);
				if (read_byte_timeout(fd, &c, 5000) && c == ACK) {
					fclose(fp);
					printf("\nXMODEM complete\n");
					return 0;
				}
			}
			fclose(fp);
			fprintf(stderr, "no ACK after EOT\n");
			return 1;
		}

		if (n < sizeof(data))
			memset(data + n, CPMEOF, sizeof(data) - n);

		pkt[0] = STX;
		pkt[1] = (unsigned char)blk;
		pkt[2] = (unsigned char)(255 - pkt[1]);
		memcpy(pkt + 3, data, sizeof(data));
		uint16_t crc = crc16_ccitt(data, sizeof(data));
		pkt[1027] = (unsigned char)(crc >> 8);
		pkt[1028] = (unsigned char)(crc & 0xff);

		for (;;) {
			unsigned char c;
			write_all(fd, pkt, sizeof(pkt));
			if (!read_byte_timeout(fd, &c, 10000)) {
				fprintf(stderr, "timeout waiting for ACK on block %d\n", blk);
				continue;
			}
			if (c == ACK)
				break;
			if (c == NAK)
				continue;
			if (c == CAN) {
				fprintf(stderr, "transfer cancelled by receiver\n");
				fclose(fp);
				return 1;
			}
			fprintf(stderr, "unexpected response 0x%02x on block %d\n", c, blk);
		}

		if ((blk % 16) == 0) {
			printf(".");
			fflush(stdout);
		}
		blk = (blk + 1) & 0xff;
	}
}

static int cmd_probe(int argc, char **argv)
{
	const char *dev = "/dev/ttyS2";
	const char *cmd = "\r\n";
	int baud = 921600;
	int timeout_ms = 3000;
	int fd;

	while (argc > 0) {
		if (!strcmp(argv[0], "-d") && argc >= 2) {
			dev = argv[1]; argv += 2; argc -= 2;
		} else if (!strcmp(argv[0], "-b") && argc >= 2) {
			baud = atoi(argv[1]); argv += 2; argc -= 2;
		} else if (!strcmp(argv[0], "-t") && argc >= 2) {
			timeout_ms = atoi(argv[1]); argv += 2; argc -= 2;
		} else {
			cmd = argv[0]; argv++; argc--;
		}
	}

	fd = open_uart(dev, baud);
	write_all(fd, cmd, strlen(cmd));
	if (!strchr(cmd, '\r') && !strchr(cmd, '\n'))
		write_all(fd, "\r\n", 2);
	drain_print(fd, timeout_ms);
	close(fd);
	return 0;
}

static int cmd_load(int argc, char **argv)
{
	const char *dev = "/dev/ttyS2";
	const char *addr = "160000";
	const char *fw = NULL;
	char line[64];
	int baud = 921600;
	int timeout_ms = 3000;
	int fd;

	while (argc > 0) {
		if (!strcmp(argv[0], "-d") && argc >= 2) {
			dev = argv[1]; argv += 2; argc -= 2;
		} else if (!strcmp(argv[0], "-b") && argc >= 2) {
			baud = atoi(argv[1]); argv += 2; argc -= 2;
		} else if (!strcmp(argv[0], "-a") && argc >= 2) {
			addr = argv[1]; argv += 2; argc -= 2;
		} else if (!strcmp(argv[0], "-t") && argc >= 2) {
			timeout_ms = atoi(argv[1]); argv += 2; argc -= 2;
		} else {
			fw = argv[0]; argv++; argc--;
		}
	}
	if (!fw) {
		usage(stderr);
		return 1;
	}

	fd = open_uart(dev, baud);
	snprintf(line, sizeof(line), "x %s\r\n", addr);
	write_all(fd, line, strlen(line));
	if (!wait_receiver(fd, timeout_ms)) {
		fprintf(stderr, "receiver did not enter XMODEM mode\n");
		close(fd);
		return 1;
	}
	if (send_xmodem_1k(fd, fw) != 0) {
		close(fd);
		return 1;
	}
	drain_print(fd, 1000);

	snprintf(line, sizeof(line), "g %s\r\n", addr);
	write_all(fd, line, strlen(line));
	drain_print(fd, 5000);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argc < 2 ? stderr : stdout);
		return argc < 2 ? 1 : 0;
	}

	if (!strcmp(argv[1], "probe"))
		return cmd_probe(argc - 2, argv + 2);
	if (!strcmp(argv[1], "load"))
		return cmd_load(argc - 2, argv + 2);

	usage(stderr);
	return 1;
}
