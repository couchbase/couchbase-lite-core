# Couchbase Lite (-Core) Utility Tools

These are command-line tools intended to help developers working with Couchbase Lite databases. They are self-contained and can be run from anywhere.

Each tool has a small amount of online help that can be accessed by running it either with no arguments, or with just a `--help` flag.

**Compatibility:**

* Each tool is self-contained, with its own embedded LiteCore library. This means it's important to upgrade the tools when you upgrade Couchbase Lite, since an older tool may not be able to read a newer database.
* The tools can operate on databases created on any platform, as long as they're accessible through the filesystem. So for instance, you can copy a database from an Android app to an SD card, mount it on a Mac, and run the tool there.
* The tools are not compatible with Couchbase Lite 1.x databases.

## cblite

A multipurpose tool for examining and querying a database. It has a number of subcommands, of which the most generally useful are:

* `ls`: Lists the document IDs
* `cat`: Displays documents as pretty-printed JSON
* `query`: Runs a query, expressed in low-level [JSON format](https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema) (sorry, no N1QL parser!)

It can be run in an interactive mode by giving a database path but no subcommand. This lets you run multiple commands without having to re-enter the database name every time.

## litecp

A document-copying tool that can:

* Create a database from one or more JSON files
* Import JSON file(s) into an existing database as documents
* Export a database's documents into a JSON file or files
* Push-replicate one local database file to another one
* Push or pull from a remote database

## litecorelog

Reads a binary log file produced by `c4log_writeToBinaryFile`, and outputs it as plain text.
