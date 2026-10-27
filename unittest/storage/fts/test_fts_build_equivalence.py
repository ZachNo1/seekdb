#!/usr/bin/env python3
"""Rebuild FTS indexes and compare complete IDs/scores across binary versions.

Uses only the dedicated fts_build_regression database on FTS_TEST_PORT (2904).
Run once with --capture baseline.json on the baseline, then on the candidate
with --capture candidate.json --reference baseline.json.
"""
import argparse
import json
import os
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--capture', type=Path, required=True)
parser.add_argument('--reference', type=Path)
args = parser.parse_args()
client = ['mysql', '-h127.0.0.1', '-P' + os.environ.get('FTS_TEST_PORT', '2904'),
          '-uroot', '-N', '-s', '-A', '--batch', '--raw', '--default-character-set=utf8mb4']
db = 'fts_build_regression'

def sql(query):
    result = subprocess.run(client, input='SET ob_query_timeout=100000000000;\n' + query,
                            text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()

def literal(text):
    return "CONVERT(X'%s' USING utf8mb4)" % text.encode().hex()

texts = ['alpha beta 数据库 全文索引', 'ALPHA café 数据库', 'beta gamma 中文分词',
         'e é É résumé 中文', 'trailing space  ', 'nul\x00byte emoji 😀', '', None,
         '数据库 数据库 alpha alpha', 'OceanBase database performance audit',
         '中文，标点。English! 123', 'alpha beta gamma ' * 80]
results = {}
sql('DROP DATABASE IF EXISTS ' + db + '; CREATE DATABASE ' + db + ';')
for collation in ['utf8mb4_bin', 'utf8mb4_general_ci', 'utf8mb4_unicode_ci']:
    for tokenizer in ['beng', 'ik']:
        table = db + '.docs_' + collation + '_' + tokenizer
        sql('CREATE TABLE ' + table + '(id BIGINT, k VARCHAR(40), title VARCHAR(64),'
            ' body TEXT, PRIMARY KEY(id,k)) DEFAULT CHARSET=utf8mb4 COLLATE=' + collation + ';')
        values = []
        for i in range(1, 145):
            body = texts[i % len(texts)]
            key = ['a', 'A ', 'é', '中文', '😀', 'z\x00q'][i % 6]
            title = ['alpha', '数据库', 'OceanBase', ''][i % 4]
            values.append('(%d,%s,%s,%s)' % (i, literal(key), literal(title),
                          'NULL' if body is None else literal(body)))
        sql('INSERT INTO ' + table + ' VALUES ' + ','.join(values) + ';'
            'ALTER TABLE ' + table + ' ADD FULLTEXT INDEX ft(title,body) WITH PARSER ' + tokenizer + ';')
        for query in ['alpha', 'beta gamma', 'café', '数据库', '中文 分词', 'OceanBase database', '', 'zznotpresent']:
            match = 'MATCH(title,body) AGAINST(' + literal(query) + ')'
            name = collation + '/' + tokenizer + '/' + query
            rows = sql('SELECT id,HEX(k),' + match + ' FROM ' + table + ' WHERE ' + match + ' ORDER BY id;')
            count = sql('SELECT COUNT(*) FROM ' + table + ' WHERE ' + match + ';')
            assert count == [str(len(rows))], (name, count, len(rows))
            ranked = sql('SELECT id,HEX(k),' + match + ' FROM ' + table + ' WHERE ' + match
                         + ' ORDER BY ' + match + ' DESC,id LIMIT 12;')
            results[name] = {'rows': rows, 'ranked': ranked, 'count': count}
        sql('DROP TABLE ' + table + ';')
sql('DROP DATABASE ' + db + ';')
args.capture.write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
if args.reference:
    baseline = json.loads(args.reference.read_text())
    differences = [key for key in results if results[key] != baseline.get(key)]
    assert not differences, differences
print(json.dumps({'builds': 6, 'query_cases': len(results), 'complete_results': 'captured',
                  'comparison': 'identical' if args.reference else 'reference'}))
