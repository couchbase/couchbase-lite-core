#include "ParsedSequenceID.hh"
#include "c4Test.hh"

using namespace litecore::repl;

/** Parse str and assert it succeeds, returning the result. */
static ParsedSequenceID mustParse(const std::string& str) {
    ParsedSequenceID out;
    REQUIRE(ParsedSequenceID::parse(str, out));
    return out;
}

/** Assert that parse(str) fails. */
static void mustFail(const std::string& str) {
    ParsedSequenceID out;
    CHECK_FALSE(ParsedSequenceID::parse(str, out));
}

TEST_CASE("ParsedSequenceID parse - simple revision", "[ParsedSequenceID]") {
    // Format: "Seq"
    // Fields: seq=N, triggeredBy=0, lowSeq=0

    SECTION("single digit") {
        auto s = mustParse("5");
        CHECK(s.seq() == 5);
        CHECK(s.triggeredBy() == 0);
        CHECK(s.lowSeq() == 0);
        CHECK(s.isSimpleRevision());
        CHECK_FALSE(s.isBackfillFormat());
        CHECK_FALSE(s.isLowSeqFormat());
    }

    SECTION("multi digit") {
        auto s = mustParse("12345");
        CHECK(s.seq() == 12345);
        CHECK(s.triggeredBy() == 0);
        CHECK(s.lowSeq() == 0);
        CHECK(s.isSimpleRevision());
    }

    SECTION("zero") {
        auto s = mustParse("0");
        CHECK(s.seq() == 0);
        CHECK(s.isSimpleRevision());
    }

    SECTION("large uint64 value") {
        auto s = mustParse("18446744073709551615");  // UINT64_MAX
        CHECK(s.seq() == UINT64_MAX);
        CHECK(s.isSimpleRevision());
    }
}

TEST_CASE("ParsedSequenceID parse - backfill (TriggeredBy:Seq)", "[ParsedSequenceID]") {
    // Format: "TriggeredBy:Seq"
    // Fields: seq=Seq, triggeredBy=TriggeredBy, lowSeq=0

    SECTION("basic backfill") {
        auto s = mustParse("100:35");
        CHECK(s.triggeredBy() == 100);
        CHECK(s.seq() == 35);
        CHECK(s.lowSeq() == 0);
        CHECK(s.isBackfillFormat());
        CHECK_FALSE(s.isSimpleRevision());
        CHECK_FALSE(s.isLowSeqFormat());
    }

    SECTION("both components equal") {
        auto s = mustParse("50:50");
        CHECK(s.triggeredBy() == 50);
        CHECK(s.seq() == 50);
        CHECK(s.isBackfillFormat());
    }

    SECTION("triggeredBy larger than seq") {
        auto s = mustParse("200:1");
        CHECK(s.triggeredBy() == 200);
        CHECK(s.seq() == 1);
        CHECK(s.isBackfillFormat());
    }
}

TEST_CASE("ParsedSequenceID parse - LowSeq with TriggeredBy (LowSeq:TriggeredBy:Seq)", "[ParsedSequenceID]") {
    // Format: "LowSeq:TriggeredBy:Seq"
    // Fields: seq=Seq, triggeredBy=TriggeredBy, lowSeq=LowSeq

    SECTION("full three-part") {
        auto s = mustParse("20:100:35");
        CHECK(s.lowSeq() == 20);
        CHECK(s.triggeredBy() == 100);
        CHECK(s.seq() == 35);
        CHECK(s.isLowSeqFormat());
        CHECK_FALSE(s.isBackfillFormat());
        CHECK_FALSE(s.isSimpleRevision());
    }

    SECTION("all parts equal") {
        auto s = mustParse("10:10:10");
        CHECK(s.lowSeq() == 10);
        CHECK(s.triggeredBy() == 10);
        CHECK(s.seq() == 10);
        CHECK(s.isLowSeqFormat());
    }
}

TEST_CASE("ParsedSequenceID parse - LowSeq without TriggeredBy (LowSeq::Seq)", "[ParsedSequenceID]") {
    // Format: "LowSeq::Seq" — TriggeredBy is empty, treated as 0

    SECTION("basic lowseq without triggeredby") {
        auto s = mustParse("20::35");
        CHECK(s.lowSeq() == 20);
        CHECK(s.triggeredBy() == 0);
        CHECK(s.seq() == 35);
        CHECK(s.isLowSeqFormat());
    }

    SECTION("lowseq large values") {
        auto s = mustParse("1000::9999");
        CHECK(s.lowSeq() == 1000);
        CHECK(s.triggeredBy() == 0);
        CHECK(s.seq() == 9999);
        CHECK(s.isLowSeqFormat());
    }
}

// ══════════════════════════════════════════════════════════════════
//  parse() — invalid inputs (safety net)
// ══════════════════════════════════════════════════════════════════

