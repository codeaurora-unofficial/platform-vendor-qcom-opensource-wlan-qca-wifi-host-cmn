/*
 * Copyright (c) 2011-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file htt_rx.c
 * @brief Implement receive aspects of HTT.
 * @details
 *  This file contains three categories of HTT rx code:
 *  1.  An abstraction of the rx descriptor, to hide the
 *      differences between the HL vs. LL rx descriptor.
 *  2.  Functions for providing access to the (series of)
 *      rx descriptor(s) and rx frame(s) associated with
 *      an rx indication message.
 *  3.  Functions for setting up and using the MAC DMA
 *      rx ring (applies to LL only).
 */

#include <qdf_mem.h>         /* qdf_mem_malloc,free, etc. */
#include <qdf_types.h>          /* qdf_print, bool */
#include <qdf_nbuf.h>           /* qdf_nbuf_t, etc. */
#include <qdf_timer.h>		/* qdf_timer_free */

#include <htt.h>                /* HTT_HL_RX_DESC_SIZE */
#include <ol_cfg.h>
#include <ol_rx.h>
#include <ol_htt_rx_api.h>
#include <htt_internal.h>       /* HTT_ASSERT, htt_pdev_t, HTT_RX_BUF_SIZE */
#include "regtable.h"

#include <cds_ieee80211_common.h>   /* ieee80211_frame, ieee80211_qoscntl */
#include <cds_ieee80211_defines.h>  /* ieee80211_rx_status */
#include <cds_utils.h>
#include <wlan_policy_mgr_api.h>
#include "ol_txrx_types.h"
#ifdef DEBUG_DMA_DONE
#include <asm/barrier.h>
#include <wma_api.h>
#endif
#include <pktlog_ac_fmt.h>

/* AR9888v1 WORKAROUND for EV#112367 */
/* FIX THIS - remove this WAR when the bug is fixed */
#define PEREGRINE_1_0_ZERO_LEN_PHY_ERR_WAR

/*--- setup / tear-down functions -------------------------------------------*/

#ifndef HTT_RX_RING_SIZE_MIN
#define HTT_RX_RING_SIZE_MIN 128        /* slightly > than one large A-MPDU */
#endif

#ifndef HTT_RX_RING_SIZE_MAX
#define HTT_RX_RING_SIZE_MAX 2048       /* ~20 ms @ 1 Gbps of 1500B MSDUs */
#endif

#ifndef HTT_RX_AVG_FRM_BYTES
#define HTT_RX_AVG_FRM_BYTES 1000
#endif

#ifndef HTT_RX_HOST_LATENCY_MAX_MS
#define HTT_RX_HOST_LATENCY_MAX_MS 20 /* ms */	/* very conservative */
#endif

 /* very conservative to ensure enough buffers are allocated */
#ifndef HTT_RX_HOST_LATENCY_WORST_LIKELY_MS
#ifdef QCA_WIFI_3_0
#define HTT_RX_HOST_LATENCY_WORST_LIKELY_MS 20
#else
#define HTT_RX_HOST_LATENCY_WORST_LIKELY_MS 10
#endif
#endif

#ifndef HTT_RX_RING_REFILL_RETRY_TIME_MS
#define HTT_RX_RING_REFILL_RETRY_TIME_MS    50
#endif

/*--- RX In Order Definitions ------------------------------------------------*/

/* Number of buckets in the hash table */
#define RX_NUM_HASH_BUCKETS 1024        /* This should always be a power of 2 */
#define RX_NUM_HASH_BUCKETS_MASK (RX_NUM_HASH_BUCKETS - 1)

/* Number of hash entries allocated per bucket */
#define RX_ENTRIES_SIZE 10

#define RX_HASH_FUNCTION(a) (((a >> 14) ^ (a >> 4)) & RX_NUM_HASH_BUCKETS_MASK)

#ifdef RX_HASH_DEBUG_LOG
#define RX_HASH_LOG(x) x
#else
#define RX_HASH_LOG(x)          /* no-op */
#endif

#ifndef CONFIG_HL_SUPPORT
/**
 * htt_get_first_packet_after_wow_wakeup() - get first packet after wow wakeup
 * @msg_word: pointer to rx indication message word
 * @buf: pointer to buffer
 *
 * Return: None
 */
static void
htt_get_first_packet_after_wow_wakeup(uint32_t *msg_word, qdf_nbuf_t buf)
{
	if (HTT_RX_IN_ORD_PADDR_IND_MSDU_INFO_GET(*msg_word) &
			FW_MSDU_INFO_FIRST_WAKEUP_M) {
		qdf_nbuf_mark_wakeup_frame(buf);
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_INFO,
			  "%s: First packet after WOW Wakeup rcvd", __func__);
	}
}

/* De -initialization function of the rx buffer hash table. This function will
 *   free up the hash table which includes freeing all the pending rx buffers
 */
static void htt_rx_hash_deinit(struct htt_pdev_t *pdev)
{

	uint32_t i;
	struct htt_rx_hash_entry *hash_entry;
	struct htt_rx_hash_bucket **hash_table;
	struct htt_list_node *list_iter = NULL;
	qdf_mem_info_t mem_map_table = {0};
	bool ipa_smmu = false;

	if (NULL == pdev->rx_ring.hash_table)
		return;

	if (qdf_mem_smmu_s1_enabled(pdev->osdev) && pdev->is_ipa_uc_enabled &&
	    pdev->rx_ring.smmu_map)
		ipa_smmu = true;

	qdf_spin_lock_bh(&(pdev->rx_ring.rx_hash_lock));
	hash_table = pdev->rx_ring.hash_table;
	pdev->rx_ring.hash_table = NULL;
	qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));

	for (i = 0; i < RX_NUM_HASH_BUCKETS; i++) {
		/* Free the hash entries in hash bucket i */
		list_iter = hash_table[i]->listhead.next;
		while (list_iter != &hash_table[i]->listhead) {
			hash_entry =
				(struct htt_rx_hash_entry *)((char *)list_iter -
							     pdev->rx_ring.
							     listnode_offset);
			if (hash_entry->netbuf) {
				if (ipa_smmu) {
					qdf_update_mem_map_table(pdev->osdev,
						&mem_map_table,
						QDF_NBUF_CB_PADDR(
							hash_entry->netbuf),
						HTT_RX_BUF_SIZE);

					cds_smmu_map_unmap(false, 1,
							   &mem_map_table);
				}
#ifdef DEBUG_DMA_DONE
				qdf_nbuf_unmap(pdev->osdev, hash_entry->netbuf,
					       QDF_DMA_BIDIRECTIONAL);
#else
				qdf_nbuf_unmap(pdev->osdev, hash_entry->netbuf,
					       QDF_DMA_FROM_DEVICE);
#endif
				qdf_nbuf_free(hash_entry->netbuf);
				hash_entry->paddr = 0;
			}
			list_iter = list_iter->next;

			if (!hash_entry->fromlist)
				qdf_mem_free(hash_entry);
		}

		qdf_mem_free(hash_table[i]);

	}
	qdf_mem_free(hash_table);

	qdf_spinlock_destroy(&(pdev->rx_ring.rx_hash_lock));
}
#endif

/*
 * This function is used both below within this file (which the compiler
 * will hopefully inline), and out-line from other files via the
 * htt_rx_msdu_first_msdu_flag function pointer.
 */

static inline bool
htt_rx_msdu_first_msdu_flag_hl(htt_pdev_handle pdev, void *msdu_desc)
{
	return ((u_int8_t *)msdu_desc - sizeof(struct hl_htt_rx_ind_base))
		[HTT_ENDIAN_BYTE_IDX_SWAP(HTT_RX_IND_HL_FLAG_OFFSET)] &
		HTT_RX_IND_HL_FLAG_FIRST_MSDU ? true : false;
}

u_int16_t
htt_rx_msdu_rx_desc_size_hl(
	htt_pdev_handle pdev,
	void *msdu_desc
		)
{
	return ((u_int8_t *)(msdu_desc) - HTT_RX_IND_HL_BYTES)
		[HTT_ENDIAN_BYTE_IDX_SWAP(HTT_RX_IND_HL_RX_DESC_LEN_OFFSET)];
}

/**
 * htt_rx_mpdu_desc_retry_hl() - Returns the retry bit from the Rx descriptor
 *                               for the High Latency driver
 * @pdev: Handle (pointer) to HTT pdev.
 * @mpdu_desc: Void pointer to the Rx descriptor for MPDU
 *             before the beginning of the payload.
 *
 *  This function returns the retry bit of the 802.11 header for the
 *  provided rx MPDU descriptor. For the high latency driver, this function
 *  pretends as if the retry bit is never set so that the mcast duplicate
 *  detection never fails.
 *
 * Return:        boolean -- false always for HL
 */
static inline bool
htt_rx_mpdu_desc_retry_hl(htt_pdev_handle pdev, void *mpdu_desc)
{
	return false;
}

#ifdef CONFIG_HL_SUPPORT
static uint16_t
htt_rx_mpdu_desc_seq_num_hl(htt_pdev_handle pdev, void *mpdu_desc)
{
	if (pdev->rx_desc_size_hl) {
		return pdev->cur_seq_num_hl =
			(u_int16_t)(HTT_WORD_GET(*(u_int32_t *)mpdu_desc,
						HTT_HL_RX_DESC_MPDU_SEQ_NUM));
	} else {
		return (u_int16_t)(pdev->cur_seq_num_hl);
	}
}

static void
htt_rx_mpdu_desc_pn_hl(
	htt_pdev_handle pdev,
	void *mpdu_desc,
	union htt_rx_pn_t *pn,
	int pn_len_bits)
{
	if (htt_rx_msdu_first_msdu_flag_hl(pdev, mpdu_desc) == true) {
		/* Fix Me: only for little endian */
		struct hl_htt_rx_desc_base *rx_desc =
			(struct hl_htt_rx_desc_base *)mpdu_desc;
		u_int32_t *word_ptr = (u_int32_t *)pn->pn128;

		/* TODO: for Host of big endian */
		switch (pn_len_bits) {
		case 128:
			/* bits 128:64 */
			*(word_ptr + 3) = rx_desc->pn_127_96;
			/* bits 63:0 */
			*(word_ptr + 2) = rx_desc->pn_95_64;
		case 48:
			/* bits 48:0
			 * copy 64 bits
			 */
			*(word_ptr + 1) = rx_desc->u0.pn_63_32;
		case 24:
			/* bits 23:0
			 * copy 32 bits
			 */
			*(word_ptr + 0) = rx_desc->pn_31_0;
			break;
		default:
			QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
				  "Error: invalid length spec (%d bits) for PN",
				  pn_len_bits);
			qdf_assert(0);
			break;
		};
	} else {
		/* not first msdu, no pn info */
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "Error: get pn from a not-first msdu.");
		qdf_assert(0);
	}
}
#endif

/**
 * htt_rx_mpdu_desc_tid_hl() - Returns the TID value from the Rx descriptor
 *                             for High Latency driver
 * @pdev:                        Handle (pointer) to HTT pdev.
 * @mpdu_desc:                   Void pointer to the Rx descriptor for the MPDU
 *                               before the beginning of the payload.
 *
 * This function returns the TID set in the 802.11 QoS Control for the MPDU
 * in the packet header, by looking at the mpdu_start of the Rx descriptor.
 * Rx descriptor gets a copy of the TID from the MAC.
 * For the HL driver, this is currently uimplemented and always returns
 * an invalid tid. It is the responsibility of the caller to make
 * sure that return value is checked for valid range.
 *
 * Return:        Invalid TID value (0xff) for HL driver.
 */
static inline uint8_t
htt_rx_mpdu_desc_tid_hl(htt_pdev_handle pdev, void *mpdu_desc)
{
	return 0xff;  /* Invalid TID */
}

static inline bool
htt_rx_msdu_desc_completes_mpdu_hl(htt_pdev_handle pdev, void *msdu_desc)
{
	return (
		((u_int8_t *)(msdu_desc) - sizeof(struct hl_htt_rx_ind_base))
		[HTT_ENDIAN_BYTE_IDX_SWAP(HTT_RX_IND_HL_FLAG_OFFSET)]
		& HTT_RX_IND_HL_FLAG_LAST_MSDU)
		? true : false;
}

static inline int
htt_rx_msdu_has_wlan_mcast_flag_hl(htt_pdev_handle pdev, void *msdu_desc)
{
	/* currently, only first msdu has hl rx_desc */
	return htt_rx_msdu_first_msdu_flag_hl(pdev, msdu_desc) == true;
}

static inline bool
htt_rx_msdu_is_wlan_mcast_hl(htt_pdev_handle pdev, void *msdu_desc)
{
	struct hl_htt_rx_desc_base *rx_desc =
		(struct hl_htt_rx_desc_base *)msdu_desc;

	return
		HTT_WORD_GET(*(u_int32_t *)rx_desc, HTT_HL_RX_DESC_MCAST_BCAST);
}

static inline int
htt_rx_msdu_is_frag_hl(htt_pdev_handle pdev, void *msdu_desc)
{
	struct hl_htt_rx_desc_base *rx_desc =
		(struct hl_htt_rx_desc_base *)msdu_desc;

	return
		HTT_WORD_GET(*(u_int32_t *)rx_desc, HTT_HL_RX_DESC_MCAST_BCAST);
}

#ifdef ENABLE_DEBUG_ADDRESS_MARKING
static qdf_dma_addr_t
htt_rx_paddr_mark_high_bits(qdf_dma_addr_t paddr)
{
	if (sizeof(qdf_dma_addr_t) > 4) {
		/* clear high bits, leave lower 37 bits (paddr) */
		paddr &= 0x01FFFFFFFFF;
		/* mark upper 16 bits of paddr */
		paddr |= (((uint64_t)RX_PADDR_MAGIC_PATTERN) << 32);
	}
	return paddr;
}
#else
static qdf_dma_addr_t
htt_rx_paddr_mark_high_bits(qdf_dma_addr_t paddr)
{
	return paddr;
}
#endif

#ifndef CONFIG_HL_SUPPORT
static bool
htt_rx_msdu_first_msdu_flag_ll(htt_pdev_handle pdev, void *msdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)msdu_desc;
	return (bool)
		(((*(((uint32_t *)&rx_desc->msdu_end) + 4)) &
		  RX_MSDU_END_4_FIRST_MSDU_MASK) >>
		 RX_MSDU_END_4_FIRST_MSDU_LSB);
}

#endif /* CONFIG_HL_SUPPORT*/

