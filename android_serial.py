import json
from abc import ABC
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
clib.rx_buffer_available.restype = c_int
clib.usb_write.argtypes = [c_char_p, c_int, c_int]
clib.usb_write.restype = c_int
clib.rx_buffer_read.argtypes = [POINTER(c_char), c_int, c_int]
clib.rx_buffer_read.restype = c_int
clib.bt_get_paired_devices.restype = c_char_p
clib.bt_get_device_class.argtypes = [POINTER(c_char)]
clib.bt_get_device_class.restype = c_int
clib.bt_connect.argtypes = [c_char_p]
clib.bt_connect.restype = c_bool
clib.bt_has_connect_permission.restype = c_bool
clib.bt_write.argtypes = [c_char_p, c_int, c_int]
clib.bt_write.restype = c_int

def android_serial_for_url(url, *args, **kwargs):
    if str(url).startswith("bt://"):
        return AndroidBluetoothSerial(url, *args, **kwargs)
    return AndroidUsbSerial(*args, **kwargs)

def android_usb_host_supported() -> bool:
    """It checks, whether the android device supports USB host"""
    return clib.usb_host_supported()

def android_search_for_usb_bt_devices() -> list[str]:
    """List all connected devices (device name)"""
    usb_devices = json.loads(clib.usb_get_all_device_names().decode())
    bt_devices = json.loads(clib.bt_get_paired_devices().decode())
    return usb_devices + bt_devices

def android_usb_permission_granted(name: str) -> bool:
    return clib.usb_has_permission(name.encode()) == 1

def android_usb_request_permission(name: str) -> bool:
    return clib.usb_request_permission(name.encode()) == 1

def android_is_bluetooth_device(port: str) -> bool:
    return port.startswith("bt://")

def android_bluetooth_permission_granted():
    return clib.bt_has_connect_permission()

def android_bluetooth_request_permission():
    clib.bt_request_connect_permission()

def android_get_bt_mac_address(port: str) -> str | None:
    return port[5:] if is_bluetooth_device(port) else None

def android_bluetooth_get_device_class(port: str) -> int:
    mac = android_get_bt_mac_address(port)
    return clib.bt_get_device_class(mac.encode()) if mac else None


class AndroidSerial(ABC):
    """The pyserial serial class overwrites those properties,
        which are used by SLCAN.
        It accesses the serial devices and communication by calling functions
        of a pre-compiled c++ library.
    """
    def __init__(
        self,
        port: str,
        baudrate = 115200,
        rtscts = False,
        timeout = 0.001,
        write_timeout=1.0,
        **kwargs,
        ):
        self.port = port
        self.baudrate = baudrate
        self.rtscts = rtscts
        self.timeout = timeout
        self.write_timeout = None
        self._closed = False
        self.handle = None

    @property
    def in_waiting(self):
        """It queries the number of readable bytes in the buffer of the c++ library."""
        return clib.rx_buffer_available()

    def read(self, size=1):
        if self._closed or size <= 0:
            return b""
        timeout_ms = (
            -1
            if self.timeout is None
            else max(1, int(self.timeout * 1000)))
        buffer = create_string_buffer(size)
        result = clib.rx_buffer_read(buffer, size, timeout_ms)
        if result < 0:
            raise OSError(f"Serial read failed {result}")
        return buffer.raw[:result]

    def write(self, data):
        pass

    def flush(self):
        pass # synchronised bulk-transfer

    @staticmethod
    def reset_input_buffer():
        clib.clear_rx_buffer()

    @staticmethod
    def reset_output_buffer():
        # The OutputStream does not provide general clear.
        # It should not be assumed that the already sent data can be deleted.
        pass

    def read_until(self, expected: bytes | str = b"\n", size=None):
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
        pass

    def fileno(self):
         raise UnsupportedOperation("The device has no POSIX file descriptor")


class AndroidUsbSerial(AndroidSerial):
    def __init__(
        self,
        port: str,
        baudrate=115200,
        rtscts=False,
        timeout=0.001,
        write_timeout=1.0,
        **kwargs,
    ):
        super().__init__(port=port, baudrate=baudrate, rtscts=rtscts,
                         timeout=timeout, write_timeout=write_timeout, **kwargs)

        if not clib.usb_open_device(port.encode("utf-8")):
            raise OSError("USB device could not be opened")

        if not android_usb_permission_granted(port):
            raise PermissionError("Android USB permission has not been granted")

        self.is_open = True

    def write(self, data):
        if self._closed:
            raise OSError("Android USB device is closed")
        if not isinstance(data, (bytes, bytearray)):
            raise TypeError("write() expects bytes or bytearray")
        timeout_ms = (
            int(self.write_timeout * 100)
            if self.write_timeout is not None
            else 1000
        )
        return clib.usb_write(data, len(data), timeout_ms)

    def close(self):
        if not self._closed:
            self._closed = True
            clib.usb_close()


class AndroidBluetoothSerial(AndroidSerial):
    def __init__(
        self,
        port,
        baudrate=115200,
        rtscts=False,
        timeout=0.1,
        write_timeout=1.0,
        **kwargs,
    ):
        super().__init__(port=port, baudrate=baudrate, rtscts=rtscts,
                         timeout=timeout, write_timeout=write_timeout, **kwargs)

        if not clib.bt_has_connect_permission():
            raise PermissionError("Android Bluetooth permission has not been granted")

        mac = get_bt_mac_address(self.port)
        if not mac:
            raise ValueError(f"Invalid Bluetooth port: {self.port}")

        if not clib.bt_connect(mac.encode()):
            raise OSError(f"Bluetooth SPP connection error: {mac}")

        if not clib.bt_has_connect_permission():
            raise PermissionError("Android Bluetooth permission has not been granted")
        self.is_open = True

    def write(self, data):
        if not self.is_open:
            raise OSError("Bluetooth-connection is not open.")

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

    def close(self):
        if self.is_open:
            clib.bt_close()
            self.is_open = False


def open_serial(
    port,
    baudrate=115200,
    rtscts=False,
    timeout=1.0,
    write_timeout=1.0,
    **kwargs,
) -> AndroidSerial:
    """Used for opening UART communication."""
    port = str(port)
    if port.startswith("bt:"):
        return AndroidBluetoothSerial(
            port=port,
            baudrate=baudrate,
            rtscts=rtscts,
            timeout=timeout,
            write_timeout=write_timeout,
            **kwargs,
        )
    return AndroidUsbSerial(
        port=port,
        baudrate=baudrate,
        rtscts=rtscts,
        timeout=timeout,
        write_timeout=write_timeout,
        **kwargs,
    )

@contextmanager
def android_serial(port): # use: 'with android_serial(args) as serial:'
    android_ser = open_serial(port)
    try:
        yield android_ser
    finally:
        android_ser.close()
