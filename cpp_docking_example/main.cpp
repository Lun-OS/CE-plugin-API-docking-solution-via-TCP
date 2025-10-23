#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_impl_dx11.h"
#include "CETCP.h"
#include <d3d11.h>
#include <tchar.h>
#include <vector>
#include <string>
#include <array>
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <tlhelp32.h>

// 窗口查找相关函数
BOOL IsMainWindow(HWND handle) {
    return GetWindow(handle, GW_OWNER) == (HWND)0 && IsWindowVisible(handle);
}

struct EnumWindowsData {
    DWORD process_id;
    HWND window_handle;
};

BOOL CALLBACK EnumWindowsCallback(HWND handle, LPARAM lParam) {
    EnumWindowsData& data = *(EnumWindowsData*)lParam;
    DWORD process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);
    if (data.process_id != process_id || !IsMainWindow(handle))
        return TRUE;
    data.window_handle = handle;
    return FALSE;
}

HWND FindMainWindow(DWORD process_id) {
    EnumWindowsData data;
    data.process_id = process_id;
    data.window_handle = nullptr;
    EnumWindows(EnumWindowsCallback, (LPARAM)&data);
    return data.window_handle;
}

DWORD FindProcessId(const std::wstring& processName) {
    PROCESSENTRY32 processInfo;
    processInfo.dwSize = sizeof(processInfo);

    HANDLE processesSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (processesSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    Process32First(processesSnapshot, &processInfo);
    if (!processName.compare(processInfo.szExeFile)) {
        CloseHandle(processesSnapshot);
        return processInfo.th32ProcessID;
    }

    while (Process32Next(processesSnapshot, &processInfo)) {
        if (!processName.compare(processInfo.szExeFile)) {
            CloseHandle(processesSnapshot);
            return processInfo.th32ProcessID;
        }
    }

    CloseHandle(processesSnapshot);
    return 0;
}

// Direct3D全局变量
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// 前向声明
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 日志结构
struct LogEntry {
    std::string timestamp;
    std::string message;
    ImVec4 color;

    LogEntry(const std::string& msg, ImVec4 col = ImVec4(1, 1, 1, 1)) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm timeinfo;
        localtime_s(&timeinfo, &time);
        std::stringstream ss;
        ss << std::put_time(&timeinfo, "%H:%M:%S");
        timestamp = ss.str();
        message = msg;
        color = col;
    }
};

// 应用程序UI状态
struct AppUI {
    // CETCP客户端实例
    CETCP client;

    // 连接设置
    char serverHost[128] = "127.0.0.1";
    int serverPort = 9178;
    bool isConnected = false;

    // 内存操作输入
    char addressInput[256] = "";
    char lengthInput[64] = "4";
    char valueInput[512] = "";
    std::vector<uint8_t> lastReadData;
    int writeType = 0;     // 0:Hex bytes,1:u8,2:u16,3:u32,4:u64,5:i8,6:i16,7:i32,8:i64,9:f32,10:f64
    int writeBase = 16;    // 10 or 16

    // 模块操作输入
    char moduleNameInput[128] = "";
    char moduleOffsetInput[64] = "0";
    uint64_t moduleBaseAddress = 0;

    // 指针链输入
    char ptrBaseInput[256] = "";
    int offsetCount = 2;
    std::vector<std::array<char, 64>> offsetInputs;
    std::vector<int> offsetBaseTypes; // 每个偏移的数值类型（10/16）
    char ptrLengthInput[64] = "4";

    // 日志
    std::vector<LogEntry> logs;
    bool autoScroll = true;
    int maxLogEntries = 1000;

    // 当前标签页
    int currentTab = 0;

    // 设置窗口
    bool showSettings = false;
    bool showAbout = false;

    // 心跳时间
    std::chrono::steady_clock::time_point lastHeartbeat;

