//
// Created by Jens Alfke on 2/20/25.
//

#pragma once
#include "Logging.hh"
#include "StringUtil.hh"
#include <mutex>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** A registry that doles out `LogObjectRef` integers and associates them with "nicknames".
     *  A LogObjectRef can be associated with a parent, in which case the parent's nickname is
     *  prepended, forming a path.
     *  This registry is used instances of `Logging`.
     *  @note  This class is thread-safe. */
    class LogObjectMap {
      public:
        /// Assigns a new LogObjectRef and associates it with a nickname.
        /// On entry, if `*ref` is None, this method stores the new ref into `*ref` and returns true.
        /// Otherwise it does nothing and returns false.
        bool registerObject(LogObjectRef* ref, const std::string& nickname) {
            std::unique_lock lock(_mutex);
            if ( *ref != LogObjectRef::None ) return false;
            *ref             = LogObjectRef{++_lastObjRef};
            std::string path = stringprintf("/%s#%u/", nickname.c_str(), unsigned(*ref));
            _objects[*ref]   = {std::move(path), LogObjectRef::None};
            return true;
        }

        /// Assigns a parent to a ref.
        bool registerParentObject(LogObjectRef object, LogObjectRef parentObject) {
            const char* warning;
            {
                std::unique_lock lock(_mutex);
                if ( auto iter = _objects.find(object); iter == _objects.end() ) {
                    warning = "object is not registered";
                } else if ( auto iParent = _objects.find(parentObject); iParent == _objects.end() ) {
                    warning = "parentObject is not registered";
                } else if ( iter->second.second != LogObjectRef::None ) {
                    warning = "object is already assigned parent";
                } else {
                    // Prepend parent's nickname:
                    iter->second.first  = iParent->second.first + iter->second.first.substr(1);
                    iter->second.second = parentObject;
                    return true;
                }
            }
            WarnError("LogDomain::registerParentObject, %s", warning);
            return false;
        }

        /// Removes a LogObjectRef from the registry.
        void unregisterObject(LogObjectRef obj) {
            std::unique_lock lock(_mutex);
            _objects.erase(obj);
        }

        /// Returns a ref's path string, which is its nickname and numeric ref,
        /// prepended with its parent's path if any.
        std::string getObjectPath(LogObjectRef obj) {
            std::unique_lock lock(_mutex);
            return _getObjectPath(obj);
        }

        /// Same as getObjectPath, but writes the path into `destBuf`.
        size_t addObjectPath(char* destBuf, size_t bufSize, LogObjectRef obj) {
            std::unique_lock lock(_mutex);
            return snprintf(destBuf, bufSize, "Obj=%s ", _getObjectPath(obj).c_str());
        }

      private:
        std::string const& _getObjectPath(LogObjectRef obj) {
            if ( auto iter = _objects.find(obj); iter != _objects.end() ) return iter->second.first;
            return kEmptyString;
        }

        using ObjectMap = std::unordered_map<LogObjectRef, std::pair<std::string, LogObjectRef>>;

        static inline const std::string kEmptyString;

        std::mutex _mutex;
        ObjectMap  _objects;
        unsigned   _lastObjRef = 0;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
