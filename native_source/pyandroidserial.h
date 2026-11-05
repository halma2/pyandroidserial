#ifndef PYANDROIDSERIAL_H
#define PYANDROIDSERIAL_H

#pragma once

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#include <cstddef>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <condition_variable>

using namespace std;

extern "C"
{
    EXPORT bool usb_host_supported();
    EXPORT const char* usb_get_all_device_names();
    EXPORT bool usb_valid_device(const char* name);
    EXPORT bool usb_has_permission(const char* name);
    EXPORT bool usb_request_permission(const char* name);
    EXPORT bool usb_open_device(const char* name);
    EXPORT int usb_available();
    EXPORT int usb_write(const char* data, int len, int timeoutMs);
    EXPORT int usb_read(char* buffer, int maxlen, int timeoutMs);
    EXPORT void usb_clear_rx_buffer();
    EXPORT void usb_close();
}

// global variables that pyandroidserial_bt.cpp uses:
constexpr size_t MAX_RX_BUFFER = 1024 * 1024;
bool clearJniException();
int rxRead(char* buffer, int maxLen, int timeoutMs);
int rxBufferAvailable();
void clearRxBuffer();
inline mutex g_rxMutex;
inline thread g_rxThread;
inline atomic<bool> g_rxRunning{false};
inline deque<char> g_rxBuffer;
inline condition_variable g_rxCv;
inline bool g_rxError = false;

#endif // PYANDROIDSERIAL_H
