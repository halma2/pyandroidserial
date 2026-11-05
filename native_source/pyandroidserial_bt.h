#ifndef PYANDROIDSERIAL_BT_H
#define PYANDROIDSERIAL_BT_H
#pragma once

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

extern "C"
{
    EXPORT const char* bt_get_paired_devices();
    EXPORT int bt_get_device_class(const char* mac_address);
    EXPORT bool bt_connect(const char* mac_address);
    EXPORT bool bt_has_connect_permission();
    EXPORT void bt_request_connect_permission();
    EXPORT int bt_available();
    EXPORT int bt_read(char* buffer, int maxlen, int timeoutMs);
    EXPORT int bt_write(const char* data, int len, int timeoutMs);
    EXPORT void bt_clear_rx_buffer();
    EXPORT void bt_close();
}

#endif // PYANDROIDSERIAL_BT_H
