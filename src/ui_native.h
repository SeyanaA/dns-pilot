// 原生桌面界面
#pragma once
#include <string>

#include "engine.h"
#include "model.h"
#include "proxy.h"
#include "stats.h"

namespace ui {

struct Context {
    ConfigStore *cfg = nullptr;
    Stats *stats = nullptr;
    Engine *engine = nullptr;
    Proxy *proxy = nullptr;
    std::string version;
    std::string relaunchArgs;
    bool startMinimized = false;      // 开机自启动：最小化到任务栏且不抢焦点
    std::string startupNotice;        // 启动时自动接管失败等提示（toast）
};

// 阻塞运行原生窗口；窗口关闭后设置 util::quitFlag()
int Run(HINSTANCE inst, Context &ctx);

}  // namespace ui
