#!/usr/bin/env python3
"""Native seekdb file SQL regression; requires a dedicated local test server.

FILE_SQL_TEST_PORT defaults to 2925. FILE_SQL_TEST_ROOT defaults to
/root/sdbFileFinal. PyArrow is used only to generate Parquet fixtures, never
to execute SQL. No external SQL engine is used.
"""
import csv, datetime, json, os, subprocess, tempfile, threading, time
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

port=os.environ.get('FILE_SQL_TEST_PORT','2925')
base=Path(os.environ.get('FILE_SQL_TEST_ROOT','/root/sdbFileFinal'))
root=Path(tempfile.mkdtemp(prefix='file-sql-regression-',dir=base))
client=['mysql','-h127.0.0.1','-P'+port,'-uroot','-N','-s','-A','--batch','--default-character-set=utf8mb4']
checks=[]
def literal(text): return "'"+str(text).replace('\\','\\\\').replace("'","''")+"'"
def run(sql, enabled=True):
    prefix='SET ob_query_timeout=20000000; '+('SET enable_file_sql=ON; ' if enabled else '')
    connection=client
    if sql.startswith('SET GLOBAL secure_file_priv='):
        connection=['mysql','--socket='+str(base/'run/sql.sock'),'-uroot','-N','-s','--batch']
    return subprocess.run(connection,input=prefix+sql,text=True,capture_output=True,timeout=35)
def expect(name,sql,expected=None,error=None,contains=None,enabled=True):
    result=run(sql,enabled)
    passed=(result.returncode==0) if error is None else (result.returncode!=0 and str(error) in result.stderr)
    if expected is not None: passed=passed and result.stdout.splitlines()==expected
    if contains is not None: passed=passed and contains in result.stdout
    checks.append(dict(name=name,passed=passed,stdout=result.stdout,stderr=result.stderr,sql=sql))
    print(('PASS ' if passed else 'FAIL ')+name,flush=True)
    return result
def filefn(name,kind='read_csv',options=''):
    return kind+'('+literal(root/name)+options+')'
def writecsv(name,rows,delimiter=','):
    with (root/name).open('w',newline='',encoding='utf-8') as stream:
        csv.writer(stream,delimiter=delimiter).writerows(rows)

writecsv('left.csv',[['id','grp','score','note','active','day','created'],
    [1,'A',10.5,'hello,世界','true','2024-02-29','2024-02-29 10:11:12.000001'],
    [2,'A',20,'NULL','false','2024-03-01','2024-03-01 10:11:12'],
    [3,'B','NULL','line1\nline2','true','2024-03-02','2024-03-02 10:11:12.12'],
    [4,'B',5,'quote " and slash \\','NULL','2024-03-03','2024-03-03 10:11:12']])
(root/'right.jsonl').write_text('\n'.join(json.dumps(x) for x in [
    {'id':1,'label':'one'},{'id':3,'label':'three','extra':True},{'id':5,'label':'five'}])+'\n')