/* full_reorder_offload case: this function is called with lock held */
static int htt_rx_ring_fill_n(struct htt_pdev_t *pdev, int num)
{
	int idx;
	QDF_STATUS status;
	struct htt_host_rx_desc_base *rx_desc;
	int filled = 0;
	int debt_served = 0;
	qdf_mem_info_t mem_map_table = {0};
	bool ipa_smmu = false;

	idx = *(pdev->rx_ring.alloc_idx.vaddr);

	if (qdf_mem_smmu_s1_enabled(pdev->osdev) && pdev->is_ipa_uc_enabled &&
	    pdev->rx_ring.smmu_map)
		ipa_smmu = true;

	if ((idx < 0) || (idx > pdev->rx_ring.size_mask) ||
	    (num > pdev->rx_ring.size))  {
		QDF_TRACE(QDF_MODULE_ID_HTT,
			  QDF_TRACE_LEVEL_ERROR,
			  "%s:rx refill failed!", __func__);
		return filled;
	}

moretofill:
	while (num > 0) {
		qdf_dma_addr_t paddr, paddr_marked;
		qdf_nbuf_t rx_netbuf;
		int headroom;

		rx_netbuf =
			qdf_nbuf_alloc(pdev->osdev, HTT_RX_BUF_SIZE,
				       0, 4, false);
		if (!rx_netbuf) {
			qdf_timer_stop(&pdev->rx_ring.
						 refill_retry_timer);
			/*
			 * Failed to fill it to the desired level -
			 * we'll start a timer and try again next time.
			 * As long as enough buffers are left in the ring for
			 * another A-MPDU rx, no special recovery is needed.
			 */
#ifdef DEBUG_DMA_DONE
			pdev->rx_ring.dbg_refill_cnt++;
#endif
			pdev->refill_retry_timer_starts++;
			qdf_timer_start(
				&pdev->rx_ring.refill_retry_timer,
				HTT_RX_RING_REFILL_RETRY_TIME_MS);
			goto update_alloc_idx;
		}

		/* Clear rx_desc attention word before posting to Rx ring */
		rx_desc = htt_rx_desc(rx_netbuf);
		*(uint32_t *) &rx_desc->attention = 0;

#ifdef DEBUG_DMA_DONE
		*(uint32_t *) &rx_desc->msdu_end = 1;

#define MAGIC_PATTERN 0xDEADBEEF
		*(uint32_t *) &rx_desc->msdu_start = MAGIC_PATTERN;

		/*
		 * To ensure that attention bit is reset and msdu_end is set
		 * before calling dma_map
		 */
		smp_mb();
#endif
		/*
		 * Adjust qdf_nbuf_data to point to the location in the buffer
		 * where the rx descriptor will be filled in.
		 */
		headroom = qdf_nbuf_data(rx_netbuf) - (uint8_t *) rx_desc;
		qdf_nbuf_push_head(rx_netbuf, headroom);

#ifdef DEBUG_DMA_DONE
		status =
			qdf_nbuf_map(pdev->osdev, rx_netbuf,
						QDF_DMA_BIDIRECTIONAL);
#else
		status =
			qdf_nbuf_map(pdev->osdev, rx_netbuf,
						QDF_DMA_FROM_DEVICE);
#endif
		if (status != QDF_STATUS_SUCCESS) {
			qdf_nbuf_free(rx_netbuf);
			goto update_alloc_idx;
		}

		paddr = qdf_nbuf_get_frag_paddr(rx_netbuf, 0);
		paddr_marked = htt_rx_paddr_mark_high_bits(paddr);
		if (pdev->cfg.is_full_reorder_offload) {
			if (qdf_unlikely(htt_rx_hash_list_insert(
					pdev, paddr_marked, rx_netbuf))) {
				QDF_TRACE(QDF_MODULE_ID_HTT,
					  QDF_TRACE_LEVEL_ERROR,
					  "%s: hash insert failed!", __func__);
#ifdef DEBUG_DMA_DONE
				qdf_nbuf_unmap(pdev->osdev, rx_netbuf,
					       QDF_DMA_BIDIRECTIONAL);
#else
				qdf_nbuf_unmap(pdev->osdev, rx_netbuf,
					       QDF_DMA_FROM_DEVICE);
#endif
				qdf_nbuf_free(rx_netbuf);
				goto update_alloc_idx;
			}
			htt_rx_dbg_rxbuf_set(pdev, paddr_marked, rx_netbuf);
		} else {
			pdev->rx_ring.buf.netbufs_ring[idx] = rx_netbuf;
		}

		if (ipa_smmu) {
			qdf_update_mem_map_table(pdev->osdev, &mem_map_table,
						 paddr, HTT_RX_BUF_SIZE);
			cds_smmu_map_unmap(true, 1, &mem_map_table);
		}

		pdev->rx_ring.buf.paddrs_ring[idx] = paddr_marked;
		pdev->rx_ring.fill_cnt++;

		num--;
		idx++;
		filled++;
		idx &= pdev->rx_ring.size_mask;
	}

	if (debt_served <  qdf_atomic_read(&pdev->rx_ring.refill_debt)) {
		num = qdf_atomic_read(&pdev->rx_ring.refill_debt);
		debt_served += num;
		goto moretofill;
	}

update_alloc_idx:
	/*
	 * Make sure alloc index write is reflected correctly before FW polls
	 * remote ring write index as compiler can reorder the instructions
	 * based on optimizations.
	 */
	qdf_mb();
	*(pdev->rx_ring.alloc_idx.vaddr) = idx;
	htt_rx_dbg_rxbuf_indupd(pdev, idx);

	return filled;
}

#ifndef CONFIG_HL_SUPPORT
static int htt_rx_ring_size(struct htt_pdev_t *pdev)
{
	int size;

	/*
	 * It is expected that the host CPU will typically be able to service
	 * the rx indication from one A-MPDU before the rx indication from
	 * the subsequent A-MPDU happens, roughly 1-2 ms later.
	 * However, the rx ring should be sized very conservatively, to
	 * accommodate the worst reasonable delay before the host CPU services
	 * a rx indication interrupt.
	 * The rx ring need not be kept full of empty buffers.  In theory,
	 * the htt host SW can dynamically track the low-water mark in the
	 * rx ring, and dynamically adjust the level to which the rx ring
	 * is filled with empty buffers, to dynamically meet the desired
	 * low-water mark.
	 * In contrast, it's difficult to resize the rx ring itself, once
	 * it's in use.
	 * Thus, the ring itself should be sized very conservatively, while
	 * the degree to which the ring is filled with empty buffers should
	 * be sized moderately conservatively.
	 */
	size =
		ol_cfg_max_thruput_mbps(pdev->ctrl_pdev) *
		1000 /* 1e6 bps/mbps / 1e3 ms per sec = 1000 */  /
		(8 * HTT_RX_AVG_FRM_BYTES) * HTT_RX_HOST_LATENCY_MAX_MS;

	if (size < HTT_RX_RING_SIZE_MIN)
		size = HTT_RX_RING_SIZE_MIN;
	else if (size > HTT_RX_RING_SIZE_MAX)
		size = HTT_RX_RING_SIZE_MAX;

	size = qdf_get_pwr2(size);
	return size;
}

static int htt_rx_ring_fill_level(struct htt_pdev_t *pdev)
{
	int size;

	size = ol_cfg_max_thruput_mbps(pdev->ctrl_pdev) *
		1000 /* 1e6 bps/mbps / 1e3 ms per sec = 1000 */  /
		(8 * HTT_RX_AVG_FRM_BYTES) *
		HTT_RX_HOST_LATENCY_WORST_LIKELY_MS;

	size = qdf_get_pwr2(size);
	/*
	 * Make sure the fill level is at least 1 less than the ring size.
	 * Leaving 1 element empty allows the SW to easily distinguish
	 * between a full ring vs. an empty ring.
	 */
	if (size >= pdev->rx_ring.size)
		size = pdev->rx_ring.size - 1;

	return size;
}

static void htt_rx_ring_refill_retry(void *arg)
{
	htt_pdev_handle pdev = (htt_pdev_handle) arg;
	int             filled = 0;
	int             num;

	pdev->refill_retry_timer_calls++;
	qdf_spin_lock_bh(&(pdev->rx_ring.refill_lock));

	num = qdf_atomic_read(&pdev->rx_ring.refill_debt);
	qdf_atomic_sub(num, &pdev->rx_ring.refill_debt);
	filled = htt_rx_ring_fill_n(pdev, num);

	if (filled > num) {
		/* we served ourselves and some other debt */
		/* sub is safer than  = 0 */
		qdf_atomic_sub(filled - num, &pdev->rx_ring.refill_debt);
	} else if (num == filled) { /* nothing to be done */
	} else {
		qdf_atomic_add(num - filled, &pdev->rx_ring.refill_debt);
		/* we could not fill all, timer must have been started */
		pdev->refill_retry_timer_doubles++;
	}
	qdf_spin_unlock_bh(&(pdev->rx_ring.refill_lock));
}
#endif

static inline unsigned int htt_rx_ring_elems(struct htt_pdev_t *pdev)
{
	return
		(*pdev->rx_ring.alloc_idx.vaddr -
		 pdev->rx_ring.sw_rd_idx.msdu_payld) & pdev->rx_ring.size_mask;
}

static inline unsigned int htt_rx_in_order_ring_elems(struct htt_pdev_t *pdev)
{
	return
		(*pdev->rx_ring.alloc_idx.vaddr -
		 *pdev->rx_ring.target_idx.vaddr) &
		pdev->rx_ring.size_mask;
}

#ifndef CONFIG_HL_SUPPORT

void htt_rx_detach(struct htt_pdev_t *pdev)
{
	bool ipa_smmu = false;
	qdf_timer_stop(&pdev->rx_ring.refill_retry_timer);
	qdf_timer_free(&pdev->rx_ring.refill_retry_timer);
	htt_rx_dbg_rxbuf_deinit(pdev);

	if (qdf_mem_smmu_s1_enabled(pdev->osdev) && pdev->is_ipa_uc_enabled &&
	    pdev->rx_ring.smmu_map)
		ipa_smmu = true;

	if (pdev->cfg.is_full_reorder_offload) {
		qdf_mem_free_consistent(pdev->osdev, pdev->osdev->dev,
					   sizeof(uint32_t),
					   pdev->rx_ring.target_idx.vaddr,
					   pdev->rx_ring.target_idx.paddr,
					   qdf_get_dma_mem_context((&pdev->
								    rx_ring.
								    target_idx),
								   memctx));
		htt_rx_hash_deinit(pdev);
	} else {
		int sw_rd_idx = pdev->rx_ring.sw_rd_idx.msdu_payld;
		qdf_mem_info_t mem_map_table = {0};

		while (sw_rd_idx != *(pdev->rx_ring.alloc_idx.vaddr)) {
			if (ipa_smmu) {
				qdf_update_mem_map_table(pdev->osdev,
					&mem_map_table,
					QDF_NBUF_CB_PADDR(
						pdev->rx_ring.buf.
						netbufs_ring[sw_rd_idx]),
					HTT_RX_BUF_SIZE);
				cds_smmu_map_unmap(false, 1,
						   &mem_map_table);
			}
#ifdef DEBUG_DMA_DONE
			qdf_nbuf_unmap(pdev->osdev,
				       pdev->rx_ring.buf.
				       netbufs_ring[sw_rd_idx],
				       QDF_DMA_BIDIRECTIONAL);
#else
			qdf_nbuf_unmap(pdev->osdev,
				       pdev->rx_ring.buf.
				       netbufs_ring[sw_rd_idx],
				       QDF_DMA_FROM_DEVICE);
#endif
			qdf_nbuf_free(pdev->rx_ring.buf.
				      netbufs_ring[sw_rd_idx]);
			sw_rd_idx++;
			sw_rd_idx &= pdev->rx_ring.size_mask;
		}
		qdf_mem_free(pdev->rx_ring.buf.netbufs_ring);

	}

	qdf_mem_free_consistent(pdev->osdev, pdev->osdev->dev,
				   sizeof(uint32_t),
				   pdev->rx_ring.alloc_idx.vaddr,
				   pdev->rx_ring.alloc_idx.paddr,
				   qdf_get_dma_mem_context((&pdev->rx_ring.
							    alloc_idx),
							   memctx));

	qdf_mem_free_consistent(pdev->osdev, pdev->osdev->dev,
				   pdev->rx_ring.size * sizeof(target_paddr_t),
				   pdev->rx_ring.buf.paddrs_ring,
				   pdev->rx_ring.base_paddr,
				   qdf_get_dma_mem_context((&pdev->rx_ring.buf),
							   memctx));

	/* destroy the rx-parallelization refill spinlock */
	qdf_spinlock_destroy(&(pdev->rx_ring.refill_lock));
}
#endif

/**
 * htt_rx_mpdu_wifi_hdr_retrieve() - retrieve 802.11 header
 * @pdev - pdev handle
 * @mpdu_desc - mpdu descriptor
 *
 * Return : pointer to 802.11 header
 */
char *htt_rx_mpdu_wifi_hdr_retrieve(htt_pdev_handle pdev, void *mpdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)mpdu_desc;

	if (!rx_desc)
		return NULL;
	else
		return rx_desc->rx_hdr_status;
}

/**
 * htt_rx_mpdu_desc_tsf32() - Return the TSF timestamp indicating when
 *                            a MPDU was received.
 * @pdev - the HTT instance the rx data was received on
 * @mpdu_desc - the abstract descriptor for the MPDU in question
 *
 * return : 32 LSBs of TSF time at which the MPDU's PPDU was received
 */
uint32_t htt_rx_mpdu_desc_tsf32(htt_pdev_handle pdev, void *mpdu_desc)
{
	return 0;
}

/*--- rx descriptor field access functions ----------------------------------*/
/*
 * These functions need to use bit masks and shifts to extract fields
 * from the rx descriptors, rather than directly using the bitfields.
 * For example, use
 *     (desc & FIELD_MASK) >> FIELD_LSB
 * rather than
 *     desc.field
 * This allows the functions to work correctly on either little-endian
 * machines (no endianness conversion needed) or big-endian machines
 * (endianness conversion provided automatically by the HW DMA's
 * byte-swizzling).
 */
/* FIX THIS: APPLIES TO LL ONLY */

#ifndef CONFIG_HL_SUPPORT
/**
 * htt_rx_mpdu_desc_retry_ll() - Returns the retry bit from the Rx descriptor
 *                               for the Low Latency driver
 * @pdev:                          Handle (pointer) to HTT pdev.
 * @mpdu_desc:                     Void pointer to the Rx descriptor for MPDU
 *                                 before the beginning of the payload.
 *
 *  This function returns the retry bit of the 802.11 header for the
 *  provided rx MPDU descriptor.
 *
 * Return:        boolean -- true if retry is set, false otherwise
 */
static bool
htt_rx_mpdu_desc_retry_ll(htt_pdev_handle pdev, void *mpdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *) mpdu_desc;

	return
		(bool)(((*((uint32_t *) &rx_desc->mpdu_start)) &
		RX_MPDU_START_0_RETRY_MASK) >>
		RX_MPDU_START_0_RETRY_LSB);
}

