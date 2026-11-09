import sys

if hasattr(sys, "getandroidapilevel"):
    import time
    import android_serial as android_ser
    from can.interfaces import slcan
    import can
    slcan.serial.serial_for_url = android_ser.android_serial_for_url # monkey-patch

    available_ports: list[str] = android_ser.android_search_for_usb_bt_devices()

    if not available_ports:
        exit()

    selected_port: str = available_ports[0]  # or other ports listed here, let's assume it's a Bluetooth device
    # the port name of a bluetooth device gets a "bt://" prefix before its mac address
    # this way the library will know which serial (Usb/Bt) class should handle the port address.
    # check whether the Bluetooth device class of the selected port is 0x1F00 (uncategorized, used for UART):
    if android_ser.android_is_bluetooth_device(selected_port):
        if android_ser.android_bluetooth_get_device_class(selected_port) != int(0x1F00):
            exit(1)

    # if the android version is 31 or greater, then BLUETOOTH_CONNECT permission is needed from the user
    if not android_ser.android_bluetooth_permission_granted():
        android_ser.android_bluetooth_request_permission()
        time.sleep(5)  # or any interval the user can react

    if android_ser.android_bluetooth_permission_granted():
        with can.Bus(interface="slcan", channel=selected_port, bitrate=250_000, sleep_after_open=0) as bus:
            for _ in range(10):
                msg = can.Message(arbitration_id=0x201, data=[0x40, 0, 0, 0x20, 0, 0, 0, 0],
                                  is_extended_id=False)  # random message
                try:
                    bus.send(msg)
                    print("CAN message sent:", msg)
                except can.CanError as e:
                    print("Error with sending CAN message:", e)

                rx = bus.recv()
                if rx:
                    print("CAN rx:", rx)
                time.sleep(1)
