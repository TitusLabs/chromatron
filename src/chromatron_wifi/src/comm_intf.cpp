/*
// <license>
// 
//     This file is part of the Sapphire Operating System.
// 
//     Copyright (C) 2013-2018  Jeremy Billheimer
// 
// 
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
// 
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
// 
//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <http://www.gnu.org/licenses/>.
// 
// </license>
 */

#include "Arduino.h"
#include "comm_intf.h"
#include "wifi.h"
#include "irq_line.h"
#include "version.h"
#include <ESP8266WiFi.h>

extern "C"{
    #include "gfx_lib.h"
    #include "vm_runner.h"
    #include "crc.h"
    #include "wifi_cmd.h"
    #include "kvdb.h"
    #include "vm_core.h"
    #include "list.h"
    #include "memory.h"
}

static list_t print_list;

static uint32_t start_timeout( void ){

    return micros();
}

static int32_t elapsed( uint32_t start ){

    uint32_t now = micros();
    int32_t distance = (int32_t)( now - start );

    // check for rollover
    if( distance < 0 ){

        distance = ( UINT32_MAX - now ) + abs(distance);
    }

    return distance;
}



static bool connected;
static uint32_t comm_timeout;
static bool request_status;
static bool request_info;
static bool request_debug;
static bool request_vm_info;
static bool request_rgb_pix0;
static bool request_rgb_array;
static bool request_vm_frame_sync;
static uint8_t vm_frame_sync_index;
static bool request_vm_frame_sync_status;
static uint8_t vm_frame_sync_status;
static volatile bool request_reset_ready_timeout;

static uint16_t rgb_index;

static uint16_t comm_errors;

static process_stats_t process_stats;

static volatile uint32_t last_rx_ready_ts;
static volatile bool wifi_rx_ready;
static uint32_t last_status_ts;

static wifi_data_header_t intf_data_header;
static uint8_t intf_comm_buf[WIFI_BUF_LEN];
static uint16_t intf_comm_len;
static uint8_t intf_comm_state;

static wifi_msg_udp_header_t udp_header;
static uint8_t udp_data[WIFI_UDP_BUF_LEN];
static uint16_t udp_len;

static wifi_msg_udp_header_t rx_udp_header;
static uint16_t rx_udp_index;

#define COMM_STATE_IDLE          0
#define COMM_STATE_RX_HEADER     1
#define COMM_STATE_RX_DATA       2



void intf_v_led_on(){

    digitalWrite( LED_GPIO, LOW );
}

void intf_v_led_off(){

    digitalWrite( LED_GPIO, HIGH );
}

static void _intf_v_flush(){

    Serial.flush();
    while( Serial.read() >= 0 );
}

static bool wifi_ready( void ){

    noInterrupts();
    bool temp = wifi_rx_ready;
    interrupts();

    return temp;
}

static int8_t _intf_i8_send_msg( uint8_t data_id, uint8_t *data, uint8_t len ){

    if( len > WIFI_MAIN_MAX_DATA_LEN ){

        return -1;
    }
    else if( !wifi_ready() ){

        return -2;  
    }

    wifi_data_header_t header;
    header.len      = len;
    header.data_id  = data_id;
    header.msg_id   = 0;
    header.crc      = 0;

    uint16_t crc = crc_u16_start();
    crc = crc_u16_partial_block( crc, (uint8_t *)&header, sizeof(header) );

    crc = crc_u16_partial_block( crc, data, len );

    header.crc = crc_u16_finish( crc );


    noInterrupts();
    wifi_rx_ready = false;
    interrupts();

    Serial.write( WIFI_COMM_DATA );
    Serial.write( (uint8_t *)&header, sizeof(header) );
    Serial.write( data, len );

    return 0;
}

