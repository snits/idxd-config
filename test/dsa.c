// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/vfio.h>
#include <accfg/libaccel_config.h>
#include <linux/idxd.h>
#include "dsa.h"

#define DSA_COMPL_RING_SIZE 64

unsigned int ms_timeout = 5000;
static int umwait_support;

static inline void cpuid(unsigned int *eax, unsigned int *ebx,
		unsigned int *ecx, unsigned int *edx)
{
	/* ecx is often an input as well as an output. */
	asm volatile("cpuid"
		: "=a" (*eax),
		"=b" (*ebx),
		"=c" (*ecx),
		"=d" (*edx)
		: "0" (*eax), "2" (*ecx)
		: "memory");
}

struct dsa_context *dsa_init(void)
{
	struct dsa_context *dctx;
	unsigned int unused[2];
	unsigned int leaf, waitpkg;
	int rc;
	struct accfg_ctx *ctx;

	/* detect umwait support */
	leaf = 7;
	waitpkg = 0;
	cpuid(&leaf, unused, &waitpkg, unused+1);
	if (waitpkg & 0x20) {
		printf("umwait supported\n");
		umwait_support = 1;
	}

	dctx = malloc(sizeof(struct dsa_context));
	if (!dctx)
		return NULL;
	memset(dctx, 0, sizeof(struct dsa_context));

	rc = accfg_new(&ctx);
	if (rc < 0) {
		free(dctx);
		return NULL;
	}

	dctx->ctx = ctx;
	return dctx;
}

static void dsa_free_buffers(struct dsa_context *ctx)
{
	free(ctx->comp_ring_buf);
	free(ctx->ring);
}

static void dsa_alloc_buffers(struct dsa_context *ctx)
{
	struct dsa_ring_ent *ring;
	int buf_size;
	int i;

	ctx->head = 0;
	ctx->tail = 0;
	ctx->issued = 0;
	ctx->dmacount = 0;
	ctx->completed = 0;

	if (ctx->dedicated)
		ctx->num_entries = ctx->wq_size;
	else
		ctx->num_entries = DSA_COMPL_RING_SIZE;

	buf_size = ctx->num_entries * sizeof(*ring);

	/* allocate the array to hold the software ring */
	ring = malloc(buf_size);
	if (!ring)
		return;

	memset(ring, 0, buf_size);

	buf_size = (ctx->num_entries + 1) *
		sizeof(struct dsa_completion_record);

	ctx->comp_ring_buf = malloc(buf_size);

	if (!ctx->comp_ring_buf) {
		free(ring);
		return;
	}

	memset((void *)ctx->comp_ring_buf, 0, buf_size);

	ctx->comp_aligned =
		(struct dsa_completion_record *)ctx->comp_ring_buf;

	if ((unsigned long)ctx->comp_aligned & 0x1F)
		ctx->comp_aligned = (struct dsa_completion_record *)
			(((unsigned long)(ctx->comp_aligned + 1)) & ~0x1FUL);

	ctx->ring = ring;

	for (i = 0; i < ctx->num_entries; i++) {
		ring[i].idx = i;
		ring[i].comp = &ctx->comp_aligned[i];
		ring[i].hw.completion_addr = (uint64_t)ring[i].comp;
	}
}

static int dsa_setup_wq(struct dsa_context *ctx, struct accfg_wq *wq)
{
	struct accfg_device *dev;
	int major, minor;
	char path[1024];

	dev = accfg_wq_get_device(wq);
	major = accfg_device_get_cdev_major(dev);
	if (major < 0)
		return -ENODEV;
	minor = accfg_wq_get_cdev_minor(wq);
	if (minor < 0)
		return -ENODEV;

	sprintf(path, "/dev/char/%u:%u", major, minor);
	ctx->fd = open(path, O_RDWR);
	if (ctx->fd < 0) {
		perror("open");
		return -errno;
	}

	ctx->wq_reg = mmap(NULL, 0x1000, PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ctx->fd, 0);
	if (ctx->wq_reg == MAP_FAILED) {
		perror("mmap");
		return -errno;
	}

	return 0;
}

