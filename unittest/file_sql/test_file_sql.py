"""End-to-end native file SQL tests; run against an isolated local seekdb."""
import base64
import csv
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid


class FileSQL(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.client = os.environ.get("SEEKDB_TEST_CLIENT", str(
            cls.root.parent / "seekdb-competition/deps/3rd/u01/obclient/bin/obclient"))
        cls.port = os.environ.get("SEEKDB_TEST_PORT", "10102")
        cls.temp = tempfile.TemporaryDirectory(prefix="seekdb_file_sql_")
        cls.data = Path(cls.temp.name).resolve()
        fixture = cls.root / "tools/deploy/mysql_test/test_suite/file_sql/data"
        for name in ("basic.csv", "basic.jsonl", "invalid.txt"):
            shutil.copyfile(fixture / name, cls.data / name)
        (cls.data / "simple.parquet").write_bytes(base64.b64decode(
            (fixture / "simple.parquet.b64").read_bytes()))
        cls.table = "file_sql_" + uuid.uuid4().hex[:12]

    @classmethod
    def tearDownClass(cls):
        cls.sql(f"DROP TABLE IF EXISTS test.{cls.table}")
        cls.temp.cleanup()

    @classmethod
    def sql(cls, query, enabled=True, error=None):
        query = ("SET SESSION enable_file_sql=ON;" if enabled else "") + query
        connection = ["-h127.0.0.1", "-P" + cls.port]
        if 'SET GLOBAL secure_file_priv' in query:
            connection = ["--socket=" + os.environ.get("SEEKDB_TEST_SOCKET",
                str(cls.root.parent / "seekdb-codex-runtime/run/sql.sock"))]
        result = subprocess.run([cls.client, *connection,
            "-uroot", "-N", "-s", "--default-character-set=utf8mb4", "-e", query],
            text=True, capture_output=True, timeout=60)
        if error is not None:
            if result.returncode == 0 or f"ERROR {error} " not in result.stderr:
                raise AssertionError((result.returncode, result.stdout, result.stderr))
            return result.stderr
        if result.returncode != 0:
            raise AssertionError((query, result.stdout, result.stderr))
        return result.stdout.rstrip("\n").splitlines()

    @classmethod
    def source(cls, name, function="read_csv", options=""):
        return f"{function}('{cls.data / name}'{options})"

    def test_default_off_and_options(self):
        self.assertEqual(self.sql("SELECT @@session.enable_file_sql", enabled=False), ["0"])
        self.sql(f"SELECT COUNT(*) FROM {self.source('basic.csv')}", enabled=False, error=1235)
        self.sql(f"SELECT * FROM {self.source('basic.csv', options=", 'xml'")}", error=1064)
        self.sql(f"SELECT * FROM {self.source('basic.csv', options=', sample_size=10')}", error=1235)

    def test_csv_types_quotes_duplicates_and_aggregate(self):
        source = self.source("basic.csv")
        self.assertEqual(self.sql(f"SELECT id,name,score,active,day,created,"
            f"REPLACE(note,CHAR(10),'<NL>'),id__2 FROM {source} ORDER BY id"), [
            '1\tAlice\t98.5\t1\t2026-08-01\t2026-08-01 10:11:12\thello,<NL>world\t101',
            '2\tBob\t87\t0\tNULL\tNULL\tplain\t102',
            '3\tChen\t90.25\t1\t2026-08-03\t2026-08-03 12:13:14\tsay "hi"\t103'])
        self.assertEqual(self.sql(f"SELECT COUNT(*), SUM(id), ROUND(AVG(score),2), SUM(active) "
            f"FROM {self.source('basic.csv', options=', header=true, sample_size=-1')}"), ['3\t6\t91.92\t2'])

    def test_json_union_schema(self):
        source = self.source("basic.jsonl", "read_json", ", format='newline_delimited'")
        self.assertEqual(self.sql(f"SELECT id,name,score,active,day,note FROM {source} ORDER BY id"), [
            '1\tAlice\t98.5\t1\tNULL\tNULL', '2\tBob\tNULL\t0\t2026-08-02\tNULL',
            '3\tChen\t90\t1\tNULL\tok'])

    def test_describe_schema(self):
        self.assertEqual(self.sql(f"DESCRIBE SELECT * FROM {self.source('basic.csv')}"), [
            'id\tBIGINT\tNO\tNULL\tNULL\tNULL', 'name\tVARCHAR\tNO\tNULL\tNULL\tNULL',
            'score\tDOUBLE\tNO\tNULL\tNULL\tNULL', 'active\tBOOLEAN\tNO\tNULL\tNULL\tNULL',
            'day\tDATE\tYES\tNULL\tNULL\tNULL', 'created\tDATETIME\tYES\tNULL\tNULL\tNULL',
            'note\tVARCHAR\tNO\tNULL\tNULL\tNULL', 'id__2\tBIGINT\tNO\tNULL\tNULL\tNULL'])

    def test_parquet_native_scan(self):
        self.assertEqual(self.sql(f"SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) "
            f"FROM {self.source('simple.parquet', 'read_parquet')}"), ['7\t0\t6\t21'])

    def test_where_group_join_rescan_and_limit(self):
        source = self.source("basic.csv")
        self.sql(f"CREATE TABLE test.{self.table}(id BIGINT PRIMARY KEY, category VARCHAR(10));"
                 f"INSERT INTO test.{self.table} VALUES(1,'a'),(2,'b'),(3,'a')")
        try:
            self.assertEqual(self.sql(f"SELECT d.category,COUNT(*),ROUND(SUM(f.score),2) "
                f"FROM {source} f JOIN test.{self.table} d ON f.id=d.id "
                f"WHERE f.active=1 GROUP BY d.category"), ['a\t2\t188.75'])
            self.assertEqual(self.sql(f"SELECT a.id,b.id FROM {source} a CROSS JOIN {source} b "
                f"WHERE a.id<3 AND b.id=3 ORDER BY a.id"), ['1\t3','2\t3'])
            self.assertEqual(self.sql(f"SELECT name FROM {source} ORDER BY score DESC LIMIT 1"), ['Alice'])
        finally:
            self.sql(f"DROP TABLE test.{self.table}")

    def test_copy_round_trip_and_no_overwrite(self):
        output = self.data / ('export_' + uuid.uuid4().hex + '.csv')
        self.sql(f"COPY (SELECT id,name,score FROM {self.source('basic.csv')} ORDER BY id) "
                 f"TO '{output}' (FORMAT csv, HEADER, NULLSTR 'NULL')")
        self.assertEqual(list(csv.reader(output.open())), [
            ['id','name','score'],['1','Alice','98.5'],['2','Bob','87'],['3','Chen','90.25']])
        self.assertEqual(self.sql(f"SELECT * FROM read_csv('{output}') ORDER BY id"), [
            '1\tAlice\t98.5','2\tBob\t87','3\tChen\t90.25'])
        before = output.read_bytes()
        with self.assertRaises(AssertionError):
            self.sql(f"COPY (SELECT 9) TO '{output}' (FORMAT csv)")
        self.assertEqual(output.read_bytes(), before)

    def test_copy_null_empty_quotes_and_empty_result(self):
        output = self.data / ('escape_' + uuid.uuid4().hex + '.csv')
        self.sql(f"COPY (SELECT NULL AS n,'' AS e,'NULL' AS s,CONCAT('a',CHAR(34),',',CHAR(10),'b') AS q) "
            f"TO '{output}' (FORMAT csv, HEADER, NULLSTR 'NULL')")
        self.assertEqual(list(csv.reader(output.open())), [['n','e','s','q'],['NULL','','NULL','a",\nb']])
        self.assertEqual(self.sql(f"SELECT n IS NULL,e='',s='NULL',LENGTH(q) FROM read_csv('{output}')"), ['1\t1\t1\t5'])
        empty = self.data / ('empty_' + uuid.uuid4().hex + '.csv')
        self.sql(f"COPY (SELECT 1 AS id WHERE 0) TO '{empty}' (FORMAT csv, HEADER)")
        self.assertEqual(list(csv.reader(empty.open())), [['id']])

    def test_glob_and_malformed_records(self):
        self.assertEqual(self.sql(f"SELECT SUBSTRING_INDEX(file,'/',-1) FROM glob('{self.data}/basic.*') ORDER BY file"),
                         ['basic.csv','basic.jsonl'])
        for name, content in [('bad.csv','a,b\n1,"unterminated\n'), ('bad.jsonl','{"a":1}\n[2]\n')]:
            (self.data / name).write_text(content)
            with self.assertRaises(AssertionError):
                self.sql(f"SELECT * FROM {self.source(name, 'read_json' if name.endswith('jsonl') else 'read_csv')}")

    def test_repeated_query_rediscovers_changed_schema(self):
        path = self.data / 'changing.csv'
        path.write_text('a\n1\n')
        self.assertEqual(self.sql(f"SELECT * FROM read_csv('{path}')"), ['1'])
        path.write_text('a,b\n2,hello\n')
        self.assertEqual(self.sql(f"SELECT * FROM read_csv('{path}')"), ['2\thello'])

    def test_sample_boundary_and_streaming_aggregate(self):
        path = self.data / 'streaming.csv'
        with path.open('w') as out:
            out.write('id,value\n')
            for i in range(25000):
                out.write(f'{i},{"NULL" if i == 24000 else i}\n')
        self.assertEqual(self.sql(f"SELECT COUNT(*),SUM(id) FROM read_csv('{path}')"), ['25000\t312487500'])
        self.assertEqual(self.sql(f"SELECT id FROM read_csv('{path}') WHERE value IS NULL"), ['24000'])

    def test_secure_directory_boundary_and_symlinks(self):
        original = self.sql('SELECT @@global.secure_file_priv')[0]
        allowed = self.data / 'allowed'
        sibling = self.data / 'allowed_sibling'
        allowed.mkdir(exist_ok=True)
        sibling.mkdir(exist_ok=True)
        (allowed / 'good.csv').write_text('id\n1\n')
        (sibling / 'secret.csv').write_text('id\n2\n')
        (allowed / 'escape.csv').symlink_to(sibling / 'secret.csv')
        try:
            self.sql(f"SET GLOBAL secure_file_priv='{allowed}'")
            self.assertEqual(self.sql(f"SELECT * FROM read_csv('{allowed}/good.csv')"), ['1'])
            for path in (sibling / 'secret.csv', allowed / 'escape.csv', allowed / '../allowed_sibling/secret.csv'):
                with self.assertRaises(AssertionError):
                    self.sql(f"SELECT * FROM read_csv('{path}')")
            with self.assertRaises(AssertionError):
                self.sql(f"COPY (SELECT 1) TO '{sibling}/blocked.csv' (FORMAT csv)")
            self.assertFalse((sibling / 'blocked.csv').exists())
        finally:
            escaped = original.replace("'", "''")
            self.sql(f"SET GLOBAL secure_file_priv='{escaped}'")


if __name__ == '__main__':
    unittest.main()
