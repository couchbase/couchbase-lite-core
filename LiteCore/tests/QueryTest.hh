//
// QueryTest.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "DataFile.hh"
#include "Query.hh"
#include "Error.hh"
#include "FleeceImpl.hh"
#include "Benchmark.hh"
#include "StringUtil.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace fleece::impl;
using namespace std;


// NOTE: This test does not use RevTree or Database, so it stores plain Fleece in record bodies.


class QueryTest : public DataFileTestFixture {
protected:

    string numberString(int n) {
        static const char* kDigit[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
        string str;
        do {
            str = string(kDigit[n % 10]) + str;
            n /= 10;
            if (n > 0)
                str = string("-") + str;
        } while (n > 0);
        return str;
    }

    sequence_t writeNumberedDoc(int i, slice str, Transaction &t,
                                       DocumentFlags flags =DocumentFlags::kNone) {
        string docID = stringWithFormat("rec-%03d", i);

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("num");
        enc.writeInt(i);
        if (str) {
            enc.writeKey("str");
            enc.writeString(str);
        }
        enc.endDictionary();
        alloc_slice body = enc.finish();

        return store->set(slice(docID), nullslice, body, flags, t);
    }

    // Write 100 docs with Fleece bodies of the form {"num":n} where n is the rec #
    void addNumberedDocs(int first =1, int n =100) {
        Transaction t(store->dataFile());
        for (int i = first; i < first + n; i++)
            REQUIRE(writeNumberedDoc(i, nullslice, t) == (sequence_t)i);
        t.commit();
    }

    sequence_t writeArrayDoc(int i, Transaction &t,
                                    DocumentFlags flags =DocumentFlags::kNone) {
        string docID = stringWithFormat("rec-%03d", i);

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("numbers");
        enc.beginArray();
        for (int j = max(i-5, 1); j <= i; j++)
            enc.writeString(numberString(j));
        enc.endArray();
        enc.endDictionary();
        alloc_slice body = enc.finish();

        return store->set(slice(docID), nullslice, body, flags, t);
    }

    void addArrayDocs(int first =1, int n =100) {
        Transaction t(store->dataFile());
        for (int i = first; i < first + n; i++)
            REQUIRE(writeArrayDoc(i, t) == (sequence_t)i);
        t.commit();
    }

    void writeMultipleTypeDocs(Transaction &t) {
        string docID = "doc1";

        fleece::impl::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.beginArray();
        enc.writeInt(1);
        enc.endArray();
        enc.endDictionary();
        alloc_slice body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc2";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("cool value");
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc3";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeDouble(4.5);
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc4";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.beginDictionary(1);
        enc.writeKey("subvalue");
        enc.writeString("FTW");
        enc.endDictionary();
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc5";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeBool(true);
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
    }

    void writeFalselyDocs(Transaction &t) {
        string docID = "doc6";

        fleece::impl::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.beginArray();
        enc.endArray();
        enc.endDictionary();
        alloc_slice body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc7";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.beginDictionary();
        enc.endDictionary();
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        enc.reset();
        docID = "doc8";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeBool(false);
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
    }

    void deleteDoc(slice docID, bool hardDelete) {
        Transaction t(store->dataFile());
        if (hardDelete) {
            store->del(docID, t);
        } else {
            Record doc = store->get(docID);
            CHECK(doc.exists());
            doc.setFlag(DocumentFlags::kDeleted);
            store->write(doc, t);
        }
        t.commit();
    }

    void undeleteDoc(slice docID) {
        Transaction t(store->dataFile());
        Record doc = store->get(docID);
        CHECK(doc.exists());
        doc.clearFlag(DocumentFlags::kDeleted);
        store->write(doc, t);
        t.commit();
    }

    vector<string> extractIndexes(slice encodedIndexes) {
        set<string> retVal;
        const Array *val = Value::fromTrustedData(encodedIndexes)->asArray();
        CHECK(val != nullptr);
        Array::iterator iter(val);
        int size = iter.count();
        for(int i = 0; i < size; i++, ++iter) {
            retVal.insert(iter.value()->asString().asString());
        }
        return vector<string>(retVal.begin(), retVal.end());
    }

    int64_t rowsInQuery(string json) {
        Retained<Query> query = store->compileQuery(json);
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
        return e->getRowCount();
    }

};
