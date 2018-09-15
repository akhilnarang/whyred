/*
 * Copyright (c) 2014-2018 The Linux Foundation. All rights reserved.
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
 * DOC: qdf_mc_timer
 * QCA driver framework timer APIs serialized to MC thread
 */

/* Include Files */
#include <qdf_mc_timer.h>
#include <qdf_lock.h>
#include "qdf_lock.h"
#include "qdf_list.h"
#include "qdf_mem.h"
#include <qdf_module.h>
#include <linux/export.h>
#ifdef CONFIG_MCL
#include <cds_mc_timer.h>
#include <cds_sched.h>
#endif
/* Preprocessor definitions and constants */

#define LINUX_TIMER_COOKIE 0x12341234
#define LINUX_INVALID_TIMER_COOKIE 0xfeedface
#define TMR_INVALID_ID (0)

/* Type declarations */

/* Static Variable Definitions */
static unsigned int persistent_timer_count;
static qdf_mutex_t persistent_timer_count_lock;

static qdf_spinlock_t qdf_mc_timer_cookie_lock;
static uint32_t  g_qdf_mc_timer_cookie;

/* Function declarations and documenation */

/**
 * qdf_try_allowing_sleep() - clean up timer states after it has been deactivated
 * @type: timer type
 *
 * Clean up timer states after it has been deactivated check and try to allow
 * sleep after a timer has been stopped or expired.
 *
 * Return: none
 */
void qdf_try_allowing_sleep(QDF_TIMER_TYPE type)
{
	if (QDF_TIMER_TYPE_WAKE_APPS == type) {

		persistent_timer_count--;
		if (0 == persistent_timer_count) {
			/* since the number of persistent timers has
			 * decreased from 1 to 0, the timer should allow
			 * sleep
			 */
		}
	}
}
qdf_export_symbol(qdf_try_allowing_sleep);

/**
 * qdf_mc_timer_get_current_state() - get the current state of the timer
 * @timer: Pointer to timer object
 *
 * Return:
 * QDF_TIMER_STATE - qdf timer state
 */
QDF_TIMER_STATE qdf_mc_timer_get_current_state(qdf_mc_timer_t *timer)
{
	if (NULL == timer) {
		QDF_ASSERT(0);
		return QDF_TIMER_STATE_UNUSED;
	}

	switch (timer->state) {
	case QDF_TIMER_STATE_STOPPED:
	case QDF_TIMER_STATE_STARTING:
	case QDF_TIMER_STATE_RUNNING:
	case QDF_TIMER_STATE_UNUSED:
		return timer->state;
	default:
		QDF_ASSERT(0);
		return QDF_TIMER_STATE_UNUSED;
	}
}
qdf_export_symbol(qdf_mc_timer_get_current_state);

/**
 * qdf_timer_module_init() - initializes a QDF timer module.
 *
 * This API initializes the QDF timer module. This needs to be called
 * exactly once prior to using any QDF timers.
 *
 * Return: none
 */
void qdf_timer_module_init(void)
{
	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "Initializing the QDF MC timer module");
	qdf_mutex_create(&persistent_timer_count_lock);
	qdf_spinlock_create(&qdf_mc_timer_cookie_lock);
}
qdf_export_symbol(qdf_timer_module_init);

#ifdef TIMER_MANAGER

static qdf_list_t qdf_timer_domains[QDF_MEM_DOMAIN_MAX_COUNT];
static qdf_spinlock_t qdf_timer_list_lock;

static inline qdf_list_t *qdf_mc_timer_active_list(void)
{
	return &qdf_timer_domains[qdf_mem_get_domain()];
}

/**
 * qdf_mc_timer_manager_init() - initialize QDF debug timer manager
 *
 * This API initializes QDF timer debug functionality.
 *
 * Return: none
 */
void qdf_mc_timer_manager_init(void)
{
	int i;

	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_create(&qdf_timer_domains[i], 1000);
	qdf_spinlock_create(&qdf_timer_list_lock);
}
qdf_export_symbol(qdf_mc_timer_manager_init);

static inline void qdf_mc_timer_panic(void)
{
	QDF_BUG(0);
}