static uint16_t htt_rx_mpdu_desc_seq_num_ll(htt_pdev_handle pdev,
					    void *mpdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)mpdu_desc;

	return
		(uint16_t) (((*((uint32_t *) &rx_desc->mpdu_start)) &
			     RX_MPDU_START_0_SEQ_NUM_MASK) >>
			    RX_MPDU_START_0_SEQ_NUM_LSB);
}

/* FIX THIS: APPLIES TO LL ONLY */
static void
htt_rx_mpdu_desc_pn_ll(htt_pdev_handle pdev,
		       void *mpdu_desc, union htt_rx_pn_t *pn, int pn_len_bits)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)mpdu_desc;

	switch (pn_len_bits) {
	case 24:
		/* bits 23:0 */
		pn->pn24 = rx_desc->mpdu_start.pn_31_0 & 0xffffff;
		break;
	case 48:
		/* bits 31:0 */
		pn->pn48 = rx_desc->mpdu_start.pn_31_0;
		/* bits 47:32 */
		pn->pn48 |= ((uint64_t)
			     ((*(((uint32_t *) &rx_desc->mpdu_start) + 2))
			      & RX_MPDU_START_2_PN_47_32_MASK))
			<< (32 - RX_MPDU_START_2_PN_47_32_LSB);
		break;
	case 128:
		/* bits 31:0 */
		pn->pn128[0] = rx_desc->mpdu_start.pn_31_0;
		/* bits 47:32 */
		pn->pn128[0] |=
			((uint64_t) ((*(((uint32_t *)&rx_desc->mpdu_start) + 2))
				     & RX_MPDU_START_2_PN_47_32_MASK))
			<< (32 - RX_MPDU_START_2_PN_47_32_LSB);
		/* bits 63:48 */
		pn->pn128[0] |=
			((uint64_t) ((*(((uint32_t *) &rx_desc->msdu_end) + 2))
				     & RX_MSDU_END_1_EXT_WAPI_PN_63_48_MASK))
			<< (48 - RX_MSDU_END_1_EXT_WAPI_PN_63_48_LSB);
		/* bits 95:64 */
		pn->pn128[1] = rx_desc->msdu_end.ext_wapi_pn_95_64;
		/* bits 127:96 */
		pn->pn128[1] |=
			((uint64_t) rx_desc->msdu_end.ext_wapi_pn_127_96) << 32;
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "Error: invalid length spec (%d bits) for PN",
			  pn_len_bits);
	};
}

/**
 * htt_rx_mpdu_desc_tid_ll() - Returns the TID value from the Rx descriptor
 *                             for Low Latency driver
 * @pdev:                        Handle (pointer) to HTT pdev.
 * @mpdu_desc:                   Void pointer to the Rx descriptor for the MPDU
 *                               before the beginning of the payload.
 *
 * This function returns the TID set in the 802.11 QoS Control for the MPDU
 * in the packet header, by looking at the mpdu_start of the Rx descriptor.
 * Rx descriptor gets a copy of the TID from the MAC.
 *
 * Return:        Actual TID set in the packet header.
 */
static uint8_t
htt_rx_mpdu_desc_tid_ll(htt_pdev_handle pdev, void *mpdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *) mpdu_desc;

	return
		(uint8_t)(((*(((uint32_t *) &rx_desc->mpdu_start) + 2)) &
		RX_MPDU_START_2_TID_MASK) >>
		RX_MPDU_START_2_TID_LSB);
}

/* FIX THIS: APPLIES TO LL ONLY */
static bool htt_rx_msdu_desc_completes_mpdu_ll(htt_pdev_handle pdev,
					       void *msdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)msdu_desc;
	return (bool)
		(((*(((uint32_t *) &rx_desc->msdu_end) + 4)) &
		  RX_MSDU_END_4_LAST_MSDU_MASK) >> RX_MSDU_END_4_LAST_MSDU_LSB);
}

/* FIX THIS: APPLIES TO LL ONLY */
static int htt_rx_msdu_has_wlan_mcast_flag_ll(htt_pdev_handle pdev,
					      void *msdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)msdu_desc;
	/*
	 * HW rx desc: the mcast_bcast flag is only valid
	 * if first_msdu is set
	 */
	return
		((*(((uint32_t *) &rx_desc->msdu_end) + 4)) &
		 RX_MSDU_END_4_FIRST_MSDU_MASK) >> RX_MSDU_END_4_FIRST_MSDU_LSB;
}

/* FIX THIS: APPLIES TO LL ONLY */
static bool htt_rx_msdu_is_wlan_mcast_ll(htt_pdev_handle pdev, void *msdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)msdu_desc;
	return
		((*((uint32_t *) &rx_desc->attention)) &
		 RX_ATTENTION_0_MCAST_BCAST_MASK)
		>> RX_ATTENTION_0_MCAST_BCAST_LSB;
}

/* FIX THIS: APPLIES TO LL ONLY */
static int htt_rx_msdu_is_frag_ll(htt_pdev_handle pdev, void *msdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)msdu_desc;
	return
		((*((uint32_t *) &rx_desc->attention)) &
		 RX_ATTENTION_0_FRAGMENT_MASK) >> RX_ATTENTION_0_FRAGMENT_LSB;
}
#endif

static inline
uint8_t htt_rx_msdu_fw_desc_get(htt_pdev_handle pdev, void *msdu_desc)
{
	/*
	 * HL and LL use the same format for FW rx desc, but have the FW rx desc
	 * in different locations.
	 * In LL, the FW rx descriptor has been copied into the same
	 * htt_host_rx_desc_base struct that holds the HW rx desc.
	 * In HL, the FW rx descriptor, along with the MSDU payload,
	 * is in the same buffer as the rx indication message.
	 *
	 * Use the FW rx desc offset configured during startup to account for
	 * this difference between HL vs. LL.
	 *
	 * An optimization would be to define the LL and HL msdu_desc pointer
	 * in such a way that they both use the same offset to the FW rx desc.
	 * Then the following functions could be converted to macros, without
	 * needing to expose the htt_pdev_t definition outside HTT.
	 */
	return *(((uint8_t *) msdu_desc) + pdev->rx_fw_desc_offset);
}

int htt_rx_msdu_discard(htt_pdev_handle pdev, void *msdu_desc)
{
	return htt_rx_msdu_fw_desc_get(pdev, msdu_desc) & FW_RX_DESC_DISCARD_M;
}

int htt_rx_msdu_forward(htt_pdev_handle pdev, void *msdu_desc)
{
	return htt_rx_msdu_fw_desc_get(pdev, msdu_desc) & FW_RX_DESC_FORWARD_M;
}

int htt_rx_msdu_inspect(htt_pdev_handle pdev, void *msdu_desc)
{
	return htt_rx_msdu_fw_desc_get(pdev, msdu_desc) & FW_RX_DESC_INSPECT_M;
}

void
htt_rx_msdu_actions(htt_pdev_handle pdev,
		    void *msdu_desc, int *discard, int *forward, int *inspect)
{
	uint8_t rx_msdu_fw_desc = htt_rx_msdu_fw_desc_get(pdev, msdu_desc);
#ifdef HTT_DEBUG_DATA
	HTT_PRINT("act:0x%x ", rx_msdu_fw_desc);
#endif
	*discard = rx_msdu_fw_desc & FW_RX_DESC_DISCARD_M;
	*forward = rx_msdu_fw_desc & FW_RX_DESC_FORWARD_M;
	*inspect = rx_msdu_fw_desc & FW_RX_DESC_INSPECT_M;
}

static inline qdf_nbuf_t htt_rx_netbuf_pop(htt_pdev_handle pdev)
{
	int idx;
	qdf_nbuf_t msdu;

	HTT_ASSERT1(htt_rx_ring_elems(pdev) != 0);

#ifdef DEBUG_DMA_DONE
	pdev->rx_ring.dbg_ring_idx++;
	pdev->rx_ring.dbg_ring_idx &= pdev->rx_ring.size_mask;
#endif

	idx = pdev->rx_ring.sw_rd_idx.msdu_payld;
	msdu = pdev->rx_ring.buf.netbufs_ring[idx];
	idx++;
	idx &= pdev->rx_ring.size_mask;
	pdev->rx_ring.sw_rd_idx.msdu_payld = idx;
	pdev->rx_ring.fill_cnt--;
	return msdu;
}

/*
 * FIX ME: this function applies only to LL rx descs.
 * An equivalent for HL rx descs is needed.
 */
#ifdef CHECKSUM_OFFLOAD
static inline
void
htt_set_checksum_result_ll(htt_pdev_handle pdev, qdf_nbuf_t msdu,
			   struct htt_host_rx_desc_base *rx_desc)
{
#define MAX_IP_VER          2
#define MAX_PROTO_VAL       4
	struct rx_msdu_start *rx_msdu = &rx_desc->msdu_start;
	unsigned int proto = (rx_msdu->tcp_proto) | (rx_msdu->udp_proto << 1);

	/*
	 * HW supports TCP & UDP checksum offload for ipv4 and ipv6
	 */
	static const qdf_nbuf_l4_rx_cksum_type_t
		cksum_table[][MAX_PROTO_VAL][MAX_IP_VER] = {
		{
			/* non-fragmented IP packet */
			/* non TCP/UDP packet */
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
			/* TCP packet */
			{QDF_NBUF_RX_CKSUM_TCP, QDF_NBUF_RX_CKSUM_TCPIPV6},
			/* UDP packet */
			{QDF_NBUF_RX_CKSUM_UDP, QDF_NBUF_RX_CKSUM_UDPIPV6},
			/* invalid packet type */
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
		},
		{
			/* fragmented IP packet */
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
			{QDF_NBUF_RX_CKSUM_ZERO, QDF_NBUF_RX_CKSUM_ZERO},
		}
	};

	qdf_nbuf_rx_cksum_t cksum = {
		cksum_table[rx_msdu->ip_frag][proto][rx_msdu->ipv6_proto],
		QDF_NBUF_RX_CKSUM_NONE,
		0
	};

	if (cksum.l4_type !=
	    (qdf_nbuf_l4_rx_cksum_type_t) QDF_NBUF_RX_CKSUM_NONE) {
		cksum.l4_result =
			((*(uint32_t *) &rx_desc->attention) &
			 RX_ATTENTION_0_TCP_UDP_CHKSUM_FAIL_MASK) ?
			QDF_NBUF_RX_CKSUM_NONE :
			QDF_NBUF_RX_CKSUM_TCP_UDP_UNNECESSARY;
	}
	qdf_nbuf_set_rx_cksum(msdu, &cksum);
#undef MAX_IP_VER
#undef MAX_PROTO_VAL
}

#if defined(CONFIG_HL_SUPPORT)

static void
htt_set_checksum_result_hl(qdf_nbuf_t msdu,
			   struct htt_host_rx_desc_base *rx_desc)
{
	u_int8_t flag = ((u_int8_t *)rx_desc -
				sizeof(struct hl_htt_rx_ind_base))[
					HTT_ENDIAN_BYTE_IDX_SWAP(
						HTT_RX_IND_HL_FLAG_OFFSET)];

	int is_ipv6 = flag & HTT_RX_IND_HL_FLAG_IPV6 ? 1 : 0;
	int is_tcp = flag & HTT_RX_IND_HL_FLAG_TCP ? 1 : 0;
	int is_udp = flag & HTT_RX_IND_HL_FLAG_UDP ? 1 : 0;

	qdf_nbuf_rx_cksum_t cksum = {
		QDF_NBUF_RX_CKSUM_NONE,
		QDF_NBUF_RX_CKSUM_NONE,
		0
	};

	switch ((is_udp << 2) | (is_tcp << 1) | (is_ipv6 << 0)) {
	case 0x4:
		cksum.l4_type = QDF_NBUF_RX_CKSUM_UDP;
		break;
	case 0x2:
		cksum.l4_type = QDF_NBUF_RX_CKSUM_TCP;
		break;
	case 0x5:
		cksum.l4_type = QDF_NBUF_RX_CKSUM_UDPIPV6;
		break;
	case 0x3:
		cksum.l4_type = QDF_NBUF_RX_CKSUM_TCPIPV6;
		break;
	default:
		cksum.l4_type = QDF_NBUF_RX_CKSUM_NONE;
		break;
	}
	if (cksum.l4_type != (qdf_nbuf_l4_rx_cksum_type_t)
				QDF_NBUF_RX_CKSUM_NONE) {
		cksum.l4_result = flag & HTT_RX_IND_HL_FLAG_C4_FAILED ?
			QDF_NBUF_RX_CKSUM_NONE :
				QDF_NBUF_RX_CKSUM_TCP_UDP_UNNECESSARY;
	}
	qdf_nbuf_set_rx_cksum(msdu, &cksum);
}
#endif

#else

static inline
void htt_set_checksum_result_ll(htt_pdev_handle pdev, qdf_nbuf_t msdu,
			   struct htt_host_rx_desc_base *rx_desc)
{
}

#if defined(CONFIG_HL_SUPPORT)

static inline
void htt_set_checksum_result_hl(qdf_nbuf_t msdu,
			   struct htt_host_rx_desc_base *rx_desc)
{
}
#endif

#endif

#ifdef DEBUG_DMA_DONE
#define MAX_DONE_BIT_CHECK_ITER 5
#endif

