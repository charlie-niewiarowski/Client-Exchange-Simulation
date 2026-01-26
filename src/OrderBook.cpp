//
// Created by charl on 1/17/2026.
//


#include <iostream>
#include <thread>

#include "../include/OrderBook.hpp"

void OrderBook::run() {
    std::jthread engine([this] {
       pin_thread(0);
        engine_.run();
    });

    std::jthread gateway([this] {
       pin_thread(1);
        gateway_.run();
    });
}

void OrderBook::pin_thread(Core core) {
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    HANDLE hThread = GetCurrentThread();

    if (DWORD_PTR result = SetThreadAffinityMask(hThread, mask); result == 0) {
        std::cout << GetLastError() << "\n";
    }
}
