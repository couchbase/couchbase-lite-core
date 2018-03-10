# The `cblite` Tool

`cblite` is a command-line tool for inspecting and querying LiteCore and Couchbase Lite databases. It has the following sub-commands:

| Command | Purpose |
|---------|---------|
| `cblite cat` | Display the body of one or more documents |
| `cblite cp` | Replicate, import or export a database |
| `cblite file` | Display information about the database |
| `cblite help` | Display help text |
| `cblite logcat` | Display binary log files in readable form |
| `cblite ls` | List the documents in the database |
| `cblite query` | Run queries, using the [[JSON Query Schema]] |
| `cblite revs` | List the revisions of a document |
| `cblite serve` | Starts a (rudimentary) REST API listener |

It has an interactive mode that you start by running `cblite /path/to/database`, i.e. with no subcommand. It will then prompt you for a command, which is a command line without the initial `cblite` or the database-path parameter. Enter `quit` or press Ctrl-D to exit.

## Example

```
$  cblite file travel-sample.cblite2
Database:   travel-sample.cblite2/
Total size: 34MB
Documents:  31591, last sequence 31591

$  cblite ls -l --limit 10 travel-sample.cblite2
Document ID     Rev ID     Flags   Seq     Size
airline_10      1-d70614ae ---       1     0.1K
airline_10123   1-091f80f6 ---       2     0.1K
airline_10226   1-928c43f4 ---       3     0.1K
airline_10642   1-5cb6252c ---       4     0.1K
airline_10748   1-630b0443 ---       5     0.1K
airline_10765   1-e7999661 ---       6     0.1K
airline_109     1-bd546abb ---       7     0.1K
airline_112     1-ca955c69 ---       8     0.1K
airline_1191    1-28dbba6e ---       9     0.1K
airline_1203    1-045b6947 ---      10     0.1K
(Stopping after 10 docs)

$  cblite travel-sample.cblite2
(cblite) query --limit 10 '["=", [".type"], "airline"]'
["_id": "airline_10"]
["_id": "airline_10123"]
["_id": "airline_10226"]
["_id": "airline_10642"]
["_id": "airline_10748"]
["_id": "airline_10765"]
["_id": "airline_109"]
["_id": "airline_112"]
["_id": "airline_1191"]
["_id": "airline_1203"]
(Limit was 10 rows)
(cblite) query --limit 10 '{WHAT: [[".name"]], WHERE:  ["=", [".type"], "airline"], ORDER_BY: [[".name"]]}'
["40-Mile Air"]
["AD Aviation"]
["ATA Airlines"]
["Access Air"]
["Aigle Azur"]
["Air Austral"]
["Air Caledonie International"]
["Air CaraÃ¯bes"]
["Air Cargo Carriers"]
["Air Cudlua"]
(Limit was 10 rows)
(cblite) ^D
$
```

## Parameters

(You can run `cblite --help` to get a quick summary.)

### cat

`cblite cat` _[flags]_ _databasepath_ _DOCID_ [_DOCID_ ...]

| Flag    | Effect  |
|---------|---------|
| `--key KEY` | Display only a single key/value (may be used multiple times) |
| `--rev` | Show the revision ID(s) |
| `--raw` | Raw JSON (not pretty-printed) |
| `--json5` | JSON5 syntax (no quotes around dict keys) |

(DOCID may contain shell-style wildcards `*`, `?`)

### cp

`cblite cp` _[flags]_ _source_ _destination_

| Flag    | Effect  |
|---------|---------|
| `--continuous` | Continuous replication (never stops!) |
| `--bidi` | Bidirectional (push+pull) replication |
| `--existing` or `-x` | Fail if _destination_ doesn't already exist.|
| `--jsonid` _property_ | JSON property to use for document ID* |
| `--limit` _n_ | Stop after _n_ documents. (Replicator ignores this) |
| `--careful` | Abort on any error. |
| `--verbose` or `-v` | Log progress information. Repeat flag for more verbosity. |

_source_ and _destination_ can be database paths, replication URLs, or JSON file paths. One of them must be a database path ending in `*.cblite2`. The other can be any of the following:

* `*.cblite2` ⟶  Local replication
* `blip://*`  ⟶  Networked replication
* `*.json`    ⟶  Imports/exports JSON file (one document per line)
* `*/`        ⟶  Imports/exports directory of JSON files (one per doc)

\* `--jsonid` works as follows: When _source_ is JSON, this is a property name/path whose value will be used as the document ID. (If omitted, documents are given UUIDs.) When _destination_ is JSON, this is a property name that will be added to the JSON, whose value is the document's ID. (If this flag is omitted, the value defaults to `_id`.)

In interactive mode, the database path is already known, so it's used as the source and `cp` takes only a destination argument. You can optionally call the command `push` or `export`. Or if you use the synonyms `pull` or `import` in interactive mode, the parameter you give is treated as the _source_, while the current database is the _destination_.

### file

`cblite file` _databasepath_

### logcat

`cblite logcat` _logfilepath_

### ls

`cblite ls` _[flags]_ _databasepath_ _[PATTERN]_

| Flag    | Effect  |
|---------|---------|
| `-l` | Long format (one doc per line, with metadata) |
| `--offset` _n_ | Skip first _n_ docs |
| `--limit` _n_ | Stop after _n_ docs |
| `--desc` | Descending order |
| `--seq` | Order by sequence, not docID |
| `--del` | Include deleted documents |
| `--conf` | Include _only_ conflicted documents |
| `--body` | Display document bodies |
| `--pretty` | Pretty-print document bodies (implies `--body`) |
| `--json5` | JSON5 syntax, i.e. unquoted dict keys (implies `--body`)|

(PATTERN is an optional pattern for matching docIDs, with shell-style wildcards `*`, `?`)

### revs

`cblite revs` _databasepath_ _DOCID_

### query

`cblite query` _[flags]_ _databasepath_ _query_

| Flag    | Effect  |
|---------|---------|
| `--offset` _n_ | Skip first _n_ rows |
| `--limit` _n_ | Stop after _n_ rows |

The _query_ must follow the [[JSON query schema|JSON Query Schema]]. It can be a dictionary {`{ ... }`) containing an entire query specification, or an array (`[ ... ]`) with just a `WHERE` clause. There are examples of each up above.

The query must be a single argument; put quotes around it to ensure that and to avoid misinterpretation of special characters. [JSON5](http://json5.org) syntax is allowed. 

### serve

`cblite serve` _[flags]_ _databasepath_

| Flag    | Effect  |
|---------|---------|
| `--port` _n_ | Set TCP port number (default is 59840) |
| `--readonly` | Prevent REST calls from altering the database |
| `--verbose` or `-v` | Log requests. Repeat flag for more verbosity. |

**Note:** Only a subset of the Couchbase Lite REST API is implemented so far! (See [[REST-API]])
