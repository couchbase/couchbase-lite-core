//
// AppleBluetoothPeer.mm
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef __APPLE__

#include "AppleBluetoothPeer.hh"
#include "AppleBonjourPeer.hh"
#include "Address.hh"
#include "AppleBTSocketFactory.hh"
#include "c4Socket.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <dispatch/dispatch.h>

#undef DebugAssert // this macro conflicts with something in Apple headers
#include <CoreBluetooth/CoreBluetooth.h>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::p2p;

namespace litecore::p2p {
    // Couchbase Lite P2P sync service UUID
    static constexpr const char* kP2PServiceID              = "15BB6CAE-6B6A-4CB3-B83F-A9826AE44155";
    // Service characteristic whose value is the L2CAP port (PSM) the peeri s listening on
    static constexpr const char* kPortCharacteristicID      = "ABDD3056-28FA-441D-A470-55A75A52553A";
    // Service characteristic whose value is the peer's Fleece-encoded metadata
    static constexpr const char* kMetadataCharacteristicID  = "936D7669-E532-42BF-8B8D-97E3C1073F74";

    // Note: kPortCharacteristicID's value comes from <CoreBluetooth/CBUUID.h>:
    //    * The PSM (a little endian uint16_t) of an L2CAP Channel associated with the GATT service
    //    * containing this characteristic.  Servers can publish this characteristic with the UUID
    //    * ABDD3056-28FA-441D-A470-55A75A52553A

    static constexpr int kConnectableRSSI =  -80;       // Below this, peer's connectable is set to false
    static constexpr int kMinimumRSSI     = -100;       // Below this, peer is ignored or removed


    extern LogDomain P2PLog;

    class BluetoothPeer;
    struct BluetoothProvider;
}


@interface LiteCoreBTCentral : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*)counterpart queue: (dispatch_queue_t)queue;
- (void) startBrowsing;
- (void) stopBrowsing;
- (void) monitorPeer: (Retained<BluetoothPeer>)peer state: (bool)state;
- (void) resolveURL: (Retained<BluetoothPeer>)peer;
- (void) cancelResolveURL: (Retained<BluetoothPeer>)peer;
- (void) connect: (Retained<BluetoothPeer>)peer;
- (void) cancelConnect: (Retained<BluetoothPeer>)peer;
@end


@interface LiteCoreBTPeripheral : NSObject <CBPeripheralManagerDelegate>
- (instancetype) initWithCounterpart: (BluetoothProvider*)counterpart queue: (dispatch_queue_t)queue;
- (void) publish: (const char*)name metadata: (C4Peer::Metadata)md;
- (void) unpublish;
- (void) updateMetadata: (C4Peer::Metadata)md;
@end


namespace litecore::p2p {

#pragma mark - PEER:

    /** C4Peer subclass created by BluetoothProvider. */
    class BluetoothPeer : public C4Peer {
    public:
        BluetoothPeer(C4PeerDiscoveryProvider* provider, string const& id, string const& name, CBPeripheral* p)
        :C4Peer(provider, id, name)
        ,_peripheral(p)
        {
            if (!_peripheral)
                setConnectable(false);
        }

        void respondWithURL() {
            net::Address addr(kBTURLScheme, id, 0, "");
            resolvedURL(string(addr.url()), kC4NoError);
        }

        void removed() override {
            C4Peer::removed();
            _peripheral = nil;
            _psm = 0;
            _connectState = NotConnecting;
        }

        enum ConnectState {
            NotConnecting,
            WaitingForConnectable,
            GettingPSM,
            OpeningChannel,
        };

        CBPeripheral*   _peripheral {};         // CoreBluetooth object representing this peer (could be nil)
        CBL2CAPPSM      _psm {};                // BTLE "port number" peer is listening on; 0 if unknown
        ConnectState    _connectState = NotConnecting;
    };


#pragma mark - PROVIDER:

    static BluetoothProvider* sProvider;

    /** Implements Bluetooth LE peer discovery using CoreBluetooth. */
    struct BluetoothProvider : public C4PeerDiscoveryProvider, public Logging {

