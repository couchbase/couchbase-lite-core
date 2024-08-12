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

    // Size of the blocks that the Arena grabs from the malloc heap.
    // Typical queries only allocate a few KB, not enough to fill a single chunk.
    static constexpr size_t kArenaChunkSize = 4000;

    RootContext::RootContext() : Arena(kArenaChunkSize, alignof(Node)), ParseContext(*static_cast<Arena*>(this)) {}

    RootContext::~RootContext() {
        // Visit each Node allocated by the Arena and call its destructor, so that string and vector members
        // will be freed. But skip any Nodes that were already destructed due to exceptions thrown from a constructor.
        eachBlock([](void* block, size_t) {
            if ( *reinterpret_cast<void**>(block) )  // ignore already-destructed block
                delete reinterpret_cast<Node*>(block);
        });
    }

    void* Node::operator new(size_t size, ParseContext& ctx) noexcept { return ctx.arena.alloc(size); }

    // This is the `operator delete` called implicitly if a node constructor throws an exception.
    // The node's destructor has already been called. To prevent RootContext's destructor from trying to destruct
    // the node again, we overwrite its vtable ptr with nullptr.
    void Node::operator delete(void* ptr, ParseContext& ctx) noexcept {
        memset(ptr, 0, sizeof(void*));  // overwrite vtable ptr with null to mark block as already destructed
        ctx.arena.free(ptr);
    }

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
