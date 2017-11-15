/*
 * Whitecat, Lua net module
 * 
 * This module contains elua parts
 *
 * Copyright (C) 2015 - 2016
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, ƒintedata or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "whitecat.h"

#if LUA_USE_NET

#include "espressif/esp_common.h"

#include "lualib.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/opt.h"

#include "lwip/sys.h"

#include <FreeRTOS.h>
#include <task.h>
#include <httpd/httpd.h>


typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

int platform_net_exists(const char *interface);
void platform_net_stat_iface(const char *interface);
const char *platform_net_error(int code);
int platform_net_start(const char *interface);
int platform_net_stop(const char *interface);
int platform_net_sntp_start();

typedef s16 net_size;

enum {
  NET_ERR_OK = 0,
  NET_ERR_TIMEDOUT,
  NET_ERR_CLOSED,
  NET_ERR_ABORTED,
  NET_ERR_OVERFLOW,
  NET_ERR_OTHER,
};

typedef union {
  u32     ipaddr;
  u8      ipbytes[ 4 ];
  u16     ipwords[ 2 ];
} net_ip;

#define NET_SOCK_STREAM          0
#define NET_SOCK_DGRAM           1
#define NET_DHCP                 2
#define NET_STATIC               3

static int errno_to_err() {
    switch (errno) {
        case 0: return NET_ERR_OK;
        case EWOULDBLOCK: return NET_ERR_TIMEDOUT;
        case ECONNABORTED: return NET_ERR_ABORTED;
        case ENOTCONN: return NET_ERR_CLOSED;
        default:
            return NET_ERR_OTHER;
    }
}

static int net_accept( lua_State *L ) {
    int newsockfd;
    int ret = NET_ERR_OK;
    int clilen;
    
    struct sockaddr_in serv_addr, cli_addr;

    u16 sock = ( u16 )luaL_checkinteger( L, 1 );
    u16 port = ( u16 )luaL_checkinteger( L, 2 );
    u16 timeout = ( u16 )luaL_checkinteger( L, 3 );
    
    bzero((char *) &serv_addr, sizeof(serv_addr));
   
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
   
     // Get socket type
    int type;
    int length = sizeof( int );
    
    if ((ret = getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &length)) < 0) {
        ret = errno_to_err();
        goto exit;
    }
    
    if (bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
         ret = errno_to_err();
         goto exit;
    }
      
    if (type == SOCK_STREAM) {
        listen(sock,5);
        clilen = sizeof(cli_addr);
    }
    
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    if (type == SOCK_STREAM) {
        newsockfd = accept(sock, (struct sockaddr *)&cli_addr, &clilen);
        if (newsockfd < 0) {
             ret = errno_to_err();
             goto exit;
        }
    }
exit:
    lua_pushinteger( L, newsockfd );
    lua_pushinteger( L, cli_addr.sin_addr.s_addr );
    lua_pushinteger( L, ret );

    return 3;
}

// Lua: sock = socket( type )
static int net_socket( lua_State *L ) {
    int type = ( int )luaL_checkinteger( L, 1 );

    int socket_fd = 0;

    switch (type) {
        case NET_SOCK_DGRAM:
            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            break;

        case NET_SOCK_STREAM:
            socket_fd = socket(AF_INET, SOCK_STREAM, 0);
            break;
    }

    lua_pushinteger( L, socket_fd );
    return 1;
}

// Lua: res = close( socket )
static int net_close(lua_State* L) {
  int sock = ( int )luaL_checkinteger( L, 1 );
  int ret = NET_ERR_OK;
  
  if (closesocket(sock) < 0) {
      ret = errno_to_err();
  }
  
  lua_pushinteger( L, ret );
  return 1;
}

// Lua: res, err = send( sock, str )
static int net_send(lua_State* L) {
    int sock = ( int )luaL_checkinteger( L, 1 );
    const char *buf;
    size_t siz,sended = 0;
    int ret = NET_ERR_OK;

    if (lua_type( L, 2) == LUA_TSTRING) {
        buf = luaL_checklstring( L, 2, &siz );
    } else {
        return luaL_error( L, "Unsupported data type for send" );    
    }
    
    // Get socket type
    int type;
    int length = sizeof( int );
    
    if ((ret = getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &length)) < 0) {
        ret = errno_to_err();
    } else {
        if (type == SOCK_STREAM) {
            // TPC
            if ((sended = send(sock, buf, siz, 0)) < 0) {
                ret = errno_to_err();
                sended = 0;
            }       
        } else {
            // UDP
            struct sockaddr addr;
            size_t len;
            
            // GET ip of client
            len = sizeof(struct sockaddr);
            getpeername(sock, &addr, &len);

            // Send
            if ((sended = sendto(sock, buf, siz, 0, &addr, len)) < 0) {
                ret = errno_to_err();
                sended = 0;
            }       
        }
    }
    
    lua_pushinteger( L, sended );
    lua_pushinteger( L, ret );
    
    return 2;  
}

// Lua: err = connect( sock, iptype, port )
// "iptype" is actually an int returned by "net.packip"
static int net_connect( lua_State *L ) { 
    unsigned int ip;
    int rc = 0;
    struct sockaddr_in address;
    int ret = NET_ERR_OK;
  
    int sock = ( int )luaL_checkinteger( L, 1 );
    u16 port = ( int )luaL_checkinteger( L, 3 );
    ip = ( u32 )luaL_checkinteger( L, 2 );
 
    address.sin_port = htons(port);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ip;
        
    // Get socket type
    int type;
    int length = sizeof( int );
    
    if ((ret = getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &length)) < 0) {
        ret = errno_to_err();
    } else {
        if (type == SOCK_STREAM) {
            rc = connect(sock, (struct sockaddr*)&address, sizeof(address));
            if (rc < 0) {
                ret = errno_to_err();
            }            
        } else {
            ret = NET_ERR_OK;
        }
    }   
 
    lua_pushinteger( L, ret );
    
    return 1;  
}

// Lua: data = packip( ip0, ip1, ip2, ip3 ), or
// Lua: data = packip( "ip" )
// Returns an internal representation for the given IP address
static int net_packip( lua_State *L )
{
  net_ip ip;
  unsigned i, temp;
  
  if( lua_isnumber( L, 1 ) )
    for( i = 0; i < 4; i ++ )
    {
      temp = luaL_checkinteger( L, i + 1 );
      if( temp < 0 || temp > 255 )
        return luaL_error( L, "invalid IP adddress" );
      ip.ipbytes[ i ] = temp;
    }
  else
  {
    const char* pip = luaL_checkstring( L, 1 );
    unsigned len, temp[ 4 ];
    
    if( sscanf( pip, "%u.%u.%u.%u%n", temp, temp + 1, temp + 2, temp + 3, &len ) != 4 || len != strlen( pip ) )
      return luaL_error( L, "invalid IP adddress" );    
    for( i = 0; i < 4; i ++ )
    {
      if( temp[ i ] < 0 || temp[ i ] > 255 )
        return luaL_error( L, "invalid IP address" );
      ip.ipbytes[ i ] = ( u8 )temp[ i ];
    }
  }
  lua_pushinteger( L, ip.ipaddr );
  return 1;
}

// Lua: ip0, ip1, ip2, ip3 = unpackip( iptype, "*n" ), or
//               string_ip = unpackip( iptype, "*s" )
static int net_unpackip( lua_State *L )
{
  net_ip ip;
  unsigned i;  
  const char* fmt;
  
  ip.ipaddr = ( u32 )luaL_checkinteger( L, 1 );
  fmt = luaL_optstring( L, 2, "*n" );
  if( !strcmp( fmt, "*n" ) )
  {
    for( i = 0; i < 4; i ++ ) 
      lua_pushinteger( L, ip.ipbytes[ i ] );
    return 4;
  }
  else if( !strcmp( fmt, "*s" ) )
  {
    lua_pushfstring( L, "%d.%d.%d.%d", ( int )ip.ipbytes[ 0 ], ( int )ip.ipbytes[ 1 ], 
                     ( int )ip.ipbytes[ 2 ], ( int )ip.ipbytes[ 3 ] );
    return 1;
  }
  else
    return luaL_error( L, "invalid format" );                                      
}

// Lua: res, err = recv( sock, maxsize, [timer_id, timeout] ) or
//      res, err = recv( sock, "*l", [timer_id, timeout] )
static int net_recv( lua_State *L ) {
    int sock = ( int )luaL_checkinteger( L, 1 );
    int maxsize;
    int ret = NET_ERR_OK;
    luaL_Buffer net_recv_buff;
    int received = 0;
    int string = 0;
    int n;
    char c;
    
    if( lua_isnumber( L, 2 ) ) {
        maxsize = ( net_size )luaL_checkinteger( L, 2 );
    } else {
        if( strcmp( luaL_checkstring( L, 2 ), "*l" ) )
            return luaL_error( L, "invalid second argument to recv" );
      
        string = 1;
        maxsize = BUFSIZ;
    }
    
    luaL_buffinit( L, &net_recv_buff );

    // Get socket type
    int type;
    int length = sizeof( int );
    
    if ((ret = getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &length)) < 0) {
        ret = errno_to_err();
    } else {
        if (type == SOCK_STREAM) {
            // TPC
            if (string){
                received = 0;
                while (((n = recv(sock, &c, 1, 0)) > 0) && (received < maxsize)) {
                    *((char *)net_recv_buff.b) = c;
                    net_recv_buff.b++;
                    net_recv_buff.n++;
                    received++;
                    
                    if (c == '\n') break;
                }
                
                net_recv_buff.b = &net_recv_buff.initb[0];
            } else {
                if ((received = recv(sock, net_recv_buff.b, maxsize, 0)) < 0) {
                    ret = errno_to_err();
                    received = 0;
                }                       
            }
        } else {
            // UDP
        }
    }
    
    luaL_pushresult( &net_recv_buff );
    lua_pushinteger( L, ret );

    return 2;
}

// Lua: iptype = lookup( "host name" )
static int net_lookup(lua_State* L) {
  const char* name = luaL_checkstring( L, 1 );
  const char* prt = luaL_optstring( L, 2, "80" );
    int port = 0;
 	struct sockaddr_in address;
	int rc = 0;
	sa_family_t family = AF_INET;
	struct addrinfo *result = NULL;
//	struct addrinfo hints = {0, AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL};
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};

	rc = getaddrinfo(name, /*NULL*/prt, &hints, &result);
	if(rc != 0 || result == NULL) {
		printf("DNS lookup failed err=%d res=%p\r\n", rc, result);
		if(result)
			freeaddrinfo(result);
		//vTaskDelay(1000 / portTICK_PERIOD_MS);
		//failures++;
		//continue;
		return 0;
	}