TEST_CASE("ParsedSequenceID parse - invalid inputs", "[ParsedSequenceID]") {
    SECTION("empty string")              { mustFail(""); }
    SECTION("non-numeric single")        { mustFail("abc"); }
    SECTION("leading alpha")             { mustFail("abc:35"); }
    SECTION("trailing alpha")            { mustFail("100:abc"); }
    SECTION("too many parts (4)")        { mustFail("1:2:3:4"); }
    SECTION("too many parts (5)")        { mustFail("1:2:3:4:5"); }
    SECTION("trailing colon two-part")   { mustFail("100:"); }
    SECTION("leading colon")             { mustFail(":35"); }
    SECTION("empty lowseq in three-part"){ mustFail("::35"); }
    SECTION("empty seq in three-part")   { mustFail("20:100:"); }
    SECTION("float value")               { mustFail("1.5"); }
    SECTION("negative value")            { mustFail("-1"); }
    SECTION("spaces")                    { mustFail("100 : 35"); }
}

TEST_CASE("ParsedSequenceID format classification", "[ParsedSequenceID]") {
    SECTION("simple is exclusively simple") {
        auto s = mustParse("42");
        CHECK(s.isSimpleRevision());
        CHECK_FALSE(s.isBackfillFormat());
        CHECK_FALSE(s.isLowSeqFormat());
    }

    SECTION("backfill is exclusively backfill") {
        auto s = mustParse("100:35");
        CHECK_FALSE(s.isSimpleRevision());
        CHECK(s.isBackfillFormat());
        CHECK_FALSE(s.isLowSeqFormat());
    }

    SECTION("lowseq with triggeredby is exclusively lowseq") {
        auto s = mustParse("20:100:35");
        CHECK_FALSE(s.isSimpleRevision());
        CHECK_FALSE(s.isBackfillFormat());
        CHECK(s.isLowSeqFormat());
    }

    SECTION("lowseq without triggeredby is still lowseq") {
        auto s = mustParse("20::35");
        CHECK_FALSE(s.isSimpleRevision());
        CHECK_FALSE(s.isBackfillFormat());
        CHECK(s.isLowSeqFormat());
    }
}

TEST_CASE("ParsedSequenceID before - Simple vs Simple", "[ParsedSequenceID]") {
    // Both plain "Seq": compare seq directly.

    SECTION("a < b → a.before(b) = true") {
        CHECK(mustParse("42").before(mustParse("100")));
    }

    SECTION("a > b → a.before(b) = false") {
        CHECK_FALSE(mustParse("100").before(mustParse("42")));
    }

    SECTION("a == b → a.before(b) = false (not strictly before)") {
        CHECK_FALSE(mustParse("42").before(mustParse("42")));
    }

    SECTION("zero is before any positive") {
        CHECK(mustParse("0").before(mustParse("1")));
    }
}

TEST_CASE("ParsedSequenceID before - Backfill vs Backfill", "[ParsedSequenceID]") {
    // Both "TriggeredBy:Seq": compare triggeredBy first, seq as tie-break.

    SECTION("different triggeredBy: smaller triggeredBy is before") {
        // "100:35" before "200:55"
        CHECK(mustParse("100:35").before(mustParse("200:55")));
        CHECK_FALSE(mustParse("200:55").before(mustParse("100:35")));
    }

    SECTION("same triggeredBy, different seq: smaller seq is before") {
        // "100:35" before "100:55"
        CHECK(mustParse("100:35").before(mustParse("100:55")));
        CHECK_FALSE(mustParse("100:55").before(mustParse("100:35")));
    }

    SECTION("identical → not before") {
        CHECK_FALSE(mustParse("100:35").before(mustParse("100:35")));
    }

    SECTION("same triggeredBy, same seq → not before") {
        CHECK_FALSE(mustParse("50:50").before(mustParse("50:50")));
    }
}

TEST_CASE("ParsedSequenceID before - LowSeq vs LowSeq", "[ParsedSequenceID]") {
    // Both "LowSeq:TB:Seq": compare lowSeq first, then inner pair as tie-break.

    SECTION("different lowSeq: smaller lowSeq is before") {
        // "20:100:35" before "25:100:40"
        CHECK(mustParse("20:100:35").before(mustParse("25:100:40")));
        CHECK_FALSE(mustParse("25:100:40").before(mustParse("20:100:35")));
    }

    SECTION("same lowSeq, different triggeredBy: smaller triggeredBy is before") {
        // "20:100:35" before "20:200:35"
        CHECK(mustParse("20:100:35").before(mustParse("20:200:35")));
    }

    SECTION("same lowSeq, same triggeredBy, different seq: smaller seq is before") {
        // "20:100:35" before "20:100:55" (tie-break into inner pair)
        CHECK(mustParse("20:100:35").before(mustParse("20:100:55")));
        CHECK_FALSE(mustParse("20:100:55").before(mustParse("20:100:35")));
    }

    SECTION("identical → not before") {
        CHECK_FALSE(mustParse("20:100:35").before(mustParse("20:100:35")));
    }

    SECTION("LowSeq without triggeredBy vs LowSeq with triggeredBy, same lowSeq") {
        // "20::35" vs "20:100:55" → inner: simple(35) vs backfill(100:55)
        // simple.before(backfill): 35 < 100 → true
        CHECK(mustParse("20::35").before(mustParse("20:100:55")));
    }

    SECTION("LowSeq::Seq vs LowSeq::Seq, smaller lowSeq first") {
        CHECK(mustParse("20::35").before(mustParse("25::40")));
    }
}

