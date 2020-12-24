/****************************************************************************
 * arch/risc-v/src/bl602/bl602_oneshot_lowerhalf.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/timers/oneshot.h>

#include "riscv_arch.h"
#include "riscv_internal.h"

#include <hardware/bl602_timer.h>
#include "bl602_oneshot_lowerhalf.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Private definetions */
#define TIMER_MAX_VALUE (0xFFFFFFFF)
#define TIMER_CLK_DIV   (160)
#define TIMER_CLK_FREQ  (160000000UL / (TIMER_CLK_DIV))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of the oneshot timer lower-half driver
 */

struct bl602_oneshot_lowerhalf_s
{
  /* This is the part of the lower half driver that is visible to the upper-
   * half client of the driver.  This must be the first thing in this
   * structure so that pointers to struct oneshot_lowerhalf_s are cast
   * compatible to struct bl602_oneshot_lowerhalf_s and vice versa.
   */

  struct oneshot_lowerhalf_s lh; /* Common lower-half driver fields */

  uint32_t freq;

  /* Private lower half data follows */

  oneshot_callback_t callback; /* Internal handler that receives callback */
  FAR void *         arg;      /* Argument that is passed to the handler */
  uint8_t            tim;      /* timer tim 0,1 */
  uint8_t            irq;      /* IRQ associated with this UART */
  bool               started;  /* True: Timer has been started */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bl602_max_delay(FAR struct oneshot_lowerhalf_s *lower,
                           FAR struct timespec *           ts);
static int bl602_start(FAR struct oneshot_lowerhalf_s *lower,
                       oneshot_callback_t              callback,
                       FAR void *                      arg,
                       FAR const struct timespec *     ts);
static int bl602_cancel(FAR struct oneshot_lowerhalf_s *lower,
                        FAR struct timespec *           ts);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Lower half operations */

static const struct oneshot_operations_s g_oneshot_ops =
{
  .max_delay = bl602_max_delay,
  .start     = bl602_start,
  .cancel    = bl602_cancel,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bl602_oneshot_handler
 *
 * Description:
 *   Timer expiration handler
 *
 * Input Parameters:
 *   arg - Should be the same argument provided when bl602_oneshot_start()
 *         was called.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static int bl602_oneshot_handler(int irq, FAR void *context, FAR void *arg)
{
  FAR struct bl602_oneshot_lowerhalf_s *priv =
    (FAR struct bl602_oneshot_lowerhalf_s *)arg;

  oneshot_callback_t callback;
  FAR void *         cbarg;

  /* Clear Interrupt Bits */

  uint32_t int_id;
  uint32_t tmp_val;
  uint32_t tmp_addr;

  int_id   = getreg32(TIMER_BASE + TIMER_TMSR2_OFFSET + 4 * priv->tim);
  tmp_addr = TIMER_BASE + TIMER_TICR2_OFFSET + 4 * priv->tim;
  tmp_val  = getreg32(tmp_addr);

  /* Comparator 0 match interrupt */

  if (((int_id) & (1 << (TIMER_TMSR_0_POS))) != 0)
    {
      putreg32(tmp_val | (1 << TIMER_TCLR_0_POS), tmp_addr);
      callback = priv->callback;
      cbarg    = priv->arg;

      if (callback)
        {
          callback(&priv->lh, cbarg);
        }
    }

  /* Comparator 1 match interrupt */

  if (((int_id) & (1 << (TIMER_TMSR_1_POS))) != 0)
    {
      putreg32(tmp_val | (1 << TIMER_TCLR_1_POS), tmp_addr);
    }

  /* Comparator 2 match interrupt */

  if (((int_id) & (1 << (TIMER_TMSR_2_POS))) != 0)
    {
      putreg32(tmp_val | (1 << TIMER_TCLR_2_POS), tmp_addr);
    }

  return 0;
}

/****************************************************************************
 * Name: bl602_max_delay
 *
 * Description:
 *   Determine the maximum delay of the one-shot timer (in microseconds)
 *
 * Input Parameters:
 *   lower   An instance of the lower-half oneshot state structure.  This
 *           structure must have been previously initialized via a call to
 *           oneshot_initialize();
 *   ts      The location in which to return the maximum delay.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned
 *   on failure.
 *
 ****************************************************************************/

static int bl602_max_delay(FAR struct oneshot_lowerhalf_s *lower,
                           FAR struct timespec *           ts)
{
  FAR struct bl602_oneshot_lowerhalf_s *priv =
    (FAR struct bl602_oneshot_lowerhalf_s *)lower;
  uint64_t usecs;

  DEBUGASSERT(priv != NULL && ts != NULL);
  usecs = (uint64_t)(UINT32_MAX / priv->freq) * (uint64_t)USEC_PER_SEC;

  uint64_t sec = usecs / 1000000;
  usecs -= 1000000 * sec;

  ts->tv_sec  = (time_t)sec;
  ts->tv_nsec = (long)(usecs * 1000);

  return OK;
}

/****************************************************************************
 * Name: bl602_start
 *
 * Description:
 *   Start the oneshot timer
 *
 * Input Parameters:
 *   lower   An instance of the lower-half oneshot state structure.  This
 *           structure must have been previously initialized via a call to
 *           oneshot_initialize();
 *   handler The function to call when when the oneshot timer expires.
 *   arg     An opaque argument that will accompany the callback.
 *   ts      Provides the duration of the one shot timer.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned
 *   on failure.
 *
 ****************************************************************************/

static int bl602_start(FAR struct oneshot_lowerhalf_s *lower,
                       oneshot_callback_t              callback,
                       FAR void *                      arg,
                       FAR const struct timespec *     ts)
{
  FAR struct bl602_oneshot_lowerhalf_s *priv =
    (FAR struct bl602_oneshot_lowerhalf_s *)lower;
  irqstate_t flags;
  uint64_t   usec;

  DEBUGASSERT(priv != NULL && callback != NULL && ts != NULL);

  if (priv->started == true)
    {
      /* Yes.. then cancel it */

      tmrinfo("Already running... cancelling\n");
      bl602_cancel(lower, NULL);
    }

  /* Save the callback information and start the timer */

  flags          = enter_critical_section();
  priv->callback = callback;
  priv->arg      = arg;

  /* Express the delay in microseconds */

  usec = (uint64_t)ts->tv_sec * USEC_PER_SEC +
         (uint64_t)(ts->tv_nsec / NSEC_PER_USEC);

  bl602_timer_setcompvalue(
    priv->tim, TIMER_COMP_ID_0, usec / (TIMER_CLK_FREQ / priv->freq));

  bl602_timer_setpreloadvalue(priv->tim, 0);
  irq_attach(priv->irq, bl602_oneshot_handler, (void *)priv);
  up_enable_irq(priv->irq);
  bl602_timer_intmask(priv->tim, TIMER_INT_COMP_0, 0);
  bl602_timer_enable(priv->tim);
  priv->started = true;

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bl602_cancel
 *
 * Description:
 *   Cancel the oneshot timer and return the time remaining on the timer.
 *
 *   NOTE: This function may execute at a high rate with no timer running (as
 *   when pre-emption is enabled and disabled).
 *
 * Input Parameters:
 *   lower   Caller allocated instance of the oneshot state structure.  This
 *           structure must have been previously initialized via a call to
 *           oneshot_initialize();
 *   ts      The location in which to return the time remaining on the
 *           oneshot timer.  A time of zero is returned if the timer is
 *           not running.
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  A call to up_timer_cancel() when
 *   the timer is not active should also return success; a negated errno
 *   value is returned on any failure.
 *
 ****************************************************************************/

static int bl602_cancel(FAR struct oneshot_lowerhalf_s *lower,
                        FAR struct timespec *           ts)
{
  FAR struct bl602_oneshot_lowerhalf_s *priv =
    (FAR struct bl602_oneshot_lowerhalf_s *)lower;
  irqstate_t flags;

  DEBUGASSERT(priv != NULL);

  /* Cancel the timer */

  if (priv->started)
    {
      flags = enter_critical_section();

      bl602_timer_disable(priv->tim);
      priv->started = false;
      up_disable_irq(priv->irq);
      bl602_timer_intmask(priv->tim, TIMER_INT_COMP_0, 1);
      priv->callback = NULL;
      priv->arg      = NULL;

      leave_critical_section(flags);
      return OK;
    }

  return -ENODEV;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: oneshot_initialize
 *
 * Description:
 *   Initialize the oneshot timer and return a oneshot lower half driver
 *   instance.
 *
 * Input Parameters:
 *   chan       Timer counter channel to be used.
 *   resolution The required resolution of the timer in units of
 *              microseconds.  NOTE that the range is restricted to the
 *              range of uint16_t (excluding zero).
 *
 * Returned Value:
 *   On success, a non-NULL instance of the oneshot lower-half driver is
 *   returned.  NULL is return on any failure.
 *
 ****************************************************************************/

FAR struct oneshot_lowerhalf_s *oneshot_initialize(int      chan,
                                                   uint16_t resolution)
{
  FAR struct bl602_oneshot_lowerhalf_s *priv;
  timer_cfg_t                           timstr;

  /* Allocate an instance of the lower half driver */

  priv = (FAR struct bl602_oneshot_lowerhalf_s *)kmm_zalloc(
    sizeof(struct bl602_oneshot_lowerhalf_s));

  if (priv == NULL)
    {
      tmrerr("ERROR: Failed to initialized state structure\n");
      return NULL;
    }

  /* Initialize the lower-half driver structure */

  priv->started = false;
  priv->lh.ops  = &g_oneshot_ops;
  priv->freq    = TIMER_CLK_FREQ / resolution;
  priv->tim     = chan;
  if (priv->tim == 0)
    {
      priv->irq = BL602_IRQ_TIMER_CH0;
    }
  else
    {
      priv->irq = BL602_IRQ_TIMER_CH1;
    }

  /* Initialize the contained BL602 oneshot timer */

  timstr.timer_ch = chan;              /* Timer channel */
  timstr.clk_src  = TIMER_CLKSRC_FCLK; /* Timer clock source */
  timstr.pl_trig_src =
    TIMER_PRELOAD_TRIG_COMP0; /* Timer count register preload trigger source
                               * slelect */

  timstr.count_mode = TIMER_COUNT_PRELOAD; /* Timer count mode */

  timstr.clock_division =
    (TIMER_CLK_DIV * resolution) - 1; /* Timer clock divison value */

  timstr.match_val0 = TIMER_MAX_VALUE; /* Timer match 0 value 0 */
  timstr.match_val1 = TIMER_MAX_VALUE; /* Timer match 1 value 0 */
  timstr.match_val2 = TIMER_MAX_VALUE; /* Timer match 2 value 0 */

  timstr.pre_load_val = TIMER_MAX_VALUE; /* Timer preload value */

  bl602_timer_intmask(chan, TIMER_INT_ALL, 1);

  /* timer disable */

  bl602_timer_disable(chan);

  bl602_timer_init(&timstr);

  return &priv->lh;
}