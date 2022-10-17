# event-bus
Smallish event bus for FreeRTOS

Uses high priority thread to send messages.  

set EVENT_BUS_MASK_WIDTH to desired max event count, for example 3 would be 3 * 32
total messages. 

Test Build with Visual Studio 2019 or greater.