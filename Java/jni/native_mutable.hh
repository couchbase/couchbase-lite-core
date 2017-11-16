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
        class JNIRef : public RefCounted {
        public:
            JNIRef(JNIEnv *env, jobject native) {
                if (env != NULL && native != NULL) {
                    _env = env;
                    _native = _env->NewGlobalRef(native);
                }
            }

            ~JNIRef() {
                if (_env != NULL && _native != NULL) {
                    _env->DeleteGlobalRef(_native);
                    _native = NULL;
                    _env = NULL;
                }
            }

            jobject native() {
                return _native;
            }

        private:
            jobject _native;
            JNIEnv *_env;
        };

        typedef Retained<JNIRef> JNative;

        typedef MArray<JNative> JMArray;
        typedef MCollection<JNative> JMCollection;
        typedef MDict<JNative> JMDict;
        typedef MDictIterator<JNative> JMDictIterator;
        typedef MRoot<JNative> JMRoot;
        typedef MValue<JNative> JMValue;
    }
}

#endif // native_mutable_hpp