static void qdf_mc_timer_print_list(qdf_list_t *timers)
{
	QDF_STATUS status;
	qdf_list_node_t *node;

	qdf_spin_lock_irqsave(&qdf_timer_list_lock);
	status = qdf_list_peek_front(timers, &node);
	while (QDF_IS_STATUS_SUCCESS(status)) {
		qdf_mc_timer_node_t *timer_node = (qdf_mc_timer_node_t *)node;
		const char *filename = kbasename(timer_node->file_name);
		uint32_t line = timer_node->line_num;

		qdf_spin_unlock_irqrestore(&qdf_timer_list_lock);
		qdf_err("timer Leak@ File %s, @Line %u", filename, line);
		qdf_spin_lock_irqsave(&qdf_timer_list_lock);

		status = qdf_list_peek_next(timers, node, &node);
	}
	qdf_spin_unlock_irqrestore(&qdf_timer_list_lock);
}

void qdf_mc_timer_check_for_leaks(void)
{
	enum qdf_mem_domain domain = qdf_mem_get_domain();
	qdf_list_t *timers = &qdf_timer_domains[domain];

	if (qdf_list_empty(timers))
		return;

	qdf_err("Timer leaks detected in %s domain!",
		qdf_mem_domain_name(domain));
	qdf_mc_timer_print_list(timers);
	qdf_mc_timer_panic();
}

static void qdf_mc_timer_free_leaked_timers(qdf_list_t *timers)
{
	QDF_STATUS status;
	qdf_list_node_t *node;

	qdf_spin_lock_irqsave(&qdf_timer_list_lock);
	status = qdf_list_remove_front(timers, &node);
	while (QDF_IS_STATUS_SUCCESS(status)) {
		qdf_mem_free(node);
		status = qdf_list_remove_front(timers, &node);
	}
	qdf_spin_unlock_irqrestore(&qdf_timer_list_lock);
}

/**
 * qdf_timer_clean() - clean up QDF timer debug functionality
 *
 * This API cleans up QDF timer debug functionality and prints which QDF timers
 * are leaked. This is called during driver unload.
 *
 * Return: none
 */
static void qdf_timer_clean(void)
{
	bool leaks_detected = false;
	int i;

	/* detect and print leaks */
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i) {
		qdf_list_t *timers = &qdf_timer_domains[i];

		if (qdf_list_empty(timers))
			continue;

		leaks_detected = true;

		qdf_err("\nTimer leaks detected in the %s (Id %d) domain!\n",
			qdf_mem_domain_name(i), i);
		qdf_mc_timer_print_list(timers);
	}

	/* we're done if there were no leaks */
	if (!leaks_detected)
		return;

	/* panic, if enabled */
	qdf_mc_timer_panic();

	/* if we didn't crash, release the leaked timers */
	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_mc_timer_free_leaked_timers(&qdf_timer_domains[i]);
}
qdf_export_symbol(qdf_timer_clean);

/**
 * qdf_mc_timer_manager_exit() - exit QDF timer debug functionality
 *
 * This API exists QDF timer debug functionality
 *
 * Return: none
 */
void qdf_mc_timer_manager_exit(void)
{
	int i;

	qdf_timer_clean();

	for (i = 0; i < QDF_MEM_DOMAIN_MAX_COUNT; ++i)
		qdf_list_destroy(&qdf_timer_domains[i]);

	qdf_spinlock_destroy(&qdf_timer_list_lock);
}
qdf_export_symbol(qdf_mc_timer_manager_exit);
#endif

/**
 * qdf_mc_timer_init() - initialize a QDF timer
 * @timer: Pointer to timer object
 * @timer_type: Type of timer
 * @callback: Callback to be called after timer expiry
 * @ser_data: User data which will be passed to callback function
 *
 * This API initializes a QDF timer object.
 *
 * qdf_mc_timer_init() initializes a QDF timer object. A timer must be
 * initialized by calling qdf_mc_timer_initialize() before it may be used in
 * any other timer functions.
 *
 * Attempting to initialize timer that is already initialized results in
 * a failure. A destroyed timer object can be re-initialized with a call to
 * qdf_mc_timer_init(). The results of otherwise referencing the object
 * after it has been destroyed are undefined.
 *
 *  Calls to QDF timer functions to manipulate the timer such
 *  as qdf_mc_timer_set() will fail if the timer is not initialized or has
 *  been destroyed. Therefore, don't use the timer after it has been
 *  destroyed until it has been re-initialized.
 *
 *  All callback will be executed within the CDS main thread unless it is
 *  initialized from the Tx thread flow, in which case it will be executed
 *  within the tx thread flow.
 *
 * Return:
 * QDF_STATUS_SUCCESS: timer is initialized successfully
 * QDF failure status: timer initialization failed
 */
