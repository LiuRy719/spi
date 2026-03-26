// SPDX-License-Identifier: GPL-2.0
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "t113_io_spi.h"

#define T113_IO_SPI_MAX_MINORS		32
#define T113_IO_SPI_MAX_XFER_WORDS	27
#define T113_IO_SPI_DEFAULT_SPEED_HZ	500000U
#define T113_IO_SPI_DUMMY_WORD		0xffff
#define T113_IO_SPI_DRV_VERSION		"2026-03-26.1"
#define T113_IO_SPI_DEFAULT_WORD_DELAY_US 0U
#define T113_IO_SPI_WRITE_RETRIES	5
#define T113_IO_SPI_VERIFY_POLLS	4
#define T113_IO_SPI_AO_CH_SETTLE_US	1000U

#define T113_IO_SPI_CMD_INIT_START	0xA501
#define T113_IO_SPI_RSP_INIT_ACK	0xA502
#define T113_IO_SPI_CMD_INIT_END	0xA503
#define T113_IO_SPI_RSP_INIT_DONE	0xA50f

#define T113_IO_SPI_CMD_GET_DIN		0xA511
#define T113_IO_SPI_CMD_GET_DOUT	0xA521
#define T113_IO_SPI_CMD_SET_DOUT	0xA523
#define T113_IO_SPI_RSP_SET_DOUT	0xA524
#define T113_IO_SPI_CMD_SET_DOUT_END	0xA525

#define T113_IO_SPI_CMD_GET_AI_ALL	0xA531
#define T113_IO_SPI_CMD_GET_AI_CH	0xA533
#define T113_IO_SPI_RSP_GET_AI_CH	0xA534
#define T113_IO_SPI_CMD_SET_AI_MODE	0xA535
#define T113_IO_SPI_RSP_SET_AI_MODE	0xA536
#define T113_IO_SPI_CMD_SET_AI_MODE_END 0xA537

#define T113_IO_SPI_CMD_GET_AO_ALL	0xA541
#define T113_IO_SPI_CMD_GET_AO_CH	0xA543
#define T113_IO_SPI_RSP_GET_AO_CH	0xA544
#define T113_IO_SPI_CMD_SET_AO_ALL	0xA545
#define T113_IO_SPI_RSP_SET_AO_ALL	0xA546
#define T113_IO_SPI_CMD_SET_AO_ALL_END	0xA547
#define T113_IO_SPI_CMD_SET_AO_CH	0xA549
#define T113_IO_SPI_RSP_SET_AO_CH	0xA548
#define T113_IO_SPI_RSP_SET_AO_CH_ALT	0xA558

#define T113_IO_SPI_CMD_GET_INPUTS	0xA551
#define T113_IO_SPI_RSP_GET_INPUTS_END	0xA552
#define T113_IO_SPI_CMD_GET_OUTPUTS	0xA553
#define T113_IO_SPI_RSP_GET_OUTPUTS_END	0xA554
#define T113_IO_SPI_CMD_GET_SNAPSHOT	0xA555
#define T113_IO_SPI_RSP_GET_SNAPSHOT_END 0xA556

#define T113_IO_SPI_CMD_SET_OUTPUTS	0xA561
#define T113_IO_SPI_RSP_SET_OUTPUTS	0xA562
#define T113_IO_SPI_CMD_SET_OUTPUTS_END	0xA563

#define T113_IO_SPI_CHANNEL_PREFIX	0x5af0

struct t113_io_spi_dev {
	struct spi_device *spi;
	struct mutex lock;
	struct kref refcount;
	struct cdev cdev;
	struct device *cdev_dev;
	dev_t devt;
	int minor;
	u16 ai_mode_init;
	u16 dout_init;
	u32 word_delay_us;
	bool present;
	char name[32];
};

static dev_t t113_io_spi_base_devt;
static struct class *t113_io_spi_class;
static DEFINE_IDA(t113_io_spi_minors);

