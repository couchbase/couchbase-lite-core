//
//  ArgumentTokenizer.cc
//  LiteCore
//
//  Created by Jim Borden on 2018/01/06.
//  Copyright © 2018 Couchbase. All rights reserved.
//

#include "argumentTokenizer.hh"
using namespace std;

bool ArgumentTokenizer::tokenize(const char* input, deque<string> &args)
{
    if(input == nullptr) {
        return false;
    }
    
    const char* start = input;
    const char* current = start;
    bool inQuote = false;
    bool forceAppend = false;
    string nextArg;
    while(*current) {
        char c = *current;
        current++;
        if(c == '\r' || c == '\n') {
            continue;
        }
        
        if(!forceAppend) {
            if(c == '\\') {
                forceAppend = true;
                continue;
            } else if(c == '"') {
                inQuote = !inQuote;
                continue;
            } else if(c == ' ' && !inQuote) {
                args.push_back(nextArg);
                nextArg.clear();
                continue;
            }
        } else {
            forceAppend = false;
        }
        
        nextArg.append(1, c);
    }
    
    if(inQuote || forceAppend) {
        return false;
    }
    
    if(nextArg.length() > 0) {
        args.push_back(nextArg);
    }
    
    return true;
}
