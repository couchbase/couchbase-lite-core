//
// Created by Jens Alfke on 2/24/25.
//

#ifdef __APPLE__

#include "PeerDiscovery+AppleBT.hh"
#include "PeerDiscovery+AppleDNSSD.hh"
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

    // My service UUID
    static constexpr const char* kP2PServiceID              = "15BB6CAE-6B6A-4CB3-B83F-A9826AE44155";
    // Service characteristic whose value is the L2CAP port (PSM) the peeri s listening on
    static constexpr const char* kPortCharacteristicID      = "ABDD3056-28FA-441D-A470-55A75A52553A";
    // Service characteristic whose value is the peer's Fleece-encoded metadata
    static constexpr const char* kMetadataCharacteristicID  = "936D7669-E532-42BF-8B8D-97E3C1073F74";

    // Note: kPortCharacteristicID's value comes from <CoreBluetooth/CBUUID.h>:
    //    * The PSM (a little endian uint16_t) of an L2CAP Channel associated with the GATT service
    //    * containing this characteristic.  Servers can publish this characteristic with the UUID
    //    * ABDD3056-28FA-441D-A470-55A75A52553A

    static constexpr int kMinimumRSSI = -80;    // Discovery ignores peripherals with signal lower than this


    extern LogDomain P2PLog;

    struct BluetoothProvider;
    static BluetoothProvider* sProvider;


    /** C4Peer subclass created by BluetoothProvider. */
    class BluetoothPeer : public C4Peer {
    public:
        BluetoothPeer(C4PeerDiscoveryProvider* provider, string const& id, string const& name,CBPeripheral* p)
        :C4Peer(provider, id, name)
        ,_peripheral(p)
        { }

        bool respondWithURL() {
            if (!_peripheral || !_port) return false;
            net::Address addr("l2cap", id, _port, "/db");
            resolvedURL(string(addr.url()), {});
            _resolvingURL = false;
            return true;
        }

        void removed() override {
            C4Peer::removed();
            _peripheral = nil;
        }

        CBPeripheral*   _peripheral {};
        CBL2CAPPSM      _port {};
        bool            _resolvingURL = false;
    };

}


@interface LiteCoreBTCentral : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*)counterpart
                               queue: (dispatch_queue_t)queue
                         allServices: (bool)allServices;
- (void) startBrowsing;
- (void) stopBrowsing;
- (void) monitorPeer: (Retained<BluetoothPeer>)peer state: (bool)state;
- (void) resolveURL: (Retained<BluetoothPeer>)peer;
- (void) cancelResolveURL: (Retained<BluetoothPeer>)peer;
@end


@interface LiteCoreBTPeripheral : NSObject <CBPeripheralManagerDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*)counterpart
                               queue: (dispatch_queue_t)queue;
