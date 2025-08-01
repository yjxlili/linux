// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Exceet Electronics GmbH
 * Copyright (C) 2018 Bootlin
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */
#include <linux/dmaengine.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/sched/task_stack.h>

#include "internals.h"

#define SPI_MEM_MAX_BUSWIDTH		8

/**
 * spi_controller_dma_map_mem_op_data() - DMA-map the buffer attached to a
 *					  memory operation
 * @ctlr: the SPI controller requesting this dma_map()
 * @op: the memory operation containing the buffer to map
 * @sgt: a pointer to a non-initialized sg_table that will be filled by this
 *	 function
 *
 * Some controllers might want to do DMA on the data buffer embedded in @op.
 * This helper prepares everything for you and provides a ready-to-use
 * sg_table. This function is not intended to be called from spi drivers.
 * Only SPI controller drivers should use it.
 * Note that the caller must ensure the memory region pointed by
 * op->data.buf.{in,out} is DMA-able before calling this function.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int spi_controller_dma_map_mem_op_data(struct spi_controller *ctlr,
				       const struct spi_mem_op *op,
				       struct sg_table *sgt)
{
	struct device *dmadev;

	if (!op->data.nbytes)
		return -EINVAL;

	if (op->data.dir == SPI_MEM_DATA_OUT && ctlr->dma_tx)
		dmadev = ctlr->dma_tx->device->dev;
	else if (op->data.dir == SPI_MEM_DATA_IN && ctlr->dma_rx)
		dmadev = ctlr->dma_rx->device->dev;
	else
		dmadev = ctlr->dev.parent;

	if (!dmadev)
		return -EINVAL;

	return spi_map_buf(ctlr, dmadev, sgt, op->data.buf.in, op->data.nbytes,
			   op->data.dir == SPI_MEM_DATA_IN ?
			   DMA_FROM_DEVICE : DMA_TO_DEVICE);
}
EXPORT_SYMBOL_GPL(spi_controller_dma_map_mem_op_data);

/**
 * spi_controller_dma_unmap_mem_op_data() - DMA-unmap the buffer attached to a
 *					    memory operation
 * @ctlr: the SPI controller requesting this dma_unmap()
 * @op: the memory operation containing the buffer to unmap
 * @sgt: a pointer to an sg_table previously initialized by
 *	 spi_controller_dma_map_mem_op_data()
 *
 * Some controllers might want to do DMA on the data buffer embedded in @op.
 * This helper prepares things so that the CPU can access the
 * op->data.buf.{in,out} buffer again.
 *
 * This function is not intended to be called from SPI drivers. Only SPI
 * controller drivers should use it.
 *
 * This function should be called after the DMA operation has finished and is
 * only valid if the previous spi_controller_dma_map_mem_op_data() call
 * returned 0.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
void spi_controller_dma_unmap_mem_op_data(struct spi_controller *ctlr,
					  const struct spi_mem_op *op,
					  struct sg_table *sgt)
{
	struct device *dmadev;

	if (!op->data.nbytes)
		return;

	if (op->data.dir == SPI_MEM_DATA_OUT && ctlr->dma_tx)
		dmadev = ctlr->dma_tx->device->dev;
	else if (op->data.dir == SPI_MEM_DATA_IN && ctlr->dma_rx)
		dmadev = ctlr->dma_rx->device->dev;
	else
		dmadev = ctlr->dev.parent;

	spi_unmap_buf(ctlr, dmadev, sgt,
		      op->data.dir == SPI_MEM_DATA_IN ?
		      DMA_FROM_DEVICE : DMA_TO_DEVICE);
}
EXPORT_SYMBOL_GPL(spi_controller_dma_unmap_mem_op_data);

static int spi_check_buswidth_req(struct spi_mem *mem, u8 buswidth, bool tx)
{
	u32 mode = mem->spi->mode;

	switch (buswidth) {
	case 1:
		return 0;

	case 2:
		if ((tx &&
		     (mode & (SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL))) ||
		    (!tx &&
		     (mode & (SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL))))
			return 0;

		break;

	case 4:
		if ((tx && (mode & (SPI_TX_QUAD | SPI_TX_OCTAL))) ||
		    (!tx && (mode & (SPI_RX_QUAD | SPI_RX_OCTAL))))
			return 0;

		break;

	case 8:
		if ((tx && (mode & SPI_TX_OCTAL)) ||
		    (!tx && (mode & SPI_RX_OCTAL)))
			return 0;

		break;

	default:
		break;
	}

	return -ENOTSUPP;
}

static bool spi_mem_check_buswidth(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	if (spi_check_buswidth_req(mem, op->cmd.buswidth, true))
		return false;

	if (op->addr.nbytes &&
	    spi_check_buswidth_req(mem, op->addr.buswidth, true))
		return false;

	if (op->dummy.nbytes &&
	    spi_check_buswidth_req(mem, op->dummy.buswidth, true))
		return false;

	if (op->data.dir != SPI_MEM_NO_DATA &&
	    spi_check_buswidth_req(mem, op->data.buswidth,
				   op->data.dir == SPI_MEM_DATA_OUT))
		return false;

	return true;
}

bool spi_mem_default_supports_op(struct spi_mem *mem,
				 const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	bool op_is_dtr =
		op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr;

	if (op_is_dtr) {
		if (!spi_mem_controller_is_capable(ctlr, dtr))
			return false;

		if (op->data.swap16 && !spi_mem_controller_is_capable(ctlr, swap16))
			return false;

		if (op->cmd.nbytes != 2)
			return false;
	} else {
		if (op->cmd.nbytes != 1)
			return false;
	}

	if (op->data.ecc) {
		if (!spi_mem_controller_is_capable(ctlr, ecc))
			return false;
	}

	if (op->max_freq && mem->spi->controller->min_speed_hz &&
	    op->max_freq < mem->spi->controller->min_speed_hz)
		return false;

	if (op->max_freq &&
	    op->max_freq < mem->spi->max_speed_hz) {
		if (!spi_mem_controller_is_capable(ctlr, per_op_freq))
			return false;
	}

	return spi_mem_check_buswidth(mem, op);
}
EXPORT_SYMBOL_GPL(spi_mem_default_supports_op);

static bool spi_mem_buswidth_is_valid(u8 buswidth)
{
	if (hweight8(buswidth) > 1 || buswidth > SPI_MEM_MAX_BUSWIDTH)
		return false;

	return true;
}

static int spi_mem_check_op(const struct spi_mem_op *op)
{
	if (!op->cmd.buswidth || !op->cmd.nbytes)
		return -EINVAL;

	if ((op->addr.nbytes && !op->addr.buswidth) ||
	    (op->dummy.nbytes && !op->dummy.buswidth) ||
	    (op->data.nbytes && !op->data.buswidth))
		return -EINVAL;

	if (!spi_mem_buswidth_is_valid(op->cmd.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->addr.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->dummy.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->data.buswidth))
		return -EINVAL;

	/* Buffers must be DMA-able. */
	if (WARN_ON_ONCE(op->data.dir == SPI_MEM_DATA_IN &&
			 object_is_on_stack(op->data.buf.in)))
		return -EINVAL;

	if (WARN_ON_ONCE(op->data.dir == SPI_MEM_DATA_OUT &&
			 object_is_on_stack(op->data.buf.out)))
		return -EINVAL;

	return 0;
}