#ifndef CONFIG_HL_SUPPORT
static int
htt_rx_amsdu_pop_ll(htt_pdev_handle pdev,
		    qdf_nbuf_t rx_ind_msg,
		    qdf_nbuf_t *head_msdu, qdf_nbuf_t *tail_msdu,
		    uint32_t *msdu_count)
{
	int msdu_len, msdu_chaining = 0;
	qdf_nbuf_t msdu;
	struct htt_host_rx_desc_base *rx_desc;
	uint8_t *rx_ind_data;
	uint32_t *msg_word, num_msdu_bytes;
	qdf_dma_addr_t rx_desc_paddr;
	enum htt_t2h_msg_type msg_type;
	uint8_t pad_bytes = 0;

	HTT_ASSERT1(htt_rx_ring_elems(pdev) != 0);
	rx_ind_data = qdf_nbuf_data(rx_ind_msg);
	msg_word = (uint32_t *) rx_ind_data;

	msg_type = HTT_T2H_MSG_TYPE_GET(*msg_word);

	if (qdf_unlikely(HTT_T2H_MSG_TYPE_RX_FRAG_IND == msg_type)) {
		num_msdu_bytes = HTT_RX_FRAG_IND_FW_RX_DESC_BYTES_GET(
			*(msg_word + HTT_RX_FRAG_IND_HDR_PREFIX_SIZE32));
	} else {
		num_msdu_bytes = HTT_RX_IND_FW_RX_DESC_BYTES_GET(
			*(msg_word
			  + HTT_RX_IND_HDR_PREFIX_SIZE32
			  + HTT_RX_PPDU_DESC_SIZE32));
	}
	msdu = *head_msdu = htt_rx_netbuf_pop(pdev);
	while (1) {
		int last_msdu, msdu_len_invalid, msdu_chained;
		int byte_offset;
		qdf_nbuf_t next;

		/*
		 * Set the netbuf length to be the entire buffer length
		 * initially, so the unmap will unmap the entire buffer.
		 */
		qdf_nbuf_set_pktlen(msdu, HTT_RX_BUF_SIZE);
#ifdef DEBUG_DMA_DONE
		qdf_nbuf_unmap(pdev->osdev, msdu, QDF_DMA_BIDIRECTIONAL);
#else
		qdf_nbuf_unmap(pdev->osdev, msdu, QDF_DMA_FROM_DEVICE);
#endif

		/* cache consistency has been taken care of by qdf_nbuf_unmap */

		/*
		 * Now read the rx descriptor.
		 * Set the length to the appropriate value.
		 * Check if this MSDU completes a MPDU.
		 */
		rx_desc = htt_rx_desc(msdu);
#if defined(HELIUMPLUS)
		if (HTT_WIFI_IP(pdev, 2, 0))
			pad_bytes = rx_desc->msdu_end.l3_header_padding;
#endif /* defined(HELIUMPLUS) */

		/*
		 * Save PADDR of descriptor and make the netbuf's data pointer
		 * point to the payload rather than the descriptor.
		 */
		rx_desc_paddr = QDF_NBUF_CB_PADDR(msdu);
		qdf_nbuf_pull_head(msdu, HTT_RX_STD_DESC_RESERVATION +
					 pad_bytes);

		/*
		 * Sanity check - confirm the HW is finished filling in
		 * the rx data.
		 * If the HW and SW are working correctly, then it's guaranteed
		 * that the HW's MAC DMA is done before this point in the SW.
		 * To prevent the case that we handle a stale Rx descriptor,
		 * just assert for now until we have a way to recover.
		 */

#ifdef DEBUG_DMA_DONE
		if (qdf_unlikely(!((*(uint32_t *) &rx_desc->attention)
				   & RX_ATTENTION_0_MSDU_DONE_MASK))) {

			int dbg_iter = MAX_DONE_BIT_CHECK_ITER;

			QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
				  "malformed frame");

			while (dbg_iter &&
			       (!((*(uint32_t *) &rx_desc->attention) &
				  RX_ATTENTION_0_MSDU_DONE_MASK))) {
				qdf_mdelay(1);
				qdf_mem_dma_sync_single_for_cpu(
					pdev->osdev,
					rx_desc_paddr,
					HTT_RX_STD_DESC_RESERVATION,
					DMA_FROM_DEVICE);

				QDF_TRACE(QDF_MODULE_ID_HTT,
					  QDF_TRACE_LEVEL_INFO,
					  "debug iter %d success %d", dbg_iter,
					  pdev->rx_ring.dbg_sync_success);

				dbg_iter--;
			}

			if (qdf_unlikely(!((*(uint32_t *) &rx_desc->attention)
					   & RX_ATTENTION_0_MSDU_DONE_MASK))) {

#ifdef HTT_RX_RESTORE
				QDF_TRACE(QDF_MODULE_ID_HTT,
					  QDF_TRACE_LEVEL_ERROR,
					  "RX done bit error detected!");

				qdf_nbuf_set_next(msdu, NULL);
				*tail_msdu = msdu;
				pdev->rx_ring.rx_reset = 1;
				return msdu_chaining;
#else
				wma_cli_set_command(0, GEN_PARAM_CRASH_INJECT,
						    0, GEN_CMD);
				HTT_ASSERT_ALWAYS(0);
#endif
			}
			pdev->rx_ring.dbg_sync_success++;
			QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_INFO,
				  "debug iter %d success %d", dbg_iter,
				  pdev->rx_ring.dbg_sync_success);
		}
#else
		HTT_ASSERT_ALWAYS((*(uint32_t *) &rx_desc->attention) &
				  RX_ATTENTION_0_MSDU_DONE_MASK);
#endif
		/*
		 * Copy the FW rx descriptor for this MSDU from the rx
		 * indication message into the MSDU's netbuf.
		 * HL uses the same rx indication message definition as LL, and
		 * simply appends new info (fields from the HW rx desc, and the
		 * MSDU payload itself).
		 * So, the offset into the rx indication message only has to
		 * account for the standard offset of the per-MSDU FW rx
		 * desc info within the message, and how many bytes of the
		 * per-MSDU FW rx desc info have already been consumed.
		 * (And the endianness of the host,
		 * since for a big-endian host, the rx ind message contents,
		 * including the per-MSDU rx desc bytes, were byteswapped during
		 * upload.)
		 */
		if (pdev->rx_ind_msdu_byte_idx < num_msdu_bytes) {
			if (qdf_unlikely
				    (HTT_T2H_MSG_TYPE_RX_FRAG_IND == msg_type))
				byte_offset =
					HTT_ENDIAN_BYTE_IDX_SWAP
					(HTT_RX_FRAG_IND_FW_DESC_BYTE_OFFSET);
			else
				byte_offset =
					HTT_ENDIAN_BYTE_IDX_SWAP
					(HTT_RX_IND_FW_RX_DESC_BYTE_OFFSET +
						pdev->rx_ind_msdu_byte_idx);

			*((uint8_t *) &rx_desc->fw_desc.u.val) =
				rx_ind_data[byte_offset];
			/*
			 * The target is expected to only provide the basic
			 * per-MSDU rx descriptors.  Just to be sure,
			 * verify that the target has not attached
			 * extension data (e.g. LRO flow ID).
			 */
			/*
			 * The assertion below currently doesn't work for
			 * RX_FRAG_IND messages, since their format differs
			 * from the RX_IND format (no FW rx PPDU desc in
			 * the current RX_FRAG_IND message).
			 * If the RX_FRAG_IND message format is updated to match
			 * the RX_IND message format, then the following
			 * assertion can be restored.
			 */
			/*
			 * qdf_assert((rx_ind_data[byte_offset] &
			 * FW_RX_DESC_EXT_M) == 0);
			 */
			pdev->rx_ind_msdu_byte_idx += 1;
			/* or more, if there's ext data */
		} else {
			/*
			 * When an oversized AMSDU happened, FW will lost some
			 * of MSDU status - in this case, the FW descriptors
			 * provided will be less than the actual MSDUs
			 * inside this MPDU.
			 * Mark the FW descriptors so that it will still
			 * deliver to upper stack, if no CRC error for the MPDU.
			 *
			 * FIX THIS - the FW descriptors are actually for MSDUs
			 * in the end of this A-MSDU instead of the beginning.
			 */
			*((uint8_t *) &rx_desc->fw_desc.u.val) = 0;
		}

		/*
		 *  TCP/UDP checksum offload support
		 */
		htt_set_checksum_result_ll(pdev, msdu, rx_desc);

		msdu_len_invalid = (*(uint32_t *) &rx_desc->attention) &
				   RX_ATTENTION_0_MPDU_LENGTH_ERR_MASK;
		msdu_chained = (((*(uint32_t *) &rx_desc->frag_info) &
				 RX_FRAG_INFO_0_RING2_MORE_COUNT_MASK) >>
				RX_FRAG_INFO_0_RING2_MORE_COUNT_LSB);
		msdu_len =
			((*((uint32_t *) &rx_desc->msdu_start)) &
			 RX_MSDU_START_0_MSDU_LENGTH_MASK) >>
			RX_MSDU_START_0_MSDU_LENGTH_LSB;

		do {
			if (!msdu_len_invalid && !msdu_chained) {
#if defined(PEREGRINE_1_0_ZERO_LEN_PHY_ERR_WAR)
				if (msdu_len > 0x3000)
					break;
#endif
				qdf_nbuf_trim_tail(msdu,
						   HTT_RX_BUF_SIZE -
						   (RX_STD_DESC_SIZE +
						    msdu_len));
			}
		} while (0);

		while (msdu_chained--) {
			next = htt_rx_netbuf_pop(pdev);
			qdf_nbuf_set_pktlen(next, HTT_RX_BUF_SIZE);
			msdu_len -= HTT_RX_BUF_SIZE;
			qdf_nbuf_set_next(msdu, next);
			msdu = next;
			msdu_chaining = 1;

			if (msdu_chained == 0) {
				/* Trim the last one to the correct size -
				 * accounting for inconsistent HW lengths
				 * causing length overflows and underflows
				 */
				if (((unsigned int)msdu_len) >
				    ((unsigned int)
				     (HTT_RX_BUF_SIZE - RX_STD_DESC_SIZE))) {
					msdu_len =
						(HTT_RX_BUF_SIZE -
						 RX_STD_DESC_SIZE);
				}

				qdf_nbuf_trim_tail(next,
						   HTT_RX_BUF_SIZE -
						   (RX_STD_DESC_SIZE +
						    msdu_len));
			}
		}

		last_msdu =
			((*(((uint32_t *) &rx_desc->msdu_end) + 4)) &
			 RX_MSDU_END_4_LAST_MSDU_MASK) >>
			RX_MSDU_END_4_LAST_MSDU_LSB;

		if (last_msdu) {
			qdf_nbuf_set_next(msdu, NULL);
			break;
		}

		next = htt_rx_netbuf_pop(pdev);
		qdf_nbuf_set_next(msdu, next);
		msdu = next;
	}
	*tail_msdu = msdu;

	/*
	 * Don't refill the ring yet.
	 * First, the elements popped here are still in use - it is
	 * not safe to overwrite them until the matching call to
	 * mpdu_desc_list_next.
	 * Second, for efficiency it is preferable to refill the rx ring
	 * with 1 PPDU's worth of rx buffers (something like 32 x 3 buffers),
	 * rather than one MPDU's worth of rx buffers (sth like 3 buffers).
	 * Consequently, we'll rely on the txrx SW to tell us when it is done
	 * pulling all the PPDU's rx buffers out of the rx ring, and then
	 * refill it just once.
	 */
	return msdu_chaining;
}
#endif

#if defined(CONFIG_HL_SUPPORT)

static int
htt_rx_amsdu_pop_hl(
	htt_pdev_handle pdev,
	qdf_nbuf_t rx_ind_msg,
	qdf_nbuf_t *head_msdu,
	qdf_nbuf_t *tail_msdu,
	uint32_t *msdu_count)
{
	pdev->rx_desc_size_hl =
		(qdf_nbuf_data(rx_ind_msg))
		[HTT_ENDIAN_BYTE_IDX_SWAP(
				HTT_RX_IND_HL_RX_DESC_LEN_OFFSET)];

	/* point to the rx desc */
	qdf_nbuf_pull_head(rx_ind_msg,
			   sizeof(struct hl_htt_rx_ind_base));
	*head_msdu = *tail_msdu = rx_ind_msg;

	htt_set_checksum_result_hl(rx_ind_msg,
				   (struct htt_host_rx_desc_base *)
				   (qdf_nbuf_data(rx_ind_msg)));

	qdf_nbuf_set_next(*tail_msdu, NULL);
	return 0;
}

static int
htt_rx_frag_pop_hl(
	htt_pdev_handle pdev,
	qdf_nbuf_t frag_msg,
	qdf_nbuf_t *head_msdu,
	qdf_nbuf_t *tail_msdu,
	uint32_t *msdu_count)
{
	qdf_nbuf_pull_head(frag_msg, HTT_RX_FRAG_IND_BYTES);
	pdev->rx_desc_size_hl =
		(qdf_nbuf_data(frag_msg))
		[HTT_ENDIAN_BYTE_IDX_SWAP(
				HTT_RX_IND_HL_RX_DESC_LEN_OFFSET)];

	/* point to the rx desc */
	qdf_nbuf_pull_head(frag_msg,
			   sizeof(struct hl_htt_rx_ind_base));
	*head_msdu = *tail_msdu = frag_msg;

	qdf_nbuf_set_next(*tail_msdu, NULL);
	return 0;
}

static inline int
htt_rx_offload_msdu_cnt_hl(
    htt_pdev_handle pdev)
{
    return 1;
}

static inline int
htt_rx_offload_msdu_pop_hl(htt_pdev_handle pdev,
			   qdf_nbuf_t offload_deliver_msg,
			   int *vdev_id,
			   int *peer_id,
			   int *tid,
			   u_int8_t *fw_desc,
			   qdf_nbuf_t *head_buf,
			   qdf_nbuf_t *tail_buf)
{
	qdf_nbuf_t buf;
	u_int32_t *msdu_hdr, msdu_len;
	int ret = 0;

	*head_buf = *tail_buf = buf = offload_deliver_msg;
	msdu_hdr = (u_int32_t *)qdf_nbuf_data(buf);
	/* First dword */

	/* Second dword */
	msdu_hdr++;
	msdu_len = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_LEN_GET(*msdu_hdr);
	*peer_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_PEER_ID_GET(*msdu_hdr);

	/* Third dword */
	msdu_hdr++;
	*vdev_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_VDEV_ID_GET(*msdu_hdr);
	*tid = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_TID_GET(*msdu_hdr);
	*fw_desc = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_DESC_GET(*msdu_hdr);

	qdf_nbuf_pull_head(buf, HTT_RX_OFFLOAD_DELIVER_IND_MSDU_HDR_BYTES
			+ HTT_RX_OFFLOAD_DELIVER_IND_HDR_BYTES);

	if (msdu_len <= qdf_nbuf_len(buf)) {
		qdf_nbuf_set_pktlen(buf, msdu_len);
	} else {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "%s: drop frame with invalid msdu len %d %d",
			  __func__, msdu_len, (int)qdf_nbuf_len(buf));
		qdf_nbuf_free(offload_deliver_msg);
		ret = -1;
	}

	return ret;
}
#endif

static inline int
htt_rx_offload_msdu_cnt_ll(
    htt_pdev_handle pdev)
{
    return htt_rx_ring_elems(pdev);
}

