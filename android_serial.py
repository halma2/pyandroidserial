import json
from contextlib import contextmanager
from ctypes import *
from io import UnsupportedOperation

clib = CDLL("libpyandroidserial.so")
clib.usb_host_supported.restype = c_bool
clib.usb_get_all_device_names.restype = c_char_p
clib.usb_has_permission.argtypes = [c_char_p]
clib.usb_has_permission.restype = c_bool
clib.usb_request_permission.argtypes = [c_char_p]
clib.usb_request_permission.restype = c_bool
clib.usb_open_device.argtypes = [c_char_p]
clib.usb_open_device.restype = c_bool
clib.usb_available.restype = c_int
clib.usb_write.argtypes = [c_char_p, c_int, c_int]
clib.usb_write.restype = c_int
clib.usb_read.argtypes = [POINTER(c_char), c_int, c_int]
clib.usb_read.restype = c_int
clib.bt_get_paired_devices.restype = c_char_p
clib.bt_get_device_class.argtypes = [POINTER(c_char)]
clib.bt_get_device_class.restype = c_int
clib.bt_connect.argtypes = [c_char_p]
clib.bt_connect.restype = c_bool
clib.bt_available.restype = c_int
clib.bt_has_connect_permission.restype = c_bool
clib.bt_read.argtypes = [POINTER(c_char), c_int, c_int]
clib.bt_read.restype = c_int
clib.bt_write.argtypes = [c_char_p, c_int, c_int]
clib.bt_write.restype = c_int


def android_serial_for_url(url, *args, **kwargs):
    if str(url).startswith("bt://"):
        return AndroidBluetoothSerial(url, *args, **kwargs)
    return AndroidSerial(*args, **kwargs)

def usb_host_supported_on_android() -> bool:
    """It checks, whether the android device supports USB host"""
    return clib.usb_host_supported()

def android_search_for_usb_bt_devices() -> list[str]:
    """List all connected devices (device name)"""
    usb_devices = json.loads(clib.usb_get_all_device_names().decode("utf-8"))
    bt_devices = json.loads(clib.bt_get_paired_devices().decode("utf-8"))
    return usb_devices + bt_devices

def android_usb_permission_granted(name: str) -> bool:
    return clib.usb_has_permission(name.encode("utf-8")) == 1

def android_usb_request_permission(name: str) -> bool:
    return clib.usb_request_permission(name.encode("utf-8")) == 1

class AndroidSerial:
    """The pyserial serial class overwrites those properties,
    which are used by slcan.
    It accesses the serial devices and communication by calling functions
    of a pre-compiled c++ library.
    """
    def __init__(
        self,
        port,
        baudrate=115200,
        timeout=None,
        write_timeout=None,
        rtscts=False,
        **kwargs,
    ):
        self.port = str(port)
        self.baudrate = baudrate
        self.rtscts = rtscts
        self.timeout = timeout
        self.write_timeout = write_timeout
        self.is_open = False
        self.handle = clib.usb_open_device(port.encode("utf-8"))

        if not android_usb_permission_granted(port):
            raise PermissionError("Android USB permission has not been granted")

    @property
    def in_waiting(self):
        """It queries the number of readable bytes in the buffer of the c++ library."""
        return clib.usb_available()

    def read(self, size=1):
        if not self.is_open or size <= 0:
            return b""
        timeout_ms = (
            -1
            if self.timeout is None
            else max(1, int(self.timeout * 1000)))
        buffer = create_string_buffer(size)
        result = clib.usb_read(buffer, size, timeout_ms)
        if result < 0:
            raise OSError(f"USB read failed {result}")
        return buffer.raw[:result]

    def write(self, data):
        if not self.is_open:
            raise OSError("Android USB device is closed")
        if not isinstance(data, (bytes, bytearray)):
            raise TypeError("write() expects bytes or bytearray")
        timeout_ms = (
            int(self.write_timeout * 100)
            if self.write_timeout is not None
            else 1000
        )
        return clib.usb_write(data, len(data), timeout_ms)

    def flush(self):
        pass # synchronised bulk-transfer

    def reset_input_buffer(self):
        clib.usb_clear_rx_buffer()

    def reset_output_buffer(self):
        # The OutputStream does not provide general clear.
        # It should not be assumed that the already sent data can be deleted.
        pass

    def close(self):
        if self.is_open:
            self.is_open = False
            clib.usb_close()

    def fileno(self):
         raise UnsupportedOperation("Android USB device has no POSIX file descriptor")