left=filefn('left.csv',options=", nullstr='NULL'")
right=filefn('right.jsonl','read_ndjson')
expect('default disabled','SELECT @@enable_file_sql;', ['0'],enabled=False)
expect('disabled reader','SELECT * FROM '+left+';',error=1235,enabled=False)
expect('projection expression sort limit','SELECT id*2 AS doubled,grp FROM '+left+' WHERE score>=10 ORDER BY id DESC LIMIT 1;', ['4\tA'])
expect('aggregates having','SELECT grp,COUNT(*),SUM(id),ROUND(AVG(score),2),MIN(score),MAX(score) FROM '+left+' GROUP BY grp HAVING COUNT(*)=2 ORDER BY grp;', ['A\t2\t3\t15.25\t10.5\t20','B\t2\t7\t5.00\t5\t5'])
expect('boolean null','SELECT SUM(active),COUNT(active),COUNT(score) FROM '+left+';', ['2\t3\t3'])
expect('fractional datetime preserved','SELECT created FROM '+left+' WHERE id=1;', ['2024-02-29 10:11:12.000001'])
expect('csv multiline and unicode','SELECT HEX(note) FROM '+left+' WHERE id=3;', ['6C696E65310A6C696E6532'])
expect('inner cross format join','SELECT a.id,b.label FROM '+left+' a JOIN '+right+' b ON a.id=b.id ORDER BY a.id;', ['1\tone','3\tthree'])
expect('left join null extension','SELECT a.id,b.label FROM '+left+' a LEFT JOIN '+right+' b ON a.id=b.id ORDER BY a.id;', ['1\tone','2\tNULL','3\tthree','4\tNULL'])
expect('nested loop rescan','SELECT /*+ USE_NL(a b) */ a.id,b.id FROM '+left+' a JOIN '+left+' b ON a.id=b.id ORDER BY a.id;', ['1\t1','2\t2','3\t3','4\t4'])
expect('native explain join','EXPLAIN SELECT a.id FROM '+left+' a JOIN '+right+' b ON a.id=b.id;',contains='JOIN')
expect('native explain file scan','EXPLAIN SELECT * FROM '+left+';',contains='FUNCTION_TABLE')
expect('ordinary explain preserved','EXPLAIN SELECT 1;',contains='EXPRESSION')
expect('ordinary describe preserved','DESCRIBE SELECT 1;',contains='EXPRESSION')
expect('describe alias','DESCRIBE SELECT id AS ident,created FROM '+left+';',contains='ident\tBIGINT')
expect('desc boolean metadata','DESC SELECT active FROM '+left+';',contains='BOOLEAN')
expect('derived file table','SELECT id FROM (SELECT * FROM '+left+') d WHERE id=2;', ['2'])
expect('json union and missing','SELECT id,extra FROM '+right+' ORDER BY id;', ['1\tNULL','3\t1','5\tNULL'])
expect('sample size unsupported','SELECT * FROM '+filefn('left.csv',options=', sample_size=3')+';',error=1235)
expect('unknown option','SELECT * FROM '+filefn('left.csv',options=', wrong=1')+';',error=1064)
writecsv('headerless.csv',[[1,'first'],[2,'second']],';')
expect('headerless separator','SELECT column0,column1 FROM '+filefn('headerless.csv',options=", header=false, delimiter=';'")+' ORDER BY column0;', ['1\tfirst','2\tsecond'])
writecsv('null.csv',[['v','id'],['',1],['',2]])
expect('all null schema','DESCRIBE SELECT v FROM '+filefn('null.csv')+';',contains='v\tNULL\tYES')
expect('all null aggregate','SELECT COUNT(v),COUNT(*) FROM '+filefn('null.csv')+';', ['0\t2'])
writecsv('boundary.csv',[['i','wide','date'],[-9223372036854775808,9223372036854775808,'2023-02-29'],[9223372036854775807,3,'2024-02-29']])
expect('integer boundaries','SELECT i,wide FROM '+filefn('boundary.csv')+' ORDER BY i;', ['-9223372036854775808\t9223372036854775808','9223372036854775807\t3'])
expect('invalid date widens','DESCRIBE SELECT date FROM '+filefn('boundary.csv')+';',contains='date\tVARCHAR')
writecsv('widen.csv',[['n','d'],['true','2024-02-29'],['2.5','2024-03-01 01:02:03.12']])
expect('numeric and temporal widening','SELECT n,d FROM '+filefn('widen.csv')+' ORDER BY n;', ['1\t2024-02-29 00:00:00.00','2.5\t2024-03-01 01:02:03.12'])
writecsv('big.csv',[['id','name']]+[[i,'v'+str(i)] for i in range(1,2501)])
expect('multiple batches','SELECT COUNT(*),SUM(id),MIN(name),MAX(id) FROM '+filefn('big.csv')+';', ['2500\t3126250\tv1\t2500'])
for name,data,kind in [('bad.csv','a,b\n1,2,3\n','read_csv'),('quote.csv','x\n"unterminated','read_csv'),
                       ('nested.jsonl','{"a":{}}\n','read_json'),('array.jsonl','{"a":[]}\n','read_json'),
                       ('invalid.jsonl','{"a":01}\n','read_json')]:
    (root/name).write_text(data)
    expect('reject '+name,'SELECT * FROM '+filefn(name,kind)+';',error=1210)
(root/'utf8.csv').write_bytes(b'x\n\xff\n')
expect('invalid utf8','SELECT * FROM '+filefn('utf8.csv')+';',error=1210)
expect('missing file','SELECT * FROM '+filefn('absent.csv')+';',error=1210)

table=pa.table({'id':pa.array([1,2,3,4,5],type=pa.int64()),
    'text':pa.array(['one',None,'three','世界','five']), 'flag':pa.array([True,False,None,True,False]),
    'score':pa.array([1.25,None,3.5,4.0,5.5]),
    'day':pa.array([datetime.date(2024,2,29)]*5,type=pa.date32()),
    'ts':pa.array([datetime.datetime(2024,2,29,1,2,3,123456)]*5,type=pa.timestamp('us'))})
for version in ['1.0','2.0']:
    name='data'+version+'.parquet';pq.write_table(table,root/name,row_group_size=2,compression='snappy',data_page_version=version)
    pqfn=filefn(name,'read_parquet')
    expect('parquet rowgroups '+version,'SELECT COUNT(*),SUM(id),COUNT(text),SUM(flag) FROM '+pqfn+';', ['5\t15\t4\t2'])
    expect('parquet temporal '+version,'SELECT day,ts FROM '+pqfn+' WHERE id=1;', ['2024-02-29\t2024-02-29 01:02:03.123456'])
    expect('parquet projection '+version,'SELECT text FROM '+pqfn+' WHERE id=4;', ['世界'])
