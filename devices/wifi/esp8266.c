#include "esp8266.h"
#include "atadapter.h"
#include "at_api_interface.h"
#include "atiny_socket.h"

extern at_task at;

at_adaptor_api at_interface;

int32_t esp8266_reset(void)
{
    return at.cmd((int8_t*)AT_CMD_RST, strlen(AT_CMD_RST), "ready", NULL);
}

int32_t esp8266_choose_net_mode(enum_net_mode m)
{
    char cmd[64] = {0};
    snprintf(cmd, 64, "%s=%d", AT_CMD_CWMODE, (int)m);
    return at.cmd((int8_t*)cmd, strlen(cmd), "OK", "no change"); 
}

int32_t esp8266_set_mux_mode(int32_t m)
{
    char cmd[64] = {0};
    snprintf(cmd, 64, "%s=%d", AT_CMD_MUX, (int)m);
    return at.cmd((int8_t*)cmd, strlen(cmd), "OK", NULL);
}
int32_t esp8266_joinap(char * pssid, char * ppasswd)
{
    char cmd[64] = {0};
    snprintf(cmd, 64, "%s=\"%s\",\"%s\"", AT_CMD_JOINAP, pssid, ppasswd);
    return at.cmd((int8_t*)cmd, strlen(cmd), "OK", NULL); 
}

int32_t esp8266_connect(const int8_t * host, const int8_t *port, int32_t proto)
{
    int32_t ret = AT_FAILED;
    int32_t id = at.get_id();
    char cmd[64] = {0};

    if (AT_MUXMODE_SIGNEL == at.mux_mode)
    {
        snprintf(cmd, 64, "%s=\"%s\",\"%s\",%s", AT_CMD_CONN, proto == ATINY_PROTO_UDP? "UDP" : "TCP", host, port);
    }
    else 
    {
        if (id < 0 || id >= AT_MAX_LINK_NUM)
        {
            AT_LOG("no vailed linkid for use(id = %d)", id);
            return -1;
        }

        ret = LOS_QueueCreate("dataQueue", 16, &at.linkid[id].qid, 0, sizeof(QUEUE_BUFF));
        if (ret != LOS_OK)
        {
            AT_LOG("init dataQueue failed!");
            return  -1;
        }

        at.linkid[id].usable = 1;
        snprintf(cmd, 64, "%s=%d,\"%s\",\"%s\",%s", AT_CMD_CONN, id, proto == ATINY_PROTO_UDP? "UDP" : "TCP", host, port);
    }
    at.cmd((int8_t *)cmd, strlen(cmd), "OK", NULL);
    return id;
}

int32_t esp8266_send(int32_t id , const uint8_t  *buf, uint32_t len)
{
    char cmd[64] = {0};
    if (AT_MUXMODE_SIGNEL == at.mux_mode)
    {
        snprintf(cmd, 64, "%s=%d", AT_CMD_SEND, len);
    }
    else
    {
        snprintf(cmd, 64, "%s=%d,%d", AT_CMD_SEND, id, len);
    }
    at.cmd((int8_t*)cmd, strlen(cmd), ">", NULL);

    return at.write((int8_t*)buf, len);

}

int32_t esp8266_recv(int32_t id, int8_t * buf, uint32_t len)
{
   uint32_t qlen = sizeof(QUEUE_BUFF);

    QUEUE_BUFF  qbuf = {0, NULL};
    int ret = LOS_QueueReadCopy(at.linkid[id].qid, &qbuf, &qlen, LOS_WAIT_FOREVER);
    AT_LOG("ret = %x, len = %d", ret, qbuf.len);

    if (qbuf.len){
        memcpy(buf, qbuf.addr, qbuf.len);
        atiny_free(qbuf.addr);
    }
    return qbuf.len;
}

int32_t esp8266_recv_timeout(int32_t id, int8_t * buf, uint32_t len, int32_t timeout)
{
   uint32_t qlen = sizeof(QUEUE_BUFF);

    QUEUE_BUFF  qbuf = {0, NULL};
    int ret = LOS_QueueReadCopy(at.linkid[id].qid, &qbuf, &qlen, timeout);
    AT_LOG("ret = %x, len = %d, id = %d", ret, qbuf.len, id);

    if (qbuf.len){
        memcpy(buf, qbuf.addr, qbuf.len);
        atiny_free(qbuf.addr);
    }
    return qbuf.len;
}

int32_t esp8266_close(int32_t id)
{
    char cmd[64] = {0};
    if (AT_MUXMODE_SIGNEL == at.mux_mode)
    {
        snprintf(cmd, 64, "%s", AT_CMD_CLOSE);
    }
    else
    {
        LOS_QueueDelete(at.linkid[id].qid);
        at.linkid[id].usable = 0;
        snprintf(cmd, 64, "%s=%d", AT_CMD_CLOSE, id);
    }
    return at.cmd((int8_t*)cmd, strlen(cmd), "OK", NULL);
}

