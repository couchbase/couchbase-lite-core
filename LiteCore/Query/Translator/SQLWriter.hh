//
// SQLWriter.hh
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
#include "ExprNodes.hh"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include <iostream>
#include <type_traits>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {
    using namespace fleece;
    using namespace std;

    /** Specialized output stream for Nodes writing SQL. */
    class SQLWriter {
      public:
        explicit SQLWriter(std::ostream& out) : _out(out) {}

        /// Writes a child `Node` by calling its `writeSQL` method.
        SQLWriter& operator<<(Node const* n) {
            n->writeSQL(*this);
            return *this;
        }

        /// Writes a child `Node` by calling its `writeSQL` method.
        SQLWriter& operator<<(Node const& n) {
            n.writeSQL(*this);
            return *this;
        }

        /// Any other types are written directly to the ostream:
        template <typename T,  // (template gunk is to prevent T being a Node* or Node&)
                  typename = typename std::enable_if<
                          !std::is_base_of_v<Node, std::remove_pointer_t<std::decay_t<T>>>>::type>
        SQLWriter& operator<<(T&& t) {
            _out << std::forward<T>(t);
            return *this;
        }

        /// The name of a table's `body` column. This is altered by some callers of QueryTranslator,
        /// usually when generating SQL for triggers.
        string bodyColumnName = "body";

      private:
        friend class WithPrecedence;
        std::ostream& _out;             // Output stream
        int           _precedence = 0;  // Precedence of current operator
    };

    /** RAII helper class used with SQLWriter to temporarily change the current precedence. */
    class WithPrecedence {
      public:
        WithPrecedence(SQLWriter& ctx, int prec) : _ctx(ctx), _prev(_ctx._precedence) { _ctx._precedence = prec; }

        ~WithPrecedence() { _ctx._precedence = _prev; }

      protected:
        SQLWriter& _ctx;
        int        _prev;
    };

    /** RAII helper class used with SQLWriter for adding parentheses around an expression. */
    class Parenthesize : public WithPrecedence {
      public:
        Parenthesize(SQLWriter& ctx, int prec) : WithPrecedence(ctx, prec), _parens(prec <= _prev) {
            if ( _parens ) ctx << '(';
        }

        ~Parenthesize() {
            if ( _parens ) _ctx << ')';
        }

      private:
        bool _parens;
    };
}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
