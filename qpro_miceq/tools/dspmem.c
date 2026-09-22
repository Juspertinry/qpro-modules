// dspmem: bulk access to CM7120 DSP memory through the driver's debug node.
//   dspmem r <addr> <words> <out.bin>   dump words (little endian) to a file
//   dspmem x <addr> [words]             hex dump to stdout
//   dspmem w <addr> <value>             write one 32-bit word
// The node latches an address on a 1-arg write and returns that word on read.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NODE "/sys/devices/platform/soc/a80000.i2c/i2c-0/0-002d/cm7120dsp"

static int fd;

static uint32_t rd(uint32_t a)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "0x%08x", a);
	lseek(fd, 0, SEEK_SET);
	write(fd, buf, n);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	return strtoul(buf, NULL, 16);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: dspmem r|x|w addr ...\n");
		return 1;
	}
	fd = open(NODE, O_RDWR);
	if (fd < 0) { perror(NODE); return 1; }
	uint32_t a = strtoul(argv[2], NULL, 0) & ~3u;

	if (argv[1][0] == 'w' && argc == 4) {
		char buf[40];
		int n = snprintf(buf, sizeof(buf), "0x%08x 0x%08x", a,
				 (uint32_t)strtoul(argv[3], NULL, 0));
		write(fd, buf, n);
		printf("%08x <- %08x, now %08x\n", a,
		       (uint32_t)strtoul(argv[3], NULL, 0), rd(a));
	} else if (argv[1][0] == 'r' && argc == 5) {
		uint32_t n = strtoul(argv[3], NULL, 0);
		FILE *out = fopen(argv[4], "wb");
		if (!out) { perror(argv[4]); return 1; }
		for (uint32_t i = 0; i < n; i++) {
			uint32_t v = rd(a + 4 * i);
			fwrite(&v, 4, 1, out);
		}
		fclose(out);
	} else if (argv[1][0] == 'x') {
		uint32_t n = argc > 3 ? strtoul(argv[3], NULL, 0) : 16;
		for (uint32_t i = 0; i < n; i++) {
			if (i % 4 == 0) printf("%08x:", a + 4 * i);
			printf(" %08x", rd(a + 4 * i));
			if (i % 4 == 3 || i == n - 1) printf("\n");
		}
	}
	// leave the node pointing at the version word like the driver does
	rd(0x5ffc001c);
	close(fd);
	return 0;
}
