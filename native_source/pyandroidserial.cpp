#include "pyandroidserial.h"
#include <QJniObject>
#include <algorithm>
#include <QJsonArray>

QJniObject g_connection;
QJniObject g_device;
QJniObject g_outEndpoint;
QJniObject g_inEndpoint;
QJniObject g_interface;

/* It states whether there was an exception,
 * if there were, it logs and clear them all
 */
bool clearJniException() {
    QJniEnvironment env;
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

bool usb_host_supported() {
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative",
        "activity",
        "()Landroid/app/Activity;");

    QJniObject packageManager = activity.callObjectMethod(
        "getPackageManager",
        "()Landroid/content/pm/PackageManager;");

    jboolean result = packageManager.callMethod<jboolean>(
        "hasSystemFeature",
        "(Ljava/lang/String;)Z",
        QJniObject::fromString(
            "android.hardware.usb.host"
            ).object<jstring>());

    return result;
}

QJniObject getUsbManager() {
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative",
        "activity",
        "()Landroid/app/Activity;");

    QJniObject usbService = QJniObject::getStaticObjectField(
        "android/content/Context",
        "USB_SERVICE",
        "Ljava/lang/String;");

    QJniObject usbManager = activity.callObjectMethod(
        "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;",
        usbService.object());

    return usbManager;
}

QJniObject getDeviceIterator() {
    QJniObject usbManager = getUsbManager();

    QJniObject deviceList =
        usbManager.callObjectMethod(
            "getDeviceList",
            "()Ljava/util/HashMap;");

    QJniObject values =
        deviceList.callObjectMethod(
            "values",
            "()Ljava/util/Collection;");

    return values.callObjectMethod(
        "iterator",
        "()Ljava/util/Iterator;");
}

QJniObject getDevice(const char* name) {
    QJniObject usbManager = getUsbManager();
    /**
     * HashMap devices = manager.getDeviceList()
     * for device in devices.values():
     *    dev_name = device.getDeviceName()
     *    last_name = dev_name.toString().toUtf8()
     *    if last_name.constData() == name:
     *       return device
     */
    QJniObject iterator = getDeviceIterator();

    while (iterator.callMethod<jboolean>(
        "hasNext",
        "()Z")) {
        QJniObject device = iterator.callObjectMethod(
            "next",
            "()Ljava/lang/Object;");

        QJniObject dev_name = device.callObjectMethod(
            "getDeviceName",
            "()Ljava/lang/String;");

        QByteArray lastName = dev_name.toString().toUtf8();
        if (strcmp(lastName.constData(), name) == 0)
            return device;
    }
    return QJniObject();
}

bool usb_valid_device(const char* name) {
    return getDevice(name).isValid();
}

/**
 * @brief Discover and lit all connected USB devices.
 * @return The lsit of device names in JSON format
 */
const char* usb_get_all_device_names() {
    // this pointer will be stored until the next call in this thread
    static thread_local QByteArray result;
    result.clear();
    QJsonArray jsonArray;

    QJniObject iterator = getDeviceIterator();
    while (iterator.callMethod<jboolean>(
        "hasNext",
        "()Z")) {
        QJniObject device = iterator.callObjectMethod(
            "next",
            "()Ljava/lang/Object;");

        QJniObject dev_name = device.callObjectMethod(
            "getDeviceName",
            "()Ljava/lang/String;");

        QByteArray lastName = dev_name.toString().toUtf8();
        jsonArray.append(dev_name.toString());
    }
    result = QJsonDocument(jsonArray).toJson(QJsonDocument::Compact);
    return result.constData();
}

bool usb_has_permission(const char* name) {
    QJniObject usbManager = getUsbManager();
    QJniObject device = getDevice(name);

    if (device.isValid())
        return usbManager.callMethod<jboolean>(
            "hasPermission",
            "(Landroid/hardware/usb/UsbDevice;)Z",
            device.object());
    return false;
}

bool usb_request_permission(const char* name) {
    QJniObject usbManager = getUsbManager();
    if(!usbManager.isValid())
        return false;

    QJniObject device = getDevice(name);
    if (!device.isValid())
        return false;

    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative",
        "activity",
        "()Landroid/app/Activity;");

    QJniObject intent = QJniObject(
        "android/content/Intent",
        "()V");

    QJniObject pendingIntent = QJniObject::callStaticObjectMethod(
        "android/app/PendingIntent",
        "getBroadcast",
        "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;",
        activity.object(),
        0,
        intent.object(),
        0x04000000); // FLAG_IMMUTABLE

    if (!pendingIntent.isValid())
        return false;

    usbManager.callMethod<void>(
        "requestPermission",
        "(Landroid/hardware/usb/UsbDevice;Landroid/app/PendingIntent;)V",
        device.object(),
        pendingIntent.object());
    return true;
}

/**
 * It searches for the g_device's all interfaces and those endpoints.
 * In those it searches for the in and output endpoints and stores them in global.
 * (Input: {type: Bulk (transfer), direction: in;
 * Output: {type: Bulk(transfer), direction: out})
 */
