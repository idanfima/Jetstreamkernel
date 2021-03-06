/*
 * Copyright (C) 2010 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/spi/spi_aic3254.h>

static CODEC_SPI_CMD LOOPBACK_DSP_INIT[] = {
	{'w', 0x00, 0x00},
};

static CODEC_SPI_CMD_PARAM LOOPBACK_DSP_INIT_PARAM = {
	.data = (CODEC_SPI_CMD *) &LOOPBACK_DSP_INIT,
	.len  = ARRAY_SIZE(LOOPBACK_DSP_INIT),
};

static CODEC_SPI_CMD LOOPBACK_Receiver_IMIC[] = {
	{'w', 0x00, 0x00},
	{'w', 0x01, 0x01},
	{'w', 0x00, 0x01},
	{'w', 0x01, 0x08},
	{'w', 0x02, 0x21},
	{'w', 0x00, 0x01},
	{'w', 0x37, 0x08},
	{'w', 0x39, 0x08},
	{'w', 0x3B, 0x3C},
	{'w', 0x3C, 0x3C},
	{'w', 0x00, 0x00},
	{'w', 0x52, 0x00},
	{'w', 0x53, 0x00},
	{'w', 0x54, 0x00},
	{'w', 0x51, 0xC0},
	{'w', 0x00, 0x00},
	{'w', 0x40, 0x00},
	{'w', 0x41, 0x00},
	{'w', 0x42, 0x00},
	{'w', 0x3F, 0xD4},
	{'w', 0x00, 0x01},
	{'w', 0x10, 0x00},
	{'w', 0x11, 0x00},
	{'w', 0x0C, 0x01},
	{'w', 0x0D, 0x02},
	{'w', 0x09, 0x31},
};

static CODEC_SPI_CMD_PARAM LOOPBACK_Receiver_IMIC_PARAM = {
	.data = (CODEC_SPI_CMD *) &LOOPBACK_Receiver_IMIC,
	.len  = ARRAY_SIZE(LOOPBACK_Receiver_IMIC),
};

static CODEC_SPI_CMD LOOPBACK_Speaker_IMIC[] = {
	{'w', 0x00, 0x00},
	{'w', 0x01, 0x01},
	{'w', 0x00, 0x01},
	{'w', 0x01, 0x08},
	{'w', 0x02, 0x21},
	{'w', 0x00, 0x01},
	{'w', 0x34, 0x20},
	{'w', 0x36, 0x80},
	{'w', 0x37, 0x02},
	{'w', 0x39, 0x80},
	{'w', 0x3B, 0x30},
	{'w', 0x3C, 0x30},
	{'w', 0x00, 0x00},
	{'w', 0x52, 0x00},
	{'w', 0x53, 0x00},
	{'w', 0x54, 0x00},
	{'w', 0x51, 0xC0},
	{'w', 0x00, 0x00},
	{'w', 0x40, 0x00},
	{'w', 0x41, 0x00},
	{'w', 0x42, 0x00},
	{'w', 0x3F, 0xD6},
	{'w', 0x00, 0x01},
	{'w', 0x10, 0x00},
	{'w', 0x11, 0x00},
	{'w', 0x0C, 0x02},
	{'w', 0x0D, 0x02},
	{'w', 0x09, 0x33},
};

static CODEC_SPI_CMD_PARAM LOOPBACK_Speaker_IMIC_PARAM = {
	.data = (CODEC_SPI_CMD *) &LOOPBACK_Speaker_IMIC,
	.len  = ARRAY_SIZE(LOOPBACK_Speaker_IMIC),
};

static CODEC_SPI_CMD LOOPBACK_Headset_EMIC[] = {
	{'w', 0x00, 0x00},
	{'w', 0x01, 0x01},
	{'w', 0x00, 0x01},
	{'w', 0x01, 0x08},
	{'w', 0x02, 0x21},
	{'w', 0x00, 0x01},
	{'w', 0x34, 0x02},
	{'w', 0x36, 0x80},
	{'w', 0x37, 0x80},
	{'w', 0x39, 0x80},
	{'w', 0x3B, 0x3C},
	{'w', 0x3C, 0x3C},
	{'w', 0x00, 0x00},
	{'w', 0x52, 0x00},
	{'w', 0x53, 0x00},
	{'w', 0x54, 0x00},
	{'w', 0x51, 0xC0},
	{'w', 0x00, 0x00},
	{'w', 0x40, 0x00},
	{'w', 0x41, 0x00},
	{'w', 0x42, 0x00},
	{'w', 0x3F, 0xD6},
	{'w', 0x00, 0x01},
	{'w', 0x10, 0x00},
	{'w', 0x11, 0x00},
	{'w', 0x0C, 0x02},
	{'w', 0x0D, 0x02},
	{'w', 0x09, 0x33},
};

static CODEC_SPI_CMD_PARAM LOOPBACK_Headset_EMIC_PARAM = {
	.data = (CODEC_SPI_CMD *) &LOOPBACK_Headset_EMIC,
	.len  = ARRAY_SIZE(LOOPBACK_Headset_EMIC),
};

static CODEC_SPI_CMD LOOPBACK_Speaker_BMIC[] = {
    {'w', 0x00, 0x00},
	{'w', 0x01, 0x01},
	{'w', 0x00, 0x01},
	{'w', 0x01, 0x08},
	{'w', 0x02, 0x21},
	{'w', 0x00, 0x01},
	{'w', 0x34, 0x02},
	{'w', 0x36, 0x80},
	{'w', 0x37, 0x80},
	{'w', 0x39, 0x80},
	{'w', 0x3B, 0x3C},
	{'w', 0x3C, 0x3C},
	{'w', 0x00, 0x00},
	{'w', 0x52, 0x00},
	{'w', 0x53, 0x00},
	{'w', 0x54, 0x00},
	{'w', 0x51, 0xC0},
	{'w', 0x00, 0x00},
	{'w', 0x40, 0x00},
	{'w', 0x41, 0x00},
	{'w', 0x42, 0x00},
	{'w', 0x3F, 0xD6},
	{'w', 0x00, 0x01},
	{'w', 0x10, 0x00},
	{'w', 0x11, 0x00},
	{'w', 0x0C, 0x02},
	{'w', 0x0D, 0x02},
	{'w', 0x09, 0x33},
};

static CODEC_SPI_CMD_PARAM LOOPBACK_Speaker_BMIC_PARAM = {
	.data = (CODEC_SPI_CMD *) &LOOPBACK_Speaker_BMIC,
	.len  = ARRAY_SIZE(LOOPBACK_Speaker_BMIC),
};
