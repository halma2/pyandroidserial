#include "pyandroidserial_bt.h"
#include "pyandroidserial.h"
#include <QJniObject>
#include <QCoreApplication>
#include <QJsonArray>

// unique spp service ID: the short form of serial port profile UUID is 1101
constexpr const char* SPP_UUID = "00001101-0000-1000-8000-00805F9B34FB";
constexpr int ANDROID_SDK_S = 31; // below that version, Bluetooth connect permission is not needed
constexpr int BLUETOOTH_PERMISSION_REQUEST_CODE = 1001;

mutex g_stateMutex;
QJniObject g_socket;
QJniObject g_input;
QJniObject g_output;
bool g_connected = false;

bool bt_has_connect_permission() {
    if (QNativeInterface::QAndroidApplication::sdkVersion() < ANDROID_SDK_S)
        return true;

    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid())
        return false;

    QJniObject permission = QJniObject::fromString(QStringLiteral("android.permission.BLUETOOTH_CONNECT"));

    const jint granted = activity.callMethod<jint>(
        "checkSelfPermission",
        "(Ljava/lang/String;)I",
        permission.object<jstring>()
        );

    if (clearJniException())
        return false;

    return granted == 0; // PackageManager.PERMISSION_GRANTED == 0
}

const char* bt_get_paired_devices() {
    // this pointer will be stored until the next call
    static thread_local QByteArray result;
    result.clear();

    // Android 12+: BLUETOOTH_CONNECT permission is needed.
    if (!bt_has_connect_permission()) {
        bt_request_connect_permission();
        return "[]"; // empty json format
    }

    QJniObject adapter = QJniObject::callStaticObjectMethod(
        "android/bluetooth/BluetoothAdapter",
        "getDefaultAdapter",
        "()Landroid/bluetooth/BluetoothAdapter;"
        );

    if (clearJniException() || !adapter.isValid())
        return result.constData();

    QJniObject pairedDevices = adapter.callObjectMethod(
        "getBondedDevices",
        "()Ljava/util/Set;"
        );

    if (clearJniException() || !pairedDevices.isValid())
        return result.constData();

    QJniObject iterator = pairedDevices.callObjectMethod(
        "iterator",
        "()Ljava/util/Iterator;"
        );

    if (clearJniException() || !iterator.isValid())
        return result.constData();

    QJsonArray jsonArray;

    while (iterator.callMethod<jboolean>("hasNext", "()Z")) {
        if (clearJniException())
            break;

        QJniObject device = iterator.callObjectMethod(
            "next",
            "()Ljava/lang/Object;"
            );

        if (clearJniException() || !device.isValid())
            continue;

        QJniObject nameObject = device.callObjectMethod(
            "getName",
            "()Ljava/lang/String;"
            );

        if (clearJniException())
            continue;

        QJniObject addressObject = device.callObjectMethod(
            "getAddress",
            "()Ljava/lang/String;"
            );
        if (clearJniException() || !addressObject.isValid())
            continue;

        QString address = "bt://" + addressObject.toString();
        jsonArray.append(address);
    }
    result = QJsonDocument(jsonArray).toJson(QJsonDocument::Compact);
    return result.constData();
}

int bt_get_device_class(const char* mac_address) {
    if (!mac_address || !*mac_address)
        return false;

    QJniObject adapter = QJniObject::callStaticObjectMethod(
        "android/bluetooth/BluetoothAdapter",
        "getDefaultAdapter",
        "()Landroid/bluetooth/BluetoothAdapter;"
        );

    if (clearJniException() || !adapter.isValid())
        return false;

    QJniObject mac = QJniObject::fromString(QString::fromLatin1(mac_address));

    QJniObject device = adapter.callObjectMethod(
        "getRemoteDevice",
        "(Ljava/lang/String;)Landroid/bluetooth/BluetoothDevice;",
        mac.object<jstring>()
        );
    if (clearJniException() || !device.isValid())
        return 0;

    QJniObject bluetoothClass = device.callObjectMethod(
        "getBluetoothClass",
        "()Landroid/bluetooth/BluetoothClass;"
        );
    if (clearJniException() || !bluetoothClass.isValid())
        return 0;

    return bluetoothClass.callMethod<jint>(
            "getDeviceClass",
            "()I"
            );
}

