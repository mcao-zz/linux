// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IBM Power Virtual Ethernet Device Driver
 *
 * Copyright (C) IBM Corporation, 2003, 2010
 *
 * Authors: Dave Larson <larson1@us.ibm.com>
 *	    Santiago Leon <santil@linux.vnet.ibm.com>
 *	    Brian King <brking@linux.vnet.ibm.com>
 *	    Robert Jennings <rcj@linux.vnet.ibm.com>
 *	    Anton Blanchard <anton@au.ibm.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mm.h>
#include <linux/pm.h>
#include <linux/ethtool.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/slab.h>
#include <asm/hvcall.h>
#include <linux/atomic.h>
#include <asm/vio.h>
#include <asm/iommu.h>
#include <asm/firmware.h>
#include <net/tcp.h>
#include <net/ip6_checksum.h>
#include <net/netdev_queues.h>

#include "ibmveth.h"

static irqreturn_t ibmveth_interrupt(int irq, void *dev_instance);
static unsigned long ibmveth_get_desired_dma(struct vio_dev *vdev);

static struct kobj_type ktype_veth_pool;


static const char ibmveth_driver_name[] = "ibmveth";
static const char ibmveth_driver_string[] = "IBM Power Virtual Ethernet Driver";
#define ibmveth_driver_version "1.06"

MODULE_AUTHOR("Santiago Leon <santil@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("IBM Power Virtual Ethernet Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(ibmveth_driver_version);

static unsigned int tx_copybreak __read_mostly = 128;
module_param(tx_copybreak, uint, 0644);
MODULE_PARM_DESC(tx_copybreak,
	"Maximum size of packet that is copied to a new buffer on transmit");

static unsigned int rx_copybreak __read_mostly = 128;
module_param(rx_copybreak, uint, 0644);
MODULE_PARM_DESC(rx_copybreak,
	"Maximum size of packet that is copied to a new buffer on receive");

static unsigned int rx_flush __read_mostly = 0;
module_param(rx_flush, uint, 0644);
MODULE_PARM_DESC(rx_flush, "Flush receive buffers before use");

static bool old_large_send __read_mostly;
module_param(old_large_send, bool, 0444);
MODULE_PARM_DESC(old_large_send,
	"Use old large send method on firmware that supports the new method");

/**
 * enum ibmveth_stat_src - where an ethtool -S counter is stored
 * @IBMVETH_STAT_ADAPTER: plain u64 in struct ibmveth_adapter
 * @IBMVETH_STAT_RX_QSUM: per-queue u64, summed over rx_qstats[]
 * @IBMVETH_STAT_TX_QSUM: per-queue u64, summed over tx_qstats[]
 * @IBMVETH_STAT_RX_NO_BUFFER: rx_qstats[] live-page absolute plus the
 *	absolutes carried over from pages the queue has already retired
 *
 * Counters live per queue so that queues never share a field.
 */
enum ibmveth_stat_src {
	IBMVETH_STAT_ADAPTER,
	IBMVETH_STAT_RX_QSUM,
	IBMVETH_STAT_TX_QSUM,
	IBMVETH_STAT_RX_NO_BUFFER,
};

struct ibmveth_stat {
	char name[ETH_GSTRING_LEN];
	enum ibmveth_stat_src src;
	/* Offset into the struct named by @src. */
	size_t off;
};

#define IBMVETH_STAT_OFF(stat) offsetof(struct ibmveth_adapter, stat)
#define IBMVETH_RXQ_OFF(stat) offsetof(struct ibmveth_rx_queue_stats, stat)
#define IBMVETH_TXQ_OFF(stat) offsetof(struct ibmveth_tx_queue_stats, stat)

/* Single writer per counter; lockless readers use READ_ONCE(). */
#define IBMVETH_STAT_ADD(stat, n)	WRITE_ONCE((stat), (stat) + (n))
#define IBMVETH_STAT_INC(stat)		IBMVETH_STAT_ADD(stat, 1)
#define IBMVETH_GET_STAT(a, off) *((u64 *)(((unsigned long)(a)) + off))

#define IBMVETH_ADAPTER_STAT(key, field) \
	{ key, IBMVETH_STAT_ADAPTER, IBMVETH_STAT_OFF(field) }
#define IBMVETH_RXQ_STAT(key, field) \
	{ key, IBMVETH_STAT_RX_QSUM, IBMVETH_RXQ_OFF(field) }
#define IBMVETH_TXQ_STAT(key, field) \
	{ key, IBMVETH_STAT_TX_QSUM, IBMVETH_TXQ_OFF(field) }

/* Key names and order are user-visible: append only. */
static struct ibmveth_stat ibmveth_stats[] = {
	IBMVETH_RXQ_STAT("replenish_task_cycles", replenish_task_cycles),
	IBMVETH_RXQ_STAT("replenish_no_mem", replenish_no_mem),
	IBMVETH_RXQ_STAT("replenish_add_buff_failure",
			 replenish_add_buff_failure),
	IBMVETH_RXQ_STAT("replenish_add_buff_success",
			 replenish_add_buff_success),
	IBMVETH_RXQ_STAT("rx_invalid_buffer", invalid_buffers),
	{ "rx_no_buffer", IBMVETH_STAT_RX_NO_BUFFER, 0 },
	IBMVETH_ADAPTER_STAT("tx_map_failed", tx_map_failed),
	IBMVETH_TXQ_STAT("tx_send_failed", send_failures),
	IBMVETH_ADAPTER_STAT("fw_enabled_ipv4_csum", fw_ipv4_csum_support),
	IBMVETH_ADAPTER_STAT("fw_enabled_ipv6_csum", fw_ipv6_csum_support),
	IBMVETH_TXQ_STAT("tx_large_packets", large_packets),
	IBMVETH_RXQ_STAT("rx_large_packets", large_packets),
	IBMVETH_ADAPTER_STAT("fw_enabled_large_send", fw_large_send_support),
};

/* simple methods of getting data from the current rxq entry */
static u32 ibmveth_rxq_flags(struct ibmveth_adapter *adapter,
			     int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].flags_off);
}

static int ibmveth_rxq_toggle(struct ibmveth_adapter *adapter,
			      int queue_index)
{
	return (ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_TOGGLE) >>
		IBMVETH_RXQ_TOGGLE_SHIFT;
}

static int ibmveth_rxq_pending_buffer(struct ibmveth_adapter *adapter,
				      int queue_index)
{
	return ibmveth_rxq_toggle(adapter, queue_index) ==
		adapter->rx_queue[queue_index].toggle;
}

static int ibmveth_rxq_buffer_valid(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_VALID;
}

static int ibmveth_rxq_frame_offset(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_OFF_MASK;
}

static int ibmveth_rxq_large_packet(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_LRG_PKT;
}

static int ibmveth_rxq_frame_length(struct ibmveth_adapter *adapter,
				    int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	return be32_to_cpu(rxq->queue_addr[rxq->index].length);
}

static u64 ibmveth_rxq_correlator(struct ibmveth_adapter *adapter,
				  int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];
	unsigned int idx = rxq->index;

	return READ_ONCE(rxq->queue_addr[idx].correlator);
}

static int ibmveth_rxq_csum_good(struct ibmveth_adapter *adapter,
				 int queue_index)
{
	return ibmveth_rxq_flags(adapter, queue_index) & IBMVETH_RXQ_CSUM_GOOD;
}

static unsigned int ibmveth_real_max_tx_queues(void)
{
	unsigned int n_cpu = num_online_cpus();

	return min(n_cpu, IBMVETH_MAX_QUEUES);
}

/**
 * ibmveth_alloc_filter_list - Allocate and map filter list
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int
ibmveth_alloc_filter_list(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;

	adapter->filter_list_addr = (void *)get_zeroed_page(GFP_KERNEL);
	if (!adapter->filter_list_addr) {
		netdev_err(netdev, "unable to allocate filter pages\n");
		return -ENOMEM;
	}

	adapter->filter_list_dma = dma_map_single(dev,
						  adapter->filter_list_addr,
						  4096, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, adapter->filter_list_dma)) {
		netdev_err(netdev, "unable to map filter list pages\n");
		free_page((unsigned long)adapter->filter_list_addr);
		adapter->filter_list_addr = NULL;
		/* Do not leave DMA_MAPPING_ERROR in the adapter. */
		adapter->filter_list_dma = 0;
		return -ENOMEM;
	}

	netdev_dbg(netdev, "filter list @ 0x%p (DMA: 0x%llx)\n",
		   adapter->filter_list_addr,
		   (unsigned long long)adapter->filter_list_dma);

	return 0;
}

/**
 * ibmveth_free_filter_list - Free filter list resources
 * @adapter: ibmveth adapter structure
 */
static void
ibmveth_free_filter_list(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	/* Unmap by CPU pointer: SPAPR can return DMA address 0. */
	if (adapter->filter_list_addr) {
		dma_unmap_single(dev, adapter->filter_list_dma, 4096,
				 DMA_BIDIRECTIONAL);
		adapter->filter_list_dma = 0;
		free_page((unsigned long)adapter->filter_list_addr);
		adapter->filter_list_addr = NULL;
	}
}

/**
 * ibmveth_alloc_rx_qstats - Allocate per-queue RX statistics
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_rx_qstats(struct ibmveth_adapter *adapter)
{
	adapter->rx_qstats = kcalloc(IBMVETH_MAX_RX_QUEUES,
				     sizeof(*adapter->rx_qstats),
				     GFP_KERNEL);
	if (!adapter->rx_qstats)
		return -ENOMEM;

	return 0;
}

/**
 * ibmveth_free_rx_qstats - Free per-queue RX statistics
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_free_rx_qstats(struct ibmveth_adapter *adapter)
{
	kfree(adapter->rx_qstats);
	adapter->rx_qstats = NULL;
}

/**
 * ibmveth_alloc_tx_qstats - Allocate per-queue TX statistics
 * @adapter: ibmveth adapter structure
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_tx_qstats(struct ibmveth_adapter *adapter)
{
	adapter->tx_qstats = kcalloc(IBMVETH_MAX_QUEUES,
				     sizeof(*adapter->tx_qstats),
				     GFP_KERNEL);
	if (!adapter->tx_qstats)
		return -ENOMEM;

	return 0;
}

/**
 * ibmveth_free_tx_qstats - Free per-queue TX statistics
 * @adapter: ibmveth adapter structure
 */
static void ibmveth_free_tx_qstats(struct ibmveth_adapter *adapter)
{
	kfree(adapter->tx_qstats);
	adapter->tx_qstats = NULL;
}

/**
 * ibmveth_alloc_rx_queues - Allocate per-queue RX resources
 * @adapter: ibmveth adapter structure
 * @rxq_entries: Number of entries per RX queue
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int
ibmveth_alloc_rx_queues(struct ibmveth_adapter *adapter, int rxq_entries)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		adapter->buffer_list_addr[i] =
			(void *)get_zeroed_page(GFP_KERNEL);
		if (!adapter->buffer_list_addr[i]) {
			netdev_err(netdev,
				   "unable to allocate buffer list for queue %d\n",
				   i);
			goto err_cleanup;
		}

		adapter->rx_queue[i].queue_len =
			sizeof(struct ibmveth_rx_q_entry) * rxq_entries;
		adapter->rx_queue[i].queue_addr =
			dma_alloc_coherent(dev, adapter->rx_queue[i].queue_len,
					   &adapter->rx_queue[i].queue_dma,
					   GFP_KERNEL);
		if (!adapter->rx_queue[i].queue_addr) {
			netdev_err(netdev,
				   "unable to allocate RX queue for queue %d\n",
				   i);
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
			goto err_cleanup;
		}

		adapter->buffer_list_dma[i] =
			dma_map_single(dev, adapter->buffer_list_addr[i],
				       4096, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, adapter->buffer_list_dma[i])) {
			netdev_err(netdev,
				   "unable to map buffer list for queue %d\n",
				   i);
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
			adapter->buffer_list_dma[i] = 0;
			goto err_cleanup;
		}

		adapter->rx_queue[i].index = 0;
		adapter->rx_queue[i].num_slots = rxq_entries;
		adapter->rx_queue[i].toggle = 1;
	}

	return 0;

err_cleanup:
	/* Queue i may be partly set up: queue_addr can be live without
	 * a buffer list.
	 */
	for (; i >= 0; i--) {
		if (adapter->buffer_list_addr[i]) {
			dma_unmap_single(dev, adapter->buffer_list_dma[i],
					 4096, DMA_BIDIRECTIONAL);
			adapter->buffer_list_dma[i] = 0;
		}
		if (adapter->rx_queue[i].queue_addr) {
			dma_free_coherent(dev, adapter->rx_queue[i].queue_len,
					  adapter->rx_queue[i].queue_addr,
					  adapter->rx_queue[i].queue_dma);
			adapter->rx_queue[i].queue_addr = NULL;
		}
		if (adapter->buffer_list_addr[i]) {
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
		}
	}

	return -ENOMEM;
}

/**
 * ibmveth_cleanup_rx_resources - Free each RX queue's ring and buffer list
 * @adapter: ibmveth adapter structure
 *
 * The RX buffer pools are freed separately.
 */
static void
ibmveth_cleanup_rx_resources(struct ibmveth_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		if (adapter->buffer_list_addr[i]) {
			dma_unmap_single(dev, adapter->buffer_list_dma[i],
					 4096, DMA_BIDIRECTIONAL);
			adapter->buffer_list_dma[i] = 0;
		}

		if (adapter->rx_queue[i].queue_addr) {
			dma_free_coherent(dev, adapter->rx_queue[i].queue_len,
					  adapter->rx_queue[i].queue_addr,
					  adapter->rx_queue[i].queue_dma);
			adapter->rx_queue[i].queue_addr = NULL;
		}

		if (adapter->buffer_list_addr[i]) {
			free_page((unsigned long)adapter->buffer_list_addr[i]);
			adapter->buffer_list_addr[i] = NULL;
		}
	}
}

/**
 * ibmveth_toggle_irq - Mask or unmask one RX queue's interrupt in PHYP
 * @adapter: ibmveth adapter structure
 * @queue_index: RX queue index
 * @enable: true to unmask, false to mask
 *
 * Queue 0 uses h_vio_signal(); subordinate queues use H_VIOCTL.
 *
 * Return: 0 on success, -EIO on failure
 */
static int
ibmveth_toggle_irq(struct ibmveth_adapter *adapter, int queue_index,
		   bool enable)
{
	unsigned long h_rc;
	unsigned long irq = adapter->queue_irq[queue_index];
	const char *action = enable ? "enable" : "disable";

	if (queue_index == 0) {
		h_rc = h_vio_signal(adapter->vdev->unit_address,
				    enable ? VIO_IRQ_ENABLE : VIO_IRQ_DISABLE);
	} else {
		irq_hw_number_t hwirq = irqd_to_hwirq(irq_get_irq_data(irq));
		u64 vioctl_cmd = enable ? H_ENABLE_VIO_INTERRUPT :
			H_DISABLE_VIO_INTERRUPT;

		h_rc = plpar_hcall_norets(H_VIOCTL,
					  adapter->vdev->unit_address,
					  vioctl_cmd,
					  hwirq, 0, 0);
	}

	if (h_rc) {
		dev_err_ratelimited(&adapter->netdev->dev,
				    "Failed to %s IRQ for queue %d, rc=0x%lx\n",
				    action, queue_index, h_rc);
		return -EIO;
	}
	return 0;
}

/**
 * ibmveth_disable_irq - Disable interrupt for a specific queue
 * @adapter: ibmveth adapter structure
 * @queue_index: RX queue index
 *
 * Return: 0 on success, -EIO on failure
 */
static int
ibmveth_disable_irq(struct ibmveth_adapter *adapter, int queue_index)
{
	return ibmveth_toggle_irq(adapter, queue_index, false);
}

/**
 * ibmveth_enable_irq - Enable interrupt for a specific queue
 * @adapter: ibmveth adapter structure
 * @queue_index: RX queue index
 *
 * Return: 0 on success, -EIO on failure
 */
static int
ibmveth_enable_irq(struct ibmveth_adapter *adapter, int queue_index)
{
	return ibmveth_toggle_irq(adapter, queue_index, true);
}

/**
 * ibmveth_dispose_subordinate_irq_mappings - Drop virq mappings for queues 1..N
 * @adapter: ibmveth adapter structure
 *
 * Subordinate queues get mappings from irq_create_mapping() during PHYP
 * registration; queue 0 uses netdev->irq and is left alone. Call after
 * free_irq(), or after a registration failure that may have mapped only
 * some of the queues.
 */
static void
ibmveth_dispose_subordinate_irq_mappings(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 1; i < adapter->num_rx_queues; i++) {
		if (adapter->queue_irq[i]) {
			irq_dispose_mapping(adapter->queue_irq[i]);
			adapter->queue_irq[i] = 0;
		}
	}
}

