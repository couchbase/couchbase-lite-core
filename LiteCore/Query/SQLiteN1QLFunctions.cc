//
// SQLiteN1QLFunctions.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
// Implementations of N1QL functions (except for a few that are built into SQLite.)

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Path.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "function_ref.hh"
#include "FleeceImpl.hh"
#include "ParseDate.hh"
#include "NumConversion.hh"
#include <regex>
#include <cmath>
#include <string>

#ifdef _MSC_VER
#undef min
#undef max
#endif

using namespace fleece;
using namespace fleece::impl;
using namespace std;

namespace litecore {

    // Returns a string argument as a slice, or a null slice if the argument isn't a string.
    static inline slice stringSliceArgument(sqlite3_value *arg) noexcept {
        if (sqlite3_value_type(arg) != SQLITE_TEXT)
            return nullslice;
        return valueAsStringSlice(arg);
    }


    // Sets SQLite return value to a string value from an alloc_slice, without copying.
    static void result_alloc_slice(sqlite3_context *ctx, alloc_slice s) {
        s.retain();
        sqlite3_result_text(ctx, (char*)s.buf, (int)s.size, [](void *buf) {
            alloc_slice::release({buf, 1});
        });
    }


#pragma mark - ARRAY FUNCTIONS:


    static void aggregateNumericArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                               function_ref<void(double, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;
                    for (Array::iterator item(root->asArray()); item; ++item) {
                        op(item->asDouble(), stop);
                        if(stop) {
                            return;
                        }
                    }

                    break;
                }
                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    setResultFleeceNull(ctx);
                    return;
            }
        }
    }

    static void aggregateArrayOperation(sqlite3_context* ctx, int argc, sqlite3_value **argv,
                                               function_ref<void(const Value *, bool&)> op) {
        bool stop = false;
        for (int i = 0; i < argc; ++i) {
            sqlite3_value *arg = argv[i];
            switch (sqlite3_value_type(arg)) {
                case SQLITE_BLOB: {
                    const Value *root = fleeceParam(ctx, arg);
                    if (!root)
                        return;

                    if(root->type() != valueType::kArray) {
                        setResultFleeceNull(ctx);
                        return;
                    }

                    for (Array::iterator item(root->asArray()); item; ++item) {
                        op(item.value(), stop);
                        if(stop) {
                            return;
                        }
                    }

                    break;
                }

                case SQLITE_NULL:
                    sqlite3_result_null(ctx);
                    return;
                default:
                    setResultFleeceNull(ctx);
                    return;
            }
        }
    }


    // array_sum() function adds up numbers. Any argument that's a number will be added.
    // Any argument that's a Fleece array will have all numeric values in it added.
    static void fl_array_sum(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum](double num, bool& stop) {
            sum += num;
        });

        sqlite3_result_double(ctx, sum);
    }

    // array_avg() returns the mean value of a numeric array.
    static void fl_array_avg(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double sum = 0.0;
        double count = 0.0;
        aggregateNumericArrayOperation(ctx, argc, argv, [&sum, &count](double num, bool& stop) {
            sum += num;
            count++;
        });

        if(count == 0.0) {
            sqlite3_result_double(ctx, 0.0);
        } else {
            sqlite3_result_double(ctx, sum / count);
        }
    }

    // array_contains(array, value) returns true if `array` contains `value`.
    static void fl_array_contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto type = sqlite3_value_type(argv[0]);
        if (type == SQLITE_NULL)
            sqlite3_result_null(ctx);
        else if (type != SQLITE_BLOB)
            sqlite3_result_zeroblob(ctx, 0);    // return JSON 'null' when collection isn't a collection
        else {
            const Value *collection = fleeceParam(ctx, argv[0]);
            if (!collection || collection->type() != kArray)
                sqlite3_result_zeroblob(ctx, 0);    // return JSON 'null' when collection isn't a collection
            else
                collectionContainsImpl(ctx, collection, argv[1]);
        }
    }

    // array_count() returns the number of non-null items in an array.
    static void fl_array_count(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            if(val->type() != valueType::kNull) {
                count++;
            }
        });

        sqlite3_result_int64(ctx, count);
    }

    // array_ifnull() returns the first non-null item in an array.
    static void fl_array_ifnull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        const Value* foundVal = nullptr;
        aggregateArrayOperation(ctx, argc, argv, [&foundVal](const Value* val, bool& stop) {
            if(val != nullptr && val->type() != valueType::kNull) {
                foundVal = val;
                stop = true;
            }
        });

        if(!foundVal) {
            setResultFleeceNull(ctx);
        } else {
            setResultFromValue(ctx, foundVal);
        }
    }

    // array_length() returns the length of an array.
    static void fl_array_length(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        sqlite3_int64 count = 0;
        aggregateArrayOperation(ctx, argc, argv, [&count](const Value* val, bool& stop) {
            count++;
        });

        sqlite3_result_int64(ctx, count);
    }

    // array_max() returns the maximum number in an array.
    static void fl_array_max(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double max = numeric_limits<double>::min();
        bool nonEmpty = false;
        aggregateNumericArrayOperation(ctx, argc, argv, [&max, &nonEmpty](double num, bool &stop) {
            max = std::max(num, max);
            nonEmpty = true;
        });

        if(nonEmpty) {
            sqlite3_result_double(ctx, max);
        } else {
            setResultFleeceNull(ctx);
        }
    }

    // array_min() returns the minimum number in an array.
    static void fl_array_min(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double max = numeric_limits<double>::max();
        bool nonEmpty = false;
        aggregateNumericArrayOperation(ctx, argc, argv, [&max, &nonEmpty](double num, bool &stop) {
            max = std::min(num, max);
            nonEmpty = true;
        });

        if(nonEmpty) {
            sqlite3_result_double(ctx, max);
        } else {
            setResultFleeceNull(ctx);
        }
    }