- (void) publish: (string)name port: (uint16_t)port metadata: (C4Peer::Metadata)md;
- (void) unpublish;
- (void) updateMetadata: (C4Peer::Metadata)md;
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
            _central    = [[LiteCoreBTCentral alloc] initWithCounterpart: this
                                                                   queue: _queue
                                                             allServices: serviceType.empty()];
            _peripheral = [[LiteCoreBTPeripheral alloc] initWithCounterpart: this queue: _queue];
        }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ [_central startBrowsing]; });
        }

        void stopBrowsing() override {
            dispatch_async(_queue, ^{ [_central stopBrowsing]; });
        }

        void monitorMetadata(C4Peer* peer, bool start) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_central monitorPeer: btPeer state: start]; });
        }

        void resolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_central resolveURL: btPeer]; });
        }

        void cancelResolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_central cancelResolveURL: btPeer]; });
        }

        void publish(std::string_view displayName, uint16_t port, C4Peer::Metadata const& meta) override {
            string           nameStr(displayName);
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ [_peripheral publish: std::move(nameStr) port: port metadata: std::move(metaCopy)]; });
        }

        void unpublish() override {
            dispatch_async(_queue, ^{ [_peripheral unpublish]; });
        }

        void updateMetadata(C4Peer::Metadata const& meta) override {
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ [_peripheral updateMetadata: std::move(metaCopy)]; });
        }

        //---- Inherited methods redeclared as public so the Obj-C class can call them:
        void _log(LogLevel level, const char* format, ...) const __printflike(3, 4) { LOGBODY_(level) }
        void browseStateChanged(bool s, C4Error e = {})     {C4PeerDiscoveryProvider::browseStateChanged(s, e);}
        void publishStateChanged(bool s, C4Error e = {})    {C4PeerDiscoveryProvider::publishStateChanged(s, e);}
        fleece::Retained<C4Peer> addPeer(C4Peer* peer)      {return C4PeerDiscoveryProvider::addPeer(peer);}
        bool removePeer(std::string_view id)                {return C4PeerDiscoveryProvider::removePeer(id);}
        bool removePeer(C4Peer* peer)                       {return C4PeerDiscoveryProvider::removePeer(peer);}

      private:
        dispatch_queue_t const  _queue;             // Dispatch queue I run on
        LiteCoreBTCentral*      _central {};        // Obj-C instance that does the real discovery
        LiteCoreBTPeripheral*   _peripheral {};     // Obj-C instance that does the real publishing
    };

    void InitializeBluetoothProvider(string_view serviceType) {
        static once_flag sOnce;
        call_once(sOnce, [&] {
            sProvider = new BluetoothProvider(string(serviceType));
            C4PeerDiscovery::registerProvider(sProvider);
        });
    }


    static C4Error c4errorFrom(const char* error) {
        if (!error) return kC4NoError;
        return C4Error::make(LiteCoreDomain, kC4ErrorIOError, error);  //TODO: Real error
    }

    static C4Error c4errorFrom(NSError* error) {
        if (!error) return kC4NoError;
        return C4Error::make(LiteCoreDomain, kC4ErrorIOError, error.description.UTF8String);  //TODO: Real error
    }

    static CBUUID* mkUUID(const char* str)      {return [CBUUID UUIDWithString: @(str)];}
    static const char* idStr(CBUUID* p)         {return p.description.UTF8String;}
    static const char* idStr(NSUUID* p)         {return p.description.UTF8String;}
    static const char* idStr(CBAttribute* attr) {return idStr(attr.UUID);}
    static const char* idStr(CBPeripheral* p)   {return idStr(p.identifier);}

    static Retained<BluetoothPeer> peerForPeripheral(CBPeripheral* p) {
        Retained<C4Peer> peer = C4PeerDiscovery::peerWithID(idStr(p));
        return Retained(dynamic_cast<BluetoothPeer*>(peer.get()));
    }

}  // namespace litecore::p2p


#pragma mark - OBJECTIVE-C CENTRAL MANAGER:


@implementation LiteCoreBTCentral
{
    BluetoothProvider*  _counterpart;
    dispatch_queue_t    _queue;
    NSArray<CBUUID*>*   _serviceUUIDs;
    CBCentralManager*   _manager;
}

- (instancetype) initWithCounterpart: (BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
                         allServices: (bool)allServices
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
        if (!allServices)
            _serviceUUIDs = @[ mkUUID(kP2PServiceID) ];
    }
    return self;
}

- (void) startBrowsing {
    if (!_manager) {
        _counterpart->_log(LogLevel::Verbose, "Starting browsing");
        _manager = [[CBCentralManager alloc] initWithDelegate: self
                                                        queue: _queue
                                                      options: @{CBCentralManagerOptionShowPowerAlertKey: @YES}];
    }
    [self _startDiscovery];
}

- (void) _startDiscovery {
    if (!_manager.isScanning && _manager.state == CBManagerStatePoweredOn) {
        _counterpart->_log(LogLevel::Verbose, "Scanning for BLE peripherals");
        [_manager scanForPeripheralsWithServices: _serviceUUIDs options: nil];
        _counterpart->browseStateChanged(true);
    }
}

- (void) stopBrowsing {
    _counterpart->_log(LogLevel::Verbose, "Stopping browsing");
    [self _stopDiscoveryWithError: nil];
}

