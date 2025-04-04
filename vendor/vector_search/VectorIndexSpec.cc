//
// IndexSpec.cc
//
// 
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

// NOTE: This file appears in both the vectorsearch and couchbase-lite-core repos.
// Any changes made in one should be copied to the other!

#include "VectorIndexSpec.hh"
#include <cinttypes>
#include <cstring>
#include <iostream>
#include <sstream>

#ifdef SQLITECPP_BUILDING_EXTENSION
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT3
#else
#include <sqlite3.h>  // for sqlite3_log
#endif

#ifdef _MSC_VER
#define strncasecmp(x,y,z) _strnicmp(x,y,z) // https://stackoverflow.com/questions/7299119
#endif

namespace vectorsearch {
    using namespace std;


#pragma mark - VALIDATION:


    static void check(bool condition, const char* what, const char* problem =nullptr) {
        if (!condition) {
            string message = "invalid vector index spec: ";
            message += what;
            if (problem)
                message += problem;
            throw std::invalid_argument(message);
        }
    }

    template <typename T>
    static void check(T value, T minVal, T maxVal, const char* what) {
        check(value >= minVal, what, " parameter is too small");
        check(value <= maxVal, what, " parameter is too large");
    }


    void IndexSpec::validate() const {
        check(dimensions, kMinDimensions, kMaxDimensions, "dimension");
        check(metric, Metric::Euclidean2, Metric::MaxValue, "metric");
        switch (clusteringType()) {
            case ClusteringType::Flat: {
                auto &c = std::get<FlatClustering>(clustering);
                check(c.numCentroids,
                      kMinFlatClustering.numCentroids,
                      kMaxFlatClustering.numCentroids, 
                      "centroids");
                break;
            }
            case ClusteringType::MultiIndex: {
                auto& c = std::get<MultiIndexClustering>(clustering);
                check(c.subquantizers,
                      kMinMultiIndexClustering.subquantizers,
                      kMaxMultiIndexClustering.subquantizers,
                      "clustering subquantizers");
                check(c.bitsPerSub,
                      kMinMultiIndexClustering.bitsPerSub,
                      kMaxMultiIndexClustering.bitsPerSub,
                      "clustering bits");
                check(dimensions % c.subquantizers == 0,
                      "clustering subquantizers must evenly divide the number of dimensions");
                break;
            }
        }

        if (probeCount)
            check(*probeCount, 1u, numCentroids(), "probe count");

        switch (encodingType()) {
            case EncodingType::None:
                break;
            case EncodingType::PQ: {
                auto& e = std::get<PQEncoding>(encoding);
                check(e.subquantizers,
                      kMinPQEncoding.subquantizers,
                      kMaxPQEncoding.subquantizers,
                      "encoding subquantizers");
                check(e.bitsPerSub,
                      kMinPQEncoding.bitsPerSub,
                      kMaxPQEncoding.bitsPerSub,
                      "encoding bits");
                check(dimensions % e.subquantizers == 0,
                      "encoding subquantizers must evenly divide the number of dimensions");
                break;
            }
            case EncodingType::SQ: {
                auto& e = std::get<SQEncoding>(encoding);
                check(e.bitsPerDimension == 4 || e.bitsPerDimension == 6 || e.bitsPerDimension == 8,
                      "encoding bits must be 4, 6 or 8");
                break;
            }
        }
    }


    void IndexSpec::resolveTrainingCounts() {
        // If maxTrainingCount wasn't given or is zero, set it to a reasonable value:
        unsigned nCent = numCentroidsToTrain();
        if (!maxTrainingCount || *maxTrainingCount == 0)
            maxTrainingCount = kRecommendedMaxTrainingVectorsPerCentroid * nCent;

        if (!minTrainingCount || *minTrainingCount == 0) {
            // If minTrainingCount wasn't given, set a default value.
            // (kRecommendedMinTrainingVectorsPerCentroid would be better,
            // but would break compatibility.)
            minTrainingCount = kMinTrainingVectorsPerCentroid * nCent;
        } else if (int64_t m = kMinTrainingVectorsPerCentroid * nCent; *minTrainingCount < m) {
            sqlite3_log(SQLITE_WARNING,
                        "vectorsearch: minTrainingSize of %" PRIi64 " is too small;"
                        " raising it to %" PRIi64 ", based on %u centroids.",
                        *minTrainingCount, m, nCent);
            minTrainingCount = m;
        }
    }