#pragma mark - ARRAY AGGREGATE:


    static void array_agg(sqlite3_context* ctx, sqlite3_value *arg) noexcept {
        try {
            auto enc = (Encoder*) sqlite3_aggregate_context(ctx, sizeof(fleece::impl::Encoder));
            if (*(void**)enc == nullptr) {
                // On first call, initialize Fleece encoder:
                enc = new (enc) fleece::impl::Encoder();
                enc->beginArray();
            }

            if (arg) {
                // On step, write the arg to the encoder:
                switch (sqlite3_value_type(arg)) {
                    case SQLITE_INTEGER:
                        enc->writeInt(sqlite3_value_int(arg));
                        break;
                    case SQLITE_FLOAT:
                        enc->writeDouble(sqlite3_value_double(arg));
                        break;
                    case SQLITE_TEXT:
                        enc->writeString({sqlite3_value_text(arg),
                                          (size_t)sqlite3_value_bytes(arg)});
                        break;
                    case SQLITE_BLOB: {
                        const Value *value = fleeceParam(ctx, arg);
                        if (!value)
                            return; // error
                        enc->writeValue(value);
                        break;
                    }
                    case SQLITE_NULL:
                    default:
                        return;
                }

            } else {
                // On final call, finish encoding and set the result to the encoded data:
                enc->endArray();
                setResultBlobFromFleeceData(ctx,  enc->finish() );
                enc->~Encoder();
            }
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "array_agg: exception!", -1);
        }
    }

    static void array_agg_step(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        array_agg(ctx, argv[0]);
    }
    static void array_agg_final(sqlite3_context* ctx) noexcept {
        array_agg(ctx, nullptr);
    }


#pragma mark - CONDITIONAL TESTS (NULL / MISSING / INF / NAN):


    // Test for N1QL MISSING value (which is a SQLite NULL)
    static bool isMissing(sqlite3_value *arg) {
        return sqlite3_value_type(arg) == SQLITE_NULL;
    }

    // Test for N1QL NULL value (which is an empty blob tagged with kFleeceNullSubtype)
    static bool isNull(sqlite3_value *arg) {
        return sqlite3_value_type(arg) == SQLITE_BLOB
            && sqlite3_value_subtype(arg) == kFleeceNullSubtype;
    }

    // ifmissing(...) returns its first non-MISSING argument.
    static void ifmissing(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(!isMissing(argv[i])) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    // ifmissingornull(...) returns its first non-MISSING, non-null argument.
    static void ifmissingornull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(!isMissing(argv[i]) && !isNull(argv[i])) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    // ifnull(...) returns its first non-null argument. I.e. it may return MISSING.
    static void ifnull(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        for(int i = 0; i < argc; i++) {
            if(!isNull(argv[i])) {
                sqlite3_result_value(ctx, argv[i]);
                return;
            }
        }
    }

    // missingif(a,b) returns MISSING if a==b, else returns a.
    static void missingif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_value(ctx, argv[0]);
        }
    }

    // nullif(a,b) returns null if a==b, else returns a.
    static void nullif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            setResultFleeceNull(ctx);
        } else {
            sqlite3_result_value(ctx, argv[0]);
        }
    }

