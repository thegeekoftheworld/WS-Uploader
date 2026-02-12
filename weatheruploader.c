/* weatheruploader.c */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <curl/curl.h>
#include <mosquitto.h>

static int g_debug = 0;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while (0)

/* ---- Full source below (same as earlier, but fixed strncasecmp + indentation + snprintf("")) ---- */
/* NOTE: This header comment is kept small here; your build/run instructions are in the MD guide. */

/* ----------------------------- jsmn (tiny JSON parser) ----------------------------- */
/* jsmn is MIT licensed: https://github.com/zserge/jsmn */
typedef enum { JSMN_UNDEFINED=0, JSMN_OBJECT=1, JSMN_ARRAY=2, JSMN_STRING=3, JSMN_PRIMITIVE=4 } jsmntype_t;
typedef struct { jsmntype_t type; int start; int end; int size; int parent; } jsmntok_t;
typedef struct { unsigned int pos; unsigned int toknext; int toksuper; } jsmn_parser;
static void jsmn_init(jsmn_parser *parser){ parser->pos=0; parser->toknext=0; parser->toksuper=-1; }
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens){
  if (parser->toknext>=num_tokens) return NULL;
  jsmntok_t *tok=&tokens[parser->toknext++]; tok->start=tok->end=-1; tok->size=0; tok->parent=-1; tok->type=JSMN_UNDEFINED; return tok;
}
static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end){ token->type=type; token->start=start; token->end=end; token->size=0; }
static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens){
  int start=(int)parser->pos;
  for (; parser->pos<len; parser->pos++){
    char c=js[parser->pos];
    if (c=='\t'||c=='\r'||c=='\n'||c==' '||c==','||c==']'||c=='}') break;
    if (c<32) return -1;
  }
  jsmntok_t *tok=jsmn_alloc_token(parser,tokens,num_tokens); if(!tok) return -2;
  jsmn_fill_token(tok,JSMN_PRIMITIVE,start,(int)parser->pos); tok->parent=parser->toksuper; parser->pos--; return 0;
}
static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens){
  int start=(int)parser->pos; parser->pos++;
  for (; parser->pos<len; parser->pos++){
    char c=js[parser->pos];
    if (c=='"'){ jsmntok_t *tok=jsmn_alloc_token(parser,tokens,num_tokens); if(!tok) return -2;
      jsmn_fill_token(tok,JSMN_STRING,start+1,(int)parser->pos); tok->parent=parser->toksuper; return 0; }
    if (c=='\\') parser->pos++;
  }
  return -1;
}
static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, unsigned int num_tokens){
  int r;
  for (; parser->pos<len; parser->pos++){
    char c=js[parser->pos];
    switch(c){
      case '{': case '[':{
        jsmntok_t *tok=jsmn_alloc_token(parser,tokens,num_tokens); if(!tok) return -2;
        tok->type=(c=='{')?JSMN_OBJECT:JSMN_ARRAY; tok->start=(int)parser->pos; tok->parent=parser->toksuper;
        if (parser->toksuper!=-1) tokens[parser->toksuper].size++; parser->toksuper=(int)parser->toknext-1; break;}
      case '}': case ']':{
        jsmntype_t type=(c=='}')?JSMN_OBJECT:JSMN_ARRAY; int i=(int)parser->toknext-1;
        for(; i>=0; i--){
          if(tokens[i].start!=-1 && tokens[i].end==-1){
            if(tokens[i].type!=type) return -1;
            tokens[i].end=(int)parser->pos+1; parser->toksuper=tokens[i].parent; break;
          }
        }
        if(i==-1) return -1; break;}
      case '"':
        r=jsmn_parse_string(parser,js,len,tokens,num_tokens); if(r<0) return r;
        if(parser->toksuper!=-1) tokens[parser->toksuper].size++; break;
      case '\t': case '\r': case '\n': case ' ': case ':': case ',': break;
      default:
        r=jsmn_parse_primitive(parser,js,len,tokens,num_tokens); if(r<0) return r;
        if(parser->toksuper!=-1) tokens[parser->toksuper].size++; break;
    }
  }
  for(int i=(int)parser->toknext-1; i>=0; i--) if(tokens[i].start!=-1 && tokens[i].end==-1) return -1;
  return (int)parser->toknext;
}

/* ----------------------------- Utilities ----------------------------- */
#define MAX_STR 512
#define MAX_TEMPLATE 4096
#define MAX_UPLOADS 16
#define MAX_HEADERS 16
#define MAX_TOKENS 4096

static void msleep(int ms){ struct timespec ts; ts.tv_sec=ms/1000; ts.tv_nsec=(long)(ms%1000)*1000000L; nanosleep(&ts,NULL); }
static char *trim(char *s){ while(*s && isspace((unsigned char)*s)) s++; if(!*s) return s; char *e=s+strlen(s)-1; while(e>s && isspace((unsigned char)*e)) *e--='\0'; return s; }
static int str_ieq(const char *a,const char *b){ for(; *a && *b; a++,b++) if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0; return *a=='\0'&&*b=='\0'; }
static void mac_normalize(const char *in,char *out,size_t outsz){ size_t j=0; for(size_t i=0; in[i] && j+1<outsz; i++){ char c=in[i]; if(c==':'||c=='-'||c=='.') continue; out[j++]=(char)tolower((unsigned char)c);} out[j]='\0'; }
static void utc_now(char *out,size_t outsz){ time_t now=time(NULL); struct tm tmz; gmtime_r(&now,&tmz); snprintf(out,outsz,"%04d-%02d-%02d %02d:%02d:%02d",tmz.tm_year+1900,tmz.tm_mon+1,tmz.tm_mday,tmz.tm_hour,tmz.tm_min,tmz.tm_sec); }
static long epoch_now(void){ return (long)time(NULL); }
static char *url_encode(CURL *curl,const char *s){ if(!s) return NULL; return curl_easy_escape(curl,s,0); }

/* ----------------------------- HTTP ----------------------------- */
typedef struct { char *data; size_t size; } MemBuf;
static size_t write_cb(void *contents,size_t size,size_t nmemb,void *userp){
  size_t realsize=size*nmemb; MemBuf *mem=(MemBuf*)userp;
  char *ptr=realloc(mem->data, mem->size+realsize+1); if(!ptr) return 0;
  mem->data=ptr; memcpy(&(mem->data[mem->size]), contents, realsize); mem->size+=realsize; mem->data[mem->size]='\0'; return realsize;
}
static int http_get(CURL *curl,const char *url,long timeout_ms,MemBuf *out){
  out->data=NULL; out->size=0;
  DBG("HTTP GET %s (timeout=%ldms)", url, timeout_ms);
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPGET,1L);
  curl_easy_setopt(curl,CURLOPT_POST,0L);
  curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,timeout_ms);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,timeout_ms);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"weatheruploader/3.1");
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
  char errbuf[CURL_ERROR_SIZE]; errbuf[0]='\0';
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  CURLcode res=curl_easy_perform(curl);
  long code=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
  if(res!=CURLE_OK){
    DBG("HTTP GET curl error: %s", errbuf[0]?errbuf:curl_easy_strerror(res));
    return 1;
  }
  DBG("HTTP GET status=%ld bytes=%zu", code, out->size);
  if(code<200 || code>=300) return 1;
  return 0;
}
static int http_post(CURL *curl,const char *url,const char *body,long timeout_ms,const char headers[][MAX_STR],int headers_n,MemBuf *out){
  out->data=NULL; out->size=0;
  DBG("HTTP POST %s (timeout=%ldms, headers=%d, body_bytes=%zu)", url, timeout_ms, headers_n, body?strlen(body):0UL);
  struct curl_slist *hdrs=NULL;
  for(int i=0;i<headers_n;i++) if(headers[i][0]) hdrs=curl_slist_append(hdrs, headers[i]);

  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_HTTPGET,0L);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS, body?body:"");
  curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,timeout_ms);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,timeout_ms);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"weatheruploader/3.1");
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
  if(hdrs) curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdrs);

  char errbuf[CURL_ERROR_SIZE]; errbuf[0]='\0';
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  CURLcode res=curl_easy_perform(curl);
  long code=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
  if(hdrs) curl_slist_free_all(hdrs);

  if(res!=CURLE_OK){
    DBG("HTTP POST curl error: %s", errbuf[0]?errbuf:curl_easy_strerror(res));
    return 1;
  }
  DBG("HTTP POST status=%ld bytes=%zu", code, out->size);
  if(code<200 || code>=300) return 1;
  return 0;
}