/**
 * ibmveth_queue_irq_name - Name for one RX queue's Linux IRQ
 * @adapter: ibmveth adapter structure
 * @queue_idx: RX queue index
 *
 * Queue 0 keeps netdev->name, as before multi-queue. Subordinate queues
 * use "ibmveth-<unit address>-rx<N>", which survives a rename.
 * request_irq() keeps the pointer, so the string lives in the adapter.
 *
 * Return: the name to pass to request_irq()
 */
static const char *ibmveth_queue_irq_name(struct ibmveth_adapter *adapter,
					  int queue_idx)
{
	char *name = adapter->queue_irq_name[queue_idx];

	if (!queue_idx)
		return adapter->netdev->name;

	snprintf(name, sizeof(adapter->queue_irq_name[0]), "ibmveth-%x-rx%d",
		 adapter->vdev->unit_address, queue_idx);
	return name;
}

/**
 * ibmveth_setup_rx_interrupts - Register IRQs and enable NAPI
 * @adapter: ibmveth adapter structure
 *
 * Registers interrupt handlers for all RX queues, enables NAPI, then
 * enables hypervisor interrupt delivery for multi-queue mode after
 * every queue has a Linux handler installed. For multi-queue open the
 * caller should replenish RX buffers before this helper so traffic
 * during open is not dropped (PHYP only interrupts after a successful
 * enqueue, which needs buffers). Single-queue open leaves PHYP masked
 * here and kicks NAPI afterward (classic path: first poll posts then
 * enables).
 *
 * Return: 0 on success, negative error code on failure
 */
static int
ibmveth_setup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, rc, num = adapter->num_rx_queues;

	for (i = 0; i < num; i++) {
		rc = request_irq(adapter->queue_irq[i], ibmveth_interrupt,
				 0, ibmveth_queue_irq_name(adapter, i),
				 &adapter->napi[i]);
		if (rc) {
			netdev_err(netdev,
				   "request_irq() failed for irq 0x%x queue %d: %d\n",
				   adapter->queue_irq[i], i, rc);
			goto err_free_irqs;
		}
	}

	for (i = 0; i < num; i++)
		napi_enable(&adapter->napi[i]);

	if (adapter->multi_queue && num > 1) {
		for (i = 0; i < num; i++) {
			rc = ibmveth_enable_irq(adapter, i);
			if (rc)
				goto err_disable_napi;
		}
	}

	return 0;

err_disable_napi:
	/* Wait for the poll tail, then remask; an in-flight poll can
	 * unmask after napi_complete_done().
	 */
	for (i = 0; i < num; i++)
		napi_disable(&adapter->napi[i]);
	synchronize_net();
	for (i = 0; i < num; i++) {
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}
	i = num;
err_free_irqs:
	while (--i >= 0)
		free_irq(adapter->queue_irq[i], &adapter->napi[i]);
	return rc;
}

/**
 * ibmveth_cleanup_rx_interrupts - Mask PHYP IRQs, stop NAPI, and free IRQs
 * @adapter: ibmveth adapter structure
 *
 * Mask, napi_disable, wait for the poll tail, remask, then free_irq.
 * napi_disable() does not wait past napi_complete_done(), which can
 * unmask.
 */
static void
ibmveth_cleanup_rx_interrupts(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}

	for (i = 0; i < adapter->num_rx_queues; i++)
		napi_disable(&adapter->napi[i]);

	synchronize_net();

	for (i = 0; i < adapter->num_rx_queues; i++) {
		ibmveth_disable_irq(adapter, i);
		synchronize_irq(adapter->queue_irq[i]);
	}

	for (i = 0; i < adapter->num_rx_queues; i++)
		free_irq(adapter->queue_irq[i], &adapter->napi[i]);

	ibmveth_dispose_subordinate_irq_mappings(adapter);
}

/**
 * ibmveth_schedule_rx_queue - Mask PHYP IRQ and schedule NAPI for one RX queue
 * @adapter: ibmveth adapter structure
 * @qindex: RX queue index
 *
 * Shared by the IRQ handler and process-context kick sites (open, resume,
 * pool sysfs).
 *
 * Does nothing if napi_schedule_prep() fails (NAPI already scheduled,
 * or disabled because the device is closed). A failed
 * ibmveth_disable_irq() is logged and the queue may stay unmasked.
 */
static void ibmveth_schedule_rx_queue(struct ibmveth_adapter *adapter,
				      int qindex)
{
	struct napi_struct *napi = &adapter->napi[qindex];

	if (napi_schedule_prep(napi)) {
		ibmveth_disable_irq(adapter, qindex);
		__napi_schedule(napi);
	}
}

/* setup the initial settings for a buffer pool */
static void ibmveth_init_buffer_pool(struct ibmveth_buff_pool *pool,
				     u32 pool_index, u32 pool_size,
				     u32 buff_size, u32 pool_active)
{
	pool->size = pool_size;
	pool->index = pool_index;
	pool->buff_size = buff_size;
	pool->threshold = pool_size * 7 / 8;
	pool->active = pool_active;
}

/* allocate and setup an buffer pool - called during open */
static int ibmveth_alloc_buffer_pool(struct ibmveth_buff_pool *pool)
{
	int i;

	pool->free_map = kmalloc_objs(u16, pool->size);

	if (!pool->free_map)
		return -1;

	pool->dma_addr = kzalloc_objs(dma_addr_t, pool->size);
	if (!pool->dma_addr) {
		kfree(pool->free_map);
		pool->free_map = NULL;
		return -1;
	}

	pool->skbuff = kcalloc(pool->size, sizeof(void *), GFP_KERNEL);

	if (!pool->skbuff) {
		kfree(pool->dma_addr);
		pool->dma_addr = NULL;

		kfree(pool->free_map);
		pool->free_map = NULL;
		return -1;
	}

	for (i = 0; i < pool->size; ++i)
		pool->free_map[i] = i;

	atomic_set(&pool->available, 0);
	pool->producer_index = 0;
	pool->consumer_index = 0;

	return 0;
}

static inline void ibmveth_flush_buffer(void *addr, unsigned long length)
{
	unsigned long offset;

	for (offset = 0; offset < length; offset += SMP_CACHE_BYTES)
		asm("dcbf %0,%1,1" :: "b" (addr), "r" (offset));
}

/**
 * ibmveth_add_logical_lan_buffers - Add receive buffers to hypervisor
 * @adapter: ibmveth adapter structure
 * @descs: array of buffer descriptors to add
 * @filled: number of valid descriptors in the array
 * @buff_size: size of each buffer (multi-queue mode only)
 * @queue_index: RX queue index
 *
 * Return: hypervisor return code
 */
static long ibmveth_add_logical_lan_buffers(struct ibmveth_adapter *adapter,
					    union ibmveth_buf_desc *descs,
					    int filled,
					    unsigned long buff_size,
					    int queue_index)
{
	struct vio_dev *vdev = adapter->vdev;
	unsigned long rc;

	/*
	 * The MQ hcall takes six ioba words (12 packed addresses). The
	 * legacy hcall takes eight descriptors. The argument lists below
	 * are written out by hand; keep the defines matched to those lists.
	 */
	static_assert(IBMVETH_MAX_RX_PER_HCALL == 12);
	static_assert(IBMVETH_MAX_RX_REGULAR == 8);

	if (adapter->multi_queue) {
		unsigned long buffersznum = (buff_size << 32) | filled;
		unsigned long ioba[IBMVETH_MAX_RX_PER_HCALL / 2] = {0};
		unsigned long handle = adapter->queue_handle[queue_index];
		int i;

		/* Even descriptors go in the high 32 bits of an ioba word,
		 * odd ones in the low 32 bits.
		 */
		for (i = 0; i < filled && i < IBMVETH_MAX_RX_PER_HCALL; i++) {
			int pair_idx = i / 2;
			int is_high = (i % 2 == 0);

			if (is_high)
				ioba[pair_idx] = (unsigned long)
					descs[i].fields.address << 32;
			else
				ioba[pair_idx] |= descs[i].fields.address;
		}

		rc = h_add_logical_lan_buffers_queue(vdev->unit_address,
						     handle,
						     buffersznum,
						     ioba[0], ioba[1], ioba[2],
						     ioba[3], ioba[4], ioba[5]);
	} else if (filled == 1) {
		rc = h_add_logical_lan_buffer(vdev->unit_address,
					      descs[0].desc);
	} else {
		/* Legacy 8-desc hcall; rx_buffers_per_hcall never exceeds
		 * IBMVETH_MAX_RX_REGULAR outside multi-queue mode.
		 */
		rc = h_add_logical_lan_buffers(vdev->unit_address,
					       descs[0].desc, descs[1].desc,
					       descs[2].desc, descs[3].desc,
					       descs[4].desc, descs[5].desc,
					       descs[6].desc, descs[7].desc);
	}

	return rc;
}

static void ibmveth_count_add_fail(struct ibmveth_rx_queue_stats *qstats,
				   bool no_mem)
{
	if (no_mem)
		IBMVETH_STAT_INC(qstats->replenish_no_mem);
	IBMVETH_STAT_INC(qstats->replenish_add_buff_failure);
}

/* replenish the buffers for a pool.  note that we don't need to
 * skb_reserve these since they are used for incoming...
 */
static void ibmveth_replenish_buffer_pool(struct ibmveth_adapter *adapter,
					  struct ibmveth_buff_pool *pool,
					  int queue_index)
{
	struct ibmveth_rx_queue_stats *qstats =
		&adapter->rx_qstats[queue_index];
	union ibmveth_buf_desc descs[IBMVETH_MAX_RX_PER_HCALL] = {0};
	u32 remaining = pool->size - atomic_read(&pool->available);
	u64 correlators[IBMVETH_MAX_RX_PER_HCALL] = {0};
	unsigned long lpar_rc;
	u32 buffers_added = 0;
	u32 i, filled, batch;
	struct vio_dev *vdev;
	dma_addr_t dma_addr;
	struct device *dev;
	u32 index;

	vdev = adapter->vdev;
	dev = &vdev->dev;

	mb();

	batch = adapter->rx_buffers_per_hcall;

	while (remaining > 0) {
		unsigned int free_index = pool->consumer_index;

		/* Fill a batch of descriptors */
		for (filled = 0; filled < min(remaining, batch); filled++) {
			index = pool->free_map[free_index];
			if (WARN_ON(index == IBM_VETH_INVALID_MAP)) {
				ibmveth_count_add_fail(qstats, false);
				netdev_info(adapter->netdev,
					    "Invalid map index %u, reset\n",
					    index);
				schedule_work(&adapter->work);
				break;
			}

			if (!pool->skbuff[index]) {
				struct sk_buff *skb = NULL;

				skb = netdev_alloc_skb(adapter->netdev,
						       pool->buff_size);
				if (!skb) {
					ibmveth_count_add_fail(qstats, true);
					break;
				}

				dma_addr = dma_map_single(dev, skb->data,
							  pool->buff_size,
							  DMA_FROM_DEVICE);
				if (dma_mapping_error(dev, dma_addr)) {
					dev_kfree_skb_any(skb);
					ibmveth_count_add_fail(qstats, false);
					break;
				}

				pool->dma_addr[index] = dma_addr;
				pool->skbuff[index] = skb;
			} else {
				/* re-use case */
				dma_addr = pool->dma_addr[index];
			}

			if (rx_flush) {
				unsigned int len;

				len = adapter->netdev->mtu + IBMVETH_BUFF_OH;
				len = min(pool->buff_size, len);
				ibmveth_flush_buffer(pool->skbuff[index]->data,
						     len);
			}

			descs[filled].fields.flags_len = IBMVETH_BUF_VALID |
							  pool->buff_size;
			descs[filled].fields.address = dma_addr;

			correlators[filled] = ((u64)pool->index << 32) | index;
			*(u64 *)pool->skbuff[index]->data = correlators[filled];

			free_index++;
			if (free_index >= pool->size)
				free_index = 0;
		}

		if (!filled)
			break;

		lpar_rc = ibmveth_add_logical_lan_buffers(adapter, descs,
							  filled,
							  pool->buff_size,
							  queue_index);
		if (lpar_rc != H_SUCCESS) {
			dev_warn_ratelimited(dev,
					     "RX %s failed: filled=%u, rc=%lu, batch=%u\n",
					     adapter->multi_queue ?
					     "h_add_logical_lan_buffers_queue" :
					     "h_add_logical_lan",
					     filled, lpar_rc, batch);
			goto hcall_failure;
		}

		/* Only update pool state after hcall succeeds */
		for (i = 0; i < filled; i++) {
			free_index = pool->consumer_index;
			pool->free_map[free_index] = IBM_VETH_INVALID_MAP;

			pool->consumer_index++;
			if (pool->consumer_index >= pool->size)
				pool->consumer_index = 0;
		}

		buffers_added += filled;
		IBMVETH_STAT_ADD(qstats->replenish_add_buff_success, filled);
		remaining -= filled;

		memset(&descs, 0, sizeof(descs));
		memset(&correlators, 0, sizeof(correlators));
		continue;

hcall_failure:
		for (i = 0; i < filled; i++) {
			index = correlators[i] & 0xffffffffUL;
			dma_addr =  pool->dma_addr[index];

			if (pool->skbuff[index]) {
				if (dma_addr &&
				    !dma_mapping_error(dev, dma_addr))
					dma_unmap_single(dev, dma_addr,
							 pool->buff_size,
							 DMA_FROM_DEVICE);

				dev_kfree_skb_any(pool->skbuff[index]);
				pool->skbuff[index] = NULL;
			}
		}
		IBMVETH_STAT_ADD(qstats->replenish_add_buff_failure, filled);

		/*
		 * If multi rx buffers hcall is no longer supported by FW
		 * e.g. in the case of Live Partition Migration
		 */
		if (lpar_rc == H_FUNCTION && adapter->multi_queue) {
			/* Reset rather than sit without buffers, link up */
			dev_err_ratelimited(dev,
					    "MQ buffer add H_FUNCTION (q=%d, batch=%u), reset\n",
					    queue_index, batch);
			WRITE_ONCE(adapter->mq_fallback, true);
			schedule_work(&adapter->work);
		} else if (batch > 1 && lpar_rc == H_FUNCTION) {
			/*
			 * Instead of retry submit single buffer individually
			 * here just set the max rx buffer per hcall to 1
			 * buffers will be respleshed next time
			 * when ibmveth_replenish_buffer_pool() is called again
			 * with single-buffer case
			 */
			netdev_info(adapter->netdev,
				    "RX Multi buffers not supported by FW, rc=%lu\n",
				    lpar_rc);
			adapter->rx_buffers_per_hcall = 1;
			netdev_info(adapter->netdev,
				    "Next rx replesh will fall back to single-buffer hcall\n");
		}
		break;
	}

	mb();
	atomic_add(buffers_added, &(pool->available));
}

/*
 * The final 8 bytes of the buffer list is a counter of frames dropped
 * because there was not a buffer in the buffer list capable of holding
 * the frame.
 */
static void ibmveth_update_rx_no_buffer(struct ibmveth_adapter *adapter,
					int queue_index)
{
	struct ibmveth_rx_queue_stats *qstats;
	__be64 *p;
	u64 drops;

	p = adapter->buffer_list_addr[queue_index] + 4096 - 8;
	drops = be64_to_cpup(p);

	qstats = &adapter->rx_qstats[queue_index];

	/* The page reads 0 once PHYP has freed the LAN; keep what was seen. */
	if (drops < qstats->no_buffer_drops)
		IBMVETH_STAT_ADD(qstats->no_buffer_retired,
				 qstats->no_buffer_drops);
	WRITE_ONCE(qstats->no_buffer_drops, drops);
	WRITE_ONCE(qstats->no_buffer_total,
		   qstats->no_buffer_retired + drops);
}

static u64 ibmveth_rx_no_buffer_total(struct ibmveth_adapter *adapter,
				      int queue_index)
{
	return READ_ONCE(adapter->rx_qstats[queue_index].no_buffer_total);
}

/*
 * Fold the last sample into retired once the queue is freed: the next
 * registration restarts PHYP's counter at 0. The fold leaves
 * no_buffer_total unchanged.
 */
static void ibmveth_retire_rx_no_buffer(struct ibmveth_adapter *adapter,
					int queue_index)
{
	struct ibmveth_rx_queue_stats *qstats;

	qstats = &adapter->rx_qstats[queue_index];
	IBMVETH_STAT_ADD(qstats->no_buffer_retired, qstats->no_buffer_drops);
	WRITE_ONCE(qstats->no_buffer_drops, 0);
}

/* replenish routine */
static void ibmveth_replenish_task(struct ibmveth_adapter *adapter,
				   int queue_index)
{
	int i;

	IBMVETH_STAT_INC(adapter->rx_qstats[queue_index].replenish_task_cycles);

	for (i = (IBMVETH_NUM_BUFF_POOLS - 1); i >= 0; i--) {
		struct ibmveth_buff_pool *pool =
			&adapter->rx_buff_pool[queue_index][i];

		if (pool->active &&
		    (atomic_read(&pool->available) < pool->threshold))
			ibmveth_replenish_buffer_pool(adapter, pool,
						      queue_index);
	}

	ibmveth_update_rx_no_buffer(adapter, queue_index);
}

