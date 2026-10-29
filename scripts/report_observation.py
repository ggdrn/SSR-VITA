#!/usr/bin/env python3
"""Extract bounded SRR OBS telemetry into a timeline, asset table and report."""
import argparse,csv,json,re
from pathlib import Path

def analyze(text):
    events=[];bad=0;health=[]
    for line in text.splitlines():
        at=line.find('OBS {')
        if at>=0:
            try: events.append(json.loads(line[at+4:]))
            except (ValueError,TypeError): bad+=1
        if 'OBS health ' in line:
            health.append({k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)})
    events.sort(key=lambda e:(e.get('start_us',0),e.get('frame',0)))
    agg={};spans={}
    for e in events:
        key=(e.get('phase',0),e.get('kind',''),e.get('key',0),e.get('name',''))
        if e.get('type')=='asset':
            if key not in agg or e.get('count',0)>agg[key].get('count',0): agg[key]=e
        elif e.get('type')=='span' and e.get('kind') not in ('boot','input','gc'):
            spans.setdefault(key,[]).append(e)
    assets=[]
    for key in agg.keys()|spans.keys():
        if key in agg:
            e=agg[key];calls=e['count'];total=e['total_us'];worst=e['max_us'];size=e['bytes'];coverage='aggregate'
        else:
            rows=spans[key];e=rows[0];calls=len(rows);total=sum(x['duration_us'] for x in rows);worst=max(x['duration_us'] for x in rows);size=sum(x['bytes'] for x in rows);coverage='observed spans only'
        assets.append(dict(phase=key[0],kind=key[1],key=key[2],name=key[3],calls=calls,total_ms=round(total/1000,3),max_ms=round(worst/1000,3),bytes=size,coverage=coverage))
    assets.sort(key=lambda r:r['total_ms'],reverse=True)
    phases=[]
    marks=[e for e in events if e.get('kind')=='boot' and e.get('name','').startswith('scene=')]
    end=max((e.get('start_us',0)+e.get('duration_us',0) for e in events),default=0)
    for i,e in enumerate(marks):
        until=marks[i+1]['start_us'] if i+1<len(marks) else end
        phases.append(dict(phase=e['phase'],state=e['name'],start_ms=round(e['start_us']/1000,3),observed_duration_ms=round(max(0,until-e['start_us'])/1000,3),complete=i+1<len(marks)))
    return dict(events=events,assets=assets,phases=phases,health=health,invalid_json=bad)

def write_csv(path,rows,fields):
    with path.open('w',newline='',encoding='utf-8') as f:
        w=csv.DictWriter(f,fields,extrasaction='ignore');w.writeheader();w.writerows(rows)

def report(data,out):
    out.mkdir(parents=True,exist_ok=True)
    write_csv(out/'assets.csv',data['assets'],['phase','kind','key','name','calls','total_ms','max_ms','bytes','coverage'])
    write_csv(out/'timeline.csv',data['events'],['type','kind','start_us','duration_us','frame','phase','thread','object','bytes','status','count','total_us','max_us','name'])
    write_csv(out/'phases.csv',data['phases'],['phase','state','start_ms','observed_duration_ms','complete'])
    summary={k:v for k,v in data.items() if k!='events'}
    (out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
    rows=['# Observação de desempenho SRR','',f"Registros válidos: {len(data['events'])}. JSON incompletos: {data['invalid_json']}.",'']
    if not data['events']:
        rows+=['Este log não contém a instrumentação OBS. Use a versão 0.0.48 ou posterior.','']
    if data['health']:
        h={k:max(x.get(k,0) for x in data['health']) for k in data['health'][0]}
        rows += [f"Saúde da captura (máximos): {h}",'']
        if h.get('dropped',0) or h.get('aggregate_overflow',0):rows+=['Há perda de cobertura; os totais disponíveis são parciais.','']
    else:rows+=['Sem registro de saúde: não é possível confirmar que a captura está completa.','']
    rows+=['## Estados observados','', '| Fase | Estado | Início (ms) | Permanência observada (ms) |','|---|---|---:|---:|']
    for p in data['phases']:rows.append(f"| {p['phase']} | {p['state']} | {p['start_ms']} | {p['observed_duration_ms']} |")
    rows+=['','## Recursos com maior tempo acumulado','', 'Tempos inclusivos: podem conter GC, I/O e outras chamadas aninhadas, inclusive em threads simultâneas. Não some categorias para calcular tempo total de espera.','', '| Fase | Tipo | Nome | Chamadas | Total (ms) | Máximo (ms) |','|---|---|---|---:|---:|---:|']
    for a in data['assets'][:40]:rows.append(f"| {a['phase']} | {a['kind']} | {a['name'].replace('|','/')} | {a['calls']} | {a['total_ms']} | {a['max_ms']} |")
    rows+=['','## Quadros por janela','', '| Fase | Último quadro | Resumo | Render total (ms) | Pior quadro (ms) |','|---|---:|---|---:|---:|']
    for e in data['events']:
        if e.get('type')=='frames':rows.append(f"| {e['phase']} | {e['frame']} | {e['name']} | {e['total_us']/1000:.2f} | {e['max_us']/1000:.2f} |")
    rows+=['','## Maiores operações individuais','', '| Início (ms) | Fase | Quadro ao concluir | Tipo | Duração (ms) | Nome |','|---:|---|---:|---|---:|---|']
    slow=sorted((e for e in data['events'] if e.get('type')=='span'),key=lambda e:e['duration_us'],reverse=True)
    for e in slow[:30]:rows.append(f"| {e['start_us']/1000:.2f} | {e['phase']} | {e['frame']} | {e['kind']} | {e['duration_us']/1000:.2f} | {e['name'].replace('|','/')} |")
    rows+=['','## Limites','', 'As fases são estados observados a cada 15 quadros; sua duração inclui permanência e interação do usuário. Não é um tempo puro de loading. Pedidos de cena, toques e operações na timeline permitem fazer a correlação.','', 'LoadAsync e UnloadUnusedAssets medem a chamada de entrada; trabalho assíncrono posterior pode ocorrer fora dela. Nomes internos de subassets que não cruzam as entradas instrumentadas não são enumerados. Imagens WWW são identificadas pelo objeto; uploads gráficos pelo identificador da textura. Leituras genéricas usam o handle, que pode ser correlacionado à abertura nomeada na timeline.','', 'P50/P95/P99 são limites superiores de histogramas em passos de 1 ms. O valor 2001 significa 2001 ms ou mais; max_us mantém o máximo medido. A última janela/últimos eventos podem faltar se o processo for encerrado abruptamente. Eventos de rede podem ter fase/quadro de conclusão diferente do início.','']
    (out/'report.md').write_text('\n'.join(rows),encoding='utf-8')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);p.add_argument('--out-dir',type=Path,required=True);a=p.parse_args()
    data=analyze(a.log.read_text(errors='replace'));report(data,a.out_dir)
    print(a.out_dir/'report.md')
    print(f"{len(data['events'])} events, {len(data['assets'])} asset groups, {len(data['phases'])} observed states")
if __name__=='__main__':main()
