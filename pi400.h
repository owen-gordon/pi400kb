#include <pthread.h>

#ifndef HOOK_PATH
#define HOOK_PATH "/home/pi/pi500usb/hook.sh"
#endif

int initUSB();
int main();
void sendHIDReport();
