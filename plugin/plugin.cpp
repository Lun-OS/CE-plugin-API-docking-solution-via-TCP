// tcp_udp_plugin.cpp: CE 插件，提供 TCP 收发包与 CEQP 服务的 Lua API
// 作者：Lun.  QQ:1596534228   github:Lun-OS
#include "pch.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <TlHelp32.h>
#include <string>
#include <mutex>
#include "cepluginsdk.h"
#include <thread>
#include <atomic>
#include <deque>
// 新增：CEQP 使用 std::vector、tolower、固定宽度整数类型
#include <vector>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#pragma comment(lib, "Ws2_32.lib")
#include <tchar.h>

// 前向声明：TCHAR -> UTF-8 转换
static std::string tchar_to_utf8(const TCHAR* p);
// 前向声明：Lua 测试环境切换接口
static int lua_CEQPSetTestEnv(lua_State* L);

static ExportedFunctions Exported; // 从 CE 传入的导出函数表
static bool g_wsa_inited = false;
static std::mutex g_net_mtx;
static SOCKET g_tcp = INVALID_SOCKET;
static int g_plugin_id = -1;
static bool g_test_env = false; // 测试环境：输出详细日志

static void debug_logf(const char* fmt, ...){
    if (!g_test_env) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
#ifdef _MSC_VER
    vsnprintf(buf, sizeof(buf), fmt, ap);
#else
    vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    va_end(ap);
    OutputDebugStringA(buf);
}

static int lua_CEQPSetTestEnv(lua_State* L)
{
    int enable = (int)luaL_optinteger(L, 1, 1);
    g_test_env = !!enable;
    debug_logf("[CEQP] TestEnv set to %s\n", g_test_env?"true":"false");
    lua_pushboolean(L, TRUE);
    return 1;
}
// 异步接收状态
static std::thread g_tcp_rx_thread;
static std::atomic<bool> g_tcp_rx_running(false);
static std::mutex g_tcp_rx_q_mtx;
static std::deque<std::string> g_tcp_rx_q;


static bool net_init()
{
    if (!g_wsa_inited)
    {
        WSADATA wsaData;
        int r = WSAStartup(MAKEWORD(2, 2), &wsaData);
        g_wsa_inited = (r == 0);
    }
    return g_wsa_inited;
}

static void net_close_socket(SOCKET &s)
{
    if (s != INVALID_SOCKET)
    {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

// TCP
static bool tcp_connect(const char* host, int port)
{
    if (!net_init()) return false;
    std::lock_guard<std::mutex> lock(g_net_mtx);
    net_close_socket(g_tcp);

    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    char portstr[32]; sprintf_s(portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &result) != 0) return false;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = result; p; p = p->ai_next)
    {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0)
        {
            g_tcp = s;
            // 低延迟：禁用 Nagle 算法，减少小包延迟
            int flag = 1; setsockopt(g_tcp, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
            freeaddrinfo(result);
            return true;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    return false;
}

static int tcp_send(const char* data, int len)
{
    std::lock_guard<std::mutex> lock(g_net_mtx);
    if (g_tcp == INVALID_SOCKET) return -1;
    int sent = send(g_tcp, data, len, 0);
    return sent;
}

static std::string tcp_recv(int maxlen, int timeout_ms)
{
    std::string out;
    if (maxlen <= 0) return out;

    SOCKET s;
    {
        std::lock_guard<std::mutex> lock(g_net_mtx);
        s = g_tcp;
    }
    if (s == INVALID_SOCKET) return out;

    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    timeval tv{}; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rv = select(0, &rfds, nullptr, nullptr, &tv);
    if (rv <= 0 || !FD_ISSET(s, &rfds)) return out;

    int to_read = maxlen;
    if (to_read > 8192) to_read = 8192; // 防止一次性分配过大
    char buf[8192];
    int r = recv(s, buf, to_read, 0);
    if (r <= 0) return out;
    out.assign(buf, buf + r);
    return out;
}

static void tcp_close()
{
    std::lock_guard<std::mutex> lock(g_net_mtx);
    net_close_socket(g_tcp);
}

// （已移除 UDP 相关实现，仅保留 TCP 与 CEQP）

// ===================== 异步接收线程 =====================
static void tcp_rx_loop()
{
    while (g_tcp_rx_running)
    {
        SOCKET s;
        {
            std::lock_guard<std::mutex> lock(g_net_mtx);
            s = g_tcp;
        }
        if (s == INVALID_SOCKET) { Sleep(10); continue; }
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 10000; // 10ms 轮询
        int r = select(0, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(s, &rfds))
        {
            char buf[8192];
            int b = recv(s, buf, (int)sizeof(buf), 0);
            if (b > 0)
            {
                std::lock_guard<std::mutex> qlk(g_tcp_rx_q_mtx);
                g_tcp_rx_q.emplace_back(buf, buf + b);
                if (g_tcp_rx_q.size() > 1024) g_tcp_rx_q.pop_front();
            }
            else if (b == 0)
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);
                if (g_tcp == s) net_close_socket(g_tcp);
                Sleep(1);
            }
            else
            {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK){
                    std::lock_guard<std::mutex> lk(g_net_mtx);
                    if (g_tcp == s) net_close_socket(g_tcp);
                    Sleep(1);
                }
            }
        }
        else
        {
            Sleep(1);
        }
    }
}


