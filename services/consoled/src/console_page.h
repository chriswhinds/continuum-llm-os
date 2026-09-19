/* console_page.h -- the operator console's entire frontend (ARCH-001
 * §10), embedded as a string constant and compiled into consoled
 * rather than read from disk at runtime, so the binary has no asset
 * directory dependency. Vanilla JS polling GET /api/nodes every second --
 * see main.c's module comment for why this reference build polls instead
 * of pushing over a WebSocket. */
#ifndef CONSOLED_CONSOLE_PAGE_H
#define CONSOLED_CONSOLE_PAGE_H

static const char CONSOLE_INDEX_HTML[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<title>Continuum Console</title>"
"<style>"
"body{background:#0e1512;color:#dce7de;font-family:ui-monospace,Consolas,monospace;margin:0;padding:24px;}"
"h1{font-size:15px;letter-spacing:.08em;text-transform:uppercase;color:#8ca192;font-weight:600;margin:0 0 4px;}"
".sub{color:#5c6e63;font-size:12px;margin-bottom:20px;}"
"table{border-collapse:collapse;width:100%;max-width:920px;}"
"th{text-align:left;font-size:10px;letter-spacing:.08em;text-transform:uppercase;color:#6e8477;"
"   border-bottom:1px solid #223028;padding:6px 10px;}"
"td{padding:7px 10px;font-size:12.5px;border-bottom:1px solid #1b2820;}"
".bar-track{height:7px;width:120px;background:#1b2820;border-radius:4px;overflow:hidden;display:inline-block;vertical-align:middle;}"
".bar-fill{height:100%;background:#42d2c4;}"
".bar-fill.warn{background:#e3a14a;}"
".bar-fill.crit{background:#e4735a;}"
".stale{color:#5c6e63;}"
".empty{color:#5c6e63;padding:20px 10px;font-size:12.5px;}"
"</style></head><body>"
"<h1>Continuum &middot; Operator Console</h1>"
"<div class=\"sub\" id=\"status\">connecting...</div>"
"<table><thead><tr><th>Node</th><th>CPU</th><th>DRAM</th><th>Swap I/O</th><th>Last seen</th></tr></thead>"
"<tbody id=\"rows\"></tbody></table>"
"<script>"
"function fmtBytes(n){if(n>1e9)return (n/1e9).toFixed(1)+'GB';if(n>1e6)return (n/1e6).toFixed(1)+'MB';return (n/1e3).toFixed(0)+'KB';}"
"function bar(pct,cls){pct=Math.max(0,Math.min(100,pct));"
" return '<span class=\"bar-track\"><span class=\"bar-fill '+cls+'\" style=\"width:'+pct+'%\"></span></span> '"
" +pct.toFixed(0)+'%';}"
"function cls(pct){return pct>90?'crit':(pct>70?'warn':'');}"
"async function tick(){"
" try{"
"   const res=await fetch('/api/nodes');"
"   const nodes=await res.json();"
"   document.getElementById('status').textContent=nodes.length+' node(s) reporting · refreshed just now';"
"   const rows=document.getElementById('rows');"
"   if(nodes.length===0){rows.innerHTML='<tr><td colspan=5 class=\"empty\">no node-agentd telemetry received yet</td></tr>';return;}"
"   nodes.sort((a,b)=>a.node_id-b.node_id);"
"   rows.innerHTML=nodes.map(n=>{"
"     const dram_pct=n.dram_total_bytes>0?100*n.dram_used_bytes/n.dram_total_bytes:0;"
"     const age=Math.max(0,(Date.now()-n.last_seen_ms)/1000);"
"     const staleCls=age>10?'stale':'';"
"     return '<tr class=\"'+staleCls+'\"><td>node-'+n.node_id+'</td>'"
"       +'<td>'+bar(n.cpu_load_pct,cls(n.cpu_load_pct))+'</td>'"
"       +'<td>'+bar(dram_pct,cls(dram_pct))+' <span style=\"color:#6e8477\">'+fmtBytes(n.dram_used_bytes)+' / '+fmtBytes(n.dram_total_bytes)+'</span></td>'"
"       +'<td>'+fmtBytes(n.swap_io_bytes_per_sec)+'/s</td>'"
"       +'<td>'+age.toFixed(0)+'s ago</td></tr>';"
"   }).join('');"
" }catch(e){document.getElementById('status').textContent='connection lost, retrying...';}"
"}"
"tick();setInterval(tick,1000);"
"</script></body></html>";

#endif