        explicit BluetoothProvider(string const& serviceType)
        :C4PeerDiscoveryProvider("Bluetooth")
        ,Logging(P2PLog)
        ,_queue(dispatch_queue_create("LiteCore Bluetooth", DISPATCH_QUEUE_SERIAL))
        ,_myCentral([[LiteCoreBTCentral alloc] initWithCounterpart: this queue: _queue])
        ,_myPeripheral([[LiteCoreBTPeripheral alloc] initWithCounterpart: this queue: _queue])
        { }

        void startBrowsing() override {
            dispatch_async(_queue, ^{ [_myCentral startBrowsing]; });
        }

        void stopBrowsing() override {
            dispatch_async(_queue, ^{ [_myCentral stopBrowsing]; });
        }

        void monitorMetadata(C4Peer* peer, bool start) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_myCentral monitorPeer: btPeer state: start]; });
        }

        void resolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_myCentral resolveURL: btPeer]; });
        }

        void cancelResolveURL(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_myCentral cancelResolveURL: btPeer]; });
        }

        C4SocketFactory const* getSocketFactory() const override {
            return &BTSocketFactory;
        }

        void connect(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_myCentral connect: btPeer]; });
        }

        void cancelConnect(C4Peer* peer) override {
            Retained<BluetoothPeer> btPeer(dynamic_cast<BluetoothPeer*>(peer));
            dispatch_async(_queue, ^{ [_myCentral cancelConnect: btPeer]; });
        }

        void publish(std::string_view displayName, uint16_t /*port*/, C4Peer::Metadata const& meta) override {
            string nameCopy(displayName);
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ [_myPeripheral publish: nameCopy.c_str() metadata: std::move(metaCopy)]; });
        }

        void unpublish() override {
            dispatch_async(_queue, ^{ [_myPeripheral unpublish]; });
        }

        void updateMetadata(C4Peer::Metadata const& meta) override {
            C4Peer::Metadata metaCopy = meta;
            dispatch_async(_queue, ^{ [_myPeripheral updateMetadata: std::move(metaCopy)]; });
        }

        void shutdown(std::function<void()> onComplete) override {
            dispatch_async(_queue, ^{
                [_myCentral stopBrowsing];
                [_myPeripheral unpublish];
                onComplete();
            });
        }

        ~BluetoothProvider() {
            if (this == sProvider)
                sProvider = nullptr;
        }

        //---- Inherited methods redeclared as public so the Obj-C classes below can call them:
        using super = C4PeerDiscoveryProvider;
        void _log(LogLevel level, const char* format, ...) const __printflike(3, 4) { LOGBODY_(level) }
        void browseStateChanged(bool s, C4Error e = {})         {super::browseStateChanged(s, e);}
        void publishStateChanged(bool s, C4Error e = {})        {super::publishStateChanged(s, e);}
        fleece::Retained<C4Peer> addPeer(C4Peer* peer)          {return super::addPeer(peer);}
        bool removePeer(std::string_view id)                    {return super::removePeer(id);}
        bool removePeer(C4Peer* peer)                           {return super::removePeer(peer);}
        bool notifyIncomingConnection(C4Peer* p, C4Socket* s)   {return super::notifyIncomingConnection(p,s);}

      private:
        dispatch_queue_t const      _queue;             // Dispatch queue I run on
        LiteCoreBTCentral* const    _myCentral {};      // Obj-C instance that does the real discovery
        LiteCoreBTPeripheral* const _myPeripheral {};   // Obj-C instance that does the real publishing
    };

    
    void InitializeBluetoothProvider(string_view serviceType) {
        Assert(!sProvider);
        sProvider = new BluetoothProvider(string(serviceType));
        sProvider->registerProvider();
    }


    static const char* errorStr(NSError* err)   {return err ? err.description.UTF8String : "(no error)";}

    static C4Error c4errorFrom(const char* error) {
        if (!error) return kC4NoError;
        return C4Error::make(LiteCoreDomain, kC4ErrorIOError, error);  //TODO: Real error
    }

    static C4Error c4errorFrom(NSError* error) {
        if (!error) return kC4NoError;
        return C4Error::make(LiteCoreDomain, kC4ErrorIOError, errorStr(error));  //TODO: Real error
    }

    static CBUUID* mkUUID(const char* str)      {return [CBUUID UUIDWithString: @(str)];}
    static const char* idStr(NSUUID* p)         {return p.description.UTF8String;}
    static const char* idStr(CBUUID* p)         {return p.description.UTF8String;}
    static const char* idStr(CBAttribute* attr) {return idStr(attr.UUID);}
    static const char* idStr(CBPeer* p)         {return idStr(p.identifier);}

    static Retained<BluetoothPeer> peerForPeripheral(CBPeer* p) {
        Retained<C4Peer> peer = C4PeerDiscovery::peerWithID(idStr(p));
        return Retained(dynamic_cast<BluetoothPeer*>(peer.get()));
    }

}  // namespace litecore::p2p