// ----------------------- Lua 绑定 -----------------------
// ===================== CEQP 服务端（控制程序协议） =====================
#pragma pack(push, 1)
struct CEQP_FrameHeader {
    char     magic[4]; // "CEQP"
    uint8_t  version;  // 0x01
    uint8_t  type;     // 消息类型
    uint8_t  flags;    // 保留
    uint8_t  reserved; // 0
    uint32_t request_id;
    uint32_t payload_len;
};
#pragma pack(pop)

enum CEQP_MsgType : uint8_t {
    CEQP_HEARTBEAT_REQ  = 0x01,
    CEQP_HEARTBEAT_RESP = 0x02,

    CEQP_READ_MEM_ADDR  = 0x10,
    CEQP_WRITE_MEM_ADDR = 0x11,
    CEQP_READ_MOD_OFF   = 0x12,
    CEQP_WRITE_MOD_OFF  = 0x13,
    CEQP_READ_PTR_CHAIN = 0x14,
    CEQP_GET_MOD_BASE   = 0x20,

    CEQP_ERROR_RESP     = 0x7F,
};

enum CEQP_TlvType : uint16_t {
    CEQP_TLV_ADDR      = 0x0001, // u64
    CEQP_TLV_LEN       = 0x0002, // u32
    CEQP_TLV_MODNAME   = 0x0003, // utf-8 string
    CEQP_TLV_OFFSET    = 0x0004, // s64
    CEQP_TLV_OFFSETS   = 0x0005, // s64[]
    CEQP_TLV_DATA      = 0x0006, // bytes
    CEQP_TLV_DTYPE     = 0x0007, // string (可选)
    CEQP_TLV_ERRCODE   = 0x00FE, // u32
    CEQP_TLV_ERRMSG    = 0x00FF, // string
};

static SOCKET g_ceqp_listen = INVALID_SOCKET;
static SOCKET g_ceqp_client = INVALID_SOCKET;
static std::thread g_ceqp_thread;
static std::atomic<bool> g_ceqp_running(false);
static std::mutex g_ceqp_mtx;
static const int CEQP_IO_TIMEOUT_MS = 3000; // 每次收包超时，避免阻塞
static const uint32_t CEQP_MAX_PAYLOAD = 1024 * 1024; // 1MB 上限，防止内存爆炸

static inline void ceqp_put16(std::vector<uint8_t>& b, uint16_t v){ b.push_back((uint8_t)(v&0xFF)); b.push_back((uint8_t)((v>>8)&0xFF)); }
static inline void ceqp_put32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;i++) b.push_back((uint8_t)((v>>(i*8))&0xFF)); }
static inline void ceqp_put64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;i++) b.push_back((uint8_t)((v>>(i*8))&0xFF)); }
static inline bool ceqp_get32(const uint8_t* p, uint32_t& v){ v = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); return true; }
static inline bool ceqp_get64(const uint8_t* p, uint64_t& v){ v = 0; for(int i=0;i<8;i++) v |= ((uint64_t)p[i])<<(8*i); return true; }
static inline void ceqp_append(std::vector<uint8_t>& b, const void* p, size_t n){ const uint8_t* q=(const uint8_t*)p; b.insert(b.end(), q, q+n);} 

