/*
 *  Copyright (c) 2016,2017 MediaTek Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/of.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btmtk_drv.h"
#include "btmtk_sdio.h"

/*
 * This function is called by interface specific interrupt handler.
 * It updates Power Save & Host Sleep states, and wakes up the main
 * thread.
 */
void btmtk_interrupt(struct btmtk_private *priv)
{
	priv->adapter->ps_state = PS_AWAKE;

	priv->adapter->wakeup_tries = 0;

	priv->adapter->int_count++;

	wake_up_interruptible(&priv->main_thread.wait_q);
}
EXPORT_SYMBOL_GPL(btmtk_interrupt);

int btmtk_enable_hs(struct btmtk_private *priv)
{
	struct btmtk_adapter *adapter = priv->adapter;
	int ret = 0;

	pr_info("%s begin\n", __func__);

	ret = wait_event_interruptible_timeout(adapter->event_hs_wait_q,
			adapter->hs_state,
			msecs_to_jiffies(WAIT_UNTIL_HS_STATE_CHANGED));
	if (ret < 0) {
		pr_err("event_hs_wait_q terminated (%d): %d,%d,%d\n",
			ret, adapter->hs_state, adapter->ps_state,
			adapter->wakeup_tries);

	} else {
		pr_debug("host sleep enabled: %d,%d,%d\n", adapter->hs_state,
			adapter->ps_state, adapter->wakeup_tries);
		ret = 0;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(btmtk_enable_hs);

static int btmtk_tx_pkt(struct btmtk_private *priv, struct sk_buff *skb)
{
	int ret = 0;
	u32 sdio_header_len = 0;

	if (!skb) {
		pr_warn("%s skb is NULL return -EINVAL\n", __func__);
		return -EINVAL;
	}

	pr_debug("%s skb->len %d\n", __func__, skb->len);

	if (!skb->data) {
		pr_warn("%s skb->data is NULL return -EINVAL\n", __func__);
		return -EINVAL;
	}

	if (!skb->len || ((skb->len + BTM_HEADER_LEN) > BTM_UPLD_SIZE)) {
		pr_warn("Tx Error: Bad skb length %d : %d\n",
						skb->len, BTM_UPLD_SIZE);
		return -EINVAL;
	}

	sdio_header_len = skb->len + BTM_HEADER_LEN;
	memset(txbuf, 0, MTK_TXDATA_SIZE);
	txbuf[0] = (sdio_header_len & 0x0000ff);
	txbuf[1] = (sdio_header_len & 0x00ff00) >> 8;
	txbuf[2] = 0;
	txbuf[3] = 0;
	txbuf[4] = bt_cb(skb)->pkt_type;
	memcpy(&txbuf[5], &skb->data[0], skb->len);
	if (priv->hw_host_to_card)
		ret = priv->hw_host_to_card(priv, txbuf, sdio_header_len);

	pr_debug("%s end\n", __func__);
	return ret;
}

static void btmtk_init_adapter(struct btmtk_private *priv)
{
	int buf_size;

	skb_queue_head_init(&priv->adapter->tx_queue);
	skb_queue_head_init(&priv->adapter->fops_queue);
	skb_queue_head_init(&priv->adapter->fwlog_fops_queue);
	priv->adapter->ps_state = PS_AWAKE;

	buf_size = ALIGN_SZ(SDIO_BLOCK_SIZE, BTSDIO_DMA_ALIGN);
	priv->adapter->hw_regs_buf = kzalloc(buf_size, GFP_KERNEL);
	if (!priv->adapter->hw_regs_buf) {
		priv->adapter->hw_regs = NULL;
		pr_err("Unable to allocate buffer for hw_regs.\n");
	} else {
		priv->adapter->hw_regs =
			(u8 *)ALIGN_ADDR(priv->adapter->hw_regs_buf,
					BTSDIO_DMA_ALIGN);
		pr_debug("hw_regs_buf=%p hw_regs=%p\n",
			priv->adapter->hw_regs_buf, priv->adapter->hw_regs);
	}

	init_waitqueue_head(&priv->adapter->cmd_wait_q);
	init_waitqueue_head(&priv->adapter->event_hs_wait_q);
}

static void btmtk_free_adapter(struct btmtk_private *priv)
{
	skb_queue_purge(&priv->adapter->tx_queue);

	kfree(priv->adapter->hw_regs_buf);
	kfree(priv->adapter);

	priv->adapter = NULL;
}

/*
 * This function handles the event generated by firmware, rx data
 * received from firmware, and tx data sent from kernel.
 */

static int btmtk_service_main_thread(void *data)
{
	struct btmtk_thread *thread = data;
	struct btmtk_private *priv = thread->priv;
	struct btmtk_adapter *adapter = NULL;
	wait_queue_t wait;
	struct sk_buff *skb;
	int ret = 0;
	int i = 0;
	ulong flags;

	pr_notice("main_thread begin 50\n");
	/* mdelay(50); */

	for (i = 0; i <= 1000; i++) {
		if (kthread_should_stop()) {
			pr_notice("main_thread: break from main thread for probe_ready\n");
			break;
		}

		if (probe_ready)
			break;

		pr_notice("%s probe_ready %d delay 10ms~15ms\n",
			__func__, probe_ready);
		usleep_range(10*1000, 15*1000);

		if (i == 1000) {
			pr_warn("%s probe_ready %d i = %d try too many times return\n",
				__func__, probe_ready, i);
			return 0;
		}
	}

	/*Set stp_sdio_tx_rx thread priority from 120 to 101*/
	set_user_nice(current, -19);

	if (priv->adapter)
		adapter = priv->adapter;
	else {
		pr_err("%s priv->adapter is NULL return\n", __func__);
		return 0;
	}
	thread->thread_status = 1;

#if DBUG_FW_DUMP_READ_CR
	u32 u32ReadCRValue = 0;
	struct timeval g_pre_time;
	do_gettimeofday(&g_pre_time);
#endif
	init_waitqueue_entry(&wait, current);
	for (;;) {
	#if DBUG_FW_DUMP_READ_CR
		struct timeval diff;
		struct timeval temp;

        do_gettimeofday(&diff);
		temp.tv_sec = diff.tv_sec;
		temp.tv_usec = diff.tv_usec;
        if (diff.tv_usec < g_pre_time.tv_usec) {
                diff.tv_sec -= 1;
                diff.tv_usec += 1000000;
        }

        diff.tv_sec -= g_pre_time.tv_sec;
        diff.tv_usec -= g_pre_time.tv_usec;

		if ((diff.tv_sec*1000+diff.tv_usec/1000)>=1000)
		{
			g_pre_time.tv_sec = temp.tv_sec;
			g_pre_time.tv_usec = temp.tv_usec;
			btmtk_sdio_readl(SWPCDBGR, &u32ReadCRValue);
			pr_info("%s SWPCDBGR %x\n", __func__, u32ReadCRValue);
		}
	#endif
		add_wait_queue(&thread->wait_q, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			pr_warn("main_thread: break from main thread\n");
			break;
		}

		if (adapter->wakeup_tries ||
				((!adapter->int_count) &&
				(!priv->btmtk_dev.tx_dnld_rdy ||
				skb_queue_empty(&adapter->tx_queue)))) {
			pr_debug("main_thread is sleeping...\n");
			schedule();
		}

		set_current_state(TASK_RUNNING);

		remove_wait_queue(&thread->wait_q, &wait);

		if (kthread_should_stop()) {
			pr_warn("main_thread: break after wake up\n");
			break;
		}

		ret = priv->hw_set_own_back(DRIVER_OWN);
		if (ret) {
			pr_err("%s set driver own return fail\n", __func__);
			break;
		}

		spin_lock_irqsave(&priv->driver_lock, flags);
		if (adapter->int_count) {
			pr_debug("%s go int\n", __func__);
			adapter->int_count = 0;
			spin_unlock_irqrestore(&priv->driver_lock, flags);
			priv->hw_process_int_status(priv);
		} else if (adapter->ps_state == PS_SLEEP &&
					!skb_queue_empty(&adapter->tx_queue)) {
			pr_debug("%s go vender, todo\n", __func__);
			spin_unlock_irqrestore(&priv->driver_lock, flags);
			adapter->wakeup_tries++;
			continue;
		} else {
			pr_debug("%s go tx\n", __func__);
			spin_unlock_irqrestore(&priv->driver_lock, flags);
		}

		if (adapter->ps_state == PS_SLEEP) {
			pr_debug("%s ps_state == PS_SLEEP, continue\n",
				__func__);
			continue;
		}

		if (!priv->btmtk_dev.tx_dnld_rdy) {
			pr_debug("%s tx_dnld_rdy == 0, continue\n", __func__);
			continue;
		}

		spin_lock_irqsave(&priv->driver_lock, flags);
		skb = skb_dequeue(&adapter->tx_queue);
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		if (skb) {
			if (skb->len < 16)
				btmtk_print_buffer_conent(skb->data, skb->len);
			else
				btmtk_print_buffer_conent(skb->data, 16);

			btmtk_tx_pkt(priv, skb);

			if (skb) {
				pr_debug("%s after btmtk_tx_pkt kfree_skb\n",
					__func__);
				kfree_skb(skb);
			}
		}


		if (skb_queue_empty(&adapter->tx_queue)) {
			ret = priv->hw_set_own_back(FW_OWN);
			if (ret) {
				pr_err("%s set fw own return fail\n",
					__func__);
				break;
			}
		}
	}
	pr_warn("%s  end\n", __func__);
	thread->thread_status = 0;
	return 0;
}

struct btmtk_private *btmtk_add_card(void *card)
{
	struct btmtk_private *priv;

	pr_info("%s begin\n", __func__);
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		goto err_priv;

	priv->adapter = kzalloc(sizeof(*priv->adapter), GFP_KERNEL);
	if (!priv->adapter)
		goto err_adapter;

	btmtk_init_adapter(priv);

	pr_info("Starting kthread...\n");
	priv->main_thread.priv = priv;
	spin_lock_init(&priv->driver_lock);

	init_waitqueue_head(&priv->main_thread.wait_q);
	priv->main_thread.task = kthread_run(btmtk_service_main_thread,
				&priv->main_thread, "btmtk_main_service");
	if (IS_ERR(priv->main_thread.task))
		goto err_thread;

	priv->btmtk_dev.card = card;
	priv->btmtk_dev.tx_dnld_rdy = true;

	return priv;

err_thread:
	btmtk_free_adapter(priv);

err_adapter:
	kfree(priv);

err_priv:
	return NULL;
}
EXPORT_SYMBOL_GPL(btmtk_add_card);

int btmtk_remove_card(struct btmtk_private *priv)
{
	pr_info("%s begin\n", __func__);

	pr_info("%s stop main_thread, thread_status=%d\n", __func__,priv->main_thread.thread_status);
	if (IS_ERR(priv->main_thread.task)){
		pr_err("priv->main_thread.task=%lx\n",PTR_ERR(priv->main_thread.task));
	}
	else if (priv->main_thread.thread_status)
	{
	  pr_info("%s kthread_stop\n", __func__);
                kthread_stop(priv->main_thread.task);
    }
	pr_info("%s stop main_thread done\n", __func__);
#ifdef CONFIG_DEBUG_FS
	/*btmtk_debugfs_remove(hdev);*/
#endif

	btmtk_free_adapter(priv);

	kfree(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(btmtk_remove_card);

MODULE_AUTHOR("Mediatek Ltd.");
MODULE_DESCRIPTION("Mediatek Bluetooth driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL v2");