//	if (rc == 0) {
		struct addrinfo *res = result;
		while (res) {
			if (res->ai_family == AF_INET) {
				result = res;
				break;
			}
			res = res->ai_next;
		}

		if (result->ai_family == AF_INET) {
			address.sin_port = htons(port);
			address.sin_family = family = AF_INET;
            address.sin_addr = ((struct sockaddr_in*)(result->ai_addr))->sin_addr;
		} else {
            rc = -1;
        }
        
        freeaddrinfo(result);
//	}
    
//  if (rc == 0) {
      lua_pushinteger( L, address.sin_addr.s_addr );
      return 1;
//  } else {
//      return 0;
//  }
}


// net.setup( "your SSID", "your password")
static int net_setup(lua_State* L) {
	int r;
//    const char *interface = "wf"; //luaL_checkstring( L, 1 );

/*
    if (!platform_net_exists(interface)) {
        return luaL_error(L, "unknown interface");    
    }

    //if (strcmp(interface, "en") == 0) {
        
    //}
    
//    if (strcmp(interface, "gprs") == 0) {
//        const char *apn = luaL_checkstring( L, 2 );
//        const char *pin = luaL_checkstring( L, 3 );
//        
//        platform_net_setup_gprs(apn, pin);
//    }
*/
    
//    if (strcmp(interface, "wf") == 0) {
		const char *ssid=luaL_checkstring( L, 1); //2 );
		const char *password=luaL_optstring( L, 2, ""); //3 , "");
		
		struct sdk_station_config config = {
			.ssid =		"",
			.password =	"",
		};
		strncpy(config.ssid, ssid, 32-1);
		strncpy(config.password, password, 64-1);
		
		/* required to call wifi_set_opmode before station_set_config */
		r = sdk_wifi_set_opmode(STATION_MODE);
		r = sdk_wifi_station_set_config(&config);
//    }
    
    return 0;
}