static struct accfg_wq *dsa_get_wq(struct dsa_context *ctx,
		int dev_id, int shared)
{
	struct accfg_device *device;
	struct accfg_wq *wq;
	int rc;

	accfg_device_foreach(ctx->ctx, device) {
		enum accfg_device_state dstate;

		/* Make sure that the device is enabled */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED)
			continue;

		/* Match the device to the id requested */
		if (accfg_device_get_id(device) != dev_id &&
				dev_id != -1)
			continue;

		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;
			enum accfg_wq_mode mode;
			enum accfg_wq_type type;

			/* Get a workqueue that's enabled */
			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED)
				continue;

			/* The wq type should be user */
			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER)
				continue;

			/* Make sure the mode is correct */
			mode = accfg_wq_get_mode(wq);
			if ((mode == ACCFG_WQ_SHARED && !shared)
				|| (mode == ACCFG_WQ_DEDICATED && shared))
				continue;

			rc = dsa_setup_wq(ctx, wq);
			if (rc < 0)
				return NULL;

			return wq;
		}
	}

	return NULL;
}

static uint32_t bsr(uint32_t val)
{
	uint32_t msb;

	msb = (val == 0) ? 0 : 32 - __builtin_clz(val);
	return msb - 1;
}

int dsa_alloc(struct dsa_context *ctx, int shared)
{
	struct accfg_device *dev;

	/* Is wq already allocated? */
	if (ctx->wq_reg)
		return 0;

	ctx->wq = dsa_get_wq(ctx, -1, shared);
	dev = accfg_wq_get_device(ctx->wq);

	ctx->dedicated = !shared;
	ctx->wq_size = accfg_wq_get_size(ctx->wq);
	ctx->wq_idx = accfg_wq_get_id(ctx->wq);

	ctx->opcap = accfg_device_get_op_cap(dev);
	ctx->max_batch_size = accfg_device_get_max_batch_size(dev);
	ctx->max_xfer_size = accfg_device_get_max_transfer_size(dev);
	ctx->max_xfer_bits = bsr(ctx->max_xfer_size);

	printf("alloc wq %d shared %d size %d addr %p batch sz %x xfer sz %x\n",
			ctx->wq_idx, shared, ctx->wq_size, ctx->wq_reg,
			ctx->max_batch_size, ctx->max_xfer_size);

	dsa_alloc_buffers(ctx);

	if (ctx->comp_ring_buf == NULL) {
		if (munmap(ctx->wq_reg, 0x1000))
			printf("munmap failed %d\n", errno);
		close(ctx->fd);
		ctx->wq_reg = NULL;
		return -ENOMEM;
	}

	return 0;
}

int dsa_enqcmd(struct dsa_context *ctx, struct dsa_hw_desc *hw)
{
	int retry_count = 0;
	int ret = 0;

	while (retry_count < 3) {
		if (!enqcmd(hw, ctx->wq_reg))
			break;

		printf("retry\n");
		retry_count++;
	}

	return ret;
}

static inline unsigned long rdtsc(void)
{
	uint32_t  a, d;

	asm volatile("rdtsc" : "=a"(a), "=d"(d));
	return ((uint64_t)d << 32) | (uint64_t)a;
}

static inline void umonitor(volatile void *addr)
{
	asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
}

static inline int umwait(unsigned long timeout, unsigned int state)
{
	uint8_t r;
	uint32_t timeout_low = (uint32_t)timeout;
	uint32_t timeout_high = (uint32_t)(timeout >> 32);

	timeout_low = (uint32_t)timeout;
	timeout_high = (uint32_t)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
		"setc %0\t\n"
		: "=r"(r)
		: "c"(state), "a"(timeout_low), "d"(timeout_high));
	return r;
}

static int dsa_wait_on_desc_timeout(struct dsa_ring_ent *entry,
		unsigned int msec_timeout)
{
	unsigned int j = 0;
	struct dsa_completion_record *comp = entry->comp;

	if (!umwait_support) {
		while (j < msec_timeout && comp->status == 0) {
			usleep(1000);
			j++;
		}
	} else {
		unsigned long timeout = (ms_timeout * 1000000) * 3;
		int r = 1;
		unsigned long t = 0;

		timeout += rdtsc();
		while (comp->status == 0) {
			if (!r) {
				t = rdtsc();
				if (t >= timeout) {
					printf("umwait timeout %lx\n", t);
					break;
				}
			}

			umonitor((uint8_t *)comp);
			if (comp->status != 0)
				break;
			r = umwait(timeout, 0);
		}
		if (t >= timeout)
			j = msec_timeout;
	}

	return (j == msec_timeout) ? -EAGAIN: 0;
}