static void ceqp_encodeTLV(std::vector<uint8_t>& out, uint16_t t, const std::vector<uint8_t>& v){ ceqp_put16(out,t); ceqp_put16(out,(uint16_t)v.size()); ceqp_append(out,v.data(),v.size()); }
static void ceqp_encodeU32(std::vector<uint8_t>& out, uint16_t t, uint32_t x){ std::vector<uint8_t> v; ceqp_put32(v,x); ceqp_encodeTLV(out,t,v); }
static void ceqp_encodeU64(std::vector<uint8_t>& out, uint16_t t, uint64_t x){ std::vector<uint8_t> v; ceqp_put64(v,x); ceqp_encodeTLV(out,t,v); }
static void ceqp_encodeI64(std::vector<uint8_t>& out, uint16_t t, int64_t x){ std::vector<uint8_t> v; ceqp_put64(v,(uint64_t)x); ceqp_encodeTLV(out,t,v); }
static void ceqp_encodeStr(std::vector<uint8_t>& out, uint16_t t, const std::string& s){ std::vector<uint8_t> v((const uint8_t*)s.data(),(const uint8_t*)s.data()+s.size()); ceqp_encodeTLV(out,t,v); }
static void ceqp_encodeBytes(std::vector<uint8_t>& out, uint16_t t, const std::vector<uint8_t>& d){ ceqp_encodeTLV(out,t,d); }

static bool ceqp_extractBytes(const std::vector<uint8_t>& tlv, uint16_t t, std::vector<uint8_t>& out){ uint16_t T,L; size_t i=0; while(i+4<=tlv.size()){ T = tlv[i] | (tlv[i+1]<<8); L = tlv[i+2] | (tlv[i+3]<<8); i+=4; if(i+L>tlv.size()) return false; if(T==t){ out.assign(tlv.begin()+i, tlv.begin()+i+L); return true; } i+=L; } return false; }
static bool ceqp_extractU64(const std::vector<uint8_t>& tlv, uint16_t t, uint64_t& out){ uint16_t T,L; size_t i=0; while(i+4<=tlv.size()){ T = tlv[i] | (tlv[i+1]<<8); L = tlv[i+2] | (tlv[i+3]<<8); i+=4; if(i+L>tlv.size()) return false; if(T==t && L==8){ return ceqp_get64(&tlv[i],out); } i+=L; } return false; }
static bool ceqp_extractI64(const std::vector<uint8_t>& tlv, uint16_t t, int64_t& out){ uint64_t tmp; if(!ceqp_extractU64(tlv, t, tmp)) return false; out = (int64_t)tmp; return true; }
static bool ceqp_extractU32(const std::vector<uint8_t>& tlv, uint16_t t, uint32_t& out){ uint16_t T,L; size_t i=0; while(i+4<=tlv.size()){ T = tlv[i] | (tlv[i+1]<<8); L = tlv[i+2] | (tlv[i+3]<<8); i+=4; if(i+L>tlv.size()) return false; if(T==t && L==4){ return ceqp_get32(&tlv[i],out); } i+=L; } return false; }
static bool ceqp_extractStr(const std::vector<uint8_t>& tlv, uint16_t t, std::string& out){ uint16_t T,L; size_t i=0; while(i+4<=tlv.size()){ T = tlv[i] | (tlv[i+1]<<8); L = tlv[i+2] | (tlv[i+3]<<8); i+=4; if(i+L>tlv.size()) return false; if(T==t){ out.assign((const char*)&tlv[i], L); return true; } i+=L; } return false; }