static bool spi_mem_internal_supports_op(struct spi_mem *mem,
					 const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;

	if (ctlr->mem_ops && ctlr->mem_ops->supports_op)
		return ctlr->mem_ops->supports_op(mem, op);

	return spi_mem_default_supports_op(mem, op);
}

/**
 * spi_mem_supports_op() - Check if a memory device and the controller it is
 *			   connected to support a specific memory operation
 * @mem: the SPI memory
 * @op: the memory operation to check
 *
 * Some controllers are only supporting Single or Dual IOs, others might only
 * support specific opcodes, or it can even be that the controller and device
 * both support Quad IOs but the hardware prevents you from using it because
 * only 2 IO lines are connected.
 *
 * This function checks whether a specific operation is supported.
 *
 * Return: true if @op is supported, false otherwise.
 */
bool spi_mem_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	if (spi_mem_check_op(op))
		return false;

	return spi_mem_internal_supports_op(mem, op);
}
EXPORT_SYMBOL_GPL(spi_mem_supports_op);

static int spi_mem_access_start(struct spi_mem *mem)
{
	struct spi_controller *ctlr = mem->spi->controller;

	/*
	 * Flush the message queue before executing our SPI memory
	 * operation to prevent preemption of regular SPI transfers.
	 */
	spi_flush_queue(ctlr);

	if (ctlr->auto_runtime_pm) {
		int ret;

		ret = pm_runtime_resume_and_get(ctlr->dev.parent);
		if (ret < 0) {
			dev_err(&ctlr->dev, "Failed to power device: %d\n",
				ret);
			return ret;
		}
	}

	mutex_lock(&ctlr->bus_lock_mutex);
	mutex_lock(&ctlr->io_mutex);

	return 0;
}