#ifdef TIMER_MANAGER
QDF_STATUS qdf_mc_timer_init_debug(qdf_mc_timer_t *timer,
				   QDF_TIMER_TYPE timer_type,
				   qdf_mc_timer_callback_t callback,
				   void *user_data, char *file_name,
				   uint32_t line_num)
{
	qdf_list_t *active_timers = qdf_mc_timer_active_list();
	QDF_STATUS qdf_status;

	/* check for invalid pointer */
	if ((timer == NULL) || (callback == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Null params being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAULT;
	}

	timer->timer_node = qdf_mem_malloc(sizeof(qdf_mc_timer_node_t));

	if (timer->timer_node == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Not able to allocate memory for time_node",
			  __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_NOMEM;
	}

	timer->timer_node->file_name = file_name;
	timer->timer_node->line_num = line_num;
	timer->timer_node->qdf_timer = timer;

	qdf_spin_lock_irqsave(&qdf_timer_list_lock);
	qdf_status = qdf_list_insert_front(active_timers,
					   &timer->timer_node->node);
	qdf_spin_unlock_irqrestore(&qdf_timer_list_lock);
	if (QDF_STATUS_SUCCESS != qdf_status) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Unable to insert node into List qdf_status %d",
			  __func__, qdf_status);
	}

	/* set the various members of the timer structure
	 * with arguments passed or with default values
	 */
	qdf_spinlock_create(&timer->platform_info.spinlock);
	if (QDF_TIMER_TYPE_SW == timer_type)
		init_timer_deferrable(&(timer->platform_info.timer));
	else
		init_timer(&(timer->platform_info.timer));
#ifdef CONFIG_MCL
	timer->platform_info.timer.function = cds_linux_timer_callback;
#else
	timer->platform_info.timer.function = NULL;
