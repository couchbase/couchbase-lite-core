//
// Constants.cs
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

namespace LiteCore
{
    internal static class Constants
    {
        internal const string DllName = "LiteCore";

#if LITECORE_PACKAGED
        internal
#else
        public
#endif
            static readonly string ObjectTypeProperty = "@type";

#if LITECORE_PACKAGED
        internal
#else
        public
#endif
            static readonly string ObjectTypeBlob = "blob";

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
         static readonly string C4LanguageDefault = null;

#if LITECORE_PACKAGED
        internal
#else
    public
#endif
         static readonly string C4LanguageNone = "";

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
         static readonly string C4PlaceholderValue = "*";
    }
}