//  before() — Cross-format comparisons (the new enhancement)

TEST_CASE("ParsedSequenceID before - Simple vs Backfill", "[ParsedSequenceID]") {
    // Rule: simple.seq < backfill.triggeredBy  (strict <)
    // A plain seq N sorts BEFORE a backfill triggered at N,
    // meaning "N" < "N:M" for any M.

    SECTION("simple seq < triggeredBy → simple is before") {
        // "42" before "100:35": 42 < 100 → true
        CHECK(mustParse("42").before(mustParse("100:35")));
    }

    SECTION("simple seq == triggeredBy → simple is NOT before (equal tier boundary)") {
        // "100" vs "100:35": 100 < 100 → false
        CHECK_FALSE(mustParse("100").before(mustParse("100:35")));
    }

    SECTION("simple seq > triggeredBy → simple is not before") {
        // "150" vs "100:35": 150 < 100 → false
        CHECK_FALSE(mustParse("150").before(mustParse("100:35")));
    }
}

TEST_CASE("ParsedSequenceID before - Backfill vs Simple", "[ParsedSequenceID]") {
    // Rule: backfill.triggeredBy <= simple.seq  (<=)
    // A backfill triggered at N comes at-or-before simple seq N.

    SECTION("triggeredBy < simple seq → backfill is before") {
        // "100:35" before "150": 100 <= 150 → true
        CHECK(mustParse("100:35").before(mustParse("150")));
    }

    SECTION("triggeredBy == simple seq → backfill IS before (<=)") {
        // "100:35" before "100": 100 <= 100 → true
        CHECK(mustParse("100:35").before(mustParse("100")));
    }

    SECTION("triggeredBy > simple seq → backfill is not before") {
        // "100:35" vs "42": 100 <= 42 → false
        CHECK_FALSE(mustParse("100:35").before(mustParse("42")));
    }
}

TEST_CASE("ParsedSequenceID before - Simple vs LowSeq", "[ParsedSequenceID]") {
    // Rule: simple.seq <= lowseq.lowSeq  (<=)

    SECTION("simple seq < lowSeq → simple is before") {
        // "15" before "20:100:35": 15 <= 20 → true
        CHECK(mustParse("15").before(mustParse("20:100:35")));
    }

    SECTION("simple seq == lowSeq → simple IS before (<=)") {
        // "20" before "20:100:35": 20 <= 20 → true
        CHECK(mustParse("20").before(mustParse("20:100:35")));
    }

    SECTION("simple seq > lowSeq → simple is not before") {
        // "25" vs "20:100:35": 25 <= 20 → false
        CHECK_FALSE(mustParse("25").before(mustParse("20:100:35")));
    }

    SECTION("simple vs LowSeq::Seq") {
        CHECK(mustParse("10").before(mustParse("20::35")));
        CHECK_FALSE(mustParse("30").before(mustParse("20::35")));
    }
}

TEST_CASE("ParsedSequenceID before - LowSeq vs Simple", "[ParsedSequenceID]") {
    // Rule: lowseq.lowSeq < simple.seq  (strict <)

    SECTION("lowSeq < simple seq → lowseq is before") {
        // "20:100:35" before "25": 20 < 25 → true
        CHECK(mustParse("20:100:35").before(mustParse("25")));
    }

    SECTION("lowSeq == simple seq → lowseq is NOT before") {
        // "20:100:35" vs "20": 20 < 20 → false
        CHECK_FALSE(mustParse("20:100:35").before(mustParse("20")));
    }

    SECTION("lowSeq > simple seq → lowseq is not before") {
        CHECK_FALSE(mustParse("20:100:35").before(mustParse("10")));
    }
}