#pragma mark - OBJECTIVE-C CENTRAL MANAGER:


@implementation LiteCoreBTCentral
{
    BluetoothProvider*  _counterpart;   // The C++ C4PeerDiscoveryProvider
    dispatch_queue_t    _queue;         // The dispatch queue I'm called on
    NSArray<CBUUID*>*   _serviceUUIDs;  // Service UUIDs I watch for; nil for any
    CBCentralManager*   _manager;       // CoreBluetooth manager
}

- (instancetype) initWithCounterpart: (BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
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


- (void) monitorPeer: (Retained<BluetoothPeer>)peer
               state: (bool)state
{
    Assert(_manager);
    if (auto peripheral = peer->_peripheral; peripheral && peer->connectable()) {
        if (state)
            [_manager connectPeripheral: peripheral options: nil];
        else
            [_manager cancelPeripheralConnection: peripheral];
    } else {
        _counterpart->_log(LogLevel::Warning, "Can't monitor Bluetooth peer %s, it's not connectable", peer->id.c_str());
    }
}


- (void) resolveURL: (Retained<BluetoothPeer>)peer {
    peer->respondWithURL();
}

- (void) cancelResolveURL: (Retained<BluetoothPeer>)peer { }


- (void) connect: (Retained<BluetoothPeer>)peer {
    if (peer->_connectState > BluetoothPeer::NotConnecting)
        return;
    if (!peer->online())
        peer->connected(nullptr, {NetworkDomain, kC4NetErrHostUnreachable});
    peer->_connectState = BluetoothPeer::WaitingForConnectable;
    [self _tryConnect: peer];
}

- (void) _tryConnect: (BluetoothPeer*)peer {
    if (peer->_connectState == BluetoothPeer::WaitingForConnectable) {
        if (auto peripheral = peer->_peripheral; peripheral && peer->connectable()) {
            if (peer->_psm) {
                // I know the PSM, so I can connect:
                peer->_connectState = BluetoothPeer::OpeningChannel;
                [peripheral openL2CAPChannel: peer->_psm];
            } else {
                // I don't know the PSM, so I have to connect in order to read it from a Characteristic:
                peer->_connectState = BluetoothPeer::GettingPSM;
                [_manager connectPeripheral: peer->_peripheral options: nil];
            }
        }
    }
}


- (void) cancelConnect: (Retained<BluetoothPeer>)peer {
    peer->_connectState = BluetoothPeer::NotConnecting;
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
    auto peer = peerForPeripheral(peripheral);
    if (RSSI.intValue >= kConnectableRSSI && [advert[CBAdvertisementDataIsConnectable] boolValue]) {
        // Peer is connectable:
        NSString* displayName = peripheral.name ?: advert[CBAdvertisementDataLocalNameKey];
        if (peer) {
            if (peer->_peripheral == nil)
                peer->_peripheral = peripheral;
            if (displayName)
                peer->setDisplayName(displayName.UTF8String);
            if (!peer->connectable()) {
                peer->setConnectable(true);
                if (peer->_connectState == BluetoothPeer::WaitingForConnectable)
                    [self _tryConnect: peer];
            }
        } else {
            if (!displayName) displayName = @"";
            peer = fleece::make_retained<BluetoothPeer>(_counterpart,
                                                        idStr(peripheral),
                                                        displayName.UTF8String,
                                                        peripheral);
            _counterpart->addPeer(peer);
        }
    } else if (peer) {
        // Peer not connectable:
        if (RSSI.intValue >= kMinimumRSSI || peer->_connectState > BluetoothPeer::NotConnecting) {
            peer->setConnectable(false);
        } else {
            // In fact the signal is so weak, let's forget this peer:
            [peer->_peripheral setDelegate: nil];
            peer->_peripheral = nil;
            [_manager cancelPeripheralConnection: peripheral];
            _counterpart->removePeer(peer);
        }
    }
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral
{
    _counterpart->_log(LogLevel::Info, "Connected to peripheral %s", idStr(peripheral));
    peripheral.delegate = self;
    [peripheral discoverServices: _serviceUUIDs];
}

- (void) centralManager: (CBCentralManager*)central
didFailToConnectPeripheral: (CBPeripheral*)peripheral
                  error: (NSError*)error
{
    _counterpart->_log(LogLevel::Warning, "Failed to connect to peripheral %s: %s", idStr(peripheral), errorStr(error));
    peripheral.delegate = nil;
}

- (void) centralManager: (CBCentralManager*)central
didDisconnectPeripheral: (CBPeripheral*)peripheral
                  error: (NSError*)error
{
    _counterpart->_log(LogLevel::Info, "Disconnected from peripheral %s (%s)", idStr(peripheral), errorStr(error));
    peripheral.delegate = nil;
}


#pragma mark - CBPeripheralDelegate


- (void) peripheralDidUpdateName: (CBPeripheral*)peripheral {
    _counterpart->_log(LogLevel::Verbose, "Peripheral %s is now named \"%s\"",
                       idStr(peripheral), peripheral.name.UTF8String);
    if (auto peer = peerForPeripheral(peripheral))
        peer->setDisplayName(peripheral.name.UTF8String);
}

- (void) peripheral: (CBPeripheral*)peripheral
didDiscoverServices: (NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error discovering services for peripheral %s: %s",
                           idStr(peripheral), errorStr(error));
        [_manager cancelPeripheralConnection: peripheral];
        return;
    }

    bool found = false;
    for (CBService* service in peripheral.services) {
        if ([service.UUID isEqual: mkUUID(kP2PServiceID)]) {
            _counterpart->_log(LogLevel::Verbose, "%s (%s) has P2P service!",
                               idStr(peripheral), peripheral.name.UTF8String);
            [peripheral discoverCharacteristics: @[mkUUID(kPortCharacteristicID), mkUUID(kMetadataCharacteristicID)]
                                     forService: service];
            found = true;
        }
    }
    if (!found) {
        _counterpart->_log(LogLevel::Warning, "Peripheral %s doesn't have P2P service", idStr(peripheral));
        [_manager cancelPeripheralConnection: peripheral];
    }
}

