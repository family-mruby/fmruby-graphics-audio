FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libsdl2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# The wire headers (shared memory layout, HID event packets) have one copy, in
# fmruby-core: components/fmrb_wire/include. The build context is this
# repository; the root docker-compose.yml adds that directory of the
# fmruby-core checkout next to it as the extra context "wire"
# (additional_contexts), and they are copied from there.
COPY --from=wire shm_display.h fmrb_hid_event.h /app/include/

# Copy source
COPY sdl2-display/main.c /app/

# Build
RUN gcc -Wall -Wextra -O2 \
    $(sdl2-config --cflags) \
    -I/app/include \
    -o sdl2-display main.c \
    $(sdl2-config --libs) -lrt -lpthread

CMD ["./sdl2-display"]
