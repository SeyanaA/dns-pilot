// 按应用 DNS 的本地代理：SOCKS5 CONNECT + HTTP CONNECT
#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "engine.h"
#include "model.h"
#include "proc.h"
#include "util.h"

class Proxy {
  public:
    Proxy(Engine &engine, ConfigStore &cfg, ProcTracker &proc);
    bool start(std::string &err);
    void stop();
    bool ready() const { return running_; }
    uint16_t port() const { return port_; }

  private:
    void acceptLoop(SOCKET listenSock);
    void handleClient(SOCKET client, int family, uint16_t peerPort);
    bool doSocks5(SOCKET client, const std::string &exe, uint32_t pid);
    bool doHttpConnect(SOCKET client, const std::string &exe, uint32_t pid);

    Engine &engine_;
    ConfigStore &cfg_;
    ProcTracker &proc_;
    std::atomic<bool> running_{false};
    SOCKET listen4_ = INVALID_SOCKET;
    SOCKET listen6_ = INVALID_SOCKET;
    std::thread thread4_, thread6_;
    uint16_t port_ = 8899;
};