static void spi_mem_access_end(struct spi_mem *mem)
{
	struct spi_controller *ctlr = mem->spi->controller;

	mutex_unlock(&ctlr->io_mutex);
	mutex_unlock(&ctlr->bus_lock_mutex);

	if (ctlr->auto_runtime_pm)
		pm_runtime_put(ctlr->dev.parent);
}

static void spi_mem_add_op_stats(struct spi_statistics __percpu *pcpu_stats,
				 const struct spi_mem_op *op, int exec_op_ret)
{
	struct spi_statistics *stats;
	u64 len, l2len;

	get_cpu();
	stats = this_cpu_ptr(pcpu_stats);
	u64_stats_update_begin(&stats->syncp);

	/*
	 * We do not have the concept of messages or transfers. Let's consider
	 * that one operation is equivalent to one message and one transfer.
	 */
	u64_stats_inc(&stats->messages);
	u64_stats_inc(&stats->transfers);

	/* Use the sum of all lengths as bytes count and histogram value. */
	len = op->cmd.nbytes + op->addr.nbytes;
	len += op->dummy.nbytes + op->data.nbytes;
	u64_stats_add(&stats->bytes, len);
	l2len = min(fls(len), SPI_STATISTICS_HISTO_SIZE) - 1;
	u64_stats_inc(&stats->transfer_bytes_histo[l2len]);

	/* Only account for data bytes as transferred bytes. */
	if (op->data.nbytes && op->data.dir == SPI_MEM_DATA_OUT)
		u64_stats_add(&stats->bytes_tx, op->data.nbytes);
	if (op->data.nbytes && op->data.dir == SPI_MEM_DATA_IN)
		u64_stats_add(&stats->bytes_rx, op->data.nbytes);

	/*
	 * A timeout is not an error, following the same behavior as
	 * spi_transfer_one_message().
	 */
	if (exec_op_ret == -ETIMEDOUT)
		u64_stats_inc(&stats->timedout);
	else if (exec_op_ret)
		u64_stats_inc(&stats->errors);

	u64_stats_update_end(&stats->syncp);
	put_cpu();
}

