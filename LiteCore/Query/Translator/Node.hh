//
// Node.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Arena.hh"
#include "Base.hh"
#include "Error.hh"
#include "UnicodeCollator.hh"
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/Mutable.hh"
#include <iosfwd>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {

    class SQLWriter;
    class AliasedNode;
    class SelectNode;
    class SourceNode;

#pragma mark - TYPES:

    /** Properties of the N1QL `meta()` object. */
    enum class MetaProperty {
        none,
        id,
        sequence,
        deleted,
        expiration,
        revisionID,
        rowid,

        _notDeleted = -1  // used internally
    };
    static constexpr size_t kNumMetaProperties = 6;  // ignoring `none` and `_notDeleted`

    /** Types of JOINs. */
    enum class JoinType { none = -1, inner = 0, left, leftOuter, cross };

    /** Attributes of an operation. */
    enum OpFlags {
        kOpNoFlags        = 0x00,
        kOpBoolResult     = 0x02,  // Result is boolean
        kOpNumberResult   = 0x04,  // Result is a number
        kOpStringResult   = 0x08,  // Result is a string
        kOpAggregate      = 0x10,  // This is an aggregate function
        kOpWantsCollation = 0x20,  // This function supports a collation argument
    };

    /** Types of indexes. */
    enum class IndexType { FTS, vector };

#pragma mark - PARSE CONTEXT:

    /** State used during parsing, passed down through the recursive descent. */
    struct ParseContext {
        explicit ParseContext(Arena& a) : arena(a) {}

        Arena&                                   arena;                    // The arena allocator for Nodes
        SelectNode* C4NULLABLE                   select{};                 // The enclosing SELECT, if any
        std::unordered_map<string, AliasedNode*> aliases;                  // All of the sources & named results
        std::vector<SourceNode*>                 sources;                  // All sources
        SourceNode* C4NULLABLE                   from{};                   // The main source
        Collation                                collation;                // Current collation in effect
        bool                                     collationApplied = true;  // False if no COLLATE node generated
    };

    /** Top-level Context that provides an Arena, and destructs all Nodes in its destructor. */
    struct RootContext
        : Arena
        , public ParseContext {
        RootContext();
        ~RootContext();
    };

#pragma mark - NODE CLASS:

    /** Abstract syntax tree node for parsing N1QL queries from JSON/Fleece.
        Nodes are allocated in an Arena and are not copyable.
        The Node class hierarchy is described in the [README](./README.md) */
    class Node {
      public:
        // Nodes are allocated in an Arena. The ParseContext has a reference to it. Use `new (ctx) FooNode(...)`.
        void* C4NONNULL operator new(size_t size, ParseContext& ctx) noexcept;
        void            operator delete(void* C4NULLABLE ptr, ParseContext& ctx) noexcept;

        /// The node's parent in the parse tree.
        Node const* parent() const { return _parent; }

        void setParent(Node* C4NULLABLE);

        /// The SourceNode (`FROM` item) this references, if any. Overridden by MetaNode and PropertyNode.
        virtual SourceNode* C4NULLABLE source() const { return nullptr; }

        using VisitorFn = function_ref<void(Node&, unsigned depth)>;

        /// The Visitor callback will be called with this Node and each of its descendents.
        /// @param visitor  The callback
        /// @param preorder  If true, a Node is visited before its children; else after
        /// @param depth  The initial depth corresponding to this Node.
        void visitTree(VisitorFn const& visitor, bool preorder = true, unsigned depth = 0);

        /// Called after the Node tree is generated; allows each node to make changes.
        /// Overrides must call the inherited method, probably first.
        virtual void postprocess(ParseContext& ctx);

        /// Returns SQLite-flavor SQL representation.
        string SQLString() const;

        /// Writes SQLite-flavor SQL representation to a stream.
        void writeSQL(std::ostream& out) const;

        /// Writes SQL to the writer's output stream.
        virtual void writeSQL(SQLWriter&) const = 0;

      protected:
        friend struct RootContext;

        Node()          = default;
        virtual ~Node() = default;

        /// Utility to initialize a child reference, ensuring its parent points to me.
        template <class T>
        void initChild(T* C4NONNULL& var, T* C4NONNULL c) {
            c->setParent(this);
            var = c;
        }

        /// Utility to set a child reference, ensuring its parent points to me.
        template <class T>
        void setChild(T* C4NULLABLE& var, T* C4NULLABLE c) {
            if ( var ) var->setParent(nullptr);
            if ( c ) c->setParent(this);
            var = c;
        }

        /// Utility to add a child reference to a vector, ensuring its parent points to me.
        template <class T>
        void addChild(std::vector<T*>& var, T* C4NONNULL c) {
            c->setParent(this);
            var.push_back(c);
        }

        struct ChildVisitor {
            function_ref<void(Node&)> fn;

            ChildVisitor const& operator()(Node* C4NULLABLE child) const {
                if ( child ) fn(*child);
                return *this;
            }

            template <class T>
            ChildVisitor const& operator()(std::vector<T> const& children) const {
                for ( auto& child : children ) fn(*child);
                return *this;
            }
        };

        /// Subclasses that add children MUST override this and call `visitor(child)` on each direct child.
        virtual void visitChildren(ChildVisitor const& visitor) {}

        void operator delete(void* ptr) noexcept {}

        Node(Node const&)            = delete;  // not copyable
        Node& operator=(Node const&) = delete;

        const Node* C4NULLABLE _parent{};  // The parent node
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
