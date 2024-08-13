//
// Node.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Node.hh"

namespace litecore::qt {
    using namespace std;

    const char* ParseContext::newString(string_view sv) {
        auto len = sv.size();
        auto str = (char*)arena.alloc(len + 1);
        memcpy(str, sv.data(), len);
        str[len] = '\0';
        return str;
    }

    // Size of the blocks that the Arena grabs from the malloc heap.
    // Typical queries only allocate a few KB, not enough to fill a single chunk.
    static constexpr size_t kArenaChunkSize = 4000;

    RootContext::RootContext() : Arena(kArenaChunkSize, alignof(Node)), ParseContext(*static_cast<Arena*>(this)) {}

    void* Node::operator new(size_t size, ParseContext& ctx) noexcept { return ctx.arena.alloc(size); }

    void Node::operator delete(void* ptr, ParseContext& ctx) noexcept { ctx.arena.free(ptr); }

    void Node::setParent(Node* p) {
        DebugAssert(!_parent || !p);
        _parent = p;
    }

    void Node::postprocess(ParseContext& ctx) {
        visitChildren(ChildVisitor{[&](Node& child) { child.postprocess(ctx); }});
    }

    void Node::visitTree(VisitorFn const& visitor, bool preorder, unsigned depth) {
        if ( preorder ) visitor(*this, depth);
        visitChildren({[&](Node& child) { child.visitTree(visitor, preorder, depth + 1); }});
        if ( !preorder ) visitor(*this, depth);
    }

}  // namespace litecore::qt
