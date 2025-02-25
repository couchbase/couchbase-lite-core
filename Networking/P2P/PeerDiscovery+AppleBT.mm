//
// Created by Jens Alfke on 2/24/25.
//

#ifdef __APPLE__

#    include "PeerDiscovery+AppleBT.hh"
#    include "Error.hh"
#    include "Logging.hh"
#    include "NetworkInterfaces.hh"
#    include "StringUtil.hh"
#include <dispatch/dispatch.h>

#undef DebugAssert
#include <CoreBluetooth/CoreBluetooth.h>

namespace litecore::p2p {
    using namespace std;

    extern LogDomain P2PLog;

    struct BluetoothProvider;

    static BluetoothProvider* sProvider;

    static constexpr int kMinimumRSSI = -80;


#    pragma mark - BLUETOOTH PEER:

    /** C4Peer subclass created by BluetoothProvider. */
    class BluetoothPeer : public C4Peer {
    public:
        BluetoothPeer(C4PeerDiscoveryProvider* provider, string const& id, string const& name, Metadata const& md, CBPeripheral* p)
        : C4Peer(provider, id, name, md)
        , _peripheral(p)
        {}

        void removed() override {
            C4Peer::removed();
            _peripheral = nil;
        }

        CBPeripheral* _peripheral {};
    };

}


using namespace std;
using namespace fleece;
using namespace litecore::p2p;

@interface LiteCoreBluetoothProvider : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*) peer
                               queue: (dispatch_queue_t)queue
                         serviceUUID: (NSString*)serviceUUID
                  characteristicUUID: (NSString*)charUUID;
- (void) startBrowsing;
- (void) stopBrowsing;
- (void) resolveAddresses: (Retained<BluetoothPeer>)peer;
@end


namespace litecore::p2p {

#    pragma mark - BROWSER IMPL:

    /// Implements DNS-SD peer discovery.
    /// This class owns a dispatch queue, and all calls other than constructor/destructor
    /// must be made on that queue.
    struct BluetoothProvider
        : public C4PeerDiscoveryProvider
        , public Logging {
        explicit BluetoothProvider(string const& serviceType)
            : C4PeerDiscoveryProvider("Bluetooth")
            , Logging(P2PLog)
            , _queue(dispatch_queue_create("LiteCore Bluetooth", DISPATCH_QUEUE_SERIAL))
            , _counterpart([[LiteCoreBluetoothProvider alloc] initWithCounterpart: this queue: _queue serviceUUID: @(serviceType.c_str()) characteristicUUID: nil])
            {
        }

        ~BluetoothProvider() override {
        }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ [_counterpart startBrowsing]; });
        }

        /// Provider callback that stops browsing for peers. */
        void stopBrowsing() override {
            dispatch_async(_queue, ^{ [_counterpart stopBrowsing]; });
        }

        /// Provider callback that starts or stops monitoring the metadata of a peer.
        void monitorMetadata(C4Peer* peer, bool start) override {
//            Retained<BluetoothPeer> bonjourPeer(dynamic_cast<BluetoothPeer*>(peer));
//            dispatch_async(_queue, ^{ do_monitor(bonjourPeer, start); });
        }

        /// Provider callback that requests addresses be resolved for a peer.
        /// This is a one-shot operation. The provider should call `resolvedAddresses` when done.
        void resolveAddresses(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_counterpart resolveAddresses: btPeer]; });
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

        void browseStateChanged(bool state, C4Error error = {}) {
            C4PeerDiscoveryProvider::browseStateChanged(state, error);
        }

        void publishStateChanged(bool state, C4Error error = {}) {
            C4PeerDiscoveryProvider::publishStateChanged(state, error);
        }

        fleece::Retained<C4Peer> addPeer(C4Peer* peer) {
            return C4PeerDiscoveryProvider::addPeer(peer);
        }

        bool removePeer(std::string_view id) {
            return C4PeerDiscoveryProvider::removePeer(id);
        }
        bool removePeer(C4Peer* peer) {
            return C4PeerDiscoveryProvider::removePeer(peer);
        }

      private:
        dispatch_queue_t const _queue;           // Dispatch queue I run on
        LiteCoreBluetoothProvider* _counterpart {};
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
    BluetoothProvider* _counterpart;
    dispatch_queue_t _queue;
    NSArray<CBUUID*>* _serviceUUIDs;
    CBUUID* _charUUID;

    CBCentralManager* _manager;
    NSMutableDictionary<CBPeripheral*, NSArray<CBCharacteristic*>*> *_peripherals;
}