def is_bluetooth_device(name) -> bool:
    return name.startswith("bt://")

def android_bluetooth_permission_granted():
    return clib.bt_has_connect_permission()

def android_bluetooth_request_permission():
    clib.bt_request_connect_permission()

def get_bt_mac_address(port: str) -> str | None:
    return port[5:] if is_bluetooth_device(port) else None

def android_bluetooth_get_device_class(port: str) -> int:
    return clib.bt_get_device_class(get_bt_mac_address(port).encode('ascii'))


class AndroidBluetoothSerial:
    def __init__(
        self,
        port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
        **kwargs,
    ):
        self.port = str(port)
        self.baudrate = baudrate # does not affect the Bluetooth transfer speed
        self.timeout = timeout
        self.write_timeout = write_timeout
        self.is_open = False

        if not clib.bt_has_connect_permission():
            raise PermissionError("Android Bluetooth permission has not been granted")

        mac = get_bt_mac_address(self.port)
        if not mac:
            raise ValueError(f"Invalid Bluetooth port: {self.port}")
            
        if not clib.bt_connect(mac.encode("ascii")):
            raise OSError(f"Bluetooth SPP connection error: {mac}")

        if not clib.bt_has_connect_permission():
            raise PermissionError("Android Bluetooth permission has not been granted")


        self.is_open = True

    @property
    def in_waiting(self):
        return max(0, clib.bt_available())

    def read(self, size=1):
        if not self.is_open:
            raise OSError("Bluetooth is not open.")

        if size <= 0:
            return b""

        timeout_ms = (
            -1 if self.timeout is None
            else max(0, int(self.timeout * 1000))
        )
        buffer = create_string_buffer(size)
        count = clib.bt_read(buffer, size, timeout_ms)

        if count < 0:
            raise OSError("Bluetooth read error.")

        return buffer.raw[:count]

    def write(self, data):
        if not self.is_open:
            raise OSError("Bluetooth is not open.")

        data = bytes(data)
        if not data:
            return 0

        timeout_ms = (
            -1 if self.write_timeout is None
            else max(0, int(self.write_timeout * 1000))
        )
        # The C++ function gets explicit length parameter,
        # so the bytestring is not handled as null-terminated string.
        count = clib.bt_write(data, len(data), timeout_ms)

        if count < 0:
            raise OSError("Bluetooth write error.")

        return count

    def flush(self):
        pass

    def reset_input_buffer(self):
        clib.bt_clear_rx_buffer()

    def reset_output_buffer(self):
        pass

    def read_until(self, expected=b"\n", size=None):
        if isinstance(expected, str):
            expected = expected.encode()
        result = bytearray()

        while size is None or len(result) < size:
            part = self.read(1)
            if not part:
                break
            result.extend(part)
            if result.endswith(expected):
                break
        return bytes(result)

    def readline(self, size=None):
        return self.read_until(b"\n", size=size)

    def close(self):
        if self.is_open:
            clib.bt_close()
            self.is_open = False

    def fileno(self):
        raise UnsupportedOperation("Android Bluetooth device has no POSIX file descriptor.")


def open_serial(
    port,
    baudrate=115200,
    timeout=1.0,
    write_timeout=1.0,
    **kwargs,
) -> AndroidSerial | AndroidBluetoothSerial:
    """Used for opening UART communication."""
    port = str(port)
    if port.startswith("bt:"):
        return AndroidBluetoothSerial(
            port=port,
            baudrate=baudrate,
            timeout=timeout,
            write_timeout=write_timeout,
            **kwargs,
        )
    return AndroidSerial(
        channel=port,
        baudrate=baudrate,
        timeout=timeout,
        **kwargs,
    )

@contextmanager
def android_serial(port): # use: 'with android_serial(args) as serial:'
    android_ser = open_serial(port)
    try:
        yield android_ser
    finally:
        android_ser.close()
