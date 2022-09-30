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
static StackType_t xStackTest[512];
static StaticTask_t xTaskBufferTest;
static uint32_t results[6];

void callback1(event_params_t *eventParams) {
  results[1] = eventParams->params;
  printf("callback1 event(0x%X) %i (0x%p)\r\n", eventParams->event,
         eventParams->params, eventParams->ptr);
}

void callback2(event_params_t *eventParams) {
  results[2] = eventParams->params;
  printf("callback2 event(0x%X) %i (0x%p)\r\n", eventParams->event,
         eventParams->params, eventParams->ptr);
}

void callback3(event_params_t *eventParams) {
  results[3] = eventParams->params;
  printf("callback3 event(0x%X) %i (0x%p)\r\n", eventParams->event,
         eventParams->params, eventParams->ptr);
}

void callback4(event_params_t *eventParams) {
  results[4] = eventParams->params;
  printf("callback4 event(0x%X) %i (0x%p)\r\n", eventParams->event,
         eventParams->params, eventParams->ptr);
}

void callback5(event_params_t *eventParams) {
  results[5] = eventParams->params;
  printf("callback5 event(0x%X) %i (0x%p)\r\n", eventParams->event,
         eventParams->params, eventParams->ptr);
}

event_t ev1 = {.eventMask = 0x01, .callback = callback1};

event_t ev2 = {.eventMask = 0x02, .callback = callback2};

event_t ev3 = {.eventMask = 0x04, .callback = callback3};

event_t ev4 = {.eventMask = 0xFF, .callback = callback4};

static void TestTask(void *pvParameters) {
  static int i = 0;
  (void)pvParameters;
  event_params_t t;
  t.ptr = NULL;
  t.event = 1UL;
  t.params = 123456;
  t.flags = EVENT_BUS_FLAGS_RETAIN;
  // Test retain
  publishEvent(&t);
  if (results[1]) {
    printf("Test failed\r\n");
    ExitProcess(EXIT_FAILURE);
  }
  subscribeEvent(&ev1);
  if (results[1] != 123456) {
    printf("Test failed %i != %i\r\n", results[1], 123456);
    ExitProcess(EXIT_FAILURE);
  }
  printf("Passed Retain test\r\n");
  // Test delete
  results[1] = 0;
  unSubscribeEvent(&ev1);
  invalidateEvent(&t);
  subscribeEvent(&ev1);
  if (results[1] != 0) {
    printf("Test failed %i != %i\r\n", results[1], 0);
    ExitProcess(EXIT_FAILURE);
  }
  printf("Passed Invalidate test\r\n");
  subscribeEvent(&ev1);
  subscribeEvent(&ev2);
  subscribeEvent(&ev3);
  subscribeEvent(&ev4);
  t.flags = 0;
  t.params = 2;
  t.event = 0x02;
  publishEvent(&t);
  t.params = 4;
  t.event = 0x08;
  publishEvent(&t);
  if (results[2] != 2 && results[4] != 4) {
    printf("Test failed at core publish\r\n");
    ExitProcess(EXIT_FAILURE);
  }
  printf("All tests passed\r\n");
  ExitProcess(EXIT_FAILURE);
}

/*
 *
 */
int main(int argc, char **argv) {

  (void)xTaskCreateStatic(TestTask, "Test something", 512, NULL, 1, xStackTest,
                          &xTaskBufferTest);
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