/* empty and free ana buffer pool - also used to do cleanup in error paths */
static void ibmveth_free_buffer_pool(struct ibmveth_adapter *adapter,
				     struct ibmveth_buff_pool *pool)
{
	int i;

	kfree(pool->free_map);
	pool->free_map = NULL;

	if (pool->skbuff && pool->dma_addr) {
		for (i = 0; i < pool->size; ++i) {
			struct sk_buff *skb = pool->skbuff[i];
			if (skb) {
				dma_unmap_single(&adapter->vdev->dev,
						 pool->dma_addr[i],
						 pool->buff_size,
						 DMA_FROM_DEVICE);
				dev_kfree_skb_any(skb);
				pool->skbuff[i] = NULL;
			}
		}
	}

	if (pool->dma_addr) {
		kfree(pool->dma_addr);
		pool->dma_addr = NULL;
	}

	if (pool->skbuff) {
		kfree(pool->skbuff);
		pool->skbuff = NULL;
	}
}

/* The correlator comes back from PHYP; a bad one schedules a reset. */
static bool ibmveth_rxq_correlator_valid(struct ibmveth_adapter *adapter,
					 int queue_index, u64 correlator)
{
	unsigned int index = correlator & 0xffffffffUL;
	unsigned int pool = correlator >> 32;

	/* An inactive pool keeps its size but has no skbuff array. */
	if (pool < IBMVETH_NUM_BUFF_POOLS &&
	    index < adapter->rx_buff_pool[queue_index][pool].size &&
	    adapter->rx_buff_pool[queue_index][pool].skbuff)
		return true;

	if (net_ratelimit())
		netdev_err(adapter->netdev,
			   "invalid RX correlator %llx, resetting\n",
			   correlator);
	schedule_work(&adapter->work);
	return false;
}

static void ibmveth_rxq_no_skb(struct ibmveth_adapter *adapter,
			       u64 correlator)
{
	if (net_ratelimit())
		netdev_err(adapter->netdev,
			   "no buffer for RX correlator %llx, resetting\n",
			   correlator);
	schedule_work(&adapter->work);
}

/**
 * ibmveth_free_queue_buffer_pools - Free buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Frees every pool on @queue regardless of the active flag;
 * ibmveth_free_buffer_pool() skips what was never allocated.
 */
static void ibmveth_free_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_free_buffer_pool(adapter,
					 &adapter->rx_buff_pool[queue][i]);
}

/**
 * ibmveth_alloc_queue_buffer_pools - Allocate buffer pools for a single queue
 * @adapter: ibmveth adapter structure
 * @queue: queue index
 *
 * Allocates backing storage for each active pool on @queue.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_queue_buffer_pools(struct ibmveth_adapter *adapter,
					    int queue)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct ibmveth_buff_pool *bpool =
			&adapter->rx_buff_pool[queue][i];

		if (!bpool->active)
			continue;

		if (ibmveth_alloc_buffer_pool(bpool)) {
			netdev_err(netdev,
				   "queue %d pool %d alloc failed (size=%u count=%u)\n",
				   queue, i,
				   bpool->buff_size,
				   bpool->size);
			bpool->active = 0;
			/* Undo the pools this queue already allocated; the
			 * failed one freed its own partial allocation.
			 */
			ibmveth_free_queue_buffer_pools(adapter, queue);
			return -ENOMEM;
		}
	}

	return 0;
}

/**
 * ibmveth_alloc_buffer_pools - Allocate buffer pools for all queues
 * @adapter: ibmveth adapter structure
 *
 * Copy queue 0's pool metadata onto any further queues, then allocate
 * buffer pools for every queue.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int
ibmveth_alloc_buffer_pools(struct ibmveth_adapter *adapter)
{
	int i, q, rc;

	for (q = 1; q < adapter->num_rx_queues; q++) {
		for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
			struct ibmveth_buff_pool *src =
				&adapter->rx_buff_pool[0][i];
			struct ibmveth_buff_pool *dst =
				&adapter->rx_buff_pool[q][i];

			dst->size = src->size;
			dst->index = src->index;
			dst->buff_size = src->buff_size;
			dst->threshold = src->threshold;
			dst->active = src->active;
		}
	}

	for (q = 0; q < adapter->num_rx_queues; q++) {
		rc = ibmveth_alloc_queue_buffer_pools(adapter, q);
		if (rc) {
			while (--q >= 0)
				ibmveth_free_queue_buffer_pools(adapter, q);
			return rc;
		}
	}

	return 0;
}

/**
 * ibmveth_free_buffer_pools - Free buffer pools for all queues
 * @adapter: ibmveth adapter structure
 */
static void
ibmveth_free_buffer_pools(struct ibmveth_adapter *adapter)
{
	int q;

	for (q = 0; q < adapter->num_rx_queues; q++)
		ibmveth_free_queue_buffer_pools(adapter, q);
}

/**
 * ibmveth_remove_buffer_from_pool - remove a buffer from a pool
 * @adapter: adapter instance
 * @queue_index: RX queue index
 * @correlator: identifies pool and index
 * @reuse: whether to reuse buffer
 *
 * Return:
 * * %0       - success
 * * %-EINVAL - correlator maps to pool or index out of range, or to an
 *              inactive pool
 * * %-EFAULT - pool and index map to null skb
 */
static int ibmveth_remove_buffer_from_pool(struct ibmveth_adapter *adapter,
					   int queue_index, u64 correlator,
					   bool reuse)
{
	unsigned int pool  = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;
	struct ibmveth_buff_pool *bpool;
	unsigned int free_index;
	struct sk_buff *skb;

	if (!ibmveth_rxq_correlator_valid(adapter, queue_index, correlator))
		return -EINVAL;

	bpool = &adapter->rx_buff_pool[queue_index][pool];
	skb = bpool->skbuff[index];
	if (!skb) {
		ibmveth_rxq_no_skb(adapter, correlator);
		return -EFAULT;
	}

	/* if we are going to reuse the buffer then keep the pointers around
	 * but mark index as available. replenish will see the skb pointer and
	 * assume it is to be recycled.
	 */
	if (!reuse) {
		/* remove the skb pointer to mark free. actual freeing is done
		 * by upper level networking after gro_receive
		 */
		bpool->skbuff[index] = NULL;

		dma_unmap_single(&adapter->vdev->dev,
				 bpool->dma_addr[index],
				 bpool->buff_size,
				 DMA_FROM_DEVICE);
	}

	free_index = bpool->producer_index;
	bpool->producer_index++;
	if (bpool->producer_index >= bpool->size)
		bpool->producer_index = 0;
	bpool->free_map[free_index] = index;

	mb();

	atomic_dec(&bpool->available);

	return 0;
}

/* get the buffer for @correlator, read once from the current rx queue entry */
static struct sk_buff *ibmveth_rxq_get_buffer(struct ibmveth_adapter *adapter,
					      int queue_index, u64 correlator)
{
	unsigned int pool = correlator >> 32;
	unsigned int index = correlator & 0xffffffffUL;
	struct sk_buff *skb;

	if (!ibmveth_rxq_correlator_valid(adapter, queue_index, correlator))
		return NULL;

	skb = adapter->rx_buff_pool[queue_index][pool].skbuff[index];
	if (!skb)
		ibmveth_rxq_no_skb(adapter, correlator);
	return skb;
}

static void ibmveth_rxq_advance(struct ibmveth_adapter *adapter,
				int queue_index)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_index];

	if (++rxq->index == rxq->num_slots) {
		rxq->index = 0;
		rxq->toggle = !rxq->toggle;
	}
}

/**
 * ibmveth_rxq_harvest_buffer - Harvest buffer from pool
 *
 * @adapter:    pointer to adapter
 * @queue_index: RX queue index
 * @correlator: correlator of the current rx queue entry, as read by poll
 * @reuse:      whether to reuse buffer
 *
 * Context: NAPI poll, from the per-slot RX helpers
 *
 * The ring advances even on error, so poll does not return to a bad
 * slot before the scheduled reset can run.
 *
 * Return:
 * * %0    - success
 * * other - non-zero return from ibmveth_remove_buffer_from_pool
 */
static int ibmveth_rxq_harvest_buffer(struct ibmveth_adapter *adapter,
				      int queue_index, u64 correlator,
				      bool reuse)
{
	int rc;

	rc = ibmveth_remove_buffer_from_pool(adapter, queue_index, correlator,
					     reuse);
	ibmveth_rxq_advance(adapter, queue_index);

	return rc;
}

static void ibmveth_free_tx_ltb(struct ibmveth_adapter *adapter, int idx)
{
	void *ptr = adapter->tx_ltb_ptr[idx];

	if (!ptr)
		return;

	WRITE_ONCE(adapter->tx_ltb_ptr[idx], NULL);
	dma_unmap_single(&adapter->vdev->dev, adapter->tx_ltb_dma[idx],
			 adapter->tx_ltb_size, DMA_TO_DEVICE);
	kfree(ptr);
}

static int ibmveth_allocate_tx_ltb(struct ibmveth_adapter *adapter, int idx)
{
	void *ptr;

	ptr = kzalloc(adapter->tx_ltb_size, GFP_KERNEL);
	if (!ptr) {
		netdev_err(adapter->netdev,
			   "unable to allocate tx long term buffer\n");
		return -ENOMEM;
	}
	adapter->tx_ltb_dma[idx] = dma_map_single(&adapter->vdev->dev, ptr,
						  adapter->tx_ltb_size,
						  DMA_TO_DEVICE);
	if (dma_mapping_error(&adapter->vdev->dev, adapter->tx_ltb_dma[idx])) {
		netdev_err(adapter->netdev,
			   "unable to DMA map tx long term buffer\n");
		kfree(ptr);
		return -ENOMEM;
	}

	WRITE_ONCE(adapter->tx_ltb_ptr[idx], ptr);
	return 0;
}

/**
 * ibmveth_alloc_tx_resources - Allocate TX LTBs for real_num_tx_queues
 * @adapter: ibmveth adapter structure
 *
 * Allocate a TX long-term buffer for each live TX queue. On a
 * partial failure this helper already frees what it allocated.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
static int ibmveth_alloc_tx_resources(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < netdev->real_num_tx_queues; i++) {
		if (ibmveth_allocate_tx_ltb(adapter, i))
			goto err_free_ltbs;
	}

	return 0;

err_free_ltbs:
	while (--i >= 0)
		ibmveth_free_tx_ltb(adapter, i);
	return -ENOMEM;
}

/**
 * ibmveth_free_tx_resources - Free TX LTBs for real_num_tx_queues
 * @adapter: ibmveth adapter structure
 *
 * Free the TX long-term buffers for each live TX queue.
 */
static void ibmveth_free_tx_resources(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i;

	for (i = 0; i < netdev->real_num_tx_queues; i++)
		ibmveth_free_tx_ltb(adapter, i);
}

/**
 * ibmveth_free_logical_lan - Busy-retry H_FREE_LOGICAL_LAN
 * @adapter: ibmveth adapter structure
 *
 * Retries while PHYP returns H_BUSY or a long-busy code.
 *
 * Return: the last hypervisor return code
 */
static long ibmveth_free_logical_lan(struct ibmveth_adapter *adapter)
{
	long rc;

	do {
		rc = h_free_logical_lan(adapter->vdev->unit_address);
	} while (H_IS_LONG_BUSY(rc) || rc == H_BUSY);

	return rc;
}

static long ibmveth_register_logical_lan(struct ibmveth_adapter *adapter,
					 union ibmveth_buf_desc rxq_desc,
					 u64 mac_address)
{
	long rc;
	int try_again = 1;
	unsigned long ua = adapter->vdev->unit_address;
	unsigned long buf_dma = adapter->buffer_list_dma[0];
	unsigned long filter_dma = adapter->filter_list_dma;
	unsigned long qh0;

	/*
	 * After a kexec the adapter will still be open, so our attempt to
	 * open it will fail. So if we get a failure we free the adapter and
	 * try again, but only once.
	 */
retry:
	/* In multi-queue mode, obtain a queue handle for queue 0 so all RX
	 * queues can use the same per-queue buffer hypercalls.
	 */
	if (adapter->multi_queue) {
		rc = h_register_logical_lan_with_handle(ua, buf_dma,
							rxq_desc.desc,
							filter_dma,
							mac_address,
							&qh0);
		if (rc == H_SUCCESS)
			adapter->queue_handle[0] = qh0;
	} else {
		rc = h_register_logical_lan(ua, buf_dma, rxq_desc.desc,
					    filter_dma, mac_address);
	}

	if (rc != H_SUCCESS && try_again) {
		rc = ibmveth_free_logical_lan(adapter);

		try_again = 0;
		goto retry;
	}

	return rc;
}

static bool ibmveth_lan_resources_live(struct ibmveth_adapter *adapter)
{
	int i;

	if (adapter->filter_list_addr)
		return true;
	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++) {
		if (adapter->buffer_list_addr[i])
			return true;
	}
	return false;
}

static void ibmveth_release_lan_resources(struct ibmveth_adapter *adapter)
{
	ibmveth_free_buffer_pools(adapter);
	ibmveth_cleanup_rx_resources(adapter);
	ibmveth_free_filter_list(adapter);
}

/**
 * ibmveth_free_all_queues - Free all RX queues at once
 * @adapter: ibmveth adapter structure
 *
 * Issues one H_FREE_LOGICAL_LAN, which frees the logical LAN and every
 * RX queue registered under it.
 *
 * In multi-queue mode a failure other than H_PARAMETER (already free)
 * means the free never started and PHYP may still DMA into the LAN
 * memory, so return -EIO and leave it mapped; the next open() or
 * remove() retries. Single-queue mode logs and lets the caller unmap,
 * as close() always did: on a CMO partition vio_cmo_bus_remove()
 * BUG()s on an outstanding entitlement.
 *
 * PHYP zeroes the buffer-list pages on free, so each queue's no-buffer
 * counter is sampled before H_FREE_LOGICAL_LAN and retired only once
 * the caller may unmap.
 *
 * Return: 0 if the caller may unmap, -EIO if it must not
 */
static int ibmveth_free_all_queues(struct ibmveth_adapter *adapter)
{
	long lpar_rc;
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_update_rx_no_buffer(adapter, i);

	lpar_rc = ibmveth_free_logical_lan(adapter);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(adapter->netdev,
			   "h_free_logical_lan failed: %ld\n", lpar_rc);
		if (lpar_rc != H_PARAMETER && adapter->multi_queue)
			return -EIO;
	}

	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_retire_rx_no_buffer(adapter, i);

	return 0;
}

/**
 * ibmveth_register_rx_queue - Register one subordinate RX queue
 * @adapter: ibmveth adapter structure
 * @queue_idx: RX queue index (1..N)
 *
 * Registers the queue with H_REG_LOGICAL_LAN_QUEUE and maps its interrupt.
 * On failure the caller frees every queue with H_FREE_LOGICAL_LAN.
 *
 * Return: 0 on success, -EOPNOTSUPP if firmware rejects MQ (H_FUNCTION),
 *         -ENOMEM if the interrupt cannot be mapped, -EIO otherwise
 */
static int ibmveth_register_rx_queue(struct ibmveth_adapter *adapter,
				     int queue_idx)
{
	struct ibmveth_rx_q *rxq = &adapter->rx_queue[queue_idx];
	unsigned long bl = adapter->buffer_list_dma[queue_idx];
	unsigned long ua = adapter->vdev->unit_address;
	union ibmveth_buf_desc rxq_desc;
	unsigned long handle, hwirq;
	unsigned int virq;
	long lpar_rc;

	rxq_desc.fields.flags_len = IBMVETH_BUF_VALID | rxq->queue_len;
	rxq_desc.fields.address = rxq->queue_dma;

	do {
		lpar_rc = h_register_logical_lan_queue(ua, bl, rxq_desc.desc,
						       &handle, &hwirq);
	} while (H_IS_LONG_BUSY(lpar_rc) || lpar_rc == H_BUSY);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(adapter->netdev,
			   "h_register_logical_lan_queue failed for queue %d: %ld\n",
			   queue_idx, lpar_rc);
		return lpar_rc == H_FUNCTION ? -EOPNOTSUPP : -EIO;
	}

	virq = irq_create_mapping(NULL, hwirq);
	if (!virq) {
		netdev_err(adapter->netdev,
			   "failed to map IRQ for queue %d (hwirq=%lu)\n",
			   queue_idx, hwirq);
		return -ENOMEM;
	}

	adapter->queue_handle[queue_idx] = handle;
	adapter->queue_irq[queue_idx] = virq;
	return 0;
}