/**
 * spi_mem_exec_op() - Execute a memory operation
 * @mem: the SPI memory
 * @op: the memory operation to execute
 *
 * Executes a memory operation.
 *
 * This function first checks that @op is supported and then tries to execute
 * it.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int spi_mem_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	unsigned int tmpbufsize, xferpos = 0, totalxferlen = 0;
	struct spi_controller *ctlr = mem->spi->controller;
	struct spi_transfer xfers[4] = { };
	struct spi_message msg;
	u8 *tmpbuf;
	int ret;

	/* Make sure the operation frequency is correct before going futher */
	spi_mem_adjust_op_freq(mem, (struct spi_mem_op *)op);

	dev_vdbg(&mem->spi->dev, "[cmd: 0x%02x][%dB addr: %#8llx][%2dB dummy][%4dB data %s] %d%c-%d%c-%d%c-%d%c @ %uHz\n",
		 op->cmd.opcode,
		 op->addr.nbytes, (op->addr.nbytes ? op->addr.val : 0),
		 op->dummy.nbytes,
		 op->data.nbytes, (op->data.nbytes ? (op->data.dir == SPI_MEM_DATA_IN ? " read" : "write") : "     "),
		 op->cmd.buswidth, op->cmd.dtr ? 'D' : 'S',
		 op->addr.buswidth, op->addr.dtr ? 'D' : 'S',
		 op->dummy.buswidth, op->dummy.dtr ? 'D' : 'S',
		 op->data.buswidth, op->data.dtr ? 'D' : 'S',
		 op->max_freq ? op->max_freq : mem->spi->max_speed_hz);

	ret = spi_mem_check_op(op);
	if (ret)
		return ret;

	if (!spi_mem_internal_supports_op(mem, op))
		return -EOPNOTSUPP;

	if (ctlr->mem_ops && ctlr->mem_ops->exec_op && !spi_get_csgpiod(mem->spi, 0)) {
		ret = spi_mem_access_start(mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->exec_op(mem, op);

		spi_mem_access_end(mem);

		/*
		 * Some controllers only optimize specific paths (typically the
		 * read path) and expect the core to use the regular SPI
		 * interface in other cases.
		 */
		if (!ret || (ret != -ENOTSUPP && ret != -EOPNOTSUPP)) {
			spi_mem_add_op_stats(ctlr->pcpu_statistics, op, ret);
			spi_mem_add_op_stats(mem->spi->pcpu_statistics, op, ret);

			return ret;
		}
	}

	tmpbufsize = op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes;

	/*
	 * Allocate a buffer to transmit the CMD, ADDR cycles with kmalloc() so
	 * we're guaranteed that this buffer is DMA-able, as required by the
	 * SPI layer.
	 */
	tmpbuf = kzalloc(tmpbufsize, GFP_KERNEL | GFP_DMA);
	if (!tmpbuf)
		return -ENOMEM;

	spi_message_init(&msg);

	tmpbuf[0] = op->cmd.opcode;
	xfers[xferpos].tx_buf = tmpbuf;
	xfers[xferpos].len = op->cmd.nbytes;
	xfers[xferpos].tx_nbits = op->cmd.buswidth;
	xfers[xferpos].speed_hz = op->max_freq;
	spi_message_add_tail(&xfers[xferpos], &msg);
	xferpos++;
	totalxferlen++;

	if (op->addr.nbytes) {
		int i;

		for (i = 0; i < op->addr.nbytes; i++)
			tmpbuf[i + 1] = op->addr.val >>
					(8 * (op->addr.nbytes - i - 1));

		xfers[xferpos].tx_buf = tmpbuf + 1;
		xfers[xferpos].len = op->addr.nbytes;
		xfers[xferpos].tx_nbits = op->addr.buswidth;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->addr.nbytes;
	}

	if (op->dummy.nbytes) {
		memset(tmpbuf + op->addr.nbytes + 1, 0xff, op->dummy.nbytes);
		xfers[xferpos].tx_buf = tmpbuf + op->addr.nbytes + 1;
		xfers[xferpos].len = op->dummy.nbytes;
		xfers[xferpos].tx_nbits = op->dummy.buswidth;
		xfers[xferpos].dummy_data = 1;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->dummy.nbytes;
	}

	if (op->data.nbytes) {
		if (op->data.dir == SPI_MEM_DATA_IN) {
			xfers[xferpos].rx_buf = op->data.buf.in;
			xfers[xferpos].rx_nbits = op->data.buswidth;
		} else {
			xfers[xferpos].tx_buf = op->data.buf.out;
			xfers[xferpos].tx_nbits = op->data.buswidth;
		}

		xfers[xferpos].len = op->data.nbytes;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->data.nbytes;
	}

	ret = spi_sync(mem->spi, &msg);

	kfree(tmpbuf);

	if (ret)
		return ret;

	if (msg.actual_length != totalxferlen)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL_GPL(spi_mem_exec_op);

/**
 * spi_mem_get_name() - Return the SPI mem device name to be used by the
 *			upper layer if necessary
 * @mem: the SPI memory
 *
 * This function allows SPI mem users to retrieve the SPI mem device name.
 * It is useful if the upper layer needs to expose a custom name for
 * compatibility reasons.
 *
 * Return: a string containing the name of the memory device to be used
 *	   by the SPI mem user
 */
const char *spi_mem_get_name(struct spi_mem *mem)
{
	return mem->name;
}
EXPORT_SYMBOL_GPL(spi_mem_get_name);

/**
 * spi_mem_adjust_op_size() - Adjust the data size of a SPI mem operation to
 *			      match controller limitations
 * @mem: the SPI memory
 * @op: the operation to adjust
 *
 * Some controllers have FIFO limitations and must split a data transfer
 * operation into multiple ones, others require a specific alignment for
 * optimized accesses. This function allows SPI mem drivers to split a single
 * operation into multiple sub-operations when required.
 *
 * Return: a negative error code if the controller can't properly adjust @op,
 *	   0 otherwise. Note that @op->data.nbytes will be updated if @op
 *	   can't be handled in a single step.
 */
int spi_mem_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	size_t len;

	if (ctlr->mem_ops && ctlr->mem_ops->adjust_op_size)
		return ctlr->mem_ops->adjust_op_size(mem, op);

	if (!ctlr->mem_ops || !ctlr->mem_ops->exec_op) {
		len = op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes;

		if (len > spi_max_transfer_size(mem->spi))
			return -EINVAL;

		op->data.nbytes = min3((size_t)op->data.nbytes,
				       spi_max_transfer_size(mem->spi),
				       spi_max_message_size(mem->spi) -
				       len);
		if (!op->data.nbytes)
			return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(spi_mem_adjust_op_size);

/**
 * spi_mem_adjust_op_freq() - Adjust the frequency of a SPI mem operation to
 *			      match controller, PCB and chip limitations
 * @mem: the SPI memory
 * @op: the operation to adjust
 *
 * Some chips have per-op frequency limitations and must adapt the maximum
 * speed. This function allows SPI mem drivers to set @op->max_freq to the
 * maximum supported value.
 */
void spi_mem_adjust_op_freq(struct spi_mem *mem, struct spi_mem_op *op)
{
	if (!op->max_freq || op->max_freq > mem->spi->max_speed_hz)
		op->max_freq = mem->spi->max_speed_hz;
}
EXPORT_SYMBOL_GPL(spi_mem_adjust_op_freq);

/**
 * spi_mem_calc_op_duration() - Derives the theoretical length (in ns) of an
 *			        operation. This helps finding the best variant
 *			        among a list of possible choices.
 * @op: the operation to benchmark
 *
 * Some chips have per-op frequency limitations, PCBs usually have their own
 * limitations as well, and controllers can support dual, quad or even octal
 * modes, sometimes in DTR. All these combinations make it impossible to
 * statically list the best combination for all situations. If we want something
 * accurate, all these combinations should be rated (eg. with a time estimate)
 * and the best pick should be taken based on these calculations.
 *
 * Returns a ns estimate for the time this op would take, except if no
 * frequency limit has been set, in this case we return the number of
 * cycles nevertheless to allow callers to distinguish which operation
 * would be the fastest at iso-frequency.
 */
u64 spi_mem_calc_op_duration(struct spi_mem *mem, struct spi_mem_op *op)
{
	u64 ncycles = 0;
	u64 ps_per_cycles, duration;

	spi_mem_adjust_op_freq(mem, op);

	if (op->max_freq) {
		ps_per_cycles = 1000000000000ULL;
		do_div(ps_per_cycles, op->max_freq);
	} else {
		/* In this case, the unit is no longer a time unit */
		ps_per_cycles = 1;
	}

	ncycles += ((op->cmd.nbytes * 8) / op->cmd.buswidth) / (op->cmd.dtr ? 2 : 1);
	ncycles += ((op->addr.nbytes * 8) / op->addr.buswidth) / (op->addr.dtr ? 2 : 1);

	/* Dummy bytes are optional for some SPI flash memory operations */
	if (op->dummy.nbytes)
		ncycles += ((op->dummy.nbytes * 8) / op->dummy.buswidth) / (op->dummy.dtr ? 2 : 1);

	ncycles += ((op->data.nbytes * 8) / op->data.buswidth) / (op->data.dtr ? 2 : 1);

	/* Derive the duration in ps */
	duration = ncycles * ps_per_cycles;
	/* Convert into ns */
	do_div(duration, 1000);

	return duration;
}
EXPORT_SYMBOL_GPL(spi_mem_calc_op_duration);

static ssize_t spi_mem_no_dirmap_read(struct spi_mem_dirmap_desc *desc,
				      u64 offs, size_t len, void *buf)
{
	struct spi_mem_op op = desc->info.op_tmpl;
	int ret;

	op.addr.val = desc->info.offset + offs;
	op.data.buf.in = buf;
	op.data.nbytes = len;
	ret = spi_mem_adjust_op_size(desc->mem, &op);
	if (ret)
		return ret;

	ret = spi_mem_exec_op(desc->mem, &op);
	if (ret)
		return ret;

	return op.data.nbytes;
}

static ssize_t spi_mem_no_dirmap_write(struct spi_mem_dirmap_desc *desc,
				       u64 offs, size_t len, const void *buf)
{
	struct spi_mem_op op = desc->info.op_tmpl;
	int ret;

	op.addr.val = desc->info.offset + offs;
	op.data.buf.out = buf;
	op.data.nbytes = len;
	ret = spi_mem_adjust_op_size(desc->mem, &op);
	if (ret)
		return ret;

	ret = spi_mem_exec_op(desc->mem, &op);
	if (ret)
		return ret;

	return op.data.nbytes;
}

/**
 * spi_mem_dirmap_create() - Create a direct mapping descriptor
 * @mem: SPI mem device this direct mapping should be created for
 * @info: direct mapping information
 *
 * This function is creating a direct mapping descriptor which can then be used
 * to access the memory using spi_mem_dirmap_read() or spi_mem_dirmap_write().
 * If the SPI controller driver does not support direct mapping, this function
 * falls back to an implementation using spi_mem_exec_op(), so that the caller
 * doesn't have to bother implementing a fallback on his own.
 *
 * Return: a valid pointer in case of success, and ERR_PTR() otherwise.
 */
struct spi_mem_dirmap_desc *
spi_mem_dirmap_create(struct spi_mem *mem,
		      const struct spi_mem_dirmap_info *info)
{
	struct spi_controller *ctlr = mem->spi->controller;
	struct spi_mem_dirmap_desc *desc;
	int ret = -ENOTSUPP;

	/* Make sure the number of address cycles is between 1 and 8 bytes. */
	if (!info->op_tmpl.addr.nbytes || info->op_tmpl.addr.nbytes > 8)
		return ERR_PTR(-EINVAL);

	/* data.dir should either be SPI_MEM_DATA_IN or SPI_MEM_DATA_OUT. */
	if (info->op_tmpl.data.dir == SPI_MEM_NO_DATA)
		return ERR_PTR(-EINVAL);

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	desc->mem = mem;
	desc->info = *info;
	if (ctlr->mem_ops && ctlr->mem_ops->dirmap_create)
		ret = ctlr->mem_ops->dirmap_create(desc);

	if (ret) {
		desc->nodirmap = true;
		if (!spi_mem_supports_op(desc->mem, &desc->info.op_tmpl))
			ret = -EOPNOTSUPP;
		else
			ret = 0;
	}

	if (ret) {
		kfree(desc);
		return ERR_PTR(ret);
	}

	return desc;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_create);

/**
 * spi_mem_dirmap_destroy() - Destroy a direct mapping descriptor
 * @desc: the direct mapping descriptor to destroy
 *
 * This function destroys a direct mapping descriptor previously created by
 * spi_mem_dirmap_create().
 */
void spi_mem_dirmap_destroy(struct spi_mem_dirmap_desc *desc)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;

	if (!desc->nodirmap && ctlr->mem_ops && ctlr->mem_ops->dirmap_destroy)
		ctlr->mem_ops->dirmap_destroy(desc);

	kfree(desc);
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_destroy);

static void devm_spi_mem_dirmap_release(struct device *dev, void *res)
{
	struct spi_mem_dirmap_desc *desc = *(struct spi_mem_dirmap_desc **)res;

	spi_mem_dirmap_destroy(desc);
}

/**
 * devm_spi_mem_dirmap_create() - Create a direct mapping descriptor and attach
 *				  it to a device
 * @dev: device the dirmap desc will be attached to
 * @mem: SPI mem device this direct mapping should be created for
 * @info: direct mapping information
 *
 * devm_ variant of the spi_mem_dirmap_create() function. See
 * spi_mem_dirmap_create() for more details.
 *
 * Return: a valid pointer in case of success, and ERR_PTR() otherwise.
 */
struct spi_mem_dirmap_desc *
devm_spi_mem_dirmap_create(struct device *dev, struct spi_mem *mem,
			   const struct spi_mem_dirmap_info *info)
{
	struct spi_mem_dirmap_desc **ptr, *desc;

	ptr = devres_alloc(devm_spi_mem_dirmap_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	desc = spi_mem_dirmap_create(mem, info);
	if (IS_ERR(desc)) {
		devres_free(ptr);
	} else {
		*ptr = desc;
		devres_add(dev, ptr);
	}

	return desc;
}
EXPORT_SYMBOL_GPL(devm_spi_mem_dirmap_create);

static int devm_spi_mem_dirmap_match(struct device *dev, void *res, void *data)
{
	struct spi_mem_dirmap_desc **ptr = res;

	if (WARN_ON(!ptr || !*ptr))
		return 0;

	return *ptr == data;
}

/**
 * devm_spi_mem_dirmap_destroy() - Destroy a direct mapping descriptor attached
 *				   to a device
 * @dev: device the dirmap desc is attached to
 * @desc: the direct mapping descriptor to destroy
 *
 * devm_ variant of the spi_mem_dirmap_destroy() function. See
 * spi_mem_dirmap_destroy() for more details.
 */
void devm_spi_mem_dirmap_destroy(struct device *dev,
				 struct spi_mem_dirmap_desc *desc)
{
	devres_release(dev, devm_spi_mem_dirmap_release,
		       devm_spi_mem_dirmap_match, desc);
}
EXPORT_SYMBOL_GPL(devm_spi_mem_dirmap_destroy);

/**
 * spi_mem_dirmap_read() - Read data through a direct mapping
 * @desc: direct mapping descriptor
 * @offs: offset to start reading from. Note that this is not an absolute
 *	  offset, but the offset within the direct mapping which already has
 *	  its own offset
 * @len: length in bytes
 * @buf: destination buffer. This buffer must be DMA-able
 *
 * This function reads data from a memory device using a direct mapping
 * previously instantiated with spi_mem_dirmap_create().
 *
 * Return: the amount of data read from the memory device or a negative error
 * code. Note that the returned size might be smaller than @len, and the caller
 * is responsible for calling spi_mem_dirmap_read() again when that happens.
 */
ssize_t spi_mem_dirmap_read(struct spi_mem_dirmap_desc *desc,
			    u64 offs, size_t len, void *buf)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;
	ssize_t ret;

	if (desc->info.op_tmpl.data.dir != SPI_MEM_DATA_IN)
		return -EINVAL;

	if (!len)
		return 0;

	if (desc->nodirmap) {
		ret = spi_mem_no_dirmap_read(desc, offs, len, buf);
	} else if (ctlr->mem_ops && ctlr->mem_ops->dirmap_read) {
		ret = spi_mem_access_start(desc->mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->dirmap_read(desc, offs, len, buf);

		spi_mem_access_end(desc->mem);
	} else {
		ret = -ENOTSUPP;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_read);

/**
 * spi_mem_dirmap_write() - Write data through a direct mapping
 * @desc: direct mapping descriptor
 * @offs: offset to start writing from. Note that this is not an absolute
 *	  offset, but the offset within the direct mapping which already has
 *	  its own offset
 * @len: length in bytes
 * @buf: source buffer. This buffer must be DMA-able
 *
 * This function writes data to a memory device using a direct mapping
 * previously instantiated with spi_mem_dirmap_create().
 *
 * Return: the amount of data written to the memory device or a negative error
 * code. Note that the returned size might be smaller than @len, and the caller
 * is responsible for calling spi_mem_dirmap_write() again when that happens.
 */
ssize_t spi_mem_dirmap_write(struct spi_mem_dirmap_desc *desc,
			     u64 offs, size_t len, const void *buf)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;
	ssize_t ret;

	if (desc->info.op_tmpl.data.dir != SPI_MEM_DATA_OUT)
		return -EINVAL;

	if (!len)
		return 0;

	if (desc->nodirmap) {
		ret = spi_mem_no_dirmap_write(desc, offs, len, buf);
	} else if (ctlr->mem_ops && ctlr->mem_ops->dirmap_write) {
		ret = spi_mem_access_start(desc->mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->dirmap_write(desc, offs, len, buf);

		spi_mem_access_end(desc->mem);
	} else {
		ret = -ENOTSUPP;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_write);

static inline struct spi_mem_driver *to_spi_mem_drv(struct device_driver *drv)
{
	return container_of(drv, struct spi_mem_driver, spidrv.driver);
}

static int spi_mem_read_status(struct spi_mem *mem,
			       const struct spi_mem_op *op,
			       u16 *status)
{
	const u8 *bytes = (u8 *)op->data.buf.in;
	int ret;

	ret = spi_mem_exec_op(mem, op);
	if (ret)
		return ret;

	if (op->data.nbytes > 1)
		*status = ((u16)bytes[0] << 8) | bytes[1];
	else
		*status = bytes[0];

	return 0;
}

/**
 * spi_mem_poll_status() - Poll memory device status
 * @mem: SPI memory device
 * @op: the memory operation to execute
 * @mask: status bitmask to ckeck
 * @match: (status & mask) expected value
 * @initial_delay_us: delay in us before starting to poll
 * @polling_delay_us: time to sleep between reads in us
 * @timeout_ms: timeout in milliseconds
 *
 * This function polls a status register and returns when
 * (status & mask) == match or when the timeout has expired.
 *
 * Return: 0 in case of success, -ETIMEDOUT in case of error,
 *         -EOPNOTSUPP if not supported.
 */
int spi_mem_poll_status(struct spi_mem *mem,
			const struct spi_mem_op *op,
			u16 mask, u16 match,
			unsigned long initial_delay_us,
			unsigned long polling_delay_us,
			u16 timeout_ms)
{
	struct spi_controller *ctlr = mem->spi->controller;
	int ret = -EOPNOTSUPP;
	int read_status_ret;
	u16 status;

	if (op->data.nbytes < 1 || op->data.nbytes > 2 ||
	    op->data.dir != SPI_MEM_DATA_IN)
		return -EINVAL;

	if (ctlr->mem_ops && ctlr->mem_ops->poll_status && !spi_get_csgpiod(mem->spi, 0)) {
		ret = spi_mem_access_start(mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->poll_status(mem, op, mask, match,
						 initial_delay_us, polling_delay_us,
						 timeout_ms);

		spi_mem_access_end(mem);
	}

	if (ret == -EOPNOTSUPP) {
		if (!spi_mem_supports_op(mem, op))
			return ret;

		if (initial_delay_us < 10)
			udelay(initial_delay_us);
		else
			usleep_range((initial_delay_us >> 2) + 1,
				     initial_delay_us);

		ret = read_poll_timeout(spi_mem_read_status, read_status_ret,
					(read_status_ret || ((status) & mask) == match),
					polling_delay_us, timeout_ms * 1000, false, mem,
					op, &status);
		if (read_status_ret)
			return read_status_ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_poll_status);

static int spi_mem_probe(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_controller *ctlr = spi->controller;
	struct spi_mem *mem;

	mem = devm_kzalloc(&spi->dev, sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	mem->spi = spi;

	if (ctlr->mem_ops && ctlr->mem_ops->get_name)
		mem->name = ctlr->mem_ops->get_name(mem);
	else
		mem->name = dev_name(&spi->dev);

	if (IS_ERR_OR_NULL(mem->name))
		return PTR_ERR_OR_ZERO(mem->name);

	spi_set_drvdata(spi, mem);

	return memdrv->probe(mem);
}

static void spi_mem_remove(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_mem *mem = spi_get_drvdata(spi);

	if (memdrv->remove)
		memdrv->remove(mem);
}

static void spi_mem_shutdown(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_mem *mem = spi_get_drvdata(spi);

	if (memdrv->shutdown)
		memdrv->shutdown(mem);
}

/**
 * spi_mem_driver_register_with_owner() - Register a SPI memory driver
 * @memdrv: the SPI memory driver to register
 * @owner: the owner of this driver
 *
 * Registers a SPI memory driver.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */

int spi_mem_driver_register_with_owner(struct spi_mem_driver *memdrv,
				       struct module *owner)
{
	memdrv->spidrv.probe = spi_mem_probe;
	memdrv->spidrv.remove = spi_mem_remove;
	memdrv->spidrv.shutdown = spi_mem_shutdown;

	return __spi_register_driver(owner, &memdrv->spidrv);
}
EXPORT_SYMBOL_GPL(spi_mem_driver_register_with_owner);

/**
 * spi_mem_driver_unregister() - Unregister a SPI memory driver
 * @memdrv: the SPI memory driver to unregister
 *
 * Unregisters a SPI memory driver.
 */
void spi_mem_driver_unregister(struct spi_mem_driver *memdrv)
{
	spi_unregister_driver(&memdrv->spidrv);
}
EXPORT_SYMBOL_GPL(spi_mem_driver_unregister);
