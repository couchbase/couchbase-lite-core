# How Queries Are Run

After [translating](QueryTranslator.md) a N1QL (or JSON) query to SQL, there are still several differences between the query languages that have to be handled at runtime:

- Extracting property values from the document body
- Representing runtime values of N1QL/Fleece data types not known to SQL: `null` (not the same as SQL `NULL`!), booleans, arrays and dicts/objects
- Skipping deleted documents that are still hanging around in the default collection (for legacy/performance reasons.)
- `ANY`, `EVERY` and `UNNEST` operations that recurse into array items

Handling these affects the translation of the query, but rather than adding to the QueryTranslator documentation, let's discuss them in a separate document.

## Refresher: SQLite Table Schema

A collection is a SQLite table whose name begins with `kv_`. Its schema is:

    key          TEXT PRIMARY KEY,  -- the docID
    sequence     INTEGER,           -- sequence number
    flags        INTEGER DEFAULT 0, -- various flags including "deleted" (0x01)
    version      BLOB,              -- binary revid or version-vector
    body         BLOB,              -- Fleece body (within a serialized RevTree in older dbs)
    extra        BLOB,              -- Doc metadata like rev tree and conflicting revs
    expiration   INTEGER            -- Expiration time (seconds since epoch) or NULL


## Example

Here's a simple query: `SELECT first FROM _ WHERE last = 'Smith'`. First the N1QL parser converts this to JSON:

```json
["SELECT", {"WHAT": [[".first"]], "WHERE": ["=", [".", "last"], "Smith"]}]
```

This translates to:

```sql
SELECT fl_result(fl_value(_doc.body, 'first')) FROM kv_default AS _doc 
 WHERE fl_value(_doc.body, 'last') = 'Smith' AND (_doc.flags & 1 = 0)
```

Let's go through this bit by bit.

- `fl_value(_doc.body, 'first')` — This gets the property `first` of the document body as a SQLite value.
- `fl_result(...)` — This converts a runtime value to a form that can be returned from the query, recognized by SQLiteQueryRunner, and turned into the corresponding Fleece value.
- `AND (_doc.flags & 1 = 0)` — This skips deleted documents. It only appears when querying the default collection `kv_default`.


## N1QL vs SQLite Data Types

SQLite only has a few data types: NULL, integer, number, text, blob. That's less than N1QL, so we have to be creative and wrap some N1QL values to avoid losing information.

Fortunately SQLite has a thing called "subtypes". During evaluation of a query, any value (`sqlite3_value*`) can be tagged with a small integer called a subtype. Extension functions can get and set these subtypes.

SQLite also allows native pointers to be wrapped in values. Such a pointer appears as a SQLite `NULL` value, but the pointer can be unwrapped by an extension function.

| N1QL Type     | Runtime SQLite value |
|---------------|--------------|
| `MISSING`     | `NULL` |
| `null`        | Zero-length blob with subtype 0x67 |
| boolean       | Integer 0 or 1 with subtype 0x68 |
| number        | Unchanged |
| string        | Unchanged |
| array, object | Either a Fleece-encoded blob with no subtype tag, or a wrapped `FLValue` pointer |
| blob          | Blob tagged with subtype 0x66 |

 Unfortunately, subtypes and wrapped pointers are lost when SQLite's `sqlite3_column_xxx()` functions return column values to the caller — the query just returns the value without that metadata. This means we have to do some postprocessing on such values to turn them into a _different_ form that doesn't lose information.
 
 The `fl_result()` function does this. It's used in every result (projection) column, unless the column's type is known to be a type that doesn't need translation. It converts values of types null, boolean, array, object and blob into Fleece-encoded blobs. (Yes, we re-encode N1QL blob values. Otherwise they'd be indistinguishable from Fleece-wrapped values.) The corresponding code in `SQLiteQueryRunner::encodeColumn()` detects a blob value and unwraps the Fleece value inside.

The whole process of getting a value from a query looks like:

1. At rest in `collection.body`: Fleece data, maybe wrapped in serialized RevTree
2. While SQLite evaluates a row: `sqlite3_value*`, maybe tagged 
3. When SQLite returns a column value: a normal value or a Fleece-encoded blob
4. Returned from the LiteCore query API: A Fleece value


## N1QL/Fleece Support Functions

These are the core extension functions for dealing with document bodies and Fleece values. They're implemented in `SQLiteFleeceFunctions.cc`.

