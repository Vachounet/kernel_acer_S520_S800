/*
 * kernel/power/autosleep.c
 *
 * Opportunistic sleep support.
 *
 * Copyright (C) 2012 Rafael J. Wysocki <rjw@sisk.pl>
 */

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#if defined(CONFIG_ARCH_ACER_MSM8974)
#include <linux/delay.h>
#include <linux/rtc.h>
#endif

#ifdef CONFIG_POWERSUSPEND
#include <linux/powersuspend.h>
#endif

#include "power.h"

static suspend_state_t autosleep_state;
static struct workqueue_struct *autosleep_wq;
/*
 * Note: it is only safe to mutex_lock(&autosleep_lock) if a wakeup_source
 * is active, otherwise a deadlock with try_to_suspend() is possible.
 * Alternatively mutex_lock_interruptible() can be used.  This will then fail
 * if an auto_sleep cycle tries to freeze processes.
 */
static DEFINE_MUTEX(autosleep_lock);
static struct wakeup_source *autosleep_ws;

#if defined(CONFIG_ARCH_ACER_MSM8974)
static int early_suspend_flag;
static int cpu_low_power_state = 0; // 0: disable;  1: enable;  2:by battery
int kernel_is_in_earlysuspend(void);
void wakeup_source_printk(void);
void pkgl_clean_disconnect(void);
void dbs_set_min_freq(void);
void dbs_set_max_freq(void);
#endif

#if defined(CONFIG_ARCH_ACER_MSM8974)
/* create a work queue to monitor the "suspend" thread while DUT enter suspend */
static void monitor_suspend_wakelock(struct work_struct *work);
static DECLARE_WORK(monitor_wakelock, monitor_suspend_wakelock);
struct workqueue_struct *suspend_wakelock_monitored = NULL;
static int early_suspend_is_working;

static void monitor_suspend_wakelock(struct work_struct *work)
{
	while (1) {
		msleep(20000);
		if (!kernel_is_in_earlysuspend())
			break;
		if (early_suspend_is_working)
			wakeup_source_printk();
	}
}
#endif

static void try_to_suspend(struct work_struct *work)
{
	unsigned int initial_count, final_count;

	if (!pm_get_wakeup_count(&initial_count, true))
		goto out;

	mutex_lock(&autosleep_lock);

	if (!pm_save_wakeup_count(initial_count)) {
		mutex_unlock(&autosleep_lock);
		goto out;
	}

	if (autosleep_state == PM_SUSPEND_ON) {
		mutex_unlock(&autosleep_lock);
		return;
	}
	if (autosleep_state >= PM_SUSPEND_MAX)
		hibernate();
#if defined(CONFIG_ARCH_ACER_MSM8974)
	else {
		early_suspend_is_working = 0;
		pm_suspend(autosleep_state);
		early_suspend_is_working = 1;
	}
#else
	else
		pm_suspend(autosleep_state);
#endif

	mutex_unlock(&autosleep_lock);

	if (!pm_get_wakeup_count(&final_count, false))
		goto out;

	/*
	 * If the wakeup occured for an unknown reason, wait to prevent the
	 * system from trying to suspend and waking up in a tight loop.
	 */
	if (final_count == initial_count)
		schedule_timeout_uninterruptible(HZ / 2);

 out:
	queue_up_suspend_work();
}

static DECLARE_WORK(suspend_work, try_to_suspend);

void queue_up_suspend_work(void)
{
	if (!work_pending(&suspend_work) && autosleep_state > PM_SUSPEND_ON)
		queue_work(autosleep_wq, &suspend_work);
}

suspend_state_t pm_autosleep_state(void)
{
	return autosleep_state;
}

int pm_autosleep_lock(void)
{
	return mutex_lock_interruptible(&autosleep_lock);
}

void pm_autosleep_unlock(void)
{
	mutex_unlock(&autosleep_lock);
}

#if defined(CONFIG_ARCH_ACER_MSM8974)
int kernel_is_in_earlysuspend(void)
{
	return early_suspend_flag;
}

ssize_t cur_state_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	if (early_suspend_flag == 0)
		return sprintf(buf, "%s\n", "on");
	else
		return sprintf(buf, "%s\n", "mem");
}

ssize_t cur_state_store(struct kobject *kobj, struct kobj_attribute *attr,
			const char *buf, size_t n)
{
	return 0;
}

ssize_t force_cpu_low_power_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d", cpu_low_power_state);
}

ssize_t force_cpu_low_power_store(struct kobject *kobj, struct kobj_attribute *attr,
			const char *buf, size_t n)
{
	int result;

	if (buf != NULL) {
		result = (int)buf[0] - (int)'0';
		if (buf[0] > 0 && result >= 0 && result <= 2)
			cpu_low_power_state = result;
	}

	return n;
}

int get_cpu_power_mode(void)
{
	return cpu_low_power_state;
}
#endif

int pm_autosleep_set_state(suspend_state_t state)
{

#ifndef CONFIG_HIBERNATION
	if (state >= PM_SUSPEND_MAX)
		return -EINVAL;
#endif

	__pm_stay_awake(autosleep_ws);

	mutex_lock(&autosleep_lock);

	autosleep_state = state;

	__pm_relax(autosleep_ws);

#if defined(CONFIG_ARCH_ACER_MSM8974)
	{
		struct timespec ts;
		struct rtc_time tm;
		getnstimeofday(&ts);
		rtc_time_to_tm(ts.tv_sec, &tm);
		pr_info("pm_autosleep_set_state: at %lld "
				"(%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n",
				ktime_to_ns(ktime_get()),
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}
#endif

	if (state > PM_SUSPEND_ON) {
		pm_wakep_autosleep_enabled(true);
		queue_up_suspend_work();
#ifdef CONFIG_POWERSUSPEND
		set_power_suspend_state_hook(POWER_SUSPEND_ACTIVE); // Yank555.lu : add hook to handle powersuspend tasks
#endif		
#if defined(CONFIG_ARCH_ACER_MSM8974)
		early_suspend_flag = 1;
		pkgl_clean_disconnect();
		pr_info("[Suspend] Suspend state [%d]\r\n", autosleep_state);
		early_suspend_is_working = 1;
		if (cpu_low_power_state > 0)
			dbs_set_min_freq();
		queue_work(suspend_wakelock_monitored, &monitor_wakelock);
#endif
	} else {
#if defined(CONFIG_ARCH_ACER_MSM8974)
		early_suspend_flag = 0;
		pr_info("[Resume] Resume state [%d]\r\n", autosleep_state);
		early_suspend_is_working = 0;
		if (cpu_low_power_state > 0)
			dbs_set_max_freq();
#endif
		pm_wakep_autosleep_enabled(false);
#ifdef CONFIG_POWERSUSPEND
		set_power_suspend_state_hook(POWER_SUSPEND_INACTIVE); // Yank555.lu : add hook to handle powersuspend tasks
#endif
	}

	mutex_unlock(&autosleep_lock);
	return 0;
}

int __init pm_autosleep_init(void)
{
#ifdef CONFIG_ARCH_ACER_MSM8974
	if (suspend_wakelock_monitored == NULL)
		suspend_wakelock_monitored = create_singlethread_workqueue("monitor_suspend");
#endif

	autosleep_ws = wakeup_source_register("autosleep");
	if (!autosleep_ws)
		return -ENOMEM;

	autosleep_wq = alloc_ordered_workqueue("autosleep", 0);
	if (autosleep_wq)
		return 0;

	wakeup_source_unregister(autosleep_ws);
	return -ENOMEM;
}
