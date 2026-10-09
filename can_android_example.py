import sys

# only works on andorid:
if hasattr(sys, "getandroidapilevel"):
  import time
  import canopen_app.android_serial as android_usb
  from can.interfaces import slcan
  import can

  
  slcan.serial.serial_for_url = android_usb.android_serial_for_url # monkey-patch

  available_ports: list[str] = android_search_for_usb_devices()

  if available_ports == []:
    return 0
    
  selected_port: str = available_ports[0] # or other ports listed here
  
  # start reading:
  # before that, check, whether the device has permission from android
  # if it has not, ask the user for permission and then try again
  if android_usb_permission_granted(selected_port):
    android_usb_request_permission(selected_port)
    time.sleep(5) # or any interval the user can react

  if android_usb_permission_granted(selected_port):
    with can.Bus(interface="slcan", channel=selected_port, bitrate=250_000) as bus:
      for _ in range(10):
        msg = can.Message(arbitration_id=0x201, data=[0x40, 0, 0, 0x20, 0, 0, 0, 0], is_extended_id=False) # random message
        try:
            bus.send(msg)
            print("CAN message sent:", msg)
        except can.CanError as e:
            print("Error with sending CAN message:", e)

        rx =  bus.recv()
        if rx:
             print("CAN rx:", rx)
        time.sleep(1)
