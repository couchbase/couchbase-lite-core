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
#include <queue>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {

    class SQLWriter;
    class AliasedNode;
    class SelectNode;
    class SourceNode;
    template <class T>
    class List;

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
        ParseContext(Arena& a) : arena(a) {}

        ParseContext(ParseContext const& parent) : arena(parent.arena){};

        Arena&                                   arena;                    // The arena allocator
        SelectNode* C4NULLABLE                   select{};                 // The enclosing SELECT, if any
        std::unordered_map<string, AliasedNode*> aliases;                  // All of the sources & named results
        std::vector<SourceNode*>                 sources;                  // All sources
        SourceNode* C4NULLABLE                   from{};                   // The main source
        Collation                                collation;                // Current collation in effect
        bool                                     collationApplied = true;  // False if no COLLATE node generated

        const char* newString(string_view);  ///< Allocates a string in the arena
    };

    /** Top-level Context that provides an Arena, and destructs all Nodes in its destructor. */
    struct RootContext
        : Arena
        , public ParseContext {
        RootContext();
    };

#pragma mark - NODE CLASS:

    /** Abstract syntax tree node for parsing N1QL queries from JSON/Fleece.
        Nodes are allocated in an Arena and are not copyable.
        The Node class hierarchy is described in the [README](./README.md)

        @warning  Node and its subclasses MUST NOT have data members that require destruction --
            that means no `string`, no `vector`, no `fleece::MutableArray`. The destructors will not be called when
            the Arena is freed, meaning memory will be leaked.
            - Call `ParseContext::newString()` to allocate a string in the Arena.
            - Use `List` (below) instead of `vector` to collect child Nodes into lists. */
    class Node {
      public:
        // Nodes are allocated in an Arena owned by the RootContext. Use `new (ctx) FooNode(...)`.
        void* C4NONNULL operator new(size_t size, ParseContext& ctx) noexcept;
        void            operator delete(void* C4NULLABLE ptr, ParseContext& ctx) noexcept;

        /// The node's parent in the parse tree.
        Node const* parent() const { return _parent; }

        void setParent(Node* C4NULLABLE);

        /// Next sibling in list.
        /// @note Only some parents organize children into lists! Some parents use multiple lists!
        Node const* C4NULLABLE next() const { return _next; }

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
        template <class T>
        friend class List;

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

        /// Utility to add a child reference to a list, ensuring its parent points to me.
        template <class T>
        void addChild(List<T>& list, T* C4NONNULL c);

        struct ChildVisitor {
            function_ref<void(Node&)> fn;

            ChildVisitor const& operator()(Node* C4NULLABLE child) const {
                if ( child ) fn(*child);
                return *this;
            }

            template <class T>
            ChildVisitor const& operator()(List<T> const& children) const;
        };

        /// Subclasses that add children MUST override this and call `visitor(child)` on each direct child.
        virtual void visitChildren(ChildVisitor const& visitor) {}

        void operator delete(void* ptr) noexcept {}

        Node(Node const&)            = delete;  // not copyable
        Node& operator=(Node const&) = delete;

        const Node* C4NULLABLE _parent{};  // The parent node
        Node* C4NULLABLE       _next{};    // Next sibling in list (but not all parents put children in lists!)
    };

#pragma mark - LIST:

    /** A simple linked list of Nodes. */
    template <class T>
    class List {
      public:
        T* C4NULLABLE front() const noexcept FLPURE { return _head; }

        bool empty() const noexcept FLPURE { return !_head; }

        size_t size() const noexcept FLPURE {
            size_t s = 0;
            for ( Node* node = _head; node; node = node->_next ) ++s;
            return s;
        }

        T* C4NONNULL operator[](size_t i) const noexcept FLPURE {
            T* node = _head;
            while ( i-- > 0 ) node = static_cast<T*>(node->_next);
            return node;
        }

        void push_front(T* C4NONNULL node) {
            DebugAssert(!node->_next);
            node->_next = _head;
            _head       = node;
            if ( !_tail ) _tail = node;
        }

        void push_back(T* C4NONNULL node) {
            DebugAssert(!node->_next);
            if ( _tail ) _tail->_next = node;
            else
                _head = node;
            _tail = node;
        }

        T* C4NONNULL pop_front() noexcept {
            auto f = _head;
            _head  = static_cast<T*>(f->_next);
            if ( !_head ) _tail = nullptr;
            f->_next = nullptr;
            return f;
        }

        class iterator {
          public:
            bool operator==(iterator const& i) noexcept FLPURE { return _cur == i._cur; }

            bool operator!=(iterator const& i) noexcept FLPURE { return _cur != i._cur; }

            iterator& operator++() noexcept {
                _cur = static_cast<T*>(_cur->_next);
                return *this;
            }

            T* C4NULLABLE operator*() noexcept FLPURE { return _cur; }

            T* C4NULLABLE operator->() noexcept FLPURE { return _cur; }

          private:
            friend class List;

            iterator(T* C4NULLABLE first) noexcept : _cur(first) {}

            T* C4NULLABLE _cur{};
        };

        iterator begin() const noexcept FLPURE { return iterator{_head}; }

        iterator end() const noexcept FLPURE { return iterator{nullptr}; }

      private:
        T* C4NULLABLE _head{};
        T* C4NULLABLE _tail{};
    };

    template <class T>
    void Node::addChild(List<T>& list, T* C4NONNULL c) {
        list.push_back(c);
        c->setParent(this);
    }

    template <class T>
    Node::ChildVisitor const& Node::ChildVisitor::operator()(List<T> const& children) const {
        for ( auto child : children ) fn(*child);
        return *this;
    }

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
