#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <vector>
#include <cstdint>
#include <list>
#include <sys/socket.h>
#include <netinet/in.h>

class AudioDebugger {
public:
    AudioDebugger();
    ~AudioDebugger();

    void Feed(const std::vector<int16_t>& data);
    int udp_sockfd_ = -1;
    struct sockaddr_in udp_server_addr_;
private:

};

#endif 