/*
static int net_start(lua_State* L) {
    const char *interface = luaL_checkstring( L, 1 );

    if (!platform_net_exists(interface)) {
        return luaL_error(L, "unknown interface");    
    }
    
    int res = platform_net_start(interface);
    if (res) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, platform_net_error(res));   
        
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;      
}
*/

/*
static int net_stop(lua_State* L) {
    const char *interface = luaL_checkstring( L, 1 );
    
    if (!platform_net_exists(interface)) {
        return luaL_error(L, "interface not supported in this release");    
    }

    int res = platform_net_stop(interface);
    if (res) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, platform_net_error(res));   
        
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;      
}
*/

/*
static int net_stat(lua_State* L) {
//    platform_net_stat_iface("en");printf("\n");
//    platform_net_stat_iface("gprs");printf("\n");
    platform_net_stat_iface("wf");printf("\n");
    
    return 0;    
}   
*/

static int net_get_local_ip(lua_State* L) {
	struct ip_info info;
//	uint32_t ip;
	
	sdk_wifi_get_ip_info(STATION_IF, &info);
//	ip = info.ip;

	char* fmt = luaL_optstring( L, 1, "*n" );
	if( !strcmp( fmt, "*n" ) )
	{
	  lua_pushinteger(L, ip4_addr1(&info.ip) );
	  lua_pushinteger(L, ip4_addr2(&info.ip) );
	  lua_pushinteger(L, ip4_addr3(&info.ip) );
	  lua_pushinteger(L, ip4_addr4(&info.ip) );
	  return 4;
	}
	else if( !strcmp( fmt, "*s" ) )
	{
	  lua_pushfstring( L, "%d.%d.%d.%d", 
						( int )ip4_addr1(&info.ip), 
						( int )ip4_addr2(&info.ip), 
						( int )ip4_addr3(&info.ip), 
						( int )ip4_addr4(&info.ip) );
	  return 1;
	}

	return 0;
}