expect('parquet csv join','SELECT a.id,b.text FROM '+left+' a JOIN '+filefn('data1.0.parquet','read_parquet')+' b ON a.id=b.id ORDER BY a.id;', ['1\tone','2\tNULL','3\tthree','4\t世界'])
(root/'broken.parquet').write_bytes(b'PAR1BADPAR1')
expect('corrupt parquet','SELECT * FROM '+filefn('broken.parquet','read_parquet')+';',error=1210)
# Tiny malformed metadata declaring repeated enormous nested lists must be
# rejected before allocating every declared child array.
metadata=bytes([0x19])+bytes([0xf9,0x80,0x80,0x10])*32+bytes([0])
(root/'budget.parquet').write_bytes(b'PAR1'+metadata+len(metadata).to_bytes(4,'little')+b'PAR1')
expect('parquet metadata allocation bound','SELECT * FROM '+filefn('budget.parquet','read_parquet')+';',error=1210)

expect('glob discovery','SELECT COUNT(*) FROM glob('+literal(root/'data*.parquet')+');',['2'])

copy_path=root/'export.csv'
expect('copy query','COPY (SELECT id,note FROM '+left+' ORDER BY id) TO '+literal(copy_path)+" (FORMAT CSV, HEADER, DELIMITER ';', QUOTE '\"', ESCAPE '\"', NULLSTR 'NULL');", [])
expect('copy readback','SELECT id,HEX(note) FROM '+filefn('export.csv',options=", delimiter=';', nullstr='NULL'")+' ORDER BY id;',
       ['1\t'+('hello,世界'.encode().hex().upper()),'2\tNULL','3\t6C696E65310A6C696E6532','4\t'+('quote " and slash \\'.encode().hex().upper())])
old=copy_path.read_bytes() if copy_path.exists() else b''
expect('copy no overwrite','COPY (SELECT 1) TO '+literal(copy_path)+';',error=1210)
assert copy_path.read_bytes()==old
expect('copy source table','CREATE DATABASE IF NOT EXISTS file_sql_regression; CREATE TABLE file_sql_regression.t(id INT); INSERT INTO file_sql_regression.t VALUES(8),(9); COPY file_sql_regression.t TO '+literal(root/'table.csv')+' (HEADER); DROP DATABASE file_sql_regression;',[])
expect('copy table readback','SELECT SUM(id) FROM '+filefn('table.csv')+';', ['17'])
expect('copy failure cleanup','COPY (SELECT note FROM '+left+') TO '+literal(root/'failed.csv')+" (QUOTE '');",error=1210)
checks.append(dict(name='no partial COPY files',passed=not (root/'failed.csv').exists() and not list(root.glob('.seekdb-copy-*'))))

secure=run('SELECT @@GLOBAL.secure_file_priv;').stdout.strip()
changed=expect('restrict secure root','SET GLOBAL secure_file_priv='+literal(root)+';',[])
if changed.returncode==0:
    try:
        expect('allowed secure path','SELECT COUNT(*) FROM '+left+';',['4'])
        expect('deny absolute outside','SELECT * FROM read_csv(\'/etc/passwd\');',error=1210)
        (root/'escape.csv').symlink_to('/etc/passwd')
        expect('deny symlink escape','SELECT * FROM '+filefn('escape.csv')+';',error=1210)
        expect('deny traversal','SELECT * FROM read_csv('+literal(root/'../../../../etc/passwd')+');',error=1210)
        expect('deny glob escape',"SELECT * FROM glob('/etc/*');",error=1210)
        expect('deny copy escape','COPY (SELECT 1) TO '+literal(base/'outside-export.csv')+';',error=1210)
    finally:
        restored=run('SET GLOBAL secure_file_priv='+literal(secure)+';')
        assert restored.returncode==0,restored.stderr

writecsv('changing.csv',[['id']]+[[i] for i in range(300)])
def mutate():
    time.sleep(.35)
    with (root/'changing.csv').open('a') as stream:stream.write('9999\n')
thread=threading.Thread(target=mutate);thread.start()
expect('change during streaming','SELECT SLEEP(0.01),id FROM '+filefn('changing.csv')+';',error=1210)
thread.join()
expect('ordinary SQL intact','SELECT 2+3,CONCAT(\'a\',\'b\');',['5\tab'])
result={'root':str(root),'passed':sum(x['passed'] for x in checks),'total':len(checks),'checks':checks}
(root/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='checks'}),flush=True)
assert all(x['passed'] for x in checks), [x['name'] for x in checks if not x['passed']]