/**
 * ibmveth_register_rx_queues - Register RX queues with hypervisor
 * @adapter: ibmveth adapter structure
 * @mac_address: MAC address for device registration
 * @skip_unmap: set when H_FREE_LOGICAL_LAN hard-fails after a
 *              subordinate register error, so open() does not unmap
 *              pages PHYP still owns
 *
 * Registers queue 0 via ibmveth_register_logical_lan(), then subordinate
 * queues 1..N when multi-queue mode is enabled.
 *
 * Return: 0 on success, -ENONET if queue 0 registration fails,
 *         or the error from ibmveth_register_rx_queue(); on
 *         -EOPNOTSUPP (H_FUNCTION) this function latches mq_fallback
 */
static int
ibmveth_register_rx_queues(struct ibmveth_adapter *adapter, u64 mac_address,
			   bool *skip_unmap)
{
	struct net_device *netdev = adapter->netdev;
	union ibmveth_buf_desc rxq_desc;
	long lpar_rc;
	unsigned int num;
	int i, rc;

	*skip_unmap = false;

	rxq_desc.fields.flags_len = IBMVETH_BUF_VALID |
				    adapter->rx_queue[0].queue_len;
	rxq_desc.fields.address = adapter->rx_queue[0].queue_dma;
	adapter->queue_irq[0] = netdev->irq;

	ibmveth_disable_irq(adapter, 0);

	lpar_rc = ibmveth_register_logical_lan(adapter, rxq_desc, mac_address);
	if (lpar_rc != H_SUCCESS) {
		netdev_err(netdev,
			   "h_register_logical_lan failed: %ld\n", lpar_rc);
		netdev_err(netdev,
			   "buffer TCE:0x%llx filter TCE:0x%llx rxq desc:0x%llx MAC:0x%llx\n",
			   adapter->buffer_list_dma[0],
			   adapter->filter_list_dma,
			   rxq_desc.desc, mac_address);
		return -ENONET;
	}

	num = adapter->num_rx_queues;
	if (num == 1 || !adapter->multi_queue)
		return 0;

	for (i = 1; i < num; i++) {
		rc = ibmveth_register_rx_queue(adapter, i);
		if (rc) {
			/* Firmware MQ gone: ibmveth_open() retries SQ. */
			if (rc == -EOPNOTSUPP)
				WRITE_ONCE(adapter->mq_fallback, true);
			goto err_unregister;
		}
	}

	return 0;

err_unregister:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	if (ibmveth_free_all_queues(adapter))
		*skip_unmap = true;
	return rc;
}

/**
 * ibmveth_apply_mq_fallback - Drop multi-queue mode after firmware rejection
 * @adapter: ibmveth adapter
 *
 * mq_fallback is set when firmware rejects MQ (subordinate register or
 * buffer-add H_FUNCTION). Called at the start of open, after teardown,
 * so num_rx_queues never shrinks under live IRQs or NAPI. Clearing
 * multi_queue makes single-queue permanent for this device.
 */
static void ibmveth_apply_mq_fallback(struct ibmveth_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;

	if (!READ_ONCE(adapter->mq_fallback))
		return;

	WRITE_ONCE(adapter->mq_fallback, false);

	netdev_warn(netdev,
		    "Falling back to single RX queue (firmware MQ unavailable)\n");
	adapter->multi_queue = false;
	adapter->num_rx_queues = 1;
	if (netif_set_real_num_rx_queues(netdev, 1))
		netdev_err(netdev, "failed to set number of rx queues\n");
	if (adapter->rx_buffers_per_hcall > IBMVETH_MAX_RX_REGULAR)
		adapter->rx_buffers_per_hcall = IBMVETH_MAX_RX_REGULAR;
}

static int __ibmveth_open(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	u64 mac_address = ether_addr_to_u64(netdev->dev_addr);
	int rxq_entries = 1;
	bool skip_unmap;
	int rc;
	int i;

	netdev_dbg(netdev, "open starting\n");

	/* close() leaves LAN memory mapped when H_FREE_LOGICAL_LAN fails.
	 * Free it before apply_mq_fallback() changes the queue count, and
	 * before a new alloc overwrites those pages.
	 */
	if (ibmveth_lan_resources_live(adapter)) {
		if (ibmveth_free_all_queues(adapter)) {
			netdev_err(netdev,
				   "h_free_logical_lan still failing; LAN remains registered\n");
			return -EIO;
		}
		ibmveth_release_lan_resources(adapter);
	}

	ibmveth_apply_mq_fallback(adapter);

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		rxq_entries += adapter->rx_buff_pool[0][i].size;

	rc = ibmveth_alloc_filter_list(adapter);
	if (rc)
		goto out;

	rc = ibmveth_alloc_rx_queues(adapter, rxq_entries);
	if (rc)
		goto out_free_filter_list;

	/* Map the TX LTBs before RX buffers go to PHYP: under CMO they
	 * draw on the same IO entitlement, and only RX copes with a
	 * failed mapping.
	 */
	rc = ibmveth_alloc_tx_resources(adapter);
	if (rc)
		goto out_free_queue_mem;

	rc = ibmveth_alloc_buffer_pools(adapter);
	if (rc) {
		/* A kexec can leave the LAN registered by the old kernel
		 * (see ibmveth_register_logical_lan()); do not leave its
		 * buffers live in PHYP.
		 */
		ibmveth_free_logical_lan(adapter);
		goto out_free_tx_ltb;
	}

	rc = ibmveth_register_rx_queues(adapter, mac_address, &skip_unmap);
	if (rc) {
		if (skip_unmap) {
			/* Nothing is posted yet: PHYP never saw the pools. */
			ibmveth_free_buffer_pools(adapter);
			ibmveth_free_tx_resources(adapter);
			goto out;
		}
		goto out_free_buffer_pools;
	}

	rc = netif_set_real_num_rx_queues(netdev, adapter->num_rx_queues);
	if (rc) {
		netdev_err(netdev, "failed to set number of rx queues\n");
		goto out_unregister_queues;
	}

	/* NAPI is still disabled, so nothing else posts buffers yet. With
	 * more than one queue, setup_rx_interrupts() unmasks PHYP, so every
	 * queue needs buffers before it.
	 */
	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_replenish_task(adapter, i);

	/* A buffer add hit H_FUNCTION; ibmveth_open() retries single-queue. */
	if (READ_ONCE(adapter->mq_fallback)) {
		rc = -EOPNOTSUPP;
		goto out_unregister_queues;
	}

	/* Enables NAPI, so everything ibmveth_poll() touches must exist:
	 * netpoll during the direct close()/open() pairs, or a direct
	 * kick, can run it as soon as it is enabled.
	 */
	rc = ibmveth_setup_rx_interrupts(adapter);
	if (rc)
		goto out_unregister_queues;

	/* An idle queue with nothing pending would stay masked until the
	 * next reset; one poll per queue unmasks it.
	 */
	for (i = 0; i < adapter->num_rx_queues; i++)
		ibmveth_schedule_rx_queue(adapter, i);

	/* netif_tx_lock() takes each txq lock, so a sender that then sees
	 * DRV_XOFF clear under its txq lock also sees the LTBs set up above.
	 */
	netif_tx_lock_bh(netdev);
	netif_tx_start_all_queues(netdev);
	netif_tx_unlock_bh(netdev);

	adapter->opened = true;
	netdev_dbg(netdev, "open complete\n");

	return 0;

out_unregister_queues:
	ibmveth_dispose_subordinate_irq_mappings(adapter);
	if (ibmveth_free_all_queues(adapter)) {
		/* As in close(): PHYP has copied each frame before
		 * H_SEND_LOGICAL_LAN returns.
		 */
		ibmveth_free_tx_resources(adapter);
		goto out;
	}
out_free_buffer_pools:
	ibmveth_free_buffer_pools(adapter);
out_free_tx_ltb:
	ibmveth_free_tx_resources(adapter);
out_free_queue_mem:
	ibmveth_cleanup_rx_resources(adapter);
out_free_filter_list:
	ibmveth_free_filter_list(adapter);
out:
	return rc;
}

static int ibmveth_open(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int rc;

	rc = __ibmveth_open(netdev);
	/* Firmware refused MQ; the retry applies the fallback. A reset still
	 * pending is only needed if the retry fails.
	 */
	if (rc == -EOPNOTSUPP && READ_ONCE(adapter->mq_fallback)) {
		rc = __ibmveth_open(netdev);
		if (!rc)
			cancel_work(&adapter->work);
	}
	return rc;
}

static int ibmveth_close(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);

	/* change_mtu, pool sysfs, set_csum and set_tso call close() and
	 * open() directly. If that open() fails, IFF_UP stays set and
	 * NAPI is disabled; a second close() would hang in napi_disable().
	 */
	if (!adapter->opened)
		return 0;
	adapter->opened = false;

	netdev_dbg(netdev, "close starting\n");

	netif_tx_disable(netdev);

	ibmveth_cleanup_rx_interrupts(adapter);

	if (!ibmveth_free_all_queues(adapter))
		ibmveth_release_lan_resources(adapter);
	/* netif_tx_disable() above keeps xmit off the TX LTBs, and PHYP
	 * copies each frame before H_SEND_LOGICAL_LAN returns.
	 */
	ibmveth_free_tx_resources(adapter);

	netdev_dbg(netdev, "close complete\n");

	return 0;
}

/**
 * ibmveth_reset - Handle scheduled reset work
 *
 * @w: pointer to work_struct embedded in adapter structure
 *
 * Context: This routine acquires rtnl_mutex and, if the device is open,
 *          disables its NAPI through ibmveth_close. It can't be called
 *          directly in a context that has already acquired rtnl_mutex or
 *          disabled its NAPI, or directly from a poll routine.
 *
 * Return: void
 */
static void ibmveth_reset(struct work_struct *w)
{
	struct ibmveth_adapter *adapter = container_of(w, struct ibmveth_adapter, work);
	struct net_device *netdev = adapter->netdev;

	netdev_dbg(netdev, "reset starting\n");

	rtnl_lock();

	if (netif_running(netdev)) {
		dev_close(netdev);
		dev_open(netdev, NULL);
	}

	rtnl_unlock();

	netdev_dbg(netdev, "reset complete\n");
}

static int ibmveth_set_link_ksettings(struct net_device *dev,
				      const struct ethtool_link_ksettings *cmd)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	return ethtool_virtdev_set_link_ksettings(dev, cmd,
						  &adapter->speed,
						  &adapter->duplex);
}

static int ibmveth_get_link_ksettings(struct net_device *dev,
				      struct ethtool_link_ksettings *cmd)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	cmd->base.speed = adapter->speed;
	cmd->base.duplex = adapter->duplex;
	cmd->base.port = PORT_OTHER;

	return 0;
}

static void ibmveth_init_link_settings(struct net_device *dev)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	adapter->speed = SPEED_1000;
	adapter->duplex = DUPLEX_FULL;
}

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strscpy(info->driver, ibmveth_driver_name, sizeof(info->driver));
	strscpy(info->version, ibmveth_driver_version, sizeof(info->version));
}

static netdev_features_t ibmveth_fix_features(struct net_device *dev,
	netdev_features_t features)
{
	/*
	 * Since the ibmveth firmware interface does not have the
	 * concept of separate tx/rx checksum offload enable, if rx
	 * checksum is disabled we also have to disable tx checksum
	 * offload. Once we disable rx checksum offload, we are no
	 * longer allowed to send tx buffers that are not properly
	 * checksummed.
	 */

	if (!(features & NETIF_F_RXCSUM))
		features &= ~NETIF_F_CSUM_MASK;

	return features;
}

static int ibmveth_set_csum_offload(struct net_device *dev, u32 data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned long set_attr, clr_attr, ret_attr;
	unsigned long set_attr6, clr_attr6;
	long ret, ret4, ret6;
	int rc1 = 0, rc2 = 0;
	int restart = 0;

	if (netif_running(dev)) {
		restart = 1;
		ibmveth_close(dev);
	}

	set_attr = 0;
	clr_attr = 0;
	set_attr6 = 0;
	clr_attr6 = 0;

	if (data) {
		set_attr = IBMVETH_ILLAN_IPV4_TCP_CSUM;
		set_attr6 = IBMVETH_ILLAN_IPV6_TCP_CSUM;
	} else {
		clr_attr = IBMVETH_ILLAN_IPV4_TCP_CSUM;
		clr_attr6 = IBMVETH_ILLAN_IPV6_TCP_CSUM;
	}

	ret = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_PADDED_PKT_CSUM)) {
		ret4 = h_illan_attributes(adapter->vdev->unit_address, clr_attr,
					 set_attr, &ret_attr);

		if (ret4 != H_SUCCESS) {
			netdev_err(dev, "unable to change IPv4 checksum "
					"offload settings. %d rc=%ld\n",
					data, ret4);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr, clr_attr, &ret_attr);

			if (data == 1)
				dev->features &= ~NETIF_F_IP_CSUM;

		} else {
			adapter->fw_ipv4_csum_support = data;
		}

		ret6 = h_illan_attributes(adapter->vdev->unit_address,
					 clr_attr6, set_attr6, &ret_attr);

		if (ret6 != H_SUCCESS) {
			netdev_err(dev, "unable to change IPv6 checksum "
					"offload settings. %d rc=%ld\n",
					data, ret6);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr6, clr_attr6, &ret_attr);

			if (data == 1)
				dev->features &= ~NETIF_F_IPV6_CSUM;

		} else
			adapter->fw_ipv6_csum_support = data;

		if (ret4 == H_SUCCESS || ret6 == H_SUCCESS)
			adapter->rx_csum = data;
		else
			rc1 = -EIO;
	} else {
		rc1 = -EIO;
		netdev_err(dev, "unable to change checksum offload settings."
				     " %d rc=%ld ret_attr=%lx\n", data, ret,
				     ret_attr);
	}

	if (restart)
		rc2 = ibmveth_open(dev);

	return rc1 ? rc1 : rc2;
}

static int ibmveth_set_tso(struct net_device *dev, u32 data)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned long set_attr, clr_attr, ret_attr;
	long ret1, ret2;
	int rc1 = 0, rc2 = 0;
	int restart = 0;

	if (netif_running(dev)) {
		restart = 1;
		ibmveth_close(dev);
	}

	set_attr = 0;
	clr_attr = 0;

	if (data)
		set_attr = IBMVETH_ILLAN_LRG_SR_ENABLED;
	else
		clr_attr = IBMVETH_ILLAN_LRG_SR_ENABLED;

	ret1 = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	if (ret1 == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_LRG_SND_SUPPORT) &&
	    !old_large_send) {
		ret2 = h_illan_attributes(adapter->vdev->unit_address, clr_attr,
					  set_attr, &ret_attr);

		if (ret2 != H_SUCCESS) {
			netdev_err(dev, "unable to change tso settings. %d rc=%ld\n",
				   data, ret2);

			h_illan_attributes(adapter->vdev->unit_address,
					   set_attr, clr_attr, &ret_attr);

			if (data == 1)
				dev->features &= ~(NETIF_F_TSO | NETIF_F_TSO6);
			rc1 = -EIO;

		} else {
			adapter->fw_large_send_support = data;
			adapter->large_send = data;
		}
	} else {
		/* Older firmware version of large send offload does not
		 * support tcp6/ipv6
		 */
		if (data == 1) {
			dev->features &= ~NETIF_F_TSO6;
			netdev_info(dev, "TSO feature requires all partitions to have updated driver");
		}
		adapter->large_send = data;
	}

	if (restart)
		rc2 = ibmveth_open(dev);

	return rc1 ? rc1 : rc2;
}

static int ibmveth_set_features(struct net_device *dev,
	netdev_features_t features)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int rx_csum = !!(features & NETIF_F_RXCSUM);
	int large_send = !!(features & (NETIF_F_TSO | NETIF_F_TSO6));
	int rc1 = 0, rc2 = 0;

	if (rx_csum != adapter->rx_csum) {
		rc1 = ibmveth_set_csum_offload(dev, rx_csum);
		if (rc1 && !adapter->rx_csum)
			dev->features =
				features & ~(NETIF_F_CSUM_MASK |
					     NETIF_F_RXCSUM);
	}

	if (large_send != adapter->large_send) {
		rc2 = ibmveth_set_tso(dev, large_send);
		if (rc2 && !adapter->large_send)
			dev->features =
				features & ~(NETIF_F_TSO | NETIF_F_TSO6);
	}

	return rc1 ? rc1 : rc2;
}

/*
 * Sum every slot, not just the live queues, so shrinking the queue
 * count cannot make a counter go backwards.
 */
static u64 ibmveth_sum_rx_qstat(struct ibmveth_adapter *adapter, size_t off)
{
	u64 total = 0;
	int i;

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++) {
		const u8 *q = (const u8 *)&adapter->rx_qstats[i];

		total += READ_ONCE(*(const u64 *)(q + off));
	}

	return total;
}

static u64 ibmveth_sum_tx_qstat(struct ibmveth_adapter *adapter, size_t off)
{
	u64 total = 0;
	int i;

	for (i = 0; i < IBMVETH_MAX_QUEUES; i++) {
		const u8 *q = (const u8 *)&adapter->tx_qstats[i];

		total += READ_ONCE(*(const u64 *)(q + off));
	}

	return total;
}

