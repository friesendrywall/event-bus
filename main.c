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

/* Kernel includes. */
#include "event_bus.h"
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>
static StackType_t xStackTest1[512];
static StaticTask_t xTaskBufferTest1;
static StackType_t xStackTest2[512];
static StaticTask_t xTaskBufferTest2;
static uint32_t results[6];

#define STORAGE_SIZE_BYTES 2048

static uint8_t ucMessageBufferStorage1[STORAGE_SIZE_BYTES];
static uint8_t ucMessageBufferStorage2[STORAGE_SIZE_BYTES];
static MessageBufferHandle_t threadHandle1;
static MessageBufferHandle_t threadHandle2;
static StaticMessageBuffer_t xMessageBufferStruct1;
static StaticMessageBuffer_t xMessageBufferStruct2;

static void TestTask1(void *pvParameters) {
  static int i = 0;
  (void)pvParameters;
  event_t eventList = {.eventMask = 0x3F, .msgBuffHandle = threadHandle1};
  event_params_t evIgnore = {
      .event = 0x20, .len = 8, .ptr = "Event I", .ignore = threadHandle1};
  uint32_t satisfied[4] = {0};
  vTaskDelay(500 / portTICK_PERIOD_MS);
  subscribeEvent(&eventList);
  event_msg_t msg = {0};
  publishEvent(&evIgnore);
  for (;;) {
    memset(&msg, 0, sizeof(msg));
    size_t res = xMessageBufferReceive(threadHandle1, &msg, sizeof(event_msg_t),
                                       5000 / portTICK_PERIOD_MS);
    if (res == 0) {
      printf("Event timeout failure\r\n");
      ExitProcess(EXIT_FAILURE);
    }
    printf("RX-1 (%i) event(0x%04X) flags(0x%04X) %s\r\n", msg.len, msg.event,
           msg.flags, (char *)msg.data);
    switch (msg.event) {
    case 0x01:
      satisfied[0] = memcmp(msg.data, "Event 1", 7) == 0 &&
                     msg.flags & EVENT_BUS_FLAGS_RETAIN;
      break;
    case 0x02:
      satisfied[1] = memcmp(msg.data, "Event 2", 7) == 0;
      break;
    case 0x04:
      satisfied[2] = memcmp(msg.data, "Event 3", 7) == 0;
      break;
    case 0x08:
      satisfied[3] = memcmp(msg.data, "Event 4", 7) == 0;
      break;
    case 0x10:
      for (i = 0; i < 4; i++) {
        if (!satisfied[i]) {
          printf("Event %i not received\r\n", i);
          ExitProcess(EXIT_FAILURE);
        }
      }
      printf("All events received\r\n");
      ExitProcess(EXIT_SUCCESS);
      break;
    default:
      printf("Default message error event was %i\r\n", msg.event);
      ExitProcess(EXIT_FAILURE);
      break;
    }
  }
}

static void TestTask2(void *pvParameters) {
  static int i = 0;
  (void)pvParameters;
  event_params_t ev1 = {.event = 0x01,
                        .len = 8,
                        .flags = EVENT_BUS_FLAGS_RETAIN,
                        .ptr = "Event 1"};
  event_params_t ev2 = {.event = 0x02, .len = 8, .ptr = "Event 2"};
  event_params_t ev3 = {.event = 0x04, .len = 8, .ptr = "Event 3"};
  event_params_t ev4 = {.event = 0x08, .len = 8, .ptr = "Event 4"};
  event_params_t evComplete = {.event = 0x10, .len = 0, .ptr = "Done   "};
  event_t eventList = {.eventMask = 0x20, .msgBuffHandle = threadHandle2};
  event_msg_t msg = {0};
  subscribeEvent(&eventList);
  publishEvent(&ev1);
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  publishEvent(&ev2);
  publishEvent(&ev3);
  publishEvent(&ev4);
  size_t res = xMessageBufferReceive(threadHandle2, &msg, sizeof(event_msg_t),
                                     5000 / portTICK_PERIOD_MS);
  if (res == 0) {
    printf("Ignored event not received\r\n");
    ExitProcess(EXIT_FAILURE);
  }
  if (msg.event != 0x20) {
    printf("Ignored event not received\r\n");
    ExitProcess(EXIT_FAILURE);
  } else {
    printf("RX-2 (%i) event(0x%04X) flags(0x%04X) %s\r\n", msg.len, msg.event,
           msg.flags, (char *)msg.data);
  }
  publishEvent(&evComplete);
  vTaskDelay(portMAX_DELAY);
}

/*
 *
 */
int main(int argc, char **argv) {

  (void)xTaskCreateStatic(TestTask1, "Test 1", 512, NULL, 1, xStackTest1,
                          &xTaskBufferTest1);
  (void)xTaskCreateStatic(TestTask2, "Test 2", 512, NULL, 1, xStackTest2,
                          &xTaskBufferTest2);
  threadHandle1 = xMessageBufferCreateStatic(sizeof(ucMessageBufferStorage1),
                                             ucMessageBufferStorage1,
                                             &xMessageBufferStruct1);

  threadHandle2 = xMessageBufferCreateStatic(sizeof(ucMessageBufferStorage2),
                                             ucMessageBufferStorage2,
                                             &xMessageBufferStruct2);
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
    while (ulSetToNonZeroInDebuggerToContinue == 0) {
      __asm NOP;
      __asm NOP;
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