static bool ceqp_send_all(SOCKET s, const uint8_t* buf, size_t n){ size_t sent=0; while(sent<n){ int r = send(s, (const char*)buf+sent, (int)(n-sent), 0); if(r<=0) return false; sent+=r; } return true; }
static bool ceqp_recv_exact(SOCKET s, uint8_t* buf, size_t n){ size_t got=0; while(got<n){ int r = recv(s, (char*)buf+got, (int)(n-got), 0); if(r<=0) return false; got+=r; } return true; }
static bool ceqp_recv_exact_timeout(SOCKET s, uint8_t* buf, size_t n, int timeout_ms){
    size_t got = 0;
    while (got < n){
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        timeval tv{}; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        int rv = select(0, &rfds, nullptr, nullptr, &tv);
        if (rv <= 0 || !FD_ISSET(s, &rfds)) return false;
        int r = recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static void ceqp_send_error(SOCKET s, uint32_t reqid, uint32_t code, const std::string& msg){ std::vector<uint8_t> pl; ceqp_encodeU32(pl, CEQP_TLV_ERRCODE, code); ceqp_encodeStr(pl, CEQP_TLV_ERRMSG, msg); CEQP_FrameHeader h{}; h.magic[0]='C'; h.magic[1]='E'; h.magic[2]='Q'; h.magic[3]='P'; h.version=0x01; h.type=CEQP_ERROR_RESP; h.flags=0; h.reserved=0; h.request_id=reqid; h.payload_len=(uint32_t)pl.size(); std::vector<uint8_t> buf; ceqp_append(buf,&h,sizeof(h)); ceqp_append(buf,pl.data(),pl.size()); ceqp_send_all(s, buf.data(), buf.size()); }
static void ceqp_send_ok(SOCKET s, uint8_t type, uint32_t reqid, const std::vector<uint8_t>& pl){ CEQP_FrameHeader h{}; h.magic[0]='C'; h.magic[1]='E'; h.magic[2]='Q'; h.magic[3]='P'; h.version=0x01; h.type=type; h.flags=0; h.reserved=0; h.request_id=reqid; h.payload_len=(uint32_t)pl.size(); std::vector<uint8_t> buf; ceqp_append(buf,&h,sizeof(h)); if(!pl.empty()) ceqp_append(buf,pl.data(),pl.size()); ceqp_send_all(s, buf.data(), buf.size()); }

static bool ceqp_find_module_base(const std::string& name, uint64_t& base){ DWORD pid = *Exported.OpenedProcessID; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, pid); if (snap==INVALID_HANDLE_VALUE) return false; std::string lowName = name; for(char& c: lowName) c = (char)tolower((unsigned char)c);
    bool ok=false;
    MODULEENTRY32 me{}; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            std::string mod = tchar_to_utf8(me.szModule); for(char& c: mod) c=(char)tolower((unsigned char)c);
            if (mod==lowName){ base = (uint64_t)(uintptr_t)me.modBaseAddr; ok=true; break; }
        } while(Module32Next(snap, &me));
    }
    CloseHandle(snap); return ok;
}

static bool ceqp_read_mem(uint64_t addr, uint32_t len, std::vector<uint8_t>& out){ out.resize(len); SIZE_T r=0; BOOL ok = (*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)addr, out.data(), len, &r); if (!ok && r==0){ out.clear(); return false; } if (r < len) out.resize((size_t)r); return true; }
static bool ceqp_write_mem(uint64_t addr, const std::vector<uint8_t>& data){ SIZE_T w=0; BOOL ok = ::WriteProcessMemory(*Exported.OpenedProcessHandle, (LPVOID)addr, data.data(), (SIZE_T)data.size(), &w); return ok && w==data.size(); }

