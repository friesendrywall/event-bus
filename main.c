/**
 * MIT License
 *
 * Copyright (c) 2022 Erik Friesen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "minunit.h"

/* Kernel includes. */
#include "event_bus.h"
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>

#define CMD_QUEUE_SIZE 4
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorage[CMD_QUEUE_SIZE * sizeof(void *)];
static QueueHandle_t xQueueTest = NULL;

static StaticQueue_t xStaticQueue2;
static uint8_t ucQueueStorage2[CMD_QUEUE_SIZE * sizeof(void *)];
static QueueHandle_t xQueueTest2 = NULL;

enum { EVENT_1, EVENT_2, EVENT_3, EVENT_4 };
enum { CALLBACK_1, CALLBACK_2, CALLBACK_3, CALLBACK_4 };

static StackType_t xStackTest[512];
static StaticTask_t xTaskBufferTest;
static uint32_t results[CALLBACK_4 + 1];
static uint32_t eventResult[EVENT_BUS_BITS];

typedef struct {
  event_msg_t e;
  uint32_t value;
} event_value_t;

void publishEventQ(uint32_t event, uint32_t value) {
  static event_value_t newEvent = {0};
  configASSERT(event < EVENT_BUS_BITS);
  newEvent.e.event = event;
  newEvent.value = value;
  publishEvent(&newEvent.e, false);
}

void callback1(event_msg_t *eventParams) {
  event_value_t *val = (event_value_t *)eventParams;
  results[CALLBACK_1] = val->value;
  eventResult[val->e.event] = val->value;
  printf("callback1 event(0x%X) %i (0x%p)\r\n", val->e.event, val->value,
         eventParams);
}

void callback2(event_msg_t *eventParams) {
  event_value_t *val = (event_value_t *)eventParams;
  results[CALLBACK_2] = val->value;
  eventResult[val->e.event] = val->value;
  printf("callback2 event(0x%X) %i (0x%p) %i\r\n", val->e.event,
         val->value, eventParams, val->e.publisherId);
}

void callback3(event_msg_t *eventParams) {
  event_value_t *val = (event_value_t *)eventParams;
  results[CALLBACK_3] = val->value;
  eventResult[val->e.event] = val->value;
  printf("callback3 event(0x%X) %i (0x%p)\r\n", val->e.event, val->value,
         eventParams);
}

void callback4(event_msg_t *eventParams) {
  event_value_t *val = (event_value_t *)eventParams;
  results[CALLBACK_4] = val->value;
  eventResult[val->e.event] = val->value;
  printf("callback4 event(0x%X) %i (0x%p)\r\n", val->e.event, val->value,
         eventParams);
}

event_listener_t ev1 = {.callback = callback1};

event_listener_t ev2 = {.callback = callback2};

event_listener_t ev3 = {.callback = callback3};

event_listener_t ev4 = {.callback = callback4};

int tests_run = 0;

void test_setup(void) {
  int i;
  event_value_t t = {0};
  for (i = EVENT_1; i < EVENT_4 + 1; i++) {
    t.e.event = i;
    invalidateEvent(&t.e);
  }
  memset(&results, 0, sizeof(results));
  memset(&eventResult, 0, sizeof(eventResult));
  detachBus(&ev1);
  detachBus(&ev2);
  detachBus(&ev3);
  detachBus(&ev4);
  memset(ev1.eventMask, 0, sizeof(ev1.eventMask));
  memset(ev2.eventMask, 0, sizeof(ev2.eventMask));
  memset(ev3.eventMask, 0, sizeof(ev3.eventMask));
  memset(ev4.eventMask, 0, sizeof(ev4.eventMask));
}

static const char *test_pubSub(void) {
  test_setup();
  attachBus(&ev1);
  subEvent(&ev1, EVENT_1);
  publishEventQ(EVENT_1, 0xDEADBEEF);
  mu_assert("error, pubSub != 0x1234", results[CALLBACK_1] == 0xDEADBEEF);
  return NULL;
}