/* This function can be called out of order */
void dsa_free_desc(struct dsa_context *ctx, struct dsa_ring_ent *desc)
{
	if (desc->idx == ctx->tail) {
		ctx->tail = dsa_add_idx(ctx, ctx->tail, desc->n);
		desc->n = 0;
		desc++;
		while(desc->n == 0 && ctx->tail != ctx->head) {
			ctx->tail = dsa_inc_idx(ctx, ctx->tail);
			desc++;
		}
	} else
		desc->n = 0;
}

struct dsa_ring_ent * dsa_reserve_space(struct dsa_context *ctx, int n)
{
	struct dsa_ring_ent *desc = NULL;

        if (n <= dsa_ring_space(ctx)) {
                desc = dsa_get_ring_ent(ctx, ctx->head);
                ctx->head = dsa_add_idx(ctx, ctx->head, n);
		desc->n = n;
        }

        return desc;
}

void dsa_free(struct dsa_context *ctx)
{
	if (munmap(ctx->wq_reg, 0x1000))
		printf("munmap failed %d\n", errno);

	close(ctx->fd);

	dsa_free_buffers(ctx);
	accfg_unref(ctx->ctx);
	free(ctx);
}


struct dsa_batch *dsa_alloc_batch_buffers(struct dsa_context *ctx,
		int num_descs)
{
	struct dsa_batch *batch;
	int batch_size, cr_size, i;

	batch_size = sizeof(struct dsa_batch) +
		sizeof(struct dsa_hw_desc) * (num_descs + 1);
	/* batch descriptors need to be 64B aligned */
	batch = malloc(batch_size);
	if (!batch)
		return NULL;
	memset(batch, 0, batch_size);

	batch->ctx = ctx;
	batch->num_descs = num_descs;
	batch->descs = batch->descs_unaligned;

	if ((unsigned long)batch->descs & 0x3F)
		batch->descs = (struct dsa_hw_desc *)
			(((unsigned long)(batch->descs + 1)) & ~0x3FUL);

	cr_size = sizeof(struct dsa_completion_record) * (num_descs + 1);
	batch->comp_unaligned = malloc(cr_size);
	if (!batch->comp_unaligned) {
		free(batch);
		return NULL;
	}
	memset(batch->comp_unaligned, 0, cr_size);

	batch->comp = batch->comp_unaligned;

	if ((unsigned long)batch->comp & 0x1F)
		batch->comp = (struct dsa_completion_record *)
			(((unsigned long)(batch->comp + 1)) & ~0x1FUL);

	for (i = 0; i < num_descs; i++)
		batch->descs[i].completion_addr = (uint64_t)&batch->comp[i];

	return batch;
}

void dsa_free_batch_buffers(struct dsa_batch *batch)
{
	free(batch->comp_unaligned);
	free(batch);
}

int dsa_wait_batch(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c, int idx, int num_descs)
{
	int i, j;
	struct dsa_batch *batch = desc->batch;
	struct dsa_hw_desc *hw = batch->descs;
	int n = desc->n, ret = DSA_STATUS_OK;
	struct dsa_completion_record *comp = NULL;
	struct dsa_ring_ent *orig = desc;

	desc->batch = NULL;
	printf("wait %d, %d %d\n", n, idx, num_descs);
	for (i = 0; i < n; i++, desc++) {
		comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);
		if (j < 0) {
			printf("batch desc timeout out\n");
			return DSA_STATUS_TIMEOUT;
		}

		if (comp->status == DSA_COMP_BATCH_FAIL) {
			printf("batch failed %d %d\n",
					i, comp->bytes_completed);
			ret = DSA_STATUS_FAIL;
			break;
		} else if (comp->status == DSA_COMP_BATCH_PAGE_FAULT) {
			printf("Unrecoverable PF on batch\n");
			ret = DSA_STATUS_URPF;
			break;
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("Batch invalid status %d\n", comp->status);
			ret = DSA_STATUS_FAIL;
			break;
		}
	}

	if (!comp)
		return -ENXIO;

	c->status = comp->status;

	comp = &batch->comp[idx];
	for (i = 0; i < num_descs; i++, hw++) {
		if (comp[i].status == DSA_COMP_PAGE_FAULT_NOBOF) {
			printf("%d: batch page fault at addr %lx\n",
					i, comp[i].fault_addr);
			if (hw->flags & IDXD_OP_FLAG_BOF)
				return DSA_STATUS_URPF;
			else
				return DSA_STATUS_RPF;
		} else if (comp[i].status != DSA_COMP_SUCCESS) {
			printf("batch op %d %d failure %x\n",
					i, hw->opcode, comp[i].status);
			return DSA_STATUS_FAIL;
		}
	}

	dsa_free_desc(ctx, orig);
	return ret;
}