static void process_data( uint8_t data_id, uint8_t msg_id, uint8_t *data, uint16_t len ){

    if( data_id == WIFI_DATA_ID_CONNECT ){

        if( len != sizeof(wifi_msg_connect_t) ){

            return;
        }

        wifi_msg_connect_t *msg = (wifi_msg_connect_t *)data;

        wifi_v_set_ap( msg->ssid, msg->pass );
    }
    else if( data_id == WIFI_DATA_ID_AP_MODE ){

        if( len != sizeof(wifi_msg_ap_connect_t) ){

            return;
        }

        wifi_msg_ap_connect_t *msg = (wifi_msg_ap_connect_t *)data;

        wifi_v_set_ap_mode( msg->ssid, msg->pass );
    }
    else if( data_id == WIFI_DATA_ID_WIFI_SCAN ){

        wifi_v_scan();
    }
    else if( data_id == WIFI_DATA_ID_PORTS ){

        if( len != sizeof(wifi_msg_ports_t) ){

            return;
        }

        wifi_msg_ports_t *msg = (wifi_msg_ports_t *)data;
    
        wifi_v_set_ports( msg->ports );
    }
    else if( data_id == WIFI_DATA_ID_GFX_PARAMS ){

        if( len != sizeof(gfx_params_t) ){

            return;
        }

        gfx_params_t *msg = (gfx_params_t *)data;
    
        gfx_v_set_params( msg );
    }
    else if( data_id == WIFI_DATA_ID_RESET_VM ){

        vm_v_reset();
    }
    else if( data_id == WIFI_DATA_ID_LOAD_VM ){

        vm_i8_load( data, len );
    }
    else if( data_id == WIFI_DATA_ID_VM_FRAME_SYNC ){

        wifi_msg_vm_frame_sync_t *msg = (wifi_msg_vm_frame_sync_t *)data;

        vm_frame_sync_status = vm_u8_set_frame_sync( msg );
        request_vm_frame_sync_status = true;
    }
    else if( data_id == WIFI_DATA_ID_REQUEST_FRAME_SYNC ){
        
        intf_v_request_vm_frame_sync();
    }
    else if( data_id == WIFI_DATA_ID_RUN_VM ){

        vm_v_run_vm();
    }
    else if( data_id == WIFI_DATA_ID_RUN_FADER ){

        vm_v_run_faders();
    }
    else if( data_id == WIFI_DATA_ID_KV_BATCH ){

        wifi_msg_kv_batch_t *msg = (wifi_msg_kv_batch_t *)data;

        for( uint32_t i = 0; i < msg->count; i++ ){
            
            kvdb_i8_add( msg->entries[i].hash, msg->entries[i].data, 0, 0 );
        }
    }
    else if( data_id == WIFI_DATA_ID_UDP_HEADER ){

        if( len != sizeof(wifi_msg_udp_header_t) ){

            Serial.write( 0x99 );
            Serial.write( 0x01 );

            intf_v_led_on();
            return;
        }

        wifi_msg_udp_header_t *msg = (wifi_msg_udp_header_t *)data;

        udp_len = 0;
        memcpy( &udp_header, msg, sizeof(udp_header) );
    }
    else if( data_id == WIFI_DATA_ID_UDP_DATA ){

        // bounds check
        if( ( udp_len + len ) > sizeof(udp_data) ){

            Serial.write( 0x99 );
            Serial.write( 0x02 );
            Serial.write( udp_len >> 8 );
            Serial.write( udp_len & 0xff );
            Serial.write( len >> 8 );
            Serial.write( len & 0xff );

            intf_v_led_on();
            return;
        }

        memcpy( &udp_data[udp_len], data, len );

        udp_len += len;

        if( udp_len == udp_header.len ){

            // check crc
            if( crc_u16_block( udp_data, udp_len ) != udp_header.crc ){

                Serial.write( 0x99 );
                Serial.write( 0x03 );

                intf_v_led_on();
                return;
            }

            wifi_v_send_udp( &udp_header, udp_data );
        }
    }
}

static void set_rx_ready( void ){

    // flush serial buffers
    _intf_v_flush();

    // Serial.write( WIFI_COMM_READY );  

    irqline_v_strobe_irq();
}

