/* 
 * File:   eventBus.h
 * Author: Erik Friesen
 *
 * Created on September 14, 2022, 9:27 AM
 */

#ifndef EVENTBUS_H
#define EVENTBUS_H

#define EVENT_BUS_FLAGS_RETAIN (1UL << 0)
#define EVENT_BUS_BITS 32

typedef struct {
	uint32_t event;
	void * ptr;
	uint32_t params;
	uint32_t flags;
} event_params_t;

typedef void(*eventCallback) (event_params_t*);

struct EVENT_T {
	uint32_t eventMask;
	eventCallback callback;
	struct EVENT_T * prev;
	struct EVENT_T * next;
};
typedef struct EVENT_T event_t;

void initEventBus(void);
void subscribeEvent(event_t *event);
void unSubscribeEvent(event_t *event);
void publishEvent(event_params_t *event);
void invalidateEvent(event_params_t *event);

#endif /* EVENTBUS_H */

