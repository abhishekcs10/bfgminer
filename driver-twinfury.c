/*
 * Copyright 2013 Andreas Auer
 * Copyright 2013 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/*
 * Twin Bitfury USB miner with two Bitfury ASIC
 */

#include "config.h"
#include "miner.h"
#include "logging.h"
#include "util.h"

#include "libbitfury.h"
#include "lowlevel.h"
#include "lowl-vcom.h"
#include "deviceapi.h"
#include "sha2.h"

#include "driver-twinfury.h"

#include <stdio.h>
#include <pthread.h>
#include <termios.h>

BFG_REGISTER_DRIVER(twinfury_drv)

//------------------------------------------------------------------------------
static bool twinfury_detect_custom(const char *devpath, struct device_drv *api, struct twinfury_info *info)
{
	int fd = serial_open(devpath, info->baud, 1, true);

	if(fd < 0)
	{
		return false;
	}

	char buf[1024];
	int len;

	serial_read(fd, buf, sizeof(buf));
	if (1 != write(fd, "I", 1))
	{
		applog(LOG_ERR, "%s: Failed writing id request to %s",
		       twinfury_drv.dname, devpath);
		return false;
	}
	len = serial_read(fd, buf, sizeof(buf));
	if(len != 21)
	{
		serial_close(fd);
		return false;
	}

	info->id.version = buf[1];
	memcpy(info->id.product, buf+2, 8);
	bin2hex(info->id.serial, buf+10, 11);
	applog(LOG_DEBUG, "%s: %s: %d, %s %s",
	       twinfury_drv.dname,
	       devpath,
	       info->id.version, info->id.product,
	       info->id.serial);

	char buf_state[sizeof(struct twinfury_state)+1];
	len = 0;
	if (1 != write(fd, "R", 1))
	{
		applog(LOG_ERR, "%s: Failed writing reset request to %s",
		       twinfury_drv.dname, devpath);
		return false;
	}

	while(len == 0)
	{
		len = serial_read(fd, buf, sizeof(buf_state));
		cgsleep_ms(100);
	}
	serial_close(fd);

	if(len != 8)
	{
		applog(LOG_ERR, "%s: %s not responding to reset: %d",
		       twinfury_drv.dname,
		       devpath, len);
		return false;
	}

	if (serial_claim_v(devpath, api))
		return false;

	struct cgpu_info *bigpic;
	bigpic = calloc(1, sizeof(struct cgpu_info));
	bigpic->drv = api;
	bigpic->device_path = strdup(devpath);
	bigpic->device_fd = -1;
	bigpic->threads = 1;
	bigpic->procs = 2;
	add_cgpu(bigpic);

	applog(LOG_INFO, "Found %"PRIpreprv" at %s", bigpic->proc_repr, devpath);

	applog(LOG_DEBUG, "%"PRIpreprv": Init: baud=%d",
		bigpic->proc_repr, info->baud);

	bigpic->device_data = info;

	return true;
}

