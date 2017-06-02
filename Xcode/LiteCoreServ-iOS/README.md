# LiteCoreServ-iOS

LiteCoreServ-iOS is a LiteCoreServe app that runs on iOS devices or simulators. The purpose of the app is for testing LiteCore via the REST APIs.

## How to build and run?

### Requirement
- XCode 8.3

### Get the code
 ```
 $git clone https://github.com/couchbase/couchbase-lite-core.git
 $git submodule update --init
 ```
 
### Build and run with XCode
1. $cd Xcode
1. Open LiteCore.xcodeproj project with XCode.
2. Select `LiteCoreServ-iOS` scheme to run on iOS devices.

### Build and run with command lines (Simulator only)
1. Build the app:

 ```
 $xcodebuild -scheme LiteCoreServ-iOS -sdk iphonesimulator -configuration Release -derivedDataPath build
 ```

2. Run the simulator:
 ```
 $killall Simulator
 $open -a Simulator --args -CurrentDeviceUDID <YOUR SIMULATOR UUID>
 ```
 To find the simulator UUID, use one of the following commands:
 ```
 $instruments -s devices
 $xcrun simctl list
 ```

 To wait for the simulator to boot, you can write a bash script like this:
 ```
 count=`xcrun simctl list | grep Booted | wc -l | sed -e 's/ //g'`
 while [ $count -lt 1 ]
 do
    sleep 1
    count=`xcrun simctl list | grep Booted | wc -l | sed -e 's/ //g'`
 done
 ```
3. Install and run the app on the simulator:
 ```
 $xcrun simctl uninstall booted com.couchbase.LiteServ-iOS
 $xcrun simctl install booted <PATH to LiteServ-iOS.app>
 $xcrun simctl launch booted com.couchbase.LiteServ-iOS
 ```

 Reference: [https://coderwall.com/p/fprm_g/chose-ios-simulator-via-command-line--2](https://coderwall.com/p/fprm_g/chose-ios-simulator-via-command-line--2)

## How to change default settings?
Before running the app, you can setup environment variables to set the app settings. The app settings consist of:

Name       | Default value| Description|
-----------|--------------|------------|
adminPort  |59850         |Admin port to listen on
port       |59840         |Listener port to listen on


If running the app with XCode, you can select `Edit Scheme...` of the scheme you want to run and then setup your environment variables from there. If running the app by using the `xcrun` command, you can set the environment variables by using export command and prefix each variable with the `SIMCTL_CHILD_` as below:

```
export SIMCTL_CHILD_port="8888"
```

## How to use Admin port?
1. `PUT /start` : Start or restart the listener with JSON configuration.

 ```
$curl -X PUT -H "Content-Type: application/json" -d '{ "port": 8888 }' "http://localhost:59850/start"
 ```
 
2. `PUT /stop` : Stop the listener.
 ```
 $curl -X PUT "http://localhost:59850/stop"
 ```
 
3. `GET /` See current runnig configuration:
 ```
 $curl -X GET "http://localhost:59850/"
 ```
