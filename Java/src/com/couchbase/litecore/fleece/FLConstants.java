package com.couchbase.litecore.fleece;

public class FLConstants {
    // Types of Fleece values. Basically JSON, with the addition of Data (raw blob).
    public interface FLValueType {
        int kFLUndefined = -1; // Type of a nullptr FLValue (i.e. no such value)
        int kFLNull = 0;
        int kFLBoolean = 1;
        int kFLNumber = 2;
        int kFLString = 3;
        int kFLData = 4;
        int kFLArray = 5;
        int kFLDict = 6;
    }

    public interface FLError {
        int NoError = 0;
        int MemoryError = 1;        // Out of memory, or allocation failed
        int OutOfRange = 2;        // Array index or iterator out of range
        int InvalidData = 3;        // Bad input data (NaN, non-string key, etc.)
        int EncodeError = 4;        // Structural error encoding (missing value, too many ends, etc.)
        int JSONError = 5;        // Error parsing JSON
        int UnknownValue = 6;       // Unparseable data in a Value (corrupt? Or from some distant future?)
        int InternalError = 7;      // Something that shouldn't happen
        int NotFound = 8;
    }
}