- (void) peripheral:(CBPeripheral*)peripheral
  didModifyServices:(NSArray<CBService*>*)invalidatedServices
{
    for (CBService* service in invalidatedServices) {
        _counterpart->_log(LogLevel::Info, "%s (%s) invalidated service %s",
                           idStr(peripheral),
                           peripheral.name.UTF8String,
                           idStr(service.UUID));
    }
}

- (void) peripheral:(CBPeripheral*) peripheral
didDiscoverCharacteristicsForService:(CBService*) service
              error:(NSError*) error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error discovering characteristics for peripheral %s: %s",
                           peripheral.name.UTF8String,
                           errorStr(error));
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

- (void) peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)ch
              error:(nullable NSError*)error
{
    Retained<BluetoothPeer> peer = peerForPeripheral(peripheral);
    if (!peer)
        return;
    if ([ch.UUID isEqual: mkUUID(kPortCharacteristicID)]) {
        if (ch.value.length == sizeof(CBL2CAPPSM)) {
            memcpy(&peer->_psm, ch.value.bytes, sizeof(CBL2CAPPSM));
            _counterpart->_log(LogLevel::Info, "%s (%s) listens on port/PSM %u",
                               idStr(peripheral), peripheral.name.UTF8String, peer->_psm);
            if (peer->_connectState > BluetoothPeer::NotConnecting) {
                peer->_connectState = BluetoothPeer::OpeningChannel;
                [peripheral openL2CAPChannel: peer->_psm];
            }
        } else {
            _counterpart->_log(LogLevel::Warning, "%s (%s) has invalid port/PSM data (%zu bytes)",
                               idStr(peripheral), peripheral.name.UTF8String, ch.value.length);
        }
    } else if ([ch.UUID isEqual: mkUUID(kMetadataCharacteristicID)]) {
        _counterpart->_log(LogLevel::Info, "%s (%s) has metadata (%zu bytes)",
                           idStr(peripheral), peripheral.name.UTF8String, ch.value.length);
        if (ch.value)
            peer->setMetadata(DecodeTXTToMetadata(slice(ch.value)));
    }
}

