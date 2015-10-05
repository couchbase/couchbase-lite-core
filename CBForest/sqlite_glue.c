//
//  sqlite_glue.c
//  CBForest
//
//  Created by Jens Alfke on 10/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include <sqlite3.h>
#include <stdlib.h>

// Implements the couple of sqlite functions called by the tokenizer (sqlite3-unicodesn)

void* sqlite3_malloc(int size)              {return malloc(size);}
void* sqlite3_realloc(void *ptr, int size)  {return realloc(ptr, size);}
void sqlite3_free(void *ptr)                {free(ptr);}