static bool ceqp_handle(SOCKET s, uint8_t type, uint32_t reqid, const std::vector<uint8_t>& pl){
    if (type==CEQP_HEARTBEAT_REQ){ std::vector<uint8_t> ep; ceqp_send_ok(s, CEQP_HEARTBEAT_RESP, reqid, ep); return true; }
    if (type==CEQP_GET_MOD_BASE){ std::string mod; if(!ceqp_extractStr(pl, CEQP_TLV_MODNAME, mod)){ ceqp_send_error(s, reqid, 1, "modname missing"); return false; } uint64_t base=0; if(!ceqp_find_module_base(mod, base)){ ceqp_send_error(s, reqid, 2, "module not found"); return false; } std::vector<uint8_t> ep; ceqp_encodeU64(ep, CEQP_TLV_ADDR, base); ceqp_send_ok(s, type, reqid, ep); return true; }
    if (type==CEQP_READ_MEM_ADDR){ uint64_t addr=0; uint32_t len=0; if(!ceqp_extractU64(pl, CEQP_TLV_ADDR, addr) || !ceqp_extractU32(pl, CEQP_TLV_LEN, len)){ ceqp_send_error(s, reqid, 3, "addr/len missing"); return false; } std::vector<uint8_t> data; if(!ceqp_read_mem(addr, len, data)){ ceqp_send_error(s, reqid, 4, "read failed"); return false; } std::vector<uint8_t> ep; ceqp_encodeBytes(ep, CEQP_TLV_DATA, data); ceqp_send_ok(s, type, reqid, ep); return true; }
    if (type==CEQP_WRITE_MEM_ADDR){ uint64_t addr=0; std::vector<uint8_t> data; if(!ceqp_extractU64(pl, CEQP_TLV_ADDR, addr) || !ceqp_extractBytes(pl, CEQP_TLV_DATA, data)){ ceqp_send_error(s, reqid, 5, "addr/data missing"); return false; } if(!ceqp_write_mem(addr, data)){ ceqp_send_error(s, reqid, 6, "write failed"); return false; } std::vector<uint8_t> ep; ceqp_send_ok(s, type, reqid, ep); return true; }
    if (type==CEQP_READ_MOD_OFF){ std::string mod; int64_t off=0; uint32_t len=0; if(!ceqp_extractStr(pl, CEQP_TLV_MODNAME, mod) || !ceqp_extractI64(pl, CEQP_TLV_OFFSET, off) || !ceqp_extractU32(pl, CEQP_TLV_LEN, len)){ ceqp_send_error(s, reqid, 7, "mod/offset/len missing"); return false; } uint64_t base=0; if(!ceqp_find_module_base(mod, base)){ ceqp_send_error(s, reqid, 8, "module not found"); return false; } std::vector<uint8_t> data; if(!ceqp_read_mem(base + (uint64_t)off, len, data)){ ceqp_send_error(s, reqid, 9, "read failed"); return false; } std::vector<uint8_t> ep; ceqp_encodeBytes(ep, CEQP_TLV_DATA, data); ceqp_send_ok(s, type, reqid, ep); return true; }
    if (type==CEQP_WRITE_MOD_OFF){ std::string mod; int64_t off=0; std::vector<uint8_t> data; if(!ceqp_extractStr(pl, CEQP_TLV_MODNAME, mod) || !ceqp_extractI64(pl, CEQP_TLV_OFFSET, off) || !ceqp_extractBytes(pl, CEQP_TLV_DATA, data)){ ceqp_send_error(s, reqid, 10, "mod/offset/data missing"); return false; } uint64_t base=0; if(!ceqp_find_module_base(mod, base)){ ceqp_send_error(s, reqid, 11, "module not found"); return false; } if(!ceqp_write_mem(base + (uint64_t)off, data)){ ceqp_send_error(s, reqid, 12, "write failed"); return false; } std::vector<uint8_t> ep; ceqp_send_ok(s, type, reqid, ep); return true; }
    if (type==CEQP_READ_PTR_CHAIN){ uint64_t addr=0; std::vector<uint8_t> offsBytes; if(!ceqp_extractU64(pl, CEQP_TLV_ADDR, addr) || !ceqp_extractBytes(pl, CEQP_TLV_OFFSETS, offsBytes)){ ceqp_send_error(s, reqid, 13, "addr/offsets missing"); return false; } // offsets 为 s64 列表
        std::vector<int64_t> offsets; for(size_t i=0;i+8<=offsBytes.size();i+=8){ uint64_t t=0; ceqp_get64(&offsBytes[i], t); offsets.push_back((int64_t)t); }
        // 根据目标进程位数选择指针宽度（WOW64 -> 32 位），允许 DTYPE 覆盖
        BOOL isWow64 = FALSE; IsWow64Process(*Exported.OpenedProcessHandle, &isWow64);
        size_t ptrSize = isWow64 ? 4 : sizeof(UINT_PTR);
        std::string dtype; if (ceqp_extractStr(pl, CEQP_TLV_DTYPE, dtype)){
            for(char& c: dtype) c=(char)tolower((unsigned char)c);
            if (dtype=="u32ptr" || dtype=="ptr32" || dtype=="u32") ptrSize = 4;
            else if (dtype=="u64ptr" || dtype=="ptr64" || dtype=="u64") ptrSize = 8;
        }
        debug_logf("[CEQP] READ_PTR_CHAIN addr=0x%llX, offs=%zu, ptrSize=%zu\n", (unsigned long long)addr, offsets.size(), ptrSize);
        UINT_PTR cur = (UINT_PTR)addr; SIZE_T r=0;
        for(size_t k=0;k<offsets.size();++k){
            UINT_PTR prev = cur;
            if (ptrSize == 4) {
                uint32_t val32 = 0; BOOL ok = (*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)cur, &val32, (SIZE_T)4, &r);
                if(!ok || r!=4){ debug_logf("[CEQP] step %zu: read32 [0x%llX] failed (r=%llu)\n", k, (unsigned long long)prev, (unsigned long long)r); ceqp_send_error(s, reqid, 14, "ptr read failed"); return false; }
                cur = (UINT_PTR)val32 + (UINT_PTR)offsets[k];
                debug_logf("[CEQP] step %zu: [0x%llX]=>0x%08X , +off 0x%llX => 0x%llX\n", k, (unsigned long long)prev, (unsigned int)val32, (unsigned long long)offsets[k], (unsigned long long)cur);
            } else {
                uint64_t val64 = 0; BOOL ok = (*Exported.ReadProcessMemory)(*Exported.OpenedProcessHandle, (LPCVOID)cur, &val64, (SIZE_T)8, &r);
                if(!ok || r!=8){ debug_logf("[CEQP] step %zu: read64 [0x%llX] failed (r=%llu)\n", k, (unsigned long long)prev, (unsigned long long)r); ceqp_send_error(s, reqid, 14, "ptr read failed"); return false; }
                cur = (UINT_PTR)val64 + (UINT_PTR)offsets[k];
                debug_logf("[CEQP] step %zu: [0x%llX]=>0x%016llX , +off 0x%llX => 0x%llX\n", k, (unsigned long long)prev, (unsigned long long)val64, (unsigned long long)offsets[k], (unsigned long long)cur);
            }
        }
        // 最终地址读取 len（未指定则默认按指针宽度）
        uint32_t len=0; if(!ceqp_extractU32(pl, CEQP_TLV_LEN, len)) len=(uint32_t)ptrSize;
        debug_logf("[CEQP] final addr=0x%llX, len=%u\n", (unsigned long long)cur, len);
        std::vector<uint8_t> data; if(!ceqp_read_mem((uint64_t)cur, len, data)){ char buf[64]; sprintf_s(buf, "final read failed at 0x%llX", (unsigned long long)cur); debug_logf("[CEQP] ERROR 15: %s\n", buf); ceqp_send_error(s, reqid, 15, std::string(buf)); return false; }
        std::vector<uint8_t> ep; ceqp_encodeBytes(ep, CEQP_TLV_DATA, data); ceqp_encodeU64(ep, CEQP_TLV_ADDR, (uint64_t)cur); if(g_test_env){ std::string dtypeOut = (ptrSize==4)?"u32ptr":"u64ptr"; ceqp_encodeStr(ep, CEQP_TLV_DTYPE, dtypeOut); ceqp_encodeU32(ep, CEQP_TLV_LEN, len);} ceqp_send_ok(s, type, reqid, ep); return true; }
    // 未知类型
    ceqp_send_error(s, reqid, 100, "unknown type"); return false;
}