#ifndef CONFIG_HL_SUPPORT
static int
htt_rx_offload_msdu_pop_ll(htt_pdev_handle pdev,
			   qdf_nbuf_t offload_deliver_msg,
			   int *vdev_id,
			   int *peer_id,
			   int *tid,
			   uint8_t *fw_desc,
			   qdf_nbuf_t *head_buf, qdf_nbuf_t *tail_buf)
{
	qdf_nbuf_t buf;
	uint32_t *msdu_hdr, msdu_len;

	*head_buf = *tail_buf = buf = htt_rx_netbuf_pop(pdev);

	if (qdf_unlikely(NULL == buf)) {
		qdf_print("%s: netbuf pop failed!\n", __func__);
		return 1;
	}

	/* Fake read mpdu_desc to keep desc ptr in sync */
	htt_rx_mpdu_desc_list_next(pdev, NULL);
	qdf_nbuf_set_pktlen(buf, HTT_RX_BUF_SIZE);
#ifdef DEBUG_DMA_DONE
	qdf_nbuf_unmap(pdev->osdev, buf, QDF_DMA_BIDIRECTIONAL);
#else
	qdf_nbuf_unmap(pdev->osdev, buf, QDF_DMA_FROM_DEVICE);
#endif
	msdu_hdr = (uint32_t *) qdf_nbuf_data(buf);

	/* First dword */
	msdu_len = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_LEN_GET(*msdu_hdr);
	*peer_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_PEER_ID_GET(*msdu_hdr);

	/* Second dword */
	msdu_hdr++;
	*vdev_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_VDEV_ID_GET(*msdu_hdr);
	*tid = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_TID_GET(*msdu_hdr);
	*fw_desc = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_DESC_GET(*msdu_hdr);

	qdf_nbuf_pull_head(buf, HTT_RX_OFFLOAD_DELIVER_IND_MSDU_HDR_BYTES);
	qdf_nbuf_set_pktlen(buf, msdu_len);
	return 0;
}

int
htt_rx_offload_paddr_msdu_pop_ll(htt_pdev_handle pdev,
				 uint32_t *msg_word,
				 int msdu_iter,
				 int *vdev_id,
				 int *peer_id,
				 int *tid,
				 uint8_t *fw_desc,
				 qdf_nbuf_t *head_buf, qdf_nbuf_t *tail_buf)
{
	qdf_nbuf_t buf;
	uint32_t *msdu_hdr, msdu_len;
	uint32_t *curr_msdu;
	qdf_dma_addr_t paddr;

	curr_msdu =
		msg_word + (msdu_iter * HTT_RX_IN_ORD_PADDR_IND_MSDU_DWORDS);
	paddr = htt_rx_in_ord_paddr_get(curr_msdu);
	*head_buf = *tail_buf = buf = htt_rx_in_order_netbuf_pop(pdev, paddr);

	if (qdf_unlikely(NULL == buf)) {
		qdf_print("%s: netbuf pop failed!\n", __func__);
		return 1;
	}
	qdf_nbuf_set_pktlen(buf, HTT_RX_BUF_SIZE);
#ifdef DEBUG_DMA_DONE
	qdf_nbuf_unmap(pdev->osdev, buf, QDF_DMA_BIDIRECTIONAL);
#else
	qdf_nbuf_unmap(pdev->osdev, buf, QDF_DMA_FROM_DEVICE);
#endif

	if (pdev->cfg.is_first_wakeup_packet)
		htt_get_first_packet_after_wow_wakeup(
			msg_word + NEXT_FIELD_OFFSET_IN32, buf);

	msdu_hdr = (uint32_t *) qdf_nbuf_data(buf);

	/* First dword */
	msdu_len = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_LEN_GET(*msdu_hdr);
	*peer_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_PEER_ID_GET(*msdu_hdr);

	/* Second dword */
	msdu_hdr++;
	*vdev_id = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_VDEV_ID_GET(*msdu_hdr);
	*tid = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_TID_GET(*msdu_hdr);
	*fw_desc = HTT_RX_OFFLOAD_DELIVER_IND_MSDU_DESC_GET(*msdu_hdr);

	qdf_nbuf_pull_head(buf, HTT_RX_OFFLOAD_DELIVER_IND_MSDU_HDR_BYTES);
	qdf_nbuf_set_pktlen(buf, msdu_len);
	return 0;
}
#endif

uint32_t htt_rx_amsdu_rx_in_order_get_pktlog(qdf_nbuf_t rx_ind_msg)
{
	uint32_t *msg_word;

	msg_word = (uint32_t *) qdf_nbuf_data(rx_ind_msg);
	return HTT_RX_IN_ORD_PADDR_IND_PKTLOG_GET(*msg_word);
}

#ifndef CONFIG_HL_SUPPORT
/* Return values: 1 - success, 0 - failure */
#define RX_DESC_DISCARD_IS_SET ((*((u_int8_t *) &rx_desc->fw_desc.u.val)) & \
							FW_RX_DESC_DISCARD_M)
#define RX_DESC_MIC_ERR_IS_SET ((*((u_int8_t *) &rx_desc->fw_desc.u.val)) & \
							FW_RX_DESC_ANY_ERR_M)

static int
htt_rx_amsdu_rx_in_order_pop_ll(htt_pdev_handle pdev,
				qdf_nbuf_t rx_ind_msg,
				qdf_nbuf_t *head_msdu, qdf_nbuf_t *tail_msdu,
				uint32_t *replenish_cnt)
{
	qdf_nbuf_t msdu, next, prev = NULL;
	uint8_t *rx_ind_data;
	uint32_t *msg_word;
	uint32_t rx_ctx_id;
	unsigned int msdu_count = 0;
	uint8_t offload_ind, frag_ind;
	uint8_t peer_id;
	struct htt_host_rx_desc_base *rx_desc;
	enum rx_pkt_fate status = RX_PKT_FATE_SUCCESS;
	qdf_dma_addr_t paddr;
	qdf_mem_info_t mem_map_table = {0};
	int ret = 1;
	bool ipa_smmu = false;

	HTT_ASSERT1(htt_rx_in_order_ring_elems(pdev) != 0);

	rx_ind_data = qdf_nbuf_data(rx_ind_msg);
	rx_ctx_id = QDF_NBUF_CB_RX_CTX_ID(rx_ind_msg);
	msg_word = (uint32_t *) rx_ind_data;
	peer_id = HTT_RX_IN_ORD_PADDR_IND_PEER_ID_GET(
					*(u_int32_t *)rx_ind_data);

	offload_ind = HTT_RX_IN_ORD_PADDR_IND_OFFLOAD_GET(*msg_word);
	frag_ind = HTT_RX_IN_ORD_PADDR_IND_FRAG_GET(*msg_word);

	/* Get the total number of MSDUs */
	msdu_count = HTT_RX_IN_ORD_PADDR_IND_MSDU_CNT_GET(*(msg_word + 1));
	HTT_RX_CHECK_MSDU_COUNT(msdu_count);

	if (qdf_mem_smmu_s1_enabled(pdev->osdev) && pdev->is_ipa_uc_enabled &&
	    pdev->rx_ring.smmu_map)
		ipa_smmu = true;

	ol_rx_update_histogram_stats(msdu_count, frag_ind, offload_ind);
	htt_rx_dbg_rxbuf_httrxind(pdev, msdu_count);

	msg_word =
		(uint32_t *) (rx_ind_data + HTT_RX_IN_ORD_PADDR_IND_HDR_BYTES);
	if (offload_ind) {
		ol_rx_offload_paddr_deliver_ind_handler(pdev, msdu_count,
							msg_word);
		*head_msdu = *tail_msdu = NULL;
		ret = 0;
		goto end;
	}

	paddr = htt_rx_in_ord_paddr_get(msg_word);
	(*head_msdu) = msdu = htt_rx_in_order_netbuf_pop(pdev, paddr);

	if (qdf_unlikely(NULL == msdu)) {
		qdf_print("%s: netbuf pop failed!\n", __func__);
		*tail_msdu = NULL;
		pdev->rx_ring.pop_fail_cnt++;
		ret = 0;
		goto end;
	}

	while (msdu_count > 0) {
		if (ipa_smmu) {
			qdf_update_mem_map_table(pdev->osdev, &mem_map_table,
						 QDF_NBUF_CB_PADDR(msdu),
						 HTT_RX_BUF_SIZE);
			cds_smmu_map_unmap(false, 1, &mem_map_table);
		}

		/*
		 * Set the netbuf length to be the entire buffer length
		 * initially, so the unmap will unmap the entire buffer.
		 */
		qdf_nbuf_set_pktlen(msdu, HTT_RX_BUF_SIZE);
#ifdef DEBUG_DMA_DONE
		qdf_nbuf_unmap(pdev->osdev, msdu, QDF_DMA_BIDIRECTIONAL);
#else
		qdf_nbuf_unmap(pdev->osdev, msdu, QDF_DMA_FROM_DEVICE);
#endif

		/* cache consistency has been taken care of by qdf_nbuf_unmap */
		rx_desc = htt_rx_desc(msdu);
		htt_rx_extract_lro_info(msdu, rx_desc);

		/*
		 * Make the netbuf's data pointer point to the payload rather
		 * than the descriptor.
		 */
		qdf_nbuf_pull_head(msdu, HTT_RX_STD_DESC_RESERVATION);

		QDF_NBUF_CB_DP_TRACE_PRINT(msdu) = false;
		qdf_dp_trace_set_track(msdu, QDF_RX);
		QDF_NBUF_CB_TX_PACKET_TRACK(msdu) = QDF_NBUF_TX_PKT_DATA_TRACK;
		QDF_NBUF_CB_RX_CTX_ID(msdu) = rx_ctx_id;
		DPTRACE(qdf_dp_trace(msdu,
			QDF_DP_TRACE_RX_HTT_PACKET_PTR_RECORD,
			QDF_TRACE_DEFAULT_PDEV_ID,
			qdf_nbuf_data_addr(msdu),
			sizeof(qdf_nbuf_data(msdu)), QDF_RX));

		qdf_nbuf_trim_tail(msdu,
				   HTT_RX_BUF_SIZE -
				   (RX_STD_DESC_SIZE +
				    HTT_RX_IN_ORD_PADDR_IND_MSDU_LEN_GET(
				    *(msg_word + NEXT_FIELD_OFFSET_IN32))));
#if defined(HELIUMPLUS_DEBUG)
		ol_txrx_dump_pkt(msdu, 0, 64);
#endif
		*((uint8_t *) &rx_desc->fw_desc.u.val) =
			HTT_RX_IN_ORD_PADDR_IND_FW_DESC_GET(*(msg_word +
						NEXT_FIELD_OFFSET_IN32));

		msdu_count--;

		/* calling callback function for packet logging */
		if (pdev->rx_pkt_dump_cb) {
			if (qdf_unlikely(RX_DESC_MIC_ERR_IS_SET &&
					 !RX_DESC_DISCARD_IS_SET))
				status = RX_PKT_FATE_FW_DROP_INVALID;
			pdev->rx_pkt_dump_cb(msdu, peer_id, status);
		}

		if (pdev->cfg.is_first_wakeup_packet)
			htt_get_first_packet_after_wow_wakeup(
				msg_word + NEXT_FIELD_OFFSET_IN32, msdu);

		/* if discard flag is set (SA is self MAC), then
		 * don't check mic failure.
		 */
		if (qdf_unlikely(RX_DESC_MIC_ERR_IS_SET &&
					!RX_DESC_DISCARD_IS_SET)) {
			uint8_t tid =
				HTT_RX_IN_ORD_PADDR_IND_EXT_TID_GET(
					*(u_int32_t *)rx_ind_data);
			ol_rx_mic_error_handler(pdev->txrx_pdev, tid, peer_id,
						rx_desc, msdu);

			htt_rx_desc_frame_free(pdev, msdu);
			/* if this is the last msdu */
			if (!msdu_count) {
				/* if this is the only msdu */
				if (!prev) {
					*head_msdu = *tail_msdu = NULL;
					ret = 0;
					goto end;
				}
				*tail_msdu = prev;
				qdf_nbuf_set_next(prev, NULL);
				goto end;
			} else { /* if this is not the last msdu */
				/* get the next msdu */
				msg_word += HTT_RX_IN_ORD_PADDR_IND_MSDU_DWORDS;
				paddr = htt_rx_in_ord_paddr_get(msg_word);
				next = htt_rx_in_order_netbuf_pop(pdev, paddr);
				if (qdf_unlikely(NULL == next)) {
					qdf_print("%s: netbuf pop failed!\n",
								 __func__);
					*tail_msdu = NULL;
					pdev->rx_ring.pop_fail_cnt++;
					ret = 0;
					goto end;
				}

				/* if this is not the first msdu, update the
				 * next pointer of the preceding msdu
				 */
				if (prev) {
					qdf_nbuf_set_next(prev, next);
				} else {
					/* if this is the first msdu, update the
					 * head pointer
					 */
					*head_msdu = next;
				}
				msdu = next;
				continue;
			}
		}

		/* Update checksum result */
		htt_set_checksum_result_ll(pdev, msdu, rx_desc);

		/* check if this is the last msdu */
		if (msdu_count) {
			msg_word += HTT_RX_IN_ORD_PADDR_IND_MSDU_DWORDS;
			paddr = htt_rx_in_ord_paddr_get(msg_word);
			next = htt_rx_in_order_netbuf_pop(pdev, paddr);
			if (qdf_unlikely(NULL == next)) {
				qdf_print("%s: netbuf pop failed!\n",
					  __func__);
				*tail_msdu = NULL;
				pdev->rx_ring.pop_fail_cnt++;
				ret = 0;
				goto end;
			}
			qdf_nbuf_set_next(msdu, next);
			prev = msdu;
			msdu = next;
		} else {
			*tail_msdu = msdu;
			qdf_nbuf_set_next(msdu, NULL);
		}
	}

end:
	return ret;
}
#endif

int16_t htt_rx_mpdu_desc_rssi_dbm(htt_pdev_handle pdev, void *mpdu_desc)
{
	/*
	 * Currently the RSSI is provided only as a field in the
	 * HTT_T2H_RX_IND message, rather than in each rx descriptor.
	 */
	return HTT_RSSI_INVALID;
}

/*
 * htt_rx_amsdu_pop -
 * global function pointer that is programmed during attach to point
 * to either htt_rx_amsdu_pop_ll or htt_rx_amsdu_rx_in_order_pop_ll.
 */
int (*htt_rx_amsdu_pop)(htt_pdev_handle pdev,
			qdf_nbuf_t rx_ind_msg,
			qdf_nbuf_t *head_msdu, qdf_nbuf_t *tail_msdu,
			uint32_t *msdu_count);

/*
 * htt_rx_frag_pop -
 * global function pointer that is programmed during attach to point
 * to either htt_rx_amsdu_pop_ll
 */
int (*htt_rx_frag_pop)(htt_pdev_handle pdev,
		       qdf_nbuf_t rx_ind_msg,
		       qdf_nbuf_t *head_msdu, qdf_nbuf_t *tail_msdu,
		       uint32_t *msdu_count);

int
(*htt_rx_offload_msdu_cnt)(
    htt_pdev_handle pdev);