static int t113_io_spi_get_output_state_locked(
	struct t113_io_spi_dev *tdev,
	struct t113_io_spi_output_state *out);
static int t113_io_spi_set_output_state_locked(
	struct t113_io_spi_dev *tdev,
	const struct t113_io_spi_output_state *out);

static void t113_io_spi_free(struct kref *ref)
{
	struct t113_io_spi_dev *tdev;

	tdev = container_of(ref, struct t113_io_spi_dev, refcount);
	kfree(tdev);
}

static void t113_io_spi_fill_dummy(u16 *buf, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		buf[i] = T113_IO_SPI_DUMMY_WORD;
}

static int t113_io_spi_xfer(struct t113_io_spi_dev *tdev, const u16 *tx_words,
			    u16 *rx_words, size_t count)
{
	struct spi_transfer xfer = { 0 };
	u8 tx_buf[T113_IO_SPI_MAX_XFER_WORDS * 2];
	u8 rx_buf[T113_IO_SPI_MAX_XFER_WORDS * 2];
	size_t i;
	int ret;

	if (!tdev->present)
		return -ENODEV;

	if (!count || count > T113_IO_SPI_MAX_XFER_WORDS)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		tx_buf[i * 2] = tx_words[i] >> 8;
		tx_buf[i * 2 + 1] = tx_words[i] & 0xff;
	}

	memset(rx_buf, 0, count * 2);

	xfer.tx_buf = tx_buf;
	xfer.rx_buf = rx_buf;
	xfer.len = count * 2;
	xfer.bits_per_word = 16;
	xfer.speed_hz = tdev->spi->max_speed_hz;

	ret = spi_sync_transfer(tdev->spi, &xfer, 1);
	if (ret)
		return ret;

	if (!rx_words)
		return 0;

	for (i = 0; i < count; i++)
		rx_words[i] = ((u16)rx_buf[i * 2] << 8) | rx_buf[i * 2 + 1];

	return 0;
}

static int t113_io_spi_xfer_word(struct t113_io_spi_dev *tdev, u16 tx_word,
				 u16 *rx_word)
{
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer(tdev, &tx_word, &rx, 1);
	if (!ret && rx_word)
		*rx_word = rx;

	return ret;
}

static void t113_io_spi_word_delay(struct t113_io_spi_dev *tdev)
{
	if (!tdev->word_delay_us)
		return;

	usleep_range(tdev->word_delay_us, tdev->word_delay_us + 200);
}

static int t113_io_spi_get_single_reply_locked(struct t113_io_spi_dev *tdev,
					       u16 cmd, u16 *value)
{
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, cmd, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_DUMMY_WORD, &rx);
	if (ret)
		return ret;

	*value = rx;
	return 0;
}

static int t113_io_spi_get_ai_all_locked(struct t113_io_spi_dev *tdev,
					 u16 *values)
{
	u16 tx[T113_IO_SPI_AI_CHANNELS];
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_AI_ALL, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	t113_io_spi_fill_dummy(tx, ARRAY_SIZE(tx));
	return t113_io_spi_xfer(tdev, tx, values, ARRAY_SIZE(tx));
}

static int t113_io_spi_get_ai_ch_locked(struct t113_io_spi_dev *tdev,
					u8 channel, u16 *value)
{
	u16 rx = 0;
	int ret;

	if (channel >= T113_IO_SPI_AI_CHANNELS)
		return -EINVAL;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_AI_CH, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CHANNEL_PREFIX | channel,
				    &rx);
	if (ret)
		return ret;

	if (rx != T113_IO_SPI_RSP_GET_AI_CH)
		return -EPROTO;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_DUMMY_WORD, &rx);
	if (ret)
		return ret;

	*value = rx;
	return 0;
}