/* ----------------------------- INI Config ----------------------------- */
typedef struct{
  int enabled; char type[16];
  char base_url[MAX_STR]; char id[MAX_STR]; char password[MAX_STR]; int realtime; int rtfreq;
  char method[8]; char template_url[MAX_TEMPLATE]; char url[MAX_STR]; char post_format[8];
  char body_template[MAX_TEMPLATE];
  char headers[MAX_HEADERS][MAX_STR]; int headers_n;
  long timeout_ms;
} UploadTarget;

typedef struct{
  char device_name[MAX_STR]; int expire_after;
  char base_ip[MAX_STR]; int poll_interval_seconds; int refresh_delay_ms;
  int mqtt_enabled; char mqtt_host[MAX_STR]; int mqtt_port; char mqtt_user[MAX_STR]; char mqtt_pass[MAX_STR]; char ha_discovery_base[MAX_STR];
  double lat,lon; double elev_ft; double elev_m;
  int cwop_enabled; char cwop_server[MAX_STR]; int cwop_port; char cwop_station[MAX_STR]; char cwop_pass[MAX_STR];
  int cwop_interval_seconds; time_t cwop_last_sent;
  UploadTarget uploads[MAX_UPLOADS]; int uploads_n;
} Config;

static void set_defaults(Config *cfg){
  memset(cfg,0,sizeof(*cfg));
  snprintf(cfg->device_name,sizeof(cfg->device_name),"Weather Station");
  cfg->expire_after=60;
  cfg->base_ip[0]='\0';
  cfg->poll_interval_seconds=20;
  cfg->refresh_delay_ms=250;
  cfg->mqtt_enabled=1;
  snprintf(cfg->mqtt_host,sizeof(cfg->mqtt_host),"127.0.0.1");
  cfg->mqtt_port=1883;
  snprintf(cfg->ha_discovery_base,sizeof(cfg->ha_discovery_base),"homeassistant");
  cfg->cwop_enabled=0;
  snprintf(cfg->cwop_server,sizeof(cfg->cwop_server),"cwop.aprs.net");
  cfg->cwop_port=14580;
  cfg->cwop_interval_seconds=120;
  cfg->cwop_last_sent=0;
  cfg->uploads_n=0;
}

static UploadTarget *get_upload(Config *cfg,int idx1){
  if(idx1<1||idx1>MAX_UPLOADS) return NULL;
  int i=idx1-1;
  if(i>=cfg->uploads_n) cfg->uploads_n=i+1;
  UploadTarget *u=&cfg->uploads[i];
  if(u->type[0]=='\0'){
    u->enabled=0;
    snprintf(u->type,sizeof(u->type),"custom");
    snprintf(u->method,sizeof(u->method),"GET");
    u->timeout_ms=6000;
  }
  return u;
}

static int load_ini(const char *path, Config *cfg){
  FILE *f=fopen(path,"r"); if(!f) return 1;
  char line[4096]; char section[64]="";
  while(fgets(line,sizeof(line),f)){
    char *s=trim(line);
    if(*s=='\0'||*s==';'||*s=='#') continue;
    if(*s=='['){
      char *r=strchr(s,']'); if(!r) continue; *r='\0'; snprintf(section,sizeof(section),"%s",s+1); continue;
    }
    char *eq=strchr(s,'='); if(!eq) continue; *eq='\0';
    char *key=trim(s); char *val=trim(eq+1);
    if(str_ieq(section,"device")){
      if(str_ieq(key,"name")) snprintf(cfg->device_name,sizeof(cfg->device_name),"%s",val);
      else if(str_ieq(key,"expire_after")) cfg->expire_after=atoi(val);
    } else if(str_ieq(section,"poll")){
      if(str_ieq(key,"base_ip")) snprintf(cfg->base_ip,sizeof(cfg->base_ip),"%s",val);
      else if(str_ieq(key,"interval_seconds")) cfg->poll_interval_seconds=atoi(val);
      else if(str_ieq(key,"refresh_delay_ms")) cfg->refresh_delay_ms=atoi(val);
    } else if(str_ieq(section,"mqtt")){
      if(str_ieq(key,"enabled")) cfg->mqtt_enabled=atoi(val);
      else if(str_ieq(key,"host")) snprintf(cfg->mqtt_host,sizeof(cfg->mqtt_host),"%s",val);
      else if(str_ieq(key,"port")) cfg->mqtt_port=atoi(val);
      else if(str_ieq(key,"username")) snprintf(cfg->mqtt_user,sizeof(cfg->mqtt_user),"%s",val);
      else if(str_ieq(key,"password")) snprintf(cfg->mqtt_pass,sizeof(cfg->mqtt_pass),"%s",val);
      else if(str_ieq(key,"discovery_base")) snprintf(cfg->ha_discovery_base,sizeof(cfg->ha_discovery_base),"%s",val);
    } else if(str_ieq(section,"location")){
      if(str_ieq(key,"lat")) cfg->lat=atof(val);
      else if(str_ieq(key,"lon")) cfg->lon=atof(val);
      else if(str_ieq(key,"elev_ft")) cfg->elev_ft=atof(val);
    } else if(str_ieq(section,"cwop")){
      if(str_ieq(key,"enabled")) cfg->cwop_enabled=atoi(val);
      else if(str_ieq(key,"server")) snprintf(cfg->cwop_server,sizeof(cfg->cwop_server),"%s",val);
      else if(str_ieq(key,"port")) cfg->cwop_port=atoi(val);
      else if(str_ieq(key,"station")) snprintf(cfg->cwop_station,sizeof(cfg->cwop_station),"%s",val);
      else if(str_ieq(key,"passcode")) snprintf(cfg->cwop_pass,sizeof(cfg->cwop_pass),"%s",val);
      else if(str_ieq(key,"interval_seconds")) cfg->cwop_interval_seconds=atoi(val);
    } else if(strncasecmp(section,"upload",(size_t)6)==0){
      int idx=atoi(section+6); UploadTarget *u=get_upload(cfg,idx); if(!u) continue;
      if(str_ieq(key,"enabled")) u->enabled=atoi(val);
      else if(str_ieq(key,"type")) snprintf(u->type,sizeof(u->type),"%s",val);
      else if(str_ieq(key,"timeout_ms")) u->timeout_ms=atol(val);
      else if(str_ieq(key,"base_url")) snprintf(u->base_url,sizeof(u->base_url),"%s",val);
      else if(str_ieq(key,"id")) snprintf(u->id,sizeof(u->id),"%s",val);
      else if(str_ieq(key,"password")) snprintf(u->password,sizeof(u->password),"%s",val);
      else if(str_ieq(key,"realtime")) u->realtime=atoi(val);
      else if(str_ieq(key,"rtfreq")) u->rtfreq=atoi(val);
      else if(str_ieq(key,"method")) snprintf(u->method,sizeof(u->method),"%s",val);
      else if(str_ieq(key,"template")) snprintf(u->template_url,sizeof(u->template_url),"%s",val);
      else if(str_ieq(key,"url")) snprintf(u->url,sizeof(u->url),"%s",val);
      else if(str_ieq(key,"post_format")) snprintf(u->post_format,sizeof(u->post_format),"%s",val);
      else if(str_ieq(key,"body_template")) snprintf(u->body_template,sizeof(u->body_template),"%s",val);
      else if(strncasecmp(key,"header",(size_t)6)==0){
        if(u->headers_n<MAX_HEADERS){ snprintf(u->headers[u->headers_n],sizeof(u->headers[u->headers_n]),"%s",val); u->headers_n++; }
      }
    }
  }
  fclose(f);
  cfg->elev_m=cfg->elev_ft*0.3048;
  return 0;
}

