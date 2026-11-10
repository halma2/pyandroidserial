import sys
import time
import android_serial as android_ser


selected_port: str = "/dev/bus/usb/001/002"

# or use any port from this list:
#available_ports: list[str] = android_ser.android_search_for_usb_devices()

def write_uart(port: str):
    if not hasattr(sys, "getandroidapilevel"):
        return
    # start reading:
    # before that, check, whether the device has permission from android
    # if it has not, ask the user for permission and then try again
    if android_ser.android_is_bluetooth_device(port):
        if not android_ser.android_bluetooth_permission_granted():
            android_ser.android_bluetooth_request_permission()
            time.sleep(5)
    else:
        if not android_ser.android_usb_permission_granted(port):
            android_ser.android_usb_request_permission(port)
            time.sleep(5)  # or any interval the user can react

    #bus = android_ser.open_serial(port=port, baudrate=115200) # or use "with":
    with android_ser.android_serial(port) as bus:
        for _ in range(10):
            bus.write(b"\x01")
            time.sleep(0.1)
        bus.write(b"V\r")
        for _ in range(10):
            rx_bytes = bus.read()
            print("arrived", rx_bytes if rx_bytes else "no data")
    #bus.close()

write_uart(selected_port)
