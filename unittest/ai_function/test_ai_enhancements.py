"""Integration regression tests against a running seekdb and a local HTTP mock.

Run: SEEKDB_TEST_PORT=10102 python3 -m unittest discover -s unittest/ai_function -v
SEEKDB_TEST_CLIENT may point to an alternative MySQL-compatible CLI.
"""
import json
import os
import subprocess
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class AIEnhancements(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.client = os.environ.get(
            "SEEKDB_TEST_CLIENT",
            str(root.parent / "seekdb-competition/deps/3rd/u01/obclient/bin/obclient"),
        )
        cls.port = os.environ.get("SEEKDB_TEST_PORT", "10102")
        cls.prefix = "codex_ai_" + uuid.uuid4().hex[:8]
        cls.events = []
        cls.lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with cls.lock:
                    cls.events.append((self.path, body))
                if self.path.endswith("/embeddings") or self.path == "/api/embed":
                    inputs = body["input"]
                    inputs = inputs if isinstance(inputs, list) else [inputs]
                    dimensions = body.get("dimensions", 768 if body.get("model") == "text-embedding-v1" else 4)
                    vectors = [[float(len(text))] + [0.0] * (dimensions - 1) for text in inputs]
                    if any(text == "bad-shape" for text in inputs):
                        vectors = [["not-a-number"] for _ in inputs]
                    if self.path == "/api/embed":
                        response = {"embeddings": vectors}
                    else:
                        response = {"data": [
                            {"index": i, "embedding": vector}
                            for i, vector in reversed(list(enumerate(vectors)))
                        ]}
                        if any(text == "bad-index" for text in inputs):
                            response["data"][0]["index"] = len(inputs)
                elif self.path == "/v1/rerank":
                    docs = body["documents"]
                    def score(text):
                        prefix = text.split(":")[0]
                        return int(prefix) if prefix.isdigit() else int(body["query"] in text)
                    response = {"results": [
                        {"index": i, "relevance_score": score(text)}
                        for i, text in reversed(list(enumerate(docs)))
                    ]}
                    if docs[0] == "duplicate-index":
                        response = {"results": [
                            {"index": 0, "relevance_score": 1},
                            {"index": 0, "relevance_score": 2},
                        ]}
                elif self.path == "/api/generate":
                    response = {"response": "answer:" + body["prompt"]}
                else:
                    prompt = body["messages"][-1]["content"]
                    response = {"choices": [{"message": {
                        "content": "" if prompt == "empty-response" else "answer:" + prompt
                    }}]}
                payload = json.dumps(response, ensure_ascii=False).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server_thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.server_thread.start()
        cls.models = []
        cls.sql("CREATE DATABASE " + cls.prefix)
        for suffix, kind, provider, path in [
            ("complete", "completion", "openai", "/v1/chat/completions"),
            ("embed", "dense_embedding", "openai", "/v1/embeddings"),
            ("rerank", "rerank", "siliconflow", "/v1/rerank"),
            ("ollama_c", "completion", "ollama", "/api/generate"),
            ("ollama_e", "dense_embedding", "OLLAMA", "/api/embed"),
        ]:
            name = cls.prefix + "_" + suffix
            config = json.dumps({"type": kind, "model_name": suffix})
            cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)" % (cls.quote(name), cls.quote(config)))
            endpoint = json.dumps({
                "ai_model_name": name, "scope": "all", "provider": provider,
                "url": "http://127.0.0.1:%d%s" % (cls.server.server_port, path),
                "access_key": "" if provider.lower() == "ollama" else "mock-key",
            })
            cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)" % (
                cls.quote(name + "_ep"), cls.quote(endpoint)))
            cls.models.append(name)

    @staticmethod
    def quote(value):
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"

    @classmethod
    def sql(cls, query, fail=False):
        result = subprocess.run([
            cls.client, "-h127.0.0.1", "-P" + cls.port, "-uroot",
            "--batch", "--skip-column-names", "--raw", "--connect-timeout=5",
            "-e", query,
        ], text=True, capture_output=True, timeout=60)
        if fail:
            if result.returncode == 0:
                raise AssertionError("Expected SQL failure: " + query)
        elif result.returncode:
            raise AssertionError(result.stderr + "\nSQL: " + query)
        return result.stdout.strip()

    @classmethod
    def tearDownClass(cls):
        for name in cls.models:
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)" % cls.quote(name + "_ep"))
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)" % cls.quote(name))
        cls.sql("DROP DATABASE " + cls.prefix)
        cls.server.shutdown()
        cls.server.server_close()
        cls.server_thread.join()

    def setUp(self):
        with self.lock:
            self.events.clear()

    def model(self, suffix):
        return self.quote(self.prefix + "_" + suffix)

    def test_prompt_json_and_nested_rendering(self):
        value = json.loads(self.sql("SELECT AI_PROMPT('{0}/{0}/{1}',JSON_ARRAY(1,true),JSON_OBJECT('k',2))"))
        self.assertEqual(value["args"], [[1, True], {"k": 2}])
        self.assertEqual(self.sql("SELECT AI_COMPLETE(%s,AI_PROMPT('outer:{0}',AI_PROMPT('inner:{0}','你好')))" %
                                  self.model("complete")), "answer:outer:inner:你好")

    def test_prompt_repeated_placeholders_expand_without_truncation(self):
        result = self.sql("SELECT LENGTH(AI_COMPLETE(%s,AI_PROMPT(REPEAT('{0}',80),REPEAT('x',100))))" %
                          self.model("complete"))
        self.assertEqual(int(result), len("answer:") + 8000)

    def test_lob_prompt_and_arguments(self):
        table = self.prefix + ".lob_prompt"
        self.sql("CREATE TABLE %s(t LONGTEXT,a LONGTEXT)" % table)
        self.sql("INSERT INTO %s VALUES('{0}-ok',REPEAT('x',100000))" % table)
        value = json.loads(self.sql("SELECT AI_PROMPT(t,a) FROM " + table))
        self.assertEqual(value["args"], ["x" * 100000])

    def test_invalid_placeholder_fails_before_http(self):
        for index in ["1", "999999999999999999999999999999999999999"]:
            self.sql("SELECT AI_COMPLETE(%s,AI_PROMPT('{%s}','one'))" %
                     (self.model("complete"), index), fail=True)
        self.assertEqual(self.events, [])

    def test_completion_null_and_json_config(self):
        self.assertEqual(self.sql("SELECT AI_COMPLETE(%s,'hello',NULL)" % self.model("complete")), "answer:hello")
        self.sql("SELECT AI_COMPLETE(%s,'configured',JSON_OBJECT('temperature',0.2,'max_tokens',16))" %
                 self.model("complete"))
        self.assertEqual(self.events[-1][1]["max_tokens"], 16)
        self.sql("SELECT AI_COMPLETE(%s,'empty-response')" % self.model("complete"), fail=True)
        self.sql("SELECT AI_COMPLETE(%s,'hello','[]')" % self.model("complete"), fail=True)

    def test_embedding_dimension_and_invalid_responses(self):
        self.assertEqual(json.loads(self.sql("SELECT AI_EMBED(%s,'hello',3)" % self.model("embed"))), [5, 0, 0])
        for text in ["bad-shape", "bad-index"]:
            self.sql("SELECT AI_EMBED(%s,%s)" % (self.model("embed"), self.quote(text)), fail=True)
        for dim in ["NULL", "0", "-1", "1.5"]:
            self.sql("SELECT AI_EMBED(%s,'hello',%s)" % (self.model("embed"), dim), fail=True)

    def test_embedding_batch_nulls_mapping_and_real_batch_request(self):
        table = self.prefix + ".embed_rows"
        self.sql("CREATE TABLE %s(id INT PRIMARY KEY,t TEXT)" % table)
        self.sql("INSERT INTO %s VALUES (1,'a'),(2,NULL),(3,'bbb'),(4,'cc')" % table)
        rows = self.sql("SELECT id,AI_EMBED(%s,t,3) FROM %s ORDER BY id" % (self.model("embed"), table)).splitlines()
        self.assertEqual([row.split("\t", 1)[1] for row in rows][1], "NULL")
        self.assertEqual([json.loads(rows[i].split("\t", 1)[1])[0] for i in (0, 2, 3)], [1, 3, 2])
        requests = [body for path, body in self.events if path.endswith("/embeddings")]
        self.assertTrue(any(len(body["input"]) > 1 for body in requests), requests)
        self.assertEqual(sum(len(body["input"]) for body in requests), 3)

    def test_rerank_cross_batch_order_and_original_ids(self):
        docs = [{"id": i, "text": "%d:document" % (i % 7)} for i in range(45)]
        result = json.loads(self.sql("SELECT AI_RERANK(%s,'q',%s,'text')" %
                                    (self.model("rerank"), self.quote(json.dumps(docs)))))
        expected = sorted(range(45), key=lambda i: (-(i % 7), i))
        self.assertEqual([item["id"] for item in result], expected)
        self.assertEqual([len(body["documents"]) for path, body in self.events], [20, 20, 5])

    def test_rerank_null_key_and_integer_score(self):
        docs = self.quote(json.dumps(["1:first", "2:second", "2:third"]))
        result = json.loads(self.sql("SELECT AI_RERANK(%s,'q',%s,NULL)" % (self.model("rerank"), docs)))
        self.assertEqual([item["index"] for item in result], [1, 2, 0])

    def test_rerank_duplicate_or_invalid_input_fails(self):
        self.sql("SELECT AI_RERANK(%s,'q','[\"duplicate-index\",\"1:other\"]',NULL)" %
                 self.model("rerank"), fail=True)
        for value in ["{}", "[]", "[1,2]", '[{"id":1}]']:
            self.sql("SELECT AI_RERANK(%s,'q',%s,'text')" %
                     (self.model("rerank"), self.quote(value)), fail=True)

    def test_embedding_multiple_batches_filter_and_limit(self):
        table = self.prefix + ".embed_many"
        self.sql("CREATE TABLE %s(id INT PRIMARY KEY,t LONGTEXT)" % table)
        values = ",".join("(%d,'%s')" % (i, "x" * (i % 13 + 1)) for i in range(145))
        self.sql("INSERT INTO %s VALUES %s" % (table, values))
        rows = self.sql("SELECT id,AI_EMBED(%s,t,3) FROM %s WHERE id %% 2 = 0 ORDER BY id LIMIT 67" %
                        (self.model("embed"), table)).splitlines()
        self.assertEqual(len(rows), 67)
        for row in rows:
            index, value = row.split("\t", 1)
            self.assertEqual(json.loads(value)[0], int(index) % 13 + 1)

    def test_ollama_empty_key_and_non_streaming(self):
        self.assertEqual(self.sql("SELECT AI_COMPLETE(%s,'hello')" % self.model("ollama_c")), "answer:hello")
        self.assertIs(self.events[-1][1]["stream"], False)
        self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,'{\"provider\":\"Ollama\"}')" %
                 self.quote(self.prefix + "_ollama_c_ep"))
        self.assertEqual(self.sql("SELECT AI_COMPLETE(%s,'again')" % self.model("ollama_c")), "answer:again")
        self.assertEqual(json.loads(self.sql("SELECT AI_EMBED(%s,'hello')" % self.model("ollama_e"))), [5, 0, 0, 0])


if __name__ == "__main__":
    unittest.main()
