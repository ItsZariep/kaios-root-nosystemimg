#define DEBUG

#ifdef DEBUG
#include <stdio.h>
#define LOGV(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#define LOGD(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define LOGV(...)
#define LOGD(...)
#endif