/* ----------------------------- Data model + math ----------------------------- */
typedef struct{
  char ip[MAX_STR]; int rssi; char mac[MAX_STR]; char mac_id[MAX_STR];
  double indoortempf, indoorhumidity, tempf, humidity, absbaromin, baromin;
  double windspeedmph, windgustmph, winddir, windspdmph_avg2m, winddir_avg2m, windspdmph_avg10m, winddir_avg10m;
  double rainrate_inhr, rain_hour_in, dailyrainin, weeklyrainin, monthlyrainin, yearlyrainin, totalrainin;
  double solarradiation, uv;
  char battery_status[MAX_STR];
  double dewptf, windchillf;
  int has_data;
} WeatherData;

static void wd_init(WeatherData *wd){
  memset(wd,0,sizeof(*wd));
  wd->indoortempf=wd->indoorhumidity=NAN;
  wd->tempf=wd->humidity=NAN;
  wd->absbaromin=wd->baromin=NAN;
  wd->windspeedmph=wd->windgustmph=wd->winddir=NAN;
  wd->windspdmph_avg2m=wd->winddir_avg2m=NAN;
  wd->windspdmph_avg10m=wd->winddir_avg10m=NAN;
  wd->rainrate_inhr=wd->rain_hour_in=wd->dailyrainin=NAN;
  wd->weeklyrainin=wd->monthlyrainin=wd->yearlyrainin=wd->totalrainin=NAN;
  wd->solarradiation=wd->uv=NAN;
  wd->dewptf=wd->windchillf=NAN;
  wd->battery_status[0]='\0';
  wd->has_data=0;
}
static double dewpoint_f(double tf,double rh){
  if(isnan(tf)||isnan(rh)||rh<=0.0) return NAN;
  double tc=(tf-32.0)*(5.0/9.0);
  double a=17.27,b=237.7;
  double gamma=(a*tc)/(b+tc)+log(rh/100.0);
  double dpc=(b*gamma)/(a-gamma);
  return dpc*(9.0/5.0)+32.0;
}
static double windchill_f(double tf,double wind_mph){
  if(isnan(tf)||isnan(wind_mph)) return NAN;
  if(tf>50.0||wind_mph<=3.0) return tf;
  return 35.74+0.6215*tf-35.75*pow(wind_mph,0.16)+0.4275*tf*pow(wind_mph,0.16);
}

/* JSON helpers */
static int tok_eq(const char *js,const jsmntok_t *t,const char *s){ int len=t->end-t->start; return (t->type==JSMN_STRING && (int)strlen(s)==len && strncmp(js+t->start,s,(size_t)len)==0); }
static void tok_to_str(const char *js,const jsmntok_t *t,char *out,size_t outsz){
  int len=t->end-t->start; if(len<0) len=0; if((size_t)len>=outsz) len=(int)outsz-1;
  memcpy(out,js+t->start,(size_t)len); out[len]='\0';
}
/* Find value token index for key inside object token */
static int obj_get(const char *js, jsmntok_t *toks, int obj_idx, const char *key){
  if(toks[obj_idx].type!=JSMN_OBJECT) return -1;
  int count=toks[obj_idx].size;
  int i=obj_idx+1;
  for(int k=0;k<count;k++){
    if(tok_eq(js,&toks[i],key)) return i+1;
    i++; int v=i; int end=toks[v].end; i++;
    while(i<MAX_TOKENS && toks[i].start!=-1 && toks[i].start<end) i++;
  }
  return -1;
}

static int parse_connect_status(const char *json, WeatherData *wd){
  jsmn_parser p; jsmntok_t toks[MAX_TOKENS]; jsmn_init(&p);
  int n=jsmn_parse(&p,json,strlen(json),toks,MAX_TOKENS); if(n<0) return 1;
  int ipcfg=obj_get(json,toks,0,"IPConfig"); if(ipcfg<0) return 1;
  int ip=obj_get(json,toks,ipcfg,"ip");
  int rssi=obj_get(json,toks,ipcfg,"rssi");
  int mac=obj_get(json,toks,ipcfg,"mac");
  if(ip>=0) tok_to_str(json,&toks[ip],wd->ip,sizeof(wd->ip));
  if(rssi>=0){ char buf[64]; tok_to_str(json,&toks[rssi],buf,sizeof(buf)); wd->rssi=atoi(buf); }
  if(mac>=0){ tok_to_str(json,&toks[mac],wd->mac,sizeof(wd->mac)); mac_normalize(wd->mac,wd->mac_id,sizeof(wd->mac_id)); }
  return 0;
}

static void set_field_by_title_name(WeatherData *wd,const char *title,const char *name,const char *val){
  double d=atof(val);
  if(str_ieq(title,"Indoor")){
    if(str_ieq(name,"Temperature")) wd->indoortempf=d;
    else if(str_ieq(name,"Humidity")) wd->indoorhumidity=d;
  } else if(str_ieq(title,"Outdoor")){
    if(str_ieq(name,"Temperature")) wd->tempf=d;
    else if(str_ieq(name,"Humidity")) wd->humidity=d;
  } else if(str_ieq(title,"Pressure")){
    if(str_ieq(name,"Absolute")) wd->absbaromin=d;
    else if(str_ieq(name,"Relative")) wd->baromin=d;
  } else if(str_ieq(title,"Wind Speed")){
    if(str_ieq(name,"Wind")) wd->windspeedmph=d;
    else if(str_ieq(name,"Gust")) wd->windgustmph=d;
    else if(str_ieq(name,"Direction")) wd->winddir=d;
    else if(str_ieq(name,"Wind Average 2 Minute")) wd->windspdmph_avg2m=d;
    else if(str_ieq(name,"Direction Average 2 Minute")) wd->winddir_avg2m=d;
    else if(str_ieq(name,"Wind Average 10 Minute")) wd->windspdmph_avg10m=d;
    else if(str_ieq(name,"Direction Average 10 Minute")) wd->winddir_avg10m=d;
  } else if(str_ieq(title,"Rainfall")){
    if(str_ieq(name,"Rate")) wd->rainrate_inhr=d;
    else if(str_ieq(name,"Hour")) wd->rain_hour_in=d;
    else if(str_ieq(name,"Day")) wd->dailyrainin=d;
    else if(str_ieq(name,"Week")) wd->weeklyrainin=d;
    else if(str_ieq(name,"Month")) wd->monthlyrainin=d;
    else if(str_ieq(name,"Year")) wd->yearlyrainin=d;
    else if(str_ieq(name,"Total")) wd->totalrainin=d;
  } else if(str_ieq(title,"Solar")){
    if(str_ieq(name,"Light")) wd->solarradiation=d;
    else if(str_ieq(name,"UVI")) wd->uv=d;
  }
}