int dsa_wait_memcpy(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *orig = desc, *orig1 = desc;
	int i = 0, j, retry;
	int n = desc->n, start;

again:
	retry = 0;
	start = 0;
	for (; i < n; i++, desc++) {
		struct dsa_completion_record *comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);

		if (j < 0) {
			printf("memcpy desc %d timeout out\n", i);
			return DSA_STATUS_TIMEOUT;
		}
		if (comp->status == DSA_COMP_PAGE_FAULT_NOBOF) {
			if (!(desc->hw.flags & IDXD_OP_FLAG_BOF)) {
				dsa_reprep_memcpy(ctx, desc);
				retry = 1;
			} else {
				printf("%d: invalid page fault at addr %lx\n",
					i, comp->fault_addr);
				return DSA_STATUS_URPF;
			}
			if (!start) {
				orig = desc;
				start = i;
			}
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("operation %d failure %x\n", desc->hw.opcode,
						comp->status);
			return DSA_STATUS_FAIL;
		}
	}

	if (retry) {
		desc = orig;
		i = start;
		goto again;
	}

	c->status = DSA_COMP_SUCCESS;

	dsa_free_desc(ctx, orig1);
	return DSA_STATUS_OK;
}

int dsa_batch_memcpy(struct dsa_context *ctx, void *dest, void *src,
		size_t len, unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	int ret = DSA_STATUS_OK;
	struct dsa_batch *batch;
	unsigned int desc_idx, num, i;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return DSA_STATUS_RETRY;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch)
		return DSA_STATUS_FAIL;

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		size_t copy = len;

		desc_idx = i * ctx->max_batch_size;
		num = num_descs;

		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (copy > (num * ctx->max_xfer_size))
			copy = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_memcpy(batch, desc_idx, num,
				(uint64_t)dest, (uint64_t)src, copy, dflags);

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= copy;
		src = (char *)src + copy;
		dest = (char *)dest + copy;
	}

	ret = dsa_wait_batch(ctx, desc, c, 0, batch->num_descs);

	dsa_free_batch_buffers(batch);

	return ret;
}


int dsa_memcpy(struct dsa_context *ctx, void *dest, void *src, size_t len,
		unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs, ret = DSA_STATUS_OK;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return dsa_batch_memcpy(ctx, dest, src, len, flags, c);

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_memcpy(ctx, desc, dest, src, len, dflags);

	ret = dsa_wait_memcpy(ctx, desc, c);

	return ret;
}

struct dsa_ring_ent *dsa_batch_memcpy_nb(struct dsa_context *ctx, void *dest,
		void *src, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, i, num_batch_descs = 1;
	struct dsa_batch *batch;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return NULL;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch) {
		dsa_free_desc(ctx, desc);
		return NULL;
	}

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		int desc_idx = i * ctx->max_batch_size;
		unsigned int num = num_descs;
		size_t  copy = len;

		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (copy > (num * ctx->max_xfer_size))
			copy = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_memcpy(batch, desc_idx, num, (uint64_t)dest,
						(uint64_t)src, copy, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= copy;
		src = (char *)src + copy;
		dest = (char*)dest + copy;
	}
	return desc;
}

struct dsa_ring_ent *dsa_memcpy_nb(struct dsa_context *ctx, void *dest,
		void *src, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return NULL;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_memcpy(ctx, desc, dest, src, len, dflags);

	return desc;
}

int dsa_wait_memset(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *orig = desc, *orig1 = desc;
	int i = 0, j, retry;
	int n = desc->n, start;

again:
	retry = 0;
	start = 0;
	for (; i < n; i++, desc++) {
		struct dsa_completion_record *comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);

		if (j < 0) {
			printf("memset desc %d timeout out\n", i);
			return DSA_STATUS_TIMEOUT;
		}
		if (comp->status == DSA_COMP_PAGE_FAULT_NOBOF) {
			if (!(desc->hw.flags & IDXD_OP_FLAG_BOF)) {
				dsa_reprep_memset(ctx, desc);
				retry = 1;
			} else {
				printf("%d: invalid page fault at addr %lx\n",
					i, comp->fault_addr);
				return DSA_STATUS_URPF;
			}
			if (!start) {
				start = i;
				orig = desc;
			}
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("operation %d failure %x\n", desc->hw.opcode,
						comp->status);
			return DSA_STATUS_FAIL;
		}
	}
	if (retry) {
		i = start;
		desc = orig;
		goto again;
	}

	c->status = DSA_COMP_SUCCESS;

	dsa_free_desc(ctx, orig1);
	return DSA_STATUS_OK;
}

