#pragma once
#include "cJSON.h"
#include <Preferences.h>
#include <atomic>
#include "esp_wifi.h"

// Mirrored into CatCameraRemote/wifi_settings.h. Both boards use this state machine.
enum class WifiPhase { Idle, Prepared, Queued, Connecting, Connected, Failed, Cancelled };
SemaphoreHandle_t wifiSettingsMutex = nullptr;
WifiPhase wifiPhase = WifiPhase::Idle;
String wifiJob, wifiTargetSsid, wifiTargetPassword, wifiPreviousSsid, wifiPreviousPassword;
const char *wifiError = "";
uint32_t wifiPhaseAt = 0;
char wifiBootId[24];
std::atomic<uint32_t> wifiIpEvents{0};
uint32_t wifiAttemptEvent = 0;
// All scan state is protected by wifiSettingsMutex; only loop() runs the radio scan.
enum class WifiScanPhase { Idle, Queued, Running, Ready, Failed };
WifiScanPhase wifiScanPhase = WifiScanPhase::Idle;
uint32_t wifiScanAt = 0, wifiScanSequence = 0;
String wifiScanId, wifiScanNetworks = "[]";
bool wifiScanBusy() { return wifiScanPhase==WifiScanPhase::Queued || wifiScanPhase==WifiScanPhase::Running; }
String wifiScanJson() {
  const char *phase=wifiScanPhase==WifiScanPhase::Queued||wifiScanPhase==WifiScanPhase::Running?"scanning":
    wifiScanPhase==WifiScanPhase::Ready?"ready":wifiScanPhase==WifiScanPhase::Failed?"failed":"idle";
  return "{\"scanId\":\""+wifiScanId+"\",\"phase\":\""+String(phase)+"\",\"networks\":"+wifiScanNetworks+"}";
}
void serviceWifiScan() { // caller holds mutex
  if(wifiScanPhase==WifiScanPhase::Queued) {
    WiFi.scanDelete();wifiScanAt=millis();
    const int16_t started=WiFi.scanNetworks(true,false,false,200);
    wifiScanPhase=started==WIFI_SCAN_FAILED?WifiScanPhase::Failed:WifiScanPhase::Running;
  }
  if(wifiScanPhase!=WifiScanPhase::Running)return;
  const int16_t count=WiFi.scanComplete();
  if(count==WIFI_SCAN_RUNNING) {
    if(millis()-wifiScanAt>15000){esp_wifi_scan_stop();WiFi.scanDelete();wifiScanPhase=WifiScanPhase::Failed;}
    return;
  }
  if(count<0){WiFi.scanDelete();wifiScanPhase=WifiScanPhase::Failed;return;}
  // Keep at most 20 strongest distinct (SSID, open/protected) entries.
  struct Network { String ssid; int32_t rssi; bool open; } networks[20];
  int used=0;
  for(int i=0;i<count;i++) {
    String ssid=WiFi.SSID(i);if(!ssid.length())continue;
    const int32_t rssi=WiFi.RSSI(i);const bool open=WiFi.encryptionType(i)==WIFI_AUTH_OPEN;
    int index=-1;
    for(int j=0;j<used;j++)if(networks[j].ssid==ssid&&networks[j].open==open){index=j;break;}
    if(index>=0){if(rssi<=networks[index].rssi)continue;}
    else if(used<20)index=used++;
    else {index=0;for(int j=1;j<used;j++)if(networks[j].rssi<networks[index].rssi)index=j;if(rssi<=networks[index].rssi)continue;}
    networks[index]={ssid,rssi,open};
  }
  for(int i=0;i<used;i++)for(int j=i+1;j<used;j++)if(networks[j].rssi>networks[i].rssi){Network swap=networks[i];networks[i]=networks[j];networks[j]=swap;}
  cJSON *list=cJSON_CreateArray();
  for(int i=0;i<used;i++) {
    cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"ssid",networks[i].ssid.c_str());
    cJSON_AddNumberToObject(item,"rssi",networks[i].rssi);cJSON_AddBoolToObject(item,"open",networks[i].open);cJSON_AddItemToArray(list,item);
  }
  char *printed=cJSON_PrintUnformatted(list);wifiScanNetworks=printed?printed:"[]";
  wifiScanPhase=printed?WifiScanPhase::Ready:WifiScanPhase::Failed;
  cJSON_free(printed);cJSON_Delete(list);WiFi.scanDelete();
}
const char *wifiPhaseName() {
  switch (wifiPhase) {
    case WifiPhase::Prepared:return "prepared"; case WifiPhase::Queued:return "queued";
    case WifiPhase::Connecting:return "connecting"; case WifiPhase::Connected:return "connected";
    case WifiPhase::Failed:return "failed"; case WifiPhase::Cancelled:return "cancelled";
    default:return "idle";
  }
}
bool wifiBusy() { return wifiPhase==WifiPhase::Prepared || wifiPhase==WifiPhase::Queued || wifiPhase==WifiPhase::Connecting; }
void clearWifiSecrets() { wifiTargetPassword=""; wifiPreviousPassword=""; }
void loadWifiCredentials(String &ssid, String &password) {
  String saved=preferences.getString("network", "");
  cJSON *value=cJSON_Parse(saved.c_str());
  const char *s=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value,"ssid"));
  const char *p=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value,"password"));
  if(s && p){ssid=s;password=p;}
  else {ssid=preferences.getString("ssid","");password=preferences.getString("password","");}
  cJSON_Delete(value);
}
void initializeWifiSettings() {
  wifiSettingsMutex=xSemaphoreCreateMutex();
  WiFi.onEvent([](WiFiEvent_t event) { if(event==ARDUINO_EVENT_WIFI_STA_GOT_IP)wifiIpEvents.fetch_add(1); });
  snprintf(wifiBootId,sizeof(wifiBootId),"%08lx%08lx",static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
  String ssid,password;loadWifiCredentials(ssid,password);
  if(ssid.length())WiFi.begin(ssid.c_str(),password.c_str());
}
bool wifiOriginAllowed(httpd_req_t *req) {
  if(!httpd_req_get_hdr_value_len(req,"Origin"))return true;
  char origin[128],host[100];
  if(httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK)return false;
  if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))==ESP_OK && String(origin)=="http://"+String(host))return true;