void intf_v_process( void ){

    noInterrupts();

    if( request_reset_ready_timeout ){

        request_reset_ready_timeout = false;

        last_rx_ready_ts = start_timeout();
    }   

    interrupts();

    if( ( intf_comm_state != COMM_STATE_IDLE ) &&
        ( elapsed( comm_timeout ) > 20000 ) ){

        // reset comm state
        intf_comm_state = COMM_STATE_IDLE;

        comm_errors++;

        set_rx_ready();
    }

    if( intf_comm_state == COMM_STATE_IDLE ){    
        
        char c = Serial.read();

        if( c == WIFI_COMM_RESET ){

            if( connected == false ){
                
                connected = true;

                irqline_v_enable();
            }

            set_rx_ready();
        }
        else if( c == WIFI_COMM_DATA ){

            intf_comm_state = COMM_STATE_RX_HEADER;

            comm_timeout = start_timeout();
        }
    }
    else if( intf_comm_state == COMM_STATE_RX_HEADER ){    
        
        if( Serial.available() >= (sizeof(wifi_data_header_t) ) ){

            Serial.readBytes( (uint8_t *)&intf_data_header, sizeof(intf_data_header) );

            intf_comm_state = COMM_STATE_RX_DATA;
        }   
    }    
    else if( intf_comm_state == COMM_STATE_RX_DATA ){    

        if( Serial.available() >= intf_data_header.len ){

            Serial.readBytes( intf_comm_buf, intf_data_header.len );

            // we've copied the data out of the FIFO, so we can go ahead and send RX ready
            // before we process the data.
            set_rx_ready();

            // check crc

            uint16_t msg_crc = intf_data_header.crc;
            intf_data_header.crc = 0;
            uint16_t crc = crc_u16_start();
            crc = crc_u16_partial_block( crc, (uint8_t *)&intf_data_header, sizeof(intf_data_header) );
            crc = crc_u16_partial_block( crc, intf_comm_buf, intf_data_header.len );
            crc = crc_u16_finish( crc );

            if( crc == msg_crc ){

                process_data( intf_data_header.data_id, intf_data_header.msg_id, intf_comm_buf, intf_data_header.len );
            }
            else{

                comm_errors++;
            }

            intf_comm_state = COMM_STATE_IDLE;
        }
    }






    if( !wifi_ready() ){

        // check timeout
        noInterrupts();
        uint32_t temp_last_rx_ready_ts = last_rx_ready_ts;
        interrupts();

        if( elapsed( temp_last_rx_ready_ts ) > 50000 ){

            // query for ready status
            Serial.write( WIFI_COMM_QUERY_READY );

            noInterrupts();
            last_rx_ready_ts = start_timeout();
            interrupts();
        }

        goto done;
    }

    list_t *vm_send_list;
    vm_v_get_send_list( &vm_send_list );


    if( request_status ){

        request_status = false;

        wifi_msg_status_t status_msg;
        status_msg.flags = wifi_u8_get_status();

        _intf_i8_send_msg( WIFI_DATA_ID_STATUS, (uint8_t *)&status_msg, sizeof(status_msg) );
    }
    else if( request_info ){

        request_info = false;

        wifi_msg_info_t info_msg;
        info_msg.version =  ( ( VERSION_MAJOR & 0x0f ) << 12 ) |
                            ( ( VERSION_MINOR & 0x1f ) << 7 ) |
                            ( ( VERSION_PATCH & 0x7f ) << 0 );

        WiFi.macAddress( info_msg.mac );
        
        IPAddress ip_addr = wifi_ip_get_ip();
        info_msg.ip.ip0 = ip_addr[3];
        info_msg.ip.ip1 = ip_addr[2];
        info_msg.ip.ip2 = ip_addr[1];
        info_msg.ip.ip3 = ip_addr[0];

        ip_addr = wifi_ip_get_subnet();
        info_msg.subnet.ip0 = ip_addr[3];
        info_msg.subnet.ip1 = ip_addr[2];
        info_msg.subnet.ip2 = ip_addr[1];
        info_msg.subnet.ip3 = ip_addr[0];

        ip_addr = WiFi.gatewayIP();
        info_msg.gateway.ip0 = ip_addr[3];
        info_msg.gateway.ip1 = ip_addr[2];
        info_msg.gateway.ip2 = ip_addr[1];
        info_msg.gateway.ip3 = ip_addr[0];

        ip_addr = WiFi.dnsIP();
        info_msg.dns.ip0 = ip_addr[3];
        info_msg.dns.ip1 = ip_addr[2];
        info_msg.dns.ip2 = ip_addr[1];
        info_msg.dns.ip3 = ip_addr[0];

        info_msg.rssi           = WiFi.RSSI();

        info_msg.rx_udp_fifo_overruns   = wifi_u32_get_rx_udp_fifo_overruns();
        info_msg.rx_udp_port_overruns   = wifi_u32_get_rx_udp_port_overruns();
        info_msg.udp_received           = wifi_u32_get_udp_received();
        info_msg.udp_sent               = wifi_u32_get_udp_sent();

        mem_rt_data_t rt_data;
        mem2_v_get_rt_data( &rt_data );

        info_msg.comm_errors            = comm_errors;

        info_msg.mem_heap_peak          = rt_data.peak_usage;

        info_msg.intf_max_time          = process_stats.intf_max_time;
        info_msg.vm_max_time            = process_stats.vm_max_time;
        info_msg.wifi_max_time          = process_stats.wifi_max_time;
        info_msg.mem_max_time           = process_stats.mem_max_time;

        _intf_i8_send_msg( WIFI_DATA_ID_INFO, (uint8_t *)&info_msg, sizeof(info_msg) );
    }
    else if( request_vm_info ){

        request_vm_info = false;

        vm_info_t info;
        vm_v_get_info( &info );

        _intf_i8_send_msg( WIFI_DATA_ID_VM_INFO, (uint8_t *)&info, sizeof(info) );
    }
    else if( request_vm_frame_sync ){

        wifi_msg_vm_frame_sync_t msg;
        memset( &msg, 0, sizeof(msg) );

        if( vm_i8_get_frame_sync( vm_frame_sync_index, &msg ) == 0 ){

            _intf_i8_send_msg( WIFI_DATA_ID_VM_FRAME_SYNC, (uint8_t *)&msg, sizeof(msg) );

            vm_frame_sync_index++;
        }
        else{

            request_vm_frame_sync = false;
        }
    }
    else if( request_vm_frame_sync_status ){

        request_vm_frame_sync_status = false;

        wifi_msg_vm_frame_sync_status_t msg;
        msg.status = vm_frame_sync_status;
        msg.frame_number = vm_u16_get_frame_number();

        _intf_i8_send_msg( WIFI_DATA_ID_FRAME_SYNC_STATUS, (uint8_t *)&msg, sizeof(msg) );        
    }
    else if( request_rgb_pix0 ){

        request_rgb_pix0 = false;

        wifi_msg_rgb_pix0_t msg;
        msg.r = gfx_u16_get_pix0_red();
        msg.g = gfx_u16_get_pix0_green();
        msg.b = gfx_u16_get_pix0_blue();

        _intf_i8_send_msg( WIFI_DATA_ID_RGB_PIX0, (uint8_t *)&msg, sizeof(msg) );
    }
    else if( request_rgb_array ){

        // get pointers to the arrays
        uint8_t *r = gfx_u8p_get_red();
        uint8_t *g = gfx_u8p_get_green();
        uint8_t *b = gfx_u8p_get_blue();
        uint8_t *d = gfx_u8p_get_dither();

        uint16_t pix_count = gfx_u16_get_pix_count();

        wifi_msg_rgb_array_t msg;

        uint16_t remaining = pix_count - rgb_index;
        uint8_t count = WIFI_RGB_DATA_N_PIXELS;

        if( count > remaining ){

            count = remaining;
        }

        msg.index = rgb_index;
        msg.count = count;
        uint8_t *ptr = msg.rgbd_array;
        memcpy( ptr, r + rgb_index, count );
        ptr += count;
        memcpy( ptr, g + rgb_index, count );
        ptr += count;
        memcpy( ptr, b + rgb_index, count );
        ptr += count;
        memcpy( ptr, d + rgb_index, count );

        _intf_i8_send_msg( WIFI_DATA_ID_RGB_ARRAY, 
                           (uint8_t *)&msg, 
                           sizeof(msg.index) + sizeof(msg.count) + ( count * 4 ) );

        rgb_index += count;

        if( rgb_index >= pix_count ){

            rgb_index = 0;
            request_rgb_array = false;
        }
    }
    else if( request_debug ){

        request_debug = false;

        wifi_msg_debug_t msg;
        msg.free_heap = ESP.getFreeHeap();

        _intf_i8_send_msg( WIFI_DATA_ID_DEBUG, 
                           (uint8_t *)&msg, 
                           sizeof(msg) );
    }
    else if( wifi_b_rx_udp_pending() ){

        if( rx_udp_header.lport == 0 ){

            rx_udp_index = 0;

            // get header
            wifi_i8_get_rx_udp_header( &rx_udp_header );
            
            _intf_i8_send_msg( WIFI_DATA_ID_UDP_HEADER, (uint8_t *)&rx_udp_header, sizeof(wifi_msg_udp_header_t) );
        }
        else{

            uint16_t data_len = rx_udp_header.len - rx_udp_index;

            if( data_len > WIFI_MAIN_MAX_DATA_LEN ){

                data_len = WIFI_MAIN_MAX_DATA_LEN;
            }

            uint8_t *data = wifi_u8p_get_rx_udp_data();

            _intf_i8_send_msg( WIFI_DATA_ID_UDP_DATA, &data[rx_udp_index], data_len );

            rx_udp_index += data_len;

            // check if we've sent all data
            if( rx_udp_index >= rx_udp_header.len ){

                rx_udp_index = 0;
                rx_udp_header.lport = 0;
                wifi_v_rx_udp_clear_last();
            }
        }
    }
    else if( list_u8_count( vm_send_list ) > 0 ){

        list_node_t ln = list_ln_remove_tail( vm_send_list );

        _intf_i8_send_msg( WIFI_DATA_ID_KV_BATCH, (uint8_t *)list_vp_get_data( ln ), sizeof(wifi_msg_kv_batch_t) );

        list_v_release_node( ln );
    }
    else if( list_u8_count( &print_list ) > 0 ){

        list_node_t ln = list_ln_remove_tail( &print_list );

        _intf_i8_send_msg( WIFI_DATA_ID_DEBUG_PRINT, (uint8_t *)list_vp_get_data( ln ), list_u16_node_size( ln ) ); 
        
        list_v_release_node( ln );
    }
    

    if( elapsed( last_status_ts ) > 1000000 ){

        last_status_ts = start_timeout();

        request_status = true;
        request_info = true;
        request_vm_info = true;
    }


done:
    return;
}