static u64 ibmveth_ethtool_adapter_stat(struct ibmveth_adapter *adapter,
					int index)
{
	const struct ibmveth_stat *stat = &ibmveth_stats[index];

	switch (stat->src) {
	case IBMVETH_STAT_RX_QSUM:
		return ibmveth_sum_rx_qstat(adapter, stat->off);
	case IBMVETH_STAT_TX_QSUM:
		return ibmveth_sum_tx_qstat(adapter, stat->off);
	case IBMVETH_STAT_RX_NO_BUFFER: {
		u64 total = 0;
		int i;

		for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
			total += ibmveth_rx_no_buffer_total(adapter, i);
		return total;
	}
	case IBMVETH_STAT_ADAPTER:
		break;
	}

	return IBMVETH_GET_STAT(adapter, stat->off);
}

static void ibmveth_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++, data += ETH_GSTRING_LEN)
		memcpy(data, ibmveth_stats[i].name, ETH_GSTRING_LEN);
}

static int ibmveth_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ibmveth_stats);
	default:
		return -EOPNOTSUPP;
	}
}

static void ibmveth_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	int i;
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	for (i = 0; i < ARRAY_SIZE(ibmveth_stats); i++)
		data[i] = ibmveth_ethtool_adapter_stat(adapter, i);
}

static void ibmveth_get_channels(struct net_device *netdev,
				 struct ethtool_channels *channels)
{
	channels->max_tx = ibmveth_real_max_tx_queues();
	channels->tx_count = netdev->real_num_tx_queues;

	channels->max_rx = netdev->real_num_rx_queues;
	channels->rx_count = netdev->real_num_rx_queues;
}

static int ibmveth_set_channels(struct net_device *netdev,
				struct ethtool_channels *channels)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned int old = netdev->real_num_tx_queues,
		     goal = channels->tx_count;
	int rc, i, alloc_rc = 0;

	/*
	 * RX channel resize is not supported; reject any request that
	 * changes rx_count. Read-modify-write TX adjustments submit the
	 * current rx_count and proceed.
	 */
	if (channels->rx_count != adapter->num_rx_queues)
		return -EOPNOTSUPP;

	/* If the device is not open (including a failed close/open with
	 * IFF_UP still set) then don't allocate, just set desired
	 * netdev_queue's and return
	 */
	if (!adapter->opened)
		return netif_set_real_num_tx_queues(netdev, goal);

	/* We have IBMVETH_MAX_QUEUES netdev_queue's allocated
	 * but we may need to alloc/free the ltb's.
	 */
	netif_tx_disable(netdev);

	/* Allocate any queue that we need */
	for (i = old; i < goal; i++) {
		if (adapter->tx_ltb_ptr[i])
			continue;

		rc = ibmveth_allocate_tx_ltb(adapter, i);
		if (!rc)
			continue;

		/* if something goes wrong, free everything we just allocated */
		netdev_err(netdev, "Failed to allocate more tx queues, returning to %d queues\n",
			   old);
		alloc_rc = rc;
		goal = old;
		old = i;
		break;
	}
	rc = netif_set_real_num_tx_queues(netdev, goal);
	if (rc) {
		netdev_err(netdev, "Failed to set real tx queues, returning to %d queues\n",
			   old);
		goal = old;
		old = i;
	} else if (alloc_rc) {
		rc = alloc_rc;
	}
	/* Free any that are no longer needed */
	for (i = old; i > goal; i--) {
		if (adapter->tx_ltb_ptr[i - 1])
			ibmveth_free_tx_ltb(adapter, i - 1);
	}

	/* netif_tx_lock() takes each txq lock, so a sender that then sees
	 * DRV_XOFF clear under its txq lock also sees the LTB updates above.
	 */
	netif_tx_lock_bh(netdev);
	netif_tx_wake_all_queues(netdev);
	netif_tx_unlock_bh(netdev);

	return rc;
}

static const struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		         = netdev_get_drvinfo,
	.get_link		         = ethtool_op_get_link,
	.get_strings		         = ibmveth_get_strings,
	.get_sset_count		         = ibmveth_get_sset_count,
	.get_ethtool_stats	         = ibmveth_get_ethtool_stats,
	.get_link_ksettings	         = ibmveth_get_link_ksettings,
	.set_link_ksettings              = ibmveth_set_link_ksettings,
	.get_channels			 = ibmveth_get_channels,
	.set_channels			 = ibmveth_set_channels
};

static int ibmveth_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return -EOPNOTSUPP;
}

static int ibmveth_send(struct ibmveth_adapter *adapter,
			unsigned long desc, unsigned long mss)
{
	unsigned long correlator;
	unsigned int retry_count;
	unsigned long ret;

	/*
	 * The retry count sets a maximum for the number of broadcast and
	 * multicast destinations within the system.
	 */
	retry_count = 1024;
	correlator = 0;
	do {
		ret = h_send_logical_lan(adapter->vdev->unit_address, desc,
					 correlator, &correlator, mss,
					 adapter->fw_large_send_support);
	} while ((ret == H_BUSY) && (retry_count--));

	if (ret != H_SUCCESS && ret != H_DROPPED) {
		netdev_err(adapter->netdev, "tx: h_send_logical_lan failed "
			   "with rc=%ld\n", ret);
		return 1;
	}

	return 0;
}

static int ibmveth_is_packet_unsupported(struct sk_buff *skb,
					 struct ibmveth_adapter *adapter,
					 int queue_num)
{
	struct net_device *netdev = adapter->netdev;
	struct ethhdr *ether_header;
	int ret = 0;

	ether_header = eth_hdr(skb);

	if (ether_addr_equal(ether_header->h_dest, netdev->dev_addr)) {
		netdev_dbg(netdev, "veth doesn't support loopback packets, dropping packet.\n");
		IBMVETH_STAT_INC(adapter->tx_qstats[queue_num].dropped_packets);
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static netdev_tx_t ibmveth_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	struct ibmveth_tx_queue_stats *txs;
	unsigned int desc_flags, total_bytes;
	union ibmveth_buf_desc desc;
	int i, queue_num = skb_get_queue_mapping(skb);
	unsigned long mss = 0;
	void *ltb;

	txs = &adapter->tx_qstats[queue_num];

	/* A sender that picked its queue before ethtool -L shrank the TX
	 * queues can get here after that queue's LTB was freed.
	 */
	ltb = READ_ONCE(adapter->tx_ltb_ptr[queue_num]);
	if (unlikely(!ltb)) {
		IBMVETH_STAT_INC(txs->dropped_packets);
		goto out;
	}

	if (ibmveth_is_packet_unsupported(skb, adapter, queue_num))
		goto out;
	/* veth can't checksum offload UDP */
	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    ((skb->protocol == htons(ETH_P_IP) &&
	      ip_hdr(skb)->protocol != IPPROTO_TCP) ||
	     (skb->protocol == htons(ETH_P_IPV6) &&
	      ipv6_hdr(skb)->nexthdr != IPPROTO_TCP)) &&
	    skb_checksum_help(skb)) {

		netdev_err(netdev, "tx: failed to checksum packet\n");
		IBMVETH_STAT_INC(txs->dropped_packets);
		goto out;
	}

	desc_flags = IBMVETH_BUF_VALID;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		unsigned char *buf = skb_transport_header(skb) +
						skb->csum_offset;

		desc_flags |= (IBMVETH_BUF_NO_CSUM | IBMVETH_BUF_CSUM_GOOD);

		/* Need to zero out the checksum */
		buf[0] = 0;
		buf[1] = 0;

		if (skb_is_gso(skb) && adapter->fw_large_send_support)
			desc_flags |= IBMVETH_BUF_LRG_SND;
	}

	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_is_gso(skb)) {
		if (adapter->fw_large_send_support) {
			mss = (unsigned long)skb_shinfo(skb)->gso_size;
			IBMVETH_STAT_INC(txs->large_packets);
		} else if (!skb_is_gso_v6(skb)) {
			/* Put -1 in the IP checksum to tell phyp it
			 * is a largesend packet. Put the mss in
			 * the TCP checksum.
			 */
			ip_hdr(skb)->check = 0xffff;
			tcp_hdr(skb)->check =
				cpu_to_be16(skb_shinfo(skb)->gso_size);
			IBMVETH_STAT_INC(txs->large_packets);
		}
	}

	/* Copy header into mapped buffer */
	if (unlikely(skb->len > adapter->tx_ltb_size)) {
		netdev_err(adapter->netdev, "tx: packet size (%u) exceeds ltb (%u)\n",
			   skb->len, adapter->tx_ltb_size);
		IBMVETH_STAT_INC(txs->dropped_packets);
		goto out;
	}
	memcpy(ltb, skb->data, skb_headlen(skb));
	total_bytes = skb_headlen(skb);
	/* Copy frags into mapped buffers */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		memcpy(ltb + total_bytes,
		       skb_frag_address_safe(frag), skb_frag_size(frag));
		total_bytes += skb_frag_size(frag);
	}

	if (unlikely(total_bytes != skb->len)) {
		netdev_err(adapter->netdev, "tx: incorrect packet len copied into ltb (%u != %u)\n",
			   skb->len, total_bytes);
		IBMVETH_STAT_INC(txs->dropped_packets);
		goto out;
	}
	desc.fields.flags_len = desc_flags | skb->len;
	desc.fields.address = adapter->tx_ltb_dma[queue_num];
	/* finish writing to long_term_buff before VIOS accessing it */
	dma_wmb();

	if (ibmveth_send(adapter, desc.desc, mss)) {
		IBMVETH_STAT_INC(txs->send_failures);
		IBMVETH_STAT_INC(txs->dropped_packets);
	} else {
		IBMVETH_STAT_INC(txs->packets);
		IBMVETH_STAT_ADD(txs->bytes, skb->len);
	}

out:
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;


}

/* Returns false when the IP or TCP headers fail the checks; the frame is
 * then left unchanged and not marked GSO.
 */
static bool ibmveth_rx_mss_helper(struct sk_buff *skb, u16 mss, int lrg_pkt)
{
	unsigned int gso_type, ip_len;
	struct tcphdr *tcph;
	int offset = 0;
	int hdr_len;

	/* only TCP packets will be aggregated */
	if (skb->protocol == htons(ETH_P_IP)) {
		struct iphdr *iph = (struct iphdr *)skb->data;

		if (skb_headlen(skb) < sizeof(*iph) || iph->ihl < 5)
			return false;
		if (iph->protocol == IPPROTO_TCP) {
			/* only a whole datagram has a TCP header to segment */
			if (ip_is_fragment(iph))
				return false;
			offset = iph->ihl * 4;
			ip_len = ntohs(iph->tot_len);
			gso_type = SKB_GSO_TCPV4;
		} else {
			return true;
		}
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		struct ipv6hdr *iph6 = (struct ipv6hdr *)skb->data;

		if (skb_headlen(skb) < sizeof(*iph6))
			return false;
		if (iph6->nexthdr == IPPROTO_TCP) {
			offset = sizeof(struct ipv6hdr);
			ip_len = offset + ntohs(iph6->payload_len);
			gso_type = SKB_GSO_TCPV6;
		} else {
			return true;
		}
	} else {
		return true;
	}
	/* if mss is not set through Large Packet bit/mss in rx buffer,
	 * expect that the mss will be written to the tcp header checksum.
	 */
	tcph = (struct tcphdr *)(skb->data + offset);
	if (skb_headlen(skb) < offset + sizeof(*tcph) || tcph->doff < 5 ||
	    ip_len != skb_headlen(skb) || ip_len <= offset + tcph->doff * 4)
		return false;
	skb_shinfo(skb)->gso_type = gso_type;
	if (lrg_pkt) {
		skb_shinfo(skb)->gso_size = mss;
	} else if (offset) {
		skb_shinfo(skb)->gso_size = ntohs(tcph->check);
		tcph->check = 0;
	}

	if (skb_shinfo(skb)->gso_size) {
		/* chosen by the sending partition: at least the floor
		 * virtio_net_hdr_to_skb() uses, and never GSO_BY_FRAGS
		 */
		skb_shinfo(skb)->gso_size =
			clamp_t(u16, skb_shinfo(skb)->gso_size,
				TCP_MIN_GSO_SIZE, GSO_BY_FRAGS - 1);
		hdr_len = offset + tcph->doff * 4;
		skb_shinfo(skb)->gso_segs =
				DIV_ROUND_UP(ip_len - hdr_len,
					     skb_shinfo(skb)->gso_size);
	}
	return true;
}

static void ibmveth_rx_csum_helper(struct sk_buff *skb,
				   struct ibmveth_adapter *adapter)
{
	struct iphdr *iph = NULL;
	struct ipv6hdr *iph6 = NULL;
	__be16 skb_proto = 0;
	u16 iphlen = 0;
	u16 iph_proto = 0;
	u16 tcphdrlen = 0;

	skb_proto = be16_to_cpu(skb->protocol);

	if (skb_proto == ETH_P_IP) {
		iph = (struct iphdr *)skb->data;
		if (skb_headlen(skb) < sizeof(*iph) || iph->ihl < 5 ||
		    skb_headlen(skb) < iph->ihl * 4 ||
		    (iph->protocol == IPPROTO_TCP && ip_is_fragment(iph)))
			goto bad_hdr;

		/* If the IP checksum is not offloaded and if the packet
		 *  is large send, the checksum must be rebuilt.
		 */
		if (iph->check == 0xffff) {
			iph->check = 0;
			iph->check = ip_fast_csum((unsigned char *)iph,
						  iph->ihl);
		}

		iphlen = iph->ihl * 4;
		iph_proto = iph->protocol;
	} else if (skb_proto == ETH_P_IPV6) {
		iph6 = (struct ipv6hdr *)skb->data;
		if (skb_headlen(skb) < sizeof(*iph6))
			goto bad_hdr;
		iphlen = sizeof(struct ipv6hdr);
		iph_proto = iph6->nexthdr;
	}

	/* When CSO is enabled the TCP checksum may have be set to NULL by
	 * the sender given that we zeroed out TCP checksum field in
	 * transmit path (refer ibmveth_start_xmit routine). In this case set
	 * up CHECKSUM_PARTIAL. If the packet is forwarded, the checksum will
	 * then be recalculated by the destination NIC (CSO must be enabled
	 * on the destination NIC).
	 *
	 * In an OVS environment, when a flow is not cached, specifically for a
	 * new TCP connection, the first packet information is passed up to
	 * the user space for finding a flow. During this process, OVS computes
	 * checksum on the first packet when CHECKSUM_PARTIAL flag is set.
	 *
	 * So, re-compute TCP pseudo header checksum.
	 */

	if (iph_proto == IPPROTO_TCP) {
		struct tcphdr *tcph = (struct tcphdr *)(skb->data + iphlen);

		if (skb_headlen(skb) < iphlen + sizeof(*tcph))
			goto bad_hdr;
		if (tcph->check == 0x0000) {
			/* Recompute TCP pseudo header checksum  */
			tcphdrlen = skb->len - iphlen;
			if (skb_proto == ETH_P_IP)
				tcph->check =
				 ~csum_tcpudp_magic(iph->saddr,
				iph->daddr, tcphdrlen, iph_proto, 0);
			else if (skb_proto == ETH_P_IPV6)
				tcph->check =
				 ~csum_ipv6_magic(&iph6->saddr,
				&iph6->daddr, tcphdrlen, iph_proto, 0);
			/* Setup SKB fields for checksum offload */
			skb_partial_csum_set(skb, iphlen,
					     offsetof(struct tcphdr, check));
			skb_reset_network_header(skb);
		}
	}
	return;

bad_hdr:
	/* headers do not fit the frame, or a fragment has no whole TCP
	 * segment; let the stack check it
	 */
	skb->ip_summed = CHECKSUM_NONE;
}

/* Returns true if the slot could not be recycled; poll then counts it
 * against the budget.
 */
static bool ibmveth_poll_recycle_invalid(struct net_device *netdev,
					 struct ibmveth_adapter *adapter,
					 int queue_index, u64 correlator)
{
	int rc;

	IBMVETH_STAT_INC(adapter->rx_qstats[queue_index].invalid_buffers);
	netdev_dbg(netdev, "recycling invalid buffer\n");
	rc = ibmveth_rxq_harvest_buffer(adapter, queue_index, correlator,
					true);
	if (unlikely(rc))
		IBMVETH_STAT_INC(adapter->rx_qstats[queue_index].dropped);
	return rc;
}

static void ibmveth_poll_drop_bad_frame(struct net_device *netdev,
					struct ibmveth_adapter *adapter,
					int queue_index, unsigned int off,
					unsigned int len, unsigned int room,
					u64 correlator)
{
	if (net_ratelimit())
		netdev_err(netdev,
			   "bad RX frame offset %u length %u (buffer %u) on queue %d, dropping\n",
			   off, len, room, queue_index);
	ibmveth_rxq_harvest_buffer(adapter, queue_index, correlator, true);
	IBMVETH_STAT_INC(adapter->rx_qstats[queue_index].dropped);
}

