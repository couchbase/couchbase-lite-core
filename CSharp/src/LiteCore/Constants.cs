//
// Constants.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
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
using System;
using C4StorageEngine = System.String;

namespace LiteCore
{
    internal static class Constants
    {
        internal const string DllName = "LiteCore";

        public static readonly C4StorageEngine ForestDBStorageEngine = "ForestDB";

        public static readonly C4StorageEngine SQLiteStorageEngine = "SQLite";

        public static readonly string C4InfoStore = "info";

        public static readonly string C4LocalDocStore = "_local";

        public static readonly string C4LanguageDefault = null;

        public static readonly string C4LanguageNone = "";

        public static readonly string C4PlaceholderValue = "*";
    }
}
