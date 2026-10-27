"""Run against a local seekdb server; all model traffic stays in a local mock.

SEEKDB_TEST_PORT=2898 python3 -m unittest -v test_ai_function_enhancement.py
Requires the mysql CLI. Creates and removes its own database and AI endpoints.
"""
import json
import os
import subprocess
import threading
import unittest
import uuid
from ai_function_mock import MockAIService


def quote(value):
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


class AIFunctionEnhancement(unittest.TestCase):
    @classmethod
    def sql(cls, statement, error=False, database=True):
        command = [os.environ.get("SEEKDB_MYSQL_CLIENT", "mysql"), "-h127.0.0.1",
                   "-P" + os.environ.get("SEEKDB_TEST_PORT", "2898"), "-uroot",
                   "--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4"]
        if database:
            command += ["--database=" + cls.database]
        result = subprocess.run(command, input=statement, text=True, encoding="utf-8",
                                capture_output=True, timeout=45)
        if error:
            if result.returncode == 0:
                raise AssertionError("Expected SQL error: " + statement[:250])
        elif result.returncode:
            raise AssertionError(result.stderr + "\nSQL: " + statement[:500])
        return result.stdout.rstrip("\n")

    @classmethod
    def setUpClass(cls):
        cls.prefix = "ai_test_" + uuid.uuid4().hex[:8]
        cls.database = cls.prefix
        cls.resources = []
        cls.mock = MockAIService()
        cls.thread = threading.Thread(target=cls.mock.serve_forever, daemon=True)
        cls.thread.start()
        cls.sql("CREATE DATABASE " + cls.database, database=False)

    @classmethod
    def tearDownClass(cls):
        for name in cls.resources:
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(" + quote(name + "_ep") + ");")
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(" + quote(name) + ");")
        cls.sql("DROP DATABASE " + cls.database, database=False)
        cls.mock.shutdown()
        cls.mock.server_close()
        cls.thread.join()

    @classmethod
    def model(cls, kind="completion", provider="openai", mode="echo", omit_key=False):
        name = cls.prefix + "_" + str(len(cls.resources))
        native = provider.lower() == "ollama"
        endpoint = {"completion": "/api/generate" if native else "/chat/completions",
                    "dense_embedding": "/api/embed" if native else "/embeddings",
                    "rerank": "/rerank"}[kind]
        model_config = {"type": kind, "model_name": "validation-model"}
        config = {"ai_model_name": name, "scope": "all", "provider": provider,
                  "url": f"http://127.0.0.1:{cls.mock.server_port}/{mode}{endpoint}"}
        if not omit_key:
            config["access_key"] = "" if native else "synthetic-test-key"
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(" + quote(name) + "," + quote(json.dumps(model_config)) + ");")
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(" + quote(name + "_ep") + "," + quote(json.dumps(config)) + ");")
        cls.resources.append(name)
        return quote(name)

    def completion(self, model, prompt, config=None):
        extra = "" if config is None else "," + config
        return self.sql(f"SELECT ai_complete({model},{prompt}{extra});")

    def test_prompt_json_repetition_nested_and_unicode(self):
        model = self.model()
        result = self.completion(model, "ai_prompt('{0}/{0}/{1}', repeat('x',1000), json_array(1,true))")
        self.assertEqual(result, "x" * 1000 + "/" + "x" * 1000 + "/[1, true]")
        self.assertEqual(self.completion(model, "ai_prompt('outer {0}', ai_prompt('inner {0}', '你好😀'))"),
                         "outer inner 你好😀")
        value = '引号"、反斜杠\\、\n换行和 {9}'
        self.assertEqual(self.completion(model, "ai_prompt('{0}'," + quote(value) + ")"), value)

    def test_prompt_illegal_indices_and_null_column(self):
        model = self.model()
        for template in ("{1}", "{18446744073709551616}"):
            with self.subTest(template=template):
                self.sql(f"SELECT ai_complete({model},ai_prompt({quote(template)},'x'));", error=True)
        self.sql("CREATE TABLE null_prompt(v TEXT); INSERT INTO null_prompt VALUES(NULL);")
        self.sql("SELECT ai_prompt('{0}',v) FROM null_prompt;", error=True)

    def test_prompt_longtext(self):
        model = self.model()
        self.sql("CREATE TABLE long_prompt(t LONGTEXT,v LONGTEXT);"
                 "INSERT INTO long_prompt VALUES(concat(repeat('x',70000),'{0}'),repeat('你',40000));")
        result = self.sql(f"SELECT ai_complete({model},ai_prompt(t,v)) FROM long_prompt;")
        self.assertEqual(result, "x" * 70000 + "你" * 40000)

    def test_complete_config_object_and_null(self):
        model = self.model(mode="request")
        request = json.loads(self.completion(model, "'hello'", "json_object('temperature',0.2,'max_tokens',16)"))
        self.assertEqual(request["temperature"], 0.2)
        self.assertEqual(request["max_tokens"], 16)
        request = json.loads(self.completion(model, "'hello'", "NULL"))
        self.assertEqual(request["messages"][-1]["content"], "hello")
        for config in ("'[]'", "'null'", "json_array(1)", "'not JSON'"):
            with self.subTest(config=config):
                self.sql(f"SELECT ai_complete({model},'hello',{config});", error=True)

    def test_completion_bad_responses(self):
        for provider in ("openai", "ollama"):
            for mode in ("empty", "null", "wrong-type", "bad-json", "root-array", "http-error"):
                with self.subTest(provider=provider, mode=mode):
                    model = self.model(provider=provider, mode=mode)
                    self.sql(f"SELECT ai_complete({model},'hello');", error=True)
                    self.assertEqual(self.sql("SELECT 1;"), "1")

    def test_ollama_empty_key_case_alter_and_native_protocols(self):
        for provider in ("ollama", "OLLAMA", "Ollama"):
            with self.subTest(provider=provider):
                model = self.model(provider=provider, mode="request", omit_key=provider == "ollama")
                name = self.resources[-1]
                self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(" + quote(name + "_ep") + ",'{\"provider\":\"Ollama\"}');")
                request = json.loads(self.completion(model, "'你好'"))
                self.assertIs(request["stream"], False)
                self.assertEqual(request["prompt"], "你好")
                self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(" + quote(name + "_ep") + ",'{\"provider\":\"openai\"}');", error=True)
        model = self.model("dense_embedding", "Ollama")
        vector = json.loads(self.sql(f"SELECT ai_embed({model},'hello',5);"))
        self.assertEqual(len(vector), 5)

    def test_embedding_single_and_dimension_validation(self):
        model = self.model("dense_embedding")
        self.assertEqual(self.sql(f"SELECT json_length(ai_embed({model},'one'));"), "768")
        self.assertEqual(self.sql(f"SELECT json_length(ai_embed({model},'one',5));"), "5")
        for dimension in ("0", "-1", "NULL", "1.5", "'three'"):
            with self.subTest(dimension=dimension):
                self.sql(f"SELECT ai_embed({model},'one',{dimension});", error=True)
        self.sql("CREATE TABLE null_dimension(v INT); INSERT INTO null_dimension VALUES(NULL);")
        self.sql(f"SELECT ai_embed({model},'one',v) FROM null_dimension;", error=True)

    def test_embedding_batch_shuffle_null_filter_limit_and_repeated_batches(self):
        model = self.model("dense_embedding", mode="reverse")
        nulls = {0, 3, 67}
        self.sql("CREATE TABLE embed_rows(id INT PRIMARY KEY,content TEXT);")
        values = ",".join(f"({i}," + ("NULL" if i in nulls else quote(f"row-{i}")) + ")" for i in range(137))
        self.sql("INSERT INTO embed_rows VALUES" + values + ";")
        before = len(self.mock.events)
        result = self.sql(f"SELECT id,json_extract(ai_embed({model},content,4),'$[0]') FROM embed_rows ORDER BY id;")
        rows = [line.split("\t") for line in result.splitlines()]
        self.assertEqual(len(rows), 137)
        for key, value in rows:
            self.assertEqual(value, "NULL" if int(key) in nulls else key)
        requests = self.mock.events[before:]
        sizes = [len(event["body"]["input"]) for event in requests]
        self.assertGreater(max(sizes), 1, "SQL must combine multiple rows in a request")
        self.assertEqual(sum(sizes), 134)
        self.assertGreater(len(sizes), 1)
        self.assertLessEqual(max(sizes), 64)
        before = len(self.mock.events)
        dimensions = self.sql(f"SELECT json_length(ai_embed({model},content,4)) FROM embed_rows ORDER BY id;")
        self.assertEqual(dimensions.splitlines(), ["NULL" if i in nulls else "4" for i in range(137)])
        self.assertGreater(max(len(event["body"]["input"]) for event in self.mock.events[before:]), 1)
        result = self.sql(f"SELECT id,json_extract(ai_embed({model},content,4),'$[0]') FROM embed_rows "
                          "WHERE id>10 AND id<60 AND id%3=0 ORDER BY id LIMIT 5;")
        self.assertEqual(result.splitlines(), [f"{i}\t{i}" for i in (12,15,18,21,24)])
        failing = self.model("dense_embedding", mode="fail-later")
        self.sql(f"SELECT ai_embed({failing},content,4) FROM embed_rows ORDER BY id;", error=True)
        self.assertEqual(self.sql("SELECT 1;"), "1")

    def test_embedding_row_specific_model_and_dimension(self):
        first = self.model("dense_embedding")
        second = self.model("dense_embedding", "ollama")
        self.sql("CREATE TABLE varying_embed(id INT PRIMARY KEY,m VARCHAR(128),v TEXT,d INT);"
                 f"INSERT INTO varying_embed VALUES(1,{first},'row-1',3),(2,{second},'row-2',5),(3,{first},'row-3',7);")
        result = self.sql("SELECT id,json_length(ai_embed(m,v,d)) FROM varying_embed ORDER BY id;")
        self.assertEqual(result.splitlines(), ["1\t3", "2\t5", "3\t7"])

    def test_embedding_malformed_results(self):
        for mode in ("non-numeric", "non-finite", "empty-vector", "wrong-dimension", "out-of-range", "missing", "bad-data", "root-array", "bad-json"):
            with self.subTest(mode=mode):
                model = self.model("dense_embedding", mode=mode)
                self.sql(f"SELECT ai_embed({model},'row-1',4);", error=True)
        self.sql("CREATE TABLE embed_bad(v TEXT); INSERT INTO embed_bad VALUES('row-1'),('row-2');")
        for mode in ("duplicate", "inconsistent"):
            with self.subTest(mode=mode):
                model = self.model("dense_embedding", mode=mode)
                self.sql(f"SELECT ai_embed({model},v,4) FROM embed_bad;", error=True)
        self.assertEqual(self.sql("SELECT 1;"), "1")

    def test_rerank_strings_objects_batch_mapping_and_stability(self):
        model = self.model("rerank", "siliconflow", "scores")
        documents = [f"doc-{i}" for i in range(47)]
        result = json.loads(self.sql(f"SELECT ai_rerank({model},'query',{quote(json.dumps(documents))},NULL);"))
        self.assertEqual([r["index"] for r in result], list(reversed(range(47))))
        objects = [{"id": f"id-{i}", "text": doc} for i,doc in enumerate(documents)]
        before = len(self.mock.events)
        result = json.loads(self.sql(f"SELECT ai_rerank({model},'query',{quote(json.dumps(objects))},'text');"))
        self.assertEqual([r["id"] for r in result], [f"id-{i}" for i in reversed(range(47))])
        sizes = [len(event["body"]["documents"]) for event in self.mock.events[before:]]
        self.assertEqual(sizes, [20,20,7])
        model = self.model("rerank", "siliconflow", "ties")
        result = json.loads(self.sql(f"SELECT ai_rerank({model},'query',{quote(json.dumps(documents))});"))
        self.assertEqual([r["index"] for r in result], list(range(47)))

    def test_rerank_rejects_bad_response(self):
        for mode in ("duplicate", "out-of-range", "negative-index", "non-numeric", "non-finite", "missing", "root-array"):
            with self.subTest(mode=mode):
                model = self.model("rerank", "siliconflow", mode)
                self.sql(f"SELECT ai_rerank({model},'query','[\"one\",\"two\"]');", error=True)
                self.assertEqual(self.sql("SELECT 1;"), "1")

    def test_json_wrappers_preserve_ordinary_sql_null_paths_and_filtering(self):
        self.sql("CREATE TABLE ordinary_json(id INT PRIMARY KEY,j LONGTEXT);"
                 "INSERT INTO ordinary_json VALUES(1,'[1,2]'),(2,NULL),(3,'{\"x\":[1,2,3]}'),(4,'[]');")
        rows = self.sql("SELECT id,json_length(j),json_extract(j,'$.x') FROM ordinary_json ORDER BY id;").splitlines()
        self.assertEqual([line.split('\t')[:2] for line in rows],
                         [['1','2'],['2','NULL'],['3','1'],['4','0']])
        self.assertEqual(json.loads(rows[2].split('\t')[2]), [1,2,3])
        self.assertTrue(all(rows[i].split('\t')[2] == 'NULL' for i in (0,1,3)))
        result = self.sql("SELECT id,json_length(j,'$.x') FROM ordinary_json WHERE id>=2 ORDER BY id LIMIT 2;")
        self.assertEqual(result.splitlines(), ['2\tNULL','3\t3'])
        rows = self.sql("SELECT IF(id=3,json_extract(j,'$.x'),json_array()) FROM ordinary_json ORDER BY id;").splitlines()
        self.assertEqual([json.loads(row) for row in rows], [[],[],[1,2,3],[]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