#ifdef CAT_REMOTE_CAMERA
  // The controller page can have a new private IP after moving networks.
  String source(origin);
  if(!source.startsWith("http://"))return false;
  source.remove(0,7);int colon=source.indexOf(':');
  if(colon>=0){String port=source.substring(colon+1);if(!port.length())return false;for(size_t i=0;i<port.length();i++)if(!isDigit(port[i]))return false;source=source.substring(0,colon);}
  if(source.endsWith(".local")){for(size_t i=0;i<source.length();i++)if(!isAlphaNumeric(source[i])&&source[i]!='.'&&source[i]!='-')return false;return true;}
  IPAddress ip;if(!ip.fromString(source))return false;
  return ip[0]==10 || (ip[0]==192&&ip[1]==168) || (ip[0]==172&&ip[1]>=16&&ip[1]<=31);
#else
  return false;
#endif
}
void wifiCors(httpd_req_t *req) {
  (void)req;
#ifdef CAT_REMOTE_CAMERA
  if(wifiOriginAllowed(req) && httpd_req_get_hdr_value_len(req,"Origin")) {
    // Copy the origin into session-independent storage is unnecessary: send follows synchronously.
    // All private-network callers share the same read/write policy, no cookie credentials are used.
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
  }
#endif
}
esp_err_t wifiReply(httpd_req_t *req,const char *status,const String &body) {
  wifiCors(req);httpd_resp_set_status(req,status);httpd_resp_set_type(req,"application/json");
  httpd_resp_set_hdr(req,"Cache-Control","no-store");
  return httpd_resp_send(req,body.c_str(),body.length());
}
String wifiStateJson() { // caller holds mutex
  cJSON *value=cJSON_CreateObject();
  cJSON_AddBoolToObject(value,"connected",WiFi.status()==WL_CONNECTED);
  cJSON_AddStringToObject(value,"ssid",WiFi.SSID().c_str());
  cJSON_AddStringToObject(value,"ip",WiFi.localIP().toString().c_str());
  cJSON_AddNumberToObject(value,"rssi",WiFi.status()==WL_CONNECTED?WiFi.RSSI():0);
  cJSON_AddStringToObject(value,"bootId",wifiBootId);
  String deviceId=WiFi.macAddress();deviceId.toLowerCase();
  cJSON_AddStringToObject(value,"deviceId",deviceId.c_str());
  cJSON_AddStringToObject(value,"phase",wifiPhaseName());
  cJSON_AddStringToObject(value,"requestId",wifiJob.c_str());
  cJSON_AddStringToObject(value,"error",wifiError);
  char *printed=cJSON_PrintUnformatted(value);String result=printed?printed:"{}";
  cJSON_free(printed);cJSON_Delete(value);return result;
}
esp_err_t serveWifiStatus(httpd_req_t *req) {
  if(!wifiOriginAllowed(req))return wifiReply(req,"403 Forbidden","{\"error\":\"invalid_origin\"}");
  if(!wifiSettingsMutex)return wifiReply(req,"503 Service Unavailable","{}");
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);String json=wifiStateJson();xSemaphoreGive(wifiSettingsMutex);
  return wifiReply(req,"200 OK",json);
}
esp_err_t serveWifiOptions(httpd_req_t *req) {
  if(!wifiOriginAllowed(req))return wifiReply(req,"403 Forbidden","{}");
  httpd_resp_set_hdr(req,"Access-Control-Allow-Methods","GET, POST, OPTIONS");
  httpd_resp_set_hdr(req,"Access-Control-Allow-Headers","Content-Type, X-Cat-Home");
  return wifiReply(req,"200 OK","{}");
}
esp_err_t serveWifiScan(httpd_req_t *req) {
  if(!wifiOriginAllowed(req))return wifiReply(req,"403 Forbidden","{\"error\":\"invalid_origin\"}");
  if(!wifiSettingsMutex)return wifiReply(req,"503 Service Unavailable","{}");
  if(req->method==HTTP_POST) {
    char marker[8];
    if(req->content_len!=0)return wifiReply(req,"400 Bad Request","{}");
    if(httpd_req_get_hdr_value_str(req,"X-Cat-Home",marker,sizeof(marker))!=ESP_OK||strcmp(marker,"1"))return wifiReply(req,"403 Forbidden","{}");
  }
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);
  const char *status="200 OK";String json;
  if(req->method==HTTP_POST) {
    if(wifiBusy()||WiFi.status()!=WL_CONNECTED){status="409 Conflict";json="{\"error\":\"wifi_busy\"}";}
    else if(!wifiScanBusy()&&(wifiScanPhase==WifiScanPhase::Idle||millis()-wifiScanAt>=10000)) {
      wifiScanId=String(wifiBootId)+"-"+String(++wifiScanSequence);wifiScanNetworks="[]";wifiScanAt=millis();wifiScanPhase=WifiScanPhase::Queued;
    }
  }
  if(!json.length())json=wifiScanJson();
  xSemaphoreGive(wifiSettingsMutex);return wifiReply(req,status,json);
}
bool validWifiJob(const char *id) {
  if(!id || strlen(id)<16 || strlen(id)>64)return false;
  for(const char *p=id;*p;p++)if(!isalnum(static_cast<unsigned char>(*p))&&*p!='-')return false;
  return true;
}
bool validWifiCredentials(const char *ssid,const char *password,bool open) {
  if(!ssid||!password||!strlen(ssid)||strlen(ssid)>32)return false;
  return open ? strlen(password)==0 : strlen(password)>=8&&strlen(password)<=63;
}
esp_err_t serveWifiSet(httpd_req_t *req) {
  char marker[8],type[64];
  const bool allowed=wifiOriginAllowed(req)&&httpd_req_get_hdr_value_str(req,"X-Cat-Home",marker,sizeof(marker))==ESP_OK&&!strcmp(marker,"1")&&
     httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))==ESP_OK&&!strncmp(type,"application/json",16);
  if(req->content_len<=0||req->content_len>768)return wifiReply(req,"400 Bad Request","{\"error\":\"invalid_parameters\"}");
  char raw[769];size_t n=0;
  while(n<req->content_len){int read=httpd_req_recv(req,raw+n,req->content_len-n);if(read<=0){memset(raw,0,sizeof(raw));return ESP_FAIL;}n+=read;}
  // Drain bounded request bodies even on rejection so close() does not reset the
  // connection with unread bytes and truncate the JSON error response.
  raw[n]=0;
  if(!allowed){memset(raw,0,sizeof(raw));return wifiReply(req,"403 Forbidden","{\"error\":\"invalid_request\"}");}
  cJSON *body=cJSON_ParseWithOpts(raw,nullptr,true);memset(raw,0,sizeof(raw));
  const char *action=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body,"action"));
  const char *boot=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body,"bootId"));
  const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body,"requestId"));
  const char *ssid=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body,"ssid"));
  const char *password=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body,"password"));
  const cJSON *open=cJSON_GetObjectItemCaseSensitive(body,"open");
  if(!action||!boot||!validWifiJob(id)||!wifiSettingsMutex){cJSON_Delete(body);return wifiReply(req,"400 Bad Request","{\"error\":\"invalid_parameters\"}");}
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);
  const char *status="200 OK";String reply;
  if(strcmp(boot,wifiBootId)){status="409 Conflict";reply="{\"error\":\"device_restarted\"}";}
  else if(!strcmp(action,"prepare")) {
    if(!cJSON_IsBool(open)||!validWifiCredentials(ssid,password,cJSON_IsTrue(open))){status="400 Bad Request";reply="{\"error\":\"invalid_parameters\"}";}
    else if(wifiScanBusy()){status="409 Conflict";reply="{\"error\":\"scan_busy\"}";}
    else if(wifiBusy()||wifiJob==id){
      if(wifiJob!=id||wifiPhase!=WifiPhase::Prepared||wifiTargetSsid!=ssid||wifiTargetPassword!=password){status="409 Conflict";reply="{\"error\":\"busy_or_replayed\"}";}
    }else {wifiJob=id;wifiTargetSsid=ssid;wifiTargetPassword=password;wifiError="";wifiPhase=WifiPhase::Prepared;wifiPhaseAt=millis();}
  }else if(wifiJob!=id){status="409 Conflict";reply="{\"error\":\"unknown_request\"}";}
  else if(!strcmp(action,"commit")){
    if(wifiPhase==WifiPhase::Prepared){wifiPhase=WifiPhase::Queued;wifiPhaseAt=millis();}
    else if(wifiPhase!=WifiPhase::Queued&&wifiPhase!=WifiPhase::Connecting&&wifiPhase!=WifiPhase::Connected){status="409 Conflict";reply="{\"error\":\"request_expired\"}";}
  }else if(!strcmp(action,"cancel")){
    if(wifiPhase==WifiPhase::Prepared||wifiPhase==WifiPhase::Queued){wifiPhase=WifiPhase::Cancelled;clearWifiSecrets();}
    else if(wifiPhase!=WifiPhase::Cancelled){status="409 Conflict";reply="{\"error\":\"already_started\"}";}
  }else {status="400 Bad Request";reply="{\"error\":\"invalid_action\"}";}
  if(!reply.length())reply=wifiStateJson();
  xSemaphoreGive(wifiSettingsMutex);cJSON_Delete(body);
  return wifiReply(req,status,reply);
}
void registerWifiRoutes(httpd_handle_t server) {
  httpd_uri_t route={};route.uri="/api/wifi";route.method=HTTP_GET;route.handler=serveWifiStatus;httpd_register_uri_handler(server,&route);
  route.method=HTTP_POST;route.handler=serveWifiSet;httpd_register_uri_handler(server,&route);
  route.method=HTTP_OPTIONS;route.handler=serveWifiOptions;httpd_register_uri_handler(server,&route);
  route.uri="/api/wifi/scan";route.method=HTTP_POST;route.handler=serveWifiScan;httpd_register_uri_handler(server,&route);
  route.method=HTTP_GET;httpd_register_uri_handler(server,&route);
  route.method=HTTP_OPTIONS;route.handler=serveWifiOptions;httpd_register_uri_handler(server,&route);
}
void serviceWifiSettings() {
  if(!wifiSettingsMutex)return;
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);
  serviceWifiScan();
  const uint32_t elapsed=millis()-wifiPhaseAt;
  if(wifiPhase==WifiPhase::Prepared&&elapsed>60000){wifiPhase=WifiPhase::Cancelled;wifiError="expired";clearWifiSecrets();}
  if(wifiPhase==WifiPhase::Queued&&elapsed>=8000){
    loadWifiCredentials(wifiPreviousSsid,wifiPreviousPassword);
    wifiAttemptEvent=wifiIpEvents.load();
    // Wait for the old association to end before starting another one. Retry
    // transient association failures within the bounded 25-second attempt.
    WiFi.setAutoReconnect(false);WiFi.disconnect(false,false,1000);
    WiFi.begin(wifiTargetSsid.c_str(),wifiTargetPassword.c_str());WiFi.setAutoReconnect(true);
    wifiPhase=WifiPhase::Connecting;wifiPhaseAt=millis();
  }else if(wifiPhase==WifiPhase::Connecting){
    const bool connected=wifiIpEvents.load()!=wifiAttemptEvent&&WiFi.status()==WL_CONNECTED&&WiFi.SSID()==wifiTargetSsid&&uint32_t(WiFi.localIP())!=0;
    if(connected){
      cJSON *network=cJSON_CreateObject();cJSON_AddStringToObject(network,"ssid",wifiTargetSsid.c_str());cJSON_AddStringToObject(network,"password",wifiTargetPassword.c_str());
      char *data=cJSON_PrintUnformatted(network);
      const bool saved=data&&preferences.putString("network",data)==strlen(data);
      cJSON_free(data);cJSON_Delete(network);
      if(saved){wifiPhase=WifiPhase::Connected;wifiError="";WiFi.setAutoReconnect(true);clearWifiSecrets();}
      else wifiError="save_failed";
    }
    if(wifiPhase==WifiPhase::Connecting&&(millis()-wifiPhaseAt>=25000||!strcmp(wifiError,"save_failed"))){
      if(!strlen(wifiError))wifiError="connection_failed";
      WiFi.setAutoReconnect(false);WiFi.disconnect(false,false,1000);
      if(wifiPreviousSsid.length())WiFi.begin(wifiPreviousSsid.c_str(),wifiPreviousPassword.c_str());
      WiFi.setAutoReconnect(true);wifiPhase=WifiPhase::Failed;clearWifiSecrets();
    }
  }
  xSemaphoreGive(wifiSettingsMutex);
}
bool submitSerialWifi(const String &ssid,const String &password) {
  if(!wifiSettingsMutex)return false;
  xSemaphoreTake(wifiSettingsMutex,portMAX_DELAY);
  if(wifiBusy()||wifiScanBusy()){xSemaphoreGive(wifiSettingsMutex);return false;}
  wifiJob="serial-"+String(millis());wifiTargetSsid=ssid;wifiTargetPassword=password;wifiPhase=WifiPhase::Queued;wifiPhaseAt=millis()-8000;wifiError="";
  xSemaphoreGive(wifiSettingsMutex);return true;
}
