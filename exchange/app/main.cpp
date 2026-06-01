#include "exchange.h"

#include <csignal>
#include <iostream>

int main() {
    Exchange exchange;

    std::cout << "exchange tid: " << gettid() << std::endl;

    std::signal(SIGINT, Exchange::stop);

    exchange.run();

    return 0;
}
