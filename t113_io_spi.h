/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __T113_IO_SPI_H__
#define __T113_IO_SPI_H__

#include <linux/ioctl.h>
#include <linux/types.h>

#define T113_IO_SPI_NAME			"t113-io-spi"
#define T113_IO_SPI_MAGIC			'T'
#define T113_IO_SPI_AI_CHANNELS		16
#define T113_IO_SPI_AO_CHANNELS		8

/* Raw analog sample format from the device protocol. */
#define T113_IO_SPI_AI_ERROR_MASK		0x8000
#define T113_IO_SPI_AI_CURRENT_MODE_MASK	0x4000
#define T113_IO_SPI_AI_VALUE_MASK		0x3fff

struct t113_io_spi_words16 {
	__u16 words[T113_IO_SPI_AI_CHANNELS];
};

struct t113_io_spi_words8 {
	__u16 words[T113_IO_SPI_AO_CHANNELS];
};

struct t113_io_spi_channel_data {
	__u8 channel;
	__u8 reserved;
	__u16 value;
};

struct t113_io_spi_init_cfg {
	__u16 ai_mode;
	__u16 dout;
};

struct t113_io_spi_input_state {
	__u16 ai[T113_IO_SPI_AI_CHANNELS];
	__u16 din;
};

struct t113_io_spi_output_state {
	__u16 ao[T113_IO_SPI_AO_CHANNELS];
	__u16 dout;
};

struct t113_io_spi_snapshot {
	__u16 ai[T113_IO_SPI_AI_CHANNELS];
	__u16 ao[T113_IO_SPI_AO_CHANNELS];
	__u16 din;
	__u16 dout;
};

#define T113_IO_SPI_IOC_GET_INIT_CFG \
	_IOR(T113_IO_SPI_MAGIC, 0x00, struct t113_io_spi_init_cfg)
#define T113_IO_SPI_IOC_GET_DIN \
	_IOR(T113_IO_SPI_MAGIC, 0x01, __u16)
#define T113_IO_SPI_IOC_GET_DOUT \
	_IOR(T113_IO_SPI_MAGIC, 0x02, __u16)
#define T113_IO_SPI_IOC_SET_DOUT \
	_IOW(T113_IO_SPI_MAGIC, 0x03, __u16)
#define T113_IO_SPI_IOC_GET_AI_ALL \
	_IOR(T113_IO_SPI_MAGIC, 0x04, struct t113_io_spi_words16)
#define T113_IO_SPI_IOC_GET_AI_CH \
	_IOWR(T113_IO_SPI_MAGIC, 0x05, struct t113_io_spi_channel_data)
#define T113_IO_SPI_IOC_SET_AI_MODE \
	_IOW(T113_IO_SPI_MAGIC, 0x06, __u16)
#define T113_IO_SPI_IOC_GET_AO_ALL \
	_IOR(T113_IO_SPI_MAGIC, 0x07, struct t113_io_spi_words8)
#define T113_IO_SPI_IOC_GET_AO_CH \
	_IOWR(T113_IO_SPI_MAGIC, 0x08, struct t113_io_spi_channel_data)
#define T113_IO_SPI_IOC_SET_AO_ALL \
	_IOW(T113_IO_SPI_MAGIC, 0x09, struct t113_io_spi_words8)
#define T113_IO_SPI_IOC_SET_AO_CH \
	_IOW(T113_IO_SPI_MAGIC, 0x0a, struct t113_io_spi_channel_data)
#define T113_IO_SPI_IOC_GET_INPUT_STATE \
	_IOR(T113_IO_SPI_MAGIC, 0x0b, struct t113_io_spi_input_state)
#define T113_IO_SPI_IOC_GET_OUTPUT_STATE \
	_IOR(T113_IO_SPI_MAGIC, 0x0c, struct t113_io_spi_output_state)
#define T113_IO_SPI_IOC_GET_SNAPSHOT \
	_IOR(T113_IO_SPI_MAGIC, 0x0d, struct t113_io_spi_snapshot)
#define T113_IO_SPI_IOC_SET_OUTPUT_STATE \
	_IOW(T113_IO_SPI_MAGIC, 0x0e, struct t113_io_spi_output_state)

#endif /* __T113_IO_SPI_H__ */
