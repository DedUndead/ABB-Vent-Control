#ifndef INCLUDES_DELAY_H_
#define INCLUDES_DELAY_H_


#include <atomic>


void delay_systick(const int& ticks);
uint32_t millis();


#endif /* INCLUDES_DELAY_H_ */