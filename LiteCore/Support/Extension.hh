//
// Extension.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include <string>

namespace litecore::extension {
    /// @brief Performs some convention based checks on a dynamically loadable
    ///        extension to ensure that it falls within the expected major version.
    /// @param extensionPath The path to the extension to load from the disk.
    /// @param expectedVersion The expected major (i.e. x in x.y.z) version to find.
    /// @return true if the major version matches the found extension, false otherwise.
    bool check_extension_version(const std::string& extensionPath, int expectedVersion);
}  // namespace litecore::extension