static int t113_io_spi_set_ai_mode_locked(struct t113_io_spi_dev *tdev,
					  u16 value)
{
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_AI_MODE, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, value, &rx);
	if (ret)
		return ret;

	if (rx != T113_IO_SPI_RSP_SET_AI_MODE)
		return -EPROTO;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_AI_MODE_END, &rx);
	if (ret)
		return ret;

	if (rx != value)
		return -EPROTO;

	return 0;
}

static int t113_io_spi_get_ao_all_locked(struct t113_io_spi_dev *tdev,
					 u16 *values)
{
	u16 tx[T113_IO_SPI_AO_CHANNELS];
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_AO_ALL, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	t113_io_spi_fill_dummy(tx, ARRAY_SIZE(tx));
	return t113_io_spi_xfer(tdev, tx, values, ARRAY_SIZE(tx));
}

static int t113_io_spi_get_ao_ch_locked(struct t113_io_spi_dev *tdev,
				u8 channel, u16 *value)
{
	u16 rx = 0;
	int ret;

	if (channel >= T113_IO_SPI_AO_CHANNELS)
		return -EINVAL;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_AO_CH, &rx);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CHANNEL_PREFIX | channel,
				    &rx);
	if (ret)
		return ret;

	if (rx != T113_IO_SPI_RSP_GET_AO_CH)
		return -EPROTO;

	usleep_range(T113_IO_SPI_AO_CH_SETTLE_US,
		     T113_IO_SPI_AO_CH_SETTLE_US + 200);

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_DUMMY_WORD, &rx);
	if (ret)
		return ret;

	*value = rx;
	return 0;
}

static int t113_io_spi_set_ao_all_locked(struct t113_io_spi_dev *tdev,
					 const u16 *values)
{
	u16 tx[T113_IO_SPI_AO_CHANNELS + 1];
	u16 rx[T113_IO_SPI_AO_CHANNELS + 1];
	size_t i;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_AO_ALL, NULL);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++)
		tx[i] = values[i];
	tx[T113_IO_SPI_AO_CHANNELS] = T113_IO_SPI_CMD_SET_AO_ALL_END;

	ret = t113_io_spi_xfer(tdev, tx, rx, ARRAY_SIZE(tx));
	if (ret)
		return ret;

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++) {
		if (rx[i] != T113_IO_SPI_CMD_INIT_START + i)
			return -EPROTO;
	}

	if (rx[T113_IO_SPI_AO_CHANNELS] != T113_IO_SPI_RSP_SET_AO_ALL)
		return -EPROTO;

	return 0;
}

static int t113_io_spi_set_ao_ch_locked(struct t113_io_spi_dev *tdev,
				u8 channel, u16 value)
{
	u16 rx = 0;
	int ret;

	if (channel >= T113_IO_SPI_AO_CHANNELS)
		return -EINVAL;
	if (value > T113_IO_SPI_AO_VALUE_MASK)
		return -EINVAL;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_AO_CH, &rx);
	if (ret)
		return ret;

	ret = t113_io_spi_xfer_word(tdev, value, &rx);
	if (ret)
		return ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CHANNEL_PREFIX | channel,
				    &rx);
	if (ret)
		return ret;

	if (rx != T113_IO_SPI_RSP_SET_AO_CH &&
	    rx != T113_IO_SPI_RSP_SET_AO_CH_ALT)
		return -EPROTO;

	return 0;
}

static int t113_io_spi_set_dout_locked(struct t113_io_spi_dev *tdev, u16 value)
{
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_DOUT, &rx);
	if (ret)
		return ret;

	ret = t113_io_spi_xfer_word(tdev, value, &rx);
	if (ret)
		return ret;

	if (rx != T113_IO_SPI_RSP_SET_DOUT)
		return -EPROTO;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_DOUT_END, &rx);
	if (ret)
		return ret;

	if (rx != value)
		return -EPROTO;

	return 0;
}

static int t113_io_spi_get_dout_locked(struct t113_io_spi_dev *tdev, u16 *value)
{
	u16 rx = 0;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_DOUT, &rx);
	if (ret)
		return ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_DUMMY_WORD, &rx);
	if (ret)
		return ret;

	*value = rx;
	return 0;
}