// It sends a permission request which runs in the Android UI-thread.
void bt_request_connect_permission() {
    if (QNativeInterface::QAndroidApplication::sdkVersion() < ANDROID_SDK_S || bt_has_connect_permission())
        return;

    // BLUETOOTH_CONNECT must be requested on the actual activity only on the Main Thread
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread(
        []() {
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (!activity.isValid())
            return;

        QJniObject permission = QJniObject::fromString(
            QStringLiteral(
                "android.permission.BLUETOOTH_CONNECT"
                )
            );

        QJniEnvironment env;

        jclass stringClass = env->FindClass("java/lang/String");
        if (!stringClass || clearJniException())
            return;

        jobjectArray permissions = env->NewObjectArray(
            1,
            stringClass,
            permission.object<jstring>()
            );

        env->DeleteLocalRef(stringClass); // not used anymore, free up the reference

        if (!permissions || clearJniException())
            return;

        activity.callMethod<void>(
            "requestPermissions",
            "([Ljava/lang/String;I)V",
            permissions,
            jint(BLUETOOTH_PERMISSION_REQUEST_CODE)
            );

        env->DeleteLocalRef(permissions);
    });
}

/**
 * bt_connect calls it in a new thread, where it reads from the input(stream) in a loop.
 * It loads the incoming stream to a byte array then to a temporary array then to the global buffer (queue).
 * @param input inputstream
 */
void btReaderThread(QJniObject input) {
    constexpr int BUFFER_SIZE = 1024;

    QJniEnvironment env;
    jbyteArray javaBuffer = env->NewByteArray(BUFFER_SIZE);

    bool jniError = clearJniException();
    if (!javaBuffer || jniError) { // exception occured during reading
        {
            lock_guard<mutex> lock(g_rxMutex);
            g_rxRunning = false;
            g_rxError = true;
        }
        g_rxCv.notify_all();
        return;
    }
    char temp[BUFFER_SIZE];

    while (g_rxRunning) {
        jint n = input.callMethod<jint>(
            "read",
            "([BII)I",
            javaBuffer,
            jint(0),
            jint(BUFFER_SIZE)
            ); // n = inputstream.read(&javaBuffer, maxLength=1024)

        if (clearJniException() || n < 0) { // exception occured
            if (g_rxRunning) {
                lock_guard<mutex> lock(g_rxMutex);
                g_rxError = true;
            }
            break;
        }
        if (n == 0) {
            this_thread::yield(); // gives resources & time to other threads
            continue;
        }
        env->GetByteArrayRegion( // Java byte[] -> temporary native buffer (due to the architecture of deque)
            javaBuffer,
            0,
            n,
            reinterpret_cast<jbyte*>(temp)
        );
        if (clearJniException()) {
            lock_guard<mutex> lock(g_rxMutex);
            g_rxError = true;
            break;
        }
        {
            lock_guard<mutex> lock(g_rxMutex);

            if (g_rxBuffer.size() + size_t(n) > MAX_RX_BUFFER) {
                g_rxError = true;
                break;
            }
            g_rxBuffer.insert( // Temporary buffer -> deque
                g_rxBuffer.end(),
                temp,
                temp + n
            );
        }
        g_rxCv.notify_all();
    }
    env->DeleteLocalRef(javaBuffer);
    {
        lock_guard<mutex> lock(g_rxMutex);
        g_rxRunning = false;
    }
    g_rxCv.notify_all();
}

/**
 * It connects to the device identfied by mac address: Open a RFCOMM socket
 * to the serial UUID and saves it with its input and output streams globally.
 * +clear the global buffer for reading
 * @param mac_address
 * @warning because socket.connect() is a blocking function bt_connect will become too!
 * @return bool: successful connection
 */
