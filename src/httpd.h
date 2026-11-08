// 本地可视化面板 + JSON API
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "engine.h"
#include "model.h"
#include "proxy.h"
#include "stats.h"
#include "systemdns.h"
#include "util.h"

class Httpd {
  public:
    Httpd(Engine &engine, ConfigStore &cfg, Stats &stats, Proxy &proxy, std::string version,
          std::string relaunchArgs, std::function<void()> requestQuit);
    bool start(std::string &err);
    void stop();
    uint16_t port() const { return port_; }

  private:
    void acceptLoop(SOCKET listenSock);
    void handleConn(SOCKET client);
    std::string route(const std::string &method, const std::string &path,
                      const std::map<std::string, std::string> &query,
                      const std::map<std::string, std::string> &headers, const std::string &body,
                      int &status);

    Engine &engine_;
    ConfigStore &cfg_;
    Stats &stats_;
    Proxy &proxy_;
    std::string version_;
    std::string relaunchArgs_;
    std::function<void()> requestQuit_;
    std::atomic<bool> running_{false};
    SOCKET listen_ = INVALID_SOCKET;
    std::thread thread_;
    uint16_t port_ = 8765;
    uint64_t startMs_ = 0;
};