static int t113_io_spi_set_dout_verified_locked(struct t113_io_spi_dev *tdev,
						u16 value)
{
	return t113_io_spi_set_dout_locked(tdev, value);
}

static int t113_io_spi_get_input_state_locked(struct t113_io_spi_dev *tdev,
					      struct t113_io_spi_input_state *in)
{
	u16 tx[T113_IO_SPI_AI_CHANNELS + 2];
	u16 rx[T113_IO_SPI_AI_CHANNELS + 2];
	size_t i;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_INPUTS, NULL);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	t113_io_spi_fill_dummy(tx, ARRAY_SIZE(tx));
	ret = t113_io_spi_xfer(tdev, tx, rx, ARRAY_SIZE(tx));
	if (ret)
		return ret;

	if (rx[T113_IO_SPI_AI_CHANNELS + 1] != T113_IO_SPI_RSP_GET_INPUTS_END)
		return -EPROTO;

	for (i = 0; i < T113_IO_SPI_AI_CHANNELS; i++)
		in->ai[i] = rx[i];

	in->din = rx[T113_IO_SPI_AI_CHANNELS];
	return 0;
}

static int t113_io_spi_get_output_state_locked(struct t113_io_spi_dev *tdev,
					       struct t113_io_spi_output_state *out)
{
	u16 tx[T113_IO_SPI_AO_CHANNELS + 2];
	u16 rx[T113_IO_SPI_AO_CHANNELS + 2];
	size_t i;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_OUTPUTS, NULL);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	t113_io_spi_fill_dummy(tx, ARRAY_SIZE(tx));
	ret = t113_io_spi_xfer(tdev, tx, rx, ARRAY_SIZE(tx));
	if (ret)
		return ret;

	if (rx[T113_IO_SPI_AO_CHANNELS + 1] != T113_IO_SPI_RSP_GET_OUTPUTS_END)
		return -EPROTO;

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++)
		out->ao[i] = rx[i];

	out->dout = rx[T113_IO_SPI_AO_CHANNELS];
	return 0;
}

static int t113_io_spi_get_snapshot_locked(struct t113_io_spi_dev *tdev,
					   struct t113_io_spi_snapshot *snap)
{
	u16 tx[T113_IO_SPI_AI_CHANNELS + T113_IO_SPI_AO_CHANNELS + 3];
	u16 rx[T113_IO_SPI_AI_CHANNELS + T113_IO_SPI_AO_CHANNELS + 3];
	size_t i;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_GET_SNAPSHOT, NULL);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	t113_io_spi_fill_dummy(tx, ARRAY_SIZE(tx));
	ret = t113_io_spi_xfer(tdev, tx, rx, ARRAY_SIZE(tx));
	if (ret)
		return ret;

	if (rx[ARRAY_SIZE(rx) - 1] != T113_IO_SPI_RSP_GET_SNAPSHOT_END)
		return -EPROTO;

	for (i = 0; i < T113_IO_SPI_AI_CHANNELS; i++)
		snap->ai[i] = rx[i];

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++)
		snap->ao[i] = rx[T113_IO_SPI_AI_CHANNELS + i];

	snap->din = rx[T113_IO_SPI_AI_CHANNELS + T113_IO_SPI_AO_CHANNELS];
	snap->dout = rx[T113_IO_SPI_AI_CHANNELS + T113_IO_SPI_AO_CHANNELS + 1];
	return 0;
}