int
(*htt_rx_offload_msdu_pop)(htt_pdev_handle pdev,
			   qdf_nbuf_t offload_deliver_msg,
			   int *vdev_id,
			   int *peer_id,
			   int *tid,
			   uint8_t *fw_desc,
			   qdf_nbuf_t *head_buf, qdf_nbuf_t *tail_buf);

void * (*htt_rx_mpdu_desc_list_next)(htt_pdev_handle pdev,
				    qdf_nbuf_t rx_ind_msg);

bool (*htt_rx_mpdu_desc_retry)(htt_pdev_handle pdev, void *mpdu_desc);

uint16_t (*htt_rx_mpdu_desc_seq_num)(htt_pdev_handle pdev, void *mpdu_desc);

void (*htt_rx_mpdu_desc_pn)(htt_pdev_handle pdev,
			    void *mpdu_desc,
			    union htt_rx_pn_t *pn, int pn_len_bits);

uint8_t (*htt_rx_mpdu_desc_tid)(htt_pdev_handle pdev, void *mpdu_desc);

bool (*htt_rx_msdu_desc_completes_mpdu)(htt_pdev_handle pdev, void *msdu_desc);

bool (*htt_rx_msdu_first_msdu_flag)(htt_pdev_handle pdev, void *msdu_desc);

int (*htt_rx_msdu_has_wlan_mcast_flag)(htt_pdev_handle pdev, void *msdu_desc);

bool (*htt_rx_msdu_is_wlan_mcast)(htt_pdev_handle pdev, void *msdu_desc);

int (*htt_rx_msdu_is_frag)(htt_pdev_handle pdev, void *msdu_desc);

void * (*htt_rx_msdu_desc_retrieve)(htt_pdev_handle pdev, qdf_nbuf_t msdu);

bool (*htt_rx_mpdu_is_encrypted)(htt_pdev_handle pdev, void *mpdu_desc);

bool (*htt_rx_msdu_desc_key_id)(htt_pdev_handle pdev,
				void *mpdu_desc, uint8_t *key_id);

#ifndef CONFIG_HL_SUPPORT
static
void *htt_rx_mpdu_desc_list_next_ll(htt_pdev_handle pdev, qdf_nbuf_t rx_ind_msg)
{
	int idx = pdev->rx_ring.sw_rd_idx.msdu_desc;
	qdf_nbuf_t netbuf = pdev->rx_ring.buf.netbufs_ring[idx];

	pdev->rx_ring.sw_rd_idx.msdu_desc = pdev->rx_ring.sw_rd_idx.msdu_payld;
	return (void *)htt_rx_desc(netbuf);
}
#endif

bool (*htt_rx_msdu_chan_info_present)(
	htt_pdev_handle pdev,
	void *mpdu_desc);

bool (*htt_rx_msdu_center_freq)(
	htt_pdev_handle pdev,
	struct ol_txrx_peer_t *peer,
	void *mpdu_desc,
	uint16_t *primary_chan_center_freq_mhz,
	uint16_t *contig_chan1_center_freq_mhz,
	uint16_t *contig_chan2_center_freq_mhz,
	uint8_t *phy_mode);

#ifndef CONFIG_HL_SUPPORT
static void *htt_rx_in_ord_mpdu_desc_list_next_ll(htt_pdev_handle pdev,
						  qdf_nbuf_t netbuf)
{
	return (void *)htt_rx_desc(netbuf);
}
#endif

#if defined(CONFIG_HL_SUPPORT)

/**
 * htt_rx_mpdu_desc_list_next_hl() - provides an abstract way to obtain
 *				     the next MPDU descriptor
 * @pdev: the HTT instance the rx data was received on
 * @rx_ind_msg: the netbuf containing the rx indication message
 *
 * for HL, the returned value is not mpdu_desc,
 * it's translated hl_rx_desc just after the hl_ind_msg
 * for HL AMSDU, we can't point to payload now, because
 * hl rx desc is not fixed, we can't retrieve the desc
 * by minus rx_desc_size when release. keep point to hl rx desc
 * now
 *
 * Return: next abstract rx descriptor from the series of MPDUs
 *		   referenced by an rx ind msg
 */
static inline void *
htt_rx_mpdu_desc_list_next_hl(htt_pdev_handle pdev, qdf_nbuf_t rx_ind_msg)
{
	void *mpdu_desc = (void *)qdf_nbuf_data(rx_ind_msg);
	return mpdu_desc;
}

/**
 * htt_rx_msdu_desc_retrieve_hl() - Retrieve a previously-stored rx descriptor
 *				    from a MSDU buffer
 * @pdev: the HTT instance the rx data was received on
 * @msdu - the buffer containing the MSDU payload
 *
 * currently for HL AMSDU, we don't point to payload.
 * we shift to payload in ol_rx_deliver later
 *
 * Return: the corresponding abstract rx MSDU descriptor
 */
static inline void *
htt_rx_msdu_desc_retrieve_hl(htt_pdev_handle pdev, qdf_nbuf_t msdu)
{
	return qdf_nbuf_data(msdu);
}

static
bool htt_rx_mpdu_is_encrypted_hl(htt_pdev_handle pdev, void *mpdu_desc)
{
	if (htt_rx_msdu_first_msdu_flag_hl(pdev, mpdu_desc) == true) {
		/* Fix Me: only for little endian */
		struct hl_htt_rx_desc_base *rx_desc =
			(struct hl_htt_rx_desc_base *)mpdu_desc;

		return HTT_WORD_GET(*(u_int32_t *)rx_desc,
					HTT_HL_RX_DESC_MPDU_ENC);
	} else {
		/* not first msdu, no encrypt info for hl */
		qdf_print(
			"Error: get encrypted from a not-first msdu.\n");
		qdf_assert(0);
		return false;
	}
}

static inline bool
htt_rx_msdu_chan_info_present_hl(htt_pdev_handle pdev, void *mpdu_desc)
{
	if (htt_rx_msdu_first_msdu_flag_hl(pdev, mpdu_desc) == true &&
	    HTT_WORD_GET(*(u_int32_t *)mpdu_desc,
			 HTT_HL_RX_DESC_CHAN_INFO_PRESENT))
		return true;

	return false;
}

static bool
htt_rx_msdu_center_freq_hl(htt_pdev_handle pdev,
			   struct ol_txrx_peer_t *peer,
			   void *mpdu_desc,
			   uint16_t *primary_chan_center_freq_mhz,
			   uint16_t *contig_chan1_center_freq_mhz,
			   uint16_t *contig_chan2_center_freq_mhz,
			   uint8_t *phy_mode)
{
	int pn_len, index;
	uint32_t *chan_info;

	index = htt_rx_msdu_is_wlan_mcast(pdev, mpdu_desc) ?
		txrx_sec_mcast : txrx_sec_ucast;

	pn_len = (peer ?
			pdev->txrx_pdev->rx_pn[peer->security[index].sec_type].
								len : 0);
	chan_info = (uint32_t *)((uint8_t *)mpdu_desc +
			HTT_HL_RX_DESC_PN_OFFSET + pn_len);

	if (htt_rx_msdu_chan_info_present_hl(pdev, mpdu_desc)) {
		if (primary_chan_center_freq_mhz)
			*primary_chan_center_freq_mhz =
				HTT_WORD_GET(
					*chan_info,
					HTT_CHAN_INFO_PRIMARY_CHAN_CENTER_FREQ);
		if (contig_chan1_center_freq_mhz)
			*contig_chan1_center_freq_mhz =
				HTT_WORD_GET(
					*chan_info,
					HTT_CHAN_INFO_CONTIG_CHAN1_CENTER_FREQ);
		chan_info++;
		if (contig_chan2_center_freq_mhz)
			*contig_chan2_center_freq_mhz =
				HTT_WORD_GET(
					*chan_info,
					HTT_CHAN_INFO_CONTIG_CHAN2_CENTER_FREQ);
		if (phy_mode)
			*phy_mode =
				HTT_WORD_GET(*chan_info,
					     HTT_CHAN_INFO_PHY_MODE);
		return true;
	}

	if (primary_chan_center_freq_mhz)
		*primary_chan_center_freq_mhz = 0;
	if (contig_chan1_center_freq_mhz)
		*contig_chan1_center_freq_mhz = 0;
	if (contig_chan2_center_freq_mhz)
		*contig_chan2_center_freq_mhz = 0;
	if (phy_mode)
		*phy_mode = 0;
	return false;
}

static bool
htt_rx_msdu_desc_key_id_hl(htt_pdev_handle htt_pdev,
			   void *mpdu_desc, u_int8_t *key_id)
{
	if (htt_rx_msdu_first_msdu_flag_hl(htt_pdev, mpdu_desc) == true) {
		/* Fix Me: only for little endian */
		struct hl_htt_rx_desc_base *rx_desc =
			(struct hl_htt_rx_desc_base *)mpdu_desc;

		*key_id = rx_desc->key_id_oct;
		return true;
	}

	return false;
}

#endif

#ifndef CONFIG_HL_SUPPORT
static void *htt_rx_msdu_desc_retrieve_ll(htt_pdev_handle pdev, qdf_nbuf_t msdu)
{
	return htt_rx_desc(msdu);
}

static bool htt_rx_mpdu_is_encrypted_ll(htt_pdev_handle pdev, void *mpdu_desc)
{
	struct htt_host_rx_desc_base *rx_desc =
		(struct htt_host_rx_desc_base *)mpdu_desc;

	return (((*((uint32_t *) &rx_desc->mpdu_start)) &
		 RX_MPDU_START_0_ENCRYPTED_MASK) >>
		RX_MPDU_START_0_ENCRYPTED_LSB) ? true : false;
}

static
bool htt_rx_msdu_chan_info_present_ll(htt_pdev_handle pdev, void *mpdu_desc)
{
	return false;
}

static bool htt_rx_msdu_center_freq_ll(htt_pdev_handle pdev,
	struct ol_txrx_peer_t *peer,
	void *mpdu_desc,
	uint16_t *primary_chan_center_freq_mhz,
	uint16_t *contig_chan1_center_freq_mhz,
	uint16_t *contig_chan2_center_freq_mhz,
	uint8_t *phy_mode)
{
	if (primary_chan_center_freq_mhz)
		*primary_chan_center_freq_mhz = 0;
	if (contig_chan1_center_freq_mhz)
		*contig_chan1_center_freq_mhz = 0;
	if (contig_chan2_center_freq_mhz)
		*contig_chan2_center_freq_mhz = 0;
	if (phy_mode)
		*phy_mode = 0;
	return false;
}

static bool
htt_rx_msdu_desc_key_id_ll(htt_pdev_handle pdev, void *mpdu_desc,
			   uint8_t *key_id)
{
	struct htt_host_rx_desc_base *rx_desc = (struct htt_host_rx_desc_base *)
						mpdu_desc;

	if (!htt_rx_msdu_first_msdu_flag_ll(pdev, mpdu_desc))
		return false;

	*key_id = ((*(((uint32_t *) &rx_desc->msdu_end) + 1)) &
		   (RX_MSDU_END_1_KEY_ID_OCT_MASK >>
		    RX_MSDU_END_1_KEY_ID_OCT_LSB));

	return true;
}
#endif

void htt_rx_desc_frame_free(htt_pdev_handle htt_pdev, qdf_nbuf_t msdu)
{
	qdf_nbuf_free(msdu);
}

void htt_rx_msdu_desc_free(htt_pdev_handle htt_pdev, qdf_nbuf_t msdu)
{
	/*
	 * The rx descriptor is in the same buffer as the rx MSDU payload,
	 * and does not need to be freed separately.
	 */
}

#if defined(CONFIG_HL_SUPPORT)

/**
 * htt_rx_fill_ring_count() - replenish rx msdu buffer
 * @pdev: Handle (pointer) to HTT pdev.
 *
 * This funciton will replenish the rx buffer to the max number
 * that can be kept in the ring
 *
 * Return: None
 */
static inline void htt_rx_fill_ring_count(htt_pdev_handle pdev)
{
}
#else

static void htt_rx_fill_ring_count(htt_pdev_handle pdev)
{
	int num_to_fill;

	num_to_fill = pdev->rx_ring.fill_level - pdev->rx_ring.fill_cnt;
	htt_rx_ring_fill_n(pdev, num_to_fill /* okay if <= 0 */);
}
#endif

void htt_rx_msdu_buff_replenish(htt_pdev_handle pdev)
{
	if (qdf_atomic_dec_and_test(&pdev->rx_ring.refill_ref_cnt))
		htt_rx_fill_ring_count(pdev);

	qdf_atomic_inc(&pdev->rx_ring.refill_ref_cnt);
}

#define RX_RING_REFILL_DEBT_MAX 128
int htt_rx_msdu_buff_in_order_replenish(htt_pdev_handle pdev, uint32_t num)
{
	int filled = 0;

	if (!qdf_spin_trylock_bh(&(pdev->rx_ring.refill_lock))) {
		if (qdf_atomic_read(&pdev->rx_ring.refill_debt)
			 < RX_RING_REFILL_DEBT_MAX) {
			qdf_atomic_add(num, &pdev->rx_ring.refill_debt);
			pdev->rx_buff_debt_invoked++;
			return filled; /* 0 */
		}
		/*
		 * else:
		 * If we have quite a debt, then it is better for the lock
		 * holder to finish its work and then acquire the lock and
		 * fill our own part.
		 */
		qdf_spin_lock_bh(&(pdev->rx_ring.refill_lock));
	}
	pdev->rx_buff_fill_n_invoked++;

	filled = htt_rx_ring_fill_n(pdev, num);

	if (filled > num) {
		/* we served ourselves and some other debt */
		/* sub is safer than  = 0 */
		qdf_atomic_sub(filled - num, &pdev->rx_ring.refill_debt);
	} else {
		qdf_atomic_add(num - filled, &pdev->rx_ring.refill_debt);
	}
	qdf_spin_unlock_bh(&(pdev->rx_ring.refill_lock));

	return filled;
}

#ifndef CONFIG_HL_SUPPORT
#define AR600P_ASSEMBLE_HW_RATECODE(_rate, _nss, _pream)     \
	(((_pream) << 6) | ((_nss) << 4) | (_rate))

enum AR600P_HW_RATECODE_PREAM_TYPE {
	AR600P_HW_RATECODE_PREAM_OFDM,
	AR600P_HW_RATECODE_PREAM_CCK,
	AR600P_HW_RATECODE_PREAM_HT,
	AR600P_HW_RATECODE_PREAM_VHT,
};

/*--- RX In Order Hash Code --------------------------------------------------*/

/* Initializes the circular linked list */
static inline void htt_list_init(struct htt_list_node *head)
{
	head->prev = head;
	head->next = head;
}

/* Adds entry to the end of the linked list */
static inline void htt_list_add_tail(struct htt_list_node *head,
				     struct htt_list_node *node)
{
	head->prev->next = node;
	node->prev = head->prev;
	node->next = head;
	head->prev = node;
}

