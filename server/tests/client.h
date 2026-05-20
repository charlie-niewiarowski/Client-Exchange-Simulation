//
// Created by cniew on 5/17/26.
//

#ifndef CLIENT_H
#define CLIENT_H

class Client {
public:
    void run();
private:
    int fd = -1;

    void sendMessage();
    void readResponse();
};



#endif //CLIENT_H