- (void) peripheral:(CBPeripheral*)peripheral
didOpenL2CAPChannel:(nullable CBL2CAPChannel*)channel
              error:(nullable NSError*)error
{
    // Completion routine from opening an outgoing channel.
    if (auto peer = peerForPeripheral(peripheral); peer && peer->_connectState) {
        peer->_connectState = BluetoothPeer::NotConnecting;
        if (error) {
            _counterpart->_log(LogLevel::Error, "L2CAP connection to %s failed: %s",
                               idStr(peripheral), errorStr(error));
            auto c4err = C4Error::make(LiteCoreDomain, kC4ErrorIOError, errorStr(error));  //TODO: Real error
            peer->connected(nullptr, c4err);
        } else {
            _counterpart->_log(LogLevel::Info, "Opened L2CAP connection %p to %s!", channel, idStr(peripheral));
            if (!peer->connected((__bridge void*)channel, kC4NoError)) {
                [channel.inputStream close];
                [channel.outputStream close];
            }
        }
    } else if (channel) {
        _counterpart->_log(LogLevel::Info, "Opened L2CAP connection to %s but peer canceled; closing it", idStr(peripheral));
        [channel.inputStream close];
        [channel.outputStream close];
    }
}

@end


#pragma mark - OBJECTIVE-C PERIPHERAL MANAGER:


@implementation LiteCoreBTPeripheral
{
    BluetoothProvider*          _counterpart;               // The C++ C4PeerDiscoveryProvider
    dispatch_queue_t            _queue;                     // The dispatch queue I'm called on
    NSString*                   _name;                      // The local/display name
    CBL2CAPPSM                  _psm;                       // BT "port" number I'm listening for connections on
    C4Peer::Metadata            _metadata;                  // Current metadata
    CBPeripheralManager*        _manager;                   // The CoreBluetooth peripheral manager
    CBMutableCharacteristic*    _psmCharacteristic;         // BT Characteristic whose value is my PSM
    CBMutableCharacteristic*    _metadataCharacteristic;    // BT Characteristic whose value is my metadata
}

- (instancetype) initWithCounterpart: (nonnull BluetoothProvider*) counterpart
                               queue: (dispatch_queue_t)queue
{
    self = [super init];
    if (self) {
        _counterpart = counterpart;
        _queue = queue;
    }
    return self;
}

- (void) publish: (const char*)name
        metadata: (C4Peer::Metadata)md
{
    _counterpart->_log(LogLevel::Verbose, "Starting publishing...");
    _name = @(name);
    _metadata = std::move(md);
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
    if (_manager.isAdvertising || _manager.state == CBManagerStateUnknown)
        return;

    _psmCharacteristic = [[CBMutableCharacteristic alloc] initWithType: mkUUID(kPortCharacteristicID)
                                                             properties: CBCharacteristicPropertyRead
                                                                  value: nil
                                                            permissions: CBAttributePermissionsReadable];
    _metadataCharacteristic = [[CBMutableCharacteristic alloc] initWithType: mkUUID(kMetadataCharacteristicID)
                                                                 properties: CBCharacteristicPropertyRead |
                                                                                CBCharacteristicPropertyNotify
                                                                      value: nil
                                                                permissions: CBAttributePermissionsReadable];

    auto serviceUUID = mkUUID(kP2PServiceID);
    CBMutableService *service = [[CBMutableService alloc] initWithType: serviceUUID primary: YES];
    service.characteristics = @[ _psmCharacteristic, _metadataCharacteristic];
    [_manager removeAllServices];
    [_manager addService: service];

    [_manager startAdvertising: @{
        CBAdvertisementDataLocalNameKey: _name,
        CBAdvertisementDataServiceUUIDsKey: @[serviceUUID]
    }];

    _psm = 0;
    [_manager publishL2CAPChannelWithEncryption: YES];
}

