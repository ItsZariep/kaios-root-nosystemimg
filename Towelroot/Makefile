CROSS_COMPILE ?= ~/Downloads/armv7l-linux-musleabihf-cross/bin/armv7l-linux-musleabihf-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -static -O2 -Wall -Wextra -pthread
LDFLAGS = -static -pthread
TARGET = futex
SRCS = main.c futex_requeue.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

push: $(TARGET)
	adb push $(TARGET) /data/local/tmp/futex

run: push
	adb shell "/data/local/tmp/futex 0 2 0 0"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all push run clean