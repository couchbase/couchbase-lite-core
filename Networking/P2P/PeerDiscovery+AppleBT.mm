//
// Created by Jens Alfke on 2/24/25.
//

#ifdef __APPLE__

#include "PeerDiscovery+AppleBT.hh"
#include "Address.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <dispatch/dispatch.h>

#undef DebugAssert
#include <CoreBluetooth/CoreBluetooth.h>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::p2p;

namespace litecore::p2p {
    extern LogDomain P2PLog;

    struct BluetoothProvider;
    static BluetoothProvider* sProvider;

    static constexpr int kMinimumRSSI = -80;


    /** C4Peer subclass created by BluetoothProvider. */
    class BluetoothPeer : public C4Peer {
    public:
        BluetoothPeer(C4PeerDiscoveryProvider* provider, string const& id, string const& name, Metadata const& md,
                      CBPeripheral* p)
        :C4Peer(provider, id, name, md)
        ,_peripheral(p)
        { }

        void removed() override {
            C4Peer::removed();
            _peripheral = nil;
        }

        CBPeripheral* _peripheral {};
    };

}


@interface LiteCoreBluetoothProvider : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*)counterpart
                               queue: (dispatch_queue_t)queue
                         serviceUUID: (NSString*)serviceUUID;
- (void) startBrowsing;
- (void) stopBrowsing;
- (void) monitorPeer: (Retained<BluetoothPeer>)peer state: (bool)state;
- (void) connect: (Retained<BluetoothPeer>)peer;
- (void) cancelConnect: (Retained<BluetoothPeer>)peer;
@end


namespace litecore::p2p {

#pragma mark - PROVIDER:

    /** Implements Bluetooth LE peer discovery using CoreBluetooth. */
    struct BluetoothProvider : public C4PeerDiscoveryProvider, public Logging {

        explicit BluetoothProvider(string const& serviceType)
        :C4PeerDiscoveryProvider("Bluetooth")
        ,Logging(P2PLog)
        ,_queue(dispatch_queue_create("LiteCore Bluetooth", DISPATCH_QUEUE_SERIAL))
        {
            NSString* uuid = serviceType.empty() ? nil : @(serviceType.c_str());
            _counterpart = [[LiteCoreBluetoothProvider alloc] initWithCounterpart: this queue: _queue serviceUUID: uuid];
        }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ [_counterpart startBrowsing]; });
        }

        void stopBrowsing() override {
            dispatch_async(_queue, ^{ [_counterpart stopBrowsing]; });
        }

        void monitorMetadata(C4Peer* peer, bool start) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_counterpart monitorPeer: btPeer state: start]; });
        }

        void resolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_counterpart connect: btPeer]; });
        }

        void cancelResolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_counterpart cancelConnect: btPeer]; });
        }

        void publish(std::string_view displayName, uint16_t port, C4Peer::Metadata const& meta) override {
//            string           nameStr(displayName);
//            C4Peer::Metadata metaCopy = meta;
//            dispatch_async(_queue, ^{ do_publish(std::move(nameStr), port, std::move(metaCopy)); });
        }

        void unpublish() override {
//            dispatch_async(_queue, ^{ do_unpublish(); });
        }

        void updateMetadata(C4Peer::Metadata const& meta) override {
//            C4Peer::Metadata metaCopy = meta;
//            dispatch_async(_queue, ^{ do_updateMetadata(std::move(metaCopy)); });
        }

        //---- Inherited methods redeclared as public so the Obj-C class can call them:
        void _log(LogLevel level, const char* format, ...) const __printflike(3, 4) { LOGBODY_(level) }
        void browseStateChanged(bool s, C4Error e = {})     {C4PeerDiscoveryProvider::browseStateChanged(s, e);}
        void publishStateChanged(bool s, C4Error e = {})    {C4PeerDiscoveryProvider::publishStateChanged(s, e);}
        fleece::Retained<C4Peer> addPeer(C4Peer* peer)      {return C4PeerDiscoveryProvider::addPeer(peer);}
        bool removePeer(std::string_view id)                {return C4PeerDiscoveryProvider::removePeer(id);}
        bool removePeer(C4Peer* peer)                       {return C4PeerDiscoveryProvider::removePeer(peer);}

      private:
        dispatch_queue_t const      _queue;             // Dispatch queue I run on
        LiteCoreBluetoothProvider*  _counterpart {};    // My Obj-C instance that does the real work
    };

    void InitializeBluetoothProvider(string_view serviceType) {
        static once_flag sOnce;
        call_once(sOnce, [&] {
            sProvider = new BluetoothProvider(string(serviceType));
            C4PeerDiscovery::registerProvider(sProvider);
        });
    }

}  // namespace litecore::p2p


#pragma mark - OBJECTIVE-C IMPLEMENTATION:


@implementation LiteCoreBluetoothProvider
{
    BluetoothProvider*  _counterpart;
    dispatch_queue_t    _queue;
    NSArray<CBUUID*>*   _serviceUUIDs;
    CBCentralManager*   _manager;
}

