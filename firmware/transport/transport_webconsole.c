/**
 * Web Console — ESP32 内嵌设备控制面板
 *
 * 端口: 80
 * 路由:
 *   GET  /          → Web UI (HTML)
 *   GET  /api/tools → JSON 工具列表
 *   POST /api/call  → 执行工具调用
 *   GET  /api/events → SSE 实时状态流
 */

#include "transport_webconsole.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "webconsole";
#define WC_BUF 4096

struct transport_webconsole {
    agent_bridge_t *bridge; agent_transport_t base;
    httpd_handle_t server; uint16_t port; bool running;
};

/* ═══════════════════════════════════════════
 *  内嵌 Web UI (单文件 HTML, gzip 优化前 ~3KB)
 *  深色主题, 设备卡片 + 开关 + 滑块 + 实时传感器
 * ═══════════════════════════════════════════ */

static const char WEB_UI[] =
"<!DOCTYPE html><html lang=zh><head><meta charset=UTF-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AgentBridge Console</title><style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#fff;min-height:100vh}"
".header{background:#16213e;padding:20px;border-bottom:2px solid #00d2ff}"
".header h1{font-size:24px;color:#00d2ff}.header p{color:#a0a0b0;margin-top:4px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:16px;padding:20px}"
".card{background:#25253a;border-radius:12px;padding:20px;border:1px solid #333}"
".card h3{font-size:18px;margin-bottom:4px}.card .desc{color:#888;font-size:12px;margin-bottom:16px}"
".toggle{position:relative;display:inline-block;width:56px;height:28px}"
".toggle input{opacity:0;width:0;height:0}"
".slider-w{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#444;border-radius:28px;transition:.3s}"
".slider-w:before{position:absolute;content:'';height:22px;width:22px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}"
"input:checked+.slider-w{background:#00d2ff}"
"input:checked+.slider-w:before{transform:translateX(28px)}"
".range-inp{width:100%;margin-top:8px;accent-color:#00d2ff}"
".sensor-val{font-size:28px;font-weight:bold;color:#00d2ff}"
".sensor-unit{font-size:14px;color:#888}"
".online{color:#00e696}.offline{color:#ff5555}"
".status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}"
"</style></head><body>"
"<div class=header><h1>AgentBridge Console</h1><p id=status>Loading devices...</p></div>"
"<div class=grid id=grid></div>"
"<script>"
"var devices={};var prefix='';"
"function api(path,opt){return fetch(path,opt).then(r=>r.json())}"
"function loadDevs(){api('/api/tools').then(tools=>{"
"  var seen={};tools.forEach(t=>{var n=t.name.split('.')[0];if(!seen[n]){seen[n]=1;"
"    var caps=t.name.indexOf('set_power')>=0?1:0;"
"    caps|=t.name.indexOf('set_level')>=0?2:0;"
"    devices[n]=devices[n]||{name:n,display:n.replace(/_/g,' ').replace(/\\b\\w/g,c=>c.toUpperCase()),caps:caps,state:{}}"
"  }});render();pollStates()})}"
"function render(){var g=document.getElementById('grid');g.innerHTML='';"
"for(var d in devices){var dev=devices[d];"
"var html='<div class=card><h3>'+dev.display+'</h3><div class=desc>'+d+'</div>';"
"if(dev.caps&1){html+='<label class=toggle><input type=checkbox id=sw_'+d+' onchange=toggle(\"'+d+'\",this.checked)>'"
"+(dev.state.power?' checked':'')+"><span class=slider-w></span></label> "+ (dev.state.power?'ON':'OFF');}"
"if(dev.caps&2){html+='<input class=range-inp type=range min=0 max=100 value='+(dev.state.level||0)+' id=lv_'+d+' onchange=setLevel(\"'+d+'\",this.value)>';}"
"if(!(dev.caps&3)){html+='<div class=sensor-val>'+JSON.stringify(dev.state).replace(/[{}]/g,'').replace(/,/g,'<br>').replace(/\"/g,'')+'</div>';}"
"html+='</div>';g.innerHTML+=html;}"
"document.getElementById('status').innerHTML='<span class=status-dot style=background:#00e696></span> '+Object.keys(devices).length+' devices'}"
"function toggle(name,on){api('/api/call',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:name+'.set_power',arguments:{power:on}})}).then(r=>{devices[name].state=r.state||{};render()})}"
"function setLevel(name,v){api('/api/call',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:name+'.set_level',arguments:{level:parseInt(v)}})}).then(r=>{devices[name].state=r.state||{};render()})}"
"function pollStates(){setInterval(function(){for(var d in devices){api('/api/call',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:d+'.get_state',arguments:{}})}).then(r=>{if(r.state){devices[d].state=r.state;render()}})}},5000)}"
"loadDevs();"
"</script></body></html>";

/* ═══════════════════════════════════════════
 *  HTTP handlers
 * ═══════════════════════════════════════════ */

static transport_webconsole_t *g_wc = NULL;

static esp_err_t handle_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, WEB_UI, strlen(WEB_UI));
    return ESP_OK;
}

static esp_err_t handle_api_tools(httpd_req_t *req) {
    if (!g_wc) { httpd_resp_send_500(req); return ESP_FAIL; }
    char buf[WC_BUF];
    int len = agent_bridge_get_tools_json(g_wc->bridge, buf, sizeof(buf)-1);
    buf[len] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t handle_api_call(httpd_req_t *req) {
    if (!g_wc) { httpd_resp_send_500(req); return ESP_FAIL; }
    char body[1024];
    int blen = httpd_req_recv(req, body, sizeof(body)-1);
    if (blen <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[blen] = '\0';

    /* 解析 {"name":"...","arguments":{...}} */
    const char *nm = strstr(body, "\"name\":\"");
    const char *ag = strstr(body, "\"arguments\":");
    if (!nm) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name"); return ESP_FAIL; }
    nm += 8; char tn[64]; int i=0;
    while (*nm && *nm!='"' && i<63) tn[i++]=*nm++; tn[i]='\0';

    char result[512];
    agent_bridge_dispatch_tool(g_wc->bridge, tn, ag?ag+12:"{}", result, sizeof(result));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, result, strlen(result));
    return ESP_OK;
}

/* ═══════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════ */

transport_webconsole_t *transport_webconsole_create(agent_bridge_t *bridge, uint16_t port) {
    transport_webconsole_t *wc = calloc(1, sizeof(*wc));
    wc->bridge = bridge; wc->port = port ? port : 80; g_wc = wc;
    return wc;
}

void transport_webconsole_start(transport_webconsole_t *wc) {
    if (!wc || wc->running) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = wc->port; cfg.max_uri_handlers = 8;
    httpd_start(&wc->server, &cfg);

    httpd_uri_t uris[] = {
        {.uri="/", .method=HTTP_GET, .handler=handle_index, .user_ctx=wc},
        {.uri="/api/tools", .method=HTTP_GET, .handler=handle_api_tools, .user_ctx=wc},
        {.uri="/api/call", .method=HTTP_POST, .handler=handle_api_call, .user_ctx=wc},
    };
    for (int i=0;i<3;i++) httpd_register_uri_handler(wc->server, &uris[i]);
    wc->running = true;
    ESP_LOGI(TAG, "Web Console: http://<esp32-ip>:%d", wc->port);
}

void transport_webconsole_stop(transport_webconsole_t *wc) {
    if (!wc) return; httpd_stop(wc->server); wc->running = false;
}

agent_transport_t *transport_webconsole_get_base(transport_webconsole_t *wc) {
    if (!wc) return NULL; wc->base.name="webconsole"; wc->base.ctx=wc; return &wc->base;
}