static const char *test_pubSubHighBits(void) {
  test_setup();
  attachBus(&ev1);
  subEvent(&ev1, 80);
  publishEventQ(80, 0xBEEF0BEE);
  mu_assert("error, highBits != 0xBEEF0BEE", eventResult[80] == 0xBEEF0BEE);
  return NULL;
}

static const char *test_pubSubRange(void) {
  int i;
  static char info[64];
  test_setup();
  attachBus(&ev1);
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    subEvent(&ev1, i);
  }
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    publishEventQ(i, 0xAAAA0000 + i);
    sprintf(info, "error, publish %i failed 0x%X != 0x%X", i,
            eventResult[i] , 0xAAAA0000 + i);
    mu_assert(info, eventResult[i] == 0xAAAA0000 + i);
  }
  return NULL;
}

static const char *test_pubFromISR(void) {
  event_value_t t = {.e = {.event = EVENT_1}, .value = 0xBEEF};
  test_setup();
  attachBus(&ev1);
  subEvent(&ev1, EVENT_1);
  (void)publishEventFromISR(&t.e);
  vTaskDelay(10);
  mu_assert("error, retain results != 0xBEEF", results[CALLBACK_1] == 0xBEEF);
  return NULL;
}

static const char *test_retain(void) {
  test_setup();
  event_value_t t = {.e = {.event = EVENT_1}, .value = 0x1234};
  attachBus(&ev1);
  publishEvent(&t.e, true);
  subEvent(&ev1, EVENT_1);
  mu_assert("error, retain results != 0x1234", results[CALLBACK_1] == 0x1234);
  return NULL;
}

static const char *test_invalidate(void) {
  test_setup();
  event_value_t t = {.e = {.event = EVENT_1}, .value = 0x1234};
  attachBus(&ev1);
  publishEvent(&t.e, true);
  invalidateEvent(&t.e);
  subEvent(&ev1, EVENT_1);
  mu_assert("error, Invalidate results != 0", results[CALLBACK_1] == 0);
  return NULL;
}

static const char *test_subscribeArray(void) {
  event_value_t t1 = {.e = {.event = EVENT_1}, .value = 0xE1};
  event_value_t t2 = {.e = {.event = EVENT_2}, .value = 0xE2};
  event_value_t t3 = {.e = {.event = EVENT_3}, .value = 0xE3};
  event_value_t t4 = {.e = {.event = EVENT_4}, .value = 0xE4};
  test_setup();
  attachBus(&ev1);
  uint32_t list[] = {EVENT_1, EVENT_2, EVENT_3, EVENT_4, EVENT_BUS_LAST_PARAM};
  subEventList(&ev1, list);
  publishEvent(&t1.e, false);
  publishEvent(&t2.e, false);
  publishEvent(&t3.e, false);
  publishEvent(&t4.e, false);
  mu_assert("error, event 1 != 0xE1", eventResult[EVENT_1] == 0xE1);
  mu_assert("error, event 2 != 0xE2", eventResult[EVENT_2] == 0xE2);
  mu_assert("error, event 3 != 0xE3", eventResult[EVENT_3] == 0xE3);
  mu_assert("error, event 4 != 0xE4", eventResult[EVENT_4] == 0xE4);
  return NULL;
}

static const char *test_detachBus(void) {
  event_value_t t = {.e = {.event = EVENT_1}, .value = 0x4321};
  test_setup();
  results[CALLBACK_1] = 0x1111;
  attachBus(&ev1);
  subEvent(&ev1, EVENT_1);
  detachBus(&ev1);
  publishEvent(&t.e, true);
  mu_assert("error, detachBus failed", results[CALLBACK_1] == 0x1111);
  return NULL;
}

