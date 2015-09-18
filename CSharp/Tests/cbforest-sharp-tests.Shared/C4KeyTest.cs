//
//  C4KeyTest.cs
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

namespace CBForest.Tests
{
    [TestFixture]
    public unsafe class C4KeyTest : C4Test
    {
        private C4Key *_key;
        
        public override void SetUp()
        {
            _key = Native.c4key_new();
        }
        
        public override void TearDown()
        {
            Native.c4key_free(_key);
        }
        
        [Test]
        public void TestCreateKey()
        {
            PopulateKey();
            Assert.AreEqual("[null,false,true,0,12345,-2468,\"foo\",[]]", ToJSON(_key));
        }
        
        [Test]
        public void TestReadKey()
        {
            PopulateKey();
            var r = Native.c4key_read(_key);
            Assert.AreEqual(C4KeyToken.Array, Native.c4key_peek(&r));
            Native.c4key_skipToken(&r);
            Assert.AreEqual(C4KeyToken.Null, Native.c4key_peek(&r));
            Native.c4key_skipToken(&r);
            Assert.AreEqual(C4KeyToken.Bool, Native.c4key_peek(&r));
            Assert.IsFalse(Native.c4key_readBool(&r));
            Assert.IsTrue(Native.c4key_readBool(&r));
            Assert.AreEqual(0.0, Native.c4key_readNumber(&r));
            Assert.AreEqual(12345.0, Native.c4key_readNumber(&r));
            Assert.AreEqual(-2468.0, Native.c4key_readNumber(&r));
            Assert.AreEqual("foo", Native.c4key_readString(&r));
            Assert.AreEqual(C4KeyToken.Array, Native.c4key_peek(&r));
            Native.c4key_skipToken(&r);
            Assert.AreEqual(C4KeyToken.EndSequence, Native.c4key_peek(&r));
            Native.c4key_skipToken(&r);
            Assert.AreEqual(C4KeyToken.EndSequence, Native.c4key_peek(&r));
            Native.c4key_skipToken(&r);
        }
        
        private void PopulateKey()
        {
            Native.c4key_beginArray(_key);
            Native.c4key_addNull(_key);
            Native.c4key_addBool(_key, false);
            Native.c4key_addBool(_key, true);
            Native.c4key_addNumber(_key, 0);
            Native.c4key_addNumber(_key, 12345);
            Native.c4key_addNumber(_key, -2468);
            Native.c4key_addString(_key, "foo");
            Native.c4key_beginArray(_key);
            Native.c4key_endArray(_key);
            Native.c4key_endArray(_key);
        }
    }
}