    void addLog(const std::string& msg, const ImVec4& col) {
        logs.emplace_back(msg, col);
        if ((int)logs.size() > maxLogEntries) {
            logs.erase(logs.begin(), logs.begin() + ((int)logs.size() - maxLogEntries));
        }
    }
    void addInfoLog(const std::string& msg) { addLog(msg, ImVec4(0.6f, 0.8f, 1.0f, 1.0f)); }
    void addSuccessLog(const std::string& msg) { addLog(msg, ImVec4(0.2f, 1.0f, 0.2f, 1.0f)); }
    void addErrorLog(const std::string& msg) { addLog(msg, ImVec4(1.0f, 0.3f, 0.3f, 1.0f)); }
};

// 渲染连接面板
void RenderConnectionPanel(AppUI& ui) {
    ImGui::Text("Server Address:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("##host", ui.serverHost, IM_ARRAYSIZE(ui.serverHost));

    ImGui::SameLine();
    ImGui::Text("Port:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##port", &ui.serverPort);

    ImGui::SameLine();
    if (!ui.isConnected) {
        if (ImGui::Button("Connect")) {
            ui.addInfoLog("Connecting to " + std::string(ui.serverHost) + ":" + std::to_string(ui.serverPort));
            if (ui.client.connect(ui.serverHost, ui.serverPort)) {
                ui.isConnected = true;
                ui.lastHeartbeat = std::chrono::steady_clock::now();
                ui.addSuccessLog("Connected successfully");
            }
            else {
                ui.addErrorLog("Connection failed: " + ui.client.getLastError());
            }
        }
    }
    else {
        if (ImGui::Button("Disconnect")) {
            ui.client.disconnect();
            ui.isConnected = false;
            ui.addInfoLog("Disconnected");
        }
        ImGui::SameLine();
        if (ImGui::Button("Test Connection")) {
            if (ui.client.ping()) {
                ui.addSuccessLog("Ping test successful");
            }
            else {
                ui.addErrorLog("Ping test failed: " + ui.client.getLastError());
                ui.isConnected = false;
            }
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(ui.isConnected ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
        ui.isConnected ? "Connected" : "Disconnected");
}

// 渲染内存操作标签页
void RenderMemoryTab(AppUI& ui) {
    ImGui::Text("Memory Address:");
    ImGui::InputText("##address", ui.addressInput, IM_ARRAYSIZE(ui.addressInput));
    ImGui::SameLine();
    ImGui::TextDisabled("(Supports 0x prefix for hex)");

    ImGui::Text("Data Length:");
    ImGui::InputText("##length", ui.lengthInput, IM_ARRAYSIZE(ui.lengthInput));

    if (ImGui::Button("Read Memory", ImVec2(120, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        }
        else {
            try {
                uint64_t address = CETCP::parseAddress(ui.addressInput);
                uint32_t length = std::stoul(ui.lengthInput);

                if (length > 1024 * 1024) {
                    ui.addErrorLog("Data length too large (max 1MB)");
                }
                else {
                    ui.addInfoLog("Reading memory 0x" + CETCP::bytesToHex({ (uint8_t)(address >> 56), (uint8_t)(address >> 48),
                                  (uint8_t)(address >> 40), (uint8_t)(address >> 32), (uint8_t)(address >> 24),
                                  (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address }).substr(0, 16) +
                        " length: " + std::to_string(length));

                    if (ui.client.readMemory(address, length, ui.lastReadData)) {
                        std::string hexStr = CETCP::bytesToHex(ui.lastReadData);
                        ui.addSuccessLog("Read successful: " + std::to_string(ui.lastReadData.size()) + " bytes");
                        strncpy_s(ui.valueInput, hexStr.c_str(), sizeof(ui.valueInput) - 1);
                    }
                    else {
                        ui.addErrorLog("Read failed: " + ui.client.getLastError());
                    }
                }
            }
            catch (const std::exception& ex) {
                ui.addErrorLog(std::string("Parameter parsing error: ") + ex.what());
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Write Value:");
    ImGui::InputText("##writevalue", ui.valueInput, IM_ARRAYSIZE(ui.valueInput));
    ImGui::SameLine();
    const char* typeItems[] = {
        "Byte sequence (hexadecimal)",
        "Unsigned 8-bit integer",
        "Unsigned 16-bit integer",
        "Unsigned 32-bit integer",
        "Unsigned 64-bit integer",
        "Signed 8-bit integer",
        "Signed 16-bit integer",
        "Signed 32-bit integer",
        "Signed 64-bit integer",
        "32-bit floating-point (float)",
        "64-bit floating-point (double)"
    };
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##valuetype", &ui.writeType, typeItems, IM_ARRAYSIZE(typeItems));
    ImGui::SameLine();
    bool decSelected = (ui.writeBase == 10);
    if (ImGui::RadioButton("Decimal", decSelected)) ui.writeBase = 10;
    ImGui::SameLine();
    bool hexSelected = (ui.writeBase == 16);
    if (ImGui::RadioButton("Hexadecimal", hexSelected)) ui.writeBase = 16;

    if (ImGui::Button("Write Memory", ImVec2(120, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        } else {
            try {
                uint64_t address = CETCP::parseAddress(ui.addressInput);
                std::vector<uint8_t> data;
                switch (ui.writeType) {
                    case 0: { // Hex bytes
                        std::string v = ui.valueInput;
                        // 移除空白
                        v.erase(std::remove_if(v.begin(), v.end(), [](char c){ return std::isspace((unsigned char)c); }), v.end());
                        if (v.empty()) throw std::invalid_argument("empty hex");
                        if (v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                            throw std::invalid_argument("invalid hex characters");
                        if (v.size() % 2 != 0) throw std::invalid_argument("hex string length must be even");
                        data = CETCP::hexToBytes(v);
                        break;
                    }
                    case 1: case 2: case 3: case 4: { // unsigned ints
                        unsigned int sizes[] = {1,2,4,8};
                        int sz = sizes[ui.writeType - 1];
                        std::string s = ui.valueInput;
                        if (ui.writeBase == 16) {
                            if (s.size()>=2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) s = s.substr(2);
                        }
                        unsigned long long val = std::stoull(s, nullptr, ui.writeBase);
                        data.resize(sz);
                        for (int i=0;i<sz;i++) data[i] = (uint8_t)((val >> (8*i)) & 0xFF);
                        break;
                    }
                    case 5: case 6: case 7: case 8: { // signed ints
                        unsigned int sizes[] = {1,2,4,8};
                        int sz = sizes[ui.writeType - 5];
                        std::string s = ui.valueInput;
                        bool neg = (!s.empty() && s[0]=='-');
                        if (neg) s = s.substr(1);
                        if (ui.writeBase == 16) {
                            if (s.size()>=2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) s = s.substr(2);
                        }
                        long long v = std::stoll(s, nullptr, ui.writeBase);
                        if (neg) v = -v;
                        data.resize(sz);
                        for (int i=0;i<sz;i++) data[i] = (uint8_t)(((unsigned long long)v >> (8*i)) & 0xFF);
                        break;
                    }
                    case 9: { // f32
                        float f = std::stof(ui.valueInput);
                        data.resize(4);
                        std::memcpy(data.data(), &f, 4);
                        break;
                    }
                    case 10: { // f64
                        double d = std::stod(ui.valueInput);
                        data.resize(8);
                        std::memcpy(data.data(), &d, 8);
                        break;
                    }
                }
                if (data.empty()) {
                    ui.addErrorLog("No data to write");
                } else if (ui.client.writeMemory(address, data)) {
                    ui.addSuccessLog("Write success: " + std::to_string(data.size()) + " bytes");
                } else {
                    ui.addErrorLog("Write failed: " + ui.client.getLastError());
                }
            } catch (const std::exception& ex) {
                ui.addErrorLog(std::string("Parameter parsing error: ") + ex.what());
            }
        }
    }


}

// 渲染模块操作标签页
void RenderModuleTab(AppUI& ui) {
    ImGui::Text("Module Name:");
    ImGui::InputText("##modname", ui.moduleNameInput, IM_ARRAYSIZE(ui.moduleNameInput));
    ImGui::SameLine();
    ImGui::TextDisabled("(e.g: kernel32.dll)");

    if (ImGui::Button("Get Module Base", ImVec2(150, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        }
        else if (strlen(ui.moduleNameInput) == 0) {
            ui.addErrorLog("Module name cannot be empty");
        }
        else {
            ui.addInfoLog("Querying module: " + std::string(ui.moduleNameInput));
            if (ui.client.getModuleBase(ui.moduleNameInput, ui.moduleBaseAddress)) {
                std::stringstream ss;
                ss << "0x" << std::hex << std::uppercase << ui.moduleBaseAddress;
                ui.addSuccessLog("Module base: " + ss.str());
            }
            else {
                ui.addErrorLog("Get failed: " + ui.client.getLastError());
                ui.moduleBaseAddress = 0;
            }
        }
    }

    if (ui.moduleBaseAddress != 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Base: 0x%llX", ui.moduleBaseAddress);
    }

    ImGui::Separator();
    ImGui::Text("Module Offset:");
    ImGui::InputText("##modoffset", ui.moduleOffsetInput, IM_ARRAYSIZE(ui.moduleOffsetInput));
    ImGui::SameLine();
    ImGui::TextDisabled("(Supports negative and 0x prefix)");

    ImGui::Text("Data Length:");
    ImGui::InputText("##modlength", ui.lengthInput, IM_ARRAYSIZE(ui.lengthInput));

    if (ImGui::Button("Read Module+Offset", ImVec2(150, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        }
        else if (strlen(ui.moduleNameInput) == 0) {
            ui.addErrorLog("Module name cannot be empty");
        }
        else {
            try {
                int64_t offset = CETCP::parseOffset(ui.moduleOffsetInput);
                uint32_t length = std::stoul(ui.lengthInput);

                ui.addInfoLog("Reading " + std::string(ui.moduleNameInput) + " + 0x" +
                    std::to_string(offset) + " length: " + std::to_string(length));

                if (ui.client.readModuleOffset(ui.moduleNameInput, offset, length, ui.lastReadData)) {
                    std::string hexStr = CETCP::bytesToHex(ui.lastReadData);
                    strncpy_s(ui.valueInput, hexStr.c_str(), sizeof(ui.valueInput) - 1);
                    ui.addSuccessLog("Read successful: " + std::to_string(ui.lastReadData.size()) + " bytes");
                }
                else {
                    ui.addErrorLog("Read failed: " + ui.client.getLastError());
                }
            }
            catch (const std::exception& e) {
                ui.addErrorLog("Parameter parsing error: " + std::string(e.what()));
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Write Module+Offset", ImVec2(150, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        }
        else if (strlen(ui.moduleNameInput) == 0) {
            ui.addErrorLog("Module name cannot be empty");
        }
        else {
            try {
                int64_t offset = CETCP::parseOffset(ui.moduleOffsetInput);
                std::vector<uint8_t> data = CETCP::hexToBytes(ui.valueInput);

                if (data.empty()) {
                    ui.addErrorLog("Data is empty or format error");
                }
                else {
                    ui.addInfoLog("Writing " + std::string(ui.moduleNameInput) + " + 0x" +
                        std::to_string(offset) + " data: " + std::to_string(data.size()) + " bytes");

                    if (ui.client.writeModuleOffset(ui.moduleNameInput, offset, data)) {
                        ui.addSuccessLog("Write successful: " + std::to_string(data.size()) + " bytes");
                    }
                    else {
                        ui.addErrorLog("Write failed: " + ui.client.getLastError());
                    }
                }
            }
            catch (const std::exception& e) {
                ui.addErrorLog("Parameter parsing error: " + std::string(e.what()));
            }
        }
    }


}

// 渲染指针链标签页
void RenderPointerChainTab(AppUI& ui) {
    ImGui::Text("Base Address:");
    ImGui::InputText("##ptrbase", ui.ptrBaseInput, IM_ARRAYSIZE(ui.ptrBaseInput));

    ImGui::Text("Offset Count:");
    ImGui::SliderInt("##offsetcount", &ui.offsetCount, 1, 10);

    // 确保偏移输入与类型数组大小匹配
    while ((int)ui.offsetInputs.size() < ui.offsetCount) ui.offsetInputs.emplace_back();
    while ((int)ui.offsetBaseTypes.size() < ui.offsetCount) ui.offsetBaseTypes.push_back(16);
    while ((int)ui.offsetInputs.size() > ui.offsetCount) ui.offsetInputs.pop_back();
    while ((int)ui.offsetBaseTypes.size() > ui.offsetCount) ui.offsetBaseTypes.pop_back();

    ImGui::Text("Offset Chain:");
    for (int i = 0; i < ui.offsetCount; i++) {
        ImGui::PushID(i);
        std::string label = "Offset " + std::to_string(i);
        ImGui::Text("%s:", label.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##offset", ui.offsetInputs[i].data(), ui.offsetInputs[i].size());
        ImGui::SameLine();
        ImGui::RadioButton("Decimal", &ui.offsetBaseTypes[i], 10);
        ImGui::SameLine();
        ImGui::RadioButton("Hexadecimal", &ui.offsetBaseTypes[i], 16);
        ImGui::PopID();
    }

    ImGui::Text("Data Length:");
    ImGui::InputText("##ptrlength", ui.ptrLengthInput, IM_ARRAYSIZE(ui.ptrLengthInput));

    if (ImGui::Button("Read Pointer Chain", ImVec2(150, 0))) {
        if (!ui.isConnected) {
            ui.addErrorLog("Not connected to server");
        } else {
            try {
                // 基址解析：支持纯16进制地址或 模块名+偏移
                uint64_t baseAddr = 0;
                std::string baseStr = ui.ptrBaseInput;
                bool isHexAddr = true;
                for (char c : baseStr) {
                    if (!(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'x' || c == 'X'))) { isHexAddr = false; break; }
                }
                if (isHexAddr) {
                    baseAddr = CETCP::parseAddress(baseStr.c_str());
                } else {
                    std::string moduleName = baseStr;
                    uint64_t extraOffset = 0;
                    size_t plusPos = baseStr.find('+');
                    if (plusPos != std::string::npos) {
                        moduleName = baseStr.substr(0, plusPos);
                        std::string offStr = baseStr.substr(plusPos + 1);
                        // 按显示选择解析
                        if (offStr.size() >= 2 && offStr[0] == '0' && (offStr[1] == 'x' || offStr[1] == 'X')) {
                            extraOffset = std::stoull(offStr.substr(2), nullptr, 16);
                        } else if (offStr.find_first_not_of("0123456789") != std::string::npos) {
                            extraOffset = std::stoull(offStr, nullptr, 16);
                        } else {
                            extraOffset = std::stoull(offStr, nullptr, 10);
                        }
                    }
                    uint64_t modBase = 0;
                    if (!ui.client.getModuleBase(moduleName, modBase)) {
                        ui.addErrorLog("Failed to get module base: " + moduleName + " | " + ui.client.getLastError());
                        return;
                    }
                    baseAddr = modBase + extraOffset;
                }

                // 读取长度解析：十进制或十六进制（含0x/A-F）
                std::string lenStr = ui.ptrLengthInput;
                uint32_t length = 0;
                if (lenStr.size() >= 2 && lenStr[0] == '0' && (lenStr[1] == 'x' || lenStr[1] == 'X')) {
                    length = std::stoul(lenStr.substr(2), nullptr, 16);
                } else if (lenStr.find_first_not_of("0123456789") != std::string::npos) {
                    length = std::stoul(lenStr, nullptr, 16);
                } else {
                    length = std::stoul(lenStr);
                }

                // 偏移链解析（按每行的数值类型）
                std::vector<int64_t> offsets;
                for (int i = 0; i < ui.offsetCount; i++) {
                    std::string os = ui.offsetInputs[i].data();
                    int baseSel = ui.offsetBaseTypes[i];
                    if (baseSel == 16) {
                        // 强制按十六进制解析（若无0x则补上）
                        // 允许负数
                        std::string t = os;
                        size_t a = t.find_first_not_of(" \t\r\n"); if (a==std::string::npos) throw std::invalid_argument("empty offset");
                        size_t b = t.find_last_not_of(" \t\r\n"); t = t.substr(a, b-a+1);
                        bool neg = (!t.empty() && t[0]=='-');
                        size_t start = neg ? 1 : 0;
                        std::string core = t.substr(start);
                        if (!(core.size()>=2 && core[0]=='0' && (core[1]=='x'||core[1]=='X'))) {
                            t.insert(start, "0x");
                        }
                        offsets.push_back(CETCP::parseOffset(t));
                    } else {
                        // 十进制解析
                        std::string t = os;
                        size_t a = t.find_first_not_of(" \t\r\n"); if (a==std::string::npos) throw std::invalid_argument("empty offset");
                        size_t b = t.find_last_not_of(" \t\r\n"); t = t.substr(a, b-a+1);
                        bool neg = (!t.empty() && t[0]=='-');
                        if (neg) t = t.substr(1);
                        if (t.size()>=2 && t[0]=='0' && (t[1]=='x'||t[1]=='X')) t = t.substr(2);
                        long long v = std::stoll(t, nullptr, 10);
                        offsets.push_back(neg ? -v : v);
                    }
                }

                // 调试输出（十六进制显示）
                std::stringstream ss;
                ss << "[[0x" << std::hex << baseAddr;
                for (auto off : offsets) {
                    ss << " + 0x" << std::hex << off;
                }
                ss << "]";
                ui.addInfoLog("Reading pointer chain: " + ss.str());

                if (ui.client.readPointerChain(baseAddr, offsets, length, ui.lastReadData)) {
                    std::string hexStr = CETCP::bytesToHex(ui.lastReadData);
                    strncpy_s(ui.valueInput, hexStr.c_str(), sizeof(ui.valueInput) - 1);
                    ui.addSuccessLog("Read successful: " + std::to_string(ui.lastReadData.size()) + " bytes");
                } else {
                    ui.addErrorLog("Read failed: " + ui.client.getLastError());
                }
            } catch (const std::exception& ex) {
                ui.addErrorLog(std::string("Parameter parsing error: ") + ex.what());
            }
        }
    }
}

// 渲染共享数据显示区域
void RenderSharedDataDisplay(AppUI& ui) {
    ImGui::Separator();
    ImGui::Text("Data (Hex):");
    ImGui::InputTextMultiline("##hexvalue", ui.valueInput, IM_ARRAYSIZE(ui.valueInput), ImVec2(-1, 100));

    if (!ui.lastReadData.empty()) {
        ImGui::Text("Data Parse:");
        ImGui::BeginChild("DataParse", ImVec2(0, 150), true);

        // 以不同格式显示数据
        if (ui.lastReadData.size() >= 1) {
            ImGui::Text("Int8:  %d", (int8_t)ui.lastReadData[0]);
            ImGui::Text("UInt8: %u", ui.lastReadData[0]);
        }
        if (ui.lastReadData.size() >= 4) {
            uint32_t val32 = *(uint32_t*)ui.lastReadData.data();
            ImGui::Text("Int32:  %d", (int32_t)val32);
            ImGui::Text("UInt32: %u", val32);
            ImGui::Text("Float:  %.6f", *(float*)&val32);
        }
        if (ui.lastReadData.size() >= 8) {
            uint64_t val64 = *(uint64_t*)ui.lastReadData.data();
            ImGui::Text("Int64:  %lld", (int64_t)val64);
            ImGui::Text("UInt64: %llu", val64);
            ImGui::Text("Double: %.6f", *(double*)&val64);
        }

        ImGui::EndChild();
    }
}

// 渲染日志标签页
void RenderLogTab(AppUI& ui) {
    if (ImGui::Button("Clear Log")) {
        ui.logs.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto Scroll", &ui.autoScroll);
    ImGui::SameLine();
    ImGui::Text("Log Count: %d", (int)ui.logs.size());

    ImGui::Separator();
    ImGui::BeginChild("LogArea", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    for (const auto& log : ui.logs) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[%s]", log.timestamp.c_str());
        ImGui::SameLine();
        ImGui::TextColored(log.color, "%s", log.message.c_str());
    }

    if (ui.autoScroll && !ui.logs.empty()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

// 渲染主UI
void RenderUI(AppUI& ui) {
    ImGui::Begin("CEQP Control Program", nullptr, ImGuiWindowFlags_None);

    // 标题
    ImGui::Text("CEQP Control Program");
    ImGui::Separator();

    // 连接面板
    RenderConnectionPanel(ui);
    ImGui::Separator();

    // 标签页
    bool isLogTabActive = false;
    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Memory")) {
            RenderMemoryTab(ui);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Module")) {
            RenderModuleTab(ui);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Pointer Chain")) {
            RenderPointerChainTab(ui);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Log")) {
            isLogTabActive = true;
            RenderLogTab(ui);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // 在非日志标签页显示共享数据区域
    if (!isLogTabActive) {
        RenderSharedDataDisplay(ui);
    }

    ImGui::End();

    // 关于窗口
    if (ui.showAbout) {
        ImGui::Begin("About", &ui.showAbout, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("CEQP Control Program");
        ImGui::Text("Version: 1.0");
        ImGui::Separator();
        ImGui::Text("Cheat Engine TCP_UDP Plugin Client based on CEQP Protocol");
        ImGui::Text("Supports memory read/write, module operations and pointer chain parsing");
        ImGui::End();
    }
}

// Win32 + DX11 样板代码
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 注册窗口类
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
                      GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
                      _T("CEQPControlApp"), NULL };
    RegisterClassEx(&wc);

    // 创建窗口 - 初始为全屏透明
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        wc.lpszClassName, _T("CEQP Control Program"),
        WS_POPUP, 0, 0, screenWidth, screenHeight,
        NULL, NULL, wc.hInstance, NULL);
    
    // 设置透明度和颜色键
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY | LWA_ALPHA);

    // 初始化 Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // 显示窗口
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // 初始化 ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // 启用键盘控制
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // 启用手柄控制

    // 设置 ImGui 样式
    ImGui::StyleColorsDark();

    // 设置平台/渲染器后端
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 应用程序状态
    AppUI appUI;
    bool done = false;

    // 主循环
    while (!done) {
        // 查找 windows-test.exe 进程窗口
        DWORD targetPid = FindProcessId(L"windows-test.exe");
        HWND targetHwnd = nullptr;
        if (targetPid != 0) {
            targetHwnd = FindMainWindow(targetPid);
        }

        RECT targetRect = {0, 0, 0, 0};
        if (targetHwnd != nullptr) {
            GetWindowRect(targetHwnd, &targetRect);
        }

        int windowX = targetRect.left;
        int windowY = targetRect.top;
        int windowWidth = targetRect.right - targetRect.left;
        int windowHeight = targetRect.bottom - targetRect.top;

        if (windowWidth == 0 || windowHeight == 0) {
            // 如果未找到目标窗口，使用全屏模式
            windowWidth = GetSystemMetrics(SM_CXSCREEN);
            windowHeight = GetSystemMetrics(SM_CYSCREEN);
            windowX = 0;
            windowY = 0;
        }

        // 更新当前窗口的位置和大小（允许激活以支持点击）
        ::SetWindowPos(hwnd, nullptr, windowX, windowY, windowWidth, windowHeight, SWP_NOZORDER);

        // 处理 Win32 消息
        MSG msg;
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // 心跳保活：每 2 秒一次
        if (appUI.isConnected) {
            auto now = std::chrono::steady_clock::now();
            if (now - appUI.lastHeartbeat >= std::chrono::seconds(2)) {
                if (!appUI.client.ping()) {
                    appUI.addErrorLog(std::string("Heartbeat failed: ") + appUI.client.getLastError());
                    appUI.isConnected = false;
                }
                appUI.lastHeartbeat = now;
            }
        }

        // 开始 ImGui 帧
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 渲染我们的UI
        RenderUI(appUI);

        // 渲染
        ImGui::Render();
        // 设置清除颜色为黑色，配合颜色键透明度设置
        const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // 垂直同步
    }

    // 清理
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Direct3D 设备创建
bool CreateDeviceD3D(HWND hWnd) {
    // 设置交换链
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

// 前向声明 ImGui Win32 处理函数
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 消息处理
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // 禁用 ALT 应用程序菜单
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}