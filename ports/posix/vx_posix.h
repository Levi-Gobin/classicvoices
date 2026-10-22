// Force-included (-include) into every core/*.c compile on Linux and Android.
// Only Darwin extensions need covering here: the rest of the engine is POSIX.
#pragma once
#include <pthread.h>

// Apple QoS thread attributes: no-ops (the worker thread is fast enough).
#define pthread_attr_set_qos_class_np(attr, cls, pri) 0
#define QOS_CLASS_USER_INTERACTIVE 0
