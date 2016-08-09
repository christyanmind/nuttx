/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/timers/rtc.h>
#include <arch/board/board.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

#include "up_arch.h"

#include "kinetis_config.h"
#include "chip.h"
#include "chip/kinetis_rtc.h"
#include "chip/kinetis_sim.h"
#include "kinetis.h"
#include "kinetis_alarm.h"

#if defined(CONFIG_RTC)

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/

/************************************************************************************
 * Private Data
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
static alarmcb_t g_alarmcb;
#endif

/************************************************************************************
 * Private Declarations
 ************************************************************************************/

static int kinetis_rtc_interrupt(int irq, void *context);

/************************************************************************************
 * Public Data
 ************************************************************************************/

volatile bool g_rtc_enabled = false;

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: up_rtc_initialize
 *
 * Description:
 *   Initialize the hardware RTC per the selected configuration.  This function is
 *   called once during the OS initialization sequence
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_initialize(void)
{
  int regval;

  /* enable RTC module */
  regval = getreg32(KINETIS_SIM_SCGC6);
  regval |= SIM_SCGC6_RTC;
  putreg32(regval, KINETIS_SIM_SCGC6);

  /* disable counters (just in case) */
  putreg32(0, KINETIS_RTC_SR);

  /* enable oscilator */
  putreg32(RTC_CR_SC16P | RTC_CR_SC4P | RTC_CR_OSCE, KINETIS_RTC_CR); /* capacitance values from teensyduino */
  /* TODO: delay some time (1024 cycles? would be 30ms) */

  /* disable interrupts */
  putreg32(0, KINETIS_RTC_IER);

  /* reset flags requires writing the seconds register, the following line avoids altering any stored time value */
  putreg32(getreg32(KINETIS_RTC_TSR), KINETIS_RTC_TSR);

#if defined(CONFIG_RTC_ALARM)
  /* enable alarm interrupts */
  irq_attach(KINETIS_IRQ_RTC, kinetis_rtc_interrupt);
  up_enable_irq(KINETIS_IRQ_RTC);
#endif

  /* enable counters */
  putreg32(RTC_SR_TCE, KINETIS_RTC_SR);

  /* mark RTC enabled */
  g_rtc_enabled = true;

  return OK;
}

/************************************************************************************
 * Name: up_rtc_time
 *
 * Description:
 *   Get the current time in seconds.  This is similar to the standard time()
 *   function.  This interface is only required if the low-resolution RTC/counter
 *   hardware implementation selected.  It is only used by the RTOS during
 *   initialization to set up the system time when CONFIG_RTC is set but neither
 *   CONFIG_RTC_HIRES nor CONFIG_RTC_DATETIME are set.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   The current time in seconds
 *
 ************************************************************************************/

#ifndef CONFIG_RTC_HIRES
time_t up_rtc_time(void)
{
  return getreg32(KINETIS_RTC_TSR);
}
#endif

/************************************************************************************
 * Name: up_rtc_gettime
 *
 * Description:
 *   Get the current time from the high resolution RTC clock/counter.  This interface
 *   is only supported by the high-resolution RTC/counter hardware implementation.
 *   It is used to replace the system timer.
 *
 * Input Parameters:
 *   tp - The location to return the high resolution time value.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_HIRES
int up_rtc_gettime(FAR struct timespec *tp)
{
  irqstate_t flags;
  uint32_t seconds, prescaler, prescaler2;

  /*
   * get prescaler and seconds register. this is in a loop which
   * ensures that registers will be re-read if during the reads the
   * prescaler has wrapped-around
   */

  flags = enter_critical_section();
  do
    {
      prescaler = getreg32(KINETIS_RTC_TPR);
      seconds = getreg32(KINETIS_RTC_TSR);
      prescaler2 = getreg32(KINETIS_RTC_TPR);
    }
  while (prescaler > prescaler2);
  leave_critical_section(flags);

  /* build seconds + nanoseconds from seconds and prescaler register */
  tp->tv_sec = seconds;
  tp->tv_nsec = prescaler * (1000000000 / CONFIG_RTC_FREQUENCY);
  return OK;
}
#endif

/************************************************************************************
 * Name: up_rtc_settime
 *
 * Description:
 *   Set the RTC to the provided time.  All RTC implementations must be able to
 *   set their time based on a standard timespec.
 *
 * Input Parameters:
 *   tp - the time to use
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_settime(FAR const struct timespec *tp)
{
  irqstate_t flags;
  uint32_t seconds, prescaler;

  seconds = tp->tv_sec;
  prescaler = tp->tv_nsec * (CONFIG_RTC_FREQUENCY / 1000000000);

  flags = enter_critical_section();

  putreg32(0, KINETIS_RTC_SR); /* disable counter */

  putreg32(prescaler, KINETIS_RTC_TPR); /* always write prescaler first */
  putreg32(seconds, KINETIS_RTC_TSR);

  putreg32(RTC_SR_TCE, KINETIS_RTC_SR); /* re-enable counter */

  leave_critical_section(flags);

  return OK;
}

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: kinetis_rtc_setalarm
 *
 * Description:
 *   Set up an alarm.
 *
 * Input Parameters:
 *   tp - the time to set the alarm
 *   callback - the function to call when the alarm expires.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
int kinetis_rtc_setalarm(FAR const struct timespec *tp, alarmcb_t callback)
{
  /* Is there already something waiting on the ALARM? */
  if (g_alarmcb == NULL)
  {
    /* No.. Save the callback function pointer */

    g_alarmcb = callback;

    /* Enable and set RTC alarm */

    putreg32(tp->tv_sec, KINETIS_RTC_TAR); /* set alarm (also resets flags) */
    putreg32(RTC_IER_TAIE, KINETIS_RTC_IER); /* enable alarm interrupt */

    return OK;
  }
  else
    return -EBUSY;
}
#endif

/************************************************************************************
 * Name: kinetis_rtc_cancelalarm
 *
 * Description:
 *   Cancel a pending alarm alarm
 *
 * Input Parameters:
 *   none
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

#ifdef CONFIG_RTC_ALARM
int kinetis_rtc_cancelalarm(void)
{
  if (g_alarmcb != NULL)
  {
    /* Cancel the global callback function */

    g_alarmcb = NULL;

    /* Unset the alarm */

    putreg32(0, KINETIS_RTC_IER); /* disable alarm interrupt */

    return OK;
  }
  else
    return -ENODATA;
}
#endif

/************************************************************************************
 * Name: kinetis_rtc_interrupt
 *
 * Description:
 *    RTC interrupt service routine
 *
 * Input Parameters:
 *   irq - The IRQ number that generated the interrupt
 *   context - Architecture specific register save information.
 *
 * Returned Value:
 *   Zero (OK) on success; A negated errno value on failure.
 *
 ************************************************************************************/

#if defined(CONFIG_RTC_ALARM)
static int kinetis_rtc_interrupt(int irq, void *context)
{
  if (g_alarmcb != NULL)
  {
    /* Alarm callback */
    g_alarmcb();
    g_alarmcb = NULL;
  }

  /* Clear pending flags, disable alarm */
  putreg32(0, KINETIS_RTC_TAR); /* unset alarm (resets flags) */
  putreg32(0, KINETIS_RTC_IER); /* disable alarm interrupt */

  return 0;
}
#endif

#endif // KINETIS_RTC