static const char *test_filterRX(void) {
  event_value_t t1 = {.e = {.event = EVENT_1}, .value = 0xE1};
  event_value_t t2 = {.e = {.event = EVENT_2}, .value = 0xE2};
  event_value_t t3 = {.e = {.event = EVENT_3}, .value = 0xE3};
  event_value_t t4 = {.e = {.event = EVENT_4}, .value = 0xE4};
  test_setup();
  attachBus(&ev1);
  uint32_t list[] = {EVENT_1,EVENT_4, EVENT_BUS_LAST_PARAM};
  subEventList(&ev1, list);
  publishEvent(&t1.e, false);
  publishEvent(&t2.e, false);
  publishEvent(&t3.e, false);
  publishEvent(&t4.e, false);
  mu_assert("error, filteredRX event 1 != 0xE1", eventResult[EVENT_1] == 0xE1);
  mu_assert("error, filteredRX event 2 != 0x00", eventResult[EVENT_2] == 0x00);
  mu_assert("error, filteredRX event 3 != 0x00", eventResult[EVENT_3] == 0x00);
  mu_assert("error, filteredRX event 4 != 0xE4", eventResult[EVENT_4] == 0xE4);
  return NULL;
}

static const char *test_multipleRX(void) {
  event_value_t t1 = {.e = {.event = EVENT_1}, .value = 0xAA};
  test_setup();
  attachBus(&ev1);
  attachBus(&ev2);
  attachBus(&ev3);
  attachBus(&ev4);
  uint32_t list[] = {EVENT_1, EVENT_4, EVENT_BUS_LAST_PARAM};
  subEventList(&ev1, list);
  subEventList(&ev2, list);
  subEventList(&ev3, list);
  subEvent(&ev4, EVENT_1);
  publishEvent(&t1.e, false);
  mu_assert("error, event 1 != 0xAA", results[CALLBACK_1] == 0xAA);
  mu_assert("error, event 2 != 0xAA", results[CALLBACK_2] == 0xAA);
  mu_assert("error, event 3 != 0xAA", results[CALLBACK_3] == 0xAA);
  mu_assert("error, event 4 != 0xAA", results[CALLBACK_4] == 0xAA);
  return NULL;
}

void vTimerCallback(TimerHandle_t xTimer) {
  static event_value_t t1 = {.e = {.event = EVENT_1}, .value = 0xCC};
  publishEvent(&t1.e, false);
}

static const char *test_waitEvent(void) {
  static TimerHandle_t xTimer;
  static StaticTimer_t xTimerBuffer;
  xTimer = xTimerCreateStatic("Timer", 250 / portTICK_PERIOD_MS, pdFALSE,
                              (void *)0, vTimerCallback, &xTimerBuffer);
  test_setup();
  xTimerStart(xTimer, 0);
  mu_assert("error, event wait != pdPASS",
            waitEvent(EVENT_1, 1000 / portTICK_PERIOD_MS) == pdPASS);
  xTimerStop(xTimer, 0);
  return NULL;
}

static const char *test_waitEventFail(void) {
  static TimerHandle_t xTimer;
  static StaticTimer_t xTimerBuffer;
  xTimer = xTimerCreateStatic("Timer", 250 / portTICK_PERIOD_MS, pdFALSE,
                              (void *)0, vTimerCallback, &xTimerBuffer);
  test_setup();
  xTimerStart(xTimer, 0);
  mu_assert("error, event wait != pdFAIL",
            waitEvent(EVENT_2, 1000 / portTICK_PERIOD_MS) == pdFAIL);
  xTimerStop(xTimer, 0);
  return NULL;
}

static const char *test_queueRX(void) {
  static TimerHandle_t xTimer;
  static StaticTimer_t xTimerBuffer;
  xTimer = xTimerCreateStatic("Timer", 250 / portTICK_PERIOD_MS, pdFALSE,
                              (void *)0, vTimerCallback, &xTimerBuffer);

  event_value_t * rx;
  test_setup();
  ev1.callback = NULL;
  ev1.queueHandle = xQueueTest;
  attachBus(&ev1);
  uint32_t list[] = {EVENT_1, EVENT_4, EVENT_BUS_LAST_PARAM};
  subEventList(&ev1, list);
  xTimerStart(xTimer, 0);
  BaseType_t result = xQueueReceive(xQueueTest, &rx, 5000 / portTICK_PERIOD_MS);
  xTimerStop(xTimer, 0);
  unSubEvent(&ev1, EVENT_1);
  unSubEvent(&ev1, EVENT_4);
  mu_assert("error, queued event != 0xCC", rx->value == 0xCC);
  return NULL;
}