/**
 * ibmveth_poll_deliver_frame - Build SKB from one valid RX slot and GRO it
 * @napi: NAPI context for this RX queue
 * @adapter: ibmveth adapter
 * @netdev: net_device for @adapter
 * @queue_index: RX queue index
 * @correlator: correlator of the current rx queue entry, as read by poll
 */
static void ibmveth_poll_deliver_frame(struct napi_struct *napi,
				       struct ibmveth_adapter *adapter,
				       struct net_device *netdev,
				       int queue_index, u64 correlator)
{
	struct ibmveth_rx_queue_stats *rxs = &adapter->rx_qstats[queue_index];
	int length = ibmveth_rxq_frame_length(adapter, queue_index);
	int offset = ibmveth_rxq_frame_offset(adapter, queue_index);
	int csum_good = ibmveth_rxq_csum_good(adapter, queue_index);
	int lrg_pkt = ibmveth_rxq_large_packet(adapter, queue_index);
	unsigned int pool, room, off, len;
	struct ibmveth_buff_pool *bpool;
	struct sk_buff *skb, *new_skb;
	__sum16 iph_check = 0;
	u16 mss = 0;
	int rc;

	skb = ibmveth_rxq_get_buffer(adapter, queue_index, correlator);
	if (unlikely(!skb)) {
		ibmveth_rxq_advance(adapter, queue_index);
		IBMVETH_STAT_INC(rxs->dropped);
		return;
	}

	pool = correlator >> 32;
	bpool = &adapter->rx_buff_pool[queue_index][pool];
	room = min_t(unsigned int, skb_tailroom(skb), bpool->buff_size);
	off = offset;
	len = length;
	if (unlikely(len < ETH_HLEN || off >= room || len > room - off)) {
		ibmveth_poll_drop_bad_frame(netdev, adapter, queue_index,
					    off, len, room, correlator);
		return;
	}

	/* if the large packet bit is set in the rx queue
	 * descriptor, the mss will be written by PHYP eight
	 * bytes from the start of the rx buffer, which is
	 * skb->data at this stage
	 */
	if (lrg_pkt) {
		__be64 *rxmss = (__be64 *)(skb->data + 8);

		mss = (u16)be64_to_cpu(*rxmss);
	}

	new_skb = NULL;
	if (length < rx_copybreak)
		new_skb = netdev_alloc_skb(netdev, length);

	if (new_skb) {
		skb_copy_to_linear_data(new_skb, skb->data + offset, length);
		if (rx_flush)
			ibmveth_flush_buffer(skb->data, length + offset);
		rc = ibmveth_rxq_harvest_buffer(adapter, queue_index,
						correlator, true);
		if (unlikely(rc)) {
			dev_kfree_skb_any(new_skb);
			IBMVETH_STAT_INC(rxs->dropped);
			return;
		}
		skb = new_skb;
	} else {
		rc = ibmveth_rxq_harvest_buffer(adapter, queue_index,
						correlator, false);
		if (unlikely(rc)) {
			IBMVETH_STAT_INC(rxs->dropped);
			return;
		}
		skb_reserve(skb, offset);
	}

	skb_put(skb, length);
	skb->protocol = eth_type_trans(skb, netdev);

	/* PHYP without PLSO support places a -1 in the ip
	 * checksum for large send frames.
	 */
	if (skb->protocol == cpu_to_be16(ETH_P_IP) &&
	    skb_headlen(skb) >= sizeof(struct iphdr)) {
		struct iphdr *iph = (struct iphdr *)skb->data;

		iph_check = iph->check;
	}

	if ((length > netdev->mtu + ETH_HLEN) ||
	    lrg_pkt || iph_check == 0xffff) {
		if (!ibmveth_rx_mss_helper(skb, mss, lrg_pkt))
			csum_good = 0;
		IBMVETH_STAT_INC(rxs->large_packets);
	}

	if (csum_good) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		ibmveth_rx_csum_helper(skb, adapter);
	}

	skb_record_rx_queue(skb, queue_index);
	napi_gro_receive(napi, skb);	/* send it up */

	IBMVETH_STAT_INC(rxs->packets);
	IBMVETH_STAT_ADD(rxs->bytes, length);
}

static int ibmveth_poll(struct napi_struct *napi, int budget)
{
	struct net_device *netdev = napi->dev;
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	int frames_processed = 0;
	int queue_index;
	u64 correlator;
	int rc;

	queue_index = napi - adapter->napi;

	while (frames_processed < budget) {
		if (!ibmveth_rxq_pending_buffer(adapter, queue_index))
			break;

		smp_rmb();
		correlator = ibmveth_rxq_correlator(adapter, queue_index);
		if (!ibmveth_rxq_buffer_valid(adapter, queue_index)) {
			wmb(); /* suggested by larson1 */
			if (ibmveth_poll_recycle_invalid(netdev, adapter,
							 queue_index,
							 correlator))
				frames_processed++;
		} else {
			ibmveth_poll_deliver_frame(napi, adapter, netdev,
						   queue_index, correlator);
			frames_processed++;
		}
	}

	/* netpoll polls with budget 0 for TX only, and is not serialized
	 * with a NAPI poll that was already running when it was set up
	 */
	if (budget)
		ibmveth_replenish_task(adapter, queue_index);

	if (frames_processed == budget)
		goto out;

	if (!napi_complete_done(napi, frames_processed))
		goto out;

	/* We think we are done - reenable interrupts,
	 * then check once more to make sure we are done.
	 */
	rc = ibmveth_enable_irq(adapter, queue_index);
	if (rc) {
		schedule_work(&adapter->work);
		goto out;
	}

	/* Mask before queueing, as ibmveth_schedule_rx_queue() does: a
	 * threaded NAPI can start polling, and unmask, as soon as it is
	 * queued.
	 */
	if (ibmveth_rxq_pending_buffer(adapter, queue_index) &&
	    napi_schedule_prep(napi)) {
		ibmveth_disable_irq(adapter, queue_index);
		__napi_schedule(napi);
	}

out:
	return frames_processed;
}

static irqreturn_t ibmveth_interrupt(int irq, void *dev_instance)
{
	struct napi_struct *napi = dev_instance;
	struct net_device *netdev = napi->dev;
	struct ibmveth_adapter *adapter = netdev_priv(netdev);

	ibmveth_schedule_rx_queue(adapter, napi - adapter->napi);
	return IRQ_HANDLED;
}

static void ibmveth_set_multicast_list(struct net_device *netdev)
{
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned long lpar_rc;

	if ((netdev->flags & IFF_PROMISC) ||
	    (netdev_mc_count(netdev) > adapter->mcastFilterSize)) {
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableRecv |
					   IbmVethMcastDisableFiltering,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "entering promisc mode\n", lpar_rc);
		}
	} else {
		struct netdev_hw_addr *ha;
		/* clear the filter table & disable filtering */
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableRecv |
					   IbmVethMcastDisableFiltering |
					   IbmVethMcastClearFilterTable,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "attempting to clear filter table\n",
				   lpar_rc);
		}
		/* add the addresses to the filter table */
		netdev_for_each_mc_addr(ha, netdev) {
			/* add the multicast address to the filter table */
			u64 mcast_addr;
			mcast_addr = ether_addr_to_u64(ha->addr);
			lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
						   IbmVethMcastAddFilter,
						   mcast_addr);
			if (lpar_rc != H_SUCCESS) {
				netdev_err(netdev, "h_multicast_ctrl rc=%ld "
					   "when adding an entry to the filter "
					   "table\n", lpar_rc);
			}
		}

		/* re-enable filtering */
		lpar_rc = h_multicast_ctrl(adapter->vdev->unit_address,
					   IbmVethMcastEnableFiltering,
					   0);
		if (lpar_rc != H_SUCCESS) {
			netdev_err(netdev, "h_multicast_ctrl rc=%ld when "
				   "enabling filtering\n", lpar_rc);
		}
	}
}

static int ibmveth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	struct vio_dev *viodev = adapter->vdev;
	int new_mtu_oh = new_mtu + IBMVETH_BUFF_OH;
	int i, rc;
	int need_restart = 0;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		if (new_mtu_oh <= adapter->rx_buff_pool[0][i].buff_size)
			break;

	if (i == IBMVETH_NUM_BUFF_POOLS)
		return -EINVAL;

	/* Deactivate all the buffer pools so that the next loop can activate
	   only the buffer pools necessary to hold the new MTU */
	if (netif_running(adapter->netdev)) {
		need_restart = 1;
		ibmveth_close(adapter->netdev);
	}

	/* Look for an active buffer pool that can hold the new MTU */
	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		adapter->rx_buff_pool[0][i].active = 1;

		if (new_mtu_oh <= adapter->rx_buff_pool[0][i].buff_size) {
			WRITE_ONCE(dev->mtu, new_mtu);
			vio_cmo_set_dev_desired(viodev,
						ibmveth_get_desired_dma
						(viodev));
			if (need_restart) {
				return ibmveth_open(adapter->netdev);
			}
			return 0;
		}
	}

	if (need_restart && (rc = ibmveth_open(adapter->netdev)))
		return rc;

	return -EINVAL;
}

/**
 * ibmveth_get_desired_dma - Calculate IO memory desired by the driver
 *
 * @vdev: struct vio_dev for the device whose desired IO mem is to be returned
 *
 * Return value:
 *	Number of bytes of IO data the driver will need to perform well.
 */
static unsigned long ibmveth_get_desired_dma(struct vio_dev *vdev)
{
	struct net_device *netdev = dev_get_drvdata(&vdev->dev);
	struct ibmveth_adapter *adapter;
	struct iommu_table *tbl;
	unsigned long ret;
	int i;
	int rxqentries = 1;

	tbl = get_iommu_table_base(&vdev->dev);

	/* netdev inits at probe time along with the structures we need below*/
	if (netdev == NULL)
		return IOMMU_PAGE_ALIGN(IBMVETH_IO_ENTITLEMENT_DEFAULT, tbl);

	adapter = netdev_priv(netdev);

	ret = IBMVETH_BUFF_LIST_SIZE + IBMVETH_FILT_LIST_SIZE;
	ret += IOMMU_PAGE_ALIGN(netdev->mtu, tbl);
	/* add size of mapped tx buffers */
	ret += IOMMU_PAGE_ALIGN(IBMVETH_MAX_TX_BUF_SIZE, tbl);

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		/* add the size of the active receive buffers */
		if (adapter->rx_buff_pool[0][i].active)
			ret +=
			    adapter->rx_buff_pool[0][i].size *
			    IOMMU_PAGE_ALIGN(adapter->rx_buff_pool[0][i].
					     buff_size, tbl);
		rxqentries += adapter->rx_buff_pool[0][i].size;
	}
	/* add the size of the receive queue entries */
	ret += IOMMU_PAGE_ALIGN(
		rxqentries * sizeof(struct ibmveth_rx_q_entry), tbl);

	return ret;
}

static int ibmveth_set_mac_addr(struct net_device *dev, void *p)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	struct sockaddr *addr = p;
	u64 mac_address;
	int rc;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	mac_address = ether_addr_to_u64(addr->sa_data);
	rc = h_change_logical_lan_mac(adapter->vdev->unit_address, mac_address);
	if (rc) {
		netdev_err(adapter->netdev, "h_change_logical_lan_mac failed with rc=%d\n", rc);
		return rc;
	}

	eth_hw_addr_set(dev, addr->sa_data);

	return 0;
}

static netdev_features_t ibmveth_features_check(struct sk_buff *skb,
						struct net_device *dev,
						netdev_features_t features)
{
	/* Some physical adapters do not support segmentation offload with
	 * MSS < 224. Disable GSO for such packets to avoid adapter freeze.
	 * Note: Single-segment packets (gso_segs == 1) don't need this check
	 * as they bypass the LSO path and are transmitted without segmentation.
	 */
	if (skb_is_gso(skb)) {
		if (skb_shinfo(skb)->gso_size < IBMVETH_MIN_LSO_MSS) {
			netdev_warn_once(dev,
					 "MSS %u too small for LSO, disabling GSO\n",
					 skb_shinfo(skb)->gso_size);
			features &= ~NETIF_F_GSO_MASK;
		}
	}

	return vlan_features_check(skb, features);
}

/**
 * ibmveth_get_stats64 - Return aggregated per-queue statistics
 * @dev: network device
 * @stats: rtnl link statistics storage
 *
 * Sums per-queue rx_qstats and tx_qstats into the rtnl counters.
 * rx_missed_errors is the PHYP no-buffer pair (live + retired),
 * which if_link.h counts separately from rx_dropped.
 * Walk the full allocated arrays (not the live queue count) so shrinking
 * channels cannot make the totals go backwards.
 */
static void ibmveth_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	int i;

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++) {
		stats->rx_packets += READ_ONCE(adapter->rx_qstats[i].packets);
		stats->rx_bytes += READ_ONCE(adapter->rx_qstats[i].bytes);
		stats->rx_dropped += READ_ONCE(adapter->rx_qstats[i].dropped);
		stats->rx_missed_errors +=
			ibmveth_rx_no_buffer_total(adapter, i);
	}

	for (i = 0; i < IBMVETH_MAX_QUEUES; i++) {
		stats->tx_packets += READ_ONCE(adapter->tx_qstats[i].packets);
		stats->tx_bytes += READ_ONCE(adapter->tx_qstats[i].bytes);
		stats->tx_dropped +=
			READ_ONCE(adapter->tx_qstats[i].dropped_packets);
	}
}

static void ibmveth_get_queue_stats_rx(struct net_device *dev, int idx,
				       struct netdev_queue_stats_rx *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	u64 no_buffer;

	stats->packets = READ_ONCE(adapter->rx_qstats[idx].packets);
	stats->bytes = READ_ONCE(adapter->rx_qstats[idx].bytes);
	/* Frames PHYP dropped for lack of a posted buffer, on the live
	 * page and on pages this queue already released.
	 */
	no_buffer = ibmveth_rx_no_buffer_total(adapter, idx);
	stats->hw_drops = no_buffer;
	stats->hw_drop_overruns = no_buffer;
	stats->alloc_fail = READ_ONCE(adapter->rx_qstats[idx].replenish_no_mem);
}

static void ibmveth_get_queue_stats_tx(struct net_device *dev, int idx,
				       struct netdev_queue_stats_tx *stats)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);

	stats->packets = READ_ONCE(adapter->tx_qstats[idx].packets);
	stats->bytes = READ_ONCE(adapter->tx_qstats[idx].bytes);
	/* TX hw_drops left unset: dropped_packets includes
	 * software xmit rejects that never reached PHYP.
	 */
}

/**
 * ibmveth_get_base_stats - account for traffic not on a live queue
 * @dev: network device
 * @rx: RX base statistics storage
 * @tx: TX base statistics storage
 *
 * The core sums get_queue_stats_{rx,tx}() only below
 * real_num_{rx,tx}_queues, while ibmveth_get_stats64() walks every
 * slot. Report the slots at or above real_num here so the two agree
 * after the queue count goes down.
 *
 * Every field the queue callbacks set must be set here too: the core
 * drops a field from the device total if the base leaves it unset.
 */
static void ibmveth_get_base_stats(struct net_device *dev,
				   struct netdev_queue_stats_rx *rx,
				   struct netdev_queue_stats_tx *tx)
{
	struct ibmveth_adapter *adapter = netdev_priv(dev);
	unsigned int i;

	rx->packets = 0;
	rx->bytes = 0;
	rx->alloc_fail = 0;
	rx->hw_drops = 0;
	rx->hw_drop_overruns = 0;
	tx->packets = 0;
	tx->bytes = 0;

	for (i = dev->real_num_rx_queues; i < IBMVETH_MAX_RX_QUEUES; i++) {
		u64 no_buffer = ibmveth_rx_no_buffer_total(adapter, i);

		rx->packets += READ_ONCE(adapter->rx_qstats[i].packets);
		rx->bytes += READ_ONCE(adapter->rx_qstats[i].bytes);
		rx->hw_drops += no_buffer;
		rx->hw_drop_overruns += no_buffer;
		rx->alloc_fail +=
			READ_ONCE(adapter->rx_qstats[i].replenish_no_mem);
	}

	for (i = dev->real_num_tx_queues; i < IBMVETH_MAX_QUEUES; i++) {
		tx->packets += READ_ONCE(adapter->tx_qstats[i].packets);
		tx->bytes += READ_ONCE(adapter->tx_qstats[i].bytes);
	}
}

static const struct netdev_stat_ops ibmveth_stat_ops = {
	.get_queue_stats_rx	= ibmveth_get_queue_stats_rx,
	.get_queue_stats_tx	= ibmveth_get_queue_stats_tx,
	.get_base_stats		= ibmveth_get_base_stats,
};

