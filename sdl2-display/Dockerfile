FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libsdl2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy shared headers
COPY main/common/shm_display.h /app/include/
COPY main/include/fmrb_hid_event.h /app/include/

# Copy source
COPY sdl2-display/main.c /app/

# Build
RUN gcc -Wall -Wextra -O2 \
    $(sdl2-config --cflags) \
    -I/app/include \
    -o sdl2-display main.c \
    $(sdl2-config --libs) -lrt -lpthread

CMD ["./sdl2-display"]