void vTimerAllocatedCallback(TimerHandle_t xTimer) {
  event_value_t *tx = eventAlloc(sizeof(event_value_t), EVENT_1, 0);
  configASSERT(tx != NULL);
  tx->value = 0xB0;
  publishEvent(&tx->e, false);
}

static const char *test_AllocatedEvent(void) {
  static TimerHandle_t xTimer;
  static StaticTimer_t xTimerBuffer;
  xTimer = xTimerCreateStatic("Timer", 250 / portTICK_PERIOD_MS, pdFALSE, (void *)0,
                         vTimerAllocatedCallback, &xTimerBuffer);
  event_value_t *empty = threadEventAlloc(sizeof(event_value_t), 0);
  test_setup();
  event_value_t *rx;
  event_value_t *rx2;
  ev1.callback = NULL;
  ev1.queueHandle = xQueueTest;
  attachBus(&ev1);
  ev2.callback = NULL;
  ev2.queueHandle = xQueueTest2;
  attachBus(&ev2);
  uint32_t list[] = {EVENT_1, EVENT_4, EVENT_BUS_LAST_PARAM};
  subEventList(&ev1, list);
  subEventList(&ev2, list);
  xTimerStart(xTimer, 0);
  BaseType_t result1 =
      xQueueReceive(xQueueTest, &rx, 5000 / portTICK_PERIOD_MS);
  BaseType_t result2 =
      xQueueReceive(xQueueTest2, &rx2, 5000 / portTICK_PERIOD_MS);
  xTimerStop(xTimer, 0);
  mu_assert("error, Allocated event 1 != 0xB0", rx->value == 0xB0);
  mu_assert("error, Allocated event 2 != 0xB0", rx->value == 0xB0);
  eventRelease((event_msg_t *)rx);
  mu_assert("error, RefCount != 1", rx->e.refCount == 1);
  eventRelease((event_msg_t *)rx);
  eventRelease((event_msg_t *)empty);
  return NULL;
}

static const char *test_StaticMsg(void) {
  static event_value_t msg = {.e = {.event = EVENT_1}, .value = 0xEF};
  event_value_t *tx = &msg;
  test_setup();
  event_value_t *rx;
  publishToQueue(xQueueTest, tx, portMAX_DELAY);
  // xQueueSendToBack(xQueueTest, &tx, portMAX_DELAY);
  BaseType_t result1 =
      xQueueReceive(xQueueTest, &rx, 500 / portTICK_PERIOD_MS);

  mu_assert("error, Static event != 0xEF", rx->value == 0xEF);

  return NULL;
}

static const char *all_tests() {
  mu_run_test(test_pubSub);
  mu_run_test(test_pubSubHighBits);
  mu_run_test(test_pubSubRange);
  mu_run_test(test_pubFromISR);
  mu_run_test(test_retain);
  mu_run_test(test_invalidate);
  mu_run_test(test_subscribeArray);
  mu_run_test(test_detachBus);
  mu_run_test(test_filterRX);
  mu_run_test(test_multipleRX);
  mu_run_test(test_waitEvent);
  mu_run_test(test_waitEventFail);
  mu_run_test(test_queueRX);
  mu_run_test(test_AllocatedEvent);
  mu_run_test(test_StaticMsg);
  return NULL;
}

static void TestTask(void *pvParameters) {
  static int i = 0;
  (void)pvParameters;

  const char *result = all_tests();
  if (result != NULL) {
    printf("%s\n", result);
  } else {
    printf("ALL TESTS PASSED\n");
  }
  printf("Tests run: %d\n", tests_run);
  ExitProcess(result != NULL);
}

/*
 *
 */
int main(int argc, char **argv) {

  (void)xTaskCreateStatic(TestTask, "Test something", 512, NULL, 1, xStackTest,
                          &xTaskBufferTest);
  xQueueTest = xQueueCreateStatic(CMD_QUEUE_SIZE, sizeof(void*),
                                 ucQueueStorage, &xStaticQueue);
  xQueueTest2 = xQueueCreateStatic(CMD_QUEUE_SIZE, sizeof(void *),
                                  ucQueueStorage2, &xStaticQueue2);
  initEventBus();
  vTaskStartScheduler();
  return (EXIT_SUCCESS);
}