- (instancetype) initWithCounterpart: (BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
                         serviceUUID: (NSString*)serviceUUID
                  characteristicUUID: (NSString*)charUUID
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
        if (serviceUUID.length)
            _serviceUUIDs = @[ [CBUUID UUIDWithString: serviceUUID] ];
        if (charUUID)
            _charUUID = [CBUUID UUIDWithString: charUUID];
        _peripherals = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void) startBrowsing {
    if (!_manager) {
        _manager = [[CBCentralManager alloc] initWithDelegate: self
                                                        queue: _queue
                                                      options: @{CBCentralManagerOptionShowPowerAlertKey: @YES}];
    }
    [self _startDiscovery];
}

- (void) _startDiscovery {
    if (!_manager.isScanning && _manager.state != CBManagerStateUnknown) {
        LogToAt(P2PLog, Info, "Scanning for BTLE peripherals");
        [_manager scanForPeripheralsWithServices: _serviceUUIDs options: nil];
    }
}

- (void) stopBrowsing {
    [self _stopDiscoveryWithError: nil];
}

- (void) _stopDiscoveryWithError: (const char*)error {
    if (!_manager) return;
    if (_manager.isScanning)
        [_manager stopScan];
    _manager = nil;
    C4Error c4err {};
    if (error)
        c4err = C4Error::make(LiteCoreDomain, kC4ErrorIOError, error);
    _counterpart->browseStateChanged(false, c4err);
}

- (void) resolveAddresses:(Retained<BluetoothPeer>)peer {
    if (peer->online())
        [_manager connectPeripheral: peer->_peripheral options: nil];
}

#pragma mark - CBCentralManagerDelegate

- (void) centralManagerDidUpdateState: (CBCentralManager *)central {
    switch (central.state) {
        case CBManagerStatePoweredOn:
            LogToAt(P2PLog, Verbose, "Bluetooth is on");
            [self _startDiscovery];
            break;
        case CBManagerStatePoweredOff:
            [self _stopDiscoveryWithError: "Bluetooth is off"];
            break;
        case CBManagerStateUnauthorized:
            [self _stopDiscoveryWithError: "Bluetooth access is not authorized. Check settings"];
            break;
        case CBManagerStateUnsupported:
            [self _stopDiscoveryWithError: "This device does not support Bluetooth Low Energy"];
            break;
        case CBManagerStateResetting:
            [self _stopDiscoveryWithError: "Bluetooth is resetting"];
            break;
        case CBManagerStateUnknown:
        default:
            LogToAt(P2PLog, Warning, "Unknown Bluetooth state: %ld", long(central.state));
            break;
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
    if (RSSI.intValue < kMinimumRSSI) {
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
        if (id connectable = advert[CBAdvertisementDataIsConnectable]; [connectable boolValue])
            metadata["connectable"] = alloc_slice("true");
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

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral {
    NSLog(@"%@: Did connect to peripheral : %@", self, peripheral.identifier.UUIDString);
    peripheral.delegate = self;
    [peripheral discoverServices: _serviceUUIDs];
}

- (void) centralManager: (CBCentralManager*)central didFailToConnectPeripheral: (CBPeripheral*)peripheral error: (NSError*)error {
    NSLog(@"%@: Failed to connect to peripheral : %@", self, peripheral.identifier.UUIDString);
    peripheral.delegate = nil;
}

- (void) centralManager: (CBCentralManager*)central didDisconnectPeripheral: (CBPeripheral*)peripheral error: (NSError*)error {
    NSLog(@"%@: Did disconnect from peripheral : %@", self, peripheral.identifier.UUIDString);
    peripheral.delegate = nil;
}

#pragma mark - CBPeripheralDelegate

- (void) peripheral: (CBPeripheral*)peripheral didDiscoverServices: (NSError*)error {
    if (error) {
        NSLog(@"%@: Error discovering services for peripheral %@: %@", self, peripheral.name, error);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    if (peripheral.services.count == 0) {
        NSLog(@"%@: No services found for %@", self, peripheral.name);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    for (CBService* service in peripheral.services) {
        NSLog(@"%@: %@ has service %@", self, peripheral.name, service);
        [peripheral discoverCharacteristics: nil/*@[_charUUID]*/ forService: service];
    }
}

- (void) peripheral:(CBPeripheral *) peripheral didDiscoverCharacteristicsForService:(CBService *) service
              error:(NSError *) error {
    if (error) {
        NSLog(@"%@: Error discovering characteristics for peripheral %@: %@", self, peripheral.name, error);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    for (CBCharacteristic* ch in service.characteristics) {
        NSLog(@"%@: %@ service %@ has characteristic %@", self, peripheral.name, service.UUID, ch);
        //[peripheral discoverCharacteristics: @[_charUUID] forService: service];
    }
}


@end

#endif  //___APPLE__
