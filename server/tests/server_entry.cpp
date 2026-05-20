#include "../include/server.h"

#define RINGBUFFER_SIZE 64

int main() {
    InboundRing in{RINGBUFFER_SIZE};
    OutboundRing out{RINGBUFFER_SIZE};

    Server server{in, out};
    server.run();
    return 0;
}