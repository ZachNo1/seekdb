#!/usr/bin/env python3
"""Fulltext DML visibility/ranking regression on an isolated test database."""
import os,subprocess,json
port=os.environ.get('FTS_TEST_PORT','2904')
sql="""
SET NAMES utf8mb4;
SET ob_query_timeout=100000000000;
DROP DATABASE IF EXISTS fts_hot_path_regression;
CREATE DATABASE fts_hot_path_regression;
USE fts_hot_path_regression;
CREATE TABLE docs(id INT PRIMARY KEY,c VARCHAR(500),e VARCHAR(500));
INSERT INTO docs VALUES(1,'数据库 database performance','database performance'),
 (2,'数据库 database audit','database audit'),
 (3,'全文索引 search performance','search performance'),
 (4,'other words','database database audit');
ALTER TABLE docs ADD FULLTEXT INDEX f_cn(c) WITH PARSER ik;
ALTER TABLE docs ADD FULLTEXT INDEX f_en(e) WITH PARSER beng;
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(e) AGAINST('database');
START TRANSACTION;
UPDATE docs SET c='全文索引',e='other words' WHERE id=2;
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(e) AGAINST('database');
ROLLBACK;
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
START TRANSACTION;
DELETE FROM docs WHERE id=1;
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
ROLLBACK;
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
INSERT INTO docs VALUES(5,'数据库','database');
SELECT GROUP_CONCAT(id ORDER BY id) FROM docs WHERE MATCH(c) AGAINST('数据库');
SELECT id FROM docs WHERE MATCH(c) AGAINST('数据库') ORDER BY MATCH(c) AGAINST('数据库') DESC,id LIMIT 1;
DROP DATABASE fts_hot_path_regression;
"""
run=subprocess.run(['mysql','-h127.0.0.1','-P'+port,'-uroot','-N','-s','-A','--batch','--raw','--default-character-set=utf8mb4'],input=sql,text=True,capture_output=True)
assert run.returncode==0,run.stderr
expected=['1,2','1,2,4','1','1,4','1,2','2','1,2','1,2,5','5']
actual=run.stdout.strip().splitlines()
assert actual==expected,(expected,actual)
print(json.dumps({'fulltext_insert_update_delete_rollback':'passed','ranking':'passed','checks':len(expected),'results':actual},ensure_ascii=False))