void ICACHE_RAM_ATTR buf_ready_irq( void ){

    wifi_rx_ready = true;
    request_reset_ready_timeout = true;
}

void intf_v_init( void ){

    noInterrupts();
    wifi_rx_ready = false;
    interrupts();

    pinMode( BUF_READY_GPIO, INPUT );
    attachInterrupt( digitalPinToInterrupt( BUF_READY_GPIO ), buf_ready_irq, FALLING );

    pinMode( LED_GPIO, OUTPUT );
    intf_v_led_off();

    Serial.begin( 4000000 );

    // flush serial buffers
    _intf_v_flush();

    request_debug = true;

    list_v_init( &print_list );
}

void intf_v_request_status( void ){

    request_status = true;
}

void intf_v_request_vm_info( void ){

    request_vm_info = true;
}

void intf_v_request_rgb_pix0( void ){

    request_rgb_pix0 = true;
}

void intf_v_request_rgb_array( void ){

    request_rgb_array = true;
}

void intf_v_request_vm_frame_sync( void ){

    vm_frame_sync_index = 0;
    request_vm_frame_sync = true;
}

void intf_v_get_mac( uint8_t mac[6] ){

    WiFi.macAddress( mac );
}


int8_t kvdb_i8_handle_publish( catbus_hash_t32 hash ){

    return 0;
}


void intf_v_printf( const char *format, ... ){

    char debug_print_buf[WIFI_MAX_DATA_LEN];
    memset( debug_print_buf, 0, sizeof(debug_print_buf) );
    
    va_list ap;

    // parse variable arg list
    va_start( ap, format );

    // print string
    uint32_t len = vsnprintf_P( debug_print_buf, sizeof(debug_print_buf), format, ap );

    va_end( ap );

    len++; // add null terminator

    // alloc memory
    list_node_t ln = list_ln_create_node( debug_print_buf, len );

    if( ln < 0 ){

        return;
    }

    list_v_insert_head( &print_list, ln );
}

int8_t intf_i8_send_msg( uint8_t data_id, uint8_t *data, uint8_t len ){

    return _intf_i8_send_msg( data_id, data, len );
}

void intf_v_get_proc_stats( process_stats_t **stats ){

    *stats = &process_stats;
}