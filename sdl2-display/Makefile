CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell sdl2-config --cflags)
CFLAGS += -I../main/common
CFLAGS += -I../main/include
LDFLAGS = $(shell sdl2-config --libs) -lrt -lpthread

TARGET = sdl2-display
SRCS = main.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
