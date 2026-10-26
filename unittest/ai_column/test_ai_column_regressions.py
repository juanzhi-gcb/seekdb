"""AI column generation and strict-read regressions on an isolated seekdb."""
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class AiColumnRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = os.environ.get('SEEKDB_TEST_PORT', '10104')
        cls.client = str(Path(__file__).resolve().parents[3] /
                         'seekdb-competition/deps/3rd/u01/obclient/bin/obclient')
        cls.started = [threading.Event() for _ in range(3)]
        cls.release = [threading.Event() for _ in range(3)]
        cls.calls = 0
        cls.lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                with cls.lock:
                    index = cls.calls
                    cls.calls += 1
                if index < len(cls.started):
                    cls.started[index].set()
                    cls.release[index].wait(15)
                data = json.dumps({'choices': [{'message': {'content': 'generation-' + str(index)}}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.sql('DROP DATABASE IF EXISTS codex_ai_regression; CREATE DATABASE codex_ai_regression')
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('codex_generation_model',"
                "'{\"type\":\"completion\",\"model_name\":\"mock\"}')")
        endpoint = json.dumps({'ai_model_name': 'codex_generation_model',
                               'url': f'http://127.0.0.1:{cls.server.server_port}/v1/chat/completions',
                               'access_key': 'local-test', 'provider': 'openai'})
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('codex_generation_ep', '" + endpoint + "')")
        cls.sql("CREATE TABLE codex_ai_regression.docs(id INT PRIMARY KEY, content VARCHAR(30),"
                " result VARCHAR(100) AI COLUMN(AI_COMPLETE('codex_generation_model',content))"
                " WITH(sync_mode='async',max_retries=0))")

    @classmethod
    def tearDownClass(cls):
        for event in cls.release:
            event.set()
        cls.sql('DROP DATABASE IF EXISTS codex_ai_regression')
        cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('codex_generation_ep')")
        cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('codex_generation_model')")
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    @classmethod
    def sql(cls, query):
        result = subprocess.run([cls.client, '-h127.0.0.1', '-P' + cls.port,
                                 '-uroot', '-N', '-s', '-e', query],
                                text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError((query, result.stdout, result.stderr))
        return result.stdout.strip()

    def test_generation_aba_and_strict_transaction(self):
        self.sql("INSERT INTO codex_ai_regression.docs(id,content) VALUES(1,'A')")
        self.assertTrue(self.started[0].wait(10), 'initial request did not start')
        source = subprocess.Popen([self.client, '-h127.0.0.1', '-P' + self.port,
                                   '-uroot', '-N', '-s', '--unbuffered'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            source.stdin.write("START TRANSACTION;\n"
                               "UPDATE codex_ai_regression.docs SET content='B' WHERE id=1;\n"
                               "UPDATE codex_ai_regression.docs SET content='A' WHERE id=1;\n"
                               "SELECT 'lock-held';\n")
            source.stdin.flush()
            self.assertEqual(source.stdout.readline().strip(), 'lock-held')
            # Let the old response reach UPDATE while the replacement row is locked.
            self.release[0].set()
            time.sleep(.2)
            source.stdin.write('COMMIT;\n')
            source.stdin.close()
            source.wait(timeout=30)
            self.assertEqual(source.returncode, 0, source.stderr.read())
        finally:
            if source.poll() is None:
                source.kill()
                source.wait()
            source.stdout.close()
            source.stderr.close()
        self.assertTrue(self.started[1].wait(10), 'replacement request did not start')
        self.assertEqual(self.sql('SELECT result IS NULL FROM codex_ai_regression.docs'), '1',
                         'old A generation was published into the replacement A request')
        self.release[1].set()
        for _ in range(100):
            if self.sql('SELECT result FROM codex_ai_regression.docs') == 'generation-1':
                break
            time.sleep(.1)
        self.assertEqual(self.sql('SELECT result FROM codex_ai_regression.docs'), 'generation-1')
        # A cache hit must check current state, and the guard sees own pending writes.
        query = ('SET SESSION ai_column_strict_read=ON; '
                 'SELECT result FROM codex_ai_regression.docs; START TRANSACTION; '
                 "UPDATE codex_ai_regression.docs SET content='pending' WHERE id=1; "
                 'SELECT content FROM codex_ai_regression.docs; '
                 'SELECT result FROM codex_ai_regression.docs; ROLLBACK; '
                 'SELECT result FROM codex_ai_regression.docs')
        result = subprocess.run([self.client, '-h127.0.0.1', '-P' + self.port,
                                 '-uroot', '-N', '-s', '--force'],
                                input=query.replace(';', ';\n') + ';\n', text=True, capture_output=True, timeout=30)
        self.assertEqual(result.stderr.count('ERROR 11120 '), 1, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines(), ['generation-1', 'pending', 'generation-1'])
        self.assertEqual(self.sql('SELECT result FROM codex_ai_regression.docs'), 'generation-1')
        # RETRY shares the transaction and remains usable with strict reads on.
        query = ('SET SESSION ai_column_strict_read=ON; START TRANSACTION; '
                 'ALTER TABLE codex_ai_regression.docs RETRY AI COLUMN result WHERE id=1; '
                 'SELECT result FROM codex_ai_regression.docs; ROLLBACK; '
                 'SELECT result FROM codex_ai_regression.docs')
        result = subprocess.run([self.client, '-h127.0.0.1', '-P' + self.port,
                                 '-uroot', '-N', '-s', '--force'],
                                input=query.replace(';', ';\n') + ';\n', text=True, capture_output=True, timeout=30)
        self.assertEqual(result.stderr.count('ERROR 11120 '), 1, result.stderr)
        self.assertEqual(result.stdout.strip(), 'generation-1')
        # A model response for a dropped table must not starve later requests.
        self.sql("CREATE TABLE codex_ai_regression.doomed(id INT PRIMARY KEY, content VARCHAR(30),"
                 " result VARCHAR(100) AI COLUMN(AI_COMPLETE('codex_generation_model',content))"
                 " WITH(sync_mode='async',max_retries=0))")
        tid = self.sql("SELECT table_id FROM oceanbase.__all_table WHERE table_name='doomed' "
                       "AND database_id=(SELECT database_id FROM oceanbase.__all_database "
                       "WHERE database_name='codex_ai_regression')")
        self.sql("INSERT INTO codex_ai_regression.doomed(id,content) VALUES(1,'dropped')")
        self.assertTrue(self.started[2].wait(10), 'doomed table request did not start')
        self.sql('DROP TABLE codex_ai_regression.doomed')
        self.release[2].set()
        self.sql('ALTER TABLE codex_ai_regression.docs RETRY AI COLUMN result WHERE id=1')
        for _ in range(100):
            if self.sql('SELECT result FROM codex_ai_regression.docs') == 'generation-3':
                break
            time.sleep(.1)
        self.assertEqual(self.sql('SELECT result FROM codex_ai_regression.docs'), 'generation-3')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=' + tid), '0')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id=' + tid), '0')


if __name__ == '__main__':
    unittest.main()