int dsa_batch_memset(struct dsa_context *ctx, void *dest, uint64_t val,
		size_t len, unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	int ret = DSA_STATUS_OK;
	struct dsa_batch *batch;
	unsigned int i, desc_idx, num;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return DSA_STATUS_RETRY;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch)
		return DSA_STATUS_FAIL;

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		size_t fill = len;

		desc_idx = i * ctx->max_batch_size;
		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (fill > (num * ctx->max_xfer_size))
			fill = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_memset(batch, desc_idx, num, (uint64_t)dest,
				val, fill, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= fill;
		dest = (char *)dest + fill;
	}

	ret = dsa_wait_batch(ctx, desc, c, 0, batch->num_descs);

	dsa_free_batch_buffers(batch);

	return ret;
}


int dsa_memset(struct dsa_context *ctx, void *dest, uint64_t val, size_t len,
		unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs, ret = DSA_STATUS_OK;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return dsa_batch_memset(ctx, dest, val, len, flags, c);

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_memset(ctx, desc, dest, val, len, dflags);

	ret = dsa_wait_memset(ctx, desc, c);

	return ret;
}

struct dsa_ring_ent *dsa_batch_memset_nb(struct dsa_context *ctx, void *dest,
		uint64_t val, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, i, num_batch_descs = 1;
	struct dsa_batch *batch;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return NULL;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch) {
		dsa_free_desc(ctx, desc);
		return NULL;
	}

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		int desc_idx = i * ctx->max_batch_size;
		unsigned int num = num_descs;
		unsigned int fill = len;

		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (fill > (num * ctx->max_xfer_size))
			fill = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_memset(batch, desc_idx, num, (uint64_t)dest,
						val, fill, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= fill;
		dest = (char *)dest + fill;
	}

	return desc;
}

struct dsa_ring_ent *dsa_memset_nb(struct dsa_context *ctx, void *dest,
		uint64_t val, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return NULL;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_memset(ctx, desc, dest, val, len, dflags);

	return desc;
}

int dsa_wait_compare(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *orig = desc;
	int i = 0, j;
	int n = desc->n, ret = DSA_STATUS_OK;
	size_t completed = 0;
	struct dsa_completion_record *comp = NULL;

	while (i < n) {
		comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);

		if (j < 0) {
			printf("compare desc %d timeout out\n", i);
			return DSA_STATUS_TIMEOUT;
		}
		if (comp->status == DSA_COMP_SUCCESS) {
			if (comp->result == 0) {
				completed += desc->hw.xfer_size;
			} else {
				completed += comp->bytes_completed;
				break;
			}
			i++;
			desc++;
		} else if (comp->status == DSA_COMP_PAGE_FAULT_NOBOF) {
			if (!(desc->hw.flags & IDXD_OP_FLAG_BOF)) {
				dsa_reprep_compare(ctx, desc);
			} else {
				printf("%d: invalid page fault at addr %lx\n",
					i, comp->fault_addr);
				ret = DSA_STATUS_URPF;
				break;
			}
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("operation %d failure %x\n", desc->hw.opcode,
						comp->status);
			ret = DSA_STATUS_FAIL;
			break;
		}
	}

	if (!comp)
		return -ENXIO;

	c->status = comp->status;
	c->result = comp->result;
	c->bytes_completed = completed;

	dsa_free_desc(ctx, orig);
	return ret;
}