- (void) _stopDiscoveryWithError: (const char*)error {
    if (_manager.isScanning)
        [_manager stopScan];
    _manager = nil;
    _counterpart->browseStateChanged(false, c4errorFrom(error));
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

- (void) resolveURL: (Retained<BluetoothPeer>)peer {
    if (!peer->respondWithURL()) {
        peer->_resolvingURL = true;
        [_manager connectPeripheral: peer->_peripheral options: nil];
    }
}

- (void) cancelResolveURL: (Retained<BluetoothPeer>)peer {
    peer->_resolvingURL = false;
}


- (void) connectToPeer: (Retained<BluetoothPeer>)peer {
    if (auto peripheral = peer->_peripheral; peripheral && peer->_port)
        [peripheral openL2CAPChannel: peer->_port];
}


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
            _counterpart->_log(LogLevel::Warning, "Unknown Bluetooth state: %ld", long(central.state));
            return;
    }
}

- (void) centralManager: (CBCentralManager*)central
  didDiscoverPeripheral: (CBPeripheral*)peripheral
      advertisementData: (NSDictionary<NSString*,id>*)advert
                   RSSI: (NSNumber*)RSSI {
    string peerID = idStr(peripheral);
    if (RSSI.intValue < kMinimumRSSI || ![advert[CBAdvertisementDataIsConnectable] boolValue]) {
        _counterpart->_log(LogLevel::Debug, "Bluetooth peripheral %s RSSI too low: %d", peerID.c_str(), RSSI.intValue);
        [_manager cancelPeripheralConnection: peripheral];
        _counterpart->removePeer(peerID);
    } else if (!C4PeerDiscovery::peerWithID(peerID)) {
        string displayName;
        if (peripheral.name)
            displayName = peripheral.name.UTF8String;
        else if (id name = advert[CBAdvertisementDataLocalNameKey])
            displayName = [name UTF8String];

        _counterpart->_log(LogLevel::Verbose, "peripheral %s has RSSI %d", peerID.c_str(), RSSI.intValue);
        auto peer = fleece::make_retained<BluetoothPeer>(_counterpart, peerID, displayName, peripheral);
        _counterpart->addPeer(peer);
    }
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
    _counterpart->_log(LogLevel::Info, "Connected to peripheral %s", idStr(peripheral));
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
    _counterpart->_log(LogLevel::Verbose, "Peripheral %s is now named \"%s\"", idStr(peripheral), peripheral.name.UTF8String);
    if (auto peer = peerForPeripheral(peripheral))
        peer->setDisplayName(peripheral.name.UTF8String);
}

- (void) peripheral: (CBPeripheral*)peripheral didDiscoverServices: (NSError*)error {
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error discovering services for peripheral %s: %s",
                idStr(peripheral), error.description.UTF8String);
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    bool found = false;
    for (CBService* service in peripheral.services) {
        _counterpart->_log(LogLevel::Verbose, "%s (%s) has service %s",
                           idStr(peripheral), peripheral.name.UTF8String, idStr(service.UUID));
        if ([service.UUID isEqual: mkUUID(kP2PServiceID)]) {
            _counterpart->_log(LogLevel::Verbose, "%s (%s) has P2P service!",
                               idStr(peripheral), peripheral.name.UTF8String);
            [peripheral discoverCharacteristics: @[mkUUID(kPortCharacteristicID), mkUUID(kMetadataCharacteristicID)]
                                     forService: service];
            found = true;
        }
    }
    if (!found)
        [_manager cancelPeripheralConnection: peripheral];
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
        _counterpart->_log(LogLevel::Verbose, "%s (%s) service %s has characteristic %s",
                           idStr(peripheral), peripheral.name.UTF8String, idStr(service.UUID), idStr(ch.UUID));
        bool isMD = [ch.UUID isEqual: mkUUID(kMetadataCharacteristicID)];
        if (isMD || [ch.UUID isEqual: mkUUID(kPortCharacteristicID)]) {
            [peripheral readValueForCharacteristic: ch];
            if (isMD)
                [peripheral setNotifyValue: YES forCharacteristic: ch];
        }
    }
}