static int parse_record(const char *json, WeatherData *wd){
  jsmn_parser p; jsmntok_t toks[MAX_TOKENS]; jsmn_init(&p);
  int n=jsmn_parse(&p,json,strlen(json),toks,MAX_TOKENS); if(n<0) return 1;
  int sensor=obj_get(json,toks,0,"Sensor"); if(sensor<0) return 1;
  if(toks[sensor].type!=JSMN_ARRAY) return 1;

  int idx=sensor+1;
  int arr_end=toks[sensor].end;

  while(idx<n && toks[idx].start<arr_end){
    if(toks[idx].type==JSMN_OBJECT){
      int title_tok=obj_get(json,toks,idx,"title");
      int list_tok=obj_get(json,toks,idx,"list");
      char title[128]="";
      if(title_tok>=0) tok_to_str(json,&toks[title_tok],title,sizeof(title));

      if(list_tok>=0 && toks[list_tok].type==JSMN_ARRAY){
        int li=list_tok+1;
        int list_end=toks[list_tok].end;
        while(li<n && toks[li].start<list_end){
          if(toks[li].type==JSMN_ARRAY){
            int elem=li+1;
            if(elem<n && toks[elem].type==JSMN_STRING){
              char name[128]=""; tok_to_str(json,&toks[elem],name,sizeof(name));
              if(elem+1<n){
                char val[128]=""; tok_to_str(json,&toks[elem+1],val,sizeof(val));
                for(char *p2=val; *p2; p2++) if((unsigned char)*p2==0xC2) *p2=' ';
                set_field_by_title_name(wd,title,name,val);
              }
            }
          }
          int end=toks[li].end; li++;
          while(li<n && toks[li].start!=-1 && toks[li].start<end) li++;
        }
      }
    }
    int end=toks[idx].end; idx++;
    while(idx<n && toks[idx].start!=-1 && toks[idx].start<end) idx++;
  }

  int battery=obj_get(json,toks,0,"battery");
  if(battery>=0 && toks[battery].type==JSMN_OBJECT){
    int list=obj_get(json,toks,battery,"list");
    if(list>=0 && toks[list].type==JSMN_ARRAY){
      int first=list+1;
      if(first<n && toks[first].type==JSMN_STRING) tok_to_str(json,&toks[first],wd->battery_status,sizeof(wd->battery_status));
    }
  }

  wd->dewptf=dewpoint_f(wd->tempf,wd->humidity);
  wd->windchillf=windchill_f(wd->tempf,wd->windspeedmph);
  wd->has_data=1;
  return 0;
}

/* ----------------------------- MQTT / Home Assistant ----------------------------- */
static void mqtt_publish_str(struct mosquitto *m,const char *topic,const char *payload,int retain){
  if(!m||!topic||!payload) return;
  mosquitto_publish(m,NULL,topic,(int)strlen(payload),payload,0,retain);
}
static void mqtt_publish_num(struct mosquitto *m,const char *topic,double v,int retain){
  if(!m||!topic) return;
  char buf[64];
  if(isnan(v)) snprintf(buf,sizeof(buf),"null");
  else snprintf(buf,sizeof(buf),"%.3f",v);
  mosquitto_publish(m,NULL,topic,(int)strlen(buf),buf,0,retain);
}
/*
 * Publish a single Home Assistant MQTT discovery sensor.
 *
 * device_id: stable per-station identifier (e.g. wx_<mac>), used for HA topic paths + uniq IDs
 * object_id: sensor key (e.g. outdoor_tempf)
 */
static void ha_discovery_sensor(struct mosquitto *m,const Config *cfg,
                                const char *device_id,
                                const char *object_id,const char *name,const char *state_topic,
                                const char *unit,const char *device_class,int expire_after){
  if(!m||!cfg||!device_id||!device_id[0]||!object_id||!name||!state_topic) return;

  char cfg_topic[256];
  snprintf(cfg_topic,sizeof(cfg_topic),"%s/sensor/%s/%s/config",
           cfg->ha_discovery_base[0]?cfg->ha_discovery_base:"homeassistant",
           device_id, object_id);

  char uniq[192];
  snprintf(uniq,sizeof(uniq),"%s_%s", device_id, object_id);

  /* Build JSON config payload */
  char payload[1024];
  int off = 0;
  off += snprintf(payload+off, sizeof(payload)-off,
    "{"
      "\"name\":\"%s\","
      "\"uniq_id\":\"%s\","
      "\"stat_t\":\"%s\"",
    name, uniq, state_topic);

  if(unit && unit[0]){
    off += snprintf(payload+off, sizeof(payload)-off, ",\"unit_of_meas\":\"%s\"", unit);
  }
  if(device_class && device_class[0]){
    off += snprintf(payload+off, sizeof(payload)-off, ",\"dev_cla\":\"%s\"", device_class);
  }
  if(expire_after > 0){
    off += snprintf(payload+off, sizeof(payload)-off, ",\"exp_aft\":%d", expire_after);
  }

  /* Device block (lets HA treat it as a single device) */
  off += snprintf(payload+off, sizeof(payload)-off,
    ",\"dev\":{"
      "\"ids\":[\"%s\"],"
      "\"name\":\"%s\","
      "\"mf\":\"WeatherUploader\","
      "\"mdl\":\"Weather Station Bridge\""
    "}"
    "}",
    device_id, cfg->device_name);

  (void)off;
  mosquitto_publish(m, NULL, cfg_topic, (int)strlen(payload), payload, 1, true);
  DBG("HA discovery published: %s (%s)", object_id, cfg_topic);
}