int dsa_batch_compare(struct dsa_context *ctx, void *src1, void *src2,
		size_t len, unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	int ret = DSA_STATUS_OK;
	struct dsa_batch *batch;
	struct dsa_hw_desc *hw;
	struct dsa_completion_record *compl;
	dsa_completion_t batch_c;
	size_t completed = 0;
	unsigned int i, j = 0, num = 0, desc_idx;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch)
		return DSA_STATUS_FAIL;

	for (i = 0; i < num_batch_descs; i++) {
		size_t comp = len;

		desc = dsa_reserve_space(ctx, 1);
		if (!desc)
			return DSA_STATUS_RETRY;

		desc_idx = i * ctx->max_batch_size;
		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (comp > (num * ctx->max_xfer_size))
			comp = num * ctx->max_xfer_size;

		desc->batch = batch;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

		if (ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_compare(batch, desc_idx, num, (uint64_t)src1,
						(uint64_t)src2, comp, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, desc, dflags);

		ret = dsa_wait_batch(ctx, desc, &batch_c, desc_idx, num);
		if (ret != DSA_STATUS_OK)
			goto error;

		compl = &batch->comp[desc_idx];
		hw = &batch->descs[desc_idx];
		for (j = 0; j < num; j++) {
			if (compl[j].status == DSA_COMP_SUCCESS) {
				if (compl[j].result == 0) {
					completed += hw[j].xfer_size;
				} else {
					completed += compl[j].bytes_completed;
					break;
				}
			} else if (compl[j].status ==
					DSA_COMP_PAGE_FAULT_NOBOF) {
				printf("%d: batch page fault at addr %lx\n",
						j, compl[j].fault_addr);
				if (hw[j].flags & IDXD_OP_FLAG_BOF)
					ret = DSA_STATUS_URPF;
				else
					ret = DSA_STATUS_RPF;
				goto error;
			} else if (compl[j].status != DSA_COMP_SUCCESS) {
				printf("batch op %d failure %x\n",
						hw[j].opcode,
						compl[j].status);
				ret = DSA_STATUS_FAIL;
				goto error;
			}
		}

		if (j < num && compl[j].result != 0)
			break;

		num_descs -= num;
		len -= comp;
		src1 = (char *)src1 + comp;
		src2 = (char *)src2 + comp;
	}

	if (j < num) {
		c->status = compl->status;
		c->result = compl->result;
	} else {
		c->status = DSA_COMP_SUCCESS;
		c->result = 0;
	}
	c->bytes_completed = completed;
error:
	dsa_free_batch_buffers(batch);

	return ret;
}


int dsa_compare(struct dsa_context *ctx, void *src1, void *src2, size_t len,
		unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs, ret = DSA_STATUS_OK;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return dsa_batch_compare(ctx, src1, src2, len, flags, c);

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_compare(ctx, desc, src1, src2, len, dflags);
	ret = dsa_wait_compare(ctx, desc, c);

	return ret;
}

struct dsa_ring_ent *dsa_batch_compare_nb(struct dsa_context *ctx, void *src1,
		void *src2, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	struct dsa_batch *batch;
	unsigned int i, num;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return NULL;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch) {
		dsa_free_desc(ctx, desc);
		return NULL;
	}

	desc->batch = batch;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		int desc_idx = i * ctx->max_batch_size;
		unsigned int comp = len;

		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (comp > (num * ctx->max_xfer_size))
			comp = num * ctx->max_xfer_size;

		if (ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_compare(batch, desc_idx, num, (uint64_t)src1,
						(uint64_t)src2, comp, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= comp;
		src1 = (char *)src1 + comp;
		src2 = (char *)src2 + comp;
	}

	return desc;
}

struct dsa_ring_ent *dsa_compare_nb(struct dsa_context *ctx, void *src1,
		void *src2, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return NULL;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_compare(ctx, desc, src1, src2, len, dflags);

	return desc;
}

int dsa_wait_compval(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *orig = desc;
	int i = 0, j;
	int n = desc->n, ret = DSA_STATUS_OK;
	size_t completed = 0;
	struct dsa_completion_record *comp = NULL;

	while (i < n) {
		comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);

		if (j < 0) {
			printf("compval desc %d timeout out\n", i);
			return DSA_STATUS_TIMEOUT;
		}
		if (comp->status == DSA_COMP_SUCCESS) {
			if (comp->result == 0) {
				completed += desc->hw.xfer_size;
			} else {
				completed += comp->bytes_completed;
				break;
			}
			i++;
			desc++;
		} else if (comp->status == DSA_COMP_PAGE_FAULT_NOBOF) {
			if (!(desc->hw.flags & IDXD_OP_FLAG_BOF)) {
				dsa_reprep_compval(ctx, desc);
			} else {
				printf("%d: invalid page fault at addr %lx\n",
					i, comp->fault_addr);
				ret = DSA_STATUS_URPF;
				break;
			}
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("operation %d failure %x\n", desc->hw.opcode,
						comp->status);
			ret = DSA_STATUS_FAIL;
			break;
		}
	}

	if (!comp)
		return -ENXIO;

	c->status = comp->status;
	c->result = comp->result;
	c->bytes_completed = completed;

	dsa_free_desc(ctx, orig);
	return ret;
}