static void ceqp_server_loop(){
    while (g_ceqp_running){
        SOCKET c = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lk(g_ceqp_mtx);
            SOCKET l = g_ceqp_listen;
            if (l==INVALID_SOCKET){ Sleep(50); continue; }
            fd_set rfds; FD_ZERO(&rfds); FD_SET(l, &rfds);
            timeval tv{}; tv.tv_sec=0; tv.tv_usec=50000; // 50ms
            int rv = select(0, &rfds, nullptr, nullptr, &tv);
            if (rv>0 && FD_ISSET(l, &rfds)){
                sockaddr_in cli{}; int clen=sizeof(cli); c = accept(l, (sockaddr*)&cli, &clen);
            }
        }
        if (c==INVALID_SOCKET){ continue; }
        // 禁用 Nagle
        int flag=1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
        {
            std::lock_guard<std::mutex> lk(g_ceqp_mtx);
            net_close_socket(g_ceqp_client);
            g_ceqp_client = c;
        }
        // 会话循环
        while (g_ceqp_running){
            CEQP_FrameHeader h{};
            if (!ceqp_recv_exact_timeout(c, (uint8_t*)&h, sizeof(h), CEQP_IO_TIMEOUT_MS)) break;
            if (!(h.magic[0]=='C' && h.magic[1]=='E' && h.magic[2]=='Q' && h.magic[3]=='P')) break;
            if (h.version != 0x01) { ceqp_send_error(c, 0, 101, "bad version"); break; }
            if (h.payload_len > CEQP_MAX_PAYLOAD) { ceqp_send_error(c, h.request_id, 102, "payload too large"); break; }
            std::vector<uint8_t> pl; pl.resize(h.payload_len);
            if (h.payload_len>0 && !ceqp_recv_exact_timeout(c, pl.data(), pl.size(), CEQP_IO_TIMEOUT_MS)) break;
            ceqp_handle(c, h.type, h.request_id, pl);
        }
        {
            std::lock_guard<std::mutex> lk(g_ceqp_mtx);
            net_close_socket(g_ceqp_client);
        }
    }
}