#if 0
    static void ifinf(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isinf(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void ifnan(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isnan(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void ifnanorinf(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        double num = 0.0;
        bool success = false;
        aggregateArrayOperation(ctx, argc, argv, [&num, &success](const Value* val, bool &stop) {
            if(val->type() != valueType::kNumber) {
                stop = true;
                return;
            }

            double nextNum = val->asDouble();
            if(!isinf(nextNum) && !isnan(nextNum)) {
                num = nextNum;
                success = true;
                stop = true;
            }
        });

        if(!success) {
            sqlite3_result_null(ctx);
        } else {
            sqlite3_result_double(ctx, num);
        }
    }
#endif

#if 0
    static void thisif(sqlite3_context* ctx, int argc, sqlite3_value **argv, double val) noexcept {
        auto slice0 = valueAsSlice(argv[0]);
        auto slice1 = valueAsSlice(argv[1]);
        if(slice0.buf == nullptr || slice1.buf == nullptr || slice0.size == 0 || slice1.size == 0) {
            sqlite3_result_null(ctx);
        }

        if(slice0.compare(slice1) == 0) {
            setResultFleeceNull(ctx);
        } else {
            sqlite3_result_double(ctx, val);
        }
    }

    static void nanif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, numeric_limits<double>::quiet_NaN());
    }

    static void neginfif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, -numeric_limits<double>::infinity());
    }

    static void posinfif(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        thisif(ctx, argc, argv, numeric_limits<double>::infinity());
    }
#endif


#pragma mark - STRINGS:


#if 0
    static void fl_base64(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = valueAsSlice(argv[0]);
        string base64 = arg0.base64String();
        sqlite3_result_text(ctx, (char *)base64.c_str(), (int)base64.size(), SQLITE_TRANSIENT);
    }


    static void fl_base64_decode(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg0 = stringArgument(argv[0]);
        size_t expectedLen = (arg0.size + 3) / 4 * 3;
        alloc_slice decoded(expectedLen);
        arg0.readBase64Into(decoded);
        if(sqlite3_value_type(argv[0]) == SQLITE_TEXT) {
            setResultTextFromSlice(ctx, decoded);
        } else {
            setResultBlobFromSlice(ctx, decoded);
        }
    }
#endif

    // contains(string, substring) returns 1 if `string` contains `substring`, else 0
    static void contains(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        Collation col;
        col.unicodeAware = true;
        if(argc > 2) {
            col.readSQLiteName((const char*)sqlite3_value_text(argv[2]));
        }

        auto str = stringSliceArgument(argv[0]);
        auto substr = stringSliceArgument(argv[1]);
        auto current = substr;
        while(str.size > 0) {
            size_t nextStrSize = NextUTF8Length(str);
            size_t nextSubstrSize = NextUTF8Length(current);
            if(!CompareUTF8({str.buf, nextStrSize}, {current.buf, nextSubstrSize}, col)) {
                // The characters are a match, move to the next substring character
                current.moveStart(nextSubstrSize);
                if(current.size == 0) {
                    // Found a match!
                    sqlite3_result_int(ctx, 1);
                    return;
                }
            } else {
                current = substr;
            }

            str.moveStart(nextStrSize);
        }

        sqlite3_result_int(ctx, 0);
    }

    // length() returns the length in characters of a string.
    static void length(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto str = stringSliceArgument(argv[0]);
        if (str)
            sqlite3_result_int64(ctx, UTF8Length(str));
    }

    static void changeCase(sqlite3_context* ctx, sqlite3_value **argv, bool isUpper) noexcept {
        try {
            auto str = stringSliceArgument(argv[0]);
            if (str)
                result_alloc_slice(ctx, UTF8ChangeCase(str, isUpper));
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "upper() or lower() caught an exception!", -1);
        }
    }

    // lower() converts all uppercase letters in a string to lowercase and returns the result.
    static void lower(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        changeCase(ctx, argv, false);
    }

    static void trim(sqlite3_context* ctx, int argc, sqlite3_value **argv, int onSide) noexcept {
        try {
            if (argc != 1) {
                // TODO: Implement 2nd parameter (string containing characters to trim)
                sqlite3_result_error(ctx, "two-parameter trim() is unimplemented", SQLITE_ERROR);
                return;
            }
            auto arg = argv[0];
            if (sqlite3_value_type(arg) != SQLITE_TEXT) {
                sqlite3_result_value(ctx, arg);
                return;
            }
            auto chars = (const char16_t*)sqlite3_value_text16(arg);
            size_t count = sqlite3_value_bytes16(arg) / 2;
            UTF16Trim(chars, count, onSide);
            sqlite3_result_text16(ctx, chars, (int)(2 * count), SQLITE_TRANSIENT);
        } catch (const std::exception &) {
            sqlite3_result_error(ctx, "trim() caught an exception!", -1);
        }
    }

    // ltrim(str) removes leading whitespace characters from `str` and returns the result.
    // ltrim(str, chars) removes leading characters that are contained in the string `chars`.
    static void ltrim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        trim(ctx, argc, argv, -1);
    }