- (void) _stopAdvertise: (const char*)errorMessage {
    if (_manager.isAdvertising) {
        [_manager stopAdvertising];
        if (_psm != 0) {
            [_manager unpublishL2CAPChannel: _psm];
            _psm = 0;
        }
        _counterpart->publishStateChanged(false, c4errorFrom(errorMessage));
    }
}

#pragma mark - CBPeripheralManagerDelegate

- (void) peripheralManagerDidUpdateState: (nonnull CBPeripheralManager*)manager {
    switch (manager.state) {
        case CBManagerStatePoweredOn:
            [self _startAdvertise];
            return;
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
            _counterpart->_log(LogLevel::Warning, "Unknown Bluetooth state %ld", long(manager.state));
            return;
    }
}

- (void) peripheralManagerDidStartAdvertising: (CBPeripheralManager*)manager
                                        error: (nullable NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error advertising: %s", errorStr(error));
        _counterpart->publishStateChanged(false, c4errorFrom(error));
    } else {
        _counterpart->_log(LogLevel::Debug, "Advertising started");
    }
}

- (void) peripheralManager: (CBPeripheralManager*)manager
             didAddService: (CBService*)service
                     error: (nullable NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error when adding service: %s", errorStr(error));
        [self _stopAdvertise: errorStr(error)];
    } else {
        _counterpart->_log(LogLevel::Verbose, "Advertising service");
    }
}

- (void) peripheralManager:(CBPeripheralManager*)manager
     didReceiveReadRequest:(CBATTRequest*)request
{
    _counterpart->_log(LogLevel::Info, "Read request from %s for characteristic %s offset %zu",
                       idStr(request.central), idStr(request.characteristic), size_t(request.offset));
    CBATTError result = CBATTErrorAttributeNotFound;
    if (request.offset != 0) {
        result = CBATTErrorInvalidOffset;
    } else if (request.characteristic == _psmCharacteristic) {
        request.value = [NSData dataWithBytes: &_psm length: sizeof(_psm)];
        result = CBATTErrorSuccess;
    } else if (request.characteristic == _metadataCharacteristic) {
        alloc_slice txt = EncodeMetadataAsTXT(_metadata);
        request.value = txt.copiedNSData();
        result = CBATTErrorSuccess;
    }
    [manager respondToRequest: request withResult: result];
}

- (void) peripheralManager:(CBPeripheralManager*)manager
    didPublishL2CAPChannel:(CBL2CAPPSM)psm
                     error:(nullable NSError*)error
{
    if (error) {
        _counterpart->_log(LogLevel::Info, "Error publishing L2CAP channel: %s", errorStr(error));
        [self _stopAdvertise: errorStr(error)];
    } else {
        _counterpart->_log(LogLevel::Verbose, "Published L2CAP channel on port/PSM %u", psm);
        _psm = psm;

        [_manager updateValue: [NSData dataWithBytes: &_psm length: sizeof(_psm)]
            forCharacteristic: _psmCharacteristic
         onSubscribedCentrals: nil];

        _counterpart->publishStateChanged(true);
    }
}

- (void) peripheralManager:(CBPeripheralManager*)manager
       didOpenL2CAPChannel:(nullable CBL2CAPChannel*)channel
                     error:(nullable NSError*)error
{
    // Reports an incoming connection:
    if (error) {
        _counterpart->_log(LogLevel::Error, "Incoming L2CAP connection failed: %s", errorStr(error));
        return;
    }
    _counterpart->_log(LogLevel::Info, "Incoming L2CAP connection from %s", idStr(channel.peer));

    auto central = channel.peer;
    auto c4peer = peerForPeripheral(central);
    if (!c4peer) {
        // Apparently I have not discovered this peer yet. Create & register a C4Peer, but I can't set its _peripheral
        // because `channel.peer` is a CBCentral. Once this peer ID shows up through discovery, I'll set _peripheral.
        string peerID = idStr(central);
        c4peer = fleece::make_retained<BluetoothPeer>(_counterpart, peerID, "", nil);
        _counterpart->addPeer(c4peer);
    }
    Retained<C4Socket> socket = BTSocketFromL2CAPChannel(channel, true);
    if (!_counterpart->notifyIncomingConnection(c4peer, socket))
        BTSocketFactory.close(socket);
}

@end

#endif  //___APPLE__
