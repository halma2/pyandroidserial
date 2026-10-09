#ifndef PYANDROIDSERIAL_H
#define PYANDROIDSERIAL_H
#pragma once

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

///build: Qt_6_11_1_for_Android_arm64_v8a_Debug

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
    EXPORT const char* usb_last_error();
}

#endif // PYANDROIDSERIAL_H