/* Removes the entry corresponding to the input node from the linked list */
static inline void htt_list_remove(struct htt_list_node *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

/* Helper macro to iterate through the linked list */
#define HTT_LIST_ITER_FWD(iter, head) for (iter = (head)->next;		\
					   (iter) != (head);		\
					   (iter) = (iter)->next)	\

#ifdef RX_HASH_DEBUG
/* Hash cookie related macros */
#define HTT_RX_HASH_COOKIE 0xDEED

#define HTT_RX_HASH_COOKIE_SET(hash_element) \
	((hash_element)->cookie = HTT_RX_HASH_COOKIE)

#define HTT_RX_HASH_COOKIE_CHECK(hash_element) \
	HTT_ASSERT_ALWAYS((hash_element)->cookie == HTT_RX_HASH_COOKIE)

/* Hash count related macros */
#define HTT_RX_HASH_COUNT_INCR(hash_bucket) \
	((hash_bucket)->count++)

#define HTT_RX_HASH_COUNT_DECR(hash_bucket) \
	((hash_bucket)->count--)

#define HTT_RX_HASH_COUNT_RESET(hash_bucket) ((hash_bucket)->count = 0)

#define HTT_RX_HASH_COUNT_PRINT(hash_bucket) \
	RX_HASH_LOG(qdf_print(" count %d\n", (hash_bucket)->count))
#else                           /* RX_HASH_DEBUG */
/* Hash cookie related macros */
#define HTT_RX_HASH_COOKIE_SET(hash_element)    /* no-op */
#define HTT_RX_HASH_COOKIE_CHECK(hash_element)  /* no-op */
/* Hash count related macros */
#define HTT_RX_HASH_COUNT_INCR(hash_bucket)     /* no-op */
#define HTT_RX_HASH_COUNT_DECR(hash_bucket)     /* no-op */
#define HTT_RX_HASH_COUNT_PRINT(hash_bucket)    /* no-op */
#define HTT_RX_HASH_COUNT_RESET(hash_bucket)    /* no-op */
#endif /* RX_HASH_DEBUG */

/*
 * Inserts the given "physical address - network buffer" pair into the
 * hash table for the given pdev. This function will do the following:
 * 1. Determine which bucket to insert the pair into
 * 2. First try to allocate the hash entry for this pair from the pre-allocated
 *    entries list
 * 3. If there are no more entries in the pre-allocated entries list, allocate
 *    the hash entry from the hash memory pool
 * Note: this function is not thread-safe
 * Returns 0 - success, 1 - failure
 */
int
htt_rx_hash_list_insert(struct htt_pdev_t *pdev,
			qdf_dma_addr_t paddr,
			qdf_nbuf_t netbuf)
{
	int i;
	int rc = 0;
	struct htt_rx_hash_entry *hash_element = NULL;

	qdf_spin_lock_bh(&(pdev->rx_ring.rx_hash_lock));

	/* get rid of the marking bits if they are available */
	paddr = htt_paddr_trim_to_37(paddr);

	i = RX_HASH_FUNCTION(paddr);

	/* Check if there are any entries in the pre-allocated free list */
	if (pdev->rx_ring.hash_table[i]->freepool.next !=
	    &pdev->rx_ring.hash_table[i]->freepool) {

		hash_element =
			(struct htt_rx_hash_entry *)(
				(char *)
				pdev->rx_ring.hash_table[i]->freepool.next -
				pdev->rx_ring.listnode_offset);
		if (qdf_unlikely(NULL == hash_element)) {
			HTT_ASSERT_ALWAYS(0);
			rc = 1;
			goto hli_end;
		}

		htt_list_remove(pdev->rx_ring.hash_table[i]->freepool.next);
	} else {
		hash_element = qdf_mem_malloc(sizeof(struct htt_rx_hash_entry));
		if (qdf_unlikely(NULL == hash_element)) {
			HTT_ASSERT_ALWAYS(0);
			rc = 1;
			goto hli_end;
		}
		hash_element->fromlist = 0;
	}

	hash_element->netbuf = netbuf;
	hash_element->paddr = paddr;
	HTT_RX_HASH_COOKIE_SET(hash_element);

	htt_list_add_tail(&pdev->rx_ring.hash_table[i]->listhead,
			  &hash_element->listnode);

	RX_HASH_LOG(qdf_print("rx hash: %s: paddr 0x%x netbuf %pK bucket %d\n",
			      __func__, paddr, netbuf, (int)i));

	HTT_RX_HASH_COUNT_INCR(pdev->rx_ring.hash_table[i]);
	HTT_RX_HASH_COUNT_PRINT(pdev->rx_ring.hash_table[i]);

hli_end:
	qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));
	return rc;
}
#endif

#ifndef CONFIG_HL_SUPPORT
/*
 * Given a physical address this function will find the corresponding network
 *  buffer from the hash table.
 *  paddr is already stripped off of higher marking bits.
 */
qdf_nbuf_t htt_rx_hash_list_lookup(struct htt_pdev_t *pdev,
				   qdf_dma_addr_t     paddr)
{
	uint32_t i;
	struct htt_list_node *list_iter = NULL;
	qdf_nbuf_t netbuf = NULL;
	struct htt_rx_hash_entry *hash_entry;

	qdf_spin_lock_bh(&(pdev->rx_ring.rx_hash_lock));

	if (!pdev->rx_ring.hash_table) {
		qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));
		return NULL;
	}

	i = RX_HASH_FUNCTION(paddr);

	HTT_LIST_ITER_FWD(list_iter, &pdev->rx_ring.hash_table[i]->listhead) {
		hash_entry = (struct htt_rx_hash_entry *)
			     ((char *)list_iter -
			      pdev->rx_ring.listnode_offset);

		HTT_RX_HASH_COOKIE_CHECK(hash_entry);

		if (hash_entry->paddr == paddr) {
			/* Found the entry corresponding to paddr */
			netbuf = hash_entry->netbuf;
			/* set netbuf to NULL to trace if freed entry
			 * is getting unmapped in hash deinit.
			 */
			hash_entry->netbuf = NULL;
			htt_list_remove(&hash_entry->listnode);
			HTT_RX_HASH_COUNT_DECR(pdev->rx_ring.hash_table[i]);
			/*
			 * if the rx entry is from the pre-allocated list,
			 * return it
			 */
			if (hash_entry->fromlist)
				htt_list_add_tail(
					&pdev->rx_ring.hash_table[i]->freepool,
					&hash_entry->listnode);
			else
				qdf_mem_free(hash_entry);

			htt_rx_dbg_rxbuf_reset(pdev, netbuf);
			break;
		}
	}

	RX_HASH_LOG(qdf_print("rx hash: %s: paddr 0x%x, netbuf %pK, bucket %d\n",
			      __func__, paddr, netbuf, (int)i));
	HTT_RX_HASH_COUNT_PRINT(pdev->rx_ring.hash_table[i]);

	qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));

	if (netbuf == NULL) {
		qdf_print("rx hash: %s: no entry found for %pK!\n",
			  __func__, (void *)paddr);
		if (cds_is_self_recovery_enabled())
			cds_trigger_recovery(QDF_RX_HASH_NO_ENTRY_FOUND);
		else
			HTT_ASSERT_ALWAYS(0);
	}

	return netbuf;
}

/*
 * Initialization function of the rx buffer hash table. This function will
 * allocate a hash table of a certain pre-determined size and initialize all
 * the elements
 */
static int htt_rx_hash_init(struct htt_pdev_t *pdev)
{
	int i, j;
	int rc = 0;
	void *allocation;

	HTT_ASSERT2(QDF_IS_PWR2(RX_NUM_HASH_BUCKETS));

	/* hash table is array of bucket pointers */
	pdev->rx_ring.hash_table =
		qdf_mem_malloc(RX_NUM_HASH_BUCKETS *
			       sizeof(struct htt_rx_hash_bucket *));

	if (NULL == pdev->rx_ring.hash_table) {
		qdf_print("rx hash table allocation failed!\n");
		return 1;
	}

	qdf_spinlock_create(&(pdev->rx_ring.rx_hash_lock));
	qdf_spin_lock_bh(&(pdev->rx_ring.rx_hash_lock));

	for (i = 0; i < RX_NUM_HASH_BUCKETS; i++) {

		qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));
		/* pre-allocate bucket and pool of entries for this bucket */
		allocation = qdf_mem_malloc((sizeof(struct htt_rx_hash_bucket) +
			(RX_ENTRIES_SIZE * sizeof(struct htt_rx_hash_entry))));
		qdf_spin_lock_bh(&(pdev->rx_ring.rx_hash_lock));
		pdev->rx_ring.hash_table[i] = allocation;


		HTT_RX_HASH_COUNT_RESET(pdev->rx_ring.hash_table[i]);

		/* initialize the hash table buckets */
		htt_list_init(&pdev->rx_ring.hash_table[i]->listhead);

		/* initialize the hash table free pool per bucket */
		htt_list_init(&pdev->rx_ring.hash_table[i]->freepool);

		/* pre-allocate a pool of entries for this bucket */
		pdev->rx_ring.hash_table[i]->entries =
			(struct htt_rx_hash_entry *)
			((uint8_t *)pdev->rx_ring.hash_table[i] +
			sizeof(struct htt_rx_hash_bucket));

		if (NULL == pdev->rx_ring.hash_table[i]->entries) {
			qdf_print("rx hash bucket %d entries alloc failed\n",
				(int)i);
			while (i) {
				i--;
				qdf_mem_free(pdev->rx_ring.hash_table[i]);
			}
			qdf_mem_free(pdev->rx_ring.hash_table);
			pdev->rx_ring.hash_table = NULL;
			rc = 1;
			goto hi_end;
		}

		/* initialize the free list with pre-allocated entries */
		for (j = 0; j < RX_ENTRIES_SIZE; j++) {
			pdev->rx_ring.hash_table[i]->entries[j].fromlist = 1;
			htt_list_add_tail(
				&pdev->rx_ring.hash_table[i]->freepool,
				&pdev->rx_ring.hash_table[i]->entries[j].
				listnode);
		}
	}

	pdev->rx_ring.listnode_offset =
		qdf_offsetof(struct htt_rx_hash_entry, listnode);
hi_end:
	qdf_spin_unlock_bh(&(pdev->rx_ring.rx_hash_lock));

	return rc;
}
#endif

/*--- RX In Order Hash Code --------------------------------------------------*/

/* move the function to the end of file
 * to omit ll/hl pre-declaration
 */

#if defined(CONFIG_HL_SUPPORT)

int htt_rx_attach(struct htt_pdev_t *pdev)
{
	pdev->rx_ring.size = HTT_RX_RING_SIZE_MIN;
	HTT_ASSERT2(IS_PWR2(pdev->rx_ring.size));
	pdev->rx_ring.size_mask = pdev->rx_ring.size - 1;
	/* host can force ring base address if it wish to do so */
	pdev->rx_ring.base_paddr = 0;
	htt_rx_amsdu_pop = htt_rx_amsdu_pop_hl;
	htt_rx_frag_pop = htt_rx_frag_pop_hl;
	htt_rx_offload_msdu_cnt = htt_rx_offload_msdu_cnt_hl;
	htt_rx_offload_msdu_pop = htt_rx_offload_msdu_pop_hl;
	htt_rx_mpdu_desc_list_next = htt_rx_mpdu_desc_list_next_hl;
	htt_rx_mpdu_desc_retry = htt_rx_mpdu_desc_retry_hl;
	htt_rx_mpdu_desc_seq_num = htt_rx_mpdu_desc_seq_num_hl;
	htt_rx_mpdu_desc_pn = htt_rx_mpdu_desc_pn_hl;
	htt_rx_mpdu_desc_tid = htt_rx_mpdu_desc_tid_hl;
	htt_rx_msdu_desc_completes_mpdu = htt_rx_msdu_desc_completes_mpdu_hl;
	htt_rx_msdu_first_msdu_flag = htt_rx_msdu_first_msdu_flag_hl;
	htt_rx_msdu_has_wlan_mcast_flag = htt_rx_msdu_has_wlan_mcast_flag_hl;
	htt_rx_msdu_is_wlan_mcast = htt_rx_msdu_is_wlan_mcast_hl;
	htt_rx_msdu_is_frag = htt_rx_msdu_is_frag_hl;
	htt_rx_msdu_desc_retrieve = htt_rx_msdu_desc_retrieve_hl;
	htt_rx_mpdu_is_encrypted = htt_rx_mpdu_is_encrypted_hl;
	htt_rx_msdu_desc_key_id = htt_rx_msdu_desc_key_id_hl;
	htt_rx_msdu_chan_info_present = htt_rx_msdu_chan_info_present_hl;
	htt_rx_msdu_center_freq = htt_rx_msdu_center_freq_hl;

	/*
	 * HL case, the rx descriptor can be different sizes for
	 * different sub-types of RX_IND messages, e.g. for the
	 * initial vs. interior vs. final MSDUs within a PPDU.
	 * The size of each RX_IND message's rx desc is read from
	 * a field within the RX_IND message itself.
	 * In the meantime, until the rx_desc_size_hl variable is
	 * set to its real value based on the RX_IND message,
	 * initialize it to a reasonable value (zero).
	 */
	pdev->rx_desc_size_hl = 0;
	return 0;	/* success */
}

#else