#endif
	timer->platform_info.timer.data = (unsigned long)timer;
	timer->callback = callback;
	timer->user_data = user_data;
	timer->type = timer_type;
	timer->platform_info.cookie = LINUX_TIMER_COOKIE;
	timer->platform_info.thread_id = 0;
	timer->state = QDF_TIMER_STATE_STOPPED;

	return QDF_STATUS_SUCCESS;
}
#else
QDF_STATUS qdf_mc_timer_init(qdf_mc_timer_t *timer, QDF_TIMER_TYPE timer_type,
			     qdf_mc_timer_callback_t callback,
			     void *user_data)
{
	/* check for invalid pointer */
	if ((timer == NULL) || (callback == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Null params being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAULT;
	}

	/* set the various members of the timer structure
	 * with arguments passed or with default values
	 */
	qdf_spinlock_create(&timer->platform_info.spinlock);
	if (QDF_TIMER_TYPE_SW == timer_type)
		init_timer_deferrable(&(timer->platform_info.timer));
	else
		init_timer(&(timer->platform_info.timer));
#ifdef CONFIG_MCL
	timer->platform_info.timer.function = cds_linux_timer_callback;
#else
	timer->platform_info.timer.function = NULL;
#endif
	timer->platform_info.timer.data = (unsigned long)timer;
	timer->callback = callback;
	timer->user_data = user_data;
	timer->type = timer_type;
	timer->platform_info.cookie = LINUX_TIMER_COOKIE;
	timer->platform_info.thread_id = 0;
	timer->state = QDF_TIMER_STATE_STOPPED;

	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * qdf_mc_timer_destroy() - destroy QDF timer
 * @timer: Pointer to timer object
 *
 * qdf_mc_timer_destroy() function shall destroy the timer object.
 * After a successful return from \a qdf_mc_timer_destroy() the timer
 * object becomes, in effect, uninitialized.
 *
 * A destroyed timer object can be re-initialized by calling
 * qdf_mc_timer_init().  The results of otherwise referencing the object
 * after it has been destroyed are undefined.
 *
 * Calls to QDF timer functions to manipulate the timer, such
 * as qdf_mc_timer_set() will fail if the lock is destroyed.  Therefore,
 * don't use the timer after it has been destroyed until it has
 * been re-initialized.
 *
 * Return:
 * QDF_STATUS_SUCCESS - timer is initialized successfully
 * QDF failure status - timer initialization failed
 */
#ifdef TIMER_MANAGER
QDF_STATUS qdf_mc_timer_destroy(qdf_mc_timer_t *timer)
{
	qdf_list_t *active_timers = qdf_mc_timer_active_list();
	QDF_STATUS v_status = QDF_STATUS_SUCCESS;

	/* check for invalid pointer */
	if (NULL == timer) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Null timer pointer being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAULT;
	}

	/* Check if timer refers to an uninitialized object */
	if (LINUX_TIMER_COOKIE != timer->platform_info.cookie) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot destroy uninitialized timer", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	qdf_spin_lock_irqsave(&qdf_timer_list_lock);
	v_status = qdf_list_remove_node(active_timers,
					&timer->timer_node->node);
	qdf_spin_unlock_irqrestore(&qdf_timer_list_lock);
	if (v_status != QDF_STATUS_SUCCESS) {
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}
	qdf_mem_free(timer->timer_node);

	qdf_spin_lock_irqsave(&timer->platform_info.spinlock);

	switch (timer->state) {

	case QDF_TIMER_STATE_STARTING:
		v_status = QDF_STATUS_E_BUSY;
		break;

	case QDF_TIMER_STATE_RUNNING:
		/* Stop the timer first */
		del_timer(&(timer->platform_info.timer));
		v_status = QDF_STATUS_SUCCESS;
		break;
	case QDF_TIMER_STATE_STOPPED:
		v_status = QDF_STATUS_SUCCESS;
		break;

	case QDF_TIMER_STATE_UNUSED:
		v_status = QDF_STATUS_E_ALREADY;
		break;

	default:
		v_status = QDF_STATUS_E_FAULT;
		break;
	}

	if (QDF_STATUS_SUCCESS == v_status) {
		timer->platform_info.cookie = LINUX_INVALID_TIMER_COOKIE;
		timer->state = QDF_TIMER_STATE_UNUSED;
		qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);
		qdf_spinlock_destroy(&timer->platform_info.spinlock);
		return v_status;
	}

	qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: Cannot destroy timer in state = %d", __func__,
		  timer->state);
	QDF_ASSERT(0);

	return v_status;
}
qdf_export_symbol(qdf_mc_timer_destroy);

#else

/**
 * qdf_mc_timer_destroy() - destroy QDF timer
 * @timer: Pointer to timer object
 *
 * qdf_mc_timer_destroy() function shall destroy the timer object.
 * After a successful return from \a qdf_mc_timer_destroy() the timer
 * object becomes, in effect, uninitialized.
 *
 * A destroyed timer object can be re-initialized by calling
 * qdf_mc_timer_init(). The results of otherwise referencing the object
 * after it has been destroyed are undefined.
 *
 * Calls to QDF timer functions to manipulate the timer, such
 * as qdf_mc_timer_set() will fail if the lock is destroyed. Therefore,
 * don't use the timer after it has been destroyed until it has
 * been re-initialized.
 *
 * Return:
 * QDF_STATUS_SUCCESS - timer is initialized successfully
 * QDF failure status - timer initialization failed
 */