TEST_CASE("ParsedSequenceID before - Backfill vs LowSeq", "[ParsedSequenceID]") {
    // Rule: backfill.triggeredBy <= lowseq.lowSeq  (<=)

    SECTION("triggeredBy < lowSeq → backfill is before") {
        // "100:35" before "20:200:55"? 100 <= 20 → false. Use "10:35" before "20:200:55"
        CHECK(mustParse("10:35").before(mustParse("20:200:55")));
    }

    SECTION("triggeredBy == lowSeq → backfill IS before (<=)") {
        // "20:35" before "20:100:55": 20 <= 20 → true
        CHECK(mustParse("20:35").before(mustParse("20:100:55")));
    }

    SECTION("triggeredBy > lowSeq → backfill is not before") {
        // "100:35" before "20:200:55": 100 <= 20 → false
        CHECK_FALSE(mustParse("100:35").before(mustParse("20:200:55")));
    }
}

TEST_CASE("ParsedSequenceID before - LowSeq vs Backfill", "[ParsedSequenceID]") {
    // Rule: lowseq.lowSeq < backfill.triggeredBy  (strict <)

    SECTION("lowSeq < triggeredBy → lowseq is before") {
        // "20:100:35" before "100:35"? 20 < 100 → true
        CHECK(mustParse("20:100:35").before(mustParse("100:35")));
    }

    SECTION("lowSeq == triggeredBy → lowseq is NOT before") {
        // "20:100:35" vs "20:35": 20 < 20 → false
        CHECK_FALSE(mustParse("20:100:35").before(mustParse("20:35")));
    }

    SECTION("lowSeq > triggeredBy → lowseq is not before") {
        CHECK_FALSE(mustParse("50:100:35").before(mustParse("20:35")));
    }
}

TEST_CASE("ParsedSequenceID checkpoint resolution scenarios", "[ParsedSequenceID]") {

    SECTION("Scenario 1 - both int, local older → keep local") {
        // local=42, remote=100 → remote.before(local) = 100<42 = false
        CHECK_FALSE(mustParse("100").before(mustParse("42")));
    }

    SECTION("Scenario 2 - both int, remote older → roll back") {
        // local=150, remote=42 → remote.before(local) = 42<150 = true
        CHECK(mustParse("42").before(mustParse("150")));
    }

    SECTION("Scenario 3 - local int, remote backfill, local older → keep local") {
        // local=42, remote=100:35 → remote.before(local): triggeredBy(100) <= seq(42) = false
        CHECK_FALSE(mustParse("100:35").before(mustParse("42")));
    }

    SECTION("Scenario 4 - local int, remote backfill, remote older → roll back") {
        // local=150, remote=100:35 → remote.before(local): 100 <= 150 = true
        CHECK(mustParse("100:35").before(mustParse("150")));
    }

    SECTION("Scenario 5 - same triggeredBy, remote has lower seq → roll back") {
        // local=100:55, remote=100:35 → remote.before(local): TB equal, 35<55 = true
        CHECK(mustParse("100:35").before(mustParse("100:55")));
    }

    SECTION("Scenario 6 - same triggeredBy, remote has higher seq → keep local") {
        // local=100:35, remote=100:55 → remote.before(local): TB equal, 55<35 = false
        CHECK_FALSE(mustParse("100:55").before(mustParse("100:35")));
    }

    SECTION("Scenario 7 - three-part: different lowSeq, remote lower → roll back") {
        // local=25:100:40, remote=20:100:35 → remote.before(local): 20<25 = true
        CHECK(mustParse("20:100:35").before(mustParse("25:100:40")));
    }

    SECTION("Scenario 8 - three-part: same lowSeq tie-break, remote lower seq → roll back") {
        // local=20:100:55, remote=20:100:35 → tie on lowSeq, 35<55 = true → roll back
        CHECK(mustParse("20:100:35").before(mustParse("20:100:55")));
    }

    SECTION("Scenario 9 - LowSeq::Seq format: remote lower → roll back") {
        // local=25::40, remote=20::35 → 20<25 = true → roll back
        CHECK(mustParse("20::35").before(mustParse("25::40")));
    }

    SECTION("Scenario 10 - exact match → not before (fast-path, no rollback)") {
        CHECK_FALSE(mustParse("100:35").before(mustParse("100:35")));
    }
}

TEST_CASE("ParsedSequenceID before - asymmetry at tier boundary", "[ParsedSequenceID]") {
    // A backfill triggered at N is considered at-or-after simple sequence N.
    // Verify the < vs <= asymmetry is correct at the exact boundary.

    SECTION("simple(N) is NOT before backfill(N:M) — equal boundary") {
        // "100" vs "100:35": simple 100 < triggeredBy 100 → false (strict <)
        CHECK_FALSE(mustParse("100").before(mustParse("100:35")));
    }

    SECTION("backfill(N:M) IS before simple(N) — equal boundary") {
        // "100:35" vs "100": triggeredBy 100 <= seq 100 → true (<=)
        CHECK(mustParse("100:35").before(mustParse("100")));
    }

    SECTION("simple(N) IS before backfill(N+1:M)") {
        // "100" before "101:35": 100 < 101 → true
        CHECK(mustParse("100").before(mustParse("101:35")));
    }
}