int htt_rx_attach(struct htt_pdev_t *pdev)
{
	qdf_dma_addr_t paddr;
	uint32_t ring_elem_size = sizeof(target_paddr_t);

	pdev->rx_ring.size = htt_rx_ring_size(pdev);
	HTT_ASSERT2(QDF_IS_PWR2(pdev->rx_ring.size));
	pdev->rx_ring.size_mask = pdev->rx_ring.size - 1;

	/*
	 * Set the initial value for the level to which the rx ring
	 * should be filled, based on the max throughput and the worst
	 * likely latency for the host to fill the rx ring.
	 * In theory, this fill level can be dynamically adjusted from
	 * the initial value set here to reflect the actual host latency
	 * rather than a conservative assumption.
	 */
	pdev->rx_ring.fill_level = htt_rx_ring_fill_level(pdev);

	if (pdev->cfg.is_full_reorder_offload) {
		if (htt_rx_hash_init(pdev))
			goto fail1;

		/* allocate the target index */
		pdev->rx_ring.target_idx.vaddr =
			 qdf_mem_alloc_consistent(pdev->osdev, pdev->osdev->dev,
				 sizeof(uint32_t),
				 &paddr);

		if (!pdev->rx_ring.target_idx.vaddr)
			goto fail2;

		pdev->rx_ring.target_idx.paddr = paddr;
		*pdev->rx_ring.target_idx.vaddr = 0;
	} else {
		pdev->rx_ring.buf.netbufs_ring =
			qdf_mem_malloc(pdev->rx_ring.size * sizeof(qdf_nbuf_t));
		if (!pdev->rx_ring.buf.netbufs_ring)
			goto fail1;

		pdev->rx_ring.sw_rd_idx.msdu_payld = 0;
		pdev->rx_ring.sw_rd_idx.msdu_desc = 0;
	}

	pdev->rx_ring.buf.paddrs_ring =
		qdf_mem_alloc_consistent(
			pdev->osdev, pdev->osdev->dev,
			 pdev->rx_ring.size * ring_elem_size,
			 &paddr);
	if (!pdev->rx_ring.buf.paddrs_ring)
		goto fail3;

	pdev->rx_ring.base_paddr = paddr;
	pdev->rx_ring.alloc_idx.vaddr =
		 qdf_mem_alloc_consistent(
			pdev->osdev, pdev->osdev->dev,
			 sizeof(uint32_t), &paddr);

	if (!pdev->rx_ring.alloc_idx.vaddr)
		goto fail4;

	pdev->rx_ring.alloc_idx.paddr = paddr;
	*pdev->rx_ring.alloc_idx.vaddr = 0;

	/*
	 * Initialize the Rx refill reference counter to be one so that
	 * only one thread is allowed to refill the Rx ring.
	 */
	qdf_atomic_init(&pdev->rx_ring.refill_ref_cnt);
	qdf_atomic_inc(&pdev->rx_ring.refill_ref_cnt);

	/* Initialize the refill_lock and debt (for rx-parallelization) */
	qdf_spinlock_create(&(pdev->rx_ring.refill_lock));
	qdf_atomic_init(&pdev->rx_ring.refill_debt);


	/* Initialize the Rx refill retry timer */
	qdf_timer_init(pdev->osdev,
		 &pdev->rx_ring.refill_retry_timer,
		 htt_rx_ring_refill_retry, (void *)pdev,
		 QDF_TIMER_TYPE_SW);

	pdev->rx_ring.fill_cnt = 0;
	pdev->rx_ring.pop_fail_cnt = 0;
#ifdef DEBUG_DMA_DONE
	pdev->rx_ring.dbg_ring_idx = 0;
	pdev->rx_ring.dbg_refill_cnt = 0;
	pdev->rx_ring.dbg_sync_success = 0;
#endif
#ifdef HTT_RX_RESTORE
	pdev->rx_ring.rx_reset = 0;
	pdev->rx_ring.htt_rx_restore = 0;
#endif
	htt_rx_dbg_rxbuf_init(pdev);
	htt_rx_ring_fill_n(pdev, pdev->rx_ring.fill_level);

	if (pdev->cfg.is_full_reorder_offload) {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_INFO,
			"HTT: full reorder offload enabled");
		htt_rx_amsdu_pop = htt_rx_amsdu_rx_in_order_pop_ll;
		htt_rx_frag_pop = htt_rx_amsdu_rx_in_order_pop_ll;
		htt_rx_mpdu_desc_list_next =
			 htt_rx_in_ord_mpdu_desc_list_next_ll;
	} else {
		htt_rx_amsdu_pop = htt_rx_amsdu_pop_ll;
		htt_rx_frag_pop = htt_rx_amsdu_pop_ll;
		htt_rx_mpdu_desc_list_next = htt_rx_mpdu_desc_list_next_ll;
	}

	if (cds_get_conparam() == QDF_GLOBAL_MONITOR_MODE)
		htt_rx_amsdu_pop = htt_rx_mon_amsdu_rx_in_order_pop_ll;

	htt_rx_offload_msdu_cnt = htt_rx_offload_msdu_cnt_ll;
	htt_rx_offload_msdu_pop = htt_rx_offload_msdu_pop_ll;
	htt_rx_mpdu_desc_retry = htt_rx_mpdu_desc_retry_ll;
	htt_rx_mpdu_desc_seq_num = htt_rx_mpdu_desc_seq_num_ll;
	htt_rx_mpdu_desc_pn = htt_rx_mpdu_desc_pn_ll;
	htt_rx_mpdu_desc_tid = htt_rx_mpdu_desc_tid_ll;
	htt_rx_msdu_desc_completes_mpdu = htt_rx_msdu_desc_completes_mpdu_ll;
	htt_rx_msdu_first_msdu_flag = htt_rx_msdu_first_msdu_flag_ll;
	htt_rx_msdu_has_wlan_mcast_flag = htt_rx_msdu_has_wlan_mcast_flag_ll;
	htt_rx_msdu_is_wlan_mcast = htt_rx_msdu_is_wlan_mcast_ll;
	htt_rx_msdu_is_frag = htt_rx_msdu_is_frag_ll;
	htt_rx_msdu_desc_retrieve = htt_rx_msdu_desc_retrieve_ll;
	htt_rx_mpdu_is_encrypted = htt_rx_mpdu_is_encrypted_ll;
	htt_rx_msdu_desc_key_id = htt_rx_msdu_desc_key_id_ll;
	htt_rx_msdu_chan_info_present = htt_rx_msdu_chan_info_present_ll;
	htt_rx_msdu_center_freq = htt_rx_msdu_center_freq_ll;

	return 0;               /* success */

fail4:
	qdf_mem_free_consistent(pdev->osdev, pdev->osdev->dev,
				   pdev->rx_ring.size * sizeof(target_paddr_t),
				   pdev->rx_ring.buf.paddrs_ring,
				   pdev->rx_ring.base_paddr,
				   qdf_get_dma_mem_context((&pdev->rx_ring.buf),
							   memctx));

fail3:
	if (pdev->cfg.is_full_reorder_offload)
		qdf_mem_free_consistent(pdev->osdev, pdev->osdev->dev,
					   sizeof(uint32_t),
					   pdev->rx_ring.target_idx.vaddr,
					   pdev->rx_ring.target_idx.paddr,
					   qdf_get_dma_mem_context((&pdev->
								    rx_ring.
								    target_idx),
								   memctx));
	else
		qdf_mem_free(pdev->rx_ring.buf.netbufs_ring);

fail2:
	if (pdev->cfg.is_full_reorder_offload)
		htt_rx_hash_deinit(pdev);

fail1:
	return 1;               /* failure */
}
#endif

#ifdef IPA_OFFLOAD
#ifdef QCA_WIFI_3_0
/**
 * htt_rx_ipa_uc_alloc_wdi2_rsc() - Allocate WDI2.0 resources
 * @pdev: htt context
 * @rx_ind_ring_elements: rx ring elements
 *
 * Return: 0 success
 */
static int htt_rx_ipa_uc_alloc_wdi2_rsc(struct htt_pdev_t *pdev,
			 unsigned int rx_ind_ring_elements)
{
	/*
	 * Allocate RX2 indication ring
	 * RX2 IND ring element
	 *   4bytes: pointer
	 *   2bytes: VDEV ID
	 *   2bytes: length
	 *
	 * RX indication ring size, by bytes
	 */
	pdev->ipa_uc_rx_rsc.rx2_ind_ring =
		qdf_mem_shared_mem_alloc(pdev->osdev,
					 rx_ind_ring_elements *
					 sizeof(qdf_dma_addr_t));
	if (!pdev->ipa_uc_rx_rsc.rx2_ind_ring) {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate memory for IPA rx2 ind ring",
			  __func__);
		return 1;
	}

	pdev->ipa_uc_rx_rsc.rx2_ipa_prc_done_idx =
		qdf_mem_shared_mem_alloc(pdev->osdev, 4);
	if (!pdev->ipa_uc_rx_rsc.rx2_ipa_prc_done_idx) {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate memory for IPA rx proc done index",
			  __func__);
		qdf_mem_shared_mem_free(pdev->osdev,
					pdev->ipa_uc_rx_rsc.rx2_ind_ring);
		return 1;
	}

	return 0;
}

/**
 * htt_rx_ipa_uc_free_wdi2_rsc() - Free WDI2.0 resources
 * @pdev: htt context
 *
 * Return: None
 */
static void htt_rx_ipa_uc_free_wdi2_rsc(struct htt_pdev_t *pdev)
{
	qdf_mem_shared_mem_free(pdev->osdev, pdev->ipa_uc_rx_rsc.rx2_ind_ring);
	qdf_mem_shared_mem_free(pdev->osdev,
				pdev->ipa_uc_rx_rsc.rx2_ipa_prc_done_idx);
}
#else
static int htt_rx_ipa_uc_alloc_wdi2_rsc(struct htt_pdev_t *pdev,
			 unsigned int rx_ind_ring_elements)
{
	return 0;
}

static void htt_rx_ipa_uc_free_wdi2_rsc(struct htt_pdev_t *pdev)
{
}
#endif

/**
 * htt_rx_ipa_uc_attach() - attach htt ipa uc rx resource
 * @pdev: htt context
 * @rx_ind_ring_size: rx ring size
 *
 * Return: 0 success
 */
int htt_rx_ipa_uc_attach(struct htt_pdev_t *pdev,
			 unsigned int rx_ind_ring_elements)
{
	int ret = 0;

	/*
	 * Allocate RX indication ring
	 * RX IND ring element
	 *   4bytes: pointer
	 *   2bytes: VDEV ID
	 *   2bytes: length
	 */
	pdev->ipa_uc_rx_rsc.rx_ind_ring =
		qdf_mem_shared_mem_alloc(pdev->osdev,
					 rx_ind_ring_elements *
					 sizeof(struct ipa_uc_rx_ring_elem_t));
	if (!pdev->ipa_uc_rx_rsc.rx_ind_ring) {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate memory for IPA rx ind ring",
			  __func__);
		return 1;
	}

	pdev->ipa_uc_rx_rsc.rx_ipa_prc_done_idx =
		qdf_mem_shared_mem_alloc(pdev->osdev, 4);
	if (!pdev->ipa_uc_rx_rsc.rx_ipa_prc_done_idx) {
		QDF_TRACE(QDF_MODULE_ID_HTT, QDF_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate memory for IPA rx proc done index",
			  __func__);
		qdf_mem_shared_mem_free(pdev->osdev,
					pdev->ipa_uc_rx_rsc.rx_ind_ring);
		return 1;
	}

	ret = htt_rx_ipa_uc_alloc_wdi2_rsc(pdev, rx_ind_ring_elements);
	if (ret) {
		qdf_mem_shared_mem_free(pdev->osdev, pdev->ipa_uc_rx_rsc.rx_ind_ring);
		qdf_mem_shared_mem_free(pdev->osdev,
					pdev->ipa_uc_rx_rsc.rx_ipa_prc_done_idx);
	}
	return ret;
}

int htt_rx_ipa_uc_detach(struct htt_pdev_t *pdev)
{
	qdf_mem_shared_mem_free(pdev->osdev, pdev->ipa_uc_rx_rsc.rx_ind_ring);
	qdf_mem_shared_mem_free(pdev->osdev,
				pdev->ipa_uc_rx_rsc.rx_ipa_prc_done_idx);

	htt_rx_ipa_uc_free_wdi2_rsc(pdev);
	return 0;
}
#endif /* IPA_OFFLOAD */

#ifndef REMOVE_PKT_LOG
/**
 * htt_register_rx_pkt_dump_callback() - registers callback to
 *   get rx pkt status and call callback to do rx packet dump
 *
 * @pdev: htt pdev handle
 * @callback: callback to get rx pkt status and
 *     call callback to do rx packet dump
 *
 * This function is used to register the callback to get
 * rx pkt status and call callback to do rx packet dump
 *
 * Return: None
 *
 */
void htt_register_rx_pkt_dump_callback(struct htt_pdev_t *pdev,
				tp_rx_pkt_dump_cb callback)
{
	if (!pdev) {
		qdf_print("%s: %s, %s",
			__func__,
			"htt pdev is NULL",
			"rx packet status callback register unsuccessful\n");
		return;
	}
	pdev->rx_pkt_dump_cb = callback;
}

/**
 * htt_deregister_rx_pkt_dump_callback() - deregisters callback to
 *   get rx pkt status and call callback to do rx packet dump
 *
 * @pdev: htt pdev handle
 *
 * This function is used to deregister the callback to get
 * rx pkt status and call callback to do rx packet dump
 *
 * Return: None
 *
 */
void htt_deregister_rx_pkt_dump_callback(struct htt_pdev_t *pdev)
{
	if (!pdev) {
		qdf_print("%s: %s, %s",
			__func__,
			"htt pdev is NULL",
			"rx packet status callback deregister unsuccessful\n");
		return;
	}
	pdev->rx_pkt_dump_cb = NULL;
}

static QDF_STATUS htt_rx_hash_smmu_map(bool map, struct htt_pdev_t *pdev)
{
	uint32_t i;
	struct htt_rx_hash_entry *hash_entry;
	struct htt_rx_hash_bucket **hash_table;
	struct htt_list_node *list_iter = NULL;
	qdf_mem_info_t mem_map_table = {0};
	int ret;

	qdf_spin_lock_bh(&pdev->rx_ring.rx_hash_lock);
	hash_table = pdev->rx_ring.hash_table;

	for (i = 0; i < RX_NUM_HASH_BUCKETS; i++) {
		/* Free the hash entries in hash bucket i */
		list_iter = hash_table[i]->listhead.next;
		while (list_iter != &hash_table[i]->listhead) {
			hash_entry =
				(struct htt_rx_hash_entry *)((char *)list_iter -
							     pdev->rx_ring.
							     listnode_offset);
			if (hash_entry->netbuf) {
				qdf_update_mem_map_table(pdev->osdev,
						&mem_map_table,
						QDF_NBUF_CB_PADDR(
							hash_entry->netbuf),
						HTT_RX_BUF_SIZE);
				ret = cds_smmu_map_unmap(map, 1,
							 &mem_map_table);
				if (ret) {
					qdf_spin_unlock_bh(
						&pdev->rx_ring.rx_hash_lock);
					return QDF_STATUS_E_FAILURE;
				}
			}
			list_iter = list_iter->next;
		}
	}
	qdf_spin_unlock_bh(&pdev->rx_ring.rx_hash_lock);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS htt_rx_update_smmu_map(struct htt_pdev_t *pdev, bool map)
{
	QDF_STATUS status;

	if (NULL == pdev->rx_ring.hash_table)
		return QDF_STATUS_SUCCESS;

	if (!qdf_mem_smmu_s1_enabled(pdev->osdev) || !pdev->is_ipa_uc_enabled)
		return QDF_STATUS_SUCCESS;

	qdf_spin_lock_bh(&pdev->rx_ring.refill_lock);
	pdev->rx_ring.smmu_map = map;
	status = htt_rx_hash_smmu_map(map, pdev);
	qdf_spin_unlock_bh(&pdev->rx_ring.refill_lock);

	return status;
}
#endif
