#!/usr/bin/env python3
"""Actual HTTP request serializer: coalesced binary POST, large fallback, failed sends."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'src/net/http.c').read_text()
a=src.index('static int has_header(');b=src.index('static int http_request_impl(',a)
harness=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef struct {int tls;char host[256];int port;char path[2048];}url_parts;
typedef struct {const char *name,*value;}http_header;
typedef int conn;
#define USER_AGENT "test-agent"
static unsigned calls;static int fail;
static unsigned char captured[65536];static size_t used;
static int conn_write(conn *c,const void *data,size_t len){calls++;if(fail)return -1;assert(used+len<=sizeof(captured));memcpy(captured+used,data,len);used+=len;return 0;}
'''+src[a:b]+r'''
int main(void){
 conn c=0;url_parts u={.tls=1,.host="example.test",.port=443,.path="/api?x=1"};
 http_header h[]={{"Content-Type","custom/type"},{"X-Token","sample"},{"Host","bad"},{"Connection","close"},{"Content-Length","999"}};
 unsigned char body[8193];for(size_t i=0;i<sizeof(body);i++)body[i]=(unsigned char)i;
 size_t lengths[]={0,1,64,8192,8193};
 for(unsigned i=0;i<5;i++){
  size_t n=lengths[i];calls=used=0;
  assert(send_request(&c,"POST",&u,h,5,body,n)==0);
  assert(calls==(n>8192?2:1));
  unsigned char *sep=(unsigned char *)strstr((char *)captured,"\r\n\r\n");assert(sep);size_t header=sep+4-captured;
  assert(used==header+n&&!memcmp(captured+header,body,n));
  assert(!strncmp((char *)captured,"POST /api?x=1 HTTP/1.1\r\nHost: example.test\r\n",43));
  assert(strstr((char *)captured,"Content-Type: custom/type\r\n"));
  assert(!strstr((char *)captured,"Host: bad")&&!strstr((char *)captured,"Connection: close"));
  char expected[64];snprintf(expected,sizeof(expected),"Content-Length: %zu\r\n",n);assert(strstr((char *)captured,expected));
  memset(captured,0,sizeof(captured));
 }
 calls=used=0;u.port=8443;
 assert(!send_request(&c,"GET",&u,NULL,0,NULL,0));assert(calls==1);
 assert(strstr((char *)captured,"Host: example.test:8443\r\n"));assert(!strstr((char *)captured,"Content-Length:"));
 calls=used=0;fail=1;assert(send_request(&c,"POST",&u,NULL,0,body,8193)==-1&&calls==1);
 puts("PASS: exact HTTP headers/binary body; small POST one write; large body unchanged; custom headers/ports; no body after failed send");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness)
 subprocess.run(['cc','-Wno-deprecated-declarations','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