int32_t esp8266_data_handler(int8_t * buf, int32_t len)
{
    if (NULL == buf || len <= 0)
    {
        AT_LOG("param invailed!");
        return -1;
    }
    AT_LOG("entry!");

    //process data frame ,like +IPD,linkid,len:data
    int32_t ret = 0;
    int32_t linkid = 0, data_len = 0;
    char * p1, *p2;
    QUEUE_BUFF qbuf;
    p1 = (char *)buf;

    if (0 == memcmp(p1, AT_DATAF_PREFIX, strlen(AT_DATAF_PREFIX)))
    {
        p2 = strstr(p1, ",");
        if (NULL == p2)
        {
            AT_LOG("got data prefix invailed!");
            goto END;
        }
        
        if (AT_MUXMODE_MULTI == at.mux_mode)
        {
            linkid = 0;
            for (p2++; *p2 <= '9' && *p2 >= '0'; p2++)
            {
                linkid = linkid * 10 + (*p2 - '0');
            }
        }

        for (p2++; *p2 <= '9' && *p2 >= '0' ;p2++)
        {
            data_len = (data_len * 10 + (*p2 - '0'));
        }
        p2++; //over ':'

        qbuf.addr = atiny_malloc(data_len);
        if (NULL == qbuf.addr)
        {
            AT_LOG("malloc for qbuf failed!");
            goto END;
        }

        qbuf.len = data_len;
        memcpy(qbuf.addr, p2, data_len);

        if (LOS_OK != (ret = LOS_QueueWriteCopy(at.linkid[linkid].qid, &qbuf, sizeof(QUEUE_BUFF), 0)))
        {
            AT_LOG("LOS_QueueWriteCopy  failed! ret = %x", ret);
            atiny_free(qbuf.addr);
            goto END;
        }
        ret = (p2 + data_len - buf);
    }
END:
    return ret;
}

int8_t* esp8266_get_localip(int8_t * ip, int8_t * gw, int8_t * mask)/*获取本地IP*/
{
    char resp[512] = {0};
    at.cmd((int8_t*)AT_CMD_CHECK_IP, strlen((char*)AT_CMD_CHECK_IP), "OK", resp);

    char * p1, *p2;
    p1 = strstr(resp, "ip");
    if (ip && p1)
    {
        p1 = strstr(p1, "\"");
        p2 = strstr(p1 + 1, "\"");
        memcpy(ip, p1 + 1, p2 - p1 -1);
    }

    p1 = strstr(resp, "gateway");
    if (gw && p1)
    {
        p1 = strstr(p1, "\"");
        p2 = strstr(p1 + 1, "\"");
        memcpy(gw, p1 + 1, p2 - p1 -1);
    }

    p1 = strstr(resp, "netmask");
    if (mask && p1)
    {
        p1 = strstr(p1, "\"");
        p2 = strstr(p1 + 1, "\"");
        memcpy(mask, p1 + 1, p2 - p1 -1);
    }

//    printf("get ip :%s", resp);
    return NULL;
}

int8_t* esp8266_get_localmac(int8_t * mac)/*获取本地IP*/
{
    char resp[512] = {0};    
    char * p1, *p2;

    at.cmd((int8_t*)AT_CMD_CHECK_MAC, strlen((char*)AT_CMD_CHECK_MAC), "OK", resp);

    p1 = strstr(resp, ":");
    if (mac && p1)
    {
        p1 = strstr(p1, "\"");
        p2 = strstr(p1 + 1, "\"");
        memcpy(mac, p1 + 1, p2 - p1 -1);
    }


//    printf("get ip :%s", resp);
    return NULL;
}

int32_t esp8266_recv_cb(int32_t id)
{
    return -1;
}

int32_t esp8266_deinit(void)
{
    return AT_FAILED;
}

int32_t esp8266_listner_add(int8_t * perfix , int8_t * suffix, event_cb cb)
{
    at_listner *l;

    l = atiny_malloc(sizeof(at_listner));
    if (NULL == l)
        return -1;

    l->perfix = perfix;
    l->suffix = suffix;
    l->callback = cb;
    l->resp = NULL;
    l->resp_len = 0;

    at_listener_list_add(l);
    return 0;
}


int32_t esp8266_init()
{
    at.init();
    esp8266_listner_add((int8_t*)AT_DATAF_PREFIX, NULL, esp8266_data_handler);

    esp8266_reset();
    esp8266_choose_net_mode(STA);
    while(0 == esp8266_joinap(WIFI_SSID, WIFI_PASSWD));
    esp8266_set_mux_mode(at.mux_mode);

    static int8_t ip[32];
    static int8_t gw[32];
    static int8_t mac[32];
    esp8266_get_localip(ip, gw, NULL);
    esp8266_get_localmac(mac);
    AT_LOG("get ip:%s, gw:%s mac:%s", ip, gw, mac);
    return AT_OK;
}

at_adaptor_api at_interface = {
    .name = (int8_t*)AT_MODU_NAME,
    .timeout = 2000,   // 2000 tick
    .init = esp8266_init,    

//    .get_id = NULL, /*获取连接id，仅多连接时需要*/
//    .get_localip = esp8266_get_localip,/*获取本地IP*/
    /*建立TCP或者UDP连接*/
    .connect = esp8266_connect,
    /*发送，当命令发送后，如果超过一定的时间没收到应答，要返回错误*/
    .send = esp8266_send,

    .recv_timeout = esp8266_recv_timeout,
    .recv = esp8266_recv,

    .close = esp8266_close,/*关闭连接*/
    .recv_cb = esp8266_recv_cb,/*收到各种事件处理，暂不实现 */

    .deinit = esp8266_deinit,
};

