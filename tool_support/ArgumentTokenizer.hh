//
// ArgumentTokenizer.hh
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include <string>
#include <vector>

class ArgumentTokenizer {
  public:
    ArgumentTokenizer() {}

    ArgumentTokenizer(std::string input) { reset(std::move(input)); }

    ArgumentTokenizer(const ArgumentTokenizer& other)
        : _args(other._args), _input(other._input), _argument(other._argument), _hasArgument(other._hasArgument) {
        if ( other._current ) {
            size_t currentPos = other._current - other._input.c_str();
            _current          = &_input[currentPos];
        }
        if ( other._startOfArg ) {
            size_t startOfArgPos = other._startOfArg - other._input.c_str();
            _startOfArg          = &_input[startOfArgPos];
        }
    }

    /// Clears the input line and internal state.
    void reset();

    /// Stores an input line and resets to the start.
    void reset(std::string input);

    /// Stores a list of pre-parsed arguments and resets to the start.
    void reset(std::vector<std::string> args);

    /// Returns to the start of the input line.
    void rewind() { reset(_input); }

    /// True if there is currently an argument to read.
    bool hasArgument() const { return _hasArgument; }

    /// Returns the current argument, or an empty string if none.
    const std::string& argument() const { return _argument; }

    /// True if there is whitespace after this argument. (This may be true even if this is the
    /// last argument, if there is trailing whitespace. This method is intended for use by
    /// command completion utilities, to determine if the final argument needs completion.)
    bool spaceAfterArgument() const { return _spaceAfterArgument; }

    /// Moves to the next argument. Returns true if there is one, else false.
    bool next();

    /// Returns the remainder of the input line, after the current argument and any following
    /// whitespace.
    std::string restOfInput();

    /// Static utility function that breaks an input line into arguments.
    /// Returns false if the input is `nullptr` or has parse errors (unclosed quotes.)
    static bool tokenize(const char* input, std::vector<std::string>& outArgs) {
        if ( !input ) { return false; }

        try {
            return ArgumentTokenizer(input)._tokenize(outArgs);
        } catch ( std::exception& e ) { return false; }
    }

  private:
    bool _tokenize(std::vector<std::string>& outArgs);

    std::vector<std::string> _args;                 // Pre-parsed arguments, if any
    std::string              _input;                // Raw input line
    const char*              _current{nullptr};     // Points to next unread char in _input, if any
    const char*              _startOfArg{nullptr};  // Points to start of current arg in _input, if any
    std::string              _argument;             // The current argument
    bool                     _hasArgument;          // True if there is a current argument
    bool                     _spaceAfterArgument;   // True if current parsed argument ended at whitespace
};