- (instancetype) initWithCounterpart: (BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
                         serviceUUID: (NSString*)serviceUUID
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
        if (serviceUUID)
            _serviceUUIDs = @[ [CBUUID UUIDWithString: serviceUUID] ];
    }
    return self;
}

- (void) startBrowsing {
    if (!_manager) {
        _counterpart->_log(LogLevel::Info, "Starting browsing...");
        _manager = [[CBCentralManager alloc] initWithDelegate: self
                                                        queue: _queue
                                                      options: @{CBCentralManagerOptionShowPowerAlertKey: @YES}];
    }
    [self _startDiscovery];
}

- (void) _startDiscovery {
    if (!_manager.isScanning && _manager.state == CBManagerStatePoweredOn) {
        _counterpart->_log(LogLevel::Info, "Scanning for BLE peripherals");
        [_manager scanForPeripheralsWithServices: _serviceUUIDs options: nil];
    }
}

- (void) stopBrowsing {
    _counterpart->_log(LogLevel::Info, "Stopping browsing");
    [self _stopDiscoveryWithError: nil];
}

- (void) _stopDiscoveryWithError: (const char*)error {
    if (_manager.isScanning)
        [_manager stopScan];
    _manager = nil;
    C4Error c4err {};
    if (error)
        c4err = C4Error::make(LiteCoreDomain, kC4ErrorIOError, error);  //TODO: Real error
    _counterpart->browseStateChanged(false, c4err);
}

- (void) monitorPeer: (Retained<BluetoothPeer>)peer state: (bool)state {
    Assert(_manager);
    if (auto peripheral = peer->_peripheral) {
        if (state)
            [_manager connectPeripheral: peripheral options: nil];
        else
            [_manager cancelPeripheralConnection: peripheral];
    }
}

- (void) connect: (Retained<BluetoothPeer>)peer {
    net::Address addr("l2cap", peer->id, 42, "/db");
    peer->resolvedURL(string(addr.url()), {});
}

- (void) cancelConnect: (Retained<BluetoothPeer>)peer { }


#pragma mark - CBCentralManagerDelegate

- (void) centralManagerDidUpdateState: (CBCentralManager*)central {
    switch (central.state) {
        case CBManagerStatePoweredOn:
            [self _startDiscovery];
            return;
        case CBManagerStatePoweredOff:
            return [self _stopDiscoveryWithError: "Bluetooth is off"];
        case CBManagerStateUnauthorized:
            return [self _stopDiscoveryWithError: "Bluetooth access is not authorized. Check settings"];
        case CBManagerStateUnsupported:
            return [self _stopDiscoveryWithError: "This device does not support Bluetooth Low Energy"];
        case CBManagerStateResetting:
            return [self _stopDiscoveryWithError: "Bluetooth is resetting"];
        case CBManagerStateUnknown:
        default:
            LogToAt(P2PLog, Warning, "Unknown Bluetooth state: %ld", long(central.state));
            return;
    }
}

static alloc_slice convert(id value) {
    if ([value isKindOfClass: [NSData class]]) {
        return alloc_slice((NSData*)value);
    } else if ([value isKindOfClass: [CBUUID class]]) {
        return alloc_slice(((CBUUID*)value).data);
    } else if ([value isKindOfClass: [NSArray class]]) {
        stringstream out;
        out << '[';
        for (id item in value) {
            if (out.gcount() > 1) out << ", ";
            if ([item isKindOfClass: [CBUUID class]])
                out << '<' << slice(((CBUUID*)item).data).hexString() << '>';
            else
                out << [item description].UTF8String;
        }
        out << ']';
        return alloc_slice(out.str());
    } else if ([value isKindOfClass: [NSDictionary class]]) {
        stringstream out;
        out << '[';
        for (id key in value) {
            if (out.gcount() > 1) out << ", ";
            out << key << ":";
            id item = value[key];
            if ([item isKindOfClass: [CBUUID class]])
                out << '<' << slice(((CBUUID*)item).data).hexString() << '>';
            else
                out << [item description].UTF8String;
        }
        out << '}';
        return alloc_slice(out.str());
    } else {
        return alloc_slice( [value description].UTF8String );
    }
}