#if 0
    static void position(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = stringArgument(argv[0]).asString();
        unsigned long result = val.find((char *)sqlite3_value_text(argv[1]));
        if(result == string::npos) {
            sqlite3_result_int64(ctx, -1);
        } else {
            sqlite3_result_int64(ctx, result);
        }
    }
#endif

#if 0
    static void repeat(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto base = stringArgument(argv[0]).asString();
        auto num = sqlite3_value_int(argv[1]);
        stringstream result;
        for(int i = 0; i < num; i++) {
            result << base;
        }

        auto resultStr = result.str();
        sqlite3_result_text(ctx, resultStr.c_str(), (int)resultStr.size(), SQLITE_TRANSIENT);
    }
#endif

#if 0
    static void replace(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = stringArgument(argv[0]).asString();
        auto search = stringArgument(argv[1]).asString();
        auto replacement = stringArgument(argv[2]).asString();
        int n = -1;
        if(argc == 4) {
            n = sqlite3_value_int(argv[3]);
        }

        size_t start_pos = 0;
        while(n-- && (start_pos = val.find(search, start_pos)) != std::string::npos) {
            val.replace(start_pos, search.length(), replacement);
            start_pos += replacement.length(); // In case 'replacement' contains 'search', like replacing 'x' with 'yx'
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

#if 0
    static void reverse(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = stringArgument(argv[0]).asString();
        reverse(val.begin(), val.end());
        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

    // rtrim(str) removes trailing whitespace characters from `str` and returns the result.
    // rtrim(str, chars) removes trailing characters that are contained in the string `chars`.
    static void rtrim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        trim(ctx, argc, argv, 1);
    }

#if 0
    static void substr(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto val = stringArgument(argv[0]).asString();
        if(argc == 3) {
            val = val.substr(sqlite3_value_int(argv[1]), sqlite3_value_int(argv[2]));
        } else {
            val = val.substr(sqlite3_value_int(argv[1]));
        }

        sqlite3_result_text(ctx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
    }
#endif

    // trim(str, [chars]) combines the effects of ltrim() and rtrim().
    static void trim(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        trim(ctx, argc, argv, 0);
    }

    // upper() converts all lowercase letters in a string to uppercase and returns the result.
    static void upper(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        changeCase(ctx, argv, true);
    }


#pragma mark - REGULAR EXPRESSIONS:


    static void regexp_like(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto str = stringSliceArgument(argv[0]);
        auto pattern = stringSliceArgument(argv[1]);
        if (str && pattern) {
            regex r((const char*)pattern.buf, pattern.size, regex_constants::ECMAScript);
            bool result = regex_search((const char*)str.buf, (const char*)str.end(), r);
            sqlite3_result_int(ctx, result != 0);
        }
    }

    static void regexp_position(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto str = stringSliceArgument(argv[0]);
        auto pattern = stringSliceArgument(argv[1]);
        if (str && pattern) {
            regex r((const char*)pattern.buf, pattern.size, regex_constants::ECMAScript);
            cmatch pattern_match;
            if(!regex_search((const char*)str.buf, (const char*)str.end(), pattern_match, r)) {
                sqlite3_result_int64(ctx, -1);
                return;
            }

            sqlite3_result_int64(ctx, pattern_match.prefix().length());
        }
    }

    static void regexp_replace(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto str = stringSliceArgument(argv[0]);
        auto pattern = stringSliceArgument(argv[1]);
        auto replacement = stringSliceArgument(argv[2]);
        if (str && pattern && replacement) {
            int n = -1;
            if (argc == 4) {
                n = sqlite3_value_int(argv[3]);
            }

            regex r((const char*)pattern.buf, pattern.size, regex_constants::ECMAScript);
            string s(str);
            auto iter = sregex_iterator(s.begin(), s.end(), r);
            auto last_iter = iter;
            auto stop = sregex_iterator();
            if (iter == stop) {
                sqlite3_result_value(ctx, argv[0]);
            } else {
                string result;
                auto out = back_inserter(result);
                for(; n-- && iter != stop; ++iter) {
                    out = copy(iter->prefix().first, iter->prefix().second, out);
                    out = iter->format(out, (const char*)replacement.buf, (const char*)replacement.end());
                    last_iter = iter;
                }

                out = copy(last_iter->suffix().first, last_iter->suffix().second, out);
                sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
            }
        }
    }


#pragma mark - MATH:


    static bool isNumericNoError(sqlite3_value *arg) {
        auto type = sqlite3_value_type(arg);
        return type == SQLITE_FLOAT || type == SQLITE_INTEGER;
    }

    static bool isNumeric(sqlite3_context* ctx, sqlite3_value *arg) {
        if (_usuallyTrue(isNumericNoError(arg))) {
            return true;
        } else {
            sqlite3_result_error(ctx, "Invalid numeric value", SQLITE_MISMATCH);
            return false;
        }
    }


    static void unaryFunction(sqlite3_context* ctx, sqlite3_value **argv, double (*fn)(double)) {
        sqlite3_value *arg = argv[0];
        if (_usuallyTrue(isNumeric(ctx, arg)))
            sqlite3_result_double(ctx, fn(sqlite3_value_double(arg)));
    }

    #define DefineUnaryMathFn(NAME, C_FN) \
        static void fl_##NAME(sqlite3_context* ctx, int argc, sqlite3_value **argv) { \
            unaryFunction(ctx, argv, C_FN); \
        }

    DefineUnaryMathFn(abs,   abs)
    DefineUnaryMathFn(acos,  acos)
    DefineUnaryMathFn(asin,  asin)
    DefineUnaryMathFn(atan,  atan)
    DefineUnaryMathFn(ceil,  ceil)
    DefineUnaryMathFn(cos,   cos)
    DefineUnaryMathFn(degrees, [](double rad) {return rad * 180 / M_PI;})
    DefineUnaryMathFn(exp,   exp)
    DefineUnaryMathFn(floor, floor)
    DefineUnaryMathFn(ln,    log)
    DefineUnaryMathFn(log,   log10)
    DefineUnaryMathFn(radians, [](double deg) {return deg * M_PI / 180;})
    DefineUnaryMathFn(sin,   sin)
    DefineUnaryMathFn(sqrt,  sqrt)
    DefineUnaryMathFn(tan,   tan)


    // atan2(x, y) returns the arctangent of y/x, i.e. the angle of the vector from the origin to
    // (x, y). It works correctly in all quadrants, and when x=0.
    static void fl_atan2(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumeric(ctx, argv[0]) && isNumeric(ctx, argv[1]))
            sqlite3_result_double(ctx, atan2(sqlite3_value_double(argv[1]),
                                             sqlite3_value_double(argv[0])));
    }

    static void fl_power(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumeric(ctx, argv[0]) && isNumeric(ctx, argv[1]))
            sqlite3_result_double(ctx, pow(sqlite3_value_double(argv[0]),
                                           sqlite3_value_double(argv[1])));
    }

    static void fl_e(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        sqlite3_result_double(ctx, M_E);
    }

    static void fl_pi(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        sqlite3_result_double(ctx, M_PI);
    }

    static void roundTo(sqlite3_context* ctx, int argc, sqlite3_value **argv, double (*fn)(double)) {
        // Takes an optional 2nd argument giving the number of decimal places to round to.
        if (!isNumeric(ctx, argv[0]))
            return;
        double result = sqlite3_value_double(argv[0]);

        if(argc == 1) {
            result = fn(result);
        } else {
            if (!isNumeric(ctx, argv[1]))
                return;
            double scale = pow(10, sqlite3_value_double(argv[1]));
            result = fn(result * scale) / scale;
        }

        sqlite3_result_double(ctx, result);
    }

    // round(n) returns the value of `n` rounded to the nearest integer.
    // round(n, places) rounds n to `places` decimal places.
    static void fl_round(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        roundTo(ctx, argc, argv, round);
    }

    // trunc(n, [places]) is like round(), but truncates, i.e. rounds toward zero.
    static void fl_trunc(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        roundTo(ctx, argc, argv, trunc);
    }

    // sign(n) returns the numeric sign of `n` as either -1, 0, or 1.
    static void fl_sign(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (!isNumeric(ctx, argv[0]))
            return;
        double num = sqlite3_value_double(argv[0]);
        sqlite3_result_int(ctx, num > 0 ? 1 : (num < 0 ? -1 : 0) );
    }