static int net_sntp(lua_State* L) {
    int res;
    
    res = platform_net_sntp_start();
    if (res) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, platform_net_error(res));   
        return 2;
    }
    
    time_t start;
    time_t now;

    time(&start);

    if (start < 1420070400) {
        // Clock not set
        // Wait for set, or things may be wrong
        time(&now);
        while ((now < 1420070400) && (now - start < 30)) {
            time(&now);                    
        }

        if (now < 1420070400) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "Network timeout");   
            
            return 2;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}




//#include "httpd.inc.c"
//int httpd_start(lua_State* L) ;
//int httpd_task(lua_State* L) ;
//int httpd_task_stop(lua_State* L) ;



#include "modules.h"

const LUA_REG_TYPE sock_map[] = {
//	{ LSTRKEY( "setup" ),		LFUNCVAL( net_setup ) },
//    { LSTRKEY( "sntp" ),		LFUNCVAL( net_sntp )},
//    { LSTRKEY( "start" ),		LFUNCVAL( net_start )},
//    { LSTRKEY( "stop" ),		LFUNCVAL( net_stop )},
//    { LSTRKEY( "stat" ),		LFUNCVAL( net_stat )},
    { LSTRKEY( "accept" ),		LFUNCVAL( net_accept )},
//    { LSTRKEY( "packip" ),		LFUNCVAL( net_packip )},
//    { LSTRKEY( "unpackip" ),	LFUNCVAL( net_unpackip )},
    { LSTRKEY( "connect" ),		LFUNCVAL( net_connect )},
    { LSTRKEY( "socket" ),		LFUNCVAL( net_socket )},
    { LSTRKEY( "close" ),		LFUNCVAL( net_close )},
    { LSTRKEY( "send" ),		LFUNCVAL( net_send )},
    { LSTRKEY( "recv" ),		LFUNCVAL( net_recv )},
//    { LSTRKEY( "lookup" ),		LFUNCVAL( net_lookup )},
	{ LNILKEY, LNILVAL }
};

