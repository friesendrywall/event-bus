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

#ifndef EVENTBUS_H
#define EVENTBUS_H

#define EVENT_BUS_VERSION "0.50.02"

#define EVENT_BUS_FLAGS_RETAIN (1UL << 0)
#define EVENT_BUS_BITS (32 * EVENT_BUS_MASK_WIDTH)
#define EVENT_BUS_LAST_PARAM (EVENT_BUS_BITS + 1)

#include <stdbool.h>
#include <stdarg.h>

#include "event_bus_config.h"
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#ifndef EVENT_BUS_RTOS_PRIORITY
#define EVENT_BUS_RTOS_PRIORITY (configMAX_PRIORITIES - 2)
#endif

#ifndef EVENT_BUS_MASK_WIDTH
#error EVENT_BUS_MASK_WIDTH must be declared in config file
#endif

#if (EVENT_BUS_USE_TASK_NOTIFICATION_INDEX + 1) >                             \
    configTASK_NOTIFICATION_ARRAY_ENTRIES
#error configTASK_NOTIFICATION_ARRAY_ENTRIES must be greater than \
    EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
#endif

typedef struct {
  uint32_t event;
  uint32_t publishTime;
  volatile uint16_t refCount;
  uint16_t publisherId : 12;
  uint16_t dynamicAlloc : 3;
  uint16_t published : 1;
} event_t;

struct LISTENER_T {
  uint32_t eventMask[EVENT_BUS_MASK_WIDTH];
  uint32_t refCount : 16; /* Debug helper */
  uint32_t errFull : 1;
  void (*callback)(event_t *ev);
  QueueHandle_t queueHandle;
  TaskHandle_t waitingTask;
  const char * name;
  struct LISTENER_T *prev;
  struct LISTENER_T *next;
};
typedef struct LISTENER_T event_listener_t;

TaskHandle_t initEventBus(void);
void subEvent(event_listener_t *listener, uint32_t eventId);
void subEventList(event_listener_t *listener, const uint32_t *eventList);
void unSubEvent(event_listener_t *listener, uint32_t eventId);
void attachBus(event_listener_t *listener);
void detachBus(event_listener_t *listener);
void publishEvent(event_t *ev, bool retain);
BaseType_t publishToListener(event_listener_t *listener, event_t *ev,
                             TickType_t xTicksToWait);
BaseType_t publishEventFromISR(event_t *ev);
void invalidateEvent(event_t *ev);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
BaseType_t waitEvent(uint32_t event, uint32_t waitTicks);
#endif
void *eventAlloc(size_t size, uint32_t eventId, uint16_t publisherId);
void eventRelease(event_t *ev, event_listener_t *listener);
/* Debugging aids */
uint32_t eventListenerInfo(char *const buf, uint32_t bufLen);
uint32_t eventResponseInfo(char *const buf, uint32_t bufLen);
uint32_t eventPoolInfo(char *const buf, uint32_t bufLen);

#endif /* EVENTBUS_H */