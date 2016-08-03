//
//  CBForestTest.cc
//  CBForest
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "CBForestTest.hh"
#include "ForestDatabase.hh"
#include <stdlib.h>
#include <unistd.h>


using namespace std;


void Log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}


string stringWithFormat(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *cstr;
    vasprintf(&cstr, format, args);
    va_end(args);
    string str(cstr);
    free(cstr);
    return str;
}


std::string sliceToHex(slice result) {
    std::string hex;
    for (size_t i = 0; i < result.size; i++) {
        char str[4];
        sprintf(str, "%02X", result[i]);
        hex.append(str);
        if (i % 2 && i != result.size-1)
            hex.append(" ");
    }
    return hex;
}


std::string sliceToHexDump(slice result, size_t width) {
    std::string hex;
    for (size_t row = 0; row < result.size; row += width) {
        size_t end = std::min(row + width, result.size);
        for (size_t i = row; i < end; ++i) {
            char str[4];
            sprintf(str, "%02X", result[i]);
            hex.append(str);
            if (i % 2 && i != result.size-1)
                hex.append(" ");
        }
        hex.append("    ");
        for (size_t i = row; i < end; ++i) {
            char str[2] = {(char)result[i], 0};
            if (result[i] < 32 || result[i] >= 127)
                str[0] = '.';
            hex.append(str);
        }
        hex.append("\n");
    }
    return hex;
}


void randomBytes(slice dst) {
    arc4random_buf((void*)dst.buf, dst.size);
}


std::ostream& operator<< (std::ostream& o, slice s) {
    o << "slice[";
    if (s.buf == NULL)
        return o << "null]";
    auto buf = (const uint8_t*)s.buf;
    for (size_t i = 0; i < s.size; i++) {
        if (buf[i] < 32 || buf[i] > 126)
            return o << sliceToHex(s) << "]";
    }
    return o << "\"" << std::string((char*)s.buf, s.size) << "\"]";
}


Database* DatabaseTestFixture::newDatabase(std::string path, Database::Options *options) {
    //TODO: Set up options
    return new ForestDatabase(path, options);
}


void DatabaseTestFixture::reopenDatabase(Database::Options *newOptions) {
    auto dbPath = db->filename();
    auto options = db->options();
    Log("//// Closing db");
    delete db;
    db = nullptr;
    store = nullptr;
    Log("//// Reopening db");
    db = newDatabase(dbPath, newOptions ? newOptions : &options);
    store = &db->defaultKeyStore();
}


void DatabaseTestFixture::setUp() {
    TestFixture::setUp();
    const char *dbPath = kTestDir "forest_temp.fdb";
    Database::deleteDatabase(dbPath);
    db = newDatabase(dbPath);
    store = &db->defaultKeyStore();
}


void DatabaseTestFixture::tearDown() {
    delete db;
    TestFixture::tearDown();
}