- (void) peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)ch
              error:(nullable NSError *)error
{
    Retained<BluetoothPeer> peer = peerForPeripheral(peripheral);
    if (!peer)
        return;
    if ([ch.UUID isEqual: mkUUID(kPortCharacteristicID)]) {
        if (ch.value.length == sizeof(CBL2CAPPSM)) {
            memcpy(&peer->_port, ch.value.bytes, sizeof(CBL2CAPPSM));
            _counterpart->_log(LogLevel::Info, "%s (%s) listens on port/PSM %u",
                               idStr(peripheral), peripheral.name.UTF8String, peer->_port);
            if (peer->_resolvingURL)
                peer->respondWithURL();
        } else {
            _counterpart->_log(LogLevel::Warning, "%s (%s) has invalid port/PSM data (%zu bytes)",
                               idStr(peripheral), peripheral.name.UTF8String, ch.value.length);
        }
    } else if ([ch.UUID isEqual: mkUUID(kMetadataCharacteristicID)]) {
        _counterpart->_log(LogLevel::Info, "%s (%s) has metadata (%zu bytes)",
                           idStr(peripheral), peripheral.name.UTF8String, ch.value.length);
        peer->setMetadata(DecodeTXTToMetadata(slice(ch.value)));
    }
}

@end


#pragma mark - OBJECTIVE-C PERIPHERAL MANAGER:


@implementation LiteCoreBTPeripheral
{
    BluetoothProvider*  _counterpart;
    dispatch_queue_t    _queue;
    NSString* _name;
    uint16_t _port;
    C4Peer::Metadata _metadata;
    CBUUID*   _serviceUUID;
    CBPeripheralManager*   _manager;
    CBManagerState _managerState;
    CBMutableCharacteristic* _portCharacteristic;
    CBMutableCharacteristic* _metadataCharacteristic;
}

- (instancetype) initWithCounterpart: (nonnull BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
        _serviceUUID = mkUUID(kP2PServiceID);
    }
    return self;
}

- (void) publish: (string)name port: (uint16_t)port metadata: (C4Peer::Metadata)md {
    _counterpart->_log(LogLevel::Verbose, "Starting publishing...");
    _name = @(name.c_str());
    _port = port;
    _metadata = md;
    if (!_manager) {
        _manager = [[CBPeripheralManager alloc] initWithDelegate: self
                                                           queue: _queue
                                                         options: @{CBPeripheralManagerOptionShowPowerAlertKey: @YES}];
    }
    [self _startAdvertise];
}

- (void) unpublish {
    [self _stopAdvertise: nullptr];
    _manager = nil;
}

- (void) updateMetadata: (C4Peer::Metadata)md {
    _metadata = std::move(md);
}

- (void) _startAdvertise {
    if (_manager.isAdvertising || _managerState == CBManagerStateUnknown)
        return;

    _portCharacteristic = [[CBMutableCharacteristic alloc] initWithType: mkUUID(kPortCharacteristicID)
                                                             properties: CBCharacteristicPropertyRead
                                                                  value: nil
                                                            permissions: CBAttributePermissionsReadable];
    _metadataCharacteristic = [[CBMutableCharacteristic alloc] initWithType: mkUUID(kMetadataCharacteristicID)
                                                                 properties: CBCharacteristicPropertyRead |
                                                                                CBCharacteristicPropertyNotify
                                                                      value: nil
                                                                permissions: CBAttributePermissionsReadable];

    CBMutableService *service = [[CBMutableService alloc] initWithType: _serviceUUID primary: YES];
    service.characteristics = @[ _portCharacteristic, _metadataCharacteristic];
    [_manager removeAllServices];
    [_manager addService: service];

    [_manager startAdvertising: @{
        CBAdvertisementDataLocalNameKey: _name,
        CBAdvertisementDataServiceUUIDsKey: @[_serviceUUID]
    }];

    _port = 0;
    [_manager publishL2CAPChannelWithEncryption: YES];
}

