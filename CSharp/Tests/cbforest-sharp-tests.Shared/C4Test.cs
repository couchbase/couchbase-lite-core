//
//  C4Test.cs
//
//  Author:
//  	Jim Borden  <jim.borden@couchbase.com>
//
//  Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
using System;
using NUnit.Framework;
using System.IO;


namespace CBForest.Tests
{
    public unsafe abstract class C4Test
    {
        protected const string BODY = "{\"name\":007}";
        protected const string DOC_ID = "mydoc";
        protected const string REV_ID = "1-abcdef";
        protected const string REV_2_ID = "2-d00d3333";
        
        protected virtual C4EncryptionKey* EncryptionKey 
        {
            get {
                return null;
            }
        }
        
        protected C4Database *_db;
        
        [SetUp]
        public virtual void SetUp()
        {
            Native.c4log_register(C4LogLevel.Warning, Log);
            Directory.CreateDirectory(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
            var dbPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "forest_temp.fdb");
            C4Error error;
            _db = Native.c4db_open(dbPath, C4DatabaseFlags.Create, EncryptionKey, &error);
            Assert.IsFalse(_db == null);
        }
        
        [TearDown]
        public virtual void TearDown()
        {
            C4Error error;
            Native.c4db_delete(_db, &error);
        }
        
        public string ToJSON(C4KeyReader r)
        {
            return Native.c4key_toJSON(&r);
        }
        
        public string ToJSON(C4Key *key)
        {
            return ToJSON(Native.c4key_read(key));   
        }
        
        protected void CreateRev(string docID, string revID, string body, bool isNew = true)
        {
            using(var t = new TransactionHelper(_db)) {
                C4Error error;
                var doc = Native.c4doc_get(_db, docID, false, &error);
                Assert.IsTrue(doc != null);
                var deleted = body == null;
                Assert.AreEqual(isNew ? 1 : 0, Native.c4doc_insertRevision(doc, revID, body, deleted, false, false, &error));
                Assert.IsTrue(Native.c4doc_save(doc, 20, &error));
                Native.c4doc_free(doc);
            }
        }

        private static void Log(C4LogLevel level, string message)
        {
            string[] levelNames = new[] { "debug", "info", "WARNING", "ERROR" };
            Console.Error.WriteLineAsync(String.Format("CBForest-C {0}: {1}", levelNames[(int)level], message));
        }
    }
}