bool scanInterfaces() {
    if (!g_device.isValid())
        return false;

    bool foundIn = false;
    bool foundOut = false;

    const jint INTERFACE_COUNT = g_device.callMethod<jint>(
        "getInterfaceCount",
        "()I");

    const jint USB_DIR_OUT = QJniObject::getStaticField<jint>(
        "android/hardware/usb/UsbConstants",
        "USB_DIR_OUT");

    const jint USB_DIR_IN = QJniObject::getStaticField<jint>(
        "android/hardware/usb/UsbConstants",
        "USB_DIR_IN");

    const jint USB_ENDPOINT_XFER_BULK = QJniObject::getStaticField<jint>(
        "android/hardware/usb/UsbConstants",
        "USB_ENDPOINT_XFER_BULK");

    for (int i = 0; i < INTERFACE_COUNT; i++) {
        QJniObject iface =
            g_device.callObjectMethod(
                "getInterface",
                "(I)Landroid/hardware/usb/UsbInterface;",
                i);
        jint endpointCount =
            iface.callMethod<jint>(
                "getEndpointCount",
                "()I");

        for (int ep = 0; ep < endpointCount; ep++) {
            QJniObject endpoint =
                iface.callObjectMethod(
                    "getEndpoint",
                    "(I)Landroid/hardware/usb/UsbEndpoint;",
                    ep);
            jint direction =
                endpoint.callMethod<jint>(
                    "getDirection",
                    "()I");

            jint type =
                endpoint.callMethod<jint>(
                    "getType",
                    "()I");
            if (type == USB_ENDPOINT_XFER_BULK) { // BULK=2, Interrupt=3
                if (direction == USB_DIR_OUT) { // direction: OUT=0, IN=0x80
                    g_outEndpoint = endpoint;
                    foundOut = true;
                }
                else if (direction == USB_DIR_IN){
                    g_inEndpoint = endpoint;
                    foundIn = true;
                }
            }
        }
        if (foundIn && foundOut) {
            g_interface = iface;
            break;
        }
    }
    if (!foundIn || !foundOut)
        return false;
    return true;
}

/**
 * @brief In Android, it calls a bulk transfer read which result will be stroed in a buffer.
 * @param buffer: temporary (the length of the calling usbReaderThread buffer is 4096).
 * @param maxlen
 * @param timeoutMs
 * @return lenght of the incoming data
 */
int usb_bulk_read(jbyteArray arr, char* buffer, int maxLen, int timeoutMs) {
    if (!g_connection.isValid() || !g_inEndpoint.isValid() || !arr || maxLen <= 0)
        return -1;

    QJniEnvironment env;

    jint received = g_connection.callMethod<jint>(
        "bulkTransfer",
        "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
        g_inEndpoint.object(),
        arr,
        maxLen,
        timeoutMs);

    if (clearJniException())
        return -1;

    if (received > 0) {
        env->GetByteArrayRegion(
            arr,
            0,
            received,
            reinterpret_cast<jbyte *>(buffer));

        if (clearJniException())
            return -1;
    }
    return received;
}

/**
 * It calls a bulk transfer read every 10 ms, which it uses a 4KB buffer.
 * If new data does not arrive, then it loads the previously arrived datas into the end of the buffer.
 * When data arrives, it flags with g_rxCV which says that the global buffer can be read.
 * It reads until connection is broken: usb_close -> g_rxRunning = false;
 */
void usbReaderThread() {
    constexpr int BUFFER_SIZE = 4096;
    char temp[BUFFER_SIZE];

    QJniEnvironment env;
    jbyteArray javaBuffer = env->NewByteArray(BUFFER_SIZE);
    bool jniError = clearJniException();
    if (!javaBuffer || jniError) {
        {
            lock_guard<mutex> lock(g_rxMutex);
            g_rxError = true;
        }
        g_rxRunning = false;
        g_rxCv.notify_all();
        return;
    }
    while (g_rxRunning) {
        int received = usb_bulk_read(javaBuffer, temp, sizeof(temp), 10);
        if (received > 0) {
            {
                lock_guard<mutex> lock(g_rxMutex);

                if (received > MAX_RX_BUFFER - g_rxBuffer.size()) {
                    g_rxError = true;
                    g_rxCv.notify_all();
                    break;
                }
                g_rxBuffer.insert(g_rxBuffer.end(), temp, temp + received);
            }
            g_rxCv.notify_all();
        }
        else if (received < 0) {
            if (g_rxRunning)
                break;
            this_thread::sleep_for(chrono::milliseconds(5));
        }
    }
    env->DeleteLocalRef(javaBuffer);
    g_rxRunning = false;
    g_rxCv.notify_all();
}

/**
 * If the app has permission for using USB device @param name,
 * then it opens a connection, maps the in and out endpoints and stores them global,
 * claims the data interface and starts the continous reading in a different thread (usbReaderThread).
 * Finally it clears the reading buffer.
 */
