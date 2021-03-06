/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#ifdef CONFIG_BATMAN_VZW_XO_VOTER_FOR_EVDO
#include <linux/moduleparam.h>
#include <linux/delay.h>
#endif
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/seq_file.h>

#include <mach/msm_xo.h>
#include <mach/rpm.h>
#ifdef CONFIG_BATMAN_VZW_XO_VOTER_FOR_EVDO
#include <linux/syscalls.h>
#include <linux/fcntl.h> 
#include <linux/fs.h>
#include <linux/uaccess.h>
#endif
#include <mach/socinfo.h>

#include "rpm_resources.h"

static DEFINE_SPINLOCK(msm_xo_lock);

struct msm_xo {
	unsigned votes[NUM_MSM_XO_MODES];
	unsigned mode;
	struct list_head voters;
};

struct msm_xo_voter {
	const char *name;
	unsigned mode;
	struct msm_xo *xo;
	struct list_head list;
};

static struct msm_xo msm_xo_sources[NUM_MSM_XO_IDS];

#ifdef CONFIG_BATMAN_VZW_XO_VOTER_FOR_EVDO
int evdo_set_status = 0;
#endif

#ifdef CONFIG_DEBUG_FS
static const char *msm_xo_mode_to_str(unsigned mode)
{
	switch (mode) {
	case MSM_XO_MODE_ON:
		return "ON";
	case MSM_XO_MODE_PIN_CTRL:
		return "PIN";
	case MSM_XO_MODE_OFF:
		return "OFF";
	default:
		return "ERR";
	}
}

static void msm_xo_dump_xo(struct seq_file *m, struct msm_xo *xo,
		const char *name)
{
	struct msm_xo_voter *voter;

	seq_printf(m, "%-20s%s\n", name, msm_xo_mode_to_str(xo->mode));
	list_for_each_entry(voter, &xo->voters, list)
		seq_printf(m, " %s %-16s %s\n",
				xo->mode == voter->mode ? "*" : " ",
				voter->name,
				msm_xo_mode_to_str(voter->mode));
}

static int msm_xo_show_voters(struct seq_file *m, void *v)
{
	unsigned long flags;

	spin_lock_irqsave(&msm_xo_lock, flags);
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_TCXO_D0], "TCXO D0");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_TCXO_D1], "TCXO D1");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_TCXO_A0], "TCXO A0");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_TCXO_A1], "TCXO A1");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_TCXO_A2], "TCXO A2");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_CORE], "TCXO Core");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_PXO], "PXO during sleep");
	msm_xo_dump_xo(m, &msm_xo_sources[MSM_XO_CXO], "CXO");
	spin_unlock_irqrestore(&msm_xo_lock, flags);

	return 0;
}

static int msm_xo_voters_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_xo_show_voters, inode->i_private);
}

