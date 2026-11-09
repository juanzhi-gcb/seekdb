# Local development SQL protocol

This adapter is explicitly selected with --protocol dev-tsv. It is **not** a claimed competition protocol: the official driver and framing have not been provided.

## Launch

minivectordb --protocol dev-tsv --data-dir /data --sort-memory 8388608

The database is /data/database.mvdb and sort runs are under /data/tmp. The sort option is a byte budget for sort state, not a total process-memory limit. Minimum CLI value: 4096 bytes. The current parser materializes one SQL statement, with a CLI limit of 16 MiB.

Input is UTF-8 SQL on stdin. Semicolons separate statements outside single-quoted strings, backtick identifiers, -- line comments and /* block comments */. Doubled quote/backtick characters escape themselves. A complete last statement may end at EOF without a semicolon. An unterminated quote/comment produces an error and is not executed. No backslash SQL string-escape mode is implied.

The engine is single-threaded and single-process. The data file is locked against a second writer. EOF/destruction rolls back any active transaction. A successful commit response is emitted only after durable commit returns.

## Output on stdout

- Non-query success: OK<TAB>affected_rows
- Query header: RESULT<TAB>column_name...
- Each query row: ROW<TAB>typed_value...
- Successful query completion: END<TAB>row_count
- Statement failure: ERROR<TAB>category<TAB>message

Categories are internal diagnostics, not MySQL error numbers. Output flushes after every completed statement/error. A result is valid only after END; if a streaming query fails, ERROR invalidates any rows already emitted for that result. Consumers must drain row output while queries are running.

Cells use i: for signed integers, s: for text, v:[...] for float32 vectors, and d: for double-precision distances. Text, names and messages escape backslash, tab, LF, CR and NUL as \\, \t, \n, \r and \0. Other UTF-8 bytes are preserved. Floats use enough decimal digits to round trip.

Exit status: 0 for an error-free session, 1 if any SQL/framing error occurred, 2 for command-line/startup/fatal transport errors. SQL errors do not end the input session. Diagnostics for fatal errors go to stderr.

## Example

Input:

    CREATE TABLE items (id BIGINT PRIMARY KEY, label VARCHAR(30), v VECTOR(2));
    INSERT INTO items VALUES (1,'first',[0,0]),(2,'second',[3,4]);
    SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM items ORDER BY distance LIMIT 1;

Expected output (tabs shown as spaces):

    OK  0
    OK  2
    RESULT  id  distance
    ROW  i:1  d:0
    END  1

Do not use this local contract to claim official compatibility. The adapter remains replaceable while catalog, execution and persistence stay behind the streaming Database API.