#pragma mark - DATES:


    static bool parseDateArg(sqlite3_value *arg, int64_t *outTime) {
        auto str = stringSliceArgument(arg);
        return str && kInvalidDate != (*outTime = ParseISO8601Date(str));
    }

    static void setResultDateString(sqlite3_context* ctx, int64_t millis, bool asUTC) {
        char buf[kFormattedISO8601DateMaxSize];
        setResultTextFromSlice(ctx, FormatISO8601Date(buf, millis, asUTC));
    }

    static void millis_to_utc(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumericNoError(argv[0])) {
            int64_t millis = sqlite3_value_int64(argv[0]);
            setResultDateString(ctx, millis, true);
        }
    }

    static void millis_to_str(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        if (isNumericNoError(argv[0])) {
            int64_t millis = sqlite3_value_int64(argv[0]);
            setResultDateString(ctx, millis, false);
        }
    }

    static void str_to_millis(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        int64_t millis;
        if (parseDateArg(argv[0], &millis))
            sqlite3_result_int64(ctx, millis);
    }

    static void str_to_utc(sqlite3_context* ctx, int argc, sqlite3_value **argv) {
        int64_t millis;
        if (parseDateArg(argv[0], &millis))
            setResultDateString(ctx, millis, true);
    }


#pragma mark - TYPE TESTS & CONVERSIONS:


    static const string value_type(sqlite3_context* ctx, sqlite3_value *arg) {
        switch(sqlite3_value_type(arg)) {
            case SQLITE_FLOAT:
                return "number";
            case SQLITE_INTEGER:
                return sqlite3_value_subtype(arg) == kFleeceIntBoolean ?
                "boolean" : "number";
            case SQLITE_TEXT:
                return "string";
            case SQLITE_NULL:
                return "missing";
            case SQLITE_BLOB:
            {
                auto fleece = fleeceParam(ctx, arg);
                if(fleece == nullptr) {
                    return "null";
                }

                switch(fleece->type()) {
                    case valueType::kArray:
                        return "array";
                    case valueType::kBoolean:
                        return "boolean";
                    case valueType::kData:
                        return "binary";
                    case valueType::kDict:
                        return "object";
                    case valueType::kNull:
                        return "null";
                    case valueType::kNumber:
                        return "number";
                    case valueType::kString:
                        return "string";
                }
            }
            default:
                return "missing";
        }
    }

    // isarray(v) returns true if `v` is an array.
    static void isarray(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "array" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // isatom(v) returns true if `v` is a boolean, number or string.
    static void isatom(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto type = value_type(ctx, argv[0]);
        int result = (type == "boolean" || type == "number" || type == "string") ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // isboolean(v) returns true if `v` is a boolean. (Since SQLite doesn't distinguish between
    // booleans and integers, this will return false if a boolean value has gone through SQLite.)
    static void isboolean(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "boolean" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // isnumber(v) returns true if `v` is a number.
    static void isnumber(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "number" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // isobject(v) returns true if `v` is a dictionary.
    static void isobject(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "object" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // isatom(v) returns true if `v` is a string.
    static void isstring(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        int result =  value_type(ctx, argv[0]) == "string" ? 1 : 0;
        sqlite3_result_int(ctx, result);
    }

    // type(v) returns a string naming the type of `v`.
    static void type(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto result =  value_type(ctx, argv[0]);
        sqlite3_result_text(ctx, result.c_str(), (int)result.size(), SQLITE_TRANSIENT);
    }

    // toatom(v) returns a boolean/number/string derived from `v`:
    // MISSING is MISSING.
    // NULL is NULL.
    // Arrays of length 1 are the result of TOATOM() on their single element.
    // Objects of length 1 are the result of TOATOM() on their single value.
    // Booleans, numbers, and strings are themselves.
    // All other values are NULL.
    static void toatom(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        auto arg = argv[0];
        if (sqlite3_value_type(arg) != SQLITE_BLOB) {
            // Standard SQLite types map to themselves.
            sqlite3_result_value(ctx, arg);
            return;
        }

        auto fleece = fleeceParam(ctx, arg);
        if (!fleece)
            return;

        switch(fleece->type()) {
            case valueType::kArray:
            {
                auto arr = fleece->asArray();
                if(arr->count() != 1) {
                    setResultFleeceNull(ctx);
                    break;
                }

                setResultFromValue(ctx, arr->get(0));
                break;
            }
            case valueType::kDict:
            {
                auto dict = fleece->asDict();
                if(dict->count() != 1) {
                    setResultFleeceNull(ctx);
                    break;
                }

                auto iter = dict->begin();
                setResultFromValue(ctx, iter.value());
                break;
            }
            default:
                // Other Fleece types map to themselves:
                sqlite3_result_value(ctx, arg);
                break;
        }
    }

    // toboolean(v) returns a boolean derived from `v`:
    // MISSING is MISSING.
    // NULL is NULL.
    // False is false.
    // Numbers +0, -0, and NaN are false.
    // Empty strings, arrays, and objects are false.
    // All other values are true.
    static void toboolean(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        bool result;
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            {
                auto val = sqlite3_value_double(argv[0]);
                result = (val != 0.0 && !std::isnan(val));
                break;
            }
            case SQLITE_TEXT:
            {
                // Need to call sqlite3_value_text here?
                result = sqlite3_value_bytes(argv[0]) > 0;
                break;
            }
            case SQLITE_BLOB:
            {
                auto fleece = fleeceParam(ctx, argv[0]);
                if (fleece == nullptr) {
                    result = false;
                } else switch(fleece->type()) {
                    case valueType::kArray:
                        result = fleece->asArray()->count() > 0;
                        break;
                    case valueType::kData:
                        result = fleece->asData().size > 0;
                        break;
                    case valueType::kDict:
                        result = fleece->asDict()->count() > 0;
                        break;
                    case valueType::kNull:
                        sqlite3_result_value(ctx, argv[0]);
                        return;
                    default:
                        // Other Fleece types never show up in blobs
                        result = false;
                        break;
                }
                break;
            }
            default:
                result = true;
                break;
        }
        sqlite3_result_int(ctx, result);
        sqlite3_result_subtype(ctx, kFleeceIntBoolean);
    }

    static double tonumber(const string &s) {
        try {
            return ParseDouble(s.c_str());
        } catch (const invalid_argument&) {
            return NAN;
        } catch (const out_of_range&) {
            return NAN;
        }
    }

    // tonumber(v) returns a number derived from `v`:
    // MISSING is MISSING.
    // NULL is NULL.
    // False is 0.
    // True is 1.
    // Numbers are themselves.
    // Strings that parse as numbers are those numbers.
    // All other values are NULL.
    static void tonumber(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            case SQLITE_INTEGER:
            {
                sqlite3_result_value(ctx, argv[0]);
                break;
            }
            case SQLITE_TEXT:
            {
                auto txt = (const char *)sqlite3_value_text(argv[0]);
                string str(txt, sqlite3_value_bytes(argv[0]));
                double result = tonumber(str);
                if(std::isnan(result)) {
                    setResultFleeceNull(ctx);
                } else {
                    sqlite3_result_double(ctx, result);
                }
                break;
            }
            case SQLITE_BLOB:
            {
                // A blob is a Fleece array, dict, or data; all of which result in NULL.
                setResultFleeceNull(ctx);
                break;
            }

        }
    }

    // tostring(v) returns a string derived from `v`:
    // MISSING is MISSING.
    // NULL is NULL.
    // False is "false".
    // True is "true".
    // Numbers are their string representation.
    // Strings are themselves.
    // All other values are NULL.
    static void tostring(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        switch(sqlite3_value_type(argv[0])) {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_FLOAT:
            {
                auto num = sqlite3_value_double(argv[0]);
                auto str = to_string(num);
                sqlite3_result_text(ctx, str.c_str(), (int)str.size(), SQLITE_TRANSIENT);
                break;
            }
            case SQLITE_INTEGER:
            {
                
                auto num = sqlite3_value_int64(argv[0]);
                string str;
                if(sqlite3_value_subtype(argv[0]) == kFleeceIntBoolean) {
                    str = num == 1 ? "true" : "false";
                } else {
                    str = to_string(num);
                }
                
                sqlite3_result_text(ctx, str.c_str(), (int)str.size(), SQLITE_TRANSIENT);
                break;
            }
            case SQLITE_TEXT:
            {
                sqlite3_result_value(ctx, argv[0]);
                break;
            }
            case SQLITE_BLOB:
            {
                // A blob is a Fleece array, dict, or data; all of which result in NULL.
                setResultFleeceNull(ctx);
                break;
            }
        }
    }


#pragma mark - REGISTRATION:


    // LCOV_EXCL_START
    // placeholder implementation for unimplemented functions; just returns a SQLite error.
    static void unimplemented(sqlite3_context* ctx, int argc, sqlite3_value **argv) noexcept {
        Warn("Calling unimplemented N1QL function; query will fail");
        sqlite3_result_error(ctx, "unimplemented N1QL function", -1);
    }
    // LCOV_EXCL_STOP


    const SQLiteFunctionSpec kN1QLFunctionsSpec[] = {
        { "array_agg",         1, nullptr, array_agg_step, array_agg_final },
//        { "array_append",     -1, unimplemented },
        { "array_avg",        -1, fl_array_avg },
//        { "array_concat",     -1, unimplemented },
        { "array_contains",   -1, fl_array_contains },
        { "array_count",      -1, fl_array_count },
//        { "array_distinct",    1, unimplemented },
//        { "array_flatten",     2, unimplemented },
//        { "array_agg",         1, unimplemented },
        { "array_ifnull",     -1, fl_array_ifnull },
//        { "array_insert",     -1, unimplemented },
//        { "array_intersect",  -1, unimplemented },
        { "array_length",     -1, fl_array_length },
        { "array_max",        -1, fl_array_max },
        { "array_min",        -1, fl_array_min },
//        { "array_position",    2, unimplemented },
//        { "array_prepend",    -1, unimplemented },
//        { "array_put",        -1, unimplemented },
//        { "array_range",       2, unimplemented },
//        { "array_range",       3, unimplemented },
//        { "array_remove",     -1, unimplemented },
//        { "array_repeat",      2, unimplemented },
//        { "array_replace",     3, unimplemented },
//        { "array_replace",     4, unimplemented },
//        { "array_reverse",     1, unimplemented },
//        { "array_sort",        1, unimplemented },
//        { "array_star",        1, unimplemented },
        { "array_sum",        -1, fl_array_sum },
//        { "array_symdiff",    -1, unimplemented },
//        { "array_symdiffn",   -1, unimplemented },
//        { "array_union",      -1, unimplemented },

        { "ifmissing",        -1, ifmissing },
        { "ifmissingornull",  -1, ifmissingornull },
        { "N1QL_ifnull",      -1, ifnull },
        { "missingif",         2, missingif },
        { "N1QL_nullif",       2, nullif },

//        { "ifinf",            -1, ifinf },
//        { "isnan",            -1, ifnan },
//        { "isnanorinf",       -1, ifnanorinf },
//        { "nanif",             2, nanif },
//        { "neginfif",          2, neginfif },
//        { "posinfif",          2, posinfif },

//        { "base64",            1, fl_base64 },
//        { "base64_encode",     1, fl_base64 },
//        { "base64_decode",     1, fl_base64_decode },

        { "contains",          2, contains },
        { "contains",          3, contains },
//        { "initcap",           1, init_cap },
        { "N1QL_length",       1, length },
        { "N1QL_lower",        1, lower },
        { "N1QL_ltrim",        1, ltrim },
        { "N1QL_ltrim",        2, ltrim },
//        { "position",          2, position },
//        { "repeat",            2, repeat },
//        { "replace",           3, replace },
//        { "replace",           4, replace },
//        { "reverse",           1, reverse },
        { "N1QL_rtrim",        1, rtrim },
        { "N1QL_rtrim",        2, rtrim },
//        { "split",             1, unimplemented },
//        { "split",             2, unimplemented },
//        { "substr",            2, substr },
//        { "substr",            3, substr },
//        { "suffixes",          1, unimplemented },
//        { "title",             1, init_cap },
//        { "tokens",            2, unimplemented },
        { "N1QL_trim",         1, trim },
        { "N1QL_trim",         2, trim },
        { "N1QL_upper",        1, upper },

        { "regexp_contains",   2, regexp_like, },
        { "regexp_like",       2, regexp_like },
        { "regexp_position",   2, regexp_position },
        { "regexp_replace",    3, regexp_replace },
        { "regexp_replace",    4, regexp_replace },

        { "isarray",           1, isarray },
        { "isatom",            1, isatom },
        { "isboolean",         1, isboolean },
        { "isnumber",          1, isnumber },
        { "isobject",          1, isobject },
        { "isstring",          1, isstring },
        { "type",              1, type },
        { "toarray",           1, unimplemented },
        { "toatom",            1, toatom },
        { "toboolean",         1, toboolean },
        { "tonumber",          1, tonumber },
        { "toobject",          1, unimplemented },
        { "tostring",          1, tostring },

        { "abs",               1, fl_abs },
        { "acos",              1, fl_acos },
        { "asin",              1, fl_asin },
        { "atan",              1, fl_atan },
        { "atan2",             2, fl_atan2 },
        { "ceil",              1, fl_ceil },
        { "cos",               1, fl_cos },
        { "degrees",           1, fl_degrees },
        { "e",                 0, fl_e },
        { "exp",               1, fl_exp },
        { "floor",             1, fl_floor },
        { "ln",                1, fl_ln },
        { "log",               1, fl_log },
        { "pi",                0, fl_pi },
        { "power",             2, fl_power },
        { "radians",           1, fl_radians },
        { "round",             1, fl_round },
        { "round",             2, fl_round },
        { "sign",              1, fl_sign },
        { "sin",               1, fl_sin },
        { "sqrt",              1, fl_sqrt },
        { "tan",               1, fl_tan },
        { "trunc",             1, fl_trunc },
        { "trunc",             2, fl_trunc },

        { "millis_to_str",     1, millis_to_str },
        { "millis_to_utc",     1, millis_to_utc },
        { "str_to_millis",     1, str_to_millis },
        { "str_to_utc",        1, str_to_utc },

        { }
    };

}