static const struct net_device_ops ibmveth_netdev_ops = {
	.ndo_open		= ibmveth_open,
	.ndo_stop		= ibmveth_close,
	.ndo_start_xmit		= ibmveth_start_xmit,
	.ndo_set_rx_mode	= ibmveth_set_multicast_list,
	.ndo_eth_ioctl		= ibmveth_ioctl,
	.ndo_change_mtu		= ibmveth_change_mtu,
	.ndo_fix_features	= ibmveth_fix_features,
	.ndo_set_features	= ibmveth_set_features,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address    = ibmveth_set_mac_addr,
	.ndo_features_check	= ibmveth_features_check,
	.ndo_get_stats64	= ibmveth_get_stats64,
};

/**
 * ibmveth_pool_kobj_release - Mark a pool kobject finished
 * @kobj: kobject embedded in the pool
 *
 * The pool kobjects live in netdev_priv(), so the last put must wait
 * for this before free_netdev().
 */
static void ibmveth_pool_kobj_release(struct kobject *kobj)
{
	struct ibmveth_buff_pool *pool = container_of(kobj,
						      struct ibmveth_buff_pool,
						      kobj);

	complete(&pool->released);
}

/**
 * ibmveth_put_pool_kobjs - Drop the pool kobjects and wait for release
 * @adapter: ibmveth adapter
 *
 * With CONFIG_DEBUG_KOBJECT_RELEASE the cleanup, including removing the
 * sysfs files, runs later from a work item in the kobject; wait for it
 * so free_netdev() cannot free the pools first.
 */
static void ibmveth_put_pool_kobjs(struct ibmveth_adapter *adapter)
{
	int i;

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		kobject_put(&adapter->rx_buff_pool[0][i].kobj);
	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		wait_for_completion(&adapter->rx_buff_pool[0][i].released);
}

static int ibmveth_probe(struct vio_dev *dev, const struct vio_device_id *id)
{
	int rc, i, mac_len;
	struct net_device *netdev;
	struct ibmveth_adapter *adapter;
	unsigned char *mac_addr_p;
	__be32 *mcastFilterSize_p;
	long ret;
	unsigned long ret_attr;

	dev_dbg(&dev->dev, "entering ibmveth_probe for UA 0x%x\n",
		dev->unit_address);

	mac_addr_p = (unsigned char *)vio_get_attribute(dev, VETH_MAC_ADDR,
							&mac_len);
	if (!mac_addr_p) {
		dev_err(&dev->dev, "Can't find VETH_MAC_ADDR attribute\n");
		return -EINVAL;
	}
	/* Workaround for old/broken pHyp */
	if (mac_len == 8)
		mac_addr_p += 2;
	else if (mac_len != 6) {
		dev_err(&dev->dev, "VETH_MAC_ADDR attribute wrong len %d\n",
			mac_len);
		return -EINVAL;
	}

	mcastFilterSize_p = (__be32 *)vio_get_attribute(dev,
							VETH_MCAST_FILTER_SIZE,
							NULL);
	if (!mcastFilterSize_p) {
		dev_err(&dev->dev, "Can't find VETH_MCAST_FILTER_SIZE "
			"attribute\n");
		return -EINVAL;
	}

	netdev = alloc_etherdev_mqs(sizeof(struct ibmveth_adapter),
				    IBMVETH_MAX_QUEUES, IBMVETH_MAX_RX_QUEUES);
	if (!netdev)
		return -ENOMEM;

	adapter = netdev_priv(netdev);
	dev_set_drvdata(&dev->dev, netdev);

	adapter->vdev = dev;
	adapter->netdev = netdev;
	INIT_WORK(&adapter->work, ibmveth_reset);
	adapter->mcastFilterSize = be32_to_cpu(*mcastFilterSize_p);
	ibmveth_init_link_settings(netdev);

	for (i = 0; i < IBMVETH_MAX_RX_QUEUES; i++)
		netif_napi_add_weight(netdev, &adapter->napi[i],
				      ibmveth_poll, 16);

	netdev->irq = dev->irq;
	netdev->netdev_ops = &ibmveth_netdev_ops;
	netdev->ethtool_ops = &netdev_ethtool_ops;
	netdev->stat_ops = &ibmveth_stat_ops;
	SET_NETDEV_DEV(netdev, &dev->dev);
	netdev->hw_features = NETIF_F_SG;
	if (vio_get_attribute(dev, "ibm,illan-options", NULL) != NULL) {
		netdev->hw_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
				       NETIF_F_RXCSUM;
	}

	netdev->features |= netdev->hw_features;

	ret = h_illan_attributes(adapter->vdev->unit_address, 0, 0, &ret_attr);

	/* If running older firmware, TSO should not be enabled by default */
	if (ret == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_LRG_SND_SUPPORT) &&
	    !old_large_send) {
		netdev->hw_features |= NETIF_F_TSO | NETIF_F_TSO6;
		netdev->features |= netdev->hw_features;
	} else {
		netdev->hw_features |= NETIF_F_TSO;
	}

	adapter->is_active_trunk = false;
	if (ret == H_SUCCESS && (ret_attr & IBMVETH_ILLAN_ACTIVE_TRUNK)) {
		adapter->is_active_trunk = true;
		netdev->hw_features |= NETIF_F_FRAGLIST;
		netdev->features |= NETIF_F_FRAGLIST;
	}

	/* MQ can keep LAN memory mapped after a hard H_FREE failure;
	 * CMO BUG()s on outstanding IO entitlement at remove.
	 */
	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_RX_MULTI_QUEUE_SUPPORT) &&
	    !firmware_has_feature(FW_FEATURE_CMO)) {
		adapter->multi_queue = true;
		adapter->num_rx_queues = min(num_online_cpus(),
					     IBMVETH_DEFAULT_QUEUES);
		netdev_dbg(netdev, "RX multi queue mode enabled: %u queues\n",
			   adapter->num_rx_queues);
	} else {
		adapter->multi_queue = false;
		adapter->num_rx_queues = IBMVETH_DEFAULT_RX_QUEUES;
	}

	if (ret == H_SUCCESS &&
	    (ret_attr & IBMVETH_ILLAN_RX_MULTI_BUFF_SUPPORT)) {
		if (adapter->multi_queue)
			adapter->rx_buffers_per_hcall =
				IBMVETH_MAX_RX_PER_HCALL;
		else
			adapter->rx_buffers_per_hcall = IBMVETH_MAX_RX_REGULAR;
		netdev_dbg(netdev,
			   "RX Multi-buffer hcall supported by FW, batch set to %u\n",
			    adapter->rx_buffers_per_hcall);
	} else {
		adapter->rx_buffers_per_hcall = 1;
		netdev_dbg(netdev,
			   "RX Single-buffer hcall mode, batch set to %u\n",
			   adapter->rx_buffers_per_hcall);
	}

	netdev->min_mtu = IBMVETH_MIN_MTU;
	netdev->max_mtu = ETH_MAX_MTU - IBMVETH_BUFF_OH;

	eth_hw_addr_set(netdev, mac_addr_p);

	if (firmware_has_feature(FW_FEATURE_CMO))
		memcpy(pool_count, pool_count_cmo, sizeof(pool_count));

	for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
		struct kobject *kobj = &adapter->rx_buff_pool[0][i].kobj;
		int error;

		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);
		init_completion(&adapter->rx_buff_pool[0][i].released);
		error = kobject_init_and_add(kobj, &ktype_veth_pool,
					     &dev->dev.kobj, "pool%d", i);
		if (!error)
			kobject_uevent(kobj, KOBJ_ADD);
	}

	if (ibmveth_alloc_rx_qstats(adapter) ||
	    ibmveth_alloc_tx_qstats(adapter)) {
		rc = -ENOMEM;
		goto err_put_pools;
	}

	rc = netif_set_real_num_tx_queues(netdev, min(num_online_cpus(),
						      IBMVETH_DEFAULT_QUEUES));
	if (rc) {
		netdev_dbg(netdev, "failed to set number of tx queues rc=%d\n",
			   rc);
		goto err_put_pools;
	}

	/* alloc_etherdev_mqs() set real_num_rx_queues to the maximum. */
	rc = netif_set_real_num_rx_queues(netdev, adapter->num_rx_queues);
	if (rc) {
		netdev_dbg(netdev, "failed to set number of rx queues rc=%d\n",
			   rc);
		goto err_put_pools;
	}

	adapter->tx_ltb_size = PAGE_ALIGN(IBMVETH_MAX_TX_BUF_SIZE);
	for (i = 0; i < IBMVETH_MAX_QUEUES; i++)
		adapter->tx_ltb_ptr[i] = NULL;

	netdev_dbg(netdev, "adapter @ 0x%p\n", adapter);
	netdev_dbg(netdev, "registering netdev...\n");

	ibmveth_set_features(netdev, netdev->features);

	rc = register_netdev(netdev);

	if (rc) {
		netdev_dbg(netdev, "failed to register netdev rc=%d\n", rc);
		goto err_put_pools;
	}

	netdev_dbg(netdev, "registered\n");

	return 0;

err_put_pools:
	ibmveth_put_pool_kobjs(adapter);
	ibmveth_free_tx_qstats(adapter);
	ibmveth_free_rx_qstats(adapter);
	free_netdev(netdev);
	return rc;
}

static void ibmveth_remove(struct vio_dev *dev)
{
	struct net_device *netdev = dev_get_drvdata(&dev->dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);

	disable_work_sync(&adapter->work);

	ibmveth_put_pool_kobjs(adapter);

	unregister_netdev(netdev);

	if (ibmveth_lan_resources_live(adapter)) {
		if (ibmveth_free_all_queues(adapter))
			dev_err(&dev->dev,
				"h_free_logical_lan still failing at remove; leaking LAN buffers\n");
		else
			ibmveth_release_lan_resources(adapter);
	}

	ibmveth_free_tx_qstats(adapter);
	ibmveth_free_rx_qstats(adapter);

	free_netdev(netdev);
	dev_set_drvdata(&dev->dev, NULL);
}

static struct attribute veth_active_attr;
static struct attribute veth_num_attr;
static struct attribute veth_size_attr;

static ssize_t veth_pool_show(struct kobject *kobj,
			      struct attribute *attr, char *buf)
{
	struct ibmveth_buff_pool *pool = container_of(kobj,
						      struct ibmveth_buff_pool,
						      kobj);

	if (attr == &veth_active_attr)
		return sprintf(buf, "%d\n", pool->active);
	else if (attr == &veth_num_attr)
		return sprintf(buf, "%d\n", pool->size);
	else if (attr == &veth_size_attr)
		return sprintf(buf, "%d\n", pool->buff_size);
	return 0;
}

/**
 * veth_pool_store - sysfs store handler for pool attributes
 * @kobj: kobject embedded in pool
 * @attr: attribute being changed
 * @buf: value being stored
 * @count: length of @buf in bytes
 *
 * Stores new value in pool attribute. Verifies the range of the new value for
 * size and buff_size. Verifies that at least one pool remains available to
 * receive MTU-sized packets.
 *
 * Context: Process context.
 *          Takes and releases rtnl_mutex to ensure correct ordering of close
 *	    and open calls.
 * Return:
 * * %-EPERM  - Not allowed to disabled all MTU-sized buffer pools
 * * %-EINVAL - New pool size or buffer size is out of range
 * * %-EBUSY  - The old pools are still mapped after a failed
 *              H_FREE_LOGICAL_LAN; nothing was changed
 * * count    - Return count for success
 * * other    - Return value from a failed ibmveth_open call; the new
 *              geometry stays if that left its pools mapped
 */
static ssize_t veth_pool_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct ibmveth_buff_pool *pool = container_of(kobj,
						      struct ibmveth_buff_pool,
						      kobj);
	struct net_device *netdev = dev_get_drvdata(kobj_to_dev(kobj->parent));
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	long value = simple_strtol(buf, NULL, 10);
	bool change = false;
	u32 newbuff_size;
	u32 oldbuff_size;
	int newactive;
	int oldactive;
	u32 newsize;
	u32 oldsize;
	long rc;

	rtnl_lock();

	oldbuff_size = pool->buff_size;
	oldactive = pool->active;
	oldsize = pool->size;

	newbuff_size = oldbuff_size;
	newactive = oldactive;
	newsize = oldsize;

	if (attr == &veth_active_attr) {
		if (value && !oldactive) {
			newactive = 1;
			change = true;
		} else if (!value && oldactive) {
			int mtu = netdev->mtu + IBMVETH_BUFF_OH;
			int i;
			/* Make sure there is a buffer pool with buffers that
			   can hold a packet of the size of the MTU */
			for (i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++) {
				if (pool == &adapter->rx_buff_pool[0][i])
					continue;
				if (!adapter->rx_buff_pool[0][i].active)
					continue;
				if (mtu <=
				    adapter->rx_buff_pool[0][i].buff_size)
					break;
			}

			if (i == IBMVETH_NUM_BUFF_POOLS) {
				netdev_err(netdev, "no active pool >= MTU\n");
				rc = -EPERM;
				goto unlock_err;
			}

			newactive = 0;
			change = true;
		}
	} else if (attr == &veth_num_attr) {
		if (value <= 0 || value > IBMVETH_MAX_POOL_COUNT) {
			rc = -EINVAL;
			goto unlock_err;
		}
		if (value != oldsize) {
			newsize = value;
			change = true;
		}
	} else if (attr == &veth_size_attr) {
		if (value <= IBMVETH_BUFF_OH || value > IBMVETH_MAX_BUF_SIZE) {
			rc = -EINVAL;
			goto unlock_err;
		}
		if (value != oldbuff_size) {
			newbuff_size = value;
			change = true;
		}
	}

	if (change) {
		if (netif_running(netdev))
			ibmveth_close(netdev);

		/* close() keeps the pools mapped when H_FREE_LOGICAL_LAN
		 * fails. Their free uses pool->size and pool->buff_size,
		 * so retry it before changing the geometry.
		 */
		if (ibmveth_lan_resources_live(adapter)) {
			if (ibmveth_free_all_queues(adapter)) {
				rc = -EBUSY;
				if (netif_running(netdev))
					rc = ibmveth_open(netdev) ?: -EBUSY;
				goto unlock_err;
			}
			ibmveth_release_lan_resources(adapter);
		}

		pool->active = newactive;
		pool->buff_size = newbuff_size;
		pool->size = newsize;

		if (netif_running(netdev)) {
			rc = ibmveth_open(netdev);
			if (rc) {
				/* A failed open() can keep the new pools
				 * mapped; their free needs this geometry.
				 */
				if (!ibmveth_lan_resources_live(adapter)) {
					pool->active = oldactive;
					pool->buff_size = oldbuff_size;
					pool->size = oldsize;
				}
				goto unlock_err;
			}
		}
	}
	rtnl_unlock();

	/* kick RX processing to allocate/deallocate pools */
	ibmveth_schedule_rx_queue(adapter, 0);
	return count;

unlock_err:
	rtnl_unlock();
	return rc;
}


#define ATTR(_name, _mode)				\
	struct attribute veth_##_name##_attr = {	\
	.name = __stringify(_name), .mode = _mode,	\
	};

static ATTR(active, 0644);
static ATTR(num, 0644);
static ATTR(size, 0644);

static struct attribute *veth_pool_attrs[] = {
	&veth_active_attr,
	&veth_num_attr,
	&veth_size_attr,
	NULL,
};
ATTRIBUTE_GROUPS(veth_pool);

static const struct sysfs_ops veth_pool_ops = {
	.show   = veth_pool_show,
	.store  = veth_pool_store,
};

static struct kobj_type ktype_veth_pool = {
	.release        = ibmveth_pool_kobj_release,
	.sysfs_ops      = &veth_pool_ops,
	.default_groups = veth_pool_groups,
};

static int ibmveth_resume(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmveth_adapter *adapter = netdev_priv(netdev);
	unsigned int num = adapter->num_rx_queues;
	int i;

	for (i = 0; i < num; i++)
		ibmveth_schedule_rx_queue(adapter, i);

	return 0;
}

static const struct vio_device_id ibmveth_device_table[] = {
	{ "network", "IBM,l-lan"},
	{ "", "" }
};
MODULE_DEVICE_TABLE(vio, ibmveth_device_table);

static const struct dev_pm_ops ibmveth_pm_ops = {
	.resume = ibmveth_resume
};

static struct vio_driver ibmveth_driver = {
	.id_table	= ibmveth_device_table,
	.probe		= ibmveth_probe,
	.remove		= ibmveth_remove,
	.get_desired_dma = ibmveth_get_desired_dma,
	.name		= ibmveth_driver_name,
	.pm		= &ibmveth_pm_ops,
};

static int __init ibmveth_module_init(void)
{
	printk(KERN_DEBUG "%s: %s %s\n", ibmveth_driver_name,
	       ibmveth_driver_string, ibmveth_driver_version);

	return vio_register_driver(&ibmveth_driver);
}

static void __exit ibmveth_module_exit(void)
{
	vio_unregister_driver(&ibmveth_driver);
}

module_init(ibmveth_module_init);
module_exit(ibmveth_module_exit);

#ifdef CONFIG_IBMVETH_KUNIT_TEST
#include <kunit/test.h>