QDF_STATUS qdf_mc_timer_destroy(qdf_mc_timer_t *timer)
{
	QDF_STATUS v_status = QDF_STATUS_SUCCESS;

	/* check for invalid pointer */
	if (NULL == timer) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Null timer pointer being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAULT;
	}

	/* check if timer refers to an uninitialized object */
	if (LINUX_TIMER_COOKIE != timer->platform_info.cookie) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot destroy uninitialized timer", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}
	qdf_spin_lock_irqsave(&timer->platform_info.spinlock);

	switch (timer->state) {

	case QDF_TIMER_STATE_STARTING:
		v_status = QDF_STATUS_E_BUSY;
		break;

	case QDF_TIMER_STATE_RUNNING:
		/* Stop the timer first */
		del_timer(&(timer->platform_info.timer));
		v_status = QDF_STATUS_SUCCESS;
		break;

	case QDF_TIMER_STATE_STOPPED:
		v_status = QDF_STATUS_SUCCESS;
		break;

	case QDF_TIMER_STATE_UNUSED:
		v_status = QDF_STATUS_E_ALREADY;
		break;

	default:
		v_status = QDF_STATUS_E_FAULT;
		break;
	}

	if (QDF_STATUS_SUCCESS == v_status) {
		timer->platform_info.cookie = LINUX_INVALID_TIMER_COOKIE;
		timer->state = QDF_TIMER_STATE_UNUSED;
		qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);
		return v_status;
	}

	qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: Cannot destroy timer in state = %d", __func__,
		  timer->state);
	QDF_ASSERT(0);

	return v_status;
}
qdf_export_symbol(qdf_mc_timer_destroy);
#endif

/**
 * qdf_mc_timer_start() - start a QDF timer object
 * @timer: Pointer to timer object
 * @expiration_time: Time to expire
 *
 * qdf_mc_timer_start() function starts a timer to expire after the
 * specified interval, thus running the timer callback function when
 * the interval expires.
 *
 * A timer only runs once (a one-shot timer). To re-start the
 * timer, qdf_mc_timer_start() has to be called after the timer runs
 * or has been cancelled.
 *
 * Return:
 * QDF_STATUS_SUCCESS: timer is initialized successfully
 * QDF failure status: timer initialization failed
 */
QDF_STATUS qdf_mc_timer_start(qdf_mc_timer_t *timer, uint32_t expiration_time)
{
	/* check for invalid pointer */
	if (NULL == timer) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s Null timer pointer being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	/* check if timer refers to an uninitialized object */
	if (LINUX_TIMER_COOKIE != timer->platform_info.cookie) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot start uninitialized timer", __func__);
		QDF_ASSERT(0);

		return QDF_STATUS_E_INVAL;
	}

	/* check if timer has expiration time less than 10 ms */
	if (expiration_time < 10) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot start a timer with expiration less than 10 ms",
			  __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	/* make sure the remainer of the logic isn't interrupted */
	qdf_spin_lock_irqsave(&timer->platform_info.spinlock);

	/* ensure if the timer can be started */
	if (QDF_TIMER_STATE_STOPPED != timer->state) {
		qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot start timer in state = %d ", __func__,
			  timer->state);
		return QDF_STATUS_E_ALREADY;
	}

	/* start the timer */
	mod_timer(&(timer->platform_info.timer),
		  jiffies + msecs_to_jiffies(expiration_time));

	timer->state = QDF_TIMER_STATE_RUNNING;

	/* get the thread ID on which the timer is being started */
	timer->platform_info.thread_id = current->pid;

	if (QDF_TIMER_TYPE_WAKE_APPS == timer->type) {
		persistent_timer_count++;
		if (1 == persistent_timer_count) {
			/* since we now have one persistent timer,
			 * we need to disallow sleep
			 * sleep_negate_okts(sleep_client_handle);
			 */
		}
	}

	qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);

	qdf_spin_lock_irqsave(&qdf_mc_timer_cookie_lock);
	timer->cookie = g_qdf_mc_timer_cookie++;
	qdf_spin_unlock_irqrestore(&qdf_mc_timer_cookie_lock);

	return QDF_STATUS_SUCCESS;
}
qdf_export_symbol(qdf_mc_timer_start);

#ifdef CONFIG_MCL
static void qdf_remove_timer_from_sys_msg(uint32_t timer_cookie)
{
	cds_remove_timer_from_sys_msg(timer_cookie);
}
#else
static inline void qdf_remove_timer_from_sys_msg(uint32_t timer_cookie)
{
}
#endif

