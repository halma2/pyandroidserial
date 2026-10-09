import json
from ctypes import *
from io import UnsupportedOperation
from can.interfaces import slcan

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

def _android_serial_for_url(*args, **kwargs):
    return AndroidSerial(*args, **kwargs)

def usb_host_supported_on_android() -> bool:
    """Ellenőrzi, hogy az android eszköz támogatja-e USB host mechanizmust"""
    return clib.usb_host_supported()

def android_search_for_usb_devices() -> list[str]:
    """Kilistázza csatlakoztatott USB eszközök nevét (device name)"""
    return json.loads(clib.usb_get_all_device_names().decode("utf-8"))

def android_usb_permission_granted(name: str) -> bool:
    return clib.usb_has_permission(name.encode("utf-8")) == 1

def android_usb_request_permission(name: str) -> bool:
    return clib.usb_request_permission(name.encode("utf-8")) == 1

class AndroidSerial:
    """A pyserial serial class osztály azon mezőit írja felül,
    amelyet az slcan használ.
    A soros eszközök elérését és kommunikációját egy előre lefordított
    c++ könyvtár (clib: libpyandroidserial.so) függvényeinek hívásával éri el.
    """
    def __init__(
        self,
        channel: str,
        baudrate=115200,
        rtscts=False,
        timeout=0.001,
    ):
        self.channel = channel
        self.baudrate = baudrate
        self.rtscts = rtscts
        self.timeout = timeout

        self.write_timeout = None
        self._closed = False
        self.handle = clib.usb_open_device(channel.encode("utf-8"))

        if not android_usb_permission_granted(channel):
            raise PermissionError("Android USB permission has not been granted")

    @property
    def in_waiting(self):
        """Lekéri a c++ könyvtárban levő buffer olvasható byte-ok számát."""
        return clib.usb_available()

    def read(self, size=1):
        if self._closed or size <= 0:
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

    def flush(self):
        pass # synchronised bulk-transfer

    def reset_input_buffer(self):
        clib.usb_clear_rx_buffer()

    def close(self):
        if not self._closed:
            self._closed = True
            clib.usb_close()

    def fileno(self):
         raise UnsupportedOperation("Android USB device has no POSIX file descriptor")
