// 开机自启动：只调用系统自带的 schtasks.exe，不安装服务/驱动。
// 任务名固定为 "DNS Pilot"，动作是当前 exe + --autostart，登录时以最高权限运行。
// 用 XML 定义任务，避免 schtasks 默认“电池供电时不启动”“3 天后停止”等限制。
#include "autostart.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#include "util.h"

namespace {

const wchar_t *kTaskName = L"DNS Pilot";

std::wstring schtasksExe() {
    wchar_t dir[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"C:\\Windows\\System32\\schtasks.exe";
    return std::wstring(dir, n) + L"\\schtasks.exe";
}

bool run(const std::wstring &args, DWORD &code) {
    return util::runHidden(schtasksExe(), args, code);
}

std::wstring xmlEscape(const std::wstring &s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'&': o += L"&amp;"; break;
            case L'<': o += L"&lt;"; break;
            case L'>': o += L"&gt;"; break;
            case L'"': o += L"&quot;"; break;
            case L'\'': o += L"&apos;"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::wstring currentUserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (len == 0) {
        CloseHandle(tok);
        return {};
    }
    std::vector<unsigned char> buf(len);
    bool ok = GetTokenInformation(tok, TokenUser, buf.data(), len, &len);
    CloseHandle(tok);
    if (!ok) return {};
    auto *tu = reinterpret_cast<TOKEN_USER *>(buf.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sid)) return {};
    std::wstring out(sid);
    LocalFree(sid);
    return out;
}

std::wstring taskXml() {
    std::wstring exe = xmlEscape(L"\"" + util::w(util::exePath()) + L"\"");
    std::wstring sid = xmlEscape(currentUserSid());
    return std::wstring(L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n") +
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           L"  <RegistrationInfo>\n"
           L"    <Description>DNS Pilot 开机自启动（登录时以管理员权限运行）</Description>\n"
           L"    <URI>\\DNS Pilot</URI>\n"
           L"  </RegistrationInfo>\n"
           L"  <Principals>\n"
           L"    <Principal id=\"Author\">\n"
           L"      <UserId>" +
           sid +
           L"</UserId>\n"
           L"      <LogonType>InteractiveToken</LogonType>\n"
           L"      <RunLevel>HighestAvailable</RunLevel>\n"
           L"    </Principal>\n"
           L"  </Principals>\n"
           L"  <Settings>\n"
           L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
           L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           L"    <AllowHardTerminate>true</AllowHardTerminate>\n"
           L"    <StartWhenAvailable>true</StartWhenAvailable>\n"
           L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
           L"    <IdleSettings>\n"
           L"      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
           L"      <RestartOnIdle>false</RestartOnIdle>\n"
           L"    </IdleSettings>\n"
           L"    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
           L"    <Enabled>true</Enabled>\n"
           L"    <Hidden>false</Hidden>\n"
           L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
           L"    <WakeToRun>false</WakeToRun>\n"
           L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
           L"    <Priority>7</Priority>\n"
           L"  </Settings>\n"
           L"  <Triggers>\n"
           L"    <LogonTrigger>\n"
           L"      <Enabled>true</Enabled>\n"
           L"    </LogonTrigger>\n"
           L"  </Triggers>\n"
           L"  <Actions Context=\"Author\">\n"
           L"    <Exec>\n"
           L"      <Command>" +
           exe +
           L"</Command>\n"
           L"      <Arguments>--autostart</Arguments>\n"
           L"    </Exec>\n"
           L"  </Actions>\n"
           L"</Task>\n";
}

std::wstring tempXmlPath() {
    wchar_t dir[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(dir, n) + L"dnspilot-task-" +
           std::to_wstring((unsigned long)GetCurrentProcessId()) + L".xml";
}

bool writeUtf16File(const std::wstring &path, const std::wstring &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD wrote = 0;
    bool ok = WriteFile(h, bom, 2, &wrote, nullptr) && wrote == 2;
    if (ok) {
        DWORD bytes = (DWORD)(text.size() * sizeof(wchar_t));
        ok = WriteFile(h, text.data(), bytes, &wrote, nullptr) && wrote == bytes;
    }
    CloseHandle(h);
    return ok;
}

std::wstring queryArgs() {
    return L"/Query /TN \"" + std::wstring(kTaskName) + L"\"";
}

std::wstring deleteArgs() {
    return L"/Delete /TN \"" + std::wstring(kTaskName) + L"\" /F";
}

}  // namespace

const wchar_t *autostart::taskName() { return kTaskName; }

bool autostart::taskExists() {
    DWORD code = 1;
    if (!run(queryArgs(), code)) return false;
    return code == 0;
}

bool autostart::enable(std::string &msg) {
    msg.clear();
    if (!util::isAdmin()) {
        msg = "创建开机任务需要管理员权限：请先以管理员身份重启，再保存设置";
        return false;
    }
    std::wstring sid = currentUserSid();
    if (sid.empty()) {
        msg = "无法获取当前用户 SID，未创建开机任务";
        return false;
    }
    std::wstring xmlPath = tempXmlPath();
    if (xmlPath.empty() || !writeUtf16File(xmlPath, taskXml())) {
        msg = "无法写入计划任务定义文件";
        return false;
    }
    DWORD code = 1;
    std::wstring args =
        L"/Create /TN \"" + std::wstring(kTaskName) + L"\" /XML \"" + xmlPath + L"\" /F";
    bool launched = run(args, code);
    DeleteFileW(xmlPath.c_str());
    if (!launched) {
        msg = "无法调用 schtasks.exe（错误码 " + std::to_string((unsigned long)code) + "）";
        return false;
    }
    if (code != 0) {
        msg = "创建开机任务失败（schtasks 退出码 " + std::to_string((unsigned long)code) +
              "）。请确认以管理员身份运行后重试";
        return false;
    }
    return true;
}

bool autostart::disable(std::string &msg) {
    msg.clear();
    if (!taskExists()) return true;
    if (!util::isAdmin()) {
        msg = "关闭开机自启动需要管理员权限：请先以管理员身份重启，再保存设置";
        return false;
    }
    DWORD code = 1;
    if (!run(deleteArgs(), code)) {
        msg = "无法调用 schtasks.exe（错误码 " + std::to_string((unsigned long)code) + "）";
        return false;
    }
    if (code != 0) {
        msg = "删除开机任务失败（schtasks 退出码 " + std::to_string((unsigned long)code) + "）";
        return false;
    }
    return true;
}