/**
 * qdf_mc_timer_stop() - stop a QDF timer
 * @timer: Pointer to timer object
 * qdf_mc_timer_stop() function stops a timer that has been started but
 * has not expired, essentially cancelling the 'start' request.
 *
 * After a timer is stopped, it goes back to the state it was in after it
 * was created and can be started again via a call to qdf_mc_timer_start().
 *
 * Return:
 * QDF_STATUS_SUCCESS: timer is initialized successfully
 * QDF failure status: timer initialization failed
 */
QDF_STATUS qdf_mc_timer_stop(qdf_mc_timer_t *timer)
{
	/* check for invalid pointer */
	if (NULL == timer) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s Null timer pointer being passed", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	/* check if timer refers to an uninitialized object */
	if (LINUX_TIMER_COOKIE != timer->platform_info.cookie) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Cannot stop uninitialized timer", __func__);
		QDF_ASSERT(0);

		return QDF_STATUS_E_INVAL;
	}

	/* ensure the timer state is correct */
	qdf_spin_lock_irqsave(&timer->platform_info.spinlock);

	if (QDF_TIMER_STATE_RUNNING != timer->state) {
		qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
			  "%s: Cannot stop timer in state = %d",
			  __func__, timer->state);
		qdf_remove_timer_from_sys_msg(timer->cookie);

		return QDF_STATUS_SUCCESS;
	}

	timer->state = QDF_TIMER_STATE_STOPPED;

	del_timer(&(timer->platform_info.timer));

	qdf_spin_unlock_irqrestore(&timer->platform_info.spinlock);

	qdf_try_allowing_sleep(timer->type);

	return QDF_STATUS_SUCCESS;
}
qdf_export_symbol(qdf_mc_timer_stop);

/**
 * qdf_mc_timer_get_system_ticks() - get the system time in 10ms ticks

 * qdf_mc_timer_get_system_ticks() function returns the current number
 * of timer ticks in 10msec intervals. This function is suitable timestamping
 * and calculating time intervals by calculating the difference between two
 * timestamps.
 *
 * Return:
 * The current system tick count (in 10msec intervals).  This
 * function cannot fail.
 */
unsigned long qdf_mc_timer_get_system_ticks(void)
{
	return jiffies_to_msecs(jiffies) / 10;
}
qdf_export_symbol(qdf_mc_timer_get_system_ticks);

/**
 * qdf_mc_timer_get_system_time() - Get the system time in milliseconds
 *
 * qdf_mc_timer_get_system_time() function returns the number of milliseconds
 * that have elapsed since the system was started
 *
 * Return:
 * The current system time in milliseconds
 */
unsigned long qdf_mc_timer_get_system_time(void)
{
	struct timeval tv;

	do_gettimeofday(&tv);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
qdf_export_symbol(qdf_mc_timer_get_system_time);

s64 qdf_get_monotonic_boottime_ns(void)
{
	struct timespec ts;

	ktime_get_ts(&ts);
	return timespec_to_ns(&ts);
}
qdf_export_symbol(qdf_get_monotonic_boottime_ns);

/**
 * qdf_timer_module_deinit() - Deinitializes a QDF timer module.
 *
 * This API deinitializes the QDF timer module.
 * Return: none
 */
void qdf_timer_module_deinit(void)
{
	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "De-Initializing the QDF MC timer module");
	qdf_mutex_destroy(&persistent_timer_count_lock);
	qdf_spinlock_destroy(&qdf_mc_timer_cookie_lock);
}
qdf_export_symbol(qdf_timer_module_deinit);

void qdf_get_time_of_the_day_in_hr_min_sec_usec(char *tbuf, int len)
{
	struct timeval tv;
	struct rtc_time tm;
	unsigned long local_time;

	/* Format the Log time R#: [hr:min:sec.microsec] */
	do_gettimeofday(&tv);
	/* Convert rtc to local time */
	local_time = (u32)(tv.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);
	scnprintf(tbuf, len,
		"[%02d:%02d:%02d.%06lu]",
		tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec);
}
qdf_export_symbol(qdf_get_time_of_the_day_in_hr_min_sec_usec);