static void publish_ha_discovery_once(struct mosquitto *m,const Config *cfg,const WeatherData *wd){
  if(!m||!cfg->mqtt_enabled||!wd->mac_id[0]) return;
  char device_id[128]; snprintf(device_id,sizeof(device_id),"wx_%s",wd->mac_id);
  char base[512]; snprintf(base,sizeof(base),"weatheruploader/%s",device_id);
  char t[512];

  snprintf(t,sizeof(t),"%s/outdoor/tempf",base);
  ha_discovery_sensor(m,cfg,device_id,"outdoor_tempf","Outdoor Temperature",t,"°F","temperature",cfg->expire_after);

  snprintf(t,sizeof(t),"%s/outdoor/humidity",base);
  ha_discovery_sensor(m,cfg,device_id,"outdoor_humidity","Outdoor Humidity",t,"%","humidity",cfg->expire_after);

  snprintf(t,sizeof(t),"%s/outdoor/dewptf",base);
  ha_discovery_sensor(m,cfg,device_id,"dewptf","Dew Point",t,"°F","temperature",cfg->expire_after);

  snprintf(t,sizeof(t),"%s/outdoor/windchillf",base);
  ha_discovery_sensor(m,cfg,device_id,"windchillf","Wind Chill",t,"°F","temperature",cfg->expire_after);

  snprintf(t,sizeof(t),"%s/pressure/baromin",base);
  ha_discovery_sensor(m,cfg,device_id,"baromin","Pressure (Relative)",t,"inHg","pressure",cfg->expire_after);

  snprintf(t,sizeof(t),"%s/wind/speed",base);
  ha_discovery_sensor(m,cfg,device_id,"windspeedmph","Wind Speed",t,"mph",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/wind/gust",base);
  ha_discovery_sensor(m,cfg,device_id,"windgustmph","Wind Gust",t,"mph",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/wind/dir",base);
  ha_discovery_sensor(m,cfg,device_id,"winddir","Wind Direction",t,"°",NULL,cfg->expire_after);

  /* Rain metrics (MUST match publish_states() topics) */
  snprintf(t,sizeof(t),"%s/rain/rate_inhr",base);
  ha_discovery_sensor(m,cfg,device_id,"rainrate_inhr","Rain rate",t,"in/hr",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/hour_in",base);
  ha_discovery_sensor(m,cfg,device_id,"rain_hour_in","Rain in the last hour",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/day_in",base);
  ha_discovery_sensor(m,cfg,device_id,"dailyrainin","Rain today",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/week_in",base);
  ha_discovery_sensor(m,cfg,device_id,"weeklyrainin","Rain this week",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/month_in",base);
  ha_discovery_sensor(m,cfg,device_id,"monthlyrainin","Rain this month",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/year_in",base);
  ha_discovery_sensor(m,cfg,device_id,"yearlyrainin","Rain this year",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/rain/total_in",base);
  ha_discovery_sensor(m,cfg,device_id,"totalrainin","Total rain",t,"in",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/battery/status",base);
  ha_discovery_sensor(m,cfg,device_id,"battery_status","Battery status",t,"",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/solar/solarradiation",base);
  ha_discovery_sensor(m,cfg,device_id,"solarradiation","Solar Radiation",t,"W/m²",NULL,cfg->expire_after);

  snprintf(t,sizeof(t),"%s/solar/uv",base);
  ha_discovery_sensor(m,cfg,device_id,"uv","UV Index",t,"",NULL,cfg->expire_after);
}

static void publish_states(struct mosquitto *m,const Config *cfg,const WeatherData *wd){
  if(!m||!cfg->mqtt_enabled||!wd->mac_id[0]) return;
  char device_id[128]; snprintf(device_id,sizeof(device_id),"wx_%s",wd->mac_id);
  char base[512]; snprintf(base,sizeof(base),"weatheruploader/%s",device_id);
  char topic[512];

  snprintf(topic,sizeof(topic),"%s/outdoor/tempf",base); mqtt_publish_num(m,topic,wd->tempf,0);
  snprintf(topic,sizeof(topic),"%s/outdoor/humidity",base); mqtt_publish_num(m,topic,wd->humidity,0);
  snprintf(topic,sizeof(topic),"%s/outdoor/dewptf",base); mqtt_publish_num(m,topic,wd->dewptf,0);
  snprintf(topic,sizeof(topic),"%s/outdoor/windchillf",base); mqtt_publish_num(m,topic,wd->windchillf,0);
  snprintf(topic,sizeof(topic),"%s/pressure/absbaromin",base); mqtt_publish_num(m,topic,wd->absbaromin,0);
  snprintf(topic,sizeof(topic),"%s/pressure/baromin",base); mqtt_publish_num(m,topic,wd->baromin,0);
  snprintf(topic,sizeof(topic),"%s/wind/speed",base); mqtt_publish_num(m,topic,wd->windspeedmph,0);
  snprintf(topic,sizeof(topic),"%s/wind/gust",base); mqtt_publish_num(m,topic,wd->windgustmph,0);
  snprintf(topic,sizeof(topic),"%s/wind/dir",base); mqtt_publish_num(m,topic,wd->winddir,0);
  snprintf(topic,sizeof(topic),"%s/wind/avg2m_speed",base); mqtt_publish_num(m,topic,wd->windspdmph_avg2m,0);
  snprintf(topic,sizeof(topic),"%s/wind/avg2m_dir",base); mqtt_publish_num(m,topic,wd->winddir_avg2m,0);
  snprintf(topic,sizeof(topic),"%s/wind/avg10m_speed",base); mqtt_publish_num(m,topic,wd->windspdmph_avg10m,0);
  snprintf(topic,sizeof(topic),"%s/wind/avg10m_dir",base); mqtt_publish_num(m,topic,wd->winddir_avg10m,0);
  snprintf(topic,sizeof(topic),"%s/rain/rate_inhr",base); mqtt_publish_num(m,topic,wd->rainrate_inhr,0);
  snprintf(topic,sizeof(topic),"%s/rain/hour_in",base); mqtt_publish_num(m,topic,wd->rain_hour_in,0);
  snprintf(topic,sizeof(topic),"%s/rain/day_in",base); mqtt_publish_num(m,topic,wd->dailyrainin,0);
  snprintf(topic,sizeof(topic),"%s/rain/week_in",base); mqtt_publish_num(m,topic,wd->weeklyrainin,0);
  snprintf(topic,sizeof(topic),"%s/rain/month_in",base); mqtt_publish_num(m,topic,wd->monthlyrainin,0);
  snprintf(topic,sizeof(topic),"%s/rain/year_in",base); mqtt_publish_num(m,topic,wd->yearlyrainin,0);
  snprintf(topic,sizeof(topic),"%s/rain/total_in",base); mqtt_publish_num(m,topic,wd->totalrainin,0);
  snprintf(topic,sizeof(topic),"%s/solar/solarradiation",base); mqtt_publish_num(m,topic,wd->solarradiation,0);
  snprintf(topic,sizeof(topic),"%s/solar/uv",base); mqtt_publish_num(m,topic,wd->uv,0);

  if(wd->battery_status[0]){ snprintf(topic,sizeof(topic),"%s/battery/status",base); mqtt_publish_str(m,topic,wd->battery_status,0); }

  char ts[64]; utc_now(ts,sizeof(ts));
  snprintf(topic,sizeof(topic),"%s/status/last_utc",base); mqtt_publish_str(m,topic,ts,0);
}

/* ----------------------------- CWOP (APRS-IS) ----------------------------- */
static void aprs_format_latlon(double lat,double lon,char *out,size_t out_sz){
  char ns=(lat>=0)?'N':'S'; char ew=(lon>=0)?'E':'W';
  lat=fabs(lat); lon=fabs(lon);
  int lat_deg=(int)lat; double lat_min=(lat-lat_deg)*60.0;
  int lon_deg=(int)lon; double lon_min=(lon-lon_deg)*60.0;
  snprintf(out,out_sz,"%02d%05.2f%c/%03d%05.2f%c",lat_deg,lat_min,ns,lon_deg,lon_min,ew);
}
static int tcp_connect(const char *host,int port){
  char portstr[16]; snprintf(portstr,sizeof(portstr),"%d",port);
  struct addrinfo hints,*res=NULL,*rp=NULL; memset(&hints,0,sizeof(hints));
  hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
  int err=getaddrinfo(host,portstr,&hints,&res); if(err!=0) return -1;
  int sock=-1;
  for(rp=res; rp; rp=rp->ai_next){
    sock=socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
    if(sock<0) continue;
    if(connect(sock,rp->ai_addr,rp->ai_addrlen)==0) break;
    close(sock); sock=-1;
  }
  freeaddrinfo(res);
  return sock;
}
static int tcp_send_all(int sock,const char *buf){
  size_t left=strlen(buf); const char *p=buf;
  while(left>0){
    ssize_t n=send(sock,p,left,0); if(n<=0) return 1;
    p+= (size_t)n; left-= (size_t)n;
  }
  return 0;
}

static int tcp_recv_some(int sock,char *buf,size_t bufsz,int timeout_ms){
  if(bufsz==0) return 1;
  buf[0]='\0';
  struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
  setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  ssize_t n=recv(sock,buf,bufsz-1,0);
  if(n<=0) return 1;
  buf[n]='\0';
  return 0;
}

static int wait_for_logresp(int sock, const char *station, int timeout_ms){
  /* Wait for javAPRSSrvr logresp line after login.
     Returns 0 if verified, 2 if logresp not verified, 1 on timeout. */
  char buf[512];
  char accum[2048]; size_t alen=0; accum[0]='\0';
  int elapsed=0;

  while(elapsed < timeout_ms){
    if(tcp_recv_some(sock, buf, sizeof(buf), 500) != 0){
      elapsed += 500;
      continue;
    }
    DBG("CWOP resp: %s", buf);

    size_t blen=strlen(buf);
    if(alen + blen >= sizeof(accum)){
      alen = 0;
      accum[0] = '\0';
    }
    memcpy(accum + alen, buf, blen);
    alen += blen;
    accum[alen] = '\0';

    if(strstr(accum, "# logresp") && strstr(accum, station)){
      if(strstr(accum, "verified")) return 0;
      return 2;
    }
  }
  return 1;
}

static void build_aprs_wx_packet(const Config *cfg,const WeatherData *wd,char *out,size_t out_sz){
  time_t now=time(NULL); struct tm tmz; gmtime_r(&now,&tmz);
  /* APRS timestamp: DDHHMMz (Zulu). */
  char ztime[16]; snprintf(ztime,sizeof(ztime),"%02d%02d%02dz",tmz.tm_mday,tmz.tm_hour,tmz.tm_min);
  char pos[32]; aprs_format_latlon(cfg->lat,cfg->lon,pos,sizeof(pos));

  int c=isnan(wd->winddir)?0:(int)llround(wd->winddir);
  if(c<0) c=0;
  if(c>360) c=360;
  int s=isnan(wd->windspeedmph)?0:(int)llround(wd->windspeedmph);
  if(s<0) s=0;
  if(s>999) s=999;
  int g=isnan(wd->windgustmph)?0:(int)llround(wd->windgustmph);
  if(g<0) g=0;
  if(g>999) g=999;
  int t=isnan(wd->tempf)?0:(int)llround(wd->tempf);
  if(t<-99) t=-99;
  if(t>999) t=999;

  int r=isnan(wd->rain_hour_in)?0:(int)llround(wd->rain_hour_in*100.0);
  if(r<0) r=0;
  if(r>999) r=999;
  int p=isnan(wd->dailyrainin)?0:(int)llround(wd->dailyrainin*100.0);
  if(p<0) p=0;
  if(p>999) p=999;

  int hum=isnan(wd->humidity)?0:(int)llround(wd->humidity);
  if(hum<0) hum=0;
  if(hum>100) hum=100;
  if(hum==100) hum=99;

  int b=0;
  if(!isnan(wd->baromin)){
    double mb=wd->baromin*33.8638866667;
    b=(int)llround(mb*10.0);
    if(b<0) b=0;
    if(b>99999) b=99999;
  }

  snprintf(out,out_sz,
    "%s>APRS,TCPIP*:@%s%s_%03d/%03dg%03dt%03dr%03dp%03dh%02db%05d",
    cfg->cwop_station,ztime,pos,c,s,g,(t<0?0:t),r,p,hum,b
  );
}
static int cwop_send_if_due(const Config *cfg, Config *mut_cfg, const WeatherData *wd){
  if(!cfg->cwop_enabled) return 0;
  if(!cfg->cwop_station[0]||!cfg->cwop_pass[0]) return 0;

  time_t now=time(NULL);

  /* Gate: send immediately on boot (last_sent==0), otherwise wait interval */
  if(mut_cfg->cwop_last_sent!=0){
    long age = (long)(now - mut_cfg->cwop_last_sent);
    long rem = (long)cfg->cwop_interval_seconds - age;
    if(rem > 0){
      DBG("CWOP next send in %lds (interval=%ds, age=%lds)", rem, cfg->cwop_interval_seconds, age);
      return 0;
    }
  }else{
    DBG("CWOP boot send (last_sent=0)");
  }

  DBG("CWOP sending: server=%s:%d station=%s interval=%ds", cfg->cwop_server, cfg->cwop_port, cfg->cwop_station, cfg->cwop_interval_seconds);

  int sock=tcp_connect(cfg->cwop_server,cfg->cwop_port);
  if(sock<0){
    DBG("CWOP connect failed");
    return 1;
  }
  DBG("CWOP connected to %s:%d", cfg->cwop_server, cfg->cwop_port);

  /* Read greeting (non-fatal if none) */
  char greet[512]={0};
  if(tcp_recv_some(sock,greet,sizeof(greet),1500)==0){
    DBG("CWOP greeting: %s", greet);
  }

  /* Send login */
  char login[256];
  snprintf(login,sizeof(login),"user %s pass %s vers weatheruploader 3.1\r\n",cfg->cwop_station,cfg->cwop_pass);
  DBG("CWOP login: user %s pass %s vers weatheruploader 3.1", cfg->cwop_station, cfg->cwop_pass);

  if(tcp_send_all(sock,login)!=0){
    DBG("CWOP send failed (login write)");
    close(sock);
    return 2;
  }

  /* Wait for # logresp <station> verified */
  int vr = wait_for_logresp(sock, cfg->cwop_station, 3000);
  if(vr==1){
    DBG("CWOP auth timeout waiting for logresp");
    close(sock);
    return 2;
  }else if(vr==2){
    DBG("CWOP auth failed (logresp not verified)");
    close(sock);
    return 2;
  }
  DBG("CWOP verified OK");

  /* Build and send packet */
  char pkt[512];
  build_aprs_wx_packet(cfg,wd,pkt,sizeof(pkt));
  DBG("CWOP packet: %s", pkt);

  char line[600];
  snprintf(line,sizeof(line),"%s\r\n",pkt);

  if(tcp_send_all(sock,line)!=0){
    DBG("CWOP send failed (packet write)");
    close(sock);
    return 2;
  }

  /* Read any post-send response (non-fatal) */
  char resp[512]={0};
  if(tcp_recv_some(sock,resp,sizeof(resp),1200)==0){
    DBG("CWOP post-send: %s", resp);
  }

  close(sock);

  mut_cfg->cwop_last_sent=now; /* only update on success */
  DBG("CWOP sent OK");
  return 0;
}

/* ----------------------------- Placeholder rendering ----------------------------- */
typedef struct{ char key[64]; char val[MAX_STR]; char val_url[MAX_STR*2]; } PH;
typedef struct{ PH items[128]; int n; } PHMap;

static void ph_put(PHMap *m,CURL *curl,const char *key,const char *val_raw){
  if(!key||!val_raw) return;
  if(m->n>=(int)(sizeof(m->items)/sizeof(m->items[0]))) return;
  PH *it=&m->items[m->n++];
  snprintf(it->key,sizeof(it->key),"%s",key);
  snprintf(it->val,sizeof(it->val),"%s",val_raw);
  char *enc=url_encode(curl,val_raw);
  if(enc){ snprintf(it->val_url,sizeof(it->val_url),"%s",enc); curl_free(enc); }
  else { snprintf(it->val_url,sizeof(it->val_url),"%s",val_raw); }
}
static void ph_put_num(PHMap *m,CURL *curl,const char *key,double v){
  char buf[64];
  if(isnan(v)) buf[0]='\0';
  else snprintf(buf,sizeof(buf),"%.3f",v);
  ph_put(m,curl,key,buf);
}
static const PH *ph_find(const PHMap *m,const char *key){
  for(int i=0;i<m->n;i++) if(strcmp(m->items[i].key,key)==0) return &m->items[i];
  return NULL;
}
static void render_template(const PHMap *m,const char *tmpl,char *out,size_t outsz){
  size_t oi=0;
  for(size_t i=0; tmpl[i] && oi+1<outsz; ){
    if(tmpl[i]=='{'){
      const char *end=strchr(&tmpl[i],'}');
      if(end){
        char key[80];
        size_t klen=(size_t)(end-(&tmpl[i+1]));
        if(klen>=sizeof(key)) klen=sizeof(key)-1;
        memcpy(key,&tmpl[i+1],klen); key[klen]='\0';

        const char *suffix="_url"; size_t slen=strlen(suffix); int use_url=0;
        size_t keylen=strlen(key);
        if(keylen>slen && strcmp(key+keylen-slen,suffix)==0){
          use_url=1;
          key[keylen-slen]='\0';
        }

        const PH *ph=ph_find(m,key);
        const char *rep="";
        if(ph) rep=use_url?ph->val_url:ph->val;
        for(size_t r=0; rep[r] && oi+1<outsz; r++) out[oi++]=rep[r];
        i=(size_t)(end-tmpl)+1;
        continue;
      }
    }
    out[oi++]=tmpl[i++];
  }
  out[oi]='\0';
}
static void build_placeholders(CURL *curl,const Config *cfg,const WeatherData *wd,PHMap *m){
  memset(m,0,sizeof(*m));
  ph_put(m,curl,"mac",wd->mac);
  ph_put(m,curl,"mac_id",wd->mac_id);
  ph_put(m,curl,"ip",wd->ip);
  char rssi_buf[32]; snprintf(rssi_buf,sizeof(rssi_buf),"%d",wd->rssi); ph_put(m,curl,"rssi",rssi_buf);
  char dateutc[64]; utc_now(dateutc,sizeof(dateutc)); ph_put(m,curl,"dateutc",dateutc);
  char epoch_s[32]; snprintf(epoch_s,sizeof(epoch_s),"%ld",epoch_now()); ph_put(m,curl,"epoch",epoch_s);

  ph_put_num(m,curl,"lat",cfg->lat);
  ph_put_num(m,curl,"lon",cfg->lon);
  ph_put_num(m,curl,"elev_ft",cfg->elev_ft);
  ph_put_num(m,curl,"elev_m",cfg->elev_m);

  ph_put(m,curl,"station",cfg->cwop_station);

  ph_put_num(m,curl,"indoortempf",wd->indoortempf);
  ph_put_num(m,curl,"indoorhumidity",wd->indoorhumidity);
  ph_put_num(m,curl,"tempf",wd->tempf);
  ph_put_num(m,curl,"humidity",wd->humidity);
  ph_put_num(m,curl,"dewptf",wd->dewptf);
  ph_put_num(m,curl,"windchillf",wd->windchillf);
  ph_put_num(m,curl,"absbaromin",wd->absbaromin);
  ph_put_num(m,curl,"baromin",wd->baromin);

  ph_put_num(m,curl,"windspeedmph",wd->windspeedmph);
  ph_put_num(m,curl,"windgustmph",wd->windgustmph);
  ph_put_num(m,curl,"winddir",wd->winddir);
  ph_put_num(m,curl,"windspdmph_avg2m",wd->windspdmph_avg2m);
  ph_put_num(m,curl,"winddir_avg2m",wd->winddir_avg2m);
  ph_put_num(m,curl,"windspdmph_avg10m",wd->windspdmph_avg10m);
  ph_put_num(m,curl,"winddir_avg10m",wd->winddir_avg10m);

  ph_put_num(m,curl,"rainrate_inhr",wd->rainrate_inhr);
  ph_put_num(m,curl,"rain_hour_in",wd->rain_hour_in);
  ph_put_num(m,curl,"dailyrainin",wd->dailyrainin);
  ph_put_num(m,curl,"weeklyrainin",wd->weeklyrainin);
  ph_put_num(m,curl,"monthlyrainin",wd->monthlyrainin);
  ph_put_num(m,curl,"yearlyrainin",wd->yearlyrainin);
  ph_put_num(m,curl,"totalrainin",wd->totalrainin);

  ph_put_num(m,curl,"solarradiation",wd->solarradiation);
  ph_put_num(m,curl,"uv",wd->uv);

  ph_put(m,curl,"battery_status",wd->battery_status);
}

/* uploads */
static void wu_build_url(CURL *curl,const UploadTarget *u,const PHMap *ph,char *out,size_t outsz){
  const PH *p_date=ph_find(ph,"dateutc");
  const char *dateutc_url=(p_date)?p_date->val_url:"";
  char id_enc[256]="", pass_enc[256]="";
  char *id_e=url_encode(curl,u->id); char *pw_e=url_encode(curl,u->password);
  if(id_e){ snprintf(id_enc,sizeof(id_enc),"%s",id_e); curl_free(id_e); }
  if(pw_e){ snprintf(pass_enc,sizeof(pass_enc),"%s",pw_e); curl_free(pw_e); }

  #define PRAW(k) (ph_find(ph,(k))?ph_find(ph,(k))->val:"")
  snprintf(out,outsz,
    "%s?ID=%s&PASSWORD=%s&indoortempf=%s&indoorhumidity=%s&tempf=%s&humidity=%s&dewptf=%s&windchillf=%s&absbaromin=%s&baromin=%s"
    "&windspeedmph=%s&windgustmph=%s&winddir=%s&windspdmph_avg2m=%s&winddir_avg2m=%s&windspdmph_avg10m=%s&winddir_avg10m=%s"
    "&rainin=%s&dailyrainin=%s&weeklyrainin=%s&monthlyrainin=%s&solarradiation=%s&UV=%s&dateutc=%s&action=updateraw&realtime=%d&rtfreq=%d&",
    u->base_url,id_enc,pass_enc,
    PRAW("indoortempf"),PRAW("indoorhumidity"),
    PRAW("tempf"),PRAW("humidity"),PRAW("dewptf"),PRAW("windchillf"),
    PRAW("absbaromin"),PRAW("baromin"),
    PRAW("windspeedmph"),PRAW("windgustmph"),PRAW("winddir"),
    PRAW("windspdmph_avg2m"),PRAW("winddir_avg2m"),
    PRAW("windspdmph_avg10m"),PRAW("winddir_avg10m"),
    PRAW("rainrate_inhr"),PRAW("dailyrainin"),PRAW("weeklyrainin"),PRAW("monthlyrainin"),
    PRAW("solarradiation"),PRAW("uv"),
    dateutc_url,
    u->realtime?1:0,
    (u->rtfreq>0?u->rtfreq:20)
  );
  #undef PRAW
}
static void do_uploads(CURL *curl,const Config *cfg,const WeatherData *wd){
  PHMap ph; build_placeholders(curl,cfg,wd,&ph);
  for(int i=0;i<cfg->uploads_n;i++){
    const UploadTarget *u=&cfg->uploads[i];
    if(!u->enabled) continue;
    long tmo=(u->timeout_ms>0)?u->timeout_ms:6000;

    if(str_ieq(u->type,"wu")){
      if(!u->base_url[0]) continue;
      char url[MAX_TEMPLATE]; wu_build_url(curl,u,&ph,url,sizeof(url));
      MemBuf resp; (void)http_get(curl,url,tmo,&resp); free(resp.data);
    } else if(str_ieq(u->type,"custom")){
      char method[8]; snprintf(method,sizeof(method),"%s",u->method[0]?u->method:"GET");
      if(str_ieq(method,"GET")){
        if(!u->template_url[0]) continue;
        char url[MAX_TEMPLATE]; render_template(&ph,u->template_url,url,sizeof(url));
        MemBuf resp; (void)http_get(curl,url,tmo,&resp); free(resp.data);
      } else {
        if(!u->url[0]||!u->body_template[0]) continue;
        char body[MAX_TEMPLATE]; render_template(&ph,u->body_template,body,sizeof(body));
        MemBuf resp; (void)http_post(curl,u->url,body,tmo,u->headers,u->headers_n,&resp); free(resp.data);
      }
    }
  }
}

static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s [-d] [-c <config.ini>]\n"
    "  -c <file>   Path to INI config file (default: ./config.ini)\n"
    "  -d          Enable debug logging to stderr\n",
    prog);
}

/* main */
static void make_url(char *out,size_t outsz,const char *ip,const char *path_q){ snprintf(out,outsz,"http://%s%s",ip,path_q); }

int main(int argc,char **argv){
  const char *ini_path="./config.ini";

  int opt;
  while((opt=getopt(argc,argv,"dc:h"))!=-1){
    switch(opt){
      case 'd': g_debug=1; break;
      case 'c': ini_path=optarg; break;
      case 'h': default: usage(argv[0]); return (opt=='h')?0:2;
    }
  }

  DBG("Debug enabled");

  Config cfg; set_defaults(&cfg);
  if(load_ini(ini_path,&cfg)!=0){ fprintf(stderr,"Failed to load INI: %s\n",ini_path); return 2; }
  if(cfg.base_ip[0]=='\0'){ fprintf(stderr,"ERROR: [poll] base_ip is required in %s\n",ini_path); return 2; }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL *curl=curl_easy_init();
  if(!curl){ fprintf(stderr,"curl init failed\n"); return 3; }

  struct mosquitto *mosq=NULL;
  int discovery_sent=0;

  if(cfg.mqtt_enabled){
    mosquitto_lib_init();
    mosq=mosquitto_new(NULL,true,NULL);
    if(!mosq){ fprintf(stderr,"mosquitto_new failed\n"); cfg.mqtt_enabled=0; }
    else {
      if(cfg.mqtt_user[0]||cfg.mqtt_pass[0]) mosquitto_username_pw_set(mosq, cfg.mqtt_user[0]?cfg.mqtt_user:NULL, cfg.mqtt_pass[0]?cfg.mqtt_pass:NULL);
      mosquitto_reconnect_delay_set(mosq,2,10,true);
      if(mosquitto_connect(mosq,cfg.mqtt_host,cfg.mqtt_port,30)!=MOSQ_ERR_SUCCESS){
        fprintf(stderr,"MQTT connect failed to %s:%d\n",cfg.mqtt_host,cfg.mqtt_port);
      } else {
        mosquitto_loop_start(mosq);
      }
    }
  }

  fprintf(stderr,"weatheruploader v3.1 started. INI=%s base_ip=%s poll=%ds cwop=%s/%ds\n",
          ini_path,cfg.base_ip,cfg.poll_interval_seconds,cfg.cwop_enabled?"on":"off",cfg.cwop_interval_seconds);

  for(;;){
    WeatherData wd; wd_init(&wd);

    /* connect_status */
    { char url[MAX_STR]; make_url(url,sizeof(url),cfg.base_ip,"/config?command=connect_status");
      MemBuf resp; if(http_get(curl,url,4000,&resp)==0 && resp.data) (void)parse_connect_status(resp.data,&wd);
        DBG("connect_status ip=%s mac=%s rssi=%d", wd.ip, wd.mac, wd.rssi);
      free(resp.data);
    }

    /* rec_refresh */
    { char url[MAX_STR]; make_url(url,sizeof(url),cfg.base_ip,"/client?command=rec_refresh");
      MemBuf resp; (void)http_get(curl,url,4000,&resp); free(resp.data);
      if(cfg.refresh_delay_ms>0) msleep(cfg.refresh_delay_ms);
    }

    /* record */
    { char url[MAX_STR]; make_url(url,sizeof(url),cfg.base_ip,"/client?command=record");
      MemBuf resp; if(http_get(curl,url,6000,&resp)==0 && resp.data){
        if(parse_record(resp.data,&wd)!=0) fprintf(stderr,"parse_record failed\n");
        else DBG("record parsed: tempf=%.2f hum=%.0f wind=%.2f gust=%.2f dir=%.0f baro=%.2f rain_day=%.2f solar=%.2f uv=%.2f", wd.tempf, wd.humidity, wd.windspeedmph, wd.windgustmph, wd.winddir, wd.baromin, wd.dailyrainin, wd.solarradiation, wd.uv);
      } else {
        fprintf(stderr,"record fetch failed\n");
      }
      free(resp.data);
    }

    if(wd.has_data){
      if(cfg.mqtt_enabled && mosq){
        if(!discovery_sent && wd.mac_id[0]){ publish_ha_discovery_once(mosq,&cfg,&wd); discovery_sent=1; }
        publish_states(mosq,&cfg,&wd);
      }
      if(cfg.cwop_enabled){
        int rc=cwop_send_if_due(&cfg,&cfg,&wd);
        if(rc!=0) fprintf(stderr,"CWOP send failed (%d)\n",rc);
      }
      do_uploads(curl,&cfg,&wd);
    } else {
      fprintf(stderr,"No data parsed this cycle\n");
    }

    if(cfg.poll_interval_seconds<=0) cfg.poll_interval_seconds=20;
    sleep((unsigned int)cfg.poll_interval_seconds);
  }

  return 0;
}
