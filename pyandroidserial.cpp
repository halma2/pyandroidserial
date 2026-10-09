#include "pyandroidserial.h"
#include <QJniObject>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <thread>
#include <atomic>
using namespace std;

QJniObject g_connection;
QJniObject g_device;
QJniObject g_outEndpoint;
QJniObject g_inEndpoint;
QJniObject g_interface;
vector<char> g_rxBuffer;
mutex g_rxMutex;
condition_variable g_rxCv;
atomic<bool> g_rxRunning{false};
thread g_rxThread;

class UsbException : public runtime_error {
public:
    explicit UsbException(const string& msg): runtime_error(msg) {}
};

void checkValid(const QJniObject& obj, const char* msg) {
    if (!obj.isValid())
        throw UsbException(msg);
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
 * @brief Kilistázza az összes csatlakoztarott USB eszköz listáját.
 * @return "["dev1","dev2",..."devN",]" JSON format
 * @warning Nem szálbiztos, static string-ben tárolja:
 * (a memóriája csak a program leállásakor szabadul fel)
 */
const char* usb_get_all_device_names() {
    static string name_list;
    name_list.clear();
    name_list += "[";
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
        name_list += '"';
        name_list += lastName.constData();
        name_list += "\",";
    }
    name_list.pop_back(); // JSON format: remove last trailing comma (',')
    name_list += ']';
    return name_list.c_str();
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
 * A g_device USB minden interface-ét és azok végpontjait nézi meg.
 * Megkeresi a ki és bemeneti végpontokat és azokat globál változókban tárolja.
 * (Bemenet: {type: Bulk (transfer), direction: in;
 * Kimenet: {type: Bulk(transfer), direction: out})
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
 * @brief Androidon meghív egy bulk transfer olvasást, melyet a bufferben tárol el.
 * @param buffer: ideiglenes (az őt hívó usb_reader_thread 4096 hosszú).
 * @param maxlen
 * @param timeoutMs
 * @return beérkező adat hossza
 */
int usb_bulk_read(char* buffer, int maxlen, int timeoutMs) {
    if (!g_connection.isValid() || !g_inEndpoint.isValid())
        return -1;

    QJniEnvironment env;
    jbyteArray arr = env->NewByteArray(maxlen);
    if (!arr)
        return -1;


    jint received = g_connection.callMethod<jint>(
        "bulkTransfer",
        "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
        g_inEndpoint.object(),
        arr,
        maxlen,
        timeoutMs);

    if (received > 0) {
        env->GetByteArrayRegion(
            arr,
            0,
            received,
            reinterpret_cast<jbyte *>(buffer));
    }
    env->DeleteLocalRef(arr);
    return received;
}

/**
 * 10 ms-ként bulk transfer olvasást kezdeményez, melyhez ideiglenesen egy 4 KB os tömböt használ.
 * Ha érkezett adat, akkor azt a beérkező adatmennyiséget a globál buffer végébe tölti.
 * Adat beérkezéskor a g_rxCV változóval jelzi, hogy a globál buffer olvasható.
 * Addig olvas, míg a kapcsolat nem lesz bontva: usb_close -> g_rxRunning = false;
 */
void usb_reader_thread() {
    char temp[4096];

    while (g_rxRunning) {
        int received = usb_bulk_read(temp, sizeof(temp), 10);
        if (received > 0) {
            {
                lock_guard<mutex> lock(g_rxMutex);
                g_rxBuffer.insert(g_rxBuffer.end(), temp, temp + received);
            }
            g_rxCv.notify_all();
        }
        else if (received < 0)
        {
            if (!g_rxRunning)
                break;
            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }
    g_rxRunning = false;
    g_rxCv.notify_all();
}

/**
 * Ha az alkalmazásnak van engedélye a @param name azonosítójú USB eszköz használatára,
 * akkor kapcsolatot nyit vele, feltérképezi a ki és bemeneti végpontjait,
 * majd ezeket globál változókként eltárolja, az eszköz adatforgalmú interfészét
 * lefoglalja (claim), végül elindíja a folyamatos olvasást külön szálon (usb_reader_thread),
 * s ezzel kapcsolatban az olvasó buffert kiüríti.
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

    if (!connection.isValid()) {
        return false;
    }
    g_connection = connection;
    g_device = device;

    if (!scanInterfaces()) {
        usb_close();
        return false;
    }

    //connection.claimInterface(iface, true);
    jboolean ok = connection.callMethod<jboolean>(
        "claimInterface",
        "(Landroid/hardware/usb/UsbInterface;Z)Z",
        g_interface.object(),
        JNI_TRUE);
    if (ok == JNI_TRUE) {
        {
            lock_guard<mutex> lock(g_rxMutex);
            g_rxBuffer.clear();
        }
        g_rxRunning = true;
        g_rxThread = thread(usb_reader_thread);
        return true;
    }
    return false;
}

/**
 * Androidon egy bulk transfer írást hív meg (a szükséges paraméterekkel).
 * @return elküldött byte mennyiség
 */
int usb_write(const char* data, int len, int timeoutMs) {
    if (!g_connection.isValid() || !g_outEndpoint.isValid())
        return -1;

    QJniEnvironment env;

    jbyteArray arr = env->NewByteArray(len);

    env->SetByteArrayRegion(
        arr,
        0,
        len,
        reinterpret_cast<const jbyte *>(data));

    jint written =
        g_connection.callMethod<jint>(
            "bulkTransfer",
            "(Landroid/hardware/usb/UsbEndpoint;[BII)I",
            g_outEndpoint.object(),
            arr,
            len,
            timeoutMs);

    env->DeleteLocalRef(arr);

    return written;
}

/**
 * Olvasható byte-ok száma az RX bufferből (amely egy dinamikus tömb, vector).
 * Célja, hogy a soros olvasó (pyserial) ez alapján tudja,
 * mennyi byte beolvasandó adat érkezett be a bufferbe.
*/
int usb_available()
{
    lock_guard<mutex> lock(g_rxMutex);
    return static_cast<int>(g_rxBuffer.size());
}

/**
 * A globál buffer tartalmát olvsassa ki.
 * A kezdetben üres bufferre legfeljebb a megadott időtartalmat vár, mielőtt olvasná (hacsak nem 0-ra van állítva).
 * Ha még az idő előtt a g_rxCv feltéleles változó értesíti (hogy érkezett adat az eddig üres bufferbe),
 * akkor a külső olvasó szálat biztonságosan lezárva (lock) megnézi,
 * hogy a buffer nem üres-e, vagy az olvasó szál nem fut.
 * Ha hamis, akkor tovább vár, különben tovább megy és kimásolja magának a buffert.
 * @param buffer
 * @param maxlen
 * @param timeoutMs
 * @return min(maxlen, g_rxBuffer.size())
 */
int usb_read(char* buffer, int maxlen, int timeoutMs = 10) {
    if (buffer == nullptr || maxlen <= 0)
        return -1;

    unique_lock<mutex> lock(g_rxMutex); // timed locking

    if (g_rxBuffer.empty()) {
        if (timeoutMs < 0)
            g_rxCv.wait(lock, [] {return !g_rxBuffer.empty() || !g_rxRunning;});
        else
            g_rxCv.wait_for(lock, chrono::milliseconds(timeoutMs), [] {return !g_rxBuffer.empty() || !g_rxRunning;});
    }
    if (g_rxBuffer.empty())
        return 0;

    const int count = min(maxlen, static_cast<int>(g_rxBuffer.size()));

    copy(g_rxBuffer.begin(), g_rxBuffer.begin() + count, buffer);

    g_rxBuffer.erase(g_rxBuffer.begin(),g_rxBuffer.begin() + count);

    return count;
}

void usb_clear_rx_buffer(){
    lock_guard<mutex> lock(g_rxMutex);
    g_rxBuffer.clear();
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

        g_connection.callMethod<void>(
            "close",
            "()V");
    }
    g_interface = QJniObject();
    g_inEndpoint = QJniObject();
    g_outEndpoint = QJniObject();
    g_connection = QJniObject();
}