const LUA_REG_TYPE net_map[] = {
		{ LSTRKEY( "setup" ),		LFUNCVAL( net_setup ) },
		
		{ LSTRKEY( "sntp" ),		LFUNCVAL( net_sntp )},
		{ LSTRKEY( "lookup" ),		LFUNCVAL( net_lookup )},
		{ LSTRKEY( "packip" ),		LFUNCVAL( net_packip )},
		{ LSTRKEY( "unpackip" ),	LFUNCVAL( net_unpackip )},
		{ LSTRKEY( "localip" ),		LFUNCVAL( net_get_local_ip )},

		{ LSTRKEY( "SOCK_STREAM" ),	LINTVAL( NET_SOCK_STREAM )},
		{ LSTRKEY( "SOCK_DGRAM" ),	LINTVAL( NET_SOCK_DGRAM )},
		
		{ LSTRKEY( "DHCP" ),	LINTVAL( NET_DHCP )},
		{ LSTRKEY( "STATIC" ),	LINTVAL( NET_STATIC )},
		
		{ LSTRKEY( "ERR_OK" ),	LINTVAL( NET_ERR_OK )},
		{ LSTRKEY( "ERR_TIMEDOUT" ),	LINTVAL( NET_ERR_TIMEDOUT )},
		{ LSTRKEY( "ERR_CLOSED" ),	LINTVAL( NET_ERR_CLOSED )},
		{ LSTRKEY( "ERR_ABORTED" ),	LINTVAL( NET_ERR_ABORTED )},
		{ LSTRKEY( "ERR_OVERFLOW" ),	LINTVAL( NET_ERR_OVERFLOW )},
		{ LSTRKEY( "ERR_OTHER" ),	LINTVAL( NET_ERR_OTHER )},
		
		{ LSTRKEY( "NO_TIMEOUT" ),	LINTVAL( 0 )},

//		{ LSTRKEY( "" ),	LINTVAL(  )},
//		{ LSTRKEY( "" ),	LINTVAL(  )},
		
		{ LNILKEY, LNILVAL }
};


int luaopen_net
	( lua_State *L ) {
#if 0
    luaL_newlib(L, net_map);

    // Module constants
    lua_pushinteger(L, NET_SOCK_STREAM);
    lua_setfield(L, -2, "SOCK_STREAM");

    lua_pushinteger(L, NET_SOCK_DGRAM);
    lua_setfield(L, -2, "SOCK_DGRAM");

    lua_pushinteger(L, NET_DHCP);
    lua_setfield(L, -2, "DHCP");

    lua_pushinteger(L, NET_STATIC);
    lua_setfield(L, -2, "STATIC");

    lua_pushinteger(L, NET_ERR_OK);
    lua_setfield(L, -2, "ERR_OK");

    lua_pushinteger(L, NET_ERR_TIMEDOUT);
    lua_setfield(L, -2, "ERR_TIMEDOUT");

    lua_pushinteger(L, NET_ERR_CLOSED);
    lua_setfield(L, -2, "ERR_CLOSED");

    lua_pushinteger(L, NET_ERR_ABORTED);
    lua_setfield(L, -2, "ERR_ABORTED");

    lua_pushinteger(L, NET_ERR_OVERFLOW);
    lua_setfield(L, -2, "ERR_OVERFLOW");

    lua_pushinteger(L, NET_ERR_OTHER);
    lua_setfield(L, -2, "ERR_OTHER");

    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "NO_TIMEOUT");

    return 1;
#else
	return 0;
#endif
}



MODULE_REGISTER_MAPPED(SOCK, sock, sock_map, luaopen_net);
MODULE_REGISTER_MAPPED(NET, net, net_map, luaopen_net);

#endif
