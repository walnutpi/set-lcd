// SPDX-License-Identifier: GPL-2.0+
/*
 * FB driver for the ILI9341 LCD display controller
 *
 * This display uses 9-bit SPI: Data/Command bit + 8 data bits
 * For platforms that doesn't support 9-bit, the driver is capable
 * of emulating this using 8-bit transfer.
 * This is done by transferring eight 9-bit words in 9 bytes.
 *
 * Copyright (C) 2013 Christian Vogelgsang
 * Based on adafruit22fb.c by Noralf Tronnes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <video/mipi_display.h>

// #if VERSION_DIGIT == 5
// #include "framebuffer-5/fbtft.h"
// #elif VERSION_DIGIT == 6
#include "framebuffer-6/fbtft.h"
// #endif

#define DRVNAME "wpi_fb_st7789"
#define WIDTH 480
#define HEIGHT 480

#define TXBUFLEN (8 * PAGE_SIZE)

static int init_display(struct fbtft_par *par)
{
    par->fbtftops.reset(par);
    mdelay(120);

    write_reg(par, 0x11);
    write_reg(par, 0x11);
    mdelay(120);

    write_reg(par, 0x36, 0);

    write_reg(par, 0x3a, 0x05);
    write_reg(par, 0x21);
    write_reg(par, 0x2a, 0x00, 0x00, 0x00, 0xef);
    write_reg(par, 0x2b, 0x00, 0x00, 0x00, 0xef);
    //--------------------------------ST7789V Frame rate setting----------------------------------//
    write_reg(par, 0xb2, 0x0c, 0x0c, 0x00, 0x33, 0x33);
    write_reg(par, 0xb7, 0x35);
    //---------------------------------ST7789V Power setting--------------------------------------//
    write_reg(par, 0xbb, 0x1f);
    write_reg(par, 0xc0, 0x2c);
    write_reg(par, 0xc2, 0x01);
    write_reg(par, 0xc3, 0x12);
    write_reg(par, 0xc4, 0x20);
    write_reg(par, 0xc6, 0x0f);
    write_reg(par, 0xd0, 0xa4, 0xa1);
    //--------------------------------ST7789V gamma setting--------------------------------------//
    write_reg(par, 0xe0, 0xd0, 0x08, 0x11, 0x08, 0x0c, 0x15, 0x39, 0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2d);
    write_reg(par, 0xe1, 0xd0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0b, 0x16, 0x14, 0x2f, 0x31);

    write_reg(par, 0x2A, 0x00, 0x00, 0x00, 0xEF); // 239

    write_reg(par, 0x2B, 0x00, 0x00, 0x00, 0xEF); // 239

    write_reg(par, 0x29); // Display on

    return 0;
}

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
    printk("原始:xs=%d\t ys=%d\t xe=%d\t ye=%d\r\n", xs, ys, xe, ye);
    xs /=2 ;
    ys /=2 ;
    xe /=2 ;
    ye /=2 ;
    switch (par->info->var.rotate)
    {
    case 0:
        break;

    case 180:
        ys += 80;
        ye += 80;
        break;
    case 90:
        break;
    case 270:
        xs += 80;
        xe += 80;
        break;
    }
    printk("修正:xs=%d\t ys=%d\t xe=%d\t ye=%d\n\r\n", xs, ys, xe, ye);

    write_reg(par, 0x2a, (xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);
    write_reg(par, 0x2b, (ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

    write_reg(par, 0x2c);
}

static int set_var(struct fbtft_par *par)
{
    switch (par->info->var.rotate)
    {

    case 0:
        write_reg(par, 0x36, 0);
        break;
    case 180:
        write_reg(par, 0x36, 0xC0);
        break;
    case 90:
        write_reg(par, 0x36, 0x70);
        break;
    case 270:
        write_reg(par, 0x36, 0xA0);
        break;
    }

    return 0;
}

static struct fbtft_display display = {
    .regwidth = 8,
    .width = WIDTH,
    .height = HEIGHT,
    .txbuflen = TXBUFLEN,
    .gamma_num = 2,
    .gamma_len = 14,
    .fbtftops = {
        .init_display = init_display,
        .set_addr_win = set_addr_win,
        .set_var = set_var,
    },
};

FBTFT_REGISTER_DRIVER(DRVNAME, "walnutpi,lcd145_st7789", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:st7789");
MODULE_ALIAS("platform:st7789");

MODULE_DESCRIPTION("FB driver for the st7789 LCD display controller");
MODULE_AUTHOR("lodge");
MODULE_LICENSE("GPL");