int dsa_batch_compval(struct dsa_context *ctx, uint64_t val, void *src,
		size_t len, unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	int ret = DSA_STATUS_OK;
	struct dsa_batch *batch;
	struct dsa_hw_desc *hw;
	struct dsa_completion_record *compl = NULL;
	dsa_completion_t batch_c;
	size_t completed = 0;
	unsigned int i, j = 0, num = 0, desc_idx = 0;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch)
		return DSA_STATUS_FAIL;

	for (i = 0; i < num_batch_descs; i++) {
		size_t comp = len;

		desc = dsa_reserve_space(ctx, 1);
		if (!desc)
			return DSA_STATUS_RETRY;

		desc_idx = i * ctx->max_batch_size;
		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (comp > (num * ctx->max_xfer_size))
			comp = num * ctx->max_xfer_size;

		desc->batch = batch;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

		if (ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_compval(batch, desc_idx, num, val,
						(uint64_t)src, comp, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, desc, dflags);

		ret = dsa_wait_batch(ctx, desc, &batch_c, desc_idx, num);
		if (ret != DSA_STATUS_OK)
			goto error;

		compl = &batch->comp[desc_idx];
		hw = &batch->descs[desc_idx];
		for (j = 0; j < num; j++) {
			if (compl[j].status == DSA_COMP_SUCCESS) {
				if (compl[j].result == 0) {
					completed += hw[j].xfer_size;
				} else {
					completed += compl[j].bytes_completed;
					break;
				}
			} else if (compl[j].status ==
					DSA_COMP_PAGE_FAULT_NOBOF) {
				printf("%d: batch page fault at addr %lx\n",
						j, compl[j].fault_addr);
				if (hw[j].flags & IDXD_OP_FLAG_BOF)
					ret = DSA_STATUS_URPF;
				else
					ret = DSA_STATUS_RPF;
				goto error;
			} else if (compl[j].status != DSA_COMP_SUCCESS) {
				printf("batch op %d failure %x\n",
						hw[j].opcode,
						compl[j].status);
				ret = DSA_STATUS_FAIL;
				goto error;
			}
		}

		if (j < num && compl[j].status != DSA_COMP_SUCCESS)
			break;

		num_descs -= num;
		len -= comp;
		src = (char *)src + comp;
	}

	if (j < num) {
		if (!compl)
			return -ENXIO;

		c->status = compl->status;
		c->result = compl->result;
	} else {
		c->status = DSA_COMP_SUCCESS;
		c->result = 0;
	}
	c->bytes_completed = completed;
error:
	dsa_free_batch_buffers(batch);

	return ret;
}


int dsa_compval(struct dsa_context *ctx, uint64_t val, void *src, size_t len,
		unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs, ret = DSA_STATUS_OK;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if ((desc = dsa_reserve_space(ctx, num_descs)) == NULL)
		return dsa_batch_compval(ctx, val, src, len, flags, c);

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_compval(ctx, desc, val, src, len, dflags);

	ret = dsa_wait_compval(ctx, desc, c);

	return ret;
}

struct dsa_ring_ent *dsa_batch_compval_nb(struct dsa_context *ctx,
		uint64_t val, void *src, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	struct dsa_batch *batch;
	unsigned int i, num;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return NULL;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch) {
		dsa_free_desc(ctx, desc);
		return NULL;
	}

	desc->batch = batch;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		int desc_idx = i * ctx->max_batch_size;
		unsigned int comp = len;

		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (comp > (num * ctx->max_xfer_size))
			comp = num * ctx->max_xfer_size;

		if (ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_compval(batch, desc_idx, num, val,
						(uint64_t)src, comp, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= comp;
		src = (char *)src + comp;
	}

	return desc;
}

struct dsa_ring_ent *dsa_compval_nb(struct dsa_context *ctx, uint64_t val,
		void *src, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if ((desc = dsa_reserve_space(ctx, num_descs)) == NULL)
		return NULL;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_compval(ctx, desc, val, src, len, dflags);

	return desc;
}

int dsa_wait_dualcast(struct dsa_context *ctx, struct dsa_ring_ent *desc,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *orig = desc, *orig1 = desc;
	int i = 0, j, retry;
	int n = desc->n, start;

again:
	retry = 0;
	start = 0;
	for (; i < n; i++, desc++) {
		struct dsa_completion_record *comp = desc->comp;

		j = dsa_wait_on_desc_timeout(desc, ms_timeout);

		if (j < 0) {
			printf("dualcast desc %d timeout out\n", i);
			return DSA_STATUS_TIMEOUT;
		}
		if (comp->status == DSA_COMP_PAGE_FAULT_NOBOF) {
			if (!(desc->hw.flags & IDXD_OP_FLAG_BOF)) {
				dsa_reprep_dualcast(ctx, desc);
				retry = 1;
			} else {
				printf("%d: invalid page fault at addr %lx\n",
					i, comp->fault_addr);
				return DSA_STATUS_URPF;
			}
			if (!start) {
				start = i;
				orig = desc;
			}
		} else if (comp->status != DSA_COMP_SUCCESS) {
			printf("operation %d failure %x\n", desc->hw.opcode,
						comp->status);
			return DSA_STATUS_FAIL;
		}
	}
	if (retry) {
		i = start;
		desc = orig;
		goto again;
	}

	c->status = DSA_COMP_SUCCESS;

	dsa_free_desc(ctx, orig1);
	return DSA_STATUS_OK;
}