static int t113_io_spi_set_output_state_locked(
	struct t113_io_spi_dev *tdev,
	const struct t113_io_spi_output_state *out)
{
	u16 tx[T113_IO_SPI_AO_CHANNELS + 2];
	u16 rx[T113_IO_SPI_AO_CHANNELS + 2];
	size_t i;
	int ret;

	ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_CMD_SET_OUTPUTS, NULL);
	if (ret)
		return ret;

	t113_io_spi_word_delay(tdev);

	for (i = 0; i < T113_IO_SPI_AO_CHANNELS; i++)
		tx[i] = out->ao[i];
	tx[T113_IO_SPI_AO_CHANNELS] = out->dout;
	tx[T113_IO_SPI_AO_CHANNELS + 1] = T113_IO_SPI_CMD_SET_OUTPUTS_END;

	ret = t113_io_spi_xfer(tdev, tx, rx, ARRAY_SIZE(tx));
	if (ret)
		return ret;

	if (rx[T113_IO_SPI_AO_CHANNELS + 1] != T113_IO_SPI_RSP_SET_OUTPUTS)
		return -EPROTO;

	return 0;
}

static int t113_io_spi_hw_init_locked(struct t113_io_spi_dev *tdev)
{
	u16 seq[] = {
		T113_IO_SPI_CMD_INIT_START,
		T113_IO_SPI_DUMMY_WORD,
		tdev->ai_mode_init,
		tdev->dout_init,
		T113_IO_SPI_CMD_INIT_END,
	};
	u16 word = 0;
	bool ack_seen;
	int ret;
	int i;

	ack_seen = false;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = t113_io_spi_xfer_word(tdev, seq[i], &word);
		if (ret) {
			dev_err(&tdev->spi->dev,
				"init step %d tx=0x%04X failed, ret=%d\n",
				i, seq[i], ret);
			return ret;
		}

		dev_info(&tdev->spi->dev, "init step %d tx=0x%04X rx=0x%04X\n",
			 i, seq[i], word);

		if (word == T113_IO_SPI_RSP_INIT_ACK)
			ack_seen = true;
			else if (word == T113_IO_SPI_RSP_INIT_DONE)
				return 0;

		t113_io_spi_word_delay(tdev);
	}

	for (i = 0; i < 8; i++) {
		ret = t113_io_spi_xfer_word(tdev, T113_IO_SPI_DUMMY_WORD, &word);
		if (ret) {
			dev_err(&tdev->spi->dev,
				"init poll %d tx=0x%04X failed, ret=%d\n",
				i, T113_IO_SPI_DUMMY_WORD, ret);
			return ret;
		}

		dev_info(&tdev->spi->dev, "init poll %d tx=0x%04X rx=0x%04X\n",
			 i, T113_IO_SPI_DUMMY_WORD, word);

		if (word == T113_IO_SPI_RSP_INIT_DONE)
			return 0;
		if (word == T113_IO_SPI_RSP_INIT_ACK)
			ack_seen = true;

		t113_io_spi_word_delay(tdev);
	}

	return ack_seen ? -EPROTO : -ENODEV;
}

static int t113_io_spi_open(struct inode *inode, struct file *file)
{
	struct t113_io_spi_dev *tdev;

	tdev = container_of(inode->i_cdev, struct t113_io_spi_dev, cdev);
	if (!kref_get_unless_zero(&tdev->refcount))
		return -ENODEV;

	if (!READ_ONCE(tdev->present)) {
		kref_put(&tdev->refcount, t113_io_spi_free);
		return -ENODEV;
	}

	file->private_data = tdev;
	return nonseekable_open(inode, file);
}

static int t113_io_spi_release_file(struct inode *inode, struct file *file)
{
	struct t113_io_spi_dev *tdev = file->private_data;

	if (tdev)
		kref_put(&tdev->refcount, t113_io_spi_free);

	return 0;
}

static ssize_t t113_io_spi_read(struct file *file, char __user *buf, size_t count,
				loff_t *ppos)
{
	struct t113_io_spi_dev *tdev = file->private_data;
	struct t113_io_spi_snapshot snap;
	int ret;

	if (*ppos != 0)
		return 0;

	if (count < sizeof(snap))
		return -EINVAL;

	mutex_lock(&tdev->lock);
	ret = t113_io_spi_get_snapshot_locked(tdev, &snap);
	mutex_unlock(&tdev->lock);
	if (ret)
		return ret;

	if (copy_to_user(buf, &snap, sizeof(snap)))
		return -EFAULT;

	*ppos = sizeof(snap);
	return sizeof(snap);
}

