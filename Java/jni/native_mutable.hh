/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

#ifndef native_mutable_hpp
#define native_mutable_hpp

#include <jni.h>
#include "c4Internal.hh"
#include "RefCounted.hh"
#include "native_glue.hh"

#include "MArray.hh"
#include "MCollection.hh"
#include "MContext.hh"
#include "MDict.hh"
#include "MDictIterator.hh"
#include "MRoot.hh"
#include "MValue.hh"

using namespace fleece;
using namespace fleeceapi;

namespace litecore {
    namespace jni {
        typedef MArray<JNative> JMArray;
        typedef MCollection<JNative> JMCollection;
        typedef MDict<JNative> JMDict;
        typedef MDictIterator<JNative> JMDictIterator;
        typedef MRoot<JNative> JMRoot;
        typedef MValue<JNative> JMValue;

        class JMContext : public MContext {
        public:
            JMContext(const alloc_slice &data, FLSharedKeys sk) : MContext(data, sk) {
            }

            virtual ~JMContext() {
            }

            void setJNative(JNIEnv *env, jobject native) {
                if (env != NULL && native != NULL)
                    _nativeRef = JNative(new JNIRef(env, native));
            }

            jobject getJNative() {
                return _nativeRef.get() != NULL ? _nativeRef->native() : NULL;
            }

        private:
            JNative _nativeRef;
        };
    }
}

#endif // native_mutable_hpp