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

#define EVENT_BUS_VERSION "0.50.00"

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
  uint16_t refCount;
  uint16_t publisherId : 12;
  uint16_t dynamicAlloc : 1;
  uint16_t lg : 1;
} event_msg_t;

typedef void (*eventCallback)(event_msg_t *);

struct EVENT_T {
  uint32_t eventMask[EVENT_BUS_MASK_WIDTH];
  uint32_t errFull : 1;
  eventCallback callback;
  QueueHandle_t queueHandle;
  TaskHandle_t waitingTask;
  const char * name;
  struct EVENT_T *prev;
  struct EVENT_T *next;
};
typedef struct EVENT_T event_listener_t;

TaskHandle_t initEventBus(void);
void subEvent(event_listener_t *listener, uint32_t eventId);
void subEventList(event_listener_t *listener, const uint32_t *eventList);
void unSubEvent(event_listener_t *listener, uint32_t eventId);
void attachBus(event_listener_t *listener);
void detachBus(event_listener_t *listener);
void publishEvent(void *event, bool retain);
BaseType_t publishEventFromISR(void *event);
void invalidateEvent(void *event);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
BaseType_t waitEvent(uint32_t event, uint32_t waitTicks);
#endif
void *threadEventAlloc(size_t size);
void *eventAlloc(size_t size);
void eventRelease(void *event);

#endif /* EVENTBUS_H */