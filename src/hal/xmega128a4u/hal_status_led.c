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


#include "system.h"
#include "threading.h"
#include "timers.h"
#include "config.h"
#include "io.h"
#include "flash_fs.h"
#include "esp8266.h"
#include "adc.h"

#include "hal_status_led.h"

static bool enabled;


PT_THREAD( status_led_thread( pt_t *pt, void *state ) )
{
PT_BEGIN( pt );

    while(1){

        THREAD_WAIT_WHILE( pt, !enabled );

        if( sys_u32_get_warnings() & SYS_WARN_FLASHFS_FAIL ){

            status_led_v_set( 0, STATUS_LED_RED );

            TMR_WAIT( pt, 1000 );

            status_led_v_set( 1, STATUS_LED_RED );

            TMR_WAIT( pt, 100 );

            status_led_v_set( 0, STATUS_LED_RED );

            TMR_WAIT( pt, 100 );

            status_led_v_set( 1, STATUS_LED_RED );

            TMR_WAIT( pt, 100 );
        }
        else if( cpu_b_osc_fail() ){

            status_led_v_set( 0, STATUS_LED_RED );

            TMR_WAIT( pt, 100 );

            status_led_v_set( 1, STATUS_LED_RED );

            TMR_WAIT( pt, 900 );
        }
        else if( sys_u8_get_mode() == SYS_MODE_SAFE ){

            status_led_v_set( 0, STATUS_LED_RED );

            TMR_WAIT( pt, 500 );

            status_led_v_set( 1, STATUS_LED_RED );

            TMR_WAIT( pt, 500 );
        }
        else if( wifi_b_connected() ){

            if( wifi_b_ap_mode() ){

                status_led_v_set( 0, STATUS_LED_PURPLE );
            }
            else{

                status_led_v_set( 0, STATUS_LED_BLUE );
            }

            TMR_WAIT( pt, 500 );

            if( !( cfg_b_get_boolean( CFG_PARAM_ENABLE_LED_QUIET_MODE ) &&
                  ( tmr_u64_get_system_time_us() > 10000000 ) ) ){

                if( wifi_b_ap_mode() ){

                    status_led_v_set( 1, STATUS_LED_PURPLE );
                }
                else{

                    status_led_v_set( 1, STATUS_LED_BLUE );
                }
            }
        
            TMR_WAIT( pt, 500 );
        }
        else{

            status_led_v_set( 0, STATUS_LED_GREEN );

            TMR_WAIT( pt, 500 );

            if( !( cfg_b_get_boolean( CFG_PARAM_ENABLE_LED_QUIET_MODE ) &&
                  ( tmr_u64_get_system_time_us() > 10000000 ) ) ){

                status_led_v_set( 1, STATUS_LED_GREEN );
            }

            TMR_WAIT( pt, 500 );

            if( wifi_i8_get_status() == WIFI_STATE_ERROR ){

                status_led_v_set( 0, STATUS_LED_RED );

                TMR_WAIT( pt, 500 );

                status_led_v_set( 1, STATUS_LED_RED );

                TMR_WAIT( pt, 500 );
            }
        }
    }

PT_END( pt );
}

void reset_all( void ){

    LED_GREEN_PORT.DIRSET = ( 1 << LED_GREEN_PIN );
    LED_BLUE_PORT.DIRSET = ( 1 << LED_BLUE_PIN );
    LED_RED_PORT.DIRSET = ( 1 << LED_RED_PIN );

    LED_GREEN_PORT.OUTSET = ( 1 << LED_GREEN_PIN );
    LED_BLUE_PORT.OUTSET = ( 1 << LED_BLUE_PIN );
    LED_RED_PORT.OUTSET = ( 1 << LED_RED_PIN );
}

void status_led_v_init( void ){

    enabled = TRUE;

    thread_t_create( status_led_thread,
                     PSTR("status_led"),
                     0,
                     0 );

    reset_all();

    status_led_v_set_blink_speed( LED_BLINK_NORMAL );
}


void status_led_v_set_blink_speed( uint16_t speed ){

}

void status_led_v_enable( void ){

    enabled = TRUE;
}

void status_led_v_disable( void ){

    enabled = FALSE;
}

void status_led_v_set( uint8_t state, uint8_t led ){

    reset_all();

    if( state == 0 ){

        return;
    }

    switch( led ){
        case STATUS_LED_BLUE:
            LED_BLUE_PORT.OUTCLR = ( 1 << LED_BLUE_PIN );
            break;

        case STATUS_LED_GREEN:
            LED_GREEN_PORT.OUTCLR = ( 1 << LED_GREEN_PIN );
            break;

        case STATUS_LED_RED:
            LED_RED_PORT.OUTCLR = ( 1 << LED_RED_PIN );
            break;

        case STATUS_LED_YELLOW:
            LED_GREEN_PORT.OUTCLR = ( 1 << LED_GREEN_PIN );
            LED_RED_PORT.OUTCLR = ( 1 << LED_RED_PIN );
            break;

        case STATUS_LED_PURPLE:
            LED_BLUE_PORT.OUTCLR = ( 1 << LED_BLUE_PIN );
            LED_RED_PORT.OUTCLR = ( 1 << LED_RED_PIN );
            break;

        case STATUS_LED_TEAL:
            LED_GREEN_PORT.OUTCLR = ( 1 << LED_GREEN_PIN );
            LED_BLUE_PORT.OUTCLR = ( 1 << LED_BLUE_PIN );
            break;

        case STATUS_LED_WHITE:
            LED_GREEN_PORT.OUTCLR = ( 1 << LED_GREEN_PIN );
            LED_RED_PORT.OUTCLR = ( 1 << LED_RED_PIN );
            LED_BLUE_PORT.OUTCLR = ( 1 << LED_BLUE_PIN );
            break;

        default:
            break;
    }
}

void status_led_v_strobe( uint16_t us, uint8_t led ){

    status_led_v_set( 1, led );
    _delay_us( us );
    status_led_v_set( 0, led );
    _delay_us( us );
}