bool bt_connect(const char* mac_address) {
    if (!mac_address || !*mac_address)
        return false;
    if (!bt_has_connect_permission())
        return false;
    bt_close(); // close previous connections

    QJniObject adapter = QJniObject::callStaticObjectMethod(
        "android/bluetooth/BluetoothAdapter",
        "getDefaultAdapter",
        "()Landroid/bluetooth/BluetoothAdapter;"
        );

    if (clearJniException() || !adapter.isValid())
        return false;

    // cancel Bluetooth device-discovery before connecting
    adapter.callMethod<jboolean>("cancelDiscovery", "()Z");

    if (clearJniException())
        return false;

    QJniObject mac = QJniObject::fromString(
        QString::fromLatin1(mac_address)
        );

    QJniObject device = adapter.callObjectMethod(
        "getRemoteDevice",
        "(Ljava/lang/String;)Landroid/bluetooth/BluetoothDevice;",
        mac.object<jstring>()
        );

    if (clearJniException() || !device.isValid())
        return false;

    QJniObject uuidString = QJniObject::fromString(QString::fromLatin1(SPP_UUID));

    QJniObject uuid = QJniObject::callStaticObjectMethod(
        "java/util/UUID",
        "fromString",
        "(Ljava/lang/String;)Ljava/util/UUID;",
        uuidString.object<jstring>()
        );

    if (clearJniException() || !uuid.isValid())
        return false;

    QJniObject socket = device.callObjectMethod(
        "createRfcommSocketToServiceRecord",
        "(Ljava/util/UUID;)Landroid/bluetooth/BluetoothSocket;",
        uuid.object<jobject>()
        );

    if (clearJniException() || !socket.isValid())
        return false;

    socket.callMethod<void>("connect");
    if (clearJniException()) { // can throw a IOException if the device does not accept the connection
        socket.callMethod<void>("close");
        clearJniException();
        return false;
    }

    QJniObject input = socket.callObjectMethod(
        "getInputStream",
        "()Ljava/io/InputStream;"
        );

    QJniObject output = socket.callObjectMethod(
        "getOutputStream",
        "()Ljava/io/OutputStream;"
        );

    if (clearJniException() || !input.isValid() || !output.isValid()) {
        socket.callMethod<void>("close");
        clearJniException();
        return false;
    }
    {
        lock_guard<mutex> lock(g_stateMutex);
        g_socket = socket;
        g_input = input;
        g_output = output;
        g_connected = true;
    }
    {
        lock_guard<mutex> lock(g_rxMutex);
        g_rxBuffer.clear();
        g_rxError = false;
    }
    g_rxRunning = true;
    g_rxThread = thread(btReaderThread, input);
    return true;
}

int bt_available() {
    return rxBufferAvailable();
}

int bt_read(char* buffer, int maxLen, int timeoutMs=10) {
    return rxRead(buffer, maxLen, timeoutMs);
}

int bt_write(const char* data, int len, int timeoutMs) {
    Q_UNUSED(timeoutMs); // bluetooth does not use timeout

    if (!data || len < 0)
        return -1;
    if (len == 0)
        return 0;

    QJniObject output;
    {
        lock_guard<mutex> lock(g_stateMutex);

        if (!g_connected || !g_output.isValid())
            return -1;

        output = g_output;
    }
    QJniEnvironment env;
    jbyteArray javaBuffer = env->NewByteArray(len);

    if (!javaBuffer || clearJniException())
        return -1;

    env->SetByteArrayRegion(
        javaBuffer,
        0,
        len,
        reinterpret_cast<const jbyte*>(data)
        );

    if (clearJniException()) {
        env->DeleteLocalRef(javaBuffer);
        return -1;
    }
    output.callMethod<void>(
        "write",
        "([BII)V",
        javaBuffer,
        jint(0),
        jint(len)
        );

    const bool failed = clearJniException();

    if (!failed)
        output.callMethod<void>("flush", "()V");
    const bool flushFailed = clearJniException();
    env->DeleteLocalRef(javaBuffer);
    return (failed || flushFailed) ? -1 : len;
}

void bt_clear_rx_buffer() {
    clearRxBuffer();
}

void bt_close() {
    QJniObject socket;
    {
        lock_guard<mutex> lock(g_stateMutex);
        socket = g_socket;
        g_connected = false;
    }
    // Closing the socket may interrupt the blocking InputStream.read().
    if (socket.isValid()) {
        socket.callMethod<void>("close");
        clearJniException();
    }
    g_rxRunning = false;
    g_rxCv.notify_all();

    if (g_rxThread.joinable()) {
        if (g_rxThread.get_id() == this_thread::get_id())
            g_rxThread.detach();
        else
            g_rxThread.join();
    }
    {
        lock_guard<mutex> lock(g_stateMutex);
        g_socket = QJniObject();
        g_input = QJniObject();
        g_output = QJniObject();
    }
    {
        lock_guard<mutex> lock(g_rxMutex);
        g_rxBuffer.clear();
        g_rxError = false;
    }
}
