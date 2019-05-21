//
// LWSUtil.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "libwebsockets.h"
#include "c4Base.h"

#define Log(MSG, ...)        C4LogToAt(kC4WebSocketLog, kC4LogInfo,    "%s: " MSG, className(), ##__VA_ARGS__)
#define LogVerbose(MSG, ...) C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "%s: " MSG, className(), ##__VA_ARGS__)
#define LogDebug(MSG, ...)   C4LogToAt(kC4WebSocketLog, kC4LogDebug,   "%s: " MSG, className(), ##__VA_ARGS__)
#define LogError(MSG, ...)   C4LogToAt(kC4WebSocketLog, kC4LogError,   "%s: " MSG, className(), ##__VA_ARGS__)
#define Warn(MSG, ...)       C4LogToAt(kC4WebSocketLog, kC4LogWarning, "%s: " MSG, className(), ##__VA_ARGS__)


namespace litecore { namespace websocket {

    const char* LWSCallbackName(int /*lws_callback_reasons*/ reason);

} }