- (void) _stopAdvertise: (const char*)errorMessage {
    if (_manager.isAdvertising) {
        [_manager stopAdvertising];
        if (_port != 0) {
            [_manager unpublishL2CAPChannel: _port];
            _port = 0;
        }
        _counterpart->publishStateChanged(false, c4errorFrom(errorMessage));
    }
}

#pragma mark - CBPeripheralManagerDelegate

- (void) peripheralManagerDidUpdateState: (nonnull CBPeripheralManager*)peripheral {
    _managerState = peripheral.state;
    switch (peripheral.state) {
        case CBManagerStatePoweredOn:
            _counterpart->_log(LogLevel::Verbose, "Bluetooth is on!");
            [self _startAdvertise];
            break;
        case CBManagerStatePoweredOff:
            return [self _stopAdvertise: "Bluetooth is off"];
        case CBManagerStateUnauthorized:
            return [self _stopAdvertise: "Bluetooth access is not authorized. Check settings"];
        case CBManagerStateUnsupported:
            return [self _stopAdvertise: "This device does not support Bluetooth LE"];
        case CBManagerStateResetting:
            return [self _stopAdvertise: "Bluetooth is resetting"];
        case CBManagerStateUnknown:
        default:
            _counterpart->_log(LogLevel::Info, "Unknown Bluetooth state.");
    }
}

- (void) peripheralManagerDidStartAdvertising: (CBPeripheralManager*)peripheral
                                        error: (nullable NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error advertising: %s", error.description.UTF8String);
        _counterpart->publishStateChanged(false, c4errorFrom(error));
    } else {
        _counterpart->_log(LogLevel::Verbose, "Advertising");
    }
}

- (void) peripheralManager: (CBPeripheralManager*)peripheral
             didAddService: (CBService*)service
                     error: (nullable NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error when adding service: %s", error.description.UTF8String);
        [self _stopAdvertise: error.description.UTF8String];
    } else {
        _counterpart->_log(LogLevel::Verbose, "Advertising service");
    }
}

- (void) peripheralManager:(CBPeripheralManager *)peripheral
     didReceiveReadRequest:(CBATTRequest *)request
{
    _counterpart->_log(LogLevel::Info, "Read request for characteristic %s offset %zu",
                       idStr(request.characteristic), size_t(request.offset));
    CBATTError result = CBATTErrorAttributeNotFound;
    if (request.offset != 0) {
        result = CBATTErrorInvalidOffset;
    } else if (request.characteristic == _portCharacteristic) {
        request.value = [NSData dataWithBytes: &_port length: sizeof(_port)];
        result = CBATTErrorSuccess;
    } else if (request.characteristic == _metadataCharacteristic) {
        alloc_slice txt = EncodeMetadataAsTXT(_metadata);
        request.value = txt.copiedNSData();
        result = CBATTErrorSuccess;
    }
    [peripheral respondToRequest: request withResult: result];
}

- (void) peripheralManager:(CBPeripheralManager *)peripheral
    didPublishL2CAPChannel:(CBL2CAPPSM)PSM
                     error:(nullable NSError *)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error publishing L2CAP channel: %s", error.description.UTF8String);
        [self _stopAdvertise: error.description.UTF8String];
    } else {
        _counterpart->_log(LogLevel::Info, "Published L2CAP channel on port/PSM %u", PSM);
        _port = PSM;

        [_manager updateValue: [NSData dataWithBytes: &_port length: sizeof(_port)]
            forCharacteristic: _portCharacteristic
         onSubscribedCentrals: nil];

        _counterpart->publishStateChanged(true);
    }
}

- (void) peripheralManager:(CBPeripheralManager *)peripheral
       didOpenL2CAPChannel:(nullable CBL2CAPChannel *)channel
                     error:(nullable NSError *)error
{
    _counterpart->_log(LogLevel::Info, "Incoming L2CAP connection!");
    // TODO: Handle it!
}




@end

#endif  //___APPLE__