| Name                      | Description |
|---------------------------|-------------|
| `fl_root(body)`           | Gets the entire document body. |
| `fl_value(body, path)`    | Gets a property of the document given its path. |
| `fl_version(doc.version)` | Converts binary revID/version-vector to ASCII. |
| `fl_nested_value(value, path)`    | Gets a nested property from a Fleece dict or array.  |
| `fl_unnested_value(body, [path])` | The equivalent of `fl_root` and `fl_value` for array (`UNNEST`) indexes, i.e. tables containing unnested properties.  |
| `fl_exists(body, path)`   | Returns true if the given property path exists in the document.  |
| `fl_blob(body, path)`     | Gets a blob/attachment metadata dict from the doc, and returns the blob contents.  |
| `encode_vector(value)`    | Converts a vector value to a blob of float32s. Unlike `fl_vector_to_index` (q.v.) this does return an error on invalid input. |
| `fl_result(value)`        | Converts value to a form that can be returned from the query.  |
| `fl_boolean_result(value)`| Similar to `fl_result` but coerces integer values to boolean. |

### N1QL Functions

| Name                      | Description |
|---------------------------|-------------|
| `fl_null()`               | Simply returns a N1QL/Fleece `null` value. Used where the literal `null` appears in a query. |
| `fl_bool(num)`            | Converts a number to a N1QL/Fleece boolean. Anything that coerces to a non-zero int is `true`. |
| `array_of(value...)`      | The N1QL `ARRAY()` function. Constructs an array of its arguments. |
| `dict_of(key, value, ...)`| The N1QL `OBJECT()` function. Constructs a dict from key/value pairs. |
| `fl_count(body, path)`    | Applies the N1QL `ARRAY_COUNT` function to a property of a document.  |
| `fl_contains(body, path, value)` | Gets a doc property and returns whether it's an array or dict that contains the given value. Used as an optimization of N1QL `ANY` when the predicate simply tests for a single value. |

There are a _lot_ more N1QL functions. They're described in the [JSON Query Schema][SCHEMA] documentation, and to some degree in comments in `SQLiteN1QLFunctions.cc`.

### Indexes

| Name                                  | Description |
|---------------------------------------|-------------|
| `fl_fts_value(body, path)`            | Converts a document property to a string, for indexing by FTS. |
| `fl_vector_to_index(body, path, dim)` | Gets a vector value from a doc and returns it as a blob of float32s. This is used when indexing docs for a vector index, so invalid vector data will produce a SQLite NULL result instead of an error. |
| `fl_vector_to_index(value, NULL, dim)` | Same as above but operates on a direct vector value. |
| `rank(matchinfo)` | Computes the relevancy of an FTS match; arg is the result of SQLite's FTS `matchinfo` function. |


## ANY, EVERY, UNNEST

All of these let a query iterate over the contents of an array or object. They're implemented using the `fl_each` extension function, which is a lot more complicated than the others because it implements a "table-valued function", which is a special type of SQLite virtual table. A table-valued function can be used in the `FROM` clause of a query as though it were a table, and the function gets to determine what the virtual rows are.

| Name                      | Description |
|---------------------------|-------------|
| `fl_each(body, path)`     | Looks up an array/object property by its path and iterates its items as rows |
| `fl_each(value)`          | Iterates an array/object's items as rows |

The virtual table implemented by `fl_each` has columns

    key,    -- the current key if iterating an object, else NULL
    type,   -- The current value's type, as per the `FLValueType` enum
    value,  -- The current value as an SQLite value
    data,   -- The current value as encoded Fleece data
    body    -- The current value as an `FLValue` pointer

The multiple representations of the value are optimizations for different usages that need the value in different forms.

### Example: ANY

The N1QL expression `ANY x IN names SATISFIES x.last = "Smith"` translates to JSON:

```json
["ANY", "x", [".", "names"], ["=", ["?", "x", "last"], "Smith"]]
```

which translates to SQL:

```sql
EXISTS (SELECT 1 FROM fl_each(_doc.body, 'names') AS _x 
                WHERE fl_nested_value(_x.body, 'last') = 'Smith')
```

`ANY` and `EVERY` use nested `SELECT` expressions to call `fl_each`, and test the number of rows.

### Example: UNNEST

The N1QL query `SELECT * FROM _ as book UNNEST book.notes AS notes WHERE notes = "torn"` translates to JSON:

```json
["SELECT", {
   "WHAT": [ [".book.ISBN"] ],
   "FROM": [{"AS": "book"}, {"AS": "notes", "UNNEST": [".book.notes"]}],
  "WHERE": ["=", [".notes"], "torn"] } ]
```

which translates to SQL:

```sql
SELECT fl_result(fl_value(book, "isbn"))
  FROM kv_default AS book 
  JOIN fl_each(book.body, 'notes') AS notes 
 WHERE notes.value = 'torn' AND (book.flags & 1 = 0)
```

As you can see, `UNNEST` uses `fl_each` as the source of a `JOIN`.

### Indexed UNNEST

An UNNEST expression can be optimized if there exists an array index on the matching document property. In this case the source of the JOIN is the array index's table instead of an `fl_each` expression. If there is also a value index on the appropriate property of the array-index table, SQLite will be able to search that index very efficiently.

[SCHEMA]: https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
