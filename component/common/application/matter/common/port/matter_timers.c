/************************** 
* Matter Timers Related 
**************************/
#include "platform_opts.h"
#include "platform/platform_stdlib.h"

#ifdef __cplusplus
 extern "C" {
#endif

#include "stddef.h"
#include "string.h"
#include "errno.h"
#include "FreeRTOS.h"
#include "chip_porting.h"
#include "time.h"
#include "rtc_api.h"
#include "timer_api.h"
#include "task.h"

#define MICROSECONDS_PER_SECOND    ( 1000000LL )                                   /**< Microseconds per second. */
#define NANOSECONDS_PER_SECOND     ( 1000000000LL )                                /**< Nanoseconds per second. */
#define NANOSECONDS_PER_TICK       ( NANOSECONDS_PER_SECOND / configTICK_RATE_HZ ) /**< Nanoseconds per FreeRTOS tick. */
#define HOUR_PER_MILLISECOND       ( 3600 * 1000 )

#define US_OVERFLOW_MAX            (0xFFFFFFFF)
#define MATTER_SW_RTC_TIMER_ID     TIMER5

extern int FreeRTOS_errno;
#define errno FreeRTOS_errno

static gtimer_t matter_rtc_timer;
static uint64_t current_us = 0;
static volatile uint32_t rtc_counter = 0;

BOOL UTILS_ValidateTimespec( const struct timespec * const pxTimespec )
{
    BOOL xReturn = FALSE;

    if( pxTimespec != NULL )
    {
        /* Verify 0 <= tv_nsec < 1000000000. */
        if( ( pxTimespec->tv_nsec >= 0 ) &&
            ( pxTimespec->tv_nsec < NANOSECONDS_PER_SECOND ) )
        {
            xReturn = TRUE;
        }
    }

    return xReturn;
}

int _nanosleep( const struct timespec * rqtp,
               struct timespec * rmtp )
{
    int iStatus = 0;
    TickType_t xSleepTime = 0;

    /* Silence warnings about unused parameters. */
    ( void ) rmtp;

    /* Check rqtp. */
    if( UTILS_ValidateTimespec( rqtp ) == FALSE )
    {
        errno = EINVAL;
        iStatus = -1;
    }

    if( iStatus == 0 )
    {
        /* Convert rqtp to ticks and delay. */
        if( UTILS_TimespecToTicks( rqtp, &xSleepTime ) == 0 )
        {
            vTaskDelay( xSleepTime );
        }
    }

    return iStatus;
}

int __clock_gettime(struct timespec * tp)
{
    unsigned int update_tick = 0;
    long update_sec = 0, update_usec = 0, current_sec = 0, current_usec = 0;
    unsigned int current_tick = xTaskGetTickCount();

    sntp_get_lasttime(&update_sec, &update_usec, &update_tick);
    //if(update_tick) {
        long tick_diff_sec, tick_diff_ms;

        tick_diff_sec = (current_tick - update_tick) / configTICK_RATE_HZ;
        tick_diff_ms = (current_tick - update_tick) % configTICK_RATE_HZ / portTICK_RATE_MS;
        update_sec += tick_diff_sec;
        update_usec += (tick_diff_ms * 1000);
        current_sec = update_sec + update_usec / 1000000;
        current_usec = update_usec % 1000000;
    //}
    //else {
        //current_sec = current_tick / configTICK_RATE_HZ;
    //}
    tp->tv_sec = current_sec;
    tp->tv_nsec = current_usec*1000;
    //sntp_set_lasttime(update_sec,update_usec,update_tick);
    //printf("update_sec %d update_usec %d update_tick %d tvsec %d\r\n",update_sec,update_usec,update_tick,tp->tv_sec);
}

time_t _time( time_t * tloc )
{
#if 0
    /* Read the current FreeRTOS tick count and convert it to seconds. */
    time_t xCurrentTime = ( time_t ) ( xTaskGetTickCount() / configTICK_RATE_HZ );
#else
    time_t xCurrentTime;
    struct timespec tp;

    __clock_gettime(&tp);
    xCurrentTime = tp.tv_sec;
#endif
    /* Set the output parameter if provided. */
    if( tloc != NULL )
    {
        *tloc = xCurrentTime;
    }

    return xCurrentTime;
}

extern void vTaskDelay( const TickType_t xTicksToDelay );
int _vTaskDelay( const TickType_t xTicksToDelay )
{
    vTaskDelay(xTicksToDelay);

    return 0;
}

extern uint8_t matter_get_total_operational_hour(uint32_t *totalOperationalHours);
extern uint8_t matter_set_total_operational_hour(uint32_t time);
static void hourly_update_thread(void *pvParameters)
{
    uint32_t cur_hour = 0, prev_hour = 0;
    uint8_t ret = 0;
    char key[] = "temp_hour";

    // 1. Check if "temp_hour" exist in NVS
    if (checkExist(key, key) != DCT_SUCCESS)
    {
        // 2. If "temp_hour" exist, get "temp_hour" and set as "total_hour" into NVS
        if (getPref_u32_new(key, key, &prev_hour) == DCT_SUCCESS)
        {
            ret = matter_set_total_operational_hour(prev_hour);
            if (ret != 0)
            {
                printf("matter_store_total_operational_hour failed, ret=%d\n", ret);
                goto loop;
            }
            // 3. Delete "temp_hour" from NVS
            deleteKey(key, key);
        }
        else
        {
            printf("getPref_u32_new: %s not found\n", key);
            goto loop;
        }
    }

loop:
    while (1)
    {
        // 4. Every hour get Total operational hour
        ret = matter_get_total_operational_hour(&cur_hour);
        if (ret == 0)
        {
            // 5. If "prev_hour" and "cur_hour" differs, enter and store new value into NVS using "temp_hour"
            if (prev_hour != cur_hour)
            {
                prev_hour = cur_hour;
                if (setPref_new(key, key, (uint8_t *) &cur_hour, sizeof(cur_hour)) != DCT_SUCCESS)
                {
                    printf("setPref_new: temp_hour Failed\n");
                }
            }
        }
        vTaskDelay(HOUR_PER_MILLISECOND);
    }

    xTaskDelete(NULL);
}

void start_hourly_timer(void)
{
    if(xTaskCreate(hourly_update_thread, ((const char*)"hourly_update_thread"), 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
        printf("\n\r%s xTaskCreate(hourly_update_thread) failed", __FUNCTION__);
}

void matter_rtc_init()
{
    rtc_init();
}

long long matter_rtc_read()
{
    return rtc_read();
}

void matter_rtc_write(long long time)
{
    rtc_write(time);
}

uint64_t ameba_get_clock_time(void)
{
    uint64_t global_us = 0;
    current_us = gtimer_read_us(&matter_rtc_timer);
    global_us = ((uint64_t)rtc_counter * US_OVERFLOW_MAX) + (current_us);
    return global_us;
}

static void matter_timer_rtc_callback(void)
{
    rtc_counter++;
}

void matter_timer_init(void)
{
    gtimer_init(&matter_rtc_timer, MATTER_SW_RTC_TIMER_ID);
    hal_timer_set_cntmode(&matter_rtc_timer.timer_adp, 0); //use count up
    gtimer_start_periodical(&matter_rtc_timer, US_OVERFLOW_MAX, (void *)matter_timer_rtc_callback, (uint32_t) &matter_rtc_timer);
    start_hourly_timer();
}


#ifdef __cplusplus
}
#endif