//------------------------------------------------------------------------------
static bool twinfury_detect_one(const char *devpath)
{
	struct twinfury_info *info = calloc(1, sizeof(struct twinfury_info));
	if (unlikely(!info))
		quit(1, "Failed to malloc bigpicInfo");

	info->baud = BPM_BAUD;

	if (!twinfury_detect_custom(devpath, &twinfury_drv, info))
	{
		free(info);
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------
static int twinfury_detect_auto(void)
{
	return serial_autodetect(twinfury_detect_one, "Twinfury");
}

//------------------------------------------------------------------------------
static void twinfury_detect()
{
	serial_detect_auto(&twinfury_drv, twinfury_detect_one, twinfury_detect_auto);
}

//------------------------------------------------------------------------------
static bool twinfury_init(struct thr_info *thr)
{
	struct cgpu_info * const cgpu = thr->cgpu;
	struct twinfury_info *info = (struct twinfury_info *)cgpu->device_data;
	struct cgpu_info *proc;
	int i=0;

	applog(LOG_DEBUG, "%"PRIpreprv": init", cgpu->proc_repr);

	for(i=1, proc = cgpu->next_proc; proc; proc = proc->next_proc, i++)
	{
		struct twinfury_info *data = calloc(1, sizeof(struct twinfury_info));
		proc->device_data = data;
		data->tx_buffer[0] = 'W';
		data->tx_buffer[1] = i;
	}

	int fd = serial_open(cgpu->device_path, info->baud, 1, true);
	if (unlikely(-1 == fd))
	{
		applog(LOG_ERR, "%"PRIpreprv": Failed to open %s",
				cgpu->proc_repr, cgpu->device_path);
		return false;
	}

	cgpu->device_fd = fd;
	cgpu->dev_serial = info->id.serial;

	applog(LOG_INFO, "%"PRIpreprv": Opened %s", cgpu->proc_repr, cgpu->device_path);

	info->tx_buffer[0] = 'W';
	info->tx_buffer[1] = 0x00;

	timer_set_now(&thr->tv_poll);

	return true;
}

//------------------------------------------------------------------------------
static bool twinfury_process_results(struct cgpu_info * const proc)
{
	struct twinfury_info *device = proc->device_data;
	uint8_t *rx_buffer = device->rx_buffer;
	uint32_t rx_len = device->rx_len;

	struct work *work = proc->thr[0]->work;

	if(rx_len == 0)
	{
		return false;
	}

	if(rx_buffer[3] == 0)
	{
		return false;
	}

	if(!work)
	{
		applog(LOG_ERR, "%"PRIpreprv": Work not available at the moment", proc->proc_repr);
		return true;
	}

	uint32_t m7    = *((uint32_t *)&work->data[64]);
	uint32_t ntime = *((uint32_t *)&work->data[68]);
	uint32_t nbits = *((uint32_t *)&work->data[72]);

	int j=0;
	for(j=0; j<rx_len; j+= 8)
	{
		struct twinfury_state state;
		state.chip = rx_buffer[j + 1];
		state.state = rx_buffer[j + 2];
		state.switched = rx_buffer[j + 3];
		memcpy(&state.nonce, rx_buffer + j + 4, 4);

		uint32_t nonce = bitfury_decnonce(state.nonce);
		if((nonce & 0xFFC00000) != 0xdf800000)
		{
			applog(LOG_DEBUG, "%"PRIpreprv": Len: %lu Cmd: %c Chip: %d State: %c Switched: %d Nonce: %08lx",
					proc->proc_repr,
				   (unsigned long)rx_len, rx_buffer[j], state.chip, state.state, state.switched, (unsigned long)nonce);
			if (bitfury_fudge_nonce(work->midstate, m7, ntime, nbits, &nonce))
				submit_nonce(proc->thr[0], work, nonce);
			else
				inc_hw_errors(proc->thr[0], work, nonce);
		}
	}
	return true;
}

//------------------------------------------------------------------------------
static bool twinfury_send_command(int fd, uint8_t *tx, uint16_t tx_size)
{
	if(tx_size != write(fd, tx, tx_size))
	{
		return false;
	}
	tcflush(fd, TCIOFLUSH);

	return true;
}

//------------------------------------------------------------------------------
static uint16_t twinfury_wait_response(int fd, uint8_t *rx, uint16_t rx_size)
{
	uint16_t rx_len;
	int timeout = 20;

	while(timeout > 0)
	{
		rx_len = serial_read(fd, rx, rx_size);
		if(rx_len > 0)
			break;

		timeout--;
	}

	if(unlikely(timeout == 0))
	{
		return 0;
	}

	return rx_len;
}

//------------------------------------------------------------------------------
int64_t twinfury_job_process_results(struct thr_info *thr, struct work *work, bool stopping)
{
	// Bitfury chips process only 768/1024 of the nonce range
	return 0xbd000000;
}

//------------------------------------------------------------------------------
static
bool twinfury_job_prepare(struct thr_info *thr, struct work *work, __maybe_unused uint64_t max_nonce)
{
	struct cgpu_info *board = thr->cgpu;
	struct twinfury_info *info = (struct twinfury_info *)board->device_data;

	memcpy(&info->tx_buffer[ 2], work->midstate, 32);
	memcpy(&info->tx_buffer[34], &work->data[64], 12);

	work->blk.nonce = 0xffffffff;
	return true;
}

//------------------------------------------------------------------------------
static
void twinfury_poll(struct thr_info *thr)
{
	struct cgpu_info * const dev = thr->cgpu;
	struct cgpu_info *proc;

	uint8_t n_chips = 0;
	uint8_t timeout = 10;
	uint8_t buffer[2] = { 'Q', 0 };

	uint8_t response[8];
	uint8_t i=0;

	if(dev->flash_led)
	{
		char buf[] = "L";

		dev->flash_led = 0;
		if(1 != write(dev->device_fd, buf, 1))
			applog(LOG_ERR, "%"PRIpreprv": Failed writing flash LED", proc->proc_repr);

		if(1 != twinfury_wait_response(dev->device_fd, buf, 1))
			applog(LOG_ERR, "%"PRIpreprv": Waiting for response timed out (Flash LED)", proc->proc_repr);
	}

	for(proc = thr->cgpu; proc; proc = proc->next_proc, n_chips++)
	{
		struct twinfury_info *info = (struct twinfury_info *)proc->device_data;
		buffer[1] = n_chips;

		if(2 != write(dev->device_fd, buffer, 2))
		{
			applog(LOG_ERR, "%"PRIpreprv": Failed writing work task", proc->proc_repr);
			dev_error(dev, REASON_DEV_COMMS_ERROR);
			return;
		}

		timeout = 20;
		while(timeout > 0)
		{
			info->rx_len = serial_read(dev->device_fd, info->rx_buffer, sizeof(info->rx_buffer));
			if(info->rx_len > 0)
				break;

			timeout--;
		}

		if(unlikely(timeout == 0))
		{
			applog(LOG_ERR, "%"PRIpreprv": Query timeout", proc->proc_repr);
		}

		if(twinfury_process_results(proc) == true)
		{
			struct thr_info *proc_thr = proc->thr[0];
			mt_job_transition(proc_thr);
			// TODO: Delay morework until right before it's needed
			timer_set_now(&proc_thr->tv_morework);
			job_start_complete(proc_thr);
		}
	}

	buffer[0] = 'T';
	if(twinfury_send_command(dev->device_fd, buffer, 1))
	{
		if(8 == twinfury_wait_response(dev->device_fd, response, 8))
		{
			if(response[0] == buffer[0])
			{
				uint16_t temp = response[4] | (response[5] << 8);
				char hex[93];
				bin2hex(hex, response, 8);
				applog(LOG_DEBUG, "%"PRIpreprv": TEMP: %s",
					   dev->dev_repr, hex);

				dev->temp = (float)temp / 10.0;
				applog(LOG_DEBUG, "%"PRIpreprv": Temperature: %f", dev->dev_repr, dev->temp);
			}
		}
		else
		{
			applog(LOG_DEBUG, "%"PRIpreprv": No temperature response", dev->dev_repr);
		}
	}

	timer_set_delay_from_now(&thr->tv_poll, 250000);
}

//------------------------------------------------------------------------------
static
void twinfury_job_start(struct thr_info *thr)
{
	struct cgpu_info *board = thr->cgpu;
	struct twinfury_info *info = (struct twinfury_info *)board->device_data;
	int timeout = 50;
	int device_fd = thr->cgpu->device->device_fd;

	if (opt_dev_protocol && opt_debug)
	{
		char hex[93];
		bin2hex(hex, info->tx_buffer, 46);
		applog(LOG_DEBUG, "%"PRIpreprv": SEND: %s",
		       board->proc_repr, hex);
	}

	if (46 != write(device_fd, info->tx_buffer, 46))
	{
		applog(LOG_ERR, "%"PRIpreprv": Failed writing work task", board->proc_repr);
		dev_error(board, REASON_DEV_COMMS_ERROR);
		job_start_abort(thr, true);
		return;
	}

	while(timeout > 0)
	{
		uint8_t buffer[8];
		int len;
		len = serial_read(device_fd, buffer, 8);
		if(len > 0)
			break;

		timeout--;
	}

	if(unlikely(timeout == 0))
	{
		applog(LOG_ERR, "%"PRIpreprv": Timeout.", board->proc_repr);
	}
}

//------------------------------------------------------------------------------
static void twinfury_shutdown(struct thr_info *thr)
{
	struct cgpu_info *cgpu = thr->cgpu;

	serial_close(cgpu->device_fd);
}

//------------------------------------------------------------------------------
static bool twinfury_identify(struct cgpu_info *cgpu)
{
	cgpu->flash_led = 1;

	return true;
}

//------------------------------------------------------------------------------
struct device_drv twinfury_drv = {
	.dname = "Twinfury",
	.name = "TBF",

	.drv_detect = twinfury_detect,

	.identify_device = twinfury_identify,

	.thread_init = twinfury_init,

	.minerloop = minerloop_async,

	.job_prepare = twinfury_job_prepare,
	.job_start = twinfury_job_start,
	.poll = twinfury_poll,
	.job_process_results = twinfury_job_process_results,

	.thread_shutdown = twinfury_shutdown,
};
