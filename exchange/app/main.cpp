#include "exchange.h"

#include <csignal>

int main() {
    Exchange exchange;

    std::signal(SIGINT, Exchange::stop);

    exchange.run();

    return 0;
}