bool usb_open_device(const char* name) {
    usb_close();
    QJniObject usbManager = getUsbManager();
    if (!usbManager.isValid())
        return false;

    QJniObject device = getDevice(name);
    if (!device.isValid())
        return false;

    if (!usb_has_permission(name))
        return false;

    QJniObject connection = usbManager.callObjectMethod(
        "openDevice",
        "(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;",
        device.object());

    if (!connection.isValid())
        return false;

    g_connection = connection;
    g_device = device;

    if (!scanInterfaces()) {
        usb_close();
        return false;
    }
    jboolean ok = connection.callMethod<jboolean>(
        "claimInterface",
        "(Landroid/hardware/usb/UsbInterface;Z)Z",
        g_interface.object(),
        JNI_TRUE);
    if (clearJniException() || ok != JNI_TRUE) {
        usb_close();
        return false;
    }
    {
        lock_guard<mutex> lock(g_rxMutex);
        g_rxBuffer.clear();
        g_rxError = false;
    }
    g_rxRunning = true;
    g_rxThread = thread(usbReaderThread);
    return true;
}

/**
 * In Android it calls a bulk transfer write (with the nescessary parameteres).
 * @return the byte lenght of the sent data
 */
int usb_write(const char* data, int len, int timeoutMs) {
    if (!g_connection.isValid() || !g_outEndpoint.isValid() || !data || len < 0)
        return -1;
    if (len == 0)
        return 0;

    QJniEnvironment env;
    jbyteArray arr = env->NewByteArray(len);

    bool jniError = clearJniException();
    if (jniError || !arr)
        return -1;

    env->SetByteArrayRegion(
        arr,
        0,
        len,
        reinterpret_cast<const jbyte *>(data));
    if (clearJniException()) {
        env->DeleteLocalRef(arr);
        return -1;
    }

    jint written = g_connection.callMethod<jint>(
        "bulkTransfer",
        "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
        g_outEndpoint.object(),
        arr,
        len,
        timeoutMs);

    jniError = clearJniException();

    env->DeleteLocalRef(arr);

    return jniError ? -1 : written;
}

/**
 * Number of readable bytes from the RX buffer (dynamic / deque).
 * The py(android)serial uses to know how much incoming data arrived to the buffer.
*/
int rxBufferAvailable() {
    lock_guard<mutex> lock(g_rxMutex);
    return static_cast<int>(g_rxBuffer.size());
}

int usb_available() {
    return rxBufferAvailable();
}

/**
 * It reads the global buffer.
 * At the begining, the empty buffer wait for at most the given time before reading (except the time set to 0)
 * If g_rxCv condition variable notifies before the time limit that data arrived to the empty buffer,
 * then locking the reader thread it wathces wether the buffer is empty or the reader thread stopped.
 * If it is false, then it keeps waiting, otherwise it moves on and reads the whole buffer.
 * @param buffer
 * @param maxLen
 * @param timeoutMs
 * @return min(maxLen, g_rxBuffer.size())
 */
int rxRead(char* buffer, int maxLen, int timeoutMs = 10) {
    if (!buffer || maxLen <= 0)
        return -1;

    unique_lock<mutex> lock(g_rxMutex);
    auto ready = [] { return !g_rxBuffer.empty() || g_rxError || !g_rxRunning; };

    if (g_rxBuffer.empty()) {
        if (timeoutMs < 0)
            g_rxCv.wait(lock, ready);
        else if (!g_rxCv.wait_for(lock, chrono::milliseconds(timeoutMs), ready))
            return 0;
    }
    if (g_rxBuffer.empty()) {
        lock_guard<mutex> lock(g_rxMutex);
        return g_rxError ? -1 : 0;
    }

    const int n = min(maxLen, static_cast<int>(g_rxBuffer.size()));

    for (int i = 0; i < n; ++i) {
        buffer[i] = g_rxBuffer.front();
        g_rxBuffer.pop_front();
    }

    return n;
}

int usb_read(char* buffer, int maxLen, int timeoutMs = 10) {
    return rxRead(buffer, maxLen, timeoutMs);
}

void clearRxBuffer() {
    lock_guard<mutex> lock(g_rxMutex);
    g_rxBuffer.clear();
}

void usb_clear_rx_buffer() {
    clearRxBuffer();
}

void usb_close() {
    g_rxRunning = false;
    g_rxCv.notify_all();

    if (g_rxThread.joinable())
        g_rxThread.join();

    if (g_connection.isValid()) {
        if (g_interface.isValid()) {
            g_connection.callMethod<jboolean>(
                "releaseInterface",
                "(Landroid/hardware/usb/UsbInterface;)Z",
                g_interface.object());
        }
        clearJniException();

        g_connection.callMethod<void>(
            "close",
            "()V");
        clearJniException();
    }
    g_interface = QJniObject();
    g_inEndpoint = QJniObject();
    g_outEndpoint = QJniObject();
    g_connection = QJniObject();
}