/**
 * ibmveth_reset_kunit - reset routine for running in KUnit environment
 *
 * @w: pointer to work_struct embedded in adapter structure
 *
 * Context: Called in the KUnit environment. Does nothing.
 *
 * Return: void
 */
static void ibmveth_reset_kunit(struct work_struct *w)
{
	netdev_dbg(NULL, "reset_kunit starting\n");
	netdev_dbg(NULL, "reset_kunit complete\n");
}

/**
 * ibmveth_remove_buffer_from_pool_test - unit test for some of
 *                                        ibmveth_remove_buffer_from_pool
 * @test: pointer to kunit structure
 *
 * Tests the error returns from ibmveth_remove_buffer_from_pool.
 * Each error also logs a ratelimited netdev_err.
 *
 * Return: void
 */
static void ibmveth_remove_buffer_from_pool_test(struct kunit *test)
{
	struct ibmveth_adapter *adapter = kunit_kzalloc(test, sizeof(*adapter), GFP_KERNEL);
	struct ibmveth_buff_pool *pool;
	u64 correlator;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter);

	INIT_WORK(&adapter->work, ibmveth_reset_kunit);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);

	correlator = ((u64)IBMVETH_NUM_BUFF_POOLS << 32) | 0;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, true));

	correlator = ((u64)0 << 32) | adapter->rx_buff_pool[0][0].size;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, false));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, true));

	/* Pool 2 is in range but has no skbuff array, like an inactive pool. */
	correlator = ((u64)2 << 32) | 0;
	ret = ibmveth_remove_buffer_from_pool(adapter, 0, correlator, false);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	ret = ibmveth_remove_buffer_from_pool(adapter, 0, correlator, true);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	correlator = (u64)0 | 0;
	pool->skbuff[0] = NULL;
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, false));
	KUNIT_EXPECT_EQ(test, -EFAULT,
			ibmveth_remove_buffer_from_pool(adapter,
							0, correlator, true));

	flush_work(&adapter->work);
}

/**
 * ibmveth_rxq_get_buffer_test - unit test for ibmveth_rxq_get_buffer
 * @test: pointer to kunit structure
 *
 * Tests ibmveth_rxq_get_buffer. Each NULL return also logs a ratelimited
 * netdev_err.
 *
 * Return: void
 */
static void ibmveth_rxq_get_buffer_test(struct kunit *test)
{
	struct ibmveth_adapter *adapter = kunit_kzalloc(test, sizeof(*adapter), GFP_KERNEL);
	struct sk_buff *skb = kunit_kzalloc(test, sizeof(*skb), GFP_KERNEL);
	struct ibmveth_buff_pool *pool;
	u64 cor;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);

	INIT_WORK(&adapter->work, ibmveth_reset_kunit);

	adapter->rx_queue[0].queue_len = 1;
	adapter->rx_queue[0].index = 0;
	adapter->rx_queue[0].queue_addr =
		kunit_kzalloc(test, sizeof(struct ibmveth_rx_q_entry),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter->rx_queue[0].queue_addr);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);

	adapter->rx_queue[0].queue_addr[0].correlator =
		(u64)IBMVETH_NUM_BUFF_POOLS << 32 | 0;
	cor = ibmveth_rxq_correlator(adapter, 0);
	KUNIT_EXPECT_PTR_EQ(test, NULL,
			    ibmveth_rxq_get_buffer(adapter, 0, cor));

	adapter->rx_queue[0].queue_addr[0].correlator =
		(u64)0 << 32 | adapter->rx_buff_pool[0][0].size;
	cor = ibmveth_rxq_correlator(adapter, 0);
	KUNIT_EXPECT_PTR_EQ(test, NULL,
			    ibmveth_rxq_get_buffer(adapter, 0, cor));

	/* Pool 2 is in range but has no skbuff array, like an inactive pool. */
	adapter->rx_queue[0].queue_addr[0].correlator = (u64)2 << 32 | 0;
	cor = ibmveth_rxq_correlator(adapter, 0);
	KUNIT_EXPECT_PTR_EQ(test, NULL,
			    ibmveth_rxq_get_buffer(adapter, 0, cor));

	pool->skbuff[0] = skb;
	adapter->rx_queue[0].queue_addr[0].correlator = (u64)0 << 32 | 0;
	cor = ibmveth_rxq_correlator(adapter, 0);
	KUNIT_EXPECT_PTR_EQ(test, skb,
			    ibmveth_rxq_get_buffer(adapter, 0, cor));

	flush_work(&adapter->work);
}

/**
 * ibmveth_rxq_harvest_buffer_test - unit test for ibmveth_rxq_harvest_buffer
 * @test: pointer to kunit structure
 *
 * A bad correlator must still advance the RX ring, wrapping and flipping
 * the toggle at the end. This covers the harvest path; the advance after
 * ibmveth_rxq_get_buffer() fails in ibmveth_poll_deliver_frame() is not
 * tested here.
 *
 * Return: void
 */
static void ibmveth_rxq_harvest_buffer_test(struct kunit *test)
{
	struct ibmveth_adapter *adapter;
	struct ibmveth_buff_pool *pool;
	u64 cor;
	int ret;

	adapter = kunit_kzalloc(test, sizeof(*adapter), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter);

	INIT_WORK(&adapter->work, ibmveth_reset_kunit);

	adapter->rx_queue[0].num_slots = 2;
	adapter->rx_queue[0].index = 0;
	adapter->rx_queue[0].toggle = 1;
	adapter->rx_queue[0].queue_addr =
		kunit_kcalloc(test, 2, sizeof(struct ibmveth_rx_q_entry),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, adapter->rx_queue[0].queue_addr);

	/* Set sane values for buffer pools */
	for (int i = 0; i < IBMVETH_NUM_BUFF_POOLS; i++)
		ibmveth_init_buffer_pool(&adapter->rx_buff_pool[0][i], i,
					 pool_count[i], pool_size[i],
					 pool_active[i]);

	pool = &adapter->rx_buff_pool[0][0];
	pool->skbuff = kunit_kcalloc(test, pool->size, sizeof(void *),
				     GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool->skbuff);

	/* Slot 0: pool out of range. Slot 1: valid, but no skb. */
	adapter->rx_queue[0].queue_addr[0].correlator =
		(u64)IBMVETH_NUM_BUFF_POOLS << 32 | 0;
	adapter->rx_queue[0].queue_addr[1].correlator = (u64)0 << 32 | 0;

	cor = ibmveth_rxq_correlator(adapter, 0);
	ret = ibmveth_rxq_harvest_buffer(adapter, 0, cor, true);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);
	KUNIT_EXPECT_EQ(test, 1ULL, adapter->rx_queue[0].index);
	KUNIT_EXPECT_EQ(test, 1ULL, adapter->rx_queue[0].toggle);

	cor = ibmveth_rxq_correlator(adapter, 0);
	ret = ibmveth_rxq_harvest_buffer(adapter, 0, cor, true);
	KUNIT_EXPECT_EQ(test, -EFAULT, ret);
	KUNIT_EXPECT_EQ(test, 0ULL, adapter->rx_queue[0].index);
	KUNIT_EXPECT_EQ(test, 0ULL, adapter->rx_queue[0].toggle);

	flush_work(&adapter->work);
}

KUNIT_DEFINE_ACTION_WRAPPER(ibmveth_kfree_skb, kfree_skb, struct sk_buff *);

/* A TCP frame after eth_type_trans(): IP header, 20 byte TCP header and
 * payload bytes of data, with the IP length matching the frame.
 */
static struct sk_buff *ibmveth_test_tcp_skb(struct kunit *test, bool ipv6,
					    unsigned int payload)
{
	unsigned int iplen = ipv6 ? sizeof(struct ipv6hdr) :
				    sizeof(struct iphdr);
	unsigned int len = iplen + sizeof(struct tcphdr) + payload;
	struct sk_buff *skb = alloc_skb(len, GFP_KERNEL);
	struct tcphdr *tcph;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test,
							   ibmveth_kfree_skb,
							   skb));
	skb_put_zero(skb, len);
	if (ipv6) {
		struct ipv6hdr *iph6 = (struct ipv6hdr *)skb->data;

		skb->protocol = htons(ETH_P_IPV6);
		iph6->version = 6;
		iph6->nexthdr = IPPROTO_TCP;
		iph6->payload_len = htons(len - iplen);
	} else {
		struct iphdr *iph = (struct iphdr *)skb->data;

		skb->protocol = htons(ETH_P_IP);
		iph->version = 4;
		iph->ihl = 5;
		iph->protocol = IPPROTO_TCP;
		iph->tot_len = htons(len);
	}
	tcph = (struct tcphdr *)(skb->data + iplen);
	tcph->doff = 5;
	return skb;
}

/**
 * ibmveth_rx_mss_helper_test - unit test for ibmveth_rx_mss_helper
 * @test: pointer to kunit structure
 *
 * Large send frames get gso_size from the descriptor or the TCP checksum
 * field, clamped, and gso_segs from the IP length. Frames whose headers
 * do not fit, whose IP length is not the frame length, with no TCP
 * payload within the IP length, or that are IPv4 fragments, are refused
 * and left without GSO.
 *
 * Return: void
 */
static void ibmveth_rx_mss_helper_test(struct kunit *test)
{
	struct sk_buff *skb;
	struct tcphdr *tcph;
	struct iphdr *iph;

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	tcph = (struct tcphdr *)(skb->data + sizeof(*iph));
	tcph->check = (__force __sum16)htons(1448);
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, 0, 0));
	KUNIT_EXPECT_EQ(test, SKB_GSO_TCPV4, skb_shinfo(skb)->gso_type);
	KUNIT_EXPECT_EQ(test, 1448, skb_shinfo(skb)->gso_size);
	KUNIT_EXPECT_EQ(test, 2, skb_shinfo(skb)->gso_segs);
	KUNIT_EXPECT_EQ(test, 0, (__force u16)tcph->check);

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, 1000, 1));
	KUNIT_EXPECT_EQ(test, 1000, skb_shinfo(skb)->gso_size);
	KUNIT_EXPECT_EQ(test, 3, skb_shinfo(skb)->gso_segs);

	skb = ibmveth_test_tcp_skb(test, true, 2896);
	tcph = (struct tcphdr *)(skb->data + sizeof(struct ipv6hdr));
	tcph->check = (__force __sum16)htons(1448);
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, 0, 0));
	KUNIT_EXPECT_EQ(test, SKB_GSO_TCPV6, skb_shinfo(skb)->gso_type);
	KUNIT_EXPECT_EQ(test, 1448, skb_shinfo(skb)->gso_size);
	KUNIT_EXPECT_EQ(test, 2, skb_shinfo(skb)->gso_segs);

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, 1, 1));
	KUNIT_EXPECT_EQ(test, TCP_MIN_GSO_SIZE, skb_shinfo(skb)->gso_size);

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, GSO_BY_FRAGS, 1));
	KUNIT_EXPECT_EQ(test, GSO_BY_FRAGS - 1, skb_shinfo(skb)->gso_size);

	/* refused: ihl 0, doff 4, IP length past the frame. With ihl 0 the
	 * TCP header would overlap the IP header; saddr makes its doff 5.
	 */
	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct iphdr *)skb->data)->ihl = 0;
	((struct iphdr *)skb->data)->saddr = htonl(0x50000000);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct tcphdr *)(skb->data + sizeof(*iph)))->doff = 4;
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));

	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct iphdr *)skb->data)->tot_len = htons(skb->len + 1);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));

	/* refused: bytes past the IP length, its large send fields left
	 * alone; gso_segs from the IP length would undercount them
	 */
	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct iphdr *)skb->data)->tot_len = htons(skb->len - 100);
	tcph = (struct tcphdr *)(skb->data + sizeof(*iph));
	tcph->check = (__force __sum16)htons(1448);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 0, 0));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);
	KUNIT_EXPECT_EQ(test, 1448, ntohs((__force __be16)tcph->check));

	/* refused: a fragment past the first, its payload left alone */
	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct iphdr *)skb->data)->frag_off = htons(185);
	tcph = (struct tcphdr *)(skb->data + sizeof(*iph));
	tcph->check = (__force __sum16)htons(1448);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 0, 0));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);
	KUNIT_EXPECT_EQ(test, 1448, ntohs((__force __be16)tcph->check));

	/* refused: only Ethernet padding past the TCP header */
	skb = ibmveth_test_tcp_skb(test, false, 6);
	((struct iphdr *)skb->data)->tot_len = htons(40);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);

	skb = ibmveth_test_tcp_skb(test, true, 2896);
	((struct ipv6hdr *)skb->data)->payload_len = 0;
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));

	/* refused: no TCP payload, with the IP length matching the frame */
	skb = ibmveth_test_tcp_skb(test, false, 0);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);

	skb = ibmveth_test_tcp_skb(test, true, 0);
	KUNIT_EXPECT_FALSE(test, ibmveth_rx_mss_helper(skb, 1448, 1));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);

	/* not TCP: accepted and left alone */
	skb = ibmveth_test_tcp_skb(test, false, 2896);
	((struct iphdr *)skb->data)->protocol = IPPROTO_UDP;
	KUNIT_EXPECT_TRUE(test, ibmveth_rx_mss_helper(skb, 1448, 1));
	KUNIT_EXPECT_EQ(test, 0, skb_shinfo(skb)->gso_size);
}

/**
 * ibmveth_rx_csum_helper_test - unit test for ibmveth_rx_csum_helper
 * @test: pointer to kunit structure
 *
 * A large send IPv4 header checksum of 0xffff is rebuilt. Headers that
 * do not fit the frame, an IPv4 ihl below 5, or a TCP fragment, clear
 * CHECKSUM_UNNECESSARY instead of being read.
 *
 * Return: void
 */
static void ibmveth_rx_csum_helper_test(struct kunit *test)
{
	struct sk_buff *skb;
	struct tcphdr *tcph;
	struct iphdr *iph;

	skb = ibmveth_test_tcp_skb(test, false, 100);
	iph = (struct iphdr *)skb->data;
	iph->check = (__force __sum16)0xffff;
	tcph = (struct tcphdr *)(skb->data + sizeof(*iph));
	tcph->check = (__force __sum16)htons(1);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, 0, (__force u16)ip_fast_csum(iph, iph->ihl));
	KUNIT_EXPECT_EQ(test, CHECKSUM_UNNECESSARY, (int)skb->ip_summed);

	skb = ibmveth_test_tcp_skb(test, false, 100);
	iph = (struct iphdr *)skb->data;
	iph->ihl = 0;
	iph->check = (__force __sum16)0xffff;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, CHECKSUM_NONE, (int)skb->ip_summed);
	KUNIT_EXPECT_EQ(test, 0xffff, (__force u16)iph->check);

	/* IPv4 options past the frame; UDP, so no TCP check follows */
	skb = ibmveth_test_tcp_skb(test, false, 0);
	iph = (struct iphdr *)skb->data;
	iph->protocol = IPPROTO_UDP;
	iph->ihl = 15;
	iph->check = (__force __sum16)0xffff;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, CHECKSUM_NONE, (int)skb->ip_summed);

	/* a TCP fragment past the first: header and payload left alone */
	skb = ibmveth_test_tcp_skb(test, false, 100);
	iph = (struct iphdr *)skb->data;
	iph->frag_off = htons(185);
	iph->check = (__force __sum16)0xffff;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, CHECKSUM_NONE, (int)skb->ip_summed);
	KUNIT_EXPECT_EQ(test, 0xffff, (__force u16)iph->check);
	KUNIT_EXPECT_EQ(test, 0,
			(__force u16)((struct tcphdr *)(skb->data +
							sizeof(*iph)))->check);

	/* TCP header past the frame */
	skb = ibmveth_test_tcp_skb(test, false, 0);
	skb_trim(skb, sizeof(struct iphdr));
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, CHECKSUM_NONE, (int)skb->ip_summed);

	/* IPv6 header past the frame; UDP, so no TCP check follows */
	skb = ibmveth_test_tcp_skb(test, true, 0);
	((struct ipv6hdr *)skb->data)->nexthdr = IPPROTO_UDP;
	skb_trim(skb, sizeof(struct ipv6hdr) - 1);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ibmveth_rx_csum_helper(skb, NULL);
	KUNIT_EXPECT_EQ(test, CHECKSUM_NONE, (int)skb->ip_summed);
}

static struct kunit_case ibmveth_test_cases[] = {
	KUNIT_CASE(ibmveth_remove_buffer_from_pool_test),
	KUNIT_CASE(ibmveth_rxq_get_buffer_test),
	KUNIT_CASE(ibmveth_rxq_harvest_buffer_test),
	KUNIT_CASE(ibmveth_rx_mss_helper_test),
	KUNIT_CASE(ibmveth_rx_csum_helper_test),
	{}
};

static struct kunit_suite ibmveth_test_suite = {
	.name = "ibmveth-kunit-test",
	.test_cases = ibmveth_test_cases,
};

kunit_test_suite(ibmveth_test_suite);
#endif
