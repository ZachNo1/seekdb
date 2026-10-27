#!/usr/bin/env python3
"""Score-elision/encoded-build SQL regression against explicitly scored rows."""
import json,os,subprocess
from decimal import Decimal
PORT=os.environ.get('FTS_TEST_PORT','2904')
MYSQL=['mysql','-h127.0.0.1','-P'+PORT,'-uroot','-N','-s','-A','--batch','--raw','--default-character-set=utf8mb4']
DB='fts_membership_regression'
def literal(text):return "CONVERT(X'%s' USING utf8mb4)"%text.encode().hex()
def sql(text):
    run=subprocess.run(MYSQL,input='SET ob_query_timeout=100000000000;\n'+text,text=True,capture_output=True)
    assert run.returncode==0,run.stderr
    return run.stdout.strip().splitlines()
sql('DROP DATABASE IF EXISTS '+DB+'; CREATE DATABASE '+DB+'; CREATE TABLE '+DB+'.docs(id BIGINT PRIMARY KEY,body VARCHAR(1000));')
texts=['alpha beta','ALPHA gamma','gamma delta','alpha alpha café','beta café','e é 中文 😀','',None]
values=[]
for i in range(1,161):
    text=texts[i%len(texts)]
    values.append('(%d,%s)'%(i,'NULL' if text is None else literal(text)))
sql('INSERT INTO '+DB+'.docs VALUES'+','.join(values)+'; ALTER TABLE '+DB+'.docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng;')
queries=['alpha','beta gamma','café','zznotfound','the and','']
queries+=['alpha '+' '.join('zzq'+''.join(chr(97+(i//(26**j))%26) for j in range(3)) for i in range(300))]
reference_counts={};checks=0
for text in queries:
    match='MATCH(body) AGAINST('+literal(text)+')'
    rows=sql('SELECT id,'+match+' FROM '+DB+'.docs WHERE '+match+' ORDER BY id;')
    values=[(int(x.split('\t')[0]),Decimal(x.split('\t')[1])) for x in rows]
    ids=[x[0] for x in values];reference_counts[text]=len(ids)
    result=sql('SELECT COUNT(*),SUM(1),MIN(1),MAX(1),AVG(1),COUNT(DISTINCT 1) FROM '+DB+'.docs WHERE '+match+';')[0].split('\t')
    assert int(result[0])==len(ids),result
    if ids:
        assert Decimal(result[1])==len(ids) and all(Decimal(x)==1 for x in result[2:]),result
    else:assert result[1:]==['NULL','NULL','NULL','NULL','0'],result
    checks+=1
    filtered=[i for i in ids if 20<=i<=100]
    result=sql('SELECT COUNT(*),SUM(id) FROM '+DB+'.docs WHERE '+match+' AND id BETWEEN 20 AND 100;')[0].split('\t')
    assert int(result[0])==len(filtered) and result[1]==(str(sum(filtered)) if filtered else 'NULL'),result
    checks+=1
    for threshold in ['0','0.1','2']:
        expected=sum(score>Decimal(threshold) for _,score in values)
        assert sql('SELECT COUNT(*) FROM '+DB+'.docs WHERE '+match+'>'+threshold+';')==[str(expected)]
        checks+=1
    top=sql('SELECT id FROM '+DB+'.docs WHERE '+match+' ORDER BY '+match+' DESC,id LIMIT 8;')
    assert [int(x) for x in top]==[i for i,_ in sorted(values,key=lambda x:(-x[1],x[0]))[:8]],top
    checks+=1
    scored=sql('SELECT SUM('+match+') FROM '+DB+'.docs WHERE '+match+';')[0]
    if values:
        assert abs(Decimal(scored)-sum((v for _,v in values),Decimal(0)))<Decimal('0.000000001'),scored
    else:assert scored=='NULL'
    checks+=1
query="SELECT COUNT(*) FROM "+DB+".docs WHERE MATCH(body) AGAINST(?)"
statements=["PREPARE fts_membership FROM '"+query+"';"]
for word in ['alpha','zznotfound','beta gamma','alpha']:
    statements += ['SET @q='+literal(word)+';','EXECUTE fts_membership USING @q;']
statements+=['DEALLOCATE PREPARE fts_membership;']
assert sql('\n'.join(statements))==[str(reference_counts[x]) for x in ['alpha','zznotfound','beta gamma','alpha']]
checks+=4
match="MATCH(body) AGAINST('alpha')"
before=reference_counts['alpha']
result=sql('START TRANSACTION; INSERT INTO '+DB+".docs VALUES(1001,'alpha'); SELECT COUNT(*) FROM "+DB+'.docs WHERE '+match+'; ROLLBACK; SELECT COUNT(*) FROM '+DB+'.docs WHERE '+match+';')
assert result==[str(before+1),str(before)],result
checks+=2
sql('DROP DATABASE '+DB+';')
print(json.dumps({'membership_and_scored_reference':'passed','checks':checks,'long_queries':'passed','prepared_reuse':'passed','transaction_visibility':'passed'}))