    int64_t IndexSpec::effectiveTrainingCount(int64_t numVectors) const {
        auto count = numVectors;
        if (minTrainingCount && count < *minTrainingCount)
            return 0;
        auto trainingCentroids = numCentroidsToTrain();
        auto needed = trainingCentroids * kMinTrainingVectorsPerCentroid;
        if (count < needed)
            return 0;   // Not enough vectors to train on
        if (maxTrainingCount)
            count = min(count, max(*maxTrainingCount, needed));
        count = min(count, trainingCentroids * kMaxTrainingVectorsPerCentroid);
        return count;
    }


#pragma mark - PARSING:


    static constexpr string_view kNameOfMetric[] = {
        "euclidean2",                                           // Name recognized by LiteCore 3.2
        "cosine",                                               // Name recognized by LiteCore 3.2
        "euclidean",
        "cosine_similarity",
        "dot",
        "dot_product_similarity"
    };

    static_assert(std::size(kNameOfMetric) == size_t(Metric::MaxValue)+1);

    static constexpr pair<string_view,Metric> kMetricNames[] = { // (These are case-insensitive)
        {"euclidean",               Metric::Euclidean},             // used by SQL++
        {"L2",                      Metric::Euclidean},             // used by SQL++
        {"euclidean2",              Metric::Euclidean2},            // used by LiteCore 3.2
        {"L2_squared",              Metric::Euclidean2},            // used by SQL++
        {"euclidean_squared",       Metric::Euclidean2},            // used by SQL++
        {"cosine",                  Metric::CosineDistance},        // used by SQL++ and LiteCore 3.2
        {"dot",                     Metric::DotProductDistance},    // used by SQL++
        {"cosine_distance",         Metric::CosineDistance},
        {"cosine_similarity",       Metric::CosineSimilarity},
        {"dot_product_distance",    Metric::DotProductDistance},
        {"dot_product_similarity",  Metric::DotProductSimilarity},
        {"default",                 Metric::Default},
    };


    string_view NameOfMetric(Metric m) {
        auto val = int(m);
        if (val < 0 || val > int(Metric::MaxValue))
            throw std::invalid_argument("invalid Metric value");
        return kNameOfMetric[val];
    }

    optional<Metric> MetricNamed(string_view name) {
        auto len = name.size();
        for (auto [aName, aMetric] : kMetricNames) {
            if (aName.size() == len && 0 == strncasecmp(aName.data(), name.data(), len))
                return aMetric;
        }
        return nullopt;
    }


    static bool popPrefix(string_view &str, string_view prefix) {
        auto prefixLen = prefix.size();
        if (prefixLen > str.size() || prefix != str.substr(0, prefixLen))
            return false;
        str = str.substr(prefixLen);
        return true;
    }

    static unsigned asUInt(string_view str, string_view forKey) {
        try {
            return unsigned(std::stoul(string(str)));
        } catch (...) {
            throw invalid_argument("invalid numeric value '"s + string(str) + "' for " + string(forKey));
        }
    }

    static bool asBool(string_view str) {
        return str != "false" && str != "0";
    }


    static pair<unsigned,unsigned> readPQ(string_view value, string_view forKey) {
        if (auto x = value.find('x'); x != string::npos)
            return { asUInt(value.substr(0, x), forKey), asUInt(value.substr(x + 1), forKey) };
        else
            throw invalid_argument("value of '"s + string(forKey) +
                                   " must be of form <subquantizers> x <bits>, e.g. 32x8");
    }


    bool IndexSpec::readArg(std::string_view key, std::string_view value) {
        if (key == "dimensions") {
            dimensions = asUInt(value, "dimensions");
        } else if (key == "metric") {
            optional<Metric> m = MetricNamed(value);
            check(m.has_value(), "unknown metric");
            metric = m.value();
        } else if (key == "clustering") {
            if (popPrefix(value, "flat")) {
                clustering = FlatClustering{asUInt(value, key)};
            } else if (popPrefix(value, "multi")) {
                auto [sub, bits] = readPQ(value, key);
                clustering = MultiIndexClustering{sub, bits};
            } else {
                throw std::invalid_argument("unknown clustering");
            }
        } else if (key == "centroids") {
            clustering = FlatClustering{asUInt(value, "centroid count")};
        } else if (key == "encoding") {
            if (value == "none")
                encoding = NoEncoding{};
            else if (popPrefix(value, "PQ")) {
                auto [sub, bits] = readPQ(value, "PQ encoding");
                encoding = PQEncoding(sub, bits);
            } else if (popPrefix(value, "SQ")) {
                unsigned v = 8;
                if (!value.empty())
                    v = asUInt(value, "SQ encoding");
                if (v == 4 || v == 6 || v == 8)
                    encoding = SQEncoding{v};
                else
                    throw std::invalid_argument("invalid bits for SQ encoding");
            } else {
                throw std::invalid_argument("unknown encoding");
            }
        } else if (key == "minToTrain") {
            if (value == "never")
                minTrainingCount = kNeverTrain;
            else
                minTrainingCount = asUInt(value, "min training size");
        } else if (key == "maxToTrain") {
            maxTrainingCount = asUInt(value, "max training size");
        } else if (key == "probes") {
            probeCount = asUInt(value, "probe count");
        } else if (key == "lazyindex") {
            lazyEncoding = asBool(value);
        } else if (key == "lazyembedding") {
            lazyEmbedding = asBool(value);
        } else {
            return false; // unknown key
        }
        return true; // fall through = success
    }