static ssize_t t113_io_spi_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct t113_io_spi_dev *tdev = file->private_data;
	struct t113_io_spi_output_state out;
	int ret;

	if (count != sizeof(out))
		return -EINVAL;

	if (copy_from_user(&out, buf, sizeof(out)))
		return -EFAULT;

	mutex_lock(&tdev->lock);
	ret = t113_io_spi_set_output_state_locked(tdev, &out);
	mutex_unlock(&tdev->lock);
	if (ret)
		return ret;

	*ppos += sizeof(out);
	return sizeof(out);
}

static long t113_io_spi_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct t113_io_spi_dev *tdev = file->private_data;
	void __user *argp = (void __user *)arg;
	struct t113_io_spi_init_cfg init_cfg;
	struct t113_io_spi_input_state input_state;
	struct t113_io_spi_output_state output_state;
	struct t113_io_spi_snapshot snapshot;
	struct t113_io_spi_words16 ai_words;
	struct t113_io_spi_words8 ao_words;
	struct t113_io_spi_channel_data channel;
	u16 value;
	int ret = 0;

	switch (cmd) {
	case T113_IO_SPI_IOC_GET_INIT_CFG:
		init_cfg.ai_mode = tdev->ai_mode_init;
		init_cfg.dout = tdev->dout_init;
		if (copy_to_user(argp, &init_cfg, sizeof(init_cfg)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_DIN:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_single_reply_locked(tdev,
							  T113_IO_SPI_CMD_GET_DIN,
							  &value);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &value, sizeof(value)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_DOUT:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_dout_locked(tdev, &value);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &value, sizeof(value)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_SET_DOUT:
		if (copy_from_user(&value, argp, sizeof(value)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_set_dout_verified_locked(tdev, value);
		mutex_unlock(&tdev->lock);
		return ret;

	case T113_IO_SPI_IOC_GET_AI_ALL:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_ai_all_locked(tdev, ai_words.words);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &ai_words, sizeof(ai_words)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_AI_CH:
		if (copy_from_user(&channel, argp, sizeof(channel)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_ai_ch_locked(tdev, channel.channel,
						   &channel.value);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &channel, sizeof(channel)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_SET_AI_MODE:
		if (copy_from_user(&value, argp, sizeof(value)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_set_ai_mode_locked(tdev, value);
		mutex_unlock(&tdev->lock);
		return ret;

	case T113_IO_SPI_IOC_GET_AO_ALL:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_ao_all_locked(tdev, ao_words.words);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &ao_words, sizeof(ao_words)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_AO_CH:
		if (copy_from_user(&channel, argp, sizeof(channel)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_ao_ch_locked(tdev, channel.channel,
						   &channel.value);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &channel, sizeof(channel)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_SET_AO_ALL:
		if (copy_from_user(&ao_words, argp, sizeof(ao_words)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_set_ao_all_locked(tdev, ao_words.words);
		mutex_unlock(&tdev->lock);
		return ret;

	case T113_IO_SPI_IOC_SET_AO_CH:
		if (copy_from_user(&channel, argp, sizeof(channel)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_set_ao_ch_locked(tdev, channel.channel,
						   channel.value);
		mutex_unlock(&tdev->lock);
		return ret;

	case T113_IO_SPI_IOC_GET_INPUT_STATE:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_input_state_locked(tdev, &input_state);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &input_state, sizeof(input_state)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_OUTPUT_STATE:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_output_state_locked(tdev, &output_state);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &output_state, sizeof(output_state)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_GET_SNAPSHOT:
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_get_snapshot_locked(tdev, &snapshot);
		mutex_unlock(&tdev->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &snapshot, sizeof(snapshot)))
			return -EFAULT;
		return 0;

	case T113_IO_SPI_IOC_SET_OUTPUT_STATE:
		if (copy_from_user(&output_state, argp, sizeof(output_state)))
			return -EFAULT;
		mutex_lock(&tdev->lock);
		ret = t113_io_spi_set_output_state_locked(tdev, &output_state);
		mutex_unlock(&tdev->lock);
		return ret;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations t113_io_spi_fops = {
	.owner = THIS_MODULE,
	.open = t113_io_spi_open,
	.release = t113_io_spi_release_file,
	.read = t113_io_spi_read,
	.write = t113_io_spi_write,
	.unlocked_ioctl = t113_io_spi_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = t113_io_spi_ioctl,
#endif
	.llseek = no_llseek,
};

static int t113_io_spi_parse_fw(struct t113_io_spi_dev *tdev)
{
	u32 value;

	if (!device_property_read_u32(&tdev->spi->dev, "bingpi,ai-mode-init",
				      &value))
		tdev->ai_mode_init = value;
	else
		tdev->ai_mode_init = T113_IO_SPI_DUMMY_WORD;

	if (!device_property_read_u32(&tdev->spi->dev, "bingpi,dout-init",
				      &value))
		tdev->dout_init = value;
	else
		tdev->dout_init = T113_IO_SPI_DUMMY_WORD;

	if (!device_property_read_u32(&tdev->spi->dev, "bingpi,word-delay-us",
				      &value))
		tdev->word_delay_us = value;
	else
		tdev->word_delay_us = T113_IO_SPI_DEFAULT_WORD_DELAY_US;

	return 0;
}

static int t113_io_spi_register_chrdev(struct t113_io_spi_dev *tdev)
{
	int ret;
	u8 chip_select;

	ret = ida_alloc_max(&t113_io_spi_minors, T113_IO_SPI_MAX_MINORS - 1,
			    GFP_KERNEL);
	if (ret < 0)
		return ret;

	tdev->minor = ret;
	tdev->devt = MKDEV(MAJOR(t113_io_spi_base_devt), tdev->minor);

	cdev_init(&tdev->cdev, &t113_io_spi_fops);
	tdev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&tdev->cdev, tdev->devt, 1);
	if (ret)
		goto err_ida;

	chip_select = spi_get_chipselect(tdev->spi, 0);
	scnprintf(tdev->name, sizeof(tdev->name), "%s%d.%u",
		  T113_IO_SPI_NAME, tdev->spi->controller->bus_num,
		  chip_select);

	tdev->cdev_dev = device_create(t113_io_spi_class, &tdev->spi->dev,
				       tdev->devt, tdev, "%s", tdev->name);
	if (IS_ERR(tdev->cdev_dev)) {
		ret = PTR_ERR(tdev->cdev_dev);
		tdev->cdev_dev = NULL;
		goto err_cdev;
	}

	return 0;

err_cdev:
	cdev_del(&tdev->cdev);
err_ida:
	ida_free(&t113_io_spi_minors, tdev->minor);
	tdev->minor = -1;
	return ret;
}

static void t113_io_spi_unregister_chrdev(struct t113_io_spi_dev *tdev)
{
	if (tdev->cdev_dev)
		device_destroy(t113_io_spi_class, tdev->devt);
	cdev_del(&tdev->cdev);
	if (tdev->minor >= 0)
		ida_free(&t113_io_spi_minors, tdev->minor);
	tdev->minor = -1;
}

static int t113_io_spi_probe(struct spi_device *spi)
{
	struct t113_io_spi_dev *tdev;
	int ret;

	if (!spi_is_bpw_supported(spi, 16))
		return dev_err_probe(&spi->dev, -EINVAL,
				     "controller does not support 16-bit words\n");

	tdev = kzalloc(sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->spi = spi;
	tdev->minor = -1;
	tdev->present = true;
	mutex_init(&tdev->lock);
	kref_init(&tdev->refcount);

	t113_io_spi_parse_fw(tdev);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 16;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = T113_IO_SPI_DEFAULT_SPEED_HZ;

	dev_info(&spi->dev,
		 "probe start, driver=%s mode=%u bpw=%u speed=%u ai_mode=0x%04X dout=0x%04X word_delay=%u\n",
		 T113_IO_SPI_DRV_VERSION, spi->mode, spi->bits_per_word,
		 spi->max_speed_hz, tdev->ai_mode_init, tdev->dout_init,
		 tdev->word_delay_us);

	ret = spi_setup(spi);
	if (ret)
		goto err_put;

	mutex_lock(&tdev->lock);
	ret = t113_io_spi_hw_init_locked(tdev);
	mutex_unlock(&tdev->lock);
		if (ret) {
			dev_err(&spi->dev,
				"device handshake failed, ai_mode=0x%04X dout=0x%04X, ret=%d\n",
				tdev->ai_mode_init, tdev->dout_init, ret);
			goto err_put;
		}

	ret = t113_io_spi_register_chrdev(tdev);
	if (ret)
		goto err_put;

	spi_set_drvdata(spi, tdev);
	dev_info(&spi->dev, "registered /dev/%s\n", tdev->name);
	return 0;

err_put:
	kref_put(&tdev->refcount, t113_io_spi_free);
	return ret;
}

static void t113_io_spi_remove(struct spi_device *spi)
{
	struct t113_io_spi_dev *tdev = spi_get_drvdata(spi);

	if (!tdev)
		return;

	mutex_lock(&tdev->lock);
	tdev->present = false;
	mutex_unlock(&tdev->lock);

	t113_io_spi_unregister_chrdev(tdev);
	spi_set_drvdata(spi, NULL);
	kref_put(&tdev->refcount, t113_io_spi_free);
}

static const struct of_device_id t113_io_spi_of_match[] = {
	{ .compatible = "bingpi,t113-io-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, t113_io_spi_of_match);

static const struct spi_device_id t113_io_spi_ids[] = {
	{ "t113-io-spi", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, t113_io_spi_ids);

static struct spi_driver t113_io_spi_driver = {
	.driver = {
		.name = T113_IO_SPI_NAME,
		.of_match_table = t113_io_spi_of_match,
	},
	.probe = t113_io_spi_probe,
	.remove = t113_io_spi_remove,
	.id_table = t113_io_spi_ids,
};

static int __init t113_io_spi_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&t113_io_spi_base_devt, 0,
				  T113_IO_SPI_MAX_MINORS, T113_IO_SPI_NAME);
	if (ret)
		return ret;

	t113_io_spi_class = class_create(T113_IO_SPI_NAME);
	if (IS_ERR(t113_io_spi_class)) {
		ret = PTR_ERR(t113_io_spi_class);
		t113_io_spi_class = NULL;
		goto err_region;
	}

	ret = spi_register_driver(&t113_io_spi_driver);
	if (ret)
		goto err_class;

	return 0;

err_class:
	class_destroy(t113_io_spi_class);
	t113_io_spi_class = NULL;
err_region:
	unregister_chrdev_region(t113_io_spi_base_devt, T113_IO_SPI_MAX_MINORS);
	return ret;
}

static void __exit t113_io_spi_exit(void)
{
	spi_unregister_driver(&t113_io_spi_driver);
	if (t113_io_spi_class)
		class_destroy(t113_io_spi_class);
	unregister_chrdev_region(t113_io_spi_base_devt, T113_IO_SPI_MAX_MINORS);
	ida_destroy(&t113_io_spi_minors);
}

module_init(t113_io_spi_init);
module_exit(t113_io_spi_exit);

MODULE_AUTHOR("LiuRy");
MODULE_DESCRIPTION("T113 IO SPI protocol driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(T113_IO_SPI_DRV_VERSION);