static const struct file_operations msm_xo_voters_ops = {
	.open		= msm_xo_voters_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init msm_xo_debugfs_init(void)
{
	struct dentry *entry;

	entry = debugfs_create_file("xo_voters", S_IRUGO, NULL, NULL,
			&msm_xo_voters_ops);
	return IS_ERR(entry) ? PTR_ERR(entry) : 0;
}
late_initcall(msm_xo_debugfs_init);
#endif

#ifdef CONFIG_BATMAN_VZW_XO_VOTER_FOR_EVDO
int get_evdo_set_status(void)
{
	return evdo_set_status;
}
#endif

static int msm_xo_update_vote(struct msm_xo *xo)
{
	int ret;
	unsigned vote, prev_vote = xo->mode;
	struct msm_rpm_iv_pair cmd;

	if (xo->votes[MSM_XO_MODE_ON])
		vote = MSM_XO_MODE_ON;
	else if (xo->votes[MSM_XO_MODE_PIN_CTRL])
		vote = MSM_XO_MODE_PIN_CTRL;
	else
		vote = MSM_XO_MODE_OFF;

	if (vote == prev_vote)
		return 0;

	/*
	 * Change the vote here to simplify the TCXO logic. If the RPM
	 * command fails we'll rollback.
	 */
	xo->mode = vote;

	if (xo == &msm_xo_sources[MSM_XO_PXO]) {
		cmd.id = MSM_RPM_ID_PXO_CLK;
		cmd.value = msm_xo_sources[MSM_XO_PXO].mode ? 1 : 0;
		ret = msm_rpmrs_set_noirq(MSM_RPM_CTX_SET_SLEEP, &cmd, 1);
	} else if (xo == &msm_xo_sources[MSM_XO_CXO]) {
		cmd.id = MSM_RPM_ID_CXO_CLK;
		cmd.value = msm_xo_sources[MSM_XO_CXO].mode ? 1 : 0;
		ret = msm_rpmrs_set_noirq(MSM_RPM_CTX_SET_0, &cmd, 1);
	} else {
		cmd.id = MSM_RPM_ID_CXO_BUFFERS;
		cmd.value = (msm_xo_sources[MSM_XO_TCXO_D0].mode << 0)  |
			    (msm_xo_sources[MSM_XO_TCXO_D1].mode << 8)  |
			    (msm_xo_sources[MSM_XO_TCXO_A0].mode << 16) |
			    (msm_xo_sources[MSM_XO_TCXO_A1].mode << 24) |
			    (msm_xo_sources[MSM_XO_TCXO_A2].mode << 28) |
			    /*
			     * 8660 RPM has XO_CORE at bit 18 and 8960 RPM has
			     * XO_CORE at bit 20. Since the opposite bit is
			     * reserved in both cases, just set both and be
			     * done with it.
			     */
			    ((msm_xo_sources[MSM_XO_CORE].mode ? 1 : 0) << 20) |
			    ((msm_xo_sources[MSM_XO_CORE].mode ? 1 : 0) << 18);
		ret = msm_rpm_set_noirq(MSM_RPM_CTX_SET_0, &cmd, 1);
	}

	if (ret)
		xo->mode = prev_vote;

	return ret;
}

static int __msm_xo_mode_vote(struct msm_xo_voter *xo_voter, unsigned mode)
{
	int ret;
	struct msm_xo *xo = xo_voter->xo;

	if (xo_voter->mode == mode)
		return 0;

	xo->votes[mode]++;
	xo->votes[xo_voter->mode]--;
	ret = msm_xo_update_vote(xo);
	if (ret) {
		xo->votes[xo_voter->mode]++;
		xo->votes[mode]--;
		goto out;
	}
	xo_voter->mode = mode;
out:
	return ret;
}

/**
 * msm_xo_mode_vote() - Vote for an XO to be ON, OFF, or under PIN_CTRL
 * @xo_voter - Valid handle returned from msm_xo_get()
 * @mode - Mode to vote for (ON, OFF, PIN_CTRL)
 *
 * Vote for an XO to be either ON, OFF, or under PIN_CTRL. Votes are
 * aggregated with ON taking precedence over PIN_CTRL taking precedence
 * over OFF.
 *
 * This function returns 0 on success or a negative error code on failure.
 */
int msm_xo_mode_vote(struct msm_xo_voter *xo_voter, enum msm_xo_modes mode)
{
	int ret;
	unsigned long flags;

	if (!xo_voter)
		return 0;

	if (mode >= NUM_MSM_XO_MODES || IS_ERR(xo_voter))
		return -EINVAL;

	spin_lock_irqsave(&msm_xo_lock, flags);
	ret = __msm_xo_mode_vote(xo_voter, mode);
	spin_unlock_irqrestore(&msm_xo_lock, flags);

	return ret;
}
EXPORT_SYMBOL(msm_xo_mode_vote);

/**
 * msm_xo_get() - Get a voting handle for an XO
 * @xo_id - XO identifier
 * @voter - Debug string to identify users
 *
 * XO voters vote for OFF by default. This function returns a pointer
 * indicating success. An ERR_PTR is returned on failure.
 *
 * If XO voting is disabled, %NULL is returned.
 */
struct msm_xo_voter *msm_xo_get(enum msm_xo_ids xo_id, const char *voter)
{
	int ret;
	unsigned long flags;
	struct msm_xo_voter *xo_voter;

	/*
	 * TODO: Remove early return for 8064 once RPM XO voting support
	 * is available.
	 */
	if (cpu_is_apq8064())
		return NULL;

	if (xo_id >= NUM_MSM_XO_IDS) {
		ret = -EINVAL;
		goto err;
	}

	xo_voter = kzalloc(sizeof(*xo_voter), GFP_KERNEL);
	if (!xo_voter) {
		ret = -ENOMEM;
		goto err;
	}

	xo_voter->name = kstrdup(voter, GFP_KERNEL);
	if (!xo_voter->name) {
		ret = -ENOMEM;
		goto err_name;
	}

	xo_voter->xo = &msm_xo_sources[xo_id];

	/* Voters vote for OFF by default */
	spin_lock_irqsave(&msm_xo_lock, flags);
	xo_voter->xo->votes[MSM_XO_MODE_OFF]++;
	list_add(&xo_voter->list, &xo_voter->xo->voters);
	spin_unlock_irqrestore(&msm_xo_lock, flags);

	return xo_voter;

err_name:
	kfree(xo_voter);
err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(msm_xo_get);

/**
 * msm_xo_put() - Release a voting handle
 * @xo_voter - Valid handle returned from msm_xo_get()
 *
 * Release a reference to an XO voting handle. This also removes the voter's
 * vote, therefore calling msm_xo_mode_vote(xo_voter, MSM_XO_MODE_OFF)
 * beforehand is unnecessary.
 */
void msm_xo_put(struct msm_xo_voter *xo_voter)
{
	unsigned long flags;

	if (!xo_voter || IS_ERR(xo_voter))
		return;

	spin_lock_irqsave(&msm_xo_lock, flags);
	__msm_xo_mode_vote(xo_voter, MSM_XO_MODE_OFF);
	xo_voter->xo->votes[MSM_XO_MODE_OFF]--;
	list_del(&xo_voter->list);
	spin_unlock_irqrestore(&msm_xo_lock, flags);

	kfree(xo_voter->name);
	kfree(xo_voter);
}
EXPORT_SYMBOL(msm_xo_put);

#ifdef CONFIG_BATMAN_VZW_XO_VOTER_FOR_EVDO
int msm_xo_mode_control(int on_off)  
{
	
	static struct msm_xo_voter *voter;
	int rc = 0;
	
	if(voter == NULL)
		voter = msm_xo_get(MSM_XO_TCXO_D1, "evdo");

	if (IS_ERR(voter)) {
		pr_err("Failed to get XO CORE voter (%ld)\n",
				PTR_ERR(voter));
		goto fail;
	}
		
//	pr_err("%s: msm_xo_mode is %d\n", __func__, on_off);
	
	if(on_off)
	{
		evdo_set_status = 1;
		rc = msm_xo_mode_vote(voter, MSM_XO_MODE_ON);
	}	
	else
	{
		evdo_set_status = 0;
		rc = msm_xo_mode_vote(voter, MSM_XO_MODE_OFF);
	}
	if (rc < 0) {
		pr_err("XO Core %s failed (%d)\n",
			on_off ? "enable" : "disable", rc);
		goto fail_xo_mode_vote;
	}
//	pr_err("%s:evdo_set_status:%d\n", __func__,evdo_set_status);
	return 0;
fail_xo_mode_vote:
	msm_xo_put(voter);
fail:
	return rc;
}
EXPORT_SYMBOL(msm_xo_mode_control);


static int dummy_arg_on, dummy_arg_off;

static int msm_xo_mode_on_write(const char *val, struct kernel_param *kp)
{
	return 0;	 
}

static int msm_xo_mode_on_read(char *buf, struct kernel_param *kp)
{
	int result;
	result = msm_xo_mode_control(1);
	sprintf(buf,"%d",result);
	return 0;
}

static int msm_xo_mode_off_write(const char *val, struct kernel_param *kp)
{
	return 0;	 
}

static int msm_xo_mode_off_read(char *buf, struct kernel_param *kp)
{ 
	int result;
	result = msm_xo_mode_control(0);	
	sprintf(buf,"%d",result);
	return 0;
}

module_param_call(msm_xo_mode_on, msm_xo_mode_on_write, msm_xo_mode_on_read, &dummy_arg_on, 0665);
module_param_call(msm_xo_mode_off, msm_xo_mode_off_write, msm_xo_mode_off_read, &dummy_arg_off, 0665);
#endif

int __init msm_xo_init(void)
{
	int i;
	int ret;
	struct msm_rpm_iv_pair cmd[2];

	for (i = 0; i < ARRAY_SIZE(msm_xo_sources); i++)
		INIT_LIST_HEAD(&msm_xo_sources[i].voters);

	cmd[0].id = MSM_RPM_ID_PXO_CLK;
	cmd[0].value = 1;
	cmd[1].id = MSM_RPM_ID_CXO_BUFFERS;
	cmd[1].value = 0;
	ret = msm_rpmrs_set(MSM_RPM_CTX_SET_0, cmd, 2);
	if (ret)
		goto out;

	cmd[0].id = MSM_RPM_ID_PXO_CLK;
	cmd[0].value = 0;
	ret = msm_rpmrs_set(MSM_RPM_CTX_SET_SLEEP, cmd, 1);
	if (ret)
		goto out;
out:
	return ret;
}