void vApplicationMallocFailedHook(void) {
  /* vApplicationMallocFailedHook() will only be called if
  configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
  function that will get called if a call to pvPortMalloc() fails.
  pvPortMalloc() is called internally by the kernel whenever a task, queue,
  timer or semaphore is created.  It is also called by various parts of the
  demo application.  If heap_1.c, heap_2.c or heap_4.c is being used, then the
  size of the	heap available to pvPortMalloc() is defined by
  configTOTAL_HEAP_SIZE in FreeRTOSConfig.h, and the xPortGetFreeHeapSize()
  API function can be used to query the size of free heap space that remains
  (although it does not provide information on how the remaining heap might be
  fragmented).  See http://www.freertos.org/a00111.html for more
  information. */
  vAssertCalled(__LINE__, __FILE__);
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook(void) { Sleep(0); }
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName) {
  (void)pcTaskName;
  (void)pxTask;

  /* Run time stack overflow checking is performed if
  configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
  function is called if a stack overflow is detected.  This function is
  provided as an example only as stack overflow checking does not function
  when running the FreeRTOS Windows port. */
  vAssertCalled(__LINE__, __FILE__);
}
/*-----------------------------------------------------------*/

void vApplicationTickHook(void) {
  /* This function will be called by each tick interrupt if
  configUSE_TICK_HOOK is set to 1 in FreeRTOSConfig.h.  User code can be
  added here, but the tick hook is called from an interrupt context, so
  code must not attempt to block, and only the interrupt safe FreeRTOS API
  functions can be used (those that end in FromISR()). */
}
/*-----------------------------------------------------------*/

void vApplicationDaemonTaskStartupHook(void) {
  /* This function will be called once only, when the daemon task starts to
  execute	(sometimes called the timer task).  This is useful if the
  application includes initialisation code that would benefit from executing
  after the scheduler has been started. */
}
/*-----------------------------------------------------------*/

void vAssertCalled(unsigned long ulLine, const char *const pcFileName) {
  static BaseType_t xPrinted = pdFALSE;
  volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;

  /* Called if an assertion passed to configASSERT() fails.  See
  http://www.freertos.org/a00110.html#configASSERT for more information. */

  /* Parameters are not used. */
  (void)ulLine;
  (void)pcFileName;

  taskENTER_CRITICAL();
  {

    /* You can step out of this function to debug the assertion by using
    the debugger to set ulSetToNonZeroInDebuggerToContinue to a non-zero
    value. */
    printf("vAssertCalled @ %s:%i\r\n", pcFileName, ulLine);
    while (ulSetToNonZeroInDebuggerToContinue == 0) {
      __asm NOP;
      __asm NOP;
      Sleep(10);
    }
  }
  taskEXIT_CRITICAL();
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize) {
  /* If the buffers to be provided to the Timer task are declared inside this
  function then they must be declared static - otherwise they will be allocated
  on the stack and so not exists after this function exits. */
  static StaticTask_t xTimerTaskTCB;
  static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

  /* Pass out a pointer to the StaticTask_t structure in which the Timer
  task's state will be stored. */
  *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

  /* Pass out the array that will be used as the Timer task's stack. */
  *ppxTimerTaskStackBuffer = uxTimerTaskStack;

  /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
  Note that, as the array is necessarily of type StackType_t,
  configTIMER_TASK_STACK_DEPTH is specified in words, not bytes. */
  *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize) {
  /* If the buffers to be provided to the Idle task are declared inside this
  function then they must be declared static - otherwise they will be allocated
  on the stack and so not exists after this function exits. */
  static StaticTask_t xIdleTaskTCB;
  static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

  /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
  state will be stored. */
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

  /* Pass out the array that will be used as the Idle task's stack. */
  *ppxIdleTaskStackBuffer = uxIdleTaskStack;

  /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
  Note that, as the array is necessarily of type StackType_t,
  configMINIMAL_STACK_SIZE is specified in words, not bytes. */
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
