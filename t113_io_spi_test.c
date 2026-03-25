// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "t113_io_spi.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [-D dev] info\n"
		"  %s [-D dev] din\n"
		"  %s [-D dev] dout-get\n"
		"  %s [-D dev] dout-set <value>\n"
		"  %s [-D dev] ao-all\n"
		"  %s [-D dev] ao-ch <channel>\n"
		"  %s [-D dev] ao-set <channel> <value>\n"
		"  %s [-D dev] ai-all\n"
		"  %s [-D dev] ai-ch <channel>\n"
		"  %s [-D dev] snapshot\n"
		"\n"
		"Defaults:\n"
		"  dev = /dev/t113-io-spi1.0\n"
		"\n"
		"Examples:\n"
		"  %s info\n"
		"  %s din\n"
		"  %s dout-set 0x0001\n"
		"  %s ao-all\n"
		"  %s ao-ch 2\n"
		"  %s ao-set 2 0x3FFC\n"
		"  %s ai-ch 3\n"
		"  %s snapshot\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog, prog, prog, prog, prog, prog, prog, prog);
}

static int parse_u16(const char *arg, uint16_t *value)
{
	char *end = NULL;
	unsigned long tmp;

	errno = 0;
	tmp = strtoul(arg, &end, 0);
	if (errno || end == arg || *end != '\0' || tmp > 0xFFFF)
		return -1;

	*value = (uint16_t)tmp;
	return 0;
}

static int parse_u8(const char *arg, uint8_t *value)
{
	char *end = NULL;
	unsigned long tmp;

	errno = 0;
	tmp = strtoul(arg, &end, 0);
	if (errno || end == arg || *end != '\0' || tmp > 0xFF)
		return -1;

	*value = (uint8_t)tmp;
	return 0;
}

static void dump_ai_words(const uint16_t *values)
{
	int i;

	for (i = 0; i < T113_IO_SPI_AI_CHANNELS; i++) {
		printf("AI[%02d] = 0x%04X", i, values[i]);
		if (values[i] & T113_IO_SPI_AI_ERROR_MASK) {
			printf(" error=1\n");
			continue;
		}

		printf(" mode=%s raw=%u\n",
		       (values[i] & T113_IO_SPI_AI_CURRENT_MODE_MASK) ?
		       "current" : "voltage",
		       values[i] & T113_IO_SPI_AI_VALUE_MASK);
	}
}

static void dump_ao_words(const uint16_t *values)
{
	int i;

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++)
		printf("AO[%02d] = 0x%04X\n", i, values[i]);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/t113-io-spi1.0";
	const char *cmd;
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (argc >= 4 && strcmp(argv[1], "-D") == 0) {
		dev = argv[2];
		cmd = argv[3];
		argv += 3;
		argc -= 3;
	} else {
		cmd = argv[1];
		argv += 1;
		argc -= 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	if (strcmp(cmd, "info") == 0) {
		struct t113_io_spi_init_cfg cfg;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_INIT_CFG, &cfg) < 0) {
			perror("T113_IO_SPI_IOC_GET_INIT_CFG");
			close(fd);
			return 1;
		}

		printf("device  : %s\n", dev);
		printf("ai_mode : 0x%04X\n", cfg.ai_mode);
		printf("dout    : 0x%04X\n", cfg.dout);
	} else if (strcmp(cmd, "din") == 0) {
		uint16_t value;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_DIN, &value) < 0) {
			perror("T113_IO_SPI_IOC_GET_DIN");
			close(fd);
			return 1;
		}

		printf("DIN = 0x%04X\n", value);
	} else if (strcmp(cmd, "dout-get") == 0) {
		uint16_t value;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_DOUT, &value) < 0) {
			perror("T113_IO_SPI_IOC_GET_DOUT");
			close(fd);
			return 1;
		}

		printf("DOUT = 0x%04X\n", value);
	} else if (strcmp(cmd, "dout-set") == 0) {
		uint16_t value;

		if (argc != 2 || parse_u16(argv[1], &value) < 0) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		if (ioctl(fd, T113_IO_SPI_IOC_SET_DOUT, &value) < 0) {
			perror("T113_IO_SPI_IOC_SET_DOUT");
			close(fd);
			return 1;
		}

		printf("DOUT set to 0x%04X\n", value);
	} else if (strcmp(cmd, "ao-all") == 0) {
		struct t113_io_spi_words8 ao;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_AO_ALL, &ao) < 0) {
			perror("T113_IO_SPI_IOC_GET_AO_ALL");
			close(fd);
			return 1;
		}

		dump_ao_words(ao.words);
	} else if (strcmp(cmd, "ao-ch") == 0) {
		struct t113_io_spi_channel_data data = { 0 };

		if (argc != 2 || parse_u8(argv[1], &data.channel) < 0 ||
		    data.channel >= T113_IO_SPI_AO_CHANNELS) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		if (ioctl(fd, T113_IO_SPI_IOC_GET_AO_CH, &data) < 0) {
			perror("T113_IO_SPI_IOC_GET_AO_CH");
			close(fd);
			return 1;
		}

		printf("AO[%u] = 0x%04X\n", data.channel, data.value);
	} else if (strcmp(cmd, "ao-set") == 0) {
		struct t113_io_spi_channel_data data = { 0 };

		if (argc != 3 || parse_u8(argv[1], &data.channel) < 0 ||
		    parse_u16(argv[2], &data.value) < 0 ||
		    data.channel >= T113_IO_SPI_AO_CHANNELS ||
		    data.value > T113_IO_SPI_AO_VALUE_MASK) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		if (ioctl(fd, T113_IO_SPI_IOC_SET_AO_CH, &data) < 0) {
			perror("T113_IO_SPI_IOC_SET_AO_CH");
			close(fd);
			return 1;
		}

		printf("AO[%u] set to 0x%04X\n", data.channel, data.value);
	} else if (strcmp(cmd, "ai-all") == 0) {
		struct t113_io_spi_words16 ai;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_AI_ALL, &ai) < 0) {
			perror("T113_IO_SPI_IOC_GET_AI_ALL");
			close(fd);
			return 1;
		}

		dump_ai_words(ai.words);
	} else if (strcmp(cmd, "ai-ch") == 0) {
		struct t113_io_spi_channel_data data = { 0 };

		if (argc != 2 || parse_u8(argv[1], &data.channel) < 0) {
			usage(argv[0]);
			close(fd);
			return 1;
		}

		if (ioctl(fd, T113_IO_SPI_IOC_GET_AI_CH, &data) < 0) {
			perror("T113_IO_SPI_IOC_GET_AI_CH");
			close(fd);
			return 1;
		}

		printf("AI[%u] = 0x%04X", data.channel, data.value);
		if (data.value & T113_IO_SPI_AI_ERROR_MASK)
			printf(" error=1\n");
		else
			printf(" mode=%s raw=%u\n",
			       (data.value & T113_IO_SPI_AI_CURRENT_MODE_MASK) ?
			       "current" : "voltage",
			       data.value & T113_IO_SPI_AI_VALUE_MASK);
	} else if (strcmp(cmd, "snapshot") == 0) {
		struct t113_io_spi_snapshot snap;

		if (ioctl(fd, T113_IO_SPI_IOC_GET_SNAPSHOT, &snap) < 0) {
			perror("T113_IO_SPI_IOC_GET_SNAPSHOT");
			close(fd);
			return 1;
		}

		printf("DIN  = 0x%04X\n", snap.din);
		printf("DOUT = 0x%04X\n", snap.dout);
		dump_ai_words(snap.ai);
		dump_ao_words(snap.ao);
	} else {
		usage(argv[0]);
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