static bool ceqp_start(int port){ if(!net_init()) return false; std::lock_guard<std::mutex> lk(g_ceqp_mtx); if (g_ceqp_running) return true; net_close_socket(g_ceqp_listen);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (l==INVALID_SOCKET) return false;
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY); addr.sin_port=htons(port);
    int reuse=1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (bind(l, (sockaddr*)&addr, sizeof(addr))!=0){ closesocket(l); return false; }
    if (listen(l, 4)!=0){ closesocket(l); return false; }
    g_ceqp_listen = l; g_ceqp_running = true;
    try { g_ceqp_thread = std::thread(ceqp_server_loop); } catch (...) { g_ceqp_running=false; net_close_socket(g_ceqp_listen); return false; }
    return true;
}

static void ceqp_stop(){ bool needJoin=false; { std::lock_guard<std::mutex> lk(g_ceqp_mtx); needJoin = g_ceqp_running; g_ceqp_running=false; net_close_socket(g_ceqp_listen); net_close_socket(g_ceqp_client); }
    if (needJoin && g_ceqp_thread.joinable()) g_ceqp_thread.join(); }

extern "C" {

// CEQP Lua 接口：启动/停止服务端
static int lua_CEQPStart(lua_State* L)
{
    int port = (int)luaL_optinteger(L, 1, 9178);
    bool ok = ceqp_start(port);
    lua_pushboolean(L, ok ? TRUE : FALSE);
    return 1;
}

static int lua_CEQPStop(lua_State* L)
{
    ceqp_stop();
    lua_pushboolean(L, TRUE);
    return 1;
}

static int lua_TCPConnect(lua_State* L)
{
    const char* host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    bool ok = tcp_connect(host, port);
    lua_pushboolean(L, ok);
    return 1;
}

static int lua_TCPSend(lua_State* L)
{
    size_t len = 0; const char* data = luaL_checklstring(L, 1, &len);
    int sent = tcp_send(data, (int)len);
    lua_pushinteger(L, sent);
    return 1;
}

static int lua_TCPRecv(lua_State* L)
{
    int maxlen = (int)luaL_optinteger(L, 1, 4096);
    int timeout_ms = (int)luaL_optinteger(L, 2, 1000);
    std::string r = tcp_recv(maxlen, timeout_ms);
    lua_pushlstring(L, r.data(), (size_t)r.size());
    return 1;
}

static int lua_TCPClose(lua_State* L)
{
    tcp_close();
    return 0;
}

// 异步/非阻塞 TCP Lua API
static int lua_TCPStartRecv(lua_State* L)
{
    if (g_tcp_rx_running) { lua_pushboolean(L, TRUE); return 1; }
    g_tcp_rx_running = true;
    try { g_tcp_rx_thread = std::thread(tcp_rx_loop); }
    catch (...) { g_tcp_rx_running = false; lua_pushboolean(L, FALSE); return 1; }
    lua_pushboolean(L, TRUE); return 1;
}

static int lua_TCPStopRecv(lua_State* L)
{
    if (g_tcp_rx_running) { g_tcp_rx_running = false; if (g_tcp_rx_thread.joinable()) g_tcp_rx_thread.join(); }
    lua_pushboolean(L, TRUE); return 1;
}

static int lua_TCPRecvNonblocking(lua_State* L)
{
    int maxlen = (int)luaL_optinteger(L, 1, 4096);
    std::string out;
    {
        std::lock_guard<std::mutex> qlk(g_tcp_rx_q_mtx);
        if (!g_tcp_rx_q.empty()) { out = std::move(g_tcp_rx_q.front()); g_tcp_rx_q.pop_front(); }
    }
    if ((int)out.size() > maxlen) out.resize(maxlen);
    lua_pushlstring(L, out.data(), (size_t)out.size());
    return 1;
}

static int lua_TCPSetNoDelay(lua_State* L)
{
    int enable = lua_toboolean(L, 1);
    std::lock_guard<std::mutex> lock(g_net_mtx);
    if (g_tcp == INVALID_SOCKET) { lua_pushboolean(L, FALSE); return 1; }
    int flag = enable ? 1 : 0;
    setsockopt(g_tcp, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
    lua_pushboolean(L, TRUE); return 1;
}






}

// ----------------------- CE 插件入口 -----------------------
BOOL __stdcall CEPlugin_GetVersion(PPluginVersion pv , int sizeofpluginversion)
{
    pv->version = CESDK_VERSION;
    pv->pluginname = (char*)"TCP network read and write plugin (Lua API)-TCP网络读写插件=作者作者：Lun.  QQ:1596534228 github:Lun-OS";
    return TRUE;
}

BOOL __stdcall CEPlugin_InitializePlugin(PExportedFunctions ef , int pluginid)
{
    g_plugin_id = pluginid;
    Exported = *ef;
    if (Exported.sizeofExportedFunctions != sizeof(Exported))
        return FALSE;

    // 读取环境变量以启用测试环境
    char ev[32] = {0}; DWORD n = GetEnvironmentVariableA("CEQP_TEST_ENV", ev, sizeof(ev)-1);
    if (n>0){ std::string v(ev); for(char& c: v) c=(char)tolower((unsigned char)c);
        g_test_env = (v=="1" || v=="true" || v=="yes" || v=="on");
    }

    net_init();

    lua_State* L = ef->GetLuaState();
    lua_register(L, "pluginTCPConnect", lua_TCPConnect);
    lua_register(L, "pluginTCPSend", lua_TCPSend);
    lua_register(L, "pluginTCPRecv", lua_TCPRecv);
    lua_register(L, "pluginTCPClose", lua_TCPClose);

    // 新增异步/非阻塞 TCP API
    lua_register(L, "pluginTCPStartRecv", lua_TCPStartRecv);
    lua_register(L, "pluginTCPStopRecv", lua_TCPStopRecv);
    lua_register(L, "pluginTCPRecvNonblocking", lua_TCPRecvNonblocking);
    lua_register(L, "pluginTCPSetNoDelay", lua_TCPSetNoDelay);

    // CEQP 服务端启停与测试环境切换
    lua_register(L, "QAQ", lua_CEQPStart);
    lua_register(L, "stopQAQ", lua_CEQPStop);
    lua_register(L, "pluginCEQPSetTestEnv", lua_CEQPSetTestEnv);

    debug_logf("[CEQP] Initialize: testEnv=%s\n", g_test_env?"true":"false");
    return TRUE;
}

BOOL __stdcall CEPlugin_DisablePlugin(void)
{
    // 停止异步线程
    if (g_tcp_rx_running) { g_tcp_rx_running = false; if (g_tcp_rx_thread.joinable()) g_tcp_rx_thread.join(); }

    ceqp_stop();
    tcp_close();
    if (g_wsa_inited)
    {
        WSACleanup();
        g_wsa_inited = false;
    }
    return TRUE;
}

static std::string tchar_to_utf8(const TCHAR* p){
#ifdef UNICODE
    if (!p) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s; s.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, p, -1, &s[0], len, nullptr, nullptr);
    return s;
#else
    return std::string(p ? p : "");
#endif
}
// 作者：Lun.  QQ:1596534228   github:Lun-OS