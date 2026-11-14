// 开机自启动：通过 Windows 计划任务在登录时以最高权限运行本程序
#pragma once
#include <string>

namespace autostart {

const wchar_t *taskName();
bool taskExists();
// 创建/更新计划任务：登录触发、最高权限，命令为 "<exe>" --autostart。
// 需要管理员权限；失败时 msg 给出可读原因。
bool enable(std::string &msg);
// 删除计划任务。任务不存在视为成功。
bool disable(std::string &msg);

}  // namespace autostart