int dsa_batch_dualcast(struct dsa_context *ctx, void *dest1, void *dest2,
		void *src, size_t len, unsigned int flags,
		dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, num_batch_descs = 1;
	int ret = DSA_STATUS_OK;
	unsigned int i, desc_idx = 0, num = 0;
	struct dsa_batch *batch;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}
	if ((desc = dsa_reserve_space(ctx, num_batch_descs)) == NULL)
		return DSA_STATUS_RETRY;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch)
		return DSA_STATUS_FAIL;

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		size_t copy = len;

		desc_idx = i * ctx->max_batch_size;
		num = num_descs;
		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (copy > (num * ctx->max_xfer_size))
			copy = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_dualcast(batch, desc_idx, num, (uint64_t)dest1,
				(uint64_t)dest2, (uint64_t)src, copy, dflags);

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= copy;
		src = (char *)src + copy;
		dest1 = (char *)dest1 + copy;
		dest2 = (char *)dest2 + copy;
	}

	ret = dsa_wait_batch(ctx, desc, c, 0, batch->num_descs);

	dsa_free_batch_buffers(batch);

	return ret;
}


int dsa_dualcast(struct dsa_context *ctx, void *dest1, void *dest2, void *src,
		size_t len, unsigned int flags, dsa_completion_t *c)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs, ret = DSA_STATUS_OK;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return dsa_batch_dualcast(ctx, dest1, dest2, src,
				len, flags, c);

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_dualcast(ctx, desc, dest1, dest2, src, len, dflags);

	ret = dsa_wait_dualcast(ctx, desc, c);

	return ret;
}

struct dsa_ring_ent *dsa_batch_dualcast_nb(struct dsa_context *ctx,
		void *dest1, void *dest2, void *src, size_t len,
		unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	unsigned int num_descs, i, num_batch_descs = 1;
	struct dsa_batch *batch;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	if (num_descs > ctx->max_batch_size) {
		num_batch_descs = num_descs / ctx->max_batch_size;
		num_batch_descs += !!(num_descs % ctx->max_batch_size);
	}

	desc = dsa_reserve_space(ctx, num_batch_descs);
	if (!desc)
		return NULL;

	batch = dsa_alloc_batch_buffers(ctx, num_descs);

	if (!batch) {
		dsa_free_desc(ctx, desc);
		return NULL;
	}

	desc->batch = batch;

	for (i = 0; i < num_batch_descs; i++) {
		int desc_idx = i * ctx->max_batch_size;
		unsigned int num = num_descs;
		size_t  copy = len;

		if (num > ctx->max_batch_size)
			num = ctx->max_batch_size;

		if (copy > (num * ctx->max_xfer_size))
			copy = num * ctx->max_xfer_size;

		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((flags & DSA_FLAGS_BOF) && ctx->bof)
			dflags |= IDXD_OP_FLAG_BOF;

		dsa_prep_batch_dualcast(batch, desc_idx, num, (uint64_t)dest1,
				(uint64_t)dest2, (uint64_t)src, copy, dflags);
		dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		dsa_prep_submit_batch(batch, desc_idx, num, &desc[i], dflags);

		num_descs -= num;
		len -= copy;
		src = (char *)src + copy;
		dest1 = (char *)dest1 + copy;
		dest2 = (char *)dest2 + copy;
	}
	return desc;
}

struct dsa_ring_ent *dsa_dualcast_nb(struct dsa_context *ctx, void *dest1,
		void *dest2, void *src, size_t len, unsigned int flags)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int num_descs;

	num_descs = dsa_xferlen_to_descs(ctx, len);

	desc = dsa_reserve_space(ctx, num_descs);
	if (!desc)
		return NULL;

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	dsa_prep_dualcast(ctx, desc, dest1, dest2, src, len, dflags);

	return desc;
}