    bool IndexSpec::readArg(string_view arg) {
        if (arg.empty())
            return true;    // no-op
        string_view value;
        if (auto eq = arg.find('='); eq != string::npos) {
            if (eq == 0 || eq == arg.size())
                throw std::invalid_argument("invalid virtual-table argument " + string(arg));
            value = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        }
        return readArg(arg, value);
    }


    void IndexSpec::readArgs(string_view args) {
        while (!args.empty()) {
            string_view arg;
            if (auto comma = args.find(','); comma != string::npos) {
                arg = args.substr(0, comma);
                args = args.substr(comma + 1);
            } else {
                arg = args;
                args = "";
            }
            if (!readArg(arg))
                throw std::invalid_argument("unknown virtual-table argument " + string(arg));
        }
    }



#pragma mark - GENERATING TEXT:


    std::ostream& IndexSpec::writeArgs(std::ostream& out) const {
        out << "dimensions=" << dimensions;
        if (metric != Metric::Default)
            out << ",metric=" << NameOfMetric(metric);
        switch (clusteringType()) {
            case ClusteringType::Flat: {
                auto& c = std::get<FlatClustering>(clustering);
                out << ",clustering=flat" << c.numCentroids;
                break;
            }
            case ClusteringType::MultiIndex: {
                auto& c = std::get<MultiIndexClustering>(clustering);
                out << ",clustering=multi" << c.subquantizers << 'x' << c.bitsPerSub;
                break;
            }
        }
        switch (encodingType()) {
            case EncodingType::None:
                out << ",encoding=none";
                break;
            case EncodingType::PQ: {
                auto& e = std::get<PQEncoding>(encoding);
                out << ",encoding=PQ" << e.subquantizers << 'x' << e.bitsPerSub;
                break;
            }
            case EncodingType::SQ: {
                auto& e = std::get<SQEncoding>(encoding);
                out << ",encoding=SQ" << e.bitsPerDimension;
                break;
            }
        }
        if (minTrainingCount)
            out << ",minToTrain=" << *minTrainingCount;
        if ( maxTrainingCount )
            out << ",maxToTrain=" << *maxTrainingCount;
        if ( probeCount )
            out << ",probes=" << *probeCount;
        if (lazyEncoding)
            out << ",lazyindex=true";
        if (lazyEmbedding)
            out << ",lazyembedding=true";
        return out;
    }


    string IndexSpec::createArgs() const {
        stringstream stmt;
        writeArgs(stmt);
        return stmt.str();
    }


    std::string IndexSpec::description() const {
        stringstream out;
        switch (clusteringType()) {
            case ClusteringType::Flat:
                out << get<FlatClustering>(clustering).numCentroids << " centroids, ";
                break;
            case ClusteringType::MultiIndex: {
                auto& miq = get<MultiIndexClustering>(clustering);
                out << "multi-index quantizer (" << miq.subquantizers << " subquantizers × "
                << miq.bitsPerSub << " bits), ";
                break;
            }
        }
        switch(encodingType()) {
            case EncodingType::None:
                out << " no encoding";
                break;
            case EncodingType::PQ: {
                auto& pq = get<PQEncoding>(encoding);
                out << "PQ encoding (" << pq.subquantizers << " subquantizers × "
                << pq.bitsPerSub << " bits)";
                break;
            }
            case EncodingType::SQ: {
                auto& sq = get<SQEncoding>(encoding);
                out << "SQ encoding (" << sq.bitsPerDimension << " bits)";
                break;
            }
        }
        return out.str();
    }


}
