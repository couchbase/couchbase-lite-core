//
//  c4Key.cc
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4Key.h"
#include "c4Document.h"
#include "Collatable.hh"
#include <math.h>
#include <limits.h>
using namespace cbforest;


C4Key* c4key_new()                              {return new c4Key();}
C4Key* c4key_withBytes(C4Slice bytes)           {return new c4Key(bytes);}
void c4key_free(C4Key *key)                     {delete key;}
void c4key_addNull(C4Key *key)                  {key->addNull();}
void c4key_addBool(C4Key *key, bool b)          {key->addBool(b);}
void c4key_addNumber(C4Key *key, double n)      {*key << n;}
void c4key_addString(C4Key *key, C4Slice str)   {*key << str;}
void c4key_addMapKey(C4Key *key, C4Slice mapKey){*key << mapKey;}
void c4key_beginArray(C4Key *key)               {key->beginArray();}
void c4key_endArray(C4Key *key)                 {key->endArray();}
void c4key_beginMap(C4Key *key)                 {key->beginMap();}
void c4key_endMap(C4Key *key)                   {key->endMap();}

// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice. So these functions use pointer-casting to reinterpret C4KeyReader as CollatableReader.

static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


C4KeyReader c4key_read(const C4Key *key) {
    CollatableReader r(*key);
    return asKeyReader(r);
}

/** for java binding */
C4KeyReader* c4key_newReader(const C4Key *key){
    return (C4KeyReader*)new CollatableReader(*key);
}

/** Free a C4KeyReader */
void c4key_freeReader(C4KeyReader* r){
    delete r;
}

C4KeyToken c4key_peek(const C4KeyReader* r) {
    static const C4KeyToken tagToType[] = {kC4EndSequence, kC4Null, kC4Bool, kC4Bool, kC4Number,
                                    kC4Number, kC4String, kC4Array, kC4Map, kC4Error, kC4Special};
    Collatable::Tag t = ((CollatableReader*)r)->peekTag();
    if (t >= sizeof(tagToType)/sizeof(tagToType[0]))
        return kC4Error;
    return tagToType[t];
}

void c4key_skipToken(C4KeyReader* r) {
    ((CollatableReader*)r)->skipTag();
}

bool c4key_readBool(C4KeyReader* r) {
    bool result = false;
    try {
        result = ((CollatableReader*)r)->peekTag() >= CollatableReader::kTrue;
        ((CollatableReader*)r)->skipTag();
    } catchError(NULL)
    return result;
}

double c4key_readNumber(C4KeyReader* r) {
    try {
    return ((CollatableReader*)r)->readDouble();
    } catchError(NULL)
    return nan("err");  // ????
}

C4SliceResult c4key_readString(C4KeyReader* r) {
    slice s;
    try {
        s = ((CollatableReader*)r)->readString().copy();
        //OPT: This makes an extra copy because I can't find a way to 'adopt' the allocated block
        // managed by the alloc_slice's std::shared_ptr.
    } catchError(NULL)
    return {s.buf, s.size};
}

C4SliceResult c4key_toJSON(const C4KeyReader* r) {
    if (!r || r->length == 0)
        return {NULL, 0};
    std::string str = ((CollatableReader*)r)->toJSON();
    auto s = ((slice)str).copy();
    return {s.buf, s.size};
}