- (void) centralManager: (CBCentralManager*)central
  didDiscoverPeripheral: (CBPeripheral*)peripheral
      advertisementData: (NSDictionary<NSString*,id>*)advert
                   RSSI: (NSNumber*)RSSI {
    string peerID = peripheral.identifier.UUIDString.UTF8String;
    if (RSSI.intValue < kMinimumRSSI || ![advert[CBAdvertisementDataIsConnectable] boolValue]) {
        LogToAt(P2PLog, Verbose, "Bluetooth peripheral %s RSSI too low: %d", peerID.c_str(), RSSI.intValue);
        [_manager cancelPeripheralConnection: peripheral];
        _counterpart->removePeer(peerID);
    } else {
        string displayName;
        C4Peer::Metadata metadata;
        if (id name = advert[CBAdvertisementDataLocalNameKey]) {
            displayName = [name UTF8String];
            metadata["localName"] = alloc_slice(displayName);
        }
        if (id man = advert[CBAdvertisementDataManufacturerDataKey])
            metadata["manufacturerData"] = convert(man);
        if (id man = advert[CBAdvertisementDataServiceDataKey])
            metadata["serviceData"] = convert(man);
        if (id uuids = advert[CBAdvertisementDataServiceUUIDsKey])
            metadata["serviceUUIDs"] = convert(uuids);
        if (id uuids = advert[CBAdvertisementDataOverflowServiceUUIDsKey])
            metadata["overflowServiceUUIDs"] = convert(uuids);
        if (id uuids = advert[CBAdvertisementDataSolicitedServiceUUIDsKey])
            metadata["solicitedServiceUUIDs"] = convert(uuids);
        if (id level = advert[CBAdvertisementDataTxPowerLevelKey])
            metadata["power"] = convert(level);
        //    if (RSSI != nil)
        //        metadata["RSSI"] = convert(RSSI); // this changes too often!

        if (peripheral.name)
            displayName = peripheral.name.UTF8String;

        if (auto peer = C4PeerDiscovery::peerWithID(peerID)) {
            peer->setMetadata(metadata);
        } else {
            LogToAt(P2PLog, Verbose, "Bluetooth peripheral %s has RSSI %d", peerID.c_str(), RSSI.intValue);
            peer = fleece::make_retained<BluetoothPeer>(_counterpart, peerID, displayName, metadata, peripheral);
            _counterpart->addPeer(peer);
        }
    }
}

static Retained<C4Peer> peerForPeripheral(CBPeripheral* peripheral) {
    return C4PeerDiscovery::peerWithID(peripheral.identifier.UUIDString.UTF8String);
}

static const char* idStr(CBPeripheral* p) {return p.identifier.UUIDString.UTF8String;}
static const char* idStr(CBUUID* p) {return p.description.UTF8String;}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
    _counterpart->_log(LogLevel::Info, "Did connect to peripheral %s", idStr(peripheral));
    peripheral.delegate = self;
    [peripheral discoverServices: _serviceUUIDs];
}

- (void) centralManager: (CBCentralManager*)central didFailToConnectPeripheral: (CBPeripheral*)peripheral
                  error: (NSError*)error
{
    _counterpart->_log(LogLevel::Info, "Failed to connect to peripheral %s: %s", idStr(peripheral), error.description.UTF8String);
    peripheral.delegate = nil;
}

- (void) centralManager: (CBCentralManager*)central didDisconnectPeripheral: (CBPeripheral*)peripheral
                  error: (NSError*)error
{
    _counterpart->_log(LogLevel::Info, "Did disconnect from peripheral %s (%s)", idStr(peripheral), error.description.UTF8String);
    peripheral.delegate = nil;
}

#pragma mark - CBPeripheralDelegate

- (void) peripheralDidUpdateName: (CBPeripheral*)peripheral {
    _counterpart->_log(LogLevel::Info, "Peripheral %s is now named \"%s\"", idStr(peripheral), peripheral.name.UTF8String);
    if (auto peer = peerForPeripheral(peripheral))
        peer->setDisplayName(peripheral.name.UTF8String);
}

- (void) peripheral: (CBPeripheral*)peripheral didDiscoverServices: (NSError*)error {
    if (error) {
        LogToAt(P2PLog,
                Info,
                "Error discovering services for peripheral %s: %s",
                idStr(peripheral),
                error.description.UTF8String);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    if (peripheral.services.count == 0) {
        _counterpart->_log(LogLevel::Info, "No services found for %s", idStr(peripheral));
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    for (CBService* service in peripheral.services) {
        _counterpart->_log(LogLevel::Info, "%s (%s) has service %s",
                           idStr(peripheral),
                           peripheral.name.UTF8String,
                           idStr(service.UUID));
        [peripheral discoverCharacteristics: nil forService: service];
    }
}

- (void) peripheral:(CBPeripheral*)peripheral didModifyServices:(NSArray<CBService*>*)invalidatedServices {
    for (CBService* service in invalidatedServices) {
        _counterpart->_log(LogLevel::Info, "%s (%s) invalidated service %s",
                           idStr(peripheral),
                           peripheral.name.UTF8String,
                           idStr(service.UUID));
    }
}

- (void) peripheral:(CBPeripheral*) peripheral didDiscoverCharacteristicsForService:(CBService*) service
              error:(NSError*) error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error discovering characteristics for peripheral %s: %s",
                           peripheral.name.UTF8String,
                           error.description.UTF8String);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    for (CBCharacteristic* ch in service.characteristics) {
        _counterpart->_log(LogLevel::Info, "%s (%s) service %s has characteristic %s",
                           idStr(peripheral),
                           peripheral.name.UTF8String,
                           idStr(service.UUID),
                           idStr(ch.UUID));
    }
}


@end

#endif  //___APPLE__
