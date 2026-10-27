#!/usr/bin/env python3
"""Additional native-SQL checks; requires mysql CLI and a test seekdb instance."""
import json
import os
from pathlib import Path
import subprocess
import time

PORT = int(os.environ.get("AI_COLUMN_TEST_PORT", "2903"))
MOCK_PORT = 19126
DB = "codex_ai_column_regression"
MODEL = "ai_column_regression_complete"
ENDPOINT = "ai_column_regression_ep"
MOCK = Path(__file__).with_name("ai_column_mock.py")

def sql(statement, error=None):
    result = subprocess.run(["mysql", "-h127.0.0.1", "-P"+str(PORT), "-uroot", "--batch", "--raw", "--skip-column-names"],
                            input=statement, text=True, capture_output=True)
    if error is not None:
        assert result.returncode != 0 and "ERROR %d " % error in result.stderr, result
    else:
        assert result.returncode == 0, result.stderr
    return result.stdout.strip()

def mock(command, *args):
    return subprocess.run(["python3", str(MOCK), command, str(MOCK_PORT), *args], check=True, text=True, capture_output=True)

def wait_drained():
    query = ("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task x "
             "JOIN oceanbase.__all_table t ON t.table_id=x.table_id "
             "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
             "WHERE d.database_name='%s'" % DB)
    deadline = time.monotonic()+15
    while time.monotonic()<deadline:
        if sql(query)=="0": return
        time.sleep(.1)
    raise AssertionError("AI tasks did not drain")

def main():
    mock("start")
    sql("DROP DATABASE IF EXISTS %s; CREATE DATABASE %s" % (DB,DB))
    for statement, code in [("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('%s')" % ENDPOINT,11112),
                            ("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('%s')" % MODEL,11118)]:
        try: sql(statement)
        except AssertionError as exc:
            assert "ERROR %d " % code in str(exc), str(exc)
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('%s','{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')" % MODEL)
    endpoint=json.dumps({"ai_model_name":MODEL,"url":"http://127.0.0.1:%d/v1/chat/completions" % MOCK_PORT,
                         "access_key":"reg-key","provider":"openai","request_model_name":"mock-complete"})
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('%s','%s')" % (ENDPOINT,endpoint))
    sql("CREATE TABLE %s.cases(id VARCHAR(64) PRIMARY KEY, content VARCHAR(64), "
        "result VARCHAR(6) AI COLUMN(AI_COMPLETE('%s',content)) WITH(max_retries=0))" % (DB,MODEL))
    shown=sql("SHOW AI COLUMNS FROM %s.cases" % DB)
    assert shown.startswith("result\t") and "AI_COMPLETE" in shown, shown
    print("show_definition=ok",flush=True)
    sql("INSERT INTO %s.cases(id,content) VALUES('quoted''key','汉')" % DB)
    wait_drained()
    assert sql("SELECT result FROM %s.cases WHERE id='quoted''key'" % DB)=="mock:汉"
    print("utf8_length_and_quoted_rowkey=ok",flush=True)
    sql("UPDATE %s.cases SET result='x' WHERE id='quoted''key'" % DB,error=1235)
    sql("INSERT INTO %s.cases(id,content,result) VALUES('bad','x','x')" % DB,error=1235)
    assert sql("SELECT result FROM %s.cases WHERE id='quoted''key'" % DB)=="mock:汉"
    print("direct_result_writes_rejected=ok",flush=True)
    sql("INSERT INTO %s.cases(id,content) VALUES('null-input',NULL)" % DB)
    wait_drained()
    assert sql("SET ai_column_strict_read=ON; SELECT result IS NULL FROM %s.cases WHERE id='null-input'" % DB)=="1"
    print("ready_null_is_readable=ok",flush=True)
    sql("INSERT INTO %s.cases(id,content) VALUES('too-long','overlong-input')" % DB)
    wait_drained()
    assert sql("SELECT result IS NULL FROM %s.cases WHERE id='too-long'" % DB)=="1"
    sql("SET ai_column_strict_read=ON; SELECT result FROM %s.cases WHERE id='quoted''key'" % DB,error=11120)
    sql("UPDATE %s.cases SET content='X' WHERE id='too-long'" % DB)
    wait_drained()
    assert sql("SET ai_column_strict_read=ON; SELECT result FROM %s.cases WHERE id='too-long'" % DB)=="mock:X"
    print("overlong_response_reaches_failed_and_recovers=ok",flush=True)
    mock("reset")
    sql("START TRANSACTION; INSERT INTO %s.cases(id,content) VALUES('rolled-back','Q'); DO SLEEP(0.3); ROLLBACK" % DB)
    wait_drained();mock("assert","no-requests")
    assert sql("SELECT COUNT(*) FROM %s.cases WHERE id='rolled-back'" % DB)=="0"
    print("rollback_sends_no_request=ok",flush=True)
    assert sql("CREATE TABLE %s.plain(id INT PRIMARY KEY,n INT); INSERT INTO %s.plain VALUES(1,2); "
               "UPDATE %s.plain SET n=n+2; SELECT SUM(n) FROM %s.plain" % (DB,DB,DB,DB))=="4"
    print("ordinary_sql=ok",flush=True)

if __name__=="__main